
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

#include "event_names.inc"

constexpr uintptr_t kApply = 0x14085aeb0;
constexpr uintptr_t kAuthTransform = 0x140230040;
constexpr uintptr_t kSeatAttach = 0x14022f930;
constexpr uintptr_t kSetInput = 0x14022f360;
constexpr uintptr_t kSetInputMove = 0x140232230;
constexpr uintptr_t kSetInputAxis = 0x1402322a0;
constexpr uintptr_t kAddObject = 0x140544a40;
constexpr uintptr_t kObjectLoad = 0x140534d30;
constexpr uintptr_t kSetTarget = 0x1402dbc80; // set_vehicle_body_motion_state target (body, reach tick, 4x4 doubles)
constexpr int kTypes = 0xc4;
constexpr double kJumpM = 3.0, kRevertNearM = 0.02, kRevertFarM = 0.5;
constexpr int kHistory = 16;

using U = uint64_t;
using Apply_t = void(__fastcall*)(U, U, U, U, U, U);
using Ser_t = void(__fastcall*)(uint8_t* self, uint8_t* stream);

Apply_t g_orig_apply = nullptr;
using SetTarget_t = void(__fastcall*)(uint8_t* body, uint32_t reach, const double* xform);
SetTarget_t g_orig_target = nullptr;
std::atomic<uint64_t> g_targets{0}, g_bad_targets{0};
std::atomic<int> g_target_logged{0};
Ser_t g_orig_auth = nullptr, g_orig_seat = nullptr, g_orig_input = nullptr, g_orig_move = nullptr, g_orig_axis = nullptr;

std::atomic<uint64_t> g_events[kTypes];
std::atomic<uint64_t> g_apply_calls{0}, g_events_total{0}, g_bad_rings{0};

struct Counter {
    std::atomic<uint64_t> sent{0}, received{0};
};
Counter g_seat, g_input, g_move, g_axis;

// --- character transforms --------------------------------------------------------------------------------------
struct Track {
    double hist[kHistory][3] = {};
    int n = 0;         // transforms seen
    int next = 0;      // ring position of the next one
    double last[3] = {};
    LONGLONG last_qpc = 0;
    double dist = 0, max_speed = 0;
    uint64_t count = 0, jumps = 0, reverts = 0, still = 0;
};
std::mutex g_tr_mu;
std::map<uint64_t, Track> g_tracks;
LARGE_INTEGER g_freq{};
int g_detail_left = 20;

