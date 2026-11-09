
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

#include "event_names.inc"

constexpr uintptr_t kHostSend = 0x14023e9c0; // host: per tick, every player's queued messages -> network
constexpr size_t kPlayers = 0x190, kPlayerLocal = 0xa0, kRingTicks = 0xd8, kRingEvents = 0xf0;
constexpr uint32_t kCorrection = 0x81;
constexpr size_t kScratchBytes = 8u << 20;
constexpr DWORD kEveryMs = 5000;

using Send_t = void(__fastcall*)(void* session);
using Write_t = void(__fastcall*)(void* message, void* stream);
Send_t g_orig = nullptr;

struct Stream {
    const void* buffer;
    uint8_t* cursor;
    uint32_t left;
    uint8_t mode, pad[3];
};

struct TypeSum {
    uint64_t bytes = 0, count = 0;
};
// Update thread only (the host's tick).
TypeSum g_type[2][256];
uint64_t g_ring_bytes[2] = {}, g_ticks = 0, g_truncated = 0, g_failed = 0;
uint64_t g_tick_no = 0; // never reset: when each record was last sent
uint32_t g_players = 0;
uint64_t g_bodies = 0, g_kind1 = 0, g_same = 0, g_exact = 0;
DWORD g_last_log = 0;
uint8_t* g_scratch = nullptr;

struct Rec {
    uint8_t b[0x29];
    uint64_t seen; // g_tick_no when last sent (0 = never)
};
std::unordered_map<uint64_t, Rec> g_last; // (player, vehicle, body) -> the record last sent

uint64_t key_of(const void* player, uint32_t vehicle, uint32_t body)
{
    uint64_t h = reinterpret_cast<uintptr_t>(player) * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(vehicle) << 16 | body) + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    return h;
}

// Bytes one message writes, or -1 if its write function faulted. Truncated if the scratch ran out.
int64_t written(void* msg, bool& truncated)
{
    __try {
        Stream s{g_scratch, g_scratch, uint32_t(kScratchBytes), 1, {}};
        (*reinterpret_cast<Write_t* const*>(msg))[0](msg, &s);
        truncated = s.left == 0;
        return s.cursor - g_scratch;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void correction(const void* player, const uint8_t* msg)
{
    uint32_t vehicle = *reinterpret_cast<const uint32_t*>(msg + 0x10);
    const uint8_t* recs = *reinterpret_cast<uint8_t* const*>(msg + 0x18);
    uint32_t n = *reinterpret_cast<const uint32_t*>(msg + 0x20);
    if (!recs || n > 4096) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* r = recs + i * 0x30;
        g_bodies++;
        g_kind1 += r[0x28] == 1;
        Rec& last = g_last[key_of(player, vehicle, i)];
        if (last.seen) {
            bool exact = !memcmp(last.b, r, 0x29);
            bool same = exact;
            if (!same && last.b[0x28] == r[0x28]) {
                float q0[4], q1[4];
                double p0[3], p1[3];
                memcpy(q0, last.b, 16), memcpy(q1, r, 16), memcpy(p0, last.b + 0x10, 24), memcpy(p1, r + 0x10, 24);
                double d2 = 0;
                for (int k = 0; k < 3; k++) d2 += (p1[k] - p0[k]) * (p1[k] - p0[k]);
                same = d2 < 1e-6;
                for (int k = 0; k < 4 && same; k++) same = std::fabs(q1[k] - q0[k]) < 1e-4f;
            }
            g_exact += exact;
            g_same += same;
        }
        memcpy(last.b, r, 0x29);
        last.seen = g_tick_no;
    }
}

void measure_ring(const void* player, const uint8_t* ring, int ch)
{
    const uint8_t* const* data = *reinterpret_cast<const uint8_t* const* const*>(ring);
    uint32_t cap = *reinterpret_cast<const uint32_t*>(ring + 8), first = *reinterpret_cast<const uint32_t*>(ring + 0xc),
             n = *reinterpret_cast<const uint32_t*>(ring + 0x10);
    if (!n || !data || !cap || n > 1000000) return;
    g_ring_bytes[ch] += 4; // the message count
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* msg = const_cast<uint8_t*>(data[(first + i) % cap]);
        if (!msg) continue;
        uint32_t type = *reinterpret_cast<const uint32_t*>(msg + 8);
        bool truncated = false;
        int64_t b = written(msg, truncated);
        if (b < 0) {
            g_failed++;
            continue;
        }
        g_truncated += truncated;
        TypeSum& t = g_type[ch][type & 0xff];
        t.bytes += uint64_t(b) + 4;
        t.count++;
        g_ring_bytes[ch] += uint64_t(b) + 4;
        if (ch == 0 && type == kCorrection) correction(player, msg);
    }
}

