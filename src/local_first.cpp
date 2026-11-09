
#include "hooks.h"
#include "log.h"
#include "sdk.h"
#include "game.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>

namespace {

constexpr uintptr_t kTickMessages = 0x14085aeb0, kQueuePush = 0x140234890, kIsHost = 0x140ceb6ff;
constexpr size_t kCtxQueue = 0x340, kCtxLocalPeer = 0x338, kGameWorld = 0xce190, kWorldSub = 0x38;
constexpr size_t kScanBytes = 0xA000;

enum Kind { kAttach, kDetach, kSfx, kInteract, kButton, kFloat, kLadderOn, kLadderOff, kScopeOn, kScopeOff, kDoor, kAnim, kKinds };
const char* const kNames[kKinds] = {"seat enter", "seat leave", "tool sound", "interactable", "button", "dial / slider",
                                    "ladder grab", "ladder leave", "scope raise", "scope lower", "door", "animation"};
constexpr uintptr_t kVt[kKinds] = {0x140af0ec0, 0x140b838a0, 0x140b58680, 0x140b3eb50, 0x140b053c8, 0x140b06dc0,
                                   0x140b69c40, 0x140b5ada0, 0x140b4b268, 0x140b54838, 0x140b114a0, 0x140b7bbb8};
constexpr size_t kLen[kKinds] = {0x18, 4, 0x24, 5, 0x28, 0x1c, 0x1c, 0x10, 0x14, 0, 0x16, 8};
// on / off pairs: they install together, and a mismatch drops the waiting copies of both (the host's order wins)
constexpr int kGroup[kKinds] = {1, 1, 0, 0, 0, 0, 2, 2, 3, 3, 0, 0};

struct Action {
    Kind kind;
    uint8_t d[0x28];
    ULONGLONG at;
    char name[64]; // a sound's name
};

struct GameString {
    const char* text;
    uint32_t len, cap;
};

// A sound name's key in the fields (FNV-1a hash at d + 8, length at d + 0x10, d + 0x14 zero) and, with `copy`, its text.
// false: unreadable or longer than Action::name (that sound isn't played early; its echo plays it).
bool name_key(uint8_t* d, const void* str, char* copy, size_t cap)
{
    __try {
        const GameString* g = static_cast<const GameString*>(str);
        if (!g || (!g->text && g->len) || g->len >= cap) return false;
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < g->len; i++) h = (h ^ uint8_t(g->text[i])) * 1099511628211ull;
        uint32_t len = g->len, zero = 0;
        memcpy(d + 8, &h, 8), memcpy(d + 0x10, &len, 4), memcpy(d + 0x14, &zero, 4);
        if (copy) {
            if (len) memcpy(copy, g->text, len);
            copy[len] = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using Push_t = uint64_t(__fastcall*)(void* queue, void* item);
using Messages_t = uint64_t(__fastcall*)(void*, void*, void*, void*, void*, void*);
using Attach_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer, uint32_t player, uint32_t vehicle, uint32_t body,
                                       const void* voxel, void* game_x);
using Detach_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer, uint32_t player, void* game_x);
using Sfx_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t a, uint32_t b, const void* name, uint32_t c, float f1, float f2);
using Interact_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer, uint32_t id, uint32_t flag);
using Button_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer, uint32_t a, uint32_t b, const void* v1, const void* v2,
                                       uint32_t c, uint32_t d);
using Float_t = uint64_t(__fastcall*)(void* w, void* ctx, void* game_x, uint32_t peer, uint32_t a, uint32_t b, const void* v,
                                      float f, uint32_t e);
using LadderOn_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer, uint32_t a, uint32_t b, uint32_t c, const void* v,
                                         uint32_t d);
using LadderOff_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer, uint32_t a, const void* v);
using ScopeOn_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer, uint32_t a, uint32_t b, const void* v);
using ScopeOff_t = uint64_t(__fastcall*)(void* w, void* ctx, uint32_t peer);
using Door_t = uint64_t(__fastcall*)(void* w, void* ctx, void* game_x1, void* game_x2, uint32_t peer, uint32_t a, uint32_t b,
                                     const void* v, uint32_t c, uint32_t d);

Push_t g_push = nullptr;
Messages_t g_messages = nullptr;
void* g_fn[kKinds] = {}; // the handlers (live)
uintptr_t g_vt[kKinds] = {};
const volatile uint8_t* g_is_host = nullptr;
size_t g_game_x = 0;    // game + this goes to the seat / dial / door handlers (0xce5f8, 0xce600 in 0x6ae31989)
size_t g_game_door = 0; // and this to the door handler too (0xce5c8 in the original build)
bool g_on[kKinds] = {};

