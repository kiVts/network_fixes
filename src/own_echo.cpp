
#include "hooks.h"
#include "log.h"
#include "sdk.h"
#include "game.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

constexpr uintptr_t kTickMessages = 0x14085aeb0;
constexpr size_t kScanBytes = 0xA000;
constexpr size_t kCharSeat = 0x2100, kCharOther = 0x2168;
constexpr size_t kGameNoLocalInput[2] = {0x358, 0x35c}; // the sender's two skip flags (dwords)
constexpr size_t kFields[3] = {0x23e0, 0x23e8, 0x2400}; // move, look (input copy), look
const uint8_t kHelperBytes[] = {0x8b, 0x02, 0x89, 0x01, 0x8b, 0x42, 0x04, 0x89, 0x41, 0x04, 0x48, 0x8b, 0xc1, 0xc3};

using Copy_t = void*(__fastcall*)(uint8_t* dst, const uint8_t* src);
using Messages_t = uint64_t(__fastcall*)(void*, void*, void*, void*, void*, void*);
Copy_t g_copy = nullptr;
Messages_t g_messages = nullptr;
thread_local const uint8_t* tl_game = nullptr; // the handler's `this` while it runs
thread_local const uint8_t* tl_own = nullptr;  // this player's own character while it runs
std::atomic<uint64_t> g_skipped{0}, g_applied{0};
std::atomic<uint64_t> g_last_log{0};
std::atomic<uint64_t> g_seated_writes{0};

