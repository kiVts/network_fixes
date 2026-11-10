
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "editor_background.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace {

constexpr uintptr_t kTick = 0x1408483e0;                       // (game state, ?, ?, ?): one update loop iteration
constexpr uintptr_t kMainLoop = 0x140848ae0, kMainLoopSize = 2936;
constexpr uintptr_t kIsHost = 0x140ceb6ff;                     // byte: 1 host / single player, 0 client
constexpr uintptr_t kCatchUp = 0x1408686c0;
constexpr uint32_t kQueueCount = 0xce300;
constexpr uint32_t kFrontState = 0x350, kSimulation = 0xce190; // verified in both supported builds' main loops
constexpr uint32_t kBehind = 14; // above this the game's own catch-up formula runs its full 10 ticks per call

using U = uint64_t;
using Tick_t = void(__fastcall*)(U, U, U, U);
Tick_t g_orig = nullptr;
uintptr_t g_main_lo = 0, g_main_hi = 0;
const volatile uint8_t* g_is_host = nullptr;
int g_budget_ms = 100;
int g_editor_ticks = 1;
LARGE_INTEGER g_freq{};

volatile LONG* g_editor_tid = nullptr;

uint8_t* cap_site(uint8_t* fn, size_t size)
{
    constexpr uint8_t pattern[] = {0x41, 0xbf, 10, 0, 0, 0, 0x41, 0x3b, 0xcf, 0x44, 0x0f, 0x42, 0xf9};
    uint8_t* found = nullptr;
    for (size_t i = 0; i + sizeof(pattern) <= size; ++i) {
        if (memcmp(fn + i, pattern, sizeof(pattern))) continue;
        if (found) return nullptr;
        found = fn + i;
    }
    return found;
}

bool install_editor_limit(uint8_t* fn, size_t size, int ticks)
{
    if (!fn || ticks < 1 || ticks > 10) return false;
    uint8_t* site = cap_site(fn, size);
    if (!site) return false;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t target = reinterpret_cast<uintptr_t>(site);
    const size_t page = si.dwPageSize;
    uint8_t* mem = nullptr;
    for (uintptr_t d = si.dwAllocationGranularity; d < 0x70000000 && !mem; d += si.dwAllocationGranularity) {
        for (uintptr_t a : {target - d, target + d}) {
            a &= ~(uintptr_t(si.dwAllocationGranularity) - 1);
            mem = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), 2 * page,
                                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (mem) break;
        }
    }
    if (!mem) return false;
    uint8_t code[] = {
        0x9c,                                                 // pushfq
        0x65, 0x44, 0x8b, 0x3c, 0x25, 0x48, 0, 0, 0,         // mov r15d,gs:[48h] (thread id)
        0x44, 0x3b, 0x3d, 0, 0, 0, 0,                         // cmp r15d,[rip+thread_id]
        0x41, 0xbf, 10, 0, 0, 0,                              // mov r15d,10
        0x75, 0x06,                                           // jne restore_flags
        0x41, 0xbf, 0, 0, 0, 0,                               // mov r15d,editor_ticks
        0x9d,                                                 // popfq
        0xe9, 0, 0, 0, 0};                                    // jmp site+6
    int32_t tid_rel = static_cast<int32_t>(page - 17);
    int32_t back = static_cast<int32_t>(reinterpret_cast<intptr_t>(site + 6) -
                                        reinterpret_cast<intptr_t>(mem + sizeof(code)));
    memcpy(code + 13, &tid_rel, 4);
    memcpy(code + 27, &ticks, 4);
    memcpy(code + 33, &back, 4);
    memcpy(mem, code, sizeof(code));
    auto* tid = reinterpret_cast<volatile LONG*>(mem + page);
    *tid = 0;
    DWORD old = 0;
    if (!VirtualProtect(mem, page, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }
    uint8_t jump[] = {0xe9, 0, 0, 0, 0, 0x90};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(mem) - reinterpret_cast<intptr_t>(site + 5));
    memcpy(jump + 1, &rel, 4);
    if (!VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }
    memcpy(site, jump, sizeof(jump)); // installation, before game execution starts
    DWORD ignored;
    VirtualProtect(site, sizeof(jump), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jump));
    g_editor_tid = tid;
    return true;
}

bool editor_front(U game)
{
    auto front = *reinterpret_cast<const uintptr_t*>(game + kFrontState);
    auto sim = *reinterpret_cast<const uintptr_t*>(game + kSimulation);
    return front && sim && front != sim;
}

std::atomic<bool> g_inline{false}; // written by the main thread, cleared by the update thread when it takes over
// Main thread only.
DWORD g_inline_since = 0, g_win_start = 0;
uint32_t g_backlog_at_open = 0;
uint64_t g_frames = 0, g_extra = 0, g_catchup_frames = 0;
uint64_t g_editor_frames = 0;
uint32_t g_win_max = 0;

double ms_since(LONGLONG t0)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - t0) * 1000.0 / double(g_freq.QuadPart);
}

uint32_t queued(U game) { return *reinterpret_cast<const uint32_t*>(game + kQueueCount); }