std::mutex g_mu;
std::deque<Action> g_pending; // queued requests not applied yet
std::deque<Action> g_waiting; // applied locally, echo not back yet (in order)
uint8_t* g_ctx = nullptr;     // the tick-message handler's 3rd argument (holds the request queue)
// own animation: read from case 0xc's code at install
using Lookup_t = uint8_t*(__fastcall*)(void* ctx, uint32_t peer);
using Deref_t = uint8_t*(__fastcall*)(void* handle);
using SetWord_t = void(__fastcall*)(void* comp, const uint32_t* v);
using SetByte_t = void(__fastcall*)(void* comp, uint32_t v);
Deref_t g_deref = nullptr;
SetWord_t g_set_a = nullptr, g_set_b = nullptr;
SetByte_t g_set_c = nullptr, g_set_d = nullptr, g_set_e = nullptr;
constexpr size_t kPlayerObj = 0x98, kObjHandle = 0x90, kCharAnim = 0x290, kCharAnimByte = 0x43d;
bool g_anim_local = false; // our own animation has been applied here: the host's echoes of it are stale
uint8_t* g_game = nullptr;    // its 1st

std::atomic<uint64_t> g_applied[kKinds], g_skipped[kKinds], g_mismatch[kKinds], g_aged{0};
std::atomic<uint64_t> g_last_log{0};

bool is_client() { return g_is_host && *g_is_host == 0; }
void* world_sub() { return reinterpret_cast<uint8_t*>(*reinterpret_cast<uintptr_t*>(g_game + kGameWorld)) + kWorldSub; }
uint32_t local_peer() { return *reinterpret_cast<uint16_t*>(g_ctx + kCtxLocalPeer); }
uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
float f32(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }

uint64_t __fastcall push_detour(void* queue, void* item)
{
    if (item && g_ctx && queue == g_ctx + kCtxQueue && is_client()) {
        uintptr_t vt = *static_cast<uintptr_t*>(item);
        for (int k = 0; k < kKinds; k++)
            if (vt == g_vt[k] && g_on[k]) {
                Action a{Kind(k), {}, GetTickCount64(), {}};
                memcpy(a.d, static_cast<uint8_t*>(item) + 0x10, kLen[k]);
                if (k == kSfx && !name_key(a.d, static_cast<uint8_t*>(item) + 0x18, a.name, sizeof a.name)) break;
                std::lock_guard<std::mutex> l(g_mu);
                g_pending.push_back(a);
                break;
            }
    }
    return g_push(queue, item);
}

void apply(const Action& a)
{
    const uint8_t* d = a.d;
    void* w = world_sub();
    void* gx = g_game + g_game_x;
    uint32_t peer = local_peer();
    switch (a.kind) {
    case kAttach: reinterpret_cast<Attach_t>(g_fn[kAttach])(w, g_ctx, peer, u32(d), u32(d + 4), u32(d + 8), d + 0xc, gx); break;
    case kDetach: reinterpret_cast<Detach_t>(g_fn[kDetach])(w, g_ctx, peer, u32(d), gx); break;
    case kSfx: {
        GameString name{a.name, u32(d + 0x10), u32(d + 0x10) + 1};
        reinterpret_cast<Sfx_t>(g_fn[kSfx])(w, g_ctx, u32(d), u32(d + 4), &name, d[0x18], f32(d + 0x1c), f32(d + 0x20));
        break;
    }
    case kInteract: reinterpret_cast<Interact_t>(g_fn[kInteract])(w, g_ctx, peer, u32(d), d[4]); break;
    case kButton:
        reinterpret_cast<Button_t>(g_fn[kButton])(w, g_ctx, peer, u32(d), u32(d + 4), d + 8, d + 0x18, d[0x14], d[0x24]);
        break;
    case kFloat:
        reinterpret_cast<Float_t>(g_fn[kFloat])(w, g_ctx, gx, peer, u32(d), u32(d + 4), d + 8, f32(d + 0x14), u32(d + 0x18));
        break;
    case kLadderOn:
        reinterpret_cast<LadderOn_t>(g_fn[kLadderOn])(w, g_ctx, peer, u32(d), u32(d + 4), u32(d + 8), d + 0xc, u32(d + 0x18));
        break;
    case kLadderOff: reinterpret_cast<LadderOff_t>(g_fn[kLadderOff])(w, g_ctx, peer, u32(d), d + 4); break;
    case kScopeOn: reinterpret_cast<ScopeOn_t>(g_fn[kScopeOn])(w, g_ctx, peer, u32(d), u32(d + 4), d + 8); break;
    case kScopeOff: reinterpret_cast<ScopeOff_t>(g_fn[kScopeOff])(w, g_ctx, peer); break;
    case kDoor:
        reinterpret_cast<Door_t>(g_fn[kDoor])(w, g_ctx, g_game + g_game_door, gx, peer, u32(d), u32(d + 4), d + 8, d[0x14], d[0x15]);
        break;
    case kAnim: {
        uint8_t* player = reinterpret_cast<Lookup_t>(g_fn[kAnim])(g_ctx, peer);
        uint8_t* obj = player ? *reinterpret_cast<uint8_t**>(player + kPlayerObj) : nullptr;
        uint8_t* h = obj ? g_deref(obj + kObjHandle) : nullptr;
        uint8_t* ch = h ? g_deref(h) : nullptr;
        if (!ch) return;
        uint32_t a16 = uint16_t(u32(d + 2)), b16 = uint16_t(u32(d));
        g_set_a(ch + kCharAnim, &a16);
        g_set_b(ch + kCharAnim, &b16);
        g_set_c(ch + kCharAnim, d[7]);
        g_set_d(ch + kCharAnim, d[4]);
        g_set_e(ch + kCharAnim, d[5]);
        ch[kCharAnimByte] = d[6];
        g_anim_local = true;
        break;
    }
    default: break;
    }
    g_applied[a.kind].fetch_add(1, std::memory_order_relaxed);
}

