
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "net_shared.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

constexpr double kGameTickMs = 16.0;
constexpr DWORD kReportMaxAgeMs = 2000;
constexpr DWORD kStepMs = 250;

constexpr uintptr_t kIsHost = 0x140ceb6ff; // byte: 1 host / single player, 0 client
constexpr uintptr_t kZoneAdd = 0x1401acbf0, kZoneBegin = 0x1401ad030, kZoneEnd = 0x1401ad0d0, kFrameEnd = 0x1401acf90;
constexpr unsigned kWaitZone = 57;
struct GameString {
    const char* text;
    uint32_t len, cap;
};
using ZoneAdd_t = void(__fastcall*)(void* profiler, unsigned channel, unsigned zone, const GameString* name);
using Zone_t = void(__fastcall*)(void* profiler, unsigned channel, unsigned zone);
using FrameEnd_t = void(__fastcall*)(void* profiler, unsigned channel);
ZoneAdd_t g_zone_add = nullptr;
Zone_t g_zone_begin = nullptr, g_zone_end = nullptr;
FrameEnd_t g_orig_frame_end = nullptr;
bool g_zone_added = false;
DWORD g_update_tid = 0;
int64_t g_loop_start = 0;
bool g_waited = false; // this iteration's wait already ran inside the frame

bool g_on = false;
const volatile uint8_t* g_is_host = nullptr;
int g_target = 2;
double g_max_ms = 33;
double g_catchup_max_ms = 40;
int g_queue_limit_ms = 250; // pacing_queue_ms
LARGE_INTEGER g_freq{};

// Update thread only.
HANDLE g_timer = nullptr;
double g_period = kGameTickMs;
DWORD g_last_step = 0, g_last_log = 0;
int g_last_backlog = -1, g_worst_backlog = 0;
int g_last_queue = -1, g_worst_queue = 0;
double g_period_sum = 0;
int g_period_n = 0;

double ms_since(int64_t t0)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - t0) * 1000.0 / double(g_freq.QuadPart);
}

void step(DWORD now)
{
    if (g_is_host && *g_is_host == 0) {
        // A client: the host sets the pace. A period left from hosting before goes at once (2026-10-31: joining
        // right after hosting, the client waited 6.6-7.5 ms per tick for 8 s of its catch-up with 18 ms left over).
        g_period = kGameTickMs;
        return;
    }
    int peers = 0;
    int behind = dup_max_peer_backlog(kReportMaxAgeMs, peers);
    int queue = net_send_queue_playing_ms(); // ms, -1 nobody playing
    if (queue > g_queue_limit_ms && peers > 0) behind = std::max(behind, int(queue / g_period));
    g_last_backlog = behind;
    g_last_queue = queue;
    g_worst_queue = std::max(g_worst_queue, queue);
    if (peers == 0 || behind < 0) {
        g_period = std::max(kGameTickMs, g_period - 0.1); // nobody reporting: back to the game's pace
    } else if (behind > g_target) {
        double ceiling = (behind > 60) ? g_catchup_max_ms : g_max_ms;
        if (g_period > ceiling) {
            g_period = std::max(ceiling, g_period - 0.1);
        } else {
            g_period = std::min(ceiling, g_period + std::min(0.5, 0.04 + 0.002 * (behind - g_target)));
        }
    } else if (behind <= g_target / 2) {
        g_period = std::max(kGameTickMs, g_period - 0.05);
    }
    g_worst_backlog = std::max(g_worst_backlog, behind);
    // Per second while pacing is (or was just) active: the period and the backlog it reacts to.
    static DWORD s_last_second = 0;
    static double s_prev_period = kGameTickMs;
    if (now - s_last_second >= 1000 && (g_period > kGameTickMs + 0.05 || s_prev_period > kGameTickMs + 0.05)) {
        swlog("host pacing 1s: tick period %.2f ms (%.1f ticks/s), slowest client %d ticks behind, %d reporting; send queue %d ms",
              g_period, 1000.0 / g_period, behind, peers, queue);
        s_last_second = now;
        s_prev_period = g_period;
    }
    g_period_sum += g_period;
    g_period_n++;
    if (now - g_last_log >= 5000) {
        double avg = g_period_sum / g_period_n;
        if (avg > kGameTickMs + 0.05)
            swlog("host pacing 5s: tick period %.1f ms avg (%.0f ticks/s, game's own 16.0 = 62.5/s), now %.1f; slowest "
                  "client %d ticks behind (worst %d), %d client(s) reporting; send queue %d ms (worst %d)",
                  avg, 1000.0 / avg, g_period, g_last_backlog, g_worst_backlog, peers, g_last_queue, g_worst_queue);
        g_last_log = now;
        g_period_sum = 0;
        g_period_n = 0;
        g_worst_backlog = 0;
        g_worst_queue = 0;
    }
}

