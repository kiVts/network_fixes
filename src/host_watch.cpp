
#include "game.h"
#include "hooks.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace {

constexpr uintptr_t kRequests = 0x140851630; // host: one client message's requests (game, ?, session, message, ?)
constexpr uint32_t kRing = 0x1c0, kRingCap = 0x1c8, kRingFirst = 0x1cc, kRingCount = 0x1d0, kHostTick = 0x33c;
constexpr uint32_t kId = 0x80, kLoopRate = 0xa4, kBacklog = 0xa8, kReportAge = 0xb4, kPing = 0xb8;
constexpr DWORD kEveryMs = 5000, kQuietEveryMs = 60000;
constexpr int kBehindTicks = 125; // 2 s
constexpr int kQuietTicks = 15;   // a modded client's playout buffer keeps up to ~9 queued on purpose

using U = uint64_t;
using Requests_t = void(__fastcall*)(U, U, U, U, U);
Requests_t g_orig = nullptr;

// Update thread only.
struct Seen {
    int backlog = 0, worst = 0;
    bool behind = false;
    DWORD first_behind = 0;
};
std::map<int, Seen> g_seen;
DWORD g_last_log = 0, g_last_quiet = 0, g_prev_at = 0;
int g_prev_tick = 0;

template <typename T> T at(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const T*>(p + off); }

void watch(const uint8_t* session)
{
    DWORD now = GetTickCount();
    if (now - g_last_log < kEveryMs) return;
    uint32_t count = at<uint32_t>(session, kRingCount), cap = at<uint32_t>(session, kRingCap);
    const uint8_t* const* ring = at<const uint8_t* const*>(session, kRing);
    if (!ring || !cap || count > 64) return;
    // Over the real time since the last look: requests arrive only when a client sends, so it can be well over 5 s.
    int tick = at<int>(session, kHostTick);
    double ticks_per_s = g_prev_tick && now > g_prev_at ? (tick - g_prev_tick) * 1000.0 / (now - g_prev_at) : 62.5;
    g_last_log = g_prev_at = now;
    g_prev_tick = tick;
    std::string line;
    bool anyone_behind = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* p = ring[(at<uint32_t>(session, kRingFirst) + i) % cap];
        if (!p) continue;
        int id = at<int16_t>(p, kId), backlog = at<int>(p, kBacklog), age = at<int>(p, kReportAge);
        int rate = at<int>(p, kLoopRate), ping = at<int>(p, kPing);
        if (age <= 0 && backlog == 0 && rate == 0) continue; // no report (the host's own record)
        Seen& s = g_seen[id];
        int growth = backlog - s.backlog;
        s.backlog = backlog;
        s.worst = std::max(s.worst, backlog);
        bool behind = backlog > kBehindTicks;
        if (behind && !s.behind) {
            s.first_behind = now;
            swlog("player %d fell behind: %d host ticks queued (%.1f s), its update loop %d/s, ping %d ms", id, backlog,
                  backlog / 62.5, rate, ping);
        } else if (!behind && s.behind) {
            swlog("player %d caught up after %.0f s (worst %d ticks)", id, (now - s.first_behind) / 1000.0, s.worst);
            s.worst = 0;
        }
        s.behind = behind;
        anyone_behind |= backlog > kQuietTicks;
        char b[200];
        snprintf(b, sizeof b, "  player %d: %d ticks behind (%.1f s, %+d in 5 s), loop %d/s, report %d ticks old, ping %d ms", id,
                 backlog, backlog / 62.5, growth, rate, age, ping);
        line += b;
    }
    if (line.empty()) return;
    if (!anyone_behind && now - g_last_quiet < kQuietEveryMs) return;
    g_last_quiet = now;
    swlog("players 5s: host %.1f ticks/s |%s", ticks_per_s, line.c_str());
}

void __fastcall requests_detour(U game, U a2, U session, U message, U a5)
{
    g_orig(game, a2, session, message, a5);
    if (session) watch(reinterpret_cast<const uint8_t*>(session));
}

bool install()
{
    bool ok = hook_game(kRequests, reinterpret_cast<void*>(&requests_detour), reinterpret_cast<void**>(&g_orig));
    swlog("host watch: %s (\"players 5s:\" lines: how far behind each player is, from the game's own net stats)",
          ok ? "on" : "FAILED");
    return ok;
}

HookRegistrar reg{{"host watch (player backlog)", /*needs_known_build=*/true, &install}};

} // namespace