bool measure_safe(const uint8_t* session, uint32_t& players)
{
    __try {
        const uint8_t* const* data = *reinterpret_cast<const uint8_t* const* const*>(session + kPlayers);
        uint32_t cap = *reinterpret_cast<const uint32_t*>(session + kPlayers + 8),
                 first = *reinterpret_cast<const uint32_t*>(session + kPlayers + 0xc),
                 n = *reinterpret_cast<const uint32_t*>(session + kPlayers + 0x10);
        if (!data || !cap || n > 256) return false;
        players = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* p = data[(first + i) % cap];
            if (!p || p[kPlayerLocal]) continue; // this PC's own player: its messages aren't sent
            players++;
            measure_ring(p, p + kRingTicks, 0);
            measure_ring(p, p + kRingEvents, 1);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void maybe_log()
{
    DWORD now = GetTickCount();
    if (!g_last_log) g_last_log = now;
    if (now - g_last_log < kEveryMs) return;
    double secs = (now - g_last_log) / 1000.0;
    g_last_log = now;
    if (g_ring_bytes[0] + g_ring_bytes[1] > 0 && g_players) {
        char top[700];
        int used = 0;
        top[0] = 0;
        for (int ch = 0; ch < 2; ch++) {
            int order[256];
            for (int i = 0; i < 256; i++) order[i] = i;
            std::sort(order, order + 256, [&](int a, int b) { return g_type[ch][a].bytes > g_type[ch][b].bytes; });
            used += snprintf(top + used, sizeof top - used, " | channel %d:", ch);
            for (int k = 0; k < 6 && g_type[ch][order[k]].bytes; k++) {
                const TypeSum& t = g_type[ch][order[k]];
                const char* name = order[k] < 0xc4 && kEventNames[order[k]] ? kEventNames[order[k]] : "?";
                used += snprintf(top + used, sizeof top - used, " 0x%x %s %.1f KB/s (%.0f/s, %.0f B each)", order[k], name,
                                 t.bytes / 1024.0 / secs, t.count / secs, double(t.bytes) / double(t.count));
                if (used >= int(sizeof top) - 1) break;
            }
        }
        swlog("tick bytes 5s: %u player(s); channel 0 %.1f KB/s (%.0f B per tick per player), channel 1 %.1f KB/s; %llu "
              "messages over the scratch, %llu unreadable%s",
              g_players, g_ring_bytes[0] / 1024.0 / secs, g_ticks ? double(g_ring_bytes[0]) / double(g_ticks) / g_players : 0.0,
              g_ring_bytes[1] / 1024.0 / secs, static_cast<unsigned long long>(g_truncated),
              static_cast<unsigned long long>(g_failed), top);
        if (g_bodies)
            swlog("tick bytes 5s: corrections %.0f bodies/s (%.0f%% with full positions), %.0f%% the same pose as the last one sent "
                  "to that player (%.0f%% byte-exact)",
                  g_bodies / secs, 100.0 * g_kind1 / g_bodies, 100.0 * g_same / g_bodies, 100.0 * g_exact / g_bodies);
    }
    memset(g_type, 0, sizeof g_type);
    g_ring_bytes[0] = g_ring_bytes[1] = 0;
    g_ticks = g_truncated = g_failed = g_bodies = g_kind1 = g_same = g_exact = 0;
    g_players = 0;
    for (auto it = g_last.begin(); it != g_last.end();) // bodies / players not sent to for a while
        it = it->second.seen + 5000 < g_tick_no ? g_last.erase(it) : ++it;
}

void __fastcall send_detour(void* session)
{
    if (!g_scratch) g_scratch = static_cast<uint8_t*>(VirtualAlloc(nullptr, kScratchBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    uint32_t players = 0;
    if (!g_tick_no) g_tick_no = 1;
    if (g_scratch && session && measure_safe(static_cast<const uint8_t*>(session), players)) {
        g_players = std::max(g_players, players);
        g_ticks++;
        g_tick_no++;
    }
    g_orig(session);
    maybe_log();
}

bool install()
{
    if (!GetPrivateProfileIntW(L"measure", L"tick_bytes", 1, settings_ini())) {
        swlog("tick bytes: off in settings.ini");
        return true;
    }
    if (!hook_game(kHostSend, reinterpret_cast<void*>(&send_detour), reinterpret_cast<void**>(&g_orig))) return false;
    swlog("tick bytes: on (host: what each player is sent per tick, by message type; \"tick bytes 5s\" lines)");
    return true;
}

HookRegistrar reg{{"tick bytes", /*needs_known_build=*/true, &install}};

} // namespace
