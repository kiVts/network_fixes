
#include "game.h"
#include "hooks.h"
#include "log.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

constexpr uintptr_t kCharSetTransform = 0x1404daee0;
constexpr uintptr_t kFollowReturn = 0x14075fb1a;
constexpr uintptr_t kSetLinearVelocity = 0x1401a72d0; // (scene, scene object, float[3])
constexpr int kBodies[] = {0x478, 0x480, 0x488};
constexpr double kTickRate = 62.5; // 16 ms ticks
constexpr double kTeleportM = 2.0; // per tick = 120 m/s
constexpr double kMaxSpeed = 15.0;
constexpr int kSpan = 5;          // ticks the velocity is measured over (the client's transform interval)
constexpr DWORD kForgetMs = 10000;
constexpr uintptr_t kIsHost = 0x140ceb6ff; // byte: 1 host / single player, 0 client

using SetTransform_t = void(__fastcall*)(uint8_t* character, uint8_t* scene, const double* pos);
using SetVelocity_t = void(__fastcall*)(uint8_t* scene, uint8_t* object, const float* v);

SetTransform_t g_orig = nullptr;
SetVelocity_t g_set_velocity = nullptr;
uintptr_t g_follow_return = 0;
const volatile uint8_t* g_is_host = nullptr;
std::atomic<uint64_t> g_applied{0}, g_teleports{0}, g_fast{0};
bool g_push_on = false, g_smooth_on = false;
constexpr int kSamples = 8;
constexpr double kSnapM = 5.0, kMaxDelayMs = 250;
LARGE_INTEGER g_qpf{};
// smoothing stats (60 s, under g_mu)
uint64_t g_sm_calls = 0, g_sm_poses = 0, g_sm_held = 0, g_sm_snaps = 0, g_sm_pass = 0;
double g_sm_iv_sum = 0, g_sm_iv_max = 0, g_sm_delay_sum = 0;

double now_ms()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return double(q.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
}

// Positions of each remote character over the last kSpan ticks (characters are updated from simulate; the lock is
// for safety, a few calls per tick).
struct History {
    double pos[kSpan + 1][3];
    int n = 0, next = 0;
    DWORD seen = 0;
    // smoothing: the targets the game gave, with their arrival times (oldest first)
    double st[kSamples] = {}, sp[kSamples][3] = {};
    int sn = 0, fresh_run = 0;
    double iv = 0;     // recent average interval between pose times, ms
    double arrive = 0; // when the newest pose arrived
    double rt = 0;     // the moment shown last tick
};
std::mutex g_mu;
std::unordered_map<const uint8_t*, History> g_hist;
DWORD g_last_prune = 0;
// where each remote character was put last (host), for object_push.cpp's push watch (under g_mu)
struct Placed {
    double pos[3];
    DWORD at;
};
std::unordered_map<const uint8_t*, Placed> g_placed;