// 1 = the player's own walking character (skip), 0 = apply, -1 = unreadable (apply)
int own_walking(const uint8_t* ch)
{
    __try {
        const uint8_t* g = tl_game;
        if (!g || *reinterpret_cast<const int*>(g + kGameNoLocalInput[0]) || *reinterpret_cast<const int*>(g + kGameNoLocalInput[1]))
            return 0; // the sender didn't write the character's input locally: the echo is its source
        return ch == tl_own && !*reinterpret_cast<void* const*>(ch + kCharSeat) && !*reinterpret_cast<void* const*>(ch + kCharOther);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void maybe_log()
{
    uint64_t now = GetTickCount64(), last = g_last_log.load(std::memory_order_relaxed);
    if (!last) {
        g_last_log.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 30000 || !g_last_log.compare_exchange_strong(last, now)) return;
    uint64_t s = g_skipped.exchange(0), a = g_applied.exchange(0), w = g_seated_writes.exchange(0);
    if (s || a || w)
        swlog("own echo 30s: %llu echoes of this player's own move / look not applied (walking), %llu applied (other players, seated); "
              "%llu ticks with this player's seated look / buttons from its own input",
              (unsigned long long)s / 3, (unsigned long long)a / 3, (unsigned long long)w);
}

template <int F> void* __fastcall copy_detour(uint8_t* dst, const uint8_t* src)
{
    if (own_walking(dst - kFields[F]) == 1) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        if (F == 2) maybe_log();
        return dst; // the helper returns its destination
    }
    g_applied.fetch_add(1, std::memory_order_relaxed);
    if (F == 2) maybe_log();
    return g_copy(dst, src);
}

bool g_seat_look = true;
const volatile uint8_t* g_is_host = nullptr;
constexpr uintptr_t kIsHost = 0x140ceb6ff;
constexpr size_t kGameWorld = 0xce190, kCtxPlayers = 0x1d8, kCtxLocalPeer = 0x338, kPlayerPeer = 0x80, kPlayerObj = 0x98,
                 kObjChar = 0x90;

// this player's own character: the local peer's entry in the handler's player ring (see the top), or null
uint8_t* own_character(const uint8_t* ctx)
{
    __try {
        if (!ctx) return nullptr;
        uint16_t peer = *reinterpret_cast<const uint16_t*>(ctx + kCtxLocalPeer);
        uint8_t* const* ring = *reinterpret_cast<uint8_t* const* const*>(ctx + kCtxPlayers);
        uint32_t cap = *reinterpret_cast<const uint32_t*>(ctx + kCtxPlayers + 8), head = *reinterpret_cast<const uint32_t*>(ctx + kCtxPlayers + 12),
                 count = *reinterpret_cast<const uint32_t*>(ctx + kCtxPlayers + 16);
        if (!ring || !cap || count > 256) return nullptr;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* pl = ring[(head + i) % cap];
            if (!pl || *reinterpret_cast<const uint16_t*>(pl + kPlayerPeer) != peer) continue;
            const uint8_t* obj = *reinterpret_cast<uint8_t* const*>(pl + kPlayerObj);
            uint8_t* const* h = obj ? *reinterpret_cast<uint8_t* const* const*>(obj + kObjChar) : nullptr;
            return h ? *h : nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// the local seated character's buttons / look / flags from the input record (see the top)
void write_seated(const uint8_t* game, uint8_t* ch)
{
    __try {
        if (*reinterpret_cast<const int*>(game + kGameNoLocalInput[0]) || *reinterpret_cast<const int*>(game + kGameNoLocalInput[1]))
            return;
        const uint8_t* rec = *reinterpret_cast<uint8_t* const*>(game + kGameWorld);
        if (!rec || !*reinterpret_cast<void* const*>(ch + kCharSeat)) return; // walking: the sender wrote it already
        memcpy(ch + 0x23b0, rec + 0x1a10, 16); // buttons
        memcpy(ch + 0x23e8, rec + 0x1a48, 8);  // look (input copy)
        memcpy(ch + 0x23f0, rec + 0x1a50, 5);  // flag bytes
        memcpy(ch + 0x2400, rec + 0x1a48, 8);  // look
        g_seated_writes.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uint64_t __fastcall messages_detour(void* game, void* a2, void* a3, void* a4, void* a5, void* a6)
{
    const uint8_t *prev = tl_game, *prev_own = tl_own;
    tl_game = static_cast<const uint8_t*>(game);
    tl_own = own_character(static_cast<const uint8_t*>(a3));
    uint64_t r = g_messages(game, a2, a3, a4, a5, a6);
    tl_game = prev;
    tl_own = prev_own;
    if (g_seat_look && g_is_host && *g_is_host == 0 && game)
        if (uint8_t* ch = own_character(static_cast<const uint8_t*>(a3))) write_seated(static_cast<const uint8_t*>(game), ch);
    maybe_log();
    return r;
}

uint8_t* alloc_near(uintptr_t target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x70000000; d += gran)
        for (uintptr_t a : {target - d, target + d}) {
            a &= ~(gran - 1);
            if (void* m = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(m);
        }
    return nullptr;
}

uint8_t* find_site(uint8_t* from, uint8_t* end, uint32_t field, uint8_t src_disp)
{
    uint8_t pat[13] = {0x48, 0x8D, 0x88, 0, 0, 0, 0, 0x49, 0x8D, 0x54, 0x24, src_disp, 0xE8};
    memcpy(pat + 3, &field, 4);
    for (uint8_t* p = from; p + sizeof pat + 4 <= end; p++)
        if (!memcmp(p, pat, sizeof pat)) return p + 12;
    return nullptr;
}

uint8_t* call_target(uint8_t* call)
{
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    return call + 5 + rel;
}

bool install()
{
    if (!GetPrivateProfileIntW(L"input", L"skip_own_echo", 1, settings_ini())) {
        swlog("own echo: off in settings.ini");
        return true;
    }
    g_seat_look = GetPrivateProfileIntW(L"input", L"local_seat_look", 1, settings_ini()) != 0;
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    auto* fn = reinterpret_cast<uint8_t*>(game_live(kTickMessages));
    if (!fn) return false;
    uint8_t* end = fn + kScanBytes;
    uint8_t* sites[3];
    sites[0] = find_site(fn, end, 0x23e0, 0x14);
    sites[1] = sites[0] ? find_site(sites[0], sites[0] + 0x40, 0x23e8, 0x1c) : nullptr;
    sites[2] = sites[1] ? find_site(sites[1], sites[1] + 0x40, 0x2400, 0x1c) : nullptr;
    if (!sites[2] || find_site(sites[0] + 1, end, 0x23e0, 0x14)) {
        swlog("own echo: set_input_move's copies not found (or not unique) in the tick-message handler; off");
        return true;
    }
    uint8_t* helper = call_target(sites[0]);
    if (call_target(sites[1]) != helper || call_target(sites[2]) != helper || memcmp(helper, kHelperBytes, sizeof kHelperBytes)) {
        swlog("own echo: the copies don't call the expected 2-float copy; off");
        return true;
    }
    g_copy = reinterpret_cast<Copy_t>(helper);
    uint8_t* stubs = alloc_near(reinterpret_cast<uintptr_t>(fn), 64);
    if (!stubs) {
        swlog("own echo: no memory within 2 GB of the game; off");
        return true;
    }
    void* detours[3] = {reinterpret_cast<void*>(&copy_detour<0>), reinterpret_cast<void*>(&copy_detour<1>),
                        reinterpret_cast<void*>(&copy_detour<2>)};
    for (int i = 0; i < 3; i++) {
        uint8_t* s = stubs + i * 16;
        const uint8_t jmp[] = {0xFF, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
        memcpy(s, jmp, 6);
        memcpy(s + 6, &detours[i], 8);
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, 48);
    for (int i = 0; i < 3; i++) {
        int64_t rel = reinterpret_cast<int64_t>(stubs + i * 16) - reinterpret_cast<int64_t>(sites[i] + 5);
        if (rel != int32_t(rel)) {
            swlog("own echo: stub out of reach; off");
            return true;
        }
    }
    if (!hook_game(kTickMessages, reinterpret_cast<void*>(&messages_detour), reinterpret_cast<void**>(&g_messages))) return false;
    for (int i = 0; i < 3; i++) {
        int32_t r32 = int32_t(reinterpret_cast<int64_t>(stubs + i * 16) - reinterpret_cast<int64_t>(sites[i] + 5));
        DWORD old;
        if (!VirtualProtect(sites[i] + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(sites[i] + 1, &r32, 4); // mod init: before the game's loop runs
        VirtualProtect(sites[i] + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), sites[i], 5);
    }
    swlog("own echo: on (the host's echo of this player's own set_input_move no longer overwrites its walking move / look; seated look / buttons / touch from its own input; "
          "\"own echo 30s\" lines)");
    return true;
}

HookRegistrar reg{{"own input echo", /*needs_known_build=*/true, &install}};

} // namespace
