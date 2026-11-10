
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "net_shared.h"
#include "aim_trace.h"
#include "logic_trace.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr uintptr_t kCatchUp = 0x1408686c0, kApply = 0x14085aeb0, kSimulate = 0x14073f890, kHostTick = 0x140865be0,
                    kRequests = 0x140851630, kJoins = 0x14084d610, kIsHost = 0x140ceb6ff;
constexpr uint32_t kQueueCount = 0xce300, kLoadedSent = 0xce384, kTickNo = 0x33c;
constexpr uint32_t kCaughtUpTicks = 14;
constexpr double kJoinGiveUpMs = 180000;

using U = uint64_t;
using F5 = void(__fastcall*)(U, U, U, U, U);
using F6 = void(__fastcall*)(U, U, U, U, U, U);
using F8 = void(__fastcall*)(U, U, U, U, U, U, U, U);
using F12 = void(__fastcall*)(U, U, U, U, U, U, U, U, U, U, U, U);
F5 o_catchup = nullptr, o_requests = nullptr;
F6 o_apply = nullptr;
F8 o_host_tick = nullptr, o_joins = nullptr;
F12 o_simulate = nullptr;

const volatile uint8_t* g_is_host = nullptr;
double g_stall_ms = 250;
bool g_event_details=false;
LARGE_INTEGER g_freq{};

double now_ms()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return double(q.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

struct Stat {
    std::vector<float> v;
    double sum = 0, max = 0;
    void add(double ms)
    {
        if (v.size() < 20000) v.push_back(float(ms));
        sum += ms;
        max = std::max(max, ms);
    }
    double avg() const { return v.empty() ? 0 : sum / v.size(); }
    double p95()
    {
        if (v.empty()) return 0;
        size_t k = v.size() * 95 / 100;
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    }
    void reset() { v.clear(), sum = max = 0; }
    std::string text(const char* name)
    {
        char b[96];
        snprintf(b, sizeof b, " %s %.2f/%.2f/%.1f", name, avg(), p95(), max);
        return b;
    }
};

// The update thread and, with the map open, the main thread take turns (the game stops one for the other), so one
// lock per sample is uncontended.
std::mutex g_mu;
Stat g_apply, g_simulate, g_catchup, g_host_tick, g_requests, g_joins;
uint64_t g_ticks = 0, g_catchup_calls = 0, g_main_thread_calls = 0;
uint32_t g_most_in_call = 0, g_q_max = 0, g_q_now = 0;
double g_q_sum = 0;
double g_win_start = 0;
DWORD g_update_tid = 0;
DWORD g_last_request = 0; // host: when a player's requests last came in (no "mp host" lines in single player)
int g_tick_no = 0;        // host tick: the host's latest, or the one this client replayed last

// --- behind episodes (client; the catch-up's thread, under g_mu) -------------------------------------------------
struct Call {
    double t, ms, apply, sim;
    int tick;
    uint32_t before, after, ran;
    bool render;
};
constexpr size_t kRing = 2048;
Call g_ring[kRing];
size_t g_ring_n = 0; // total calls recorded
uint32_t g_behind_ticks = 30;
struct Episode {
    bool on = false;
    double t0 = 0, last_end = -1e18, worst_call = 0, render_ms = 0, apply = 0, sim = 0;
    int tick0 = 0;
    uint32_t peak = 0;
    uint64_t ticks = 0, received = 0;
} g_ep;

// One 250 ms (or 1 s) step of the ring as text.
std::string bucket(const Call* c, size_t n, double from, double to, double t_ref, uint32_t prev_after)
{
    uint32_t ran = 0, calls = 0, render = 0, q0 = 0, q1 = 0, received = 0;
    double apply = 0, sim = 0, longest = 0;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (c[i].t < from || c[i].t >= to) continue;
        if (!any) q0 = c[i].before;
        any = true;
        received += c[i].before > prev_after ? c[i].before - prev_after : 0;
        prev_after = c[i].after;
        q1 = c[i].after;
        ran += c[i].ran, calls++, render += c[i].render;
        apply += c[i].apply, sim += c[i].sim, longest = std::max(longest, c[i].ms);
    }
    char b[300];
    if (!any) snprintf(b, sizeof b, "  %+.2f s: no catch-up call (the update / render thread was elsewhere)", (from - t_ref) / 1000);
    else
        snprintf(b, sizeof b, "  %+.2f s: received %u, simulated %u (apply %.1f + simulate %.1f ms, %.1f ms per tick), queue %u -> %u, "
                 "%u calls (%u on the render thread), longest %.0f ms",
                 (from - t_ref) / 1000, received, ran, apply, sim, ran ? (apply + sim) / ran : 0.0, q0, q1, calls, render, longest);
    return b;
}

