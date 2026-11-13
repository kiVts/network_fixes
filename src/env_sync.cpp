
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "net_shared.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace {

constexpr uintptr_t kOceanTick = 0x1402bc810;
constexpr uintptr_t kOceanTickReturn = 0x1405ea0d6;
constexpr uintptr_t kTornadoLoad = 0x1402c198a;     // the tornado distance
constexpr uint8_t kTornadoBytes[] = {0xf2, 0x41, 0x0f, 0x10, 0x57, 0x08, 0x66, 0x0f, 0x5a, 0xd2};
constexpr uintptr_t kIsHost = 0x140ceb6ff;
constexpr size_t kWindX = 0xc024, kWindZ = 0xc028;
constexpr DWORD kFreshMs = 2000;

using OceanTick_t = void(__fastcall*)(uint64_t, uint64_t, uint64_t);
OceanTick_t g_orig = nullptr;
uintptr_t g_return = 0;
const volatile uint8_t* g_is_host = nullptr;
bool g_ocean_sync = true, g_tornado_on = true;
uint8_t* g_tornado = nullptr; // the byte the fix rewrites, or null when the site isn't there

// host: its own target, written by the update thread, read by the network thread
std::atomic<uint32_t> g_host_x{0}, g_host_z{0};
std::atomic<bool> g_host_have{false};
// client: the host's target and flags, written by the network thread
std::atomic<uint32_t> g_recv_x{0}, g_recv_z{0};
std::atomic<DWORD> g_recv_at{0};
std::atomic<bool> g_recv_tornado{false}, g_recv_ocean{false};
// host: every connected player said it has this (redundancy.cpp)
std::atomic<bool> g_peers_capable{true};

std::atomic<uint64_t> g_applied{0}, g_published{0};
DWORD g_last_log = 0;
bool g_tornado_patched = false; // update thread only

uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
float flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
bool hosting() { return g_is_host && *g_is_host; }
bool fresh() { DWORD at = g_recv_at.load(std::memory_order_relaxed); return at && GetTickCount() - at < kFreshMs; }

