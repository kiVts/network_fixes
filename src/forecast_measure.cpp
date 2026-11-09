
#include "hooks.h"
#include "log.h"
#include "game.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace {

constexpr uintptr_t kApply = 0x14085aeb0, kSetTarget = 0x1402dbc80;
constexpr uintptr_t kApplySize = 35625; // functions.tsv
constexpr uint32_t kPlayerTick = 0x33c, kBodyPhys = 0x718, kBodyBack = 0x260;
constexpr uint32_t kPhysXform = 0x18, kPhysVel = 0x98, kPhysOwner = 0xc0;
constexpr double kTickS = 1.0 / 60.0;
constexpr double kMovingMs = 0.5, kFarM = 50.0;
constexpr size_t kMaxSamples = 100000;

using U = uint64_t;
using Apply_t = void(__fastcall*)(U, U, U, U, U, U);
using SetTarget_t = void(__fastcall*)(uint8_t* body, uint32_t reach, const double* xform);
Apply_t g_orig_apply = nullptr;
SetTarget_t g_orig_set = nullptr;
uintptr_t g_apply_lo = 0, g_apply_hi = 0;

// --- update thread only ------------------------------------------------------------------
const uint8_t* g_player = nullptr;
LARGE_INTEGER g_freq{};
LONGLONG g_win_start = 0;
std::unordered_map<uintptr_t, uint32_t> g_last_reach; // body -> reach tick of its previous correction

// stats for the 10 s log line
std::vector<double> g_err_moving[3], g_ang_moving, g_err_rest, g_ang_rest;
std::vector<uint32_t> g_interval;
std::unordered_set<uintptr_t> g_bodies;
uint64_t g_count = 0, g_far = 0, g_no_phys = 0, g_bad = 0, g_lead4 = 0, g_lead5 = 0, g_lead_other = 0;
uint64_t g_layout_ok = 0, g_layout_bad = 0; // since start, not per window
bool g_off = false;

struct Local {
    double xform[16];
    float vel[3];
};