void episode_check(double now)
{
    const Call& last = g_ring[(g_ring_n - 1) % kRing];
    Episode& e = g_ep;
    if (!e.on) {
        if (last.after < g_behind_ticks || now - e.last_end < 30000) return;
        e = Episode{};
        e.on = true;
        e.t0 = now;
        e.tick0 = last.tick;
        // the ring's last 3 s, oldest first
        size_t n = std::min(g_ring_n, kRing);
        std::vector<Call> c;
        for (size_t i = g_ring_n - n; i < g_ring_n; i++)
            if (now - g_ring[i % kRing].t <= 3000) c.push_back(g_ring[i % kRing]);
        swlog("behind: %u host ticks queued at host tick %d (the game's \"desynced\" range); the last 3 s in 250 ms steps "
              "(received = host ticks added to the queue):",
              last.after, last.tick);
        uint32_t prev = c.empty() ? 0 : c.front().before;
        for (double f = now - 3000; f < now; f += 250) {
            swlog("%s", bucket(c.data(), c.size(), f, f + 250, now, prev).c_str());
            for (const Call& x : c)
                if (x.t < f + 250) prev = x.after;
        }
        return;
    }
    e.peak = std::max(e.peak, last.after);
    e.worst_call = std::max(e.worst_call, last.ms);
    e.ticks += last.ran;
    e.apply += last.apply, e.sim += last.sim;
    if (last.render) e.render_ms += last.ms;
    if (last.after > kCaughtUpTicks && now - e.t0 < 120000) return;
    double secs = (now - e.t0) / 1000;
    swlog("behind: episode over at host tick %d after %.1f s (%s): peak %u queued, %llu ticks simulated (%.1f/s, %.2f ms each: "
          "apply %.2f + simulate %.2f), longest catch-up call %.0f ms, %.1f s of catch-up on the render thread",
          last.tick, secs, last.after <= kCaughtUpTicks ? "caught up" : "STILL BEHIND after 2 minutes", e.peak,
          static_cast<unsigned long long>(e.ticks), e.ticks / std::max(0.001, secs),
          e.ticks ? (e.apply + e.sim) / e.ticks : 0.0, e.ticks ? e.apply / e.ticks : 0.0, e.ticks ? e.sim / e.ticks : 0.0,
          e.worst_call, e.render_ms / 1000);
    e.on = false;
    e.last_end = now;
}
// The join in progress (client; the catch-up's thread, under g_mu).
struct Join {
    bool on = false, simulated = false, peaked = false;
    double t0 = 0, first_tick = 0, bulk_last = 0, peak_at = 0, caught_at = 0, render_ms = 0;
    uint64_t bulk0 = 0, bulk_seen = 0, ticks = 0;
    uint32_t peak = 0;
} g_join;

double since_join(double t) { return (t - g_join.t0) / 1000.0; }

