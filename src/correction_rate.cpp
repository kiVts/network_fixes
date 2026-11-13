
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "net_shared.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

namespace {

constexpr uintptr_t kIntervalAdd = 0x14074745a; // add eax, 5 ; mov ecx, 0x2d0 (the 720-tick cap)
constexpr uint8_t kIntervalBytes[] = {0x83, 0xC0, 0x05, 0xB9, 0xD0, 0x02, 0x00, 0x00};
constexpr DWORD kStepMs = 250, kCalmMs = 3000, kMaxCalmMs = 60000, kBounceMs = 10000;

constexpr uintptr_t kMovedEnough = 0x1407468f0;
using MovedEnough_t = uint64_t(__fastcall*)(void*, const double*, const double*, uint32_t);
MovedEnough_t g_orig_moved = nullptr;
uint32_t g_tolerance_ticks = 2;
uint64_t g_tightened = 0; // calls answered with a smaller interval than the game's (update thread only)

uint8_t* g_site = nullptr;
int g_min = 1, g_max = 10, g_queue_ms = 100;
int g_n = 5; // the byte as it is now (the game's 5 until install sets it)
// Far vehicles (1.11.12): the same expression caps the interval at 720 ticks (the `mov ecx, 0x2d0` after the add):
// a vehicle a few km from a player is corrected once every 11.5 s, so far ships and radar contacts drift and then
// jump. While the upload keeps up (n at its minimum) the cap is correction_far_ticks (default 180 = 3 s): one more
// small message per far moving vehicle every 3 s at most. As soon as n backs off, the game's 720 is back.
// [net] correction_far_ticks (0 = the game's 720).
constexpr int kGameFarTicks = 720;
int g_far = 180;
int g_far_now = kGameFarTicks; // the imm32 as it is now

void set_far(int ticks)
{
    if (!g_site || ticks == g_far_now) return;
    DWORD old;
    if (!VirtualProtect(g_site + 4, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    uint32_t v = uint32_t(ticks);
    memcpy(g_site + 4, &v, 4); // mov ecx, imm32; only its two low bytes change, any mix of them is a valid cap (60..720)
    VirtualProtect(g_site + 4, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_site + 4, 4);
    g_far_now = ticks;
}
DWORD g_last_step = 0, g_calm_since = 0;
DWORD g_calm_needed = kCalmMs, g_last_down = 0, g_last_over = 0;
int g_worst_queue = 0; // since the last change, for its log line

void set_n(int n, int queue_ms)
{
    if (g_far > 0) set_far(n <= g_min ? g_far : kGameFarTicks);
    if (n == g_n) return;
    DWORD old;
    if (!VirtualProtect(g_site + 2, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    g_site[2] = static_cast<uint8_t>(n); // the imm8 of `add eax, n`: one byte, read by the host tick's next pass
    VirtualProtect(g_site + 2, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_site + 2, 1);
    swlog("vehicle corrections: nearby vehicles every %d tick(s) (was %d; largest send queue %d ms, worst %d ms since the last "
          "change)",
          n, g_n, queue_ms, g_worst_queue);
    g_n = n;
    g_worst_queue = 0;
}

// One step of the controller: q = the largest send queue now (ms, -1 nobody connected).
void step(DWORD now, int q)
{
    if (q < 0) { // nobody connected
        g_calm_since = now;
        g_worst_queue = 0;
        if (g_n != g_min) set_n(g_min, q);
        return;
    }
    g_worst_queue = std::max(g_worst_queue, q);
    if (q > g_queue_ms) {
        if (g_last_down && now - g_last_down < kBounceMs) g_calm_needed = std::min(kMaxCalmMs, g_calm_needed * 2);
        g_last_down = 0;
        g_last_over = g_calm_since = now;
        set_n(std::min(g_max, g_n + (q > 4 * g_queue_ms ? 2 : 1)), q);
    } else if (q > g_queue_ms / 4) {
        g_calm_since = now;
    } else {
        if (now - g_last_over >= kMaxCalmMs) g_calm_needed = kCalmMs;
        if (now - g_calm_since >= g_calm_needed && g_n > g_min) {
            g_last_down = g_calm_since = now;
            set_n(g_n - 1, q);
        }
    }
}

uint64_t __fastcall moved_detour(void* a, const double* last, const double* now, uint32_t interval)
{
    const uint32_t limit = std::max(g_tolerance_ticks, static_cast<uint32_t>(g_n));
    if (interval > limit) {
        interval = limit;
        g_tightened++;
    }
    return g_orig_moved(a, last, now, interval);
}

// Update thread, at the start of each loop pass (the host tick that reads the byte runs there too).
void on_begin(void*)
{
    DWORD now = GetTickCount();
    if (now - g_last_step < kStepMs) return;
    g_last_step = now;
    step(now, net_send_queue_ms());
}

bool install()
{
    const wchar_t* ini = settings_ini();
    g_min = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"net", L"correction_min_ticks", 1, ini)), 1, 5);
    g_max = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"net", L"correction_max_ticks", 10, ini)), g_min, 30);
    g_queue_ms = std::max(10, static_cast<int>(GetPrivateProfileIntW(L"net", L"correction_queue_ms", 100, ini)));
    // Independent of the interval patch below: the game's own 5 ticks next to a body are kept as the floor when that
    // patch is off (g_n stays 5).
    g_tolerance_ticks = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"net", L"correction_tolerance_ticks", 2, ini)), 0, 720);
    g_far = static_cast<int>(GetPrivateProfileIntW(L"net", L"correction_far_ticks", 180, ini));
    g_far = g_far <= 0 ? 0 : std::clamp(g_far, 60, kGameFarTicks);
    if (!g_tolerance_ticks)
        swlog("net: vehicle correction tolerance: the game's (grows with the player's distance; correction_tolerance_ticks=0)");
    else if (hook_game(kMovedEnough, &moved_detour, reinterpret_cast<void**>(&g_orig_moved)))
        swlog("net: vehicle correction tolerance: at most %u tick(s)' worth (%.2f degrees, %u mm) at any distance instead of "
              "growing with it; how often a far body is sent is unchanged (only matters while hosting)",
              g_tolerance_ticks, g_tolerance_ticks * 0.0573, g_tolerance_ticks * 2);
    else
        swlog("net: vehicle correction tolerance left as the game's (comparison function not found on this build)");
    if (g_min == 5 && g_max == 5) {
        swlog("net: vehicle corrections at the game's rate (correction_min_ticks=5, correction_max_ticks=5)");
        return true;
    }
    g_site = game_ptr<uint8_t*>(kIntervalAdd);
    if (memcmp(g_site, kIntervalBytes, sizeof kIntervalBytes) != 0) {
        swlog("net: vehicle correction interval site has unexpected bytes - left at the game's 5 ticks");
        return true;
    }
    if (!swml()->on_loop(swml_self(), &on_begin, nullptr, nullptr, nullptr)) {
        swlog("net: vehicle corrections at the game's rate (the core didn't recognise the game loop)");
        return true;
    }
    set_n(g_min, 0);
    if (g_min == g_n && g_far) set_far(g_far); // set_n returns early when n already was g_min
    swlog("net: vehicle corrections: nearby vehicles every %d tick(s) instead of 5 while the upload keeps up, up to every %d "
          "when a player's send queue passes %d ms (only matters while hosting); far vehicles at least every %d ticks "
          "instead of 720 while the upload keeps up",
          g_min, g_max, g_queue_ms, g_far ? g_far : kGameFarTicks);
    return true;
}

HookRegistrar reg{{"vehicle correction rate", /*needs_known_build=*/true, &install}};

} // namespace