// The body's own transform and velocity. 0 = read, 1 = no physics object, 2 = layout check failed / fault.
int read_local(const uint8_t* body, Local* out)
{
    __try {
        const uint8_t* phys = *reinterpret_cast<const uint8_t* const*>(body + kBodyPhys);
        if (!phys) return 1;
        if (*reinterpret_cast<const uint8_t* const*>(phys + kPhysOwner) != body + kBodyBack) return 2;
        memcpy(out->xform, phys + kPhysXform, sizeof(out->xform));
        memcpy(out->vel, phys + kPhysVel, sizeof(out->vel));
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

struct MeanP95 {
    double mean = 0, p95 = 0;
};

MeanP95 summarize(std::vector<double>& v)
{
    MeanP95 r;
    if (v.empty()) return r;
    double sum = 0;
    for (double x : v) sum += x;
    r.mean = sum / double(v.size());
    size_t i = (v.size() * 95) / 100;
    std::nth_element(v.begin(), v.begin() + i, v.end());
    r.p95 = v[i];
    return r;
}

void push(std::vector<double>& v, double x)
{
    if (v.size() < kMaxSamples) v.push_back(x);
}

void log_window(LONGLONG now)
{
    double span_s = double(now - g_win_start) / double(g_freq.QuadPart);
    if (span_s < 10) return;
    if (g_count > 0) {
        uint32_t interval = 0;
        if (!g_interval.empty()) {
            std::nth_element(g_interval.begin(), g_interval.begin() + g_interval.size() / 2, g_interval.end());
            interval = g_interval[g_interval.size() / 2];
        }
        MeanP95 m0 = summarize(g_err_moving[0]), m1 = summarize(g_err_moving[1]), m2 = summarize(g_err_moving[2]);
        MeanP95 ma = summarize(g_ang_moving), r = summarize(g_err_rest), ra = summarize(g_ang_rest);
        swlog("forecast measure %.0fs: %llu corrections (%.0f/s) on %zu bodies, interval median %u ticks, reach-tick lead "
              "4:%llu 5:%llu other:%llu | moving %zu: error cm mean/p95 now %.1f/%.1f  +1 tick %.1f/%.1f  +2 ticks %.1f/%.1f, "
              "angle %.2f/%.2f deg | resting %zu: %.1f/%.1f cm, %.2f/%.2f deg | far (>%.0f m) %llu, no physics object %llu, "
              "layout check failed %llu",
              span_s, static_cast<unsigned long long>(g_count), double(g_count) / span_s, g_bodies.size(), interval,
              static_cast<unsigned long long>(g_lead4), static_cast<unsigned long long>(g_lead5),
              static_cast<unsigned long long>(g_lead_other), g_err_moving[0].size(), m0.mean, m0.p95, m1.mean, m1.p95,
              m2.mean, m2.p95, ma.mean, ma.p95, g_err_rest.size(), r.mean, r.p95, ra.mean, ra.p95, kFarM,
              static_cast<unsigned long long>(g_far), static_cast<unsigned long long>(g_no_phys),
              static_cast<unsigned long long>(g_bad));
    }
    g_win_start = now;
    for (auto& v : g_err_moving) v.clear();
    g_ang_moving.clear();
    g_err_rest.clear();
    g_ang_rest.clear();
    g_interval.clear();
    g_bodies.clear();
    g_count = g_far = g_no_phys = g_bad = g_lead4 = g_lead5 = g_lead_other = 0;
}

void measure(uint8_t* body, uint32_t reach, const double* x)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_win_start) g_win_start = q.QuadPart;
    log_window(q.QuadPart);

    for (int i = 0; i < 16; i++) {
        if (!std::isfinite(x[i])) return;
    }
    g_count++;
    g_bodies.insert(reinterpret_cast<uintptr_t>(body));

    int lead = static_cast<int>(reach - *reinterpret_cast<const uint32_t*>(g_player + kPlayerTick));
    (lead == 4 ? g_lead4 : lead == 5 ? g_lead5 : g_lead_other)++;

    auto [it, fresh] = g_last_reach.try_emplace(reinterpret_cast<uintptr_t>(body), reach);
    if (!fresh) {
        uint32_t d = reach - it->second;
        if (d > 0 && d < 1000) g_interval.push_back(d);
        it->second = reach;
    }
    if (g_last_reach.size() > 4096) g_last_reach.clear(); // bodies come and go (pointer reuse)

    Local loc;
    int got = read_local(body, &loc);
    if (got == 1) {
        g_no_phys++;
        return;
    }
    if (got == 2) {
        g_bad++;
        g_layout_bad++;
        if (g_layout_ok == 0 && g_layout_bad >= 100) {
            g_off = true;
            swlog("forecast measure: switched off, body layout doesn't match this build (body +0x718 -> +0xc0 "
                  "is not body +0x260 in 100 of 100 corrections)");
        }
        return;
    }
    g_layout_ok++;
    const double* l = loc.xform;
    const float* v = loc.vel;

    double err[3];
    for (int k = 0; k < 3; k++) {
        double s = 0;
        for (int j = 0; j < 3; j++) {
            double d = x[12 + j] - (l[12 + j] + double(v[j]) * k * kTickS);
            s += d * d;
        }
        err[k] = std::sqrt(s);
    }
    if (err[0] > kFarM) {
        g_far++;
        return;
    }
    // Rotation angle of R_host^T R_local: trace = sum of the dot products of matching columns.
    double tr = 0;
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) tr += x[c * 4 + r] * l[c * 4 + r];
    }
    double ang = std::acos(std::clamp((tr - 1) / 2, -1.0, 1.0)) * 57.29577951308232;

    double speed = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    if (speed > kMovingMs) {
        for (int k = 0; k < 3; k++) push(g_err_moving[k], err[k] * 100);
        push(g_ang_moving, ang);
    } else {
        push(g_err_rest, err[0] * 100);
        push(g_ang_rest, ang);
    }
}

void __fastcall apply_detour(U a1, U a2, U player, U a4, U a5, U a6)
{
    const uint8_t* outer = g_player;
    g_player = reinterpret_cast<const uint8_t*>(player);
    g_orig_apply(a1, a2, player, a4, a5, a6);
    g_player = outer;
}

void __fastcall set_target_detour(uint8_t* body, uint32_t reach, const double* xform)
{
    uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!g_off && g_player && body && xform && ra >= g_apply_lo && ra < g_apply_hi) measure(body, reach, xform);
    g_orig_set(body, reach, xform);
}

bool install()
{
    if (GetPrivateProfileIntW(L"forecast", L"measure", 0, settings_ini()) == 0) {
        swlog("forecast measure: off ([forecast] measure=0)");
        return true; // off, not failed
    }
    QueryPerformanceFrequency(&g_freq);
    g_apply_lo = reinterpret_cast<uintptr_t>(game_ptr<const void*>(kApply));
    g_apply_hi = g_apply_lo + kApplySize;
    bool ok = hook_game(kApply, reinterpret_cast<void*>(&apply_detour), reinterpret_cast<void**>(&g_orig_apply)) &&
              hook_game(kSetTarget, reinterpret_cast<void*>(&set_target_detour), reinterpret_cast<void**>(&g_orig_set));
    swlog("forecast measure: %s (replica drift at every host vehicle correction, log only)", ok ? "on" : "FAILED");
    return ok;
}

HookRegistrar reg{{"forecast stage 0: replica drift measurement", /*needs_known_build=*/true, &install}};

} // namespace