void join_check(uint32_t queue, double t, double call_ms, bool render)
{
    Join& j = g_join;
    uint64_t bulk = net_bulk_received();
    if (bulk != j.bulk_seen) j.bulk_seen = bulk, j.bulk_last = t;
    if (render) j.render_ms += call_ms;
    if (queue > j.peak) j.peak = queue, j.peak_at = t;
    if (!j.simulated && j.ticks) {
        j.simulated = true;
        j.first_tick = t;
        swlog("join: first host tick simulated %.1f s after reporting loaded (%.2f MB of session data so far, %u ticks queued)",
              since_join(t), (bulk - j.bulk0) / 1048576.0, queue);
    }
    // Caught up: back to a normal queue after a backlog, or never behind at all (5 s of play without one).
    bool caught = j.simulated && queue <= kCaughtUpTicks && (j.peak > kCaughtUpTicks || t - j.first_tick > 5000);
    bool give_up = t - j.t0 > kJoinGiveUpMs;
    if (!caught && !give_up) return;
    double replay_s = std::max(0.001, (t - j.peak_at) / 1000.0);
    swlog("join: %s %.1f s after reporting loaded: first tick simulated at +%.1f s, %.2f MB of session data (last at +%.1f s), "
          "backlog peak %u ticks at +%.1f s, %llu ticks simulated since (%.0f/s from the peak), %.1f s of catch-up on the render "
          "thread (map / loading screen)%s",
          caught ? "caught up" : "STILL BEHIND", since_join(t), j.simulated ? since_join(j.first_tick) : -1.0,
          (j.bulk_seen - j.bulk0) / 1048576.0, j.bulk_last ? since_join(j.bulk_last) : 0.0, j.peak, since_join(j.peak_at),
          static_cast<unsigned long long>(j.ticks), (j.peak - std::min(j.peak, queue)) / replay_s + 62.5, j.render_ms / 1000.0,
          caught ? "" : " - the client can't replay faster than the host makes ticks (see mp client 5s)");
    j.on = false;
}

// Parts of the current host tick / catch-up call, for the stall line (same thread).
thread_local double tl_sim = 0, tl_req = 0, tl_join = 0, tl_apply = 0;

void flush(bool host)
{
    double t = now_ms();
    if (!g_win_start) g_win_start = t;
    double span = t - g_win_start;
    if (span < 5000) return;
    double s = span / 1000.0;
    if (host && !g_host_tick.v.empty() && g_last_request && GetTickCount() - g_last_request < 60000) {
        swlog("mp host 5s: host tick %d, %.1f ticks/s | per tick avg/p95/max ms: tick%s =%s%s%s | requests %.0f ms/s, joins %.0f ms/s",
              g_tick_no, g_host_tick.v.size() / s, g_host_tick.text("").c_str(), g_simulate.text("simulate").c_str(),
              g_requests.text("requests").c_str(), g_joins.text("joins").c_str(), g_requests.sum / s, g_joins.sum / s);
    } else if (!host && g_catchup_calls) {
        uint64_t arrived = 0;
        double gap = 0;
        playout_take_arrivals(arrived, gap);
        swlog("mp client 5s: host tick %d replayed, host updates %.1f/s (longest gap %.0f ms), simulated %.1f ticks/s, queue avg %.1f max %u now %u | "
              "catch-up %.1f calls/s (%llu on the render thread), most %u ticks in one, call%s | per tick avg/p95/max ms:%s%s",
              g_tick_no, arrived / s, gap, g_ticks / s, g_q_sum / g_catchup_calls, g_q_max, g_q_now, g_catchup_calls / s,
              static_cast<unsigned long long>(g_main_thread_calls), g_most_in_call, g_catchup.text("").c_str(),
              g_apply.text("apply").c_str(), g_simulate.text("simulate").c_str());
    }
    g_apply.reset(), g_simulate.reset(), g_catchup.reset(), g_host_tick.reset(), g_requests.reset(), g_joins.reset();
    g_ticks = g_catchup_calls = g_main_thread_calls = 0;
    g_most_in_call = g_q_max = 0;
    g_q_sum = 0;
    g_win_start = t;
}