// Waits until the tick period has passed since `start`. False if there was nothing to wait for.
bool wait_period(int64_t start)
{
    if (g_period <= kGameTickMs + 0.05) return false; // the game's own 16 ms wait
    if (!g_timer) {
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!g_timer) return false;
    }
    double left = g_period - ms_since(start);
    if (left <= 0.2) return false; // the tick itself took that long already
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(left * 10000.0); // relative, 100 ns units
    if (SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(g_timer, 100);
    return true;
}

void on_begin(void*)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_loop_start = now.QuadPart;
    g_update_tid = GetCurrentThreadId();
    g_waited = false;
}

void __fastcall frame_end_detour(void* profiler, unsigned channel)
{
    if (channel == 0 && g_loop_start && GetCurrentThreadId() == g_update_tid && !g_zone_added) {
        // once, at the first tick (before anyone opens the profiler): the zone list grows by one
        static const char kName[] = "Waiting for Client";
        GameString name{kName, uint32_t(sizeof kName - 1), uint32_t(sizeof kName)};
        g_zone_add(profiler, 0, kWaitZone, &name);
        g_zone_added = true;
    }
    if (channel == 0 && g_zone_added && GetCurrentThreadId() == g_update_tid && g_period > kGameTickMs + 0.05) {
        g_zone_begin(profiler, 0, kWaitZone);
        g_waited = wait_period(g_loop_start);
        g_zone_end(profiler, 0, kWaitZone);
    }
    g_orig_frame_end(profiler, channel);
}

void on_idle(int64_t start, void*)
{
    DWORD now = GetTickCount();
    if (now - g_last_step >= kStepMs) {
        g_last_step = now;
        step(now);
    }
    if (!g_waited) wait_period(start); // no profiler frame this iteration (or no hook): wait here as before
    g_waited = false;
}

bool install()
{
    const wchar_t* ini = settings_ini();
    g_on = GetPrivateProfileIntW(L"net", L"host_pacing", 0, ini) != 0; // off by default: a last resort
    g_target = std::clamp(int(GetPrivateProfileIntW(L"net", L"pacing_target_ticks", 2, ini)), 2, 200);
    g_max_ms = std::clamp(int(GetPrivateProfileIntW(L"net", L"max_tick_ms", 33, ini)), 17, 50);
    g_catchup_max_ms = std::clamp(int(GetPrivateProfileIntW(L"net", L"catchup_max_tick_ms", 40, ini)), int(g_max_ms), 50);
    g_queue_limit_ms = std::max(50, int(GetPrivateProfileIntW(L"net", L"pacing_queue_ms", 250, ini)));
    if (!g_on) {
        swlog("host pacing: off in settings.ini");
        return true;
    }
    QueryPerformanceFrequency(&g_freq);
    if (game_build_matches()) g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    if (!swml()->on_loop(swml_self(), &on_begin, nullptr, &on_idle, nullptr)) {
        swlog("host pacing: unavailable (the core didn't recognise the game loop)");
        return false;
    }
    g_zone_add = game_ptr<ZoneAdd_t>(kZoneAdd);
    g_zone_begin = game_ptr<Zone_t>(kZoneBegin);
    g_zone_end = game_ptr<Zone_t>(kZoneEnd);
    if (!game_build_matches())
        swlog("host pacing: unknown game build; the wait shows as idle in the F1 profiler");
    else if (!hook_game(kFrameEnd, reinterpret_cast<void*>(&frame_end_detour), reinterpret_cast<void**>(&g_orig_frame_end)))
        swlog("host pacing: profiler frame hook failed; the wait shows as idle in the F1 profiler");
    swlog("host pacing: on (when hosting, slows to the slowest modded client: keep it within %d ticks, or a playing "
          "player's send queue under %d ms; at most %d ms per tick = %.0f ticks/s, catchup ceiling %d ms = %.0f ticks/s)",
          g_target, g_queue_limit_ms, int(g_max_ms), 1000.0 / g_max_ms, int(g_catchup_max_ms), 1000.0 / g_catchup_max_ms);
    return true;
}

HookRegistrar reg{{"host pacing", false, &install}};

} // namespace