void note_inline(U game, DWORD now)
{
    if (!g_inline.load(std::memory_order_relaxed)) {
        g_inline.store(true, std::memory_order_relaxed);
        g_inline_since = g_win_start = now;
        g_backlog_at_open = queued(game);
        g_frames = g_extra = g_catchup_frames = 0;
        g_editor_frames = 0;
        g_win_max = 0;
        swlog("map open: the game simulates on the render thread now (full-screen map, or a screen such as the editor; backlog %u ticks)", g_backlog_at_open);
    }
}

void __fastcall tick_detour(U game, U a2, U a3, U a4)
{
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    bool client = !*g_is_host;
    bool main = ret >= g_main_lo && ret < g_main_hi;
    if (client && main && editor_background_frame(game, a2, a3)) {
        g_inline.store(false, std::memory_order_relaxed); // world replay belongs to the worker, so pacing can report
        return;
    }
    bool editor = client && main && editor_front(game);
    // Set this BEFORE the first call: previously even map_catchup_ms=0 couldn't stop its ten-tick batch.
    if (editor && g_editor_tid) InterlockedExchange(g_editor_tid, static_cast<LONG>(GetCurrentThreadId()));
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    __try { g_orig(game, a2, a3, a4); }
    __finally { if (editor && g_editor_tid) InterlockedExchange(g_editor_tid, 0); }
    if (client && !main) editor_background_world_tick(game);
    if (!client) return;
    if (!main) { // the update thread's call: the map is closed
        if (g_inline.exchange(false, std::memory_order_relaxed))
            swlog("map open: map / screen closed after %.0f s, the update thread simulates again (backlog %u ticks, %u when it opened)",
                  (GetTickCount() - g_inline_since) / 1000.0, queued(game), g_backlog_at_open);
        return;
    }
    DWORD now = GetTickCount();
    note_inline(game, now);
    g_frames++;
    if (editor) g_editor_frames++;
    double last_ms = ms_since(t0.QuadPart); // the game's own call
    if (!editor && g_budget_ms > 0 && queued(game) > kBehind && last_ms < g_budget_ms) {
        g_catchup_frames++;
        while (queued(game) > kBehind && ms_since(t0.QuadPart) + last_ms <= g_budget_ms) {
            uint32_t before = queued(game);
            double c0 = ms_since(t0.QuadPart);
            g_orig(game, a2, a3, a4);
            last_ms = ms_since(t0.QuadPart) - c0;
            g_extra++;
            // No progress (a state in which the game doesn't play queued ticks, e.g. still connecting): don't spin.
            if (queued(game) >= before) break;
        }
    }
    g_win_max = std::max(g_win_max, queued(game));
    if (now - g_win_start >= 10000) {
        if (g_editor_frames)
            swlog("editor catch-up 10s: %llu frames (%.1f fps), %llu limited to at most %d host ticks, no extra calls; backlog now %u (max %u)",
                  static_cast<unsigned long long>(g_frames), g_frames * 1000.0 / (now - g_win_start),
                  static_cast<unsigned long long>(g_editor_frames), g_editor_ticks, queued(game), g_win_max);
        if (g_catchup_frames)
            swlog("map catch-up 10s: %llu frames (%.1f fps), %llu catching up with %llu extra update calls, backlog now %u (max %u)",
                  static_cast<unsigned long long>(g_frames), g_frames * 1000.0 / (now - g_win_start),
                  static_cast<unsigned long long>(g_catchup_frames), static_cast<unsigned long long>(g_extra), queued(game), g_win_max);
        g_win_start = now;
        g_frames = g_extra = g_catchup_frames = 0;
        g_editor_frames = 0;
        g_win_max = 0;
    }
}

bool install()
{
    g_budget_ms = std::clamp(int(GetPrivateProfileIntW(L"net", L"map_catchup_ms", 100, settings_ini())), 0, 1000);
    g_editor_ticks = std::clamp(int(GetPrivateProfileIntW(L"net", L"editor_catchup_ticks", 1, settings_ini())), 0, 10);
    QueryPerformanceFrequency(&g_freq);
    g_main_lo = reinterpret_cast<uintptr_t>(game_ptr<void*>(kMainLoop));
    g_main_hi = g_main_lo + per_build<uintptr_t>(kMainLoopSize, 2984); // build 0x6ae218ec: the loop grew 48 bytes
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    if (!g_is_host || !g_main_lo) return false;
    bool limited = !g_editor_ticks;
    bool ok = hook_game(kTick, reinterpret_cast<void*>(&tick_detour), reinterpret_cast<void**>(&g_orig));
    if (ok && g_editor_ticks) {
        limited = install_editor_limit(game_ptr<uint8_t*>(kCatchUp), 1728, g_editor_ticks);
        swlog("editor catch-up: %s (client foreground screen: at most %d host ticks/frame, no extra calls; world updates retained)",
              limited ? "on" : "FAILED, native cap pattern / allocation unavailable", g_editor_ticks);
    }
    swlog("map catch-up: %s", !ok ? "FAILED" : g_budget_ms ? "on (client, map open and behind: up to the frame budget)" : "off, map open/close logged only");
    if (ok && g_budget_ms) swlog("map catch-up: frame budget %d ms", g_budget_ms);
    return ok && limited;
}

HookRegistrar reg{{"map open catch-up", /*needs_known_build=*/true, &install}};

} // namespace

bool map_inline_active() { return g_inline.load(std::memory_order_relaxed); }