void __fastcall catchup_detour(U g, U a2, U a3, U a4, U a5)
{
    uint8_t loaded_before = *reinterpret_cast<const uint8_t*>(g + kLoadedSent);
    uint32_t before = *reinterpret_cast<const uint32_t*>(g + kQueueCount);
    uint64_t ticks_before;
    {
        std::lock_guard lock(g_mu);
        ticks_before = g_ticks;
    }
    tl_sim = tl_apply = 0;
    double t0 = now_ms();
    o_catchup(g, a2, a3, a4, a5);
    double ms = now_ms() - t0;
    uint32_t after = *reinterpret_cast<const uint32_t*>(g + kQueueCount);
    int tick = a4 ? *reinterpret_cast<const int*>(a4 + kTickNo) : 0;
    std::lock_guard lock(g_mu);
    uint32_t ran = uint32_t(g_ticks - ticks_before);
    g_tick_no = tick;
    g_ring[g_ring_n++ % kRing] = Call{t0, ms, tl_apply, tl_sim, tick, before, after, ran,
                                      g_update_tid && GetCurrentThreadId() != g_update_tid};
    episode_check(t0 + ms);
    g_catchup.add(ms);
    g_catchup_calls++;
    if (g_update_tid && GetCurrentThreadId() != g_update_tid) g_main_thread_calls++; // render: see below
    g_most_in_call = std::max(g_most_in_call, ran);
    g_q_sum += after;
    g_q_max = std::max(g_q_max, std::max(before, after));
    g_q_now = after;
    bool render = g_update_tid && GetCurrentThreadId() != g_update_tid;
    if (!loaded_before && *reinterpret_cast<const uint8_t*>(g + kLoadedSent)) {
        g_join = Join{};
        g_join.on = true;
        g_join.t0 = t0;
        g_join.bulk0 = g_join.bulk_seen = net_bulk_received();
        swlog("join: reported loaded to the host (set_loaded); from now on every host tick waits for this client to replay it");
    } else if (g_join.on) {
        g_join.ticks += ran;
        join_check(after, now_ms(), ms, render);
    }
    if (ms > g_stall_ms)
        swlog("mp stall: client catch-up call took %.0f ms at host tick %d: %u ticks (apply %.0f ms, simulate %.0f ms, receive / rest %.0f ms), queue %u -> %u",
              ms, tick, ran, tl_apply, tl_sim, ms - tl_apply - tl_sim, before, after);
    flush(false);
}