double dist(const double* a, const double* b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// The position to show for this tick (under g_mu). Poses are given times: arrival, but spread out when they come in a
// clump (at least 3/4 of the usual interval after the one before, at most 150 ms after arriving), and the character is
// shown at "now - delay" between the two poses around that moment. That moment never goes back; when it passes the
// newest pose (a stall) the newest pose's time moves along with it, so the next pose continues from where it stands.
void smooth(History& h, const double* target, double t, double out[3])
{
    g_sm_calls++;
    memcpy(out, target, 3 * sizeof(double));
    bool fresh = h.sn == 0 || memcmp(h.sp[h.sn - 1], target, 3 * sizeof(double)) != 0;
    h.fresh_run = fresh ? h.fresh_run + 1 : 0;
    if (fresh) {
        double jump = h.sn > 0 ? dist(h.sp[h.sn - 1], target) : 0;
        if (h.sn > 0 && (!std::isfinite(jump) || jump > kSnapM)) {
            h.sn = 0; // teleport: start again from here
            h.iv = 0;
            h.rt = 0;
            g_sm_snaps++;
        }
        double st = t;
        if (h.sn > 0) {
            g_sm_iv_sum += t - h.arrive;
            g_sm_iv_max = std::max(g_sm_iv_max, t - h.arrive);
            g_sm_poses++;
            if (h.iv > 0) st = std::min(t + 150.0, std::max(t, h.st[h.sn - 1] + 0.75 * h.iv));
            double iv = st - h.st[h.sn - 1];
            h.iv = h.iv > 0 ? h.iv * 0.8 + iv * 0.2 : iv;
        }
        h.arrive = t;
        if (h.sn == kSamples) {
            memmove(h.st, h.st + 1, (kSamples - 1) * sizeof(double));
            memmove(h.sp, h.sp + 1, (kSamples - 1) * sizeof(h.sp[0]));
            h.sn--;
        }
        h.st[h.sn] = st;
        memcpy(h.sp[h.sn], target, sizeof(h.sp[0]));
        h.sn++;
    }
    // nothing to blend yet, or the target changes every tick already (riding a vehicle): as the game has it
    if (h.sn < 2 || h.iv <= 0 || h.fresh_run >= 8) {
        g_sm_pass++;
        h.rt = t;
        return;
    }
    double delay = std::min(kMaxDelayMs, h.iv * 1.25);
    g_sm_delay_sum += delay;
    double rt = std::max(h.rt, t - delay);
    h.rt = rt;
    if (rt <= h.st[0]) {
        memcpy(out, h.sp[0], 3 * sizeof(double));
        return;
    }
    for (int i = 1; i < h.sn; i++) {
        if (rt > h.st[i]) continue;
        double f = (rt - h.st[i - 1]) / std::max(1e-6, h.st[i] - h.st[i - 1]);
        for (int k = 0; k < 3; k++) out[k] = h.sp[i - 1][k] + (h.sp[i][k] - h.sp[i - 1][k]) * f;
        return;
    }
    memcpy(out, h.sp[h.sn - 1], 3 * sizeof(double)); // ran out of poses: hold the newest, and keep its time current
    h.st[h.sn - 1] = rt;
    g_sm_held++;
}

void __fastcall set_transform_detour(uint8_t* character, uint8_t* scene, const double* pos)
{
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_follow_return || !character || !scene || !pos || !*g_is_host) {
        g_orig(character, scene, pos);
        return;
    }
    alignas(16) double shown[4] = {}; // like the game's own pose (set-transform reads it with movups, but match it anyway)
    memcpy(shown, pos, sizeof shown);
    if (g_smooth_on) {
        std::lock_guard lock(g_mu);
        History& sh = g_hist[character];
        if (!g_push_on) sh.seen = GetTickCount(); // with pushing on, its own pause check needs the old value
        smooth(sh, pos, now_ms(), shown);
    }
    pos = shown;
    {
        std::lock_guard lock(g_mu);
        Placed& pl = g_placed[character];
        memcpy(pl.pos, pos, sizeof pl.pos);
        pl.at = GetTickCount();
    }
    const double* before = reinterpret_cast<const double*>(character + 0x68);
    double step = std::sqrt((pos[0] - before[0]) * (pos[0] - before[0]) + (pos[1] - before[1]) * (pos[1] - before[1]) +
                            (pos[2] - before[2]) * (pos[2] - before[2]));
    g_orig(character, scene, pos);
    if (!g_push_on) return;
    double v[3];
    {
        std::lock_guard lock(g_mu);
        DWORD now = GetTickCount();
        if (now - g_last_prune > kForgetMs) { // characters that left
            for (auto it = g_hist.begin(); it != g_hist.end();) it = now - it->second.seen > kForgetMs ? g_hist.erase(it) : ++it;
            g_last_prune = now;
        }
        History& h = g_hist[character];
        if (!std::isfinite(step) || step > kTeleportM || now - h.seen > 500) h.n = 0; // teleport, or not updated lately
        h.seen = now;
        memcpy(h.pos[h.next], pos, sizeof(h.pos[0]));
        h.next = (h.next + 1) % (kSpan + 1);
        h.n = std::min(h.n + 1, kSpan + 1);
        if (h.n < 2) {
            g_teleports.fetch_add(step > kTeleportM, std::memory_order_relaxed);
            return; // the game's zero velocity
        }
        const double* oldest = h.pos[(h.next + (kSpan + 1) - h.n) % (kSpan + 1)];
        double ticks = h.n - 1;
        for (int i = 0; i < 3; i++) v[i] = (pos[i] - oldest[i]) * kTickRate / ticks;
    }
    double speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(speed) || speed > kMaxSpeed) {
        g_fast.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float fv[3] = {float(v[0]), float(v[1]), float(v[2])};
    for (int off : kBodies) {
        uint8_t* body = *reinterpret_cast<uint8_t**>(character + off);
        if (body) g_set_velocity(scene, body, fv);
    }
    g_applied.fetch_add(1, std::memory_order_relaxed);
}

