
#include "hooks.h"
#include "log.h"
#include "game.h"
#include "net_shared.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {

constexpr uintptr_t kCatchUp = 0x1408686c0, kClientSend = 0x14023e590, kClockMs = 0x140174f60;
constexpr uint32_t kLastTick = 0xce490, kQueueCount = 0xce300, kServerStep = 0xce3b0;
constexpr uint32_t kBacklogTicks = 14; // above this the game's own fast catch-up plays
constexpr double kGameTickMs = 16.0;   // a host that isn't lagging makes one tick per 16 ms
constexpr double kStalePaceTicks = 4;  // this far above the target, the measured period is ignored for the game's own

using U = uint64_t;
using CatchUp_t = void(__fastcall*)(U, U, U, U, U);
using ClientSend_t = void(__fastcall*)(U, U, U, U);
using Clock_t = uint32_t(__fastcall*)();
CatchUp_t g_orig_catchup = nullptr;
ClientSend_t g_orig_send = nullptr;
Clock_t g_clock = nullptr;

int g_mode = 1; // 0 observe, 1 while redundant copies arrive, 2 always
bool g_on = false;
int g_min_ticks = 1;
double g_max_ms = 20;

// --- arrivals (network thread -> update thread) --------------------------------------------
constexpr size_t kWindow = 600;
std::mutex g_arr_mu;
std::vector<int64_t> g_arrivals; // ring, guarded by g_arr_mu
size_t g_arr_next = 0;
uint64_t g_arr_total = 0;

// --- update thread only ------------------------------------------------------------------
thread_local uint8_t* g_game = nullptr;
thread_local bool g_in_catchup = false;
LARGE_INTEGER g_freq{};
LONGLONG g_prev_qpc = 0;
double g_credit = 0;     // ticks owed to the playback
double g_depth_s = -1;   // smoothed queue depth
double g_target = 4;     // ticks
double g_jitter_ms = 0, g_period_ms = 16.67;
LONGLONG g_target_lowered = 0;
uint64_t g_seen_total = 0;

// stats for the 10 s log line
LONGLONG g_win_start = 0;
uint64_t g_calls = 0, g_starved = 0, g_vanilla = 0;
double g_starved_ms = 0, g_depth_sum = 0, g_rate_min = 9, g_rate_max = 0;
uint32_t g_depth_min = UINT32_MAX, g_depth_max = 0;

// For host pacing: ticks queued beyond the playout target, read by the network thread (pacing report).
std::atomic<int> g_report_depth{-1};
std::atomic<DWORD> g_report_at{0};

double qpc_ms(LONGLONG a, LONGLONG b) { return double(b - a) * 1000.0 / double(g_freq.QuadPart); }

// Jitter from Steam's receive times: lateness of each message against the earliest-arrival line.
void update_jitter()
{
    std::vector<int64_t> a;
    {
        std::lock_guard lock(g_arr_mu);
        if (g_arr_total == g_seen_total) return;
        g_seen_total = g_arr_total;
        size_t n = g_arrivals.size();
        a.reserve(n);
        for (size_t i = 0; i < n; i++) a.push_back(g_arrivals[(g_arr_next + i) % n]); // oldest first
    }
    if (a.size() < 60) return;
    // A gap of > 2 s (menu, loading, reconnect) starts a new measurement.
    size_t first = 0;
    for (size_t i = 1; i < a.size(); i++) {
        if (a[i] - a[i - 1] > 2000000) first = i;
    }
    size_t n = a.size() - first;
    if (n < 60) return;
    double period = double(a.back() - a[first]) / double(n - 1); // us per message
    if (period <= 0) return;
    std::vector<double> late(n);
    double base = 1e300;
    for (size_t i = 0; i < n; i++) {
        late[i] = double(a[first + i] - a[first]) - double(i) * period;
        base = std::min(base, late[i]);
    }
    for (double& l : late) l -= base;
    std::nth_element(late.begin(), late.begin() + (n * 80) / 100, late.end());
    g_jitter_ms = late[(n * 80) / 100] / 1000.0;
    g_period_ms = period / 1000.0;
}

void update_target(LONGLONG now)
{
    double tick_ms = std::clamp(g_period_ms, 8.0, 100.0);
    double want = std::ceil(g_jitter_ms / tick_ms) + 1;
    want = std::clamp(want, double(g_min_ticks), std::max(double(g_min_ticks), std::floor(g_max_ms / tick_ms)));
    if (want >= g_target) {
        g_target = want;
        g_target_lowered = now;
    } else if (qpc_ms(g_target_lowered, now) > 2000) {
        g_target -= 1;
        g_target_lowered = now;
    }
}

