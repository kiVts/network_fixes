
#include "hooks.h"
#include "log.h"
#include "game.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// --- piece size ------------------------------------------------------------------------
constexpr uintptr_t kPieceSites[] = {0x140189f76, 0x14018a199}; // mov r8d, 0x7fc00
constexpr uint8_t kPieceBytes[] = {0x41, 0xB8, 0x00, 0xFC, 0x07, 0x00};
constexpr uint32_t kGamePiece = 0x7fc00;

bool patch_piece_size(uint32_t payload)
{
    for (uintptr_t s : kPieceSites) {
        if (memcmp(game_ptr<const void*>(s), kPieceBytes, sizeof(kPieceBytes)) != 0) {
            swlog("net: piece size site %llx has unexpected bytes - piece size left at %u", static_cast<unsigned long long>(s),
                  kGamePiece);
            return false;
        }
    }
    for (uintptr_t s : kPieceSites) {
        uint8_t* imm = game_ptr<uint8_t*>(s) + 2;
        DWORD old;
        if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(imm, &payload, 4);
        VirtualProtect(imm, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), imm, 4);
    }
    swlog("net: send pieces %u -> %u bytes (+8 header): tick updates no longer wait behind 511 KB pieces", kGamePiece,
          payload);
    return true;
}

// --- network loop sleep ------------------------------------------------------------------
constexpr uintptr_t kSleepJump = 0x14018cc36;
constexpr uint8_t kSleepJumpBytes[] = {0x48, 0xFF, 0x25, 0x0B, 0x65, 0x90, 0x00};
constexpr int kMeasureLoops = 200;

using Sleep_t = void(WINAPI*)(DWORD);
Sleep_t g_tramp = nullptr; // unused: the detour calls Sleep itself
int g_loop_us = 1000;
HANDLE g_timer = nullptr;
DWORD g_thread = 0;
LARGE_INTEGER g_freq{};

// Network thread only.
int g_measured = 0;
double g_sleep_sum = 0, g_sleep_max = 0;
LONGLONG g_window_start = 0, g_last_wake = 0;
uint64_t g_loops = 0;
double g_work_max = 0, g_wait_sum = 0;

double ms_between(LONGLONG a, LONGLONG b) { return double(b - a) * 1000.0 / double(g_freq.QuadPart); }

LONGLONG now_qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Entered by the network loop's tail jump, i.e. instead of Sleep(1), returning to the thread loop.
void WINAPI net_sleep(DWORD ms)
{
    LONGLONG t0 = now_qpc();
    if (g_thread == 0) g_thread = GetCurrentThreadId();
    if (g_last_wake) g_work_max = std::max(g_work_max, ms_between(g_last_wake, t0));
    if (!g_window_start) g_window_start = t0;

    if (g_measured < kMeasureLoops || !g_timer || GetCurrentThreadId() != g_thread || ms != 1) {
        Sleep(ms);
        if (g_measured < kMeasureLoops) {
            double d = ms_between(t0, now_qpc());
            g_sleep_sum += d;
            g_sleep_max = std::max(g_sleep_max, d);
            if (++g_measured == kMeasureLoops) {
                swlog("net: game's network loop Sleep(1) takes %.2f ms on average (max %.2f) -> %s", g_sleep_sum / kMeasureLoops,
                      g_sleep_max,
                      g_timer ? "now waiting on a high-resolution timer" : "kept (no high-resolution timer)");
                if (g_timer) swlog("net: network loop wait = %d us", g_loop_us);
                g_window_start = 0;
                g_loops = 0;
                g_work_max = g_wait_sum = 0;
            }
        }
    } else {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(g_loop_us) * 10; // relative, 100 ns units
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, 100);
        else Sleep(ms);
    }

    LONGLONG t1 = now_qpc();
    g_wait_sum += ms_between(t0, t1);
    g_last_wake = t1;
    g_loops++;
    if (g_window_start && ms_between(g_window_start, t1) >= 60000.0) {
        double span = ms_between(g_window_start, t1);
        swlog("net loop 60s: %.0f loops/s, period %.2f ms (wait %.2f), slowest iteration %.1f ms", g_loops * 1000.0 / span,
              span / g_loops, g_wait_sum / g_loops, g_work_max);
        g_window_start = t1;
        g_loops = 0;
        g_work_max = g_wait_sum = 0;
    }
}

bool install()
{
    const wchar_t* ini = settings_ini();
    int piece_kb = GetPrivateProfileIntW(L"net", L"piece_kb", 16, ini);
    g_loop_us = GetPrivateProfileIntW(L"net", L"net_loop_us", 1000, ini);

    if (piece_kb > 0) {
        uint32_t payload = static_cast<uint32_t>(std::clamp(piece_kb, 1, 511)) * 1024 - 8;
        patch_piece_size(std::min(payload, kGamePiece)); // failure is logged; the rest still works
    }

    if (g_loop_us <= 0) return true;
    QueryPerformanceFrequency(&g_freq);
    if (memcmp(game_ptr<const void*>(kSleepJump), kSleepJumpBytes, sizeof(kSleepJumpBytes)) != 0) {
        swlog("net: network loop sleep site has unexpected bytes - left as is");
        return true;
    }
    // The jump's IAT slot must hold Sleep.
    const uint8_t* jmp = game_ptr<const uint8_t*>(kSleepJump);
    int32_t disp;
    memcpy(&disp, jmp + 3, 4);
    void* slot_target = *reinterpret_cast<void* const*>(jmp + sizeof(kSleepJumpBytes) + disp);
    void* k32 = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep"));
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    void* kbs = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "Sleep")) : nullptr;
    if (slot_target != k32 && slot_target != kbs) {
        swlog("net: network loop jump doesn't go to Sleep (%p) - left as is", slot_target);
        return true;
    }
    g_loop_us = std::clamp(g_loop_us, 250, 15000);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) swlog("net: no high-resolution timer (error %lu): network loop keeps Sleep(1), timing logged only", GetLastError());
    return hook_game(kSleepJump, reinterpret_cast<void*>(&net_sleep), reinterpret_cast<void**>(&g_tramp));
}

HookRegistrar reg{{"send piece size / 1 ms network loop", /*needs_known_build=*/true, &install}};

} // namespace