bool event_snapshot(uintptr_t ring,uint32_t* counts,uint32_t& total)
{
    __try {
        auto data=*reinterpret_cast<const uintptr_t* const*>(ring);
        uint32_t cap=*reinterpret_cast<const uint32_t*>(ring+8),first=*reinterpret_cast<const uint32_t*>(ring+12);
        total=*reinterpret_cast<const uint32_t*>(ring+16);
        if(total>100000 || total>cap || (total && (!data || !cap || first>=cap)))return false;
        for(uint32_t i=0;i<total;++i) {
            uintptr_t event=data[(uint64_t(first)+i)%cap];
            if(event) {uint32_t type=*reinterpret_cast<const uint32_t*>(event+8);if(type<0xc4)counts[type]++;}
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

void __fastcall apply_detour(U a1, U a2, U a3, U a4, U a5, U a6)
{
    logic_trace_received(a5,a3);
    uint32_t counts[0xc4]{},event_count=0;
    const bool details=g_event_details && a5 && event_snapshot(a5,counts,event_count);
    double t0 = now_ms();
    o_apply(a1, a2, a3, a4, a5, a6);
    double ms = now_ms() - t0;
    if(details && ms>g_stall_ms) {
        std::string events;
        for(uint32_t i=0;i<0xc4;++i)if(counts[i]) {
            char entry[32];snprintf(entry,sizeof entry," 0x%x=%u",i,counts[i]);
            if(events.size()+strlen(entry)>1200)break;
            events+=entry;
        }
        swlog("mp slow apply: %.0f ms, %u native events; type counts:%s (counts identify the batch, not per-event timings)",ms,event_count,events.c_str());
    }
    tl_apply += ms;
    if (*g_is_host) return; // the host applies its own tick's events inside its tick; counted there
    std::lock_guard lock(g_mu);
    g_apply.add(ms);
}

void __fastcall simulate_detour(U a1, U a2, U a3, U a4, U a5, U a6, U a7, U a8, U a9, U a10, U a11, U a12)
{
    logic_trace_begin(a1,a3,*g_is_host!=0);
    double t0 = now_ms();
    o_simulate(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
    double ms = now_ms() - t0;
    tl_sim += ms;
    logic_trace_end();
    aim_trace_sample(a1, a3, *g_is_host != 0);
    std::lock_guard lock(g_mu);
    g_simulate.add(ms);
    if (!*g_is_host) g_ticks++;
}

void __fastcall requests_detour(U a1, U a2, U a3, U a4, U a5)
{
    double t0 = now_ms();
    o_requests(a1, a2, a3, a4, a5);
    double ms = now_ms() - t0;
    tl_req += ms;
    std::lock_guard lock(g_mu);
    g_requests.add(ms);
    g_last_request = GetTickCount();
}

void __fastcall joins_detour(U a1, U a2, U a3, U a4, U a5, U a6, U a7, U a8)
{
    double t0 = now_ms();
    o_joins(a1, a2, a3, a4, a5, a6, a7, a8);
    double ms = now_ms() - t0;
    tl_join += ms;
    std::lock_guard lock(g_mu);
    g_joins.add(ms);
}

void __fastcall host_tick_detour(U a1, U a2, U a3, U a4, U a5, U a6, U a7, U a8)
{
    tl_sim = tl_req = tl_join = 0;
    double t0 = now_ms();
    o_host_tick(a1, a2, a3, a4, a5, a6, a7, a8);
    double ms = now_ms() - t0;
    std::lock_guard lock(g_mu);
    g_host_tick.add(ms);
    if (a4) g_tick_no = *reinterpret_cast<const int*>(a4 + kTickNo);
    if (ms > g_stall_ms)
        swlog("mp stall: host tick %d took %.0f ms (simulate %.0f, players' requests %.0f, joins / world sends %.0f, rest %.0f)", g_tick_no, ms, tl_sim,
              tl_req, tl_join, ms - tl_sim - tl_req - tl_join);
    flush(true);
}

// Which thread is the update thread (to tell render-thread catch-up apart): the core's loop event.
void on_begin(void*) { g_update_tid = GetCurrentThreadId(); }

bool install()
{
    const wchar_t* ini = settings_ini();
    const bool aim_trace = aim_trace_install();
    const bool logic_trace = logic_trace_install();
    g_event_details=aim_trace || logic_trace || GetPrivateProfileIntW(L"mp",L"event_details",0,ini)!=0;
    if (!GetPrivateProfileIntW(L"mp", L"log", 1, ini) && !aim_trace && !logic_trace) {
        swlog("mp watch: off in settings.ini");
        return true;
    }
    g_stall_ms = std::clamp(int(GetPrivateProfileIntW(L"mp", L"stall_ms", 250, ini)), 20, 10000);
    g_behind_ticks = std::clamp(int(GetPrivateProfileIntW(L"mp", L"behind_ticks", 30, ini)), 15, 100000);
    QueryPerformanceFrequency(&g_freq);
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    swml()->on_loop(swml_self(), &on_begin, nullptr, nullptr, nullptr);
    bool ok = hook_game(kCatchUp, reinterpret_cast<void*>(&catchup_detour), reinterpret_cast<void**>(&o_catchup)) &&
              hook_game(kApply, reinterpret_cast<void*>(&apply_detour), reinterpret_cast<void**>(&o_apply)) &&
              hook_game(kSimulate, reinterpret_cast<void*>(&simulate_detour), reinterpret_cast<void**>(&o_simulate)) &&
              hook_game(kHostTick, reinterpret_cast<void*>(&host_tick_detour), reinterpret_cast<void**>(&o_host_tick)) &&
              hook_game(kRequests, reinterpret_cast<void*>(&requests_detour), reinterpret_cast<void**>(&o_requests)) &&
              hook_game(kJoins, reinterpret_cast<void*>(&joins_detour), reinterpret_cast<void**>(&o_joins));
    swlog("mp watch: %s (\"mp host 5s\" / \"mp client 5s\" timing lines, \"mp stall\" over %.0f ms)", ok ? "on" : "FAILED", g_stall_ms);
    return ok;
}

HookRegistrar reg{{"mp watch (tick timing)", /*needs_known_build=*/true, &install}};

} // namespace