uint32_t game_ticks(uint32_t e, uint32_t n)
{
    e = std::min(e, 100u);
    if (n > 4) e = ((n - 4) * (n - 4) + 20) * e / 20;
    return std::min(e / 16, 10u);
}

// Elapsed value that makes the game simulate exactly k ticks, chosen in the middle of the range that
// gives k so a clock tick between here and the game's own clock read doesn't change the result.
int elapsed_for(uint32_t k, uint32_t n)
{
    int lo = -1, hi = -1;
    for (int e = 0; e <= 100; e++) {
        if (game_ticks(e, n) == k) {
            if (lo < 0) lo = e;
            hi = e;
        } else if (lo >= 0) {
            break;
        }
    }
    if (lo < 0 || (hi - lo < 2 && hi < 100)) return -1;
    return (lo + hi) / 2;
}

void log_window(LONGLONG now)
{
    double span = qpc_ms(g_win_start, now);
    if (span < 10000) return;
    if (g_calls > 0) {
        swlog("playout 10s (%s): target %.0f ticks (jitter p80 %.1f ms, server tick %.1f ms)  depth avg %.1f min %u max %u  "
              "frozen %llu times / %.0f ms  speed %.2f..%.2f  game catch-up %llu calls",
              g_on ? "on" : "observe only", g_target, g_jitter_ms, g_period_ms, g_depth_sum / double(g_calls),
              g_depth_min == UINT32_MAX ? 0 : g_depth_min, g_depth_max, static_cast<unsigned long long>(g_starved),
              g_starved_ms, g_rate_min > 8 ? 1.0 : g_rate_min, g_rate_max < 0.01 ? 1.0 : g_rate_max,
              static_cast<unsigned long long>(g_vanilla));
    }
    g_win_start = now;
    g_calls = g_starved = g_vanilla = 0;
    g_starved_ms = g_depth_sum = 0;
    g_rate_min = 9;
    g_rate_max = 0;
    g_depth_min = UINT32_MAX;
    g_depth_max = 0;
}

void decide()
{
    if (!g_game) return;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    LONGLONG now = q.QuadPart;
    double dt = g_prev_qpc ? std::min(qpc_ms(g_prev_qpc, now), 250.0) : 0;
    g_prev_qpc = now;
    if (!g_win_start) g_win_start = now;

    uint32_t n0 = *reinterpret_cast<uint32_t*>(g_game + kQueueCount);
    if (n0 <= kBacklogTicks) { // during a backlog the window and the estimates are being reset (below)
        update_jitter();
        update_target(now);
    }

    uint32_t n = *reinterpret_cast<uint32_t*>(g_game + kQueueCount);
    if (n > kBacklogTicks) {
        // The game's own catch-up: what arrived during it says nothing about the network's jitter. The estimates go too:
        // kept, they would set the pace until 60 new arrivals fit between two backlogs, which they may never do.
        std::lock_guard lock(g_arr_mu);
        g_arrivals.clear();
        g_arr_next = 0;
        g_jitter_ms = 0;
        g_period_ms = kGameTickMs;
    }
    g_depth_s = g_depth_s < 0 ? n : g_depth_s + 0.1 * (double(n) - g_depth_s);
    g_calls++;
    g_depth_sum += n;
    g_depth_min = std::min(g_depth_min, n);
    g_depth_max = std::max(g_depth_max, n);

    // For host pacing: ticks queued beyond the buffer this playout wants anyway (its target covers jitter).
    g_report_depth.store(int(std::clamp(double(n) - g_target, 0.0, double(1 << 20))), std::memory_order_relaxed);
    g_report_at.store(GetTickCount(), std::memory_order_relaxed);
    double tick_ms = std::clamp(g_period_ms, 8.0, 100.0);
    if (n == 0) {
        // Nothing to play: a freeze if a tick was due.
        if (g_credit + dt / tick_ms >= 1) {
            g_starved++;
            g_starved_ms += dt;
        }
        g_credit = 0;
        log_window(now);
        return; // the game resets its own clock here
    }
    if (n > kBacklogTicks) {
        g_vanilla++;
        g_credit = 0;
        log_window(now);
        return; // big backlog: the game's own fast catch-up (its formula can't be steered exactly past 14)
    }
    double rate = std::clamp(1.0 + 0.06 * (g_depth_s - g_target), 0.8, 1.5);
    g_rate_min = std::min(g_rate_min, rate);
    g_rate_max = std::max(g_rate_max, rate);
    // A queue above the target says the host is faster than the period measured so far (it just stopped lagging), and it
    // says so sooner than the arrivals' window can. At kStalePaceTicks above, any period drains the queue.
    double stale = std::clamp((g_depth_s - g_target) / kStalePaceTicks, 0.0, 1.0);
    double pace_ms = tick_ms + (std::min(tick_ms, kGameTickMs) - tick_ms) * stale;
    g_credit = std::min(g_credit + dt / pace_ms * rate, 10.0);
    uint32_t k = std::min<uint32_t>(static_cast<uint32_t>(g_credit), std::min(n, 10u));
    log_window(now);
    g_on = g_mode == 2 || (g_mode == 1 && redundancy_active());
    if (!g_on) return;
    int e = elapsed_for(k, n);
    if (e < 0) return; // can't be expressed exactly: leave this call to the game
    g_credit -= k;
    uint32_t now_ms = g_clock();
    *reinterpret_cast<float*>(g_game + kServerStep) = 16.0f;
    *reinterpret_cast<uint32_t*>(g_game + kLastTick) = now_ms - static_cast<uint32_t>(e);
}