// Equal as the handlers see them (padding bytes aren't compared).
bool same(Kind kind, const uint8_t* x, const uint8_t* y)
{
    switch (kind) {
    case kSfx: return !memcmp(x, y, 0x18) && x[0x18] == y[0x18] && !memcmp(x + 0x1c, y + 0x1c, 8);
    case kButton: return !memcmp(x, y, 0x14) && x[0x14] == y[0x14] && !memcmp(x + 0x18, y + 0x18, 12) && x[0x24] == y[0x24];
    default: return !memcmp(x, y, kLen[kind]);
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
    char line[512];
    int n = 0;
    uint64_t total = 0;
    for (int k = 0; k < kKinds; k++) {
        uint64_t a = g_applied[k].exchange(0), s = g_skipped[k].exchange(0), m = g_mismatch[k].exchange(0);
        total += a + m;
        if (a || m)
            n += snprintf(line + n, sizeof line - n, "%s%s %llu (%llu echoes skipped, %llu not matching)", n ? ", " : "",
                          kNames[k], (unsigned long long)a, (unsigned long long)s, (unsigned long long)m);
    }
    uint64_t aged = g_aged.exchange(0);
    if (total || aged) swlog("local first 30s: done locally: %s; %llu waiting copies dropped (120 s / 512 limit)", line, (unsigned long long)aged);
}

uint64_t __fastcall messages_detour(void* game, void* a2, void* ctx, void* a4, void* a5, void* a6)
{
    if (game != g_game || ctx != g_ctx) {
        std::lock_guard<std::mutex> l(g_mu);
        g_pending.clear();
        g_waiting.clear();
        g_anim_local = false; // a new session: the host's state first, until we animate ourselves
        g_game = static_cast<uint8_t*>(game);
        g_ctx = static_cast<uint8_t*>(ctx);
    }
    std::deque<Action> todo;
    {
        std::lock_guard<std::mutex> l(g_mu);
        todo.swap(g_pending);
        ULONGLONG now = GetTickCount64();
        // The echo always comes (reliable stream) but can take as long as the client's backlog: after a join that's
        // 20-30 s of queued ticks. Dropping a copy early would apply the echo a second time (a toggle flipping back),
        // so the limits are only a safety net.
        while (!g_waiting.empty() && (now - g_waiting.front().at > 120000 || g_waiting.size() > 512)) {
            g_waiting.pop_front();
            g_aged.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (is_client())
        for (const Action& a : todo) {
            apply(a);
            std::lock_guard<std::mutex> l(g_mu);
            if (a.kind != kAnim) g_waiting.push_back(a); // animation: every own echo is skipped (anim_site)
        }
    maybe_log();
    return g_messages(game, a2, ctx, a4, a5, a6);
}

// An event of `kind` with these request-ordered fields arrived; `ours` = it came from this player. true = already
// done here: skip it.
bool own_echo(Kind kind, const uint8_t* fields, bool ours)
{
    if (!ours || !g_on[kind]) return false;
    std::lock_guard<std::mutex> l(g_mu);
    for (auto it = g_waiting.begin(); it != g_waiting.end(); ++it) {
        if (it->kind != kind) continue;
        if (same(kind, it->d, fields)) { // the oldest waiting copy of this kind
            g_waiting.erase(it);
            g_skipped[kind].fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        break;
    }
    bool had = false;
    for (auto it = g_waiting.begin(); it != g_waiting.end();)
        if (it->kind == kind || (kGroup[kind] && kGroup[it->kind] == kGroup[kind])) { // the host's order wins: ours are stale
            it = g_waiting.erase(it);
            had = true;
        } else {
            ++it;
        }
    if (had) g_mismatch[kind].fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool mine(void* ctx, uint32_t peer) { return is_client() && ctx == g_ctx && (peer & 0xffff) == local_peer(); }

uint64_t __fastcall attach_site(void* w, void* ctx, uint32_t peer, uint32_t player, uint32_t vehicle, uint32_t body,
                                const void* voxel, void* game_x)
{
    if (mine(ctx, peer)) {
        uint8_t f[0x18];
        memcpy(f, &player, 4), memcpy(f + 4, &vehicle, 4), memcpy(f + 8, &body, 4), memcpy(f + 0xc, voxel, 12);
        if (own_echo(kAttach, f, true)) return 0;
    }
    return reinterpret_cast<Attach_t>(g_fn[kAttach])(w, ctx, peer, player, vehicle, body, voxel, game_x);
}

uint64_t __fastcall detach_site(void* w, void* ctx, uint32_t peer, uint32_t player, void* game_x)
{
    if (mine(ctx, peer) && own_echo(kDetach, reinterpret_cast<const uint8_t*>(&player), true)) return 0;
    return reinterpret_cast<Detach_t>(g_fn[kDetach])(w, ctx, peer, player, game_x);
}

uint64_t __fastcall sfx_site(void* w, void* ctx, uint32_t a, uint32_t b, const void* name, uint32_t c, float f1, float f2)
{
    if (is_client() && ctx == g_ctx) {
        uint8_t f[0x24] = {};
        memcpy(f, &a, 4), memcpy(f + 4, &b, 4);
        f[0x18] = uint8_t(c);
        memcpy(f + 0x1c, &f1, 4), memcpy(f + 0x20, &f2, 4);
        // a sound event carries no peer: only an exact copy of one we played counts as ours
        bool ours = false;
        if (name_key(f, name, nullptr, sizeof(Action::name))) {
            std::lock_guard<std::mutex> l(g_mu);
            for (const Action& x : g_waiting)
                if (x.kind == kSfx && same(kSfx, x.d, f)) {
                    ours = true;
                    break;
                }
        }
        if (ours && own_echo(kSfx, f, true)) return 0;
    }
    return reinterpret_cast<Sfx_t>(g_fn[kSfx])(w, ctx, a, b, name, c, f1, f2);
}

uint64_t __fastcall interact_site(void* w, void* ctx, uint32_t peer, uint32_t id, uint32_t flag)
{
    if (mine(ctx, peer)) {
        uint8_t f[5];
        memcpy(f, &id, 4);
        f[4] = uint8_t(flag);
        if (own_echo(kInteract, f, true)) return 0;
    }
    return reinterpret_cast<Interact_t>(g_fn[kInteract])(w, ctx, peer, id, flag);
}

uint64_t __fastcall button_site(void* w, void* ctx, uint32_t peer, uint32_t a, uint32_t b, const void* v1, const void* v2,
                                uint32_t c, uint32_t d)
{
    if (mine(ctx, peer)) {
        uint8_t f[0x28] = {};
        memcpy(f, &a, 4), memcpy(f + 4, &b, 4), memcpy(f + 8, v1, 12);
        f[0x14] = uint8_t(c);
        memcpy(f + 0x18, v2, 12);
        f[0x24] = uint8_t(d);
        if (own_echo(kButton, f, true)) return 0;
    }
    return reinterpret_cast<Button_t>(g_fn[kButton])(w, ctx, peer, a, b, v1, v2, c, d);
}

uint64_t __fastcall float_site(void* w, void* ctx, void* game_x, uint32_t peer, uint32_t a, uint32_t b, const void* v, float fv,
                               uint32_t e)
{
    if (mine(ctx, peer)) {
        uint8_t f[0x1c];
        memcpy(f, &a, 4), memcpy(f + 4, &b, 4), memcpy(f + 8, v, 12), memcpy(f + 0x14, &fv, 4), memcpy(f + 0x18, &e, 4);
        if (own_echo(kFloat, f, true)) return 0;
    }
    return reinterpret_cast<Float_t>(g_fn[kFloat])(w, ctx, game_x, peer, a, b, v, fv, e);
}

uint64_t __fastcall ladder_on_site(void* w, void* ctx, uint32_t peer, uint32_t a, uint32_t b, uint32_t c, const void* v,
                                   uint32_t dd)
{
    if (mine(ctx, peer)) {
        uint8_t f[0x1c];
        memcpy(f, &a, 4), memcpy(f + 4, &b, 4), memcpy(f + 8, &c, 4), memcpy(f + 0xc, v, 12), memcpy(f + 0x18, &dd, 4);
        if (own_echo(kLadderOn, f, true)) return 0;
    }
    return reinterpret_cast<LadderOn_t>(g_fn[kLadderOn])(w, ctx, peer, a, b, c, v, dd);
}

uint64_t __fastcall ladder_off_site(void* w, void* ctx, uint32_t peer, uint32_t a, const void* v)
{
    if (mine(ctx, peer)) {
        uint8_t f[0x10];
        memcpy(f, &a, 4), memcpy(f + 4, v, 12);
        if (own_echo(kLadderOff, f, true)) return 0;
    }
    return reinterpret_cast<LadderOff_t>(g_fn[kLadderOff])(w, ctx, peer, a, v);
}

uint64_t __fastcall scope_on_site(void* w, void* ctx, uint32_t peer, uint32_t a, uint32_t b, const void* v)
{
    if (mine(ctx, peer)) {
        uint8_t f[0x14];
        memcpy(f, &a, 4), memcpy(f + 4, &b, 4), memcpy(f + 8, v, 12);
        if (own_echo(kScopeOn, f, true)) return 0;
    }
    return reinterpret_cast<ScopeOn_t>(g_fn[kScopeOn])(w, ctx, peer, a, b, v);
}

uint64_t __fastcall scope_off_site(void* w, void* ctx, uint32_t peer)
{
    uint8_t none = 0;
    if (mine(ctx, peer) && own_echo(kScopeOff, &none, true)) return 0;
    return reinterpret_cast<ScopeOff_t>(g_fn[kScopeOff])(w, ctx, peer);
}

uint64_t __fastcall door_site(void* w, void* ctx, void* gx1, void* gx2, uint32_t peer, uint32_t a, uint32_t b, const void* v,
                              uint32_t c, uint32_t dd)
{
    if (mine(ctx, peer)) {
        uint8_t f[0x16];
        memcpy(f, &a, 4), memcpy(f + 4, &b, 4), memcpy(f + 8, v, 12);
        f[0x14] = uint8_t(c), f[0x15] = uint8_t(dd);
        if (own_echo(kDoor, f, true)) return 0;
    }
    return reinterpret_cast<Door_t>(g_fn[kDoor])(w, ctx, gx1, gx2, peer, a, b, v, c, dd);
}

// case 0xc's player lookup: the own player's animation echo is stale once we animate it ourselves
uint8_t* __fastcall anim_site(void* ctx, uint32_t peer)
{
    if (g_anim_local && is_client() && ctx == g_ctx && (peer & 0xffff) == local_peer()) {
        g_skipped[kAnim].fetch_add(1, std::memory_order_relaxed);
        return nullptr; // the case leaves without touching the character
    }
    return reinterpret_cast<Lookup_t>(g_fn[kAnim])(ctx, peer);
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

// The unique code before each handler call, ending in its E8. 0x100 = any byte (rel32 / disp32 that differ by build).
constexpr uint16_t W = 0x100;
const uint16_t kAttachDetach[] = {
    0x41, 0x8B, 0x44, 0x24, 0x1C, 0x89, 0x44, 0x24, 0x28, 0x41, 0x8B, 0x44, 0x24, 0x18, 0x89, 0x44, 0x24, 0x20,
    0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7, 0x44, 0x24, 0x10, 0x49, 0x8B, 0xD7,
    0xE8, W, W, W, W,                                                                                           // call attach
    0xE9, W, W, W, W,                                                                                           // jmp out
    0x49, 0x8D, 0x85, W, W, W, W,                                                                               // lea rax,[r13+game_x]
    0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x48, 0x89, 0x44, 0x24, 0x20,
    0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7, 0x44, 0x24, 0x10, 0x49, 0x8B, 0xD7,
    0xE8, W, W, W, W}; // call detach
const uint16_t kSfxCall[] = {
    0xF3, 0x0F, 0x10, 0x40, 0x30, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x38, 0xF3, 0x0F, 0x10, 0x48, 0x2C, 0xF3, 0x0F, 0x11,
    0x4C, 0x24, 0x30, 0x0F, 0xB6, 0x40, 0x28, 0x88, 0x44, 0x24, 0x28, 0x48, 0x89, 0x54, 0x24, 0x20, 0x45, 0x8B, 0x4C,
    0x24, 0x14, 0x45, 0x8B, 0x44, 0x24, 0x10, 0x49, 0x8B, 0xD7, 0xE8, W, W, W, W};
const uint16_t kInteractCall[] = {
    0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x0F, 0xB6, 0x40, 0x18, 0x88, 0x44, 0x24, 0x20,
    0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7, 0x44, 0x24, 0x10, 0x49, 0x8B, 0xD7, 0xE8, W, W, W, W};
const uint16_t kButtonCall[] = {
    0xF2, 0x0F, 0x10, 0x40, 0x2C, 0xF2, 0x0F, 0x11, 0x85, W, W, W, W, 0x8B, 0x40, 0x34, 0x89, 0x85, W, W, W, W,
    0xF2, 0x41, 0x0F, 0x10, 0x44, 0x24, 0x1C, 0xF2, 0x0F, 0x11, 0x85, W, W, W, W, 0x41, 0x8B, 0x44, 0x24, 0x24, 0x89,
    0x85, W, W, W, W, 0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x41, 0x0F, 0xB6, 0x44, 0x24,
    0x38, 0x88, 0x44, 0x24, 0x40, 0x41, 0x0F, 0xB6, 0x44, 0x24, 0x28, 0x88, 0x44, 0x24, 0x38, 0x48, 0x8D, 0x85, W, W,
    W, W, 0x48, 0x89, 0x44, 0x24, 0x30, 0x48, 0x8D, 0x85, W, W, W, W, 0x48, 0x89, 0x44, 0x24, 0x28, 0x41, 0x8B, 0x44,
    0x24, 0x18, 0x89, 0x44, 0x24, 0x20, 0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7, 0x44, 0x24, 0x10, 0x49, 0x8B,
    0xD7, 0xE8, W, W, W, W};
const uint16_t kFloatCall[] = {
    0xF2, 0x0F, 0x10, 0x40, 0x1C, 0xF2, 0x0F, 0x11, 0x85, W, W, W, W, 0x8B, 0x40, 0x24, 0x89, 0x85, W, W, W, W,
    0x4D, 0x8D, 0x85, W, W, W, W, 0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x41, 0x8B, 0x44,
    0x24, 0x2C, 0x89, 0x44, 0x24, 0x40, 0xF3, 0x41, 0x0F, 0x10, 0x44, 0x24, 0x28, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x38,
    0x48, 0x8D, 0x85, W, W, W, W, 0x48, 0x89, 0x44, 0x24, 0x30, 0x41, 0x8B, 0x44, 0x24, 0x18, 0x89, 0x44, 0x24, 0x28,
    0x41, 0x8B, 0x44, 0x24, 0x14, 0x89, 0x44, 0x24, 0x20, 0x45, 0x0F, 0xB7, 0x4C, 0x24, 0x10, 0x49, 0x8B, 0xD7, 0xE8,
    W, W, W, W};

const uint16_t kLadderOnCall[] = {
    0xF2, 0x0F, 0x10, 0x40, 0x20, 0xF2, 0x0F, 0x11, 0x85, W, W, W, W, 0x8B, 0x40, 0x28, 0x89, 0x85, W, W, W, W,
    0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x41, 0x8B, 0x44, 0x24, 0x2C, 0x89, 0x44, 0x24,
    0x38, 0x48, 0x8D, 0x85, W, W, W, W, 0x48, 0x89, 0x44, 0x24, 0x30, 0x41, 0x8B, 0x44, 0x24, 0x1C, 0x89, 0x44, 0x24,
    0x28, 0x41, 0x8B, 0x44, 0x24, 0x18, 0x89, 0x44, 0x24, 0x20, 0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7, 0x44,
    0x24, 0x10, 0x49, 0x8B, 0xD7, 0xE8, W, W, W, W};
const uint16_t kLadderOffCall[] = {
    0xF2, 0x0F, 0x10, 0x40, 0x18, 0xF2, 0x0F, 0x11, 0x85, W, W, W, W, 0x8B, 0x40, 0x20, 0x89, 0x85, W, W, W, W,
    0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x48, 0x8D, 0x85, W, W, W, W, 0x48, 0x89, 0x44,
    0x24, 0x20, 0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7, 0x44, 0x24, 0x10, 0x49, 0x8B, 0xD7, 0xE8, W, W, W, W};
const uint16_t kScopeOnCall[] = {
    0xF2, 0x0F, 0x10, 0x40, 0x1C, 0xF2, 0x0F, 0x11, 0x85, W, W, W, W, 0x8B, 0x40, 0x24, 0x89, 0x85, W, W, W, W,
    0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x48, 0x8D, 0x85, W, W, W, W, 0x48, 0x89, 0x44,
    0x24, 0x28, 0x41, 0x8B, 0x44, 0x24, 0x18, 0x89, 0x44, 0x24, 0x20, 0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7,
    0x44, 0x24, 0x10, 0x49, 0x8B, 0xD7, 0xE8, W, W, W, W};
const uint16_t kScopeOffCall[] = {
    0x45, 0x8B, 0x4C, 0x24, 0x14, 0x45, 0x0F, 0xB7, 0x44, 0x24, 0x10, 0x49, 0x8B, 0xD7, 0xE8, W, W, W, W, // scope raise
    0xE9, W, W, W, W,                                                                                 // jmp out
    0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83, 0xC1, 0x38, 0x44, 0x0F, 0xB7, 0x40, 0x10, 0x49, 0x8B, 0xD7,
    0xE8, W, W, W, W};
const uint16_t kDoorCall[] = {
    0xF2, 0x0F, 0x10, 0x40, 0x1C, 0xF2, 0x0F, 0x11, 0x85, W, W, W, W, 0x8B, 0x40, 0x24, 0x89, 0x85, W, W, W, W,
    0x4D, 0x8D, 0x8D, W, W, W, W, 0x4D, 0x8D, 0x85, W, W, W, W, 0x49, 0x8B, 0x8D, 0x90, 0xE1, 0x0C, 0x00, 0x48, 0x83,
    0xC1, 0x38, 0x41, 0x0F, 0xB6, 0x44, 0x24, 0x29, 0x88, 0x44, 0x24, 0x48, 0x41, 0x0F, 0xB6, 0x44, 0x24, 0x28, 0x88,
    0x44, 0x24, 0x40, 0x48, 0x8D, 0x85, W, W, W, W, 0x48, 0x89, 0x44, 0x24, 0x38, 0x41, 0x8B, 0x44, 0x24, 0x18, 0x89,
    0x44, 0x24, 0x30, 0x41, 0x8B, 0x44, 0x24, 0x14, 0x89, 0x44, 0x24, 0x28, 0x41, 0x0F, 0xB7, 0x44, 0x24, 0x10, 0x66,
    0x89, 0x44, 0x24, 0x20, 0x49, 0x8B, 0xD7, 0xE8, W, W, W, W};

const uint16_t kAnimCase[] = {
    0x0F, 0xB7, 0x50, 0x10, 0x49, 0x8B, 0xCF, 0xE8, W, W, W, W, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x84, W, W, W, W,
    0x48, 0x8B, 0x88, 0x98, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x0F, 0x84, W, W, W, W, 0x48, 0x81, 0xC1, 0x90, 0x00, 0x00,
    0x00, 0xE8, W, W, W, W, 0x48, 0x85, 0xC0, 0x0F, 0x84, W, W, W, W, 0x48, 0x8B, 0x8B, 0x98, 0x00, 0x00, 0x00, 0x48, 0x81,
    0xC1, 0x90, 0x00, 0x00, 0x00, 0xE8, W, W, W, W, 0x48, 0x8B, 0xC8, 0xE8, W, W, W, W, 0x48, 0x85, 0xC0, 0x0F, 0x84, W,
    W, W, W, 0x48, 0x8B, 0x8B, 0x98, 0x00, 0x00, 0x00, 0x48, 0x81, 0xC1, 0x90, 0x00, 0x00, 0x00, 0xE8, W, W, W, W, 0x48,
    0x8B, 0xC8, 0xE8, W, W, W, W, 0x48, 0x8B, 0xF0, 0x41, 0x0F, 0xB7, 0x4C, 0x24, 0x14, 0x89, 0x4D, 0x78, 0x48, 0x8D, 0x55,
    0x78, 0x48, 0x8D, 0x88, 0x90, 0x02, 0x00, 0x00, 0xE8, W, W, W, W, 0x41, 0x0F, 0xB7, 0x54, 0x24, 0x12, 0x89, 0x55, 0x7C,
    0x48, 0x8D, 0x55, 0x7C, 0x48, 0x8D, 0x8E, 0x90, 0x02, 0x00, 0x00, 0xE8, W, W, W, W, 0x41, 0x0F, 0xB6, 0x54, 0x24, 0x19,
    0x48, 0x8D, 0x8E, 0x90, 0x02, 0x00, 0x00, 0xE8, W, W, W, W, 0x41, 0x0F, 0xB6, 0x54, 0x24, 0x16, 0x48, 0x8D, 0x8E, 0x90,
    0x02, 0x00, 0x00, 0xE8, W, W, W, W, 0x41, 0x0F, 0xB6, 0x54, 0x24, 0x17, 0x48, 0x8D, 0x8E, 0x90, 0x02, 0x00, 0x00, 0xE8,
    W, W, W, W, 0x41, 0x0F, 0xB6, 0x44, 0x24, 0x18, 0x88, 0x86, 0x3D, 0x04, 0x00, 0x00};

template <size_t N> uint8_t* find_unique(uint8_t* from, uint8_t* end, const uint16_t (&pat)[N])
{
    uint8_t* found = nullptr;
    for (uint8_t* p = from; p + N <= end; p++) {
        size_t i = 0;
        while (i < N && (pat[i] == W || p[i] == pat[i])) i++;
        if (i < N) continue;
        if (found) return nullptr; // not unique
        found = p;
    }
    return found;
}
template <size_t N> uint8_t* find_first_of_two(uint8_t* from, uint8_t* end, const uint16_t (&pat)[N])
{
    uint8_t* found[3] = {};
    int n = 0;
    for (uint8_t* p = from; p + N <= end && n < 3; p++) {
        size_t i = 0;
        while (i < N && (pat[i] == W || p[i] == pat[i])) i++;
        if (i == N) found[n++] = p;
    }
    return n == 2 ? found[0] : nullptr;
}
template <size_t N> uint8_t* call_at_end(uint8_t* match, const uint16_t (&)[N]) { return match ? match + N - 5 : nullptr; }

uint8_t* target_of(uint8_t* call)
{
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    return call + 5 + rel;
}

bool patch_call(uint8_t* call, uint8_t* stub)
{
    int64_t rel = stub - (call + 5);
    if (rel != int32_t(rel)) return false;
    int32_t r32 = int32_t(rel);
    DWORD old;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(call + 1, &r32, 4); // mod init: before the game's loop runs
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

bool install()
{
    const wchar_t* ini = settings_ini();
    bool seats = GetPrivateProfileIntW(L"input", L"local_seats", 1, ini) != 0;
    bool sounds = GetPrivateProfileIntW(L"input", L"local_tool_sounds", 1, ini) != 0;
    bool buttons = GetPrivateProfileIntW(L"input", L"local_buttons", 1, ini) != 0;
    bool ladders = GetPrivateProfileIntW(L"input", L"local_ladders", 1, ini) != 0;
    bool scopes = GetPrivateProfileIntW(L"input", L"local_scopes", 1, ini) != 0;
    bool doors = GetPrivateProfileIntW(L"input", L"local_doors", 1, ini) != 0;
    bool anims = GetPrivateProfileIntW(L"input", L"local_animations", 1, ini) != 0;
    if (!seats && !sounds && !buttons && !ladders && !scopes && !doors && !anims) {
        swlog("local first: off in settings.ini");
        return true;
    }
    auto* fn = reinterpret_cast<uint8_t*>(game_live(kTickMessages));
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    if (!fn || !g_is_host) return false;
    for (int k = 0; k < kKinds; k++)
        if (!(g_vt[k] = game_live(kVt[k]))) return false;
    uint8_t* end = fn + kScanBytes;
    uint8_t* ad = find_unique(fn, end, kAttachDetach);
    uint8_t* calls[kKinds] = {ad ? ad + 32 : nullptr, call_at_end(ad, kAttachDetach), call_at_end(find_unique(fn, end, kSfxCall), kSfxCall),
                              call_at_end(find_unique(fn, end, kInteractCall), kInteractCall),
                              call_at_end(find_unique(fn, end, kButtonCall), kButtonCall),
                              call_at_end(find_first_of_two(fn, end, kFloatCall), kFloatCall),
                              call_at_end(find_unique(fn, end, kLadderOnCall), kLadderOnCall),
                              call_at_end(find_unique(fn, end, kLadderOffCall), kLadderOffCall),
                              call_at_end(find_unique(fn, end, kScopeOnCall), kScopeOnCall),
                              call_at_end(find_unique(fn, end, kScopeOffCall), kScopeOffCall),
                              call_at_end(find_unique(fn, end, kDoorCall), kDoorCall), nullptr};
    bool want[kKinds] = {seats, seats, sounds, buttons, buttons, buttons, ladders, ladders, scopes, scopes, doors, anims};
    if (uint8_t* ac = find_unique(fn, end, kAnimCase)) { // its 11 calls: lookup, 5 derefs (one function), 5 setters
        uint8_t* call[11];
        int n = 0;
        for (size_t i = 0; i + 4 < sizeof kAnimCase / sizeof kAnimCase[0] && n < 11; i++)
            if (kAnimCase[i] == 0xE8 && kAnimCase[i + 1] == W) call[n++] = ac + i;
        bool ok = n == 11;
        for (int i = 2; ok && i <= 5; i++) ok = target_of(call[i]) == target_of(call[1]);
        if (ok) {
            calls[kAnim] = call[0];
            g_deref = reinterpret_cast<Deref_t>(target_of(call[1]));
            g_set_a = reinterpret_cast<SetWord_t>(target_of(call[6]));
            g_set_b = reinterpret_cast<SetWord_t>(target_of(call[7]));
            g_set_c = reinterpret_cast<SetByte_t>(target_of(call[8]));
            g_set_d = reinterpret_cast<SetByte_t>(target_of(call[9]));
            g_set_e = reinterpret_cast<SetByte_t>(target_of(call[10]));
        }
    }
    if (ad) {
        int32_t gx;
        memcpy(&gx, ad + 45, 4);
        g_game_x = size_t(gx);
    }
    if (uint8_t* door = find_unique(fn, end, kDoorCall)) {
        int32_t x1, x2;
        memcpy(&x2, door + 25, 4), memcpy(&x1, door + 32, 4);
        g_game_door = size_t(x1);
        if (!g_game_x || size_t(x2) != g_game_x) want[kDoor] = false; // must agree with the seat calls' offset
    }
    uint8_t* stubs = alloc_near(reinterpret_cast<uintptr_t>(fn), 4096);
    if (!stubs) {
        swlog("local first: no memory within 2 GB of the game; off");
        return true;
    }
    void* detours[kKinds] = {reinterpret_cast<void*>(&attach_site), reinterpret_cast<void*>(&detach_site),
                             reinterpret_cast<void*>(&sfx_site), reinterpret_cast<void*>(&interact_site),
                             reinterpret_cast<void*>(&button_site), reinterpret_cast<void*>(&float_site),
                             reinterpret_cast<void*>(&ladder_on_site), reinterpret_cast<void*>(&ladder_off_site),
                             reinterpret_cast<void*>(&scope_on_site), reinterpret_cast<void*>(&scope_off_site),
                             reinterpret_cast<void*>(&door_site), reinterpret_cast<void*>(&anim_site)};
    for (int k = 0; k < kKinds; k++) {
        const uint8_t jmp[] = {0xFF, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
        memcpy(stubs + k * 16, jmp, 6);
        memcpy(stubs + k * 16 + 6, &detours[k], 8);
    }
    FlushInstructionCache(GetCurrentProcess(), stubs, kKinds * 16);
    if (!hook_game(kQueuePush, reinterpret_cast<void*>(&push_detour), reinterpret_cast<void**>(&g_push)) ||
        !hook_game(kTickMessages, reinterpret_cast<void*>(&messages_detour), reinterpret_cast<void**>(&g_messages)))
        return false;
    char missing[256] = "";
    int n = 0;
    for (int k = 0; k < kKinds; k++) {
        if (!want[k]) continue;
        // an on / off pair goes together: one without the other would leave the player half attached
        bool pair_missing = false;
        for (int o = 0; o < kKinds; o++)
            if (kGroup[k] && kGroup[o] == kGroup[k] && !calls[o]) pair_missing = true;
        if (!calls[k] || pair_missing) {
            n += snprintf(missing + n, sizeof missing - n, "%s%s", n ? ", " : "", kNames[k]);
            continue;
        }
        g_fn[k] = target_of(calls[k]);
        if (!patch_call(calls[k], stubs + k * 16)) {
            n += snprintf(missing + n, sizeof missing - n, "%s%s (patch failed)", n ? ", " : "", kNames[k]);
            g_fn[k] = nullptr;
            continue;
        }
        g_on[k] = true;
    }
    swlog("local first: on for clients (own seats, ladders, scopes, doors, buttons, dials, interactables, tool sounds and "
          "animation at once; own echoes skipped; game+%#zx)%s%s; \"local first 30s\" lines",
          g_game_x, n ? "; not found, so as vanilla: " : "", missing);
    return true;
}

HookRegistrar reg{{"local first", /*needs_known_build=*/true, &install}};

} // namespace