void log_loop()
{
    for (;;) {
        Sleep(60000);
        uint64_t n = g_applied.exchange(0), t = g_teleports.exchange(0), f = g_fast.exchange(0);
        if (n || t || f)
            swlog("remote push 60s: %llu remote character moves with velocity, %llu teleports, %llu faster than %.0f m/s left at 0",
                  static_cast<unsigned long long>(n), static_cast<unsigned long long>(t), static_cast<unsigned long long>(f), kMaxSpeed);
        std::lock_guard lock(g_mu);
        if (g_sm_calls) {
            uint64_t blended = g_sm_calls - g_sm_pass;
            swlog("remote smooth 60s: %llu ticks of remote characters, %llu new poses (every %.0f ms on average, longest %.0f ms), "
                  "%llu ticks shown %.0f ms in the past between two poses, %llu held on the newest (ran out), %llu snaps over %.0f m, "
                  "%llu passed through",
                  static_cast<unsigned long long>(g_sm_calls), static_cast<unsigned long long>(g_sm_poses),
                  g_sm_poses ? g_sm_iv_sum / g_sm_poses : 0.0, g_sm_iv_max, static_cast<unsigned long long>(blended),
                  blended ? g_sm_delay_sum / blended : 0.0, static_cast<unsigned long long>(g_sm_held),
                  static_cast<unsigned long long>(g_sm_snaps), kSnapM, static_cast<unsigned long long>(g_sm_pass));
        }
        g_sm_calls = g_sm_poses = g_sm_held = g_sm_snaps = g_sm_pass = 0;
        g_sm_iv_sum = g_sm_iv_max = g_sm_delay_sum = 0;
    }
}

bool install()
{
    g_push_on = GetPrivateProfileIntW(L"push", L"remote_push", 1, settings_ini()) != 0;
    g_smooth_on = GetPrivateProfileIntW(L"push", L"smooth", 1, settings_ini()) != 0;
    QueryPerformanceFrequency(&g_qpf);
    if (!g_push_on && !g_smooth_on) {
        swlog("remote push: off in settings.ini (pushing and smoothing)");
        return true;
    }
    g_set_velocity = game_ptr<SetVelocity_t>(kSetLinearVelocity);
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    g_follow_return = reinterpret_cast<uintptr_t>(game_ptr<void*>(kFollowReturn));
    if (!hook_game(kCharSetTransform, reinterpret_cast<void*>(&set_transform_detour), reinterpret_cast<void**>(&g_orig)))
        return false;
    std::thread(log_loop).detach();
    if (g_push_on)
        swlog("remote push: on when hosting (other players' characters keep their walking velocity, measured over %d ticks, max %.0f m/s)", kSpan, kMaxSpeed);
    swlog("remote smooth: %s", g_smooth_on ? "on when hosting (other players' characters move between their received poses, shown about one "
                                            "update interval in the past; \"remote smooth 60s\" lines)"
                                          : "off in settings.ini");
    return true;
}

HookRegistrar reg{{"remote push", /*needs_known_build=*/true, &install}};

} // namespace

// physics_fixes puppet_cull.cpp (host, physics thread, at each step): the remote players' characters the game put
// somewhere in the last 500 ms; returns how many (<= max).
extern "C" __declspec(dllexport) int __cdecl swml_remote_characters(const void** out, int max)
{
    std::lock_guard lock(g_mu);
    DWORD now = GetTickCount();
    int n = 0;
    for (auto& [ch, pl] : g_placed)
        if (now - pl.at <= 500 && n < max) out[n++] = ch;
    return n;
}

int remote_character_positions(double out[][3], int max)
{
    std::lock_guard lock(g_mu);
    DWORD now = GetTickCount();
    int n = 0;
    for (auto it = g_placed.begin(); it != g_placed.end();) {
        if (now - it->second.at > 2000) { // left, or not a puppet any more
            it = g_placed.erase(it);
            continue;
        }
        if (now - it->second.at <= 500 && n < max) memcpy(out[n++], it->second.pos, sizeof out[0]);
        ++it;
    }
    return n;
}