void __fastcall catchup_detour(U g, U p2, U p3, U p4, U p5)
{
    g_game = reinterpret_cast<uint8_t*>(g);
    g_in_catchup = true;
    g_orig_catchup(g, p2, p3, p4, p5);
    g_in_catchup = false;
    g_game = nullptr;
}

// Client message send: last call before the catch-up formula reads the clock.
void __fastcall client_send_detour(U a1, U a2, U a3, U a4)
{
    g_orig_send(a1, a2, a3, a4);
    if (g_in_catchup && g_game) decide();
}

bool install()
{
    const wchar_t* ini = settings_ini();
    int mode = static_cast<int>(GetPrivateProfileIntW(L"net", L"playout", 1, ini));
    if (mode < 0) return true;
    g_mode = std::min(mode, 2);
    g_min_ticks = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"net", L"playout_min_ticks", 1, ini)), 1, 8);
    g_max_ms = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"net", L"playout_max_ms", 20, ini)), 16, 1000);
    QueryPerformanceFrequency(&g_freq);
    g_arrivals.reserve(kWindow);
    g_clock = game_ptr<Clock_t>(kClockMs);
    bool ok = hook_game(kCatchUp, reinterpret_cast<void*>(&catchup_detour), reinterpret_cast<void**>(&g_orig_catchup)) &&
              hook_game(kClientSend, reinterpret_cast<void*>(&client_send_detour), reinterpret_cast<void**>(&g_orig_send));
    static const char* kModes[] = {"observe only", "on while redundant copies arrive", "always on"};
    swlog("playout: %s (min %d ticks, max %.0f ms)", ok ? kModes[g_mode] : "FAILED", g_min_ticks, g_max_ms);
    return ok;
}

HookRegistrar reg{{"client playout buffer", /*needs_known_build=*/true, &install}};

} // namespace

bool playout_wants_arrivals() { return g_clock != nullptr; }

bool playout_client_depth(int& depth)
{
    DWORD at = g_report_at.load(std::memory_order_relaxed);
    if (!at || GetTickCount() - at > 1000) return false;
    depth = g_report_depth.load(std::memory_order_relaxed);
    return depth >= 0;
}

// For mp_watch.cpp: host updates handed to the game since the last call, and the longest gap between two (Steam's
// receive times, so the network's own gaps, not this PC's).
uint64_t g_take_n = 0;
int64_t g_take_last = 0, g_take_gap = 0;
void playout_take_arrivals(uint64_t& n, double& longest_gap_ms)
{
    std::lock_guard lock(g_arr_mu);
    n = g_take_n;
    longest_gap_ms = g_take_gap / 1000.0;
    g_take_n = 0;
    g_take_gap = 0;
}

void playout_on_arrival(int64_t usec)
{
    std::lock_guard lock(g_arr_mu);
    g_take_n++;
    if (g_take_last && usec > g_take_last) g_take_gap = std::max(g_take_gap, usec - g_take_last);
    g_take_last = usec;
    if (g_arrivals.size() < kWindow) {
        g_arrivals.push_back(usec);
    } else {
        g_arrivals[g_arr_next] = usec;
        g_arr_next = (g_arr_next + 1) % kWindow;
    }
    g_arr_total++;
}