double dist3(const double* a, const double* b)
{
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool read_transform(const uint8_t* self, double* pos, uint32_t& a, uint32_t& b)
{
    __try {
        memcpy(pos, self + 0x10, 3 * sizeof(double));
        a = *reinterpret_cast<const uint32_t*>(self + 0x40);
        b = *reinterpret_cast<const uint32_t*>(self + 0x44);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void on_transform(bool received, const uint8_t* self)
{
    double pos[3];
    uint32_t a, b;
    if (!read_transform(self, pos, a, b)) return;
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard lock(g_tr_mu);
    Track& t = g_tracks[(uint64_t(received) << 32) | a];
    t.count++;
    if (t.n > 0) {
        double d = dist3(pos, t.last);
        double dt = double(now.QuadPart - t.last_qpc) / double(g_freq.QuadPart);
        if (d < 0.001) t.still++;
        if (d > kJumpM) {
            t.jumps++;
            if (g_detail_left > 0) {
                g_detail_left--;
                swlog("audit: character transform %s (key %u/%u) jumped %.1f m in %.0f ms: (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)",
                      received ? "received" : "sent", a, b, d, dt * 1000, t.last[0], t.last[1], t.last[2], pos[0], pos[1], pos[2]);
            }
        } else {
            t.dist += d;
            if (dt > 0.001) t.max_speed = std::max(t.max_speed, d / dt);
        }
        if (d > kRevertFarM && t.n > 1) {
            int k = std::min(t.n - 1, kHistory - 1);
            for (int i = 2; i <= k + 1; i++) { // skip the previous one (i = 1)
                const double* old = t.hist[(t.next - i + kHistory) % kHistory];
                if (dist3(pos, old) < kRevertNearM) {
                    t.reverts++;
                    if (g_detail_left > 0) {
                        g_detail_left--;
                        swlog("audit: character transform %s (key %u/%u) REVERTED to the one %d before (%.2f %.2f %.2f), "
                              "%.2f m from the previous: an old state after a newer one",
                              received ? "received" : "sent", a, b, i, pos[0], pos[1], pos[2], d);
                    }
                    break;
                }
            }
        }
    }
    memcpy(t.hist[t.next], pos, sizeof(pos));
    t.next = (t.next + 1) % kHistory;
    memcpy(t.last, pos, sizeof(pos));
    t.last_qpc = now.QuadPart;
    t.n++;
}

bool reading(const uint8_t* stream)
{
    __try {
        return stream[0x14] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall auth_detour(uint8_t* self, uint8_t* stream)
{
    bool r = reading(stream);
    if (!r) on_transform(false, self); // before writing: the values being sent
    g_orig_auth(self, stream);
    if (r) on_transform(true, self); // after reading: the values received
}

void count(Counter& c, const uint8_t* stream) { (reading(stream) ? c.received : c.sent).fetch_add(1, std::memory_order_relaxed); }
void __fastcall seat_detour(uint8_t* self, uint8_t* stream) { count(g_seat, stream); g_orig_seat(self, stream); }
void __fastcall input_detour(uint8_t* self, uint8_t* stream) { count(g_input, stream); g_orig_input(self, stream); }
void __fastcall move_detour(uint8_t* self, uint8_t* stream) { count(g_move, stream); g_orig_move(self, stream); }
void __fastcall axis_detour(uint8_t* self, uint8_t* stream) { count(g_axis, stream); g_orig_axis(self, stream); }

// --- objects the host adds (event 0x4d) --------------------------------------------------------------------------
// Logs the first 80 add_object events per session with every double in the first 0x200 bytes of their data that
// looks like a world coordinate (|v| between 1 and 1e20, or not finite), to see whether an object arrives with its
// real position or with an impossible one.
using AddObject_t = U(__fastcall*)(U list, U id, U data, U a4);
AddObject_t g_orig_add = nullptr;
std::atomic<int> g_add_logged{0};
std::atomic<uint64_t> g_adds{0};

int scan_doubles(const uint8_t* data, char* out, size_t size)
{
    int n = 0;
    size_t used = 0;
    out[0] = 0;
    __try {
        for (size_t off = 0; off + 8 <= 0x200 && n < 24; off += 8) {
            double v = *reinterpret_cast<const double*>(data + off);
            if (!(std::isfinite(v) ? (std::fabs(v) >= 1.0 && std::fabs(v) <= 1e20) : true)) continue;
            int w = snprintf(out + used, size - used, " +0x%zx=%.6g", off, v);
            if (w < 0 || size_t(w) >= size - used) break;
            used += size_t(w);
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

U __fastcall add_detour(U list, U id, U data, U a4)
{
    g_adds.fetch_add(1, std::memory_order_relaxed);
    if (data && g_add_logged.fetch_add(1) < 80) {
        char buf[1200];
        scan_doubles(reinterpret_cast<const uint8_t*>(data), buf, sizeof buf);
        swlog("audit: add_object id %u, coordinates in its data:%s", unsigned(id), buf[0] ? buf : " (none)");
    }
    return g_orig_add(list, id, data, a4);
}

using ObjectLoad_t = U(__fastcall*)(U obj, U a2, U stream, U node);
ObjectLoad_t g_orig_load = nullptr;
std::atomic<int> g_load_logged{0}, g_load_bad_logged{0};
std::atomic<uint64_t> g_loads{0}, g_load_bad{0};

bool read_matrix(const uint8_t* m, double* out, int& mode, const uint8_t* stream)
{
    __try {
        memcpy(out, m, 16 * sizeof(double));
        mode = stream ? *reinterpret_cast<const int*>(stream + 0x20) : -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

U __fastcall load_detour(U obj, U a2, U stream, U node)
{
    U r = g_orig_load(obj, a2, stream, node);
    double m[16];
    int mode = -1;
    if (!obj || !read_matrix(reinterpret_cast<const uint8_t*>(obj) + 0x10, m, mode, reinterpret_cast<const uint8_t*>(stream))) return r;
    g_loads.fetch_add(1, std::memory_order_relaxed);
    bool bad = false;
    for (int i = 0; i < 16; i++) bad |= !std::isfinite(m[i]);
    for (int i = 12; i < 15; i++) bad |= std::fabs(m[i]) > 1e6;
    if (bad) g_load_bad.fetch_add(1, std::memory_order_relaxed);
    if (g_load_logged.fetch_add(1) < 60 || (bad && g_load_bad_logged.fetch_add(1) < 40)) {
        if (bad)
            swlog("audit: object %p loaded (stream mode %d) with an IMPOSSIBLE transform: [%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | "
                  "%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g]",
                  reinterpret_cast<void*>(obj), mode, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
                  m[13], m[14], m[15]);
        else
            swlog("audit: object %p loaded (stream mode %d) at (%.6g %.6g %.6g)", reinterpret_cast<void*>(obj), mode, m[12], m[13], m[14]);
    }
    return r;
}

// --- host corrections ------------------------------------------------------------------------------------------
// Every 0x81 correction the client applies: a target transform that isn't finite or is more than 1e6 m out means
// the host itself sent the impossible position (or it was decoded wrong).
bool bad_xform(const double* x)
{
    __try {
        for (int i = 0; i < 16; i++)
            if (!std::isfinite(x[i])) return true;
        for (int i = 12; i < 15; i++)
            if (std::fabs(x[i]) > 1e6) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall target_detour(uint8_t* body, uint32_t reach, const double* xform)
{
    g_targets.fetch_add(1, std::memory_order_relaxed);
    if (xform && bad_xform(xform)) {
        g_bad_targets.fetch_add(1, std::memory_order_relaxed);
        if (g_target_logged.fetch_add(1) < 20)
            swlog("audit: host correction (0x81) with an impossible target for body %p, reach tick %u: position (%.6g %.6g %.6g)",
                  body, reach, xform[12], xform[13], xform[14]);
    }
    g_orig_target(body, reach, xform);
}

// --- events ----------------------------------------------------------------------------------------------------
void count_ring(const uint8_t* ring)
{
    __try {
        auto data = *reinterpret_cast<uint8_t* const* const*>(ring);
        uint32_t cap = *reinterpret_cast<const uint32_t*>(ring + 8);
        uint32_t first = *reinterpret_cast<const uint32_t*>(ring + 0xc);
        uint32_t n = *reinterpret_cast<const uint32_t*>(ring + 0x10);
        if (!data || !cap || n > cap) {
            if (n) g_bad_rings.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* ev = data[(first + i) % cap];
            if (!ev) continue;
            uint32_t t = *reinterpret_cast<const uint32_t*>(ev + 8);
            if (t < kTypes) g_events[t].fetch_add(1, std::memory_order_relaxed);
        }
        g_events_total.fetch_add(n, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_bad_rings.fetch_add(1, std::memory_order_relaxed);
    }
}

void __fastcall apply_detour(U a1, U a2, U a3, U a4, U a5, U a6)
{
    g_apply_calls.fetch_add(1, std::memory_order_relaxed);
    if (a5) count_ring(reinterpret_cast<const uint8_t*>(a5));
    g_orig_apply(a1, a2, a3, a4, a5, a6);
}

bool interesting(int t)
{
    const char* n = kEventNames[t];
    if (!n) return false;
    return strstr(n, "character") || strstr(n, "seat") || strstr(n, "input") || strstr(n, "peer") || strstr(n, "ladder") ||
           strstr(n, "teleport") || strstr(n, "respawn") || strstr(n, "motion_state");
}

void log_loop()
{
    uint64_t last[kTypes] = {};
    uint64_t last_calls = 0, last_total = 0;
    for (;;) {
        Sleep(10000);
        uint64_t cur[kTypes], diff[kTypes];
        for (int t = 0; t < kTypes; t++) {
            cur[t] = g_events[t].load(std::memory_order_relaxed);
            diff[t] = cur[t] - last[t];
            last[t] = cur[t];
        }
        uint64_t calls = g_apply_calls.load(), total = g_events_total.load();
        uint64_t dcalls = calls - last_calls, dtotal = total - last_total;
        last_calls = calls;
        last_total = total;
        if (dcalls) {
            std::vector<int> order;
            for (int t = 0; t < kTypes; t++)
                if (diff[t]) order.push_back(t);
            std::sort(order.begin(), order.end(), [&](int x, int y) { return diff[x] > diff[y]; });
            std::string top, watch;
            char buf[160];
            for (size_t i = 0; i < order.size() && i < 12; i++) {
                int t = order[i];
                snprintf(buf, sizeof(buf), "  0x%x %s %.1f/s", t, kEventNames[t] ? kEventNames[t] : "?", diff[t] / 10.0);
                top += buf;
            }
            for (int t : order) {
                if (!interesting(t)) continue;
                snprintf(buf, sizeof(buf), "  0x%x %s %llu", t, kEventNames[t], static_cast<unsigned long long>(diff[t]));
                watch += buf;
            }
            swlog("audit 10s events: %llu applied in %llu tick applies (%.1f per apply), bad rings %llu | busiest:%s",
                  static_cast<unsigned long long>(dtotal), static_cast<unsigned long long>(dcalls),
                  double(dtotal) / double(dcalls), static_cast<unsigned long long>(g_bad_rings.load()), top.c_str());
            if (!watch.empty()) swlog("audit 10s events (character / seat / input / motion):%s", watch.c_str());
        }
        {
            std::lock_guard lock(g_tr_mu);
            for (auto& [key, t] : g_tracks) {
                if (!t.count) continue;
                swlog("audit 10s character transforms %s key %u: %llu (%.1f/s), moved %.1f m, max speed %.1f m/s, unchanged "
                      "%llu, jumps > %.0f m %llu, reverted to an older one %llu; last position (%.6g %.6g %.6g)",
                      (key >> 32) ? "received" : "sent", unsigned(key & 0xffffffff), static_cast<unsigned long long>(t.count),
                      t.count / 10.0, t.dist, t.max_speed, static_cast<unsigned long long>(t.still), kJumpM,
                      static_cast<unsigned long long>(t.jumps), static_cast<unsigned long long>(t.reverts), t.last[0], t.last[1],
                      t.last[2]);
                t.count = t.jumps = t.reverts = t.still = 0;
                t.dist = t.max_speed = 0;
            }
            g_detail_left = 20;
        }
        auto line = [](const char* what, Counter& c) {
            uint64_t s = c.sent.exchange(0), r = c.received.exchange(0);
            if (s || r) swlog("audit 10s %s: sent %llu, received %llu", what, static_cast<unsigned long long>(s), static_cast<unsigned long long>(r));
        };
        {
            uint64_t n = g_targets.exchange(0), bad = g_bad_targets.exchange(0);
            if (n) swlog("audit 10s host corrections (0x81): %llu applied, %llu with an impossible target", static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(bad));
            g_target_logged.store(std::min(g_target_logged.load(), 15));
        }
        line("seat requests", g_seat);
        line("set_input requests", g_input);
        line("set_input_move requests", g_move);
        line("set_input_axis requests", g_axis);
    }
}

bool install()
{
    if (!GetPrivateProfileIntW(L"audit", L"enabled", 1, settings_ini())) {
        swlog("audit: off in settings.ini");
        return true;
    }
    QueryPerformanceFrequency(&g_freq);
    bool ok = hook_game(kApply, reinterpret_cast<void*>(&apply_detour), reinterpret_cast<void**>(&g_orig_apply));
    ok &= hook_game(kAuthTransform, reinterpret_cast<void*>(&auth_detour), reinterpret_cast<void**>(&g_orig_auth));
    ok &= hook_game(kSeatAttach, reinterpret_cast<void*>(&seat_detour), reinterpret_cast<void**>(&g_orig_seat));
    ok &= hook_game(kSetInput, reinterpret_cast<void*>(&input_detour), reinterpret_cast<void**>(&g_orig_input));
    ok &= hook_game(kSetInputMove, reinterpret_cast<void*>(&move_detour), reinterpret_cast<void**>(&g_orig_move));
    ok &= hook_game(kSetInputAxis, reinterpret_cast<void*>(&axis_detour), reinterpret_cast<void**>(&g_orig_axis));
    ok &= hook_game(kAddObject, reinterpret_cast<void*>(&add_detour), reinterpret_cast<void**>(&g_orig_add));
    ok &= hook_game(kObjectLoad, reinterpret_cast<void*>(&load_detour), reinterpret_cast<void**>(&g_orig_load));
    ok &= hook_game(kSetTarget, reinterpret_cast<void*>(&target_detour), reinterpret_cast<void**>(&g_orig_target));
    if (!ok) return false;
    std::thread(log_loop).detach();
    swlog("audit: on (tick events by type, character transforms sent / received, seat and input requests; \"audit 10s\" lines)");
    return true;
}

HookRegistrar reg{{"network audit", /*needs_known_build=*/true, &install}};

} // namespace