void set_tornado(bool on)
{
    if (!g_tornado || on == g_tornado_patched) return;
    DWORD old;
    if (!VirtualProtect(g_tornado, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *g_tornado = on ? 0x10 : 0x08;
    VirtualProtect(g_tornado, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_tornado, 1);
    g_tornado_patched = on;
    swlog("env: tornado weather distance fix %s (%s)", on ? "on" : "off",
          hosting() ? (on ? "every player has it" : "a player without it is connected") : (on ? "the host has it on" : "the host doesn't"));
}

// The simulation scene's ocean, right before the game eases it toward its wind target this tick.
void on_ocean_tick(uint8_t* o)
{
    {
        bool host = hosting();
        if (g_tornado_on) set_tornado(host ? g_peers_capable.load(std::memory_order_relaxed)
                                           : fresh() && g_recv_tornado.load(std::memory_order_relaxed));
        if (g_ocean_sync) {
            if (host) {
                g_host_x.store(bits(*reinterpret_cast<float*>(o + kWindX)), std::memory_order_relaxed);
                g_host_z.store(bits(*reinterpret_cast<float*>(o + kWindZ)), std::memory_order_relaxed);
                g_host_have.store(true, std::memory_order_relaxed);
            } else if (fresh() && g_recv_ocean.load(std::memory_order_relaxed)) {
                *reinterpret_cast<float*>(o + kWindX) = flt(g_recv_x.load(std::memory_order_relaxed));
                *reinterpret_cast<float*>(o + kWindZ) = flt(g_recv_z.load(std::memory_order_relaxed));
                g_applied.fetch_add(1, std::memory_order_relaxed);
            }
        }
        DWORD now = GetTickCount();
        if (now - g_last_log >= 60000) {
            if (g_last_log && (g_applied || g_published))
                swlog("env 60s: %s; ocean wind target %.2f, %.2f; tornado fix %s",
                      host ? "host, sea state sent to modded players" : "client, sea state from the host",
                      *reinterpret_cast<float*>(o + kWindX), *reinterpret_cast<float*>(o + kWindZ), g_tornado_patched ? "on" : "off");
            g_last_log = now;
            g_applied = g_published = 0;
        }
    }
}

void __fastcall ocean_detour(uint64_t ocean, uint64_t a2, uint64_t tick)
{
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_return && ocean) on_ocean_tick(reinterpret_cast<uint8_t*>(ocean));
    g_orig(ocean, a2, tick);
}

bool install()
{
    const wchar_t* ini = settings_ini();
    g_ocean_sync = GetPrivateProfileIntW(L"env", L"ocean_sync", 1, ini) != 0;
    g_tornado_on = GetPrivateProfileIntW(L"env", L"tornado_fix", 1, ini) != 0;
    if (!g_ocean_sync && !g_tornado_on) {
        swlog("env: off in settings.ini");
        return true;
    }
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    g_return = game_live_if_mapped(kOceanTickReturn);
    if (!g_is_host || !g_return) {
        swlog("env: the scene update's ocean call isn't on this build - off");
        return true;
    }
    if (g_tornado_on) {
        auto* site = reinterpret_cast<uint8_t*>(game_live_if_mapped(kTornadoLoad));
        if (site && !memcmp(site, kTornadoBytes, sizeof kTornadoBytes)) g_tornado = site + 5;
        else swlog("env: tornado weather site has unexpected bytes - the game's distance kept");
    }
    if (!hook_game(kOceanTick, reinterpret_cast<void*>(&ocean_detour), reinterpret_cast<void**>(&g_orig))) return false;
    swlog("env: shared sea state %s (host sends its ocean wind to modded players twice a second); tornado weather fix %s",
          g_ocean_sync ? "on" : "off", g_tornado ? "available (on only when every player has it)" : "off");
    return true;
}

HookRegistrar reg{{"shared sea state / tornado weather fix", /*needs_known_build=*/true, &install}};

} // namespace

// redundancy.cpp (network thread)
bool env_hosting() { return hosting(); }

uint32_t env_host_payload(uint8_t* out, uint32_t cap)
{
    if (!g_is_host || !hosting() || cap < 12) return 0;
    if (!g_host_have.load(std::memory_order_relaxed) && !g_tornado) return 0;
    uint32_t x = g_host_x.load(std::memory_order_relaxed), z = g_host_z.load(std::memory_order_relaxed);
    uint32_t flags = (g_host_have.load(std::memory_order_relaxed) && g_ocean_sync ? 1u : 0u) |
                     (g_tornado_patched ? 2u : 0u);
    memcpy(out, &x, 4), memcpy(out + 4, &z, 4), memcpy(out + 8, &flags, 4);
    g_published.fetch_add(1, std::memory_order_relaxed);
    return 12;
}

void env_on_host(const uint8_t* data, uint32_t size)
{
    if (size < 12 || !g_is_host || hosting()) return;
    uint32_t x, z, flags;
    memcpy(&x, data, 4), memcpy(&z, data + 4, 4), memcpy(&flags, data + 8, 4);
    float fx = flt(x), fz = flt(z);
    if (!(fx == fx) || !(fz == fz) || fx > 1e4f || fx < -1e4f || fz > 1e4f || fz < -1e4f) return; // NaN / garbage
    g_recv_tornado.store((flags & 2) != 0, std::memory_order_relaxed);
    g_recv_ocean.store((flags & 1) != 0, std::memory_order_relaxed);
    if (flags & 1) {
        g_recv_x.store(x, std::memory_order_relaxed);
        g_recv_z.store(z, std::memory_order_relaxed);
    }
    const DWORD now = GetTickCount();
    g_recv_at.store(now ? now : 1, std::memory_order_relaxed); // 0 = never
}

void env_set_peers_capable(bool all) { g_peers_capable.store(all, std::memory_order_relaxed); }
bool env_available() { return g_orig != nullptr; }
