
#include "game.h"
#include "hooks.h"
#include "vehicle_push.h"
#include "log.h"
#include "net_shared.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace {

constexpr uintptr_t kPreStep = 0x1405ec4b0;
constexpr uintptr_t kPostTransform = 0x1401a6c70; // direct transform posts (scenery props)
constexpr uintptr_t kPostKinematic = 0x1401a6e80;
constexpr uintptr_t kSetCollisionMasks = 0x1401a81e0; // queued remove/reinsert, (scene, so, group, mask)
constexpr uintptr_t kSetCollisionFlags = 0x1401a7220; // (scene, scene object, int flags)
constexpr uintptr_t kSetLinearVelocity = 0x1401a72d0; // (scene, scene object, const float[3])
constexpr uintptr_t kSetAngularVelocity = 0x1401a73a0; // (scene, scene object, const float[3]); wakes the body
constexpr uintptr_t kIsHost = 0x140ceb6ff;

size_t world_scene() { return per_build<size_t>(0x64c8, 0x64d0); }
size_t world_ring() { return per_build<size_t>(0x6390, 0x6398); }
size_t world_vehicles() { return per_build<size_t>(0x4ca8, 0x4cb0); }
constexpr size_t kEntryHandle = 0x90, kEntryId = 0x98;
struct KindDef {
    size_t slot, scene_object;
};
constexpr KindDef kKinds[3] = {{0x18, 0x268}, {0x8, 0x230}, {0x10, 0x220}};
constexpr size_t kKind0Type = 0x260;
constexpr uint32_t kKind0TypeValue = 0xa3;
constexpr size_t kVehicleId = 4, kVehicleBodies = 8, kVehicleType = 0x10, kBodyRecordSo = 0x718;
constexpr uint32_t kVehicleTypeValue = 5;
constexpr size_t kSoBody = 0xb8, kSoFlags = 0x14;
constexpr size_t kBodyBasis = 0x10, kBodyOrigin = 0x70, kBodyProxy = 0x180, kBodyFlags = 0x1a0, kBodyActivation = 0x1b0,
                 kBodyType = 0x1f0, kBodyLinVel = 0x390, kBodyAngVel = 0x3b0, kBodyInvMass = 0x3d0,
                 kBodyConstraintCount = 0x4f4, kBodyConstraints = 0x500, kConstraintA = 0x28, kConstraintB = 0x30,
                 kBodyUser = 0x1f8;
constexpr size_t kProxyMin = 0x20, kProxyMax = 0x40, kProxyGroup = 8, kProxyMask = 0xc;
constexpr int kFlagKinematic = 2, kFlagDriven = 0x800;
constexpr size_t kCharBodies[3] = {0x478, 0x480, 0x488};
constexpr size_t kCharSeat = 0x2100;

constexpr uint8_t kJointPart = 0x20;
constexpr uint8_t kVehiclePart = 0x10; // wire kind of vehicle body i: 0x10 + i
constexpr double kTouchMargin = 0.05, kMaxExtent = 4.0, kMaxVehicleExtent = 8.0;
constexpr int kMaxLooseOwned = 5, kMaxVehiclesOwned = 1, kMaxVehicleBodies = 10;
constexpr double kClaimAboveSeaM = 1.0, kReleaseAboveSeaM = 0.3;
constexpr DWORD kReleaseAfterMs = 1000, kForceReleaseMs = 5000, kReleasingMs = 600, kDenyBlockMs = 3000, kHostCapFreshMs = 3000;
constexpr double kRestSpeed = 0.15, kRestSpin = 0.3, kStillM = 0.01;
constexpr double kStandM = 0.2; // feet at most this far under the unit's top, above its footprint: standing on it
constexpr DWORD kStillGapMs = 100;
// host
constexpr DWORD kClaimStaleMs = 400;
constexpr double kFirstClaimM = 2.0, kLostM = 5.0, kPosGain = 10.0, kRotGain = 10.0, kMaxCorrection = 4.0, kMaxSpin = 20.0,
                 kMaxCorrSpin = 6.0, kMaxSpeed = 20.0, kMaxAgeS = 0.2;
constexpr double kDoneM = 0.03, kDoneRad = 0.035;
// push watch (host)
constexpr double kWatchRadius = 3.0, kSinkM = 0.5;
constexpr int kWatchTicks = 125; // 2 s

using PreStep_t = void(__fastcall*)(uint8_t* world, uint32_t tick);
using PostKinematic_t = void(__fastcall*)(uint8_t* scene, uint8_t* so, const double* transform);
using SetMasks_t = void(__fastcall*)(uint8_t* scene, uint8_t* so, int group, int mask);
using SetFlags_t = void(__fastcall*)(uint8_t* scene, uint8_t* so, int flags);
using SetVec_t = void(__fastcall*)(uint8_t* scene, uint8_t* so, const float* v);
using LocalCharacter_t = const void*(__cdecl*)();

PreStep_t g_orig_pre = nullptr;
PostKinematic_t g_orig_kin = nullptr, g_orig_transform = nullptr;
SetFlags_t g_set_flags = nullptr;
SetMasks_t g_set_masks = nullptr;
SetVec_t g_set_lin = nullptr, g_set_ang = nullptr;
const volatile uint8_t* g_is_host = nullptr;
LocalCharacter_t g_local_char = nullptr;
DWORD g_local_looked = 0;
bool g_on = false, g_vehicles_on = false, g_outside_ok = false;
DWORD(WINAPI* tick_now)() = &GetTickCount; // the clock (tests set their own)

// --- pure parts (unit tested) -----------------------------------------------------------------------------------------

#pragma pack(push, 1)
struct WireState {
    uint32_t id;
    uint8_t kind, flags; // kind: loose object 0..2, vehicle body kVehiclePart + index; flags: 1 = releasing
    double p[3];
    float q[4], v[3], w[3];
};
struct WireHeader {
    uint32_t seq;
    uint16_t count;
};
struct WireDeny {
    uint32_t id;
    uint8_t kind;
};
#pragma pack(pop)
constexpr uint8_t kStateReleasing = 1;
constexpr uint32_t kMaxWire = 15; // 6 + 15 x 70 bytes: one packet (redundancy.cpp kMaxPacket 1100 with its header)

bool valid_kind(uint8_t k) { return k <= 2 || (k >= kVehiclePart && k < kVehiclePart + kMaxVehicleBodies); }

// Bullet's basis rows (m[r] = row r, 4 doubles each, the 4th unused) -> quaternion x, y, z, w
void basis_to_quat(const double* rows, float q[4])
{
    auto m = [&](int r, int c) { return rows[r * 4 + c]; };
    double tr = m(0, 0) + m(1, 1) + m(2, 2), x, y, z, w;
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        w = 0.25 * s, x = (m(2, 1) - m(1, 2)) / s, y = (m(0, 2) - m(2, 0)) / s, z = (m(1, 0) - m(0, 1)) / s;
    } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
        double s = std::sqrt(1.0 + m(0, 0) - m(1, 1) - m(2, 2)) * 2;
        w = (m(2, 1) - m(1, 2)) / s, x = 0.25 * s, y = (m(0, 1) + m(1, 0)) / s, z = (m(0, 2) + m(2, 0)) / s;
    } else if (m(1, 1) > m(2, 2)) {
        double s = std::sqrt(1.0 + m(1, 1) - m(0, 0) - m(2, 2)) * 2;
        w = (m(0, 2) - m(2, 0)) / s, x = (m(0, 1) + m(1, 0)) / s, y = 0.25 * s, z = (m(1, 2) + m(2, 1)) / s;
    } else {
        double s = std::sqrt(1.0 + m(2, 2) - m(0, 0) - m(1, 1)) * 2;
        w = (m(1, 0) - m(0, 1)) / s, x = (m(0, 2) + m(2, 0)) / s, y = (m(1, 2) + m(2, 1)) / s, z = 0.25 * s;
    }
    double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0)) n = 1, x = y = z = 0, w = 1;
    q[0] = float(x / n), q[1] = float(y / n), q[2] = float(z / n), q[3] = float(w / n);
}

// World-frame rotation vector (axis x angle) that turns orientation `from` into `to` (both body -> world).
void rotation_between(const float from[4], const float to[4], double out[3])
{
    // e = to * conj(from)
    double ax = to[0], ay = to[1], az = to[2], aw = to[3];
    double bx = -from[0], by = -from[1], bz = -from[2], bw = from[3];
    double x = aw * bx + ax * bw + ay * bz - az * by;
    double y = aw * by - ax * bz + ay * bw + az * bx;
    double z = aw * bz + ax * by - ay * bx + az * bw;
    double w = aw * bw - ax * bx - ay * by - az * bz;
    if (w < 0) x = -x, y = -y, z = -z, w = -w; // the short way round
    double s = std::sqrt(x * x + y * y + z * z);
    if (s < 1e-9) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    double angle = 2 * std::atan2(s, w);
    out[0] = x / s * angle, out[1] = y / s * angle, out[2] = z / s * angle;
}

double length3(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

void clamp3(double* v, double max)
{
    double n = length3(v);
    if (n > max && n > 0)
        for (int i = 0; i < 3; i++) v[i] *= max / n;
}

// Host: velocities that bring the host's body (cur_p, cur_q) onto the client's state t, age_s after the client sent it.
// Returns the position error (m) and rotation error (rad).
void steer(const WireState& t, double age_s, const double cur_p[3], const float cur_q[4], float v_out[3], float w_out[3],
           double& err_m, double& err_rad)
{
    age_s = std::clamp(age_s, 0.0, kMaxAgeS);
    double corr[3], spin[3];
    for (int i = 0; i < 3; i++) corr[i] = (t.p[i] + t.v[i] * age_s - cur_p[i]) * kPosGain;
    err_m = length3(corr) / kPosGain;
    clamp3(corr, kMaxCorrection);
    double v[3] = {t.v[0] + corr[0], t.v[1] + corr[1], t.v[2] + corr[2]};
    clamp3(v, kMaxSpeed);
    rotation_between(cur_q, t.q, spin);
    err_rad = length3(spin);
    for (double& s : spin) s *= kRotGain;
    clamp3(spin, kMaxCorrSpin);
    double w[3] = {t.w[0] + spin[0], t.w[1] + spin[1], t.w[2] + spin[2]};
    clamp3(w, kMaxSpin);
    for (int i = 0; i < 3; i++) v_out[i] = float(v[i]), w_out[i] = float(w[i]);
}

struct Box {
    double mn[3], mx[3];
};

bool boxes_touch(const Box& a, const Box& b, double margin)
{
    for (int i = 0; i < 3; i++)
        if (a.mn[i] > b.mx[i] + margin || b.mn[i] > a.mx[i] + margin) return false;
    return true;
}

bool box_ok(const Box& b, double max_extent)
{
    for (int i = 0; i < 3; i++) {
        double e = b.mx[i] - b.mn[i];
        if (!std::isfinite(b.mn[i]) || !std::isfinite(b.mx[i]) || e < 0 || e > max_extent) return false;
    }
    return true;
}

void grow(Box& u, const Box& b, bool first)
{
    for (int i = 0; i < 3; i++) {
        u.mn[i] = first ? b.mn[i] : std::min(u.mn[i], b.mn[i]);
        u.mx[i] = first ? b.mx[i] : std::max(u.mx[i], b.mx[i]);
    }
}

// Every one of n bodies is linked to the others by joints whose both ends are among them. links: (a, b) index pairs.
bool all_connected(int n, const std::vector<std::pair<int, int>>& links)
{
    if (n <= 1) return true;
    std::vector<int> up(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) up[size_t(i)] = i;
    auto root = [&](int i) {
        while (up[size_t(i)] != i) i = up[size_t(i)] = up[size_t(up[size_t(i)])];
        return i;
    };
    for (auto [a, b] : links) up[size_t(root(a))] = root(b);
    for (int i = 1; i < n; i++)
        if (root(i) != root(0)) return false;
    return true;
}

uint32_t pack_states(uint32_t seq, const std::vector<WireState>& s, uint8_t* out, uint32_t cap)
{
    uint32_t n = uint32_t(std::min<size_t>(s.size(), kMaxWire));
    uint32_t need = uint32_t(sizeof(WireHeader) + n * sizeof(WireState));
    if (!n || need > cap) return 0;
    WireHeader h{seq, uint16_t(n)};
    memcpy(out, &h, sizeof h);
    memcpy(out + sizeof h, s.data(), n * sizeof(WireState));
    return need;
}

bool unpack_states(const uint8_t* data, uint32_t size, uint32_t& seq, std::vector<WireState>& out)
{
    if (size < sizeof(WireHeader)) return false;
    WireHeader h;
    memcpy(&h, data, sizeof h);
    if (h.count > kMaxWire || size != sizeof h + h.count * sizeof(WireState)) return false;
    seq = h.seq;
    out.resize(h.count);
    memcpy(out.data(), data + sizeof h, h.count * sizeof(WireState));
    for (const WireState& s : out) {
        if (!valid_kind(s.kind)) return false;
        for (double p : s.p)
            if (!std::isfinite(p)) return false;
        for (float f : s.q)
            if (!std::isfinite(f)) return false;
        for (int i = 0; i < 3; i++)
            if (!std::isfinite(s.v[i]) || !std::isfinite(s.w[i])) return false;
    }
    return true;
}

// --- game reads (update thread; bodies are the physics thread's, read racily: a torn pose is one substep off) -------

struct RingView {
    uint8_t* const* data = nullptr;
    uint32_t cap = 0, head = 0, count = 0;
    uint8_t* at(uint32_t i) const { return data[(head + i) % cap]; }
};

bool ring_at(const uint8_t* p, RingView& r, uint32_t max_count)
{
    r.data = *reinterpret_cast<uint8_t* const* const*>(p);
    r.cap = *reinterpret_cast<const uint32_t*>(p + 8);
    r.head = *reinterpret_cast<const uint32_t*>(p + 0xc);
    r.count = *reinterpret_cast<const uint32_t*>(p + 0x10);
    return r.data && r.cap && r.count <= r.cap && r.count <= max_count;
}

uint8_t* scene_object_of(uint8_t* entry, int kind)
{
    uint8_t* handle = *reinterpret_cast<uint8_t**>(entry + kEntryHandle);
    if (!handle) return nullptr;
    uint8_t* o = *reinterpret_cast<uint8_t**>(handle + kKinds[kind].slot);
    if (!o) return nullptr;
    // 0xa3 is the cylinder type for vanilla replay, not the only movable type. Pallets are boxes.
    uint8_t* so = *reinterpret_cast<uint8_t**>(o + kKinds[kind].scene_object);
    uint8_t* body = so ? *reinterpret_cast<uint8_t**>(so + kSoBody) : nullptr;
    return body && *reinterpret_cast<uint8_t**>(body + kBodyUser) == so ? so : nullptr;
}

uint32_t id_of(const uint8_t* entry) { return *reinterpret_cast<const uint32_t*>(entry + kEntryId); }

uint8_t* find_entry(const RingView& r, uint32_t id)
{
    int lo = 0, hi = int(r.count) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint8_t* e = r.at(uint32_t(mid));
        if (!e) break;
        uint32_t v = id_of(e);
        if (v == id) return e;
        if (v < id) lo = mid + 1;
        else hi = mid - 1;
    }
    for (uint32_t i = 0; i < r.count; i++)
        if (uint8_t* e = r.at(i); e && id_of(e) == id) return e;
    return nullptr;
}

bool vehicle_bodies(const uint8_t* v, RingView& bodies)
{
    if (*reinterpret_cast<const uint32_t*>(v + kVehicleType) != kVehicleTypeValue || *v) return false;
    const uint8_t* br = *reinterpret_cast<uint8_t* const*>(v + kVehicleBodies);
    return br && ring_at(br, bodies, 100000) && bodies.count > 0;
}

uint8_t* find_vehicle(const RingView& vehicles, uint32_t id)
{
    for (uint32_t i = 0; i < vehicles.count; i++) {
        uint8_t* v = vehicles.at(i);
        RingView b;
        if (v && *reinterpret_cast<const uint32_t*>(v + kVehicleId) == id && vehicle_bodies(v, b)) return v;
    }
    return nullptr;
}

uint8_t* vehicle_body_so(const RingView& bodies, uint32_t i)
{
    if (i >= bodies.count) return nullptr;
    uint8_t* rec = bodies.at(i);
    return rec ? *reinterpret_cast<uint8_t**>(rec + kBodyRecordSo) : nullptr;
}

bool body_box(const uint8_t* body, Box& b)
{
    const uint8_t* proxy = *reinterpret_cast<uint8_t* const*>(body + kBodyProxy);
    if (!proxy) return false;
    memcpy(b.mn, proxy + kProxyMin, 24);
    memcpy(b.mx, proxy + kProxyMax, 24);
    return true;
}

void body_state(const uint8_t* body, WireState& s)
{
    memcpy(s.p, body + kBodyOrigin, 24);
    double rows[12];
    memcpy(rows, body + kBodyBasis, sizeof rows);
    basis_to_quat(rows, s.q);
    double v[3], w[3];
    memcpy(v, body + kBodyLinVel, 24);
    memcpy(w, body + kBodyAngVel, 24);
    for (int i = 0; i < 3; i++) s.v[i] = float(v[i]), s.w[i] = float(w[i]);
}

// Joints among these bodies (Bullet's per-body constraint list) as index pairs.
std::vector<std::pair<int, int>> joints_among(const std::vector<const uint8_t*>& bodies, int& total)
{
    std::vector<std::pair<int, int>> links;
    total = 0;
    auto index_of = [&](const uint8_t* b) {
        for (size_t i = 0; i < bodies.size(); i++)
            if (bodies[i] == b) return int(i);
        return -1;
    };
    for (const uint8_t* body : bodies) {
        int n = *reinterpret_cast<const int*>(body + kBodyConstraintCount);
        const uint8_t* const* list = *reinterpret_cast<const uint8_t* const* const*>(body + kBodyConstraints);
        if (n <= 0 || n > 256 || !list) continue;
        total += n;
        for (int k = 0; k < n; k++) {
            const uint8_t* c = list[k];
            if (!c) continue;
            int a = index_of(*reinterpret_cast<const uint8_t* const*>(c + kConstraintA));
            int b = index_of(*reinterpret_cast<const uint8_t* const*>(c + kConstraintB));
            if (a >= 0 && b >= 0 && a != b) links.push_back({a, b});
        }
    }
    return links;
}

// The bodies the joints of these bodies lead to that aren't among them (each once).
std::vector<const uint8_t*> joined_outside(const std::vector<const uint8_t*>& bodies)
{
    std::vector<const uint8_t*> out;
    auto among = [&](const uint8_t* b) { return std::find(bodies.begin(), bodies.end(), b) != bodies.end(); };
    for (const uint8_t* body : bodies) {
        int n = *reinterpret_cast<const int*>(body + kBodyConstraintCount);
        const uint8_t* const* list = *reinterpret_cast<const uint8_t* const* const*>(body + kBodyConstraints);
        if (n <= 0 || n > 256 || !list) continue;
        for (int k = 0; k < n; k++) {
            const uint8_t* c = list[k];
            if (!c) continue;
            for (size_t end : {kConstraintA, kConstraintB}) {
                const uint8_t* o = *reinterpret_cast<const uint8_t* const*>(c + end);
                if (o && !among(o) && std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
            }
        }
    }
    return out;
}

void class_name(const uint8_t* obj, char* name, size_t size)
{
    static uintptr_t base = 0, end = 0;
    name[0] = 0;
    if (!base) {
        base = game_base();
        if (!base) return;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        end = base + nt->OptionalHeader.SizeOfImage;
    }
    if (!obj) return;
    auto in = [&](uintptr_t a) { return a >= base && a < end; };
    uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
    if (!in(vt)) return;
    auto col = *reinterpret_cast<const uint8_t* const*>(vt - 8);
    if (!in(reinterpret_cast<uintptr_t>(col))) return;
    uint32_t td = *reinterpret_cast<const uint32_t*>(col + 12);
    if (!in(base + td + 16)) return;
    snprintf(name, size, "%s", reinterpret_cast<const char*>(base + td + 16));
}

// "flags 0x.., inverse mass .., n joints, scene object <class>, w x h x d m, lowest point y .." for a log line.
void describe_body(const uint8_t* body, char* out, size_t size)
{
    const uint8_t* so = *reinterpret_cast<const uint8_t* const*>(body + kBodyUser);
    char cls[96];
    class_name(so, cls, sizeof cls);
    Box b{};
    bool box = body_box(body, b);
    snprintf(out, size, "flags 0x%x, inverse mass %.4g, %d joints, scene object %s, %.2f x %.2f x %.2f m, lowest point y %.2f",
             *reinterpret_cast<const int*>(body + kBodyFlags), *reinterpret_cast<const double*>(body + kBodyInvMass),
             *reinterpret_cast<const int*>(body + kBodyConstraintCount), cls[0] ? cls : "(unknown class)",
             box ? b.mx[0] - b.mn[0] : 0.0, box ? b.mx[1] - b.mn[1] : 0.0, box ? b.mx[2] - b.mn[2] : 0.0, box ? b.mn[1] : 0.0);
}

int character_boxes(const uint8_t* ch, Box out[3])
{
    int n = 0;
    for (size_t off : kCharBodies) {
        const uint8_t* so = *reinterpret_cast<uint8_t* const*>(ch + off);
        const uint8_t* body = so ? *reinterpret_cast<uint8_t* const*>(so + kSoBody) : nullptr;
        if (body && body_box(body, out[n]) && box_ok(out[n], kMaxExtent)) n++;
    }
    return n;
}

const uint8_t* local_character()
{
    if (!g_local_char) {
        DWORD now = tick_now();
        if (g_local_looked && now - g_local_looked < 5000) return nullptr;
        g_local_looked = now;
        HMODULE m = GetModuleHandleW(L"physics_fixes.dll");
        g_local_char = m ? reinterpret_cast<LocalCharacter_t>(GetProcAddress(m, "swml_local_character")) : nullptr;
        if (!g_local_char) {
            static bool told = false;
            if (!told) swlog("object push: no local character (needs physics_fixes 0.25.1 or later): nothing is claimed");
            told = true;
            return nullptr;
        }
        swlog("object push: physics_fixes' local character found");
    }
    return static_cast<const uint8_t*>(g_local_char());
}

// --- shared state -----------------------------------------------------------------------------------------------------

std::mutex g_mu; // everything below: update thread vs network thread
// The native grab controller owns held movable objects. Never snap or touch-own them.
bool held_movable(const uint8_t* entry) {
    auto handle=entry?*reinterpret_cast<const uint8_t* const*>(entry+kEntryHandle):nullptr;
    auto movable=handle?*reinterpret_cast<const uint8_t* const*>(handle+kKinds[0].slot):nullptr;
    return movable && (*reinterpret_cast<const uint8_t*>(movable+0x379)!=0 ||
                       *reinterpret_cast<const void* const*>(movable+0x248)!=nullptr);
}

// client
struct Part {
    uint8_t* so = nullptr;
    uint8_t* body = nullptr;
    uint8_t kind = 0; // wire kind
    int saved_body_flags = 0, saved_so_flags = 0;
    bool flags_changed = false, masks_changed = false;
    int saved_group = 0, saved_mask = 0;
    double start_p[3] = {};
    double replay[16]{}; // latest suppressed native target, kept current while locally simulated
    bool replay_valid=false;
};
struct Unit {
    bool vehicle = false;
    uint32_t id = 0;
    uint8_t* entry = nullptr; // loose: the ring entry; vehicle: the vehicle
    std::vector<Part> parts;
    DWORD since = 0, last_touch = 0, releasing_since = 0;
    double moved_m = 0;
    bool support_logged = false;
};
std::vector<Unit> g_units;                          // update thread (read by the kinematic detour: same thread)
std::map<uint64_t, DWORD> g_blocked;                // unit key -> denied at
// Touched but not owned yet: where the replay had each body (a claim needs it still over two touching ticks).
struct Seen {
    std::vector<double> p;
    DWORD at = 0;
};
std::map<const uint8_t*, Seen> g_seen; // loose: scene object; vehicle: the vehicle
std::vector<WireState> g_out;          // the newest batch for the host
uint32_t g_out_seq = 0, g_out_sent_seq = 0;
std::vector<WireDeny> g_denied_in; // from the host, for the update thread
DWORD g_host_cap_at = 0;
std::atomic<DWORD> g_client_physics_at{0}; // fresh compatible-host capability, published by pre-step
// host
struct Claim {
    int peer = -1;
    uint32_t seq = 0;
    WireState s{};
    DWORD at = 0; // received
    bool steering = false;
    double err_m = 0, err_rad = 0;
    DWORD started = 0;
};
std::map<uint64_t, Claim> g_claims;              // (id, kind) -> current owner's newest state
std::map<int, std::vector<WireDeny>> g_deny_out; // peer -> denies to send
struct PeerSeq {
    uint32_t seq = 0;
    DWORD at = 0;
};
std::map<int, PeerSeq> g_peer_seq; // newest batch number per peer, and when it came
// stats (30 s)
uint64_t g_taken = 0, g_taken_vehicles = 0, g_released = 0, g_denies_got = 0, g_dropped_kin = 0, g_batches = 0, g_lost = 0,
         g_no_joints = 0;
uint64_t g_h_claims = 0, g_h_steer = 0, g_h_denied = 0, g_h_done = 0, g_h_batches = 0, g_watch_lines = 0;
double g_h_max_err = 0, g_c_max_moved = 0;
int g_claim_logs = 0, g_host_claim_logs = 0, g_joint_logs = 0;
DWORD g_update_thread = 0; // the pre-step's thread (the kinematic detour only acts there)
// touch census (client, update thread): physics_fixes' swml_touched_bodies, what it returned in this 30 s window
using Touched_t = int(__cdecl*)(const void** out, int max);
Touched_t g_touched = nullptr;
DWORD g_touched_looked = 0, g_census_at = 0;
std::set<const void*> g_touch_objects, g_touch_vehicles, g_touch_neither, g_neither_logged;
int g_neither_logs = 0;
DWORD g_last_stats = 0;

// Why touched units weren't claimed (30 s counts; the first 3 of each reason logged with the unit).
enum Refusal {
    kRefLimit, kRefBlocked, kRefWater, kRefMoving, kRefJoints, kRefStatic, kRefBodies, kRefSize, kRefOutside, kRefStanding,
    kRefCount
};
const char* const kRefusalNames[kRefCount] = {"already owning the most", "refused lately", "near the sea", "replay moving",
                                              "not joined", "static", "over 10 bodies", "over the size limit",
                                              "joined to bodies outside it", "standing on it"};
uint64_t g_refused[kRefCount] = {};
int g_refusal_logs[kRefCount] = {};

uint64_t key_of(uint32_t id, uint8_t kind) { return uint64_t(id) << 8 | kind; }
uint64_t unit_key(const Unit& u) { return key_of(u.id, u.vehicle ? kVehiclePart : u.parts[0].kind); }

void refuse(Refusal why, bool vehicle, uint32_t id, size_t bodies, const Box& b)
{
    g_refused[why]++;
    if (g_refusal_logs[why] >= 3) return;
    g_refusal_logs[why]++;
    swlog("object push: not claimed (%s): %s %u, %zu bod%s, %.1f x %.1f x %.1f m, lowest point y %.2f", kRefusalNames[why],
          vehicle ? "vehicle" : "object", id, bodies, bodies == 1 ? "y" : "ies", b.mx[0] - b.mn[0], b.mx[1] - b.mn[1],
          b.mx[2] - b.mn[2], b.mn[1]);
}

// Complete a vehicle's local Bullet assembly, including wheels that have no vehicle body record.
// External wheels are ordinary dynamic bodies, not host-replayed bodies. Keep them in the local
// assembly but send only bodies with real network IDs: the host simulates its own wheels via the joints.
// Refuse driven/kinematic anchors, unknown scene
// objects, invalid links and assemblies over the wire/body limit; never detach a root from its wheels.
bool complete_assembly(Unit& u)
{
    unsigned extras = 0;
    for (size_t i = 0; i < u.parts.size(); i++) {
        const uint8_t* body = u.parts[i].body;
        int n = *reinterpret_cast<const int*>(body + kBodyConstraintCount);
        auto list = *reinterpret_cast<const uint8_t* const* const*>(body + kBodyConstraints);
        if (n < 0 || n > 256 || (n && !list)) return false;
        for (int k = 0; k < n; k++) {
            const uint8_t* c = list[k];
            if (!c) return false;
            auto a = *reinterpret_cast<uint8_t* const*>(c + kConstraintA);
            auto b = *reinterpret_cast<uint8_t* const*>(c + kConstraintB);
            if (a != body && b != body) return false;
            uint8_t* other = a == body ? b : a;
            if (!other) return false;
            if (std::any_of(u.parts.begin(), u.parts.end(), [&](const Part& p) { return p.body == other; })) continue;
            if (u.parts.size() >= kMaxVehicleBodies || *reinterpret_cast<const int*>(other + kBodyType) != 2 ||
                (*reinterpret_cast<const int*>(other + kBodyFlags) & (3 | kFlagDriven)) ||
                !(*reinterpret_cast<const double*>(other + kBodyInvMass) > 0)) return false;
            Part p;
            p.body = other;
            p.so = *reinterpret_cast<uint8_t* const*>(other + kBodyUser);
            if (!p.so || *reinterpret_cast<uint8_t* const*>(p.so + kSoBody) != other) return false;
            p.kind = uint8_t(kJointPart + extras++);
            u.parts.push_back(p);
        }
    }
    return true;
}

bool is_owned_so(const uint8_t* so)
{
    for (const Unit& u : g_units)
        for (const Part& p : u.parts)
            if (p.so == so) return true;
    return false;
}

// --- client -----------------------------------------------------------------------------------------------------------

// The step a session may prove wrong (see the top): a replayed copy becomes an ordinary body, and back.
void unfreeze(uint8_t* scene, Part& p)
{
    int flags = p.saved_body_flags & ~(kFlagDriven | kFlagKinematic);
    if (flags != p.saved_body_flags) {
        g_set_flags(scene, p.so, flags);
        p.flags_changed = true;
    }
    // A client vehicle is born as group 0x80 / mask 0x20 (character-only). Clearing 0x800
    // does not change this: without reinsertion its newly dynamic chassis has NO terrain pairs.
    // Keep original bits and add native movable bits, retaining character and vehicle interactions.
    { // Includes passive native wheel helpers: clients create those with mask 0.
        int group = p.saved_group | 0x21;
        if (group != p.saved_group || p.saved_mask != -1) {
            g_set_masks(scene, p.so, group, -1);
            p.masks_changed = true;
        }
    }
}

void restore(uint8_t* scene, Part& p)
{
    if (p.flags_changed) {
        g_set_flags(scene, p.so, p.saved_body_flags);
        *reinterpret_cast<int*>(p.so + kSoFlags) = p.saved_so_flags;
        p.flags_changed = false;
    }
    if (p.masks_changed) {
        g_set_masks(scene, p.so, p.saved_group, p.saved_mask);
        p.masks_changed = false;
    }
}

#include "safe_handoff.inc"

void restore_all(uint8_t* scene, Unit& u)
{
    rebase_handoff(scene,u);
    for (Part& p : u.parts) restore(scene, p);
}

bool take(uint8_t* scene, Unit u, DWORD now)
{
    for (Part& p : u.parts) {
        p.saved_body_flags = *reinterpret_cast<const volatile int*>(p.body + kBodyFlags);
        p.saved_so_flags = *reinterpret_cast<const int*>(p.so + kSoFlags);
        auto proxy = *reinterpret_cast<const uint8_t* const*>(p.body + kBodyProxy);
        if (!proxy) return false;
        p.saved_group = *reinterpret_cast<const int*>(proxy + kProxyGroup);
        p.saved_mask = *reinterpret_cast<const int*>(proxy + kProxyMask);
        memcpy(p.start_p, p.body + kBodyOrigin, 24);
    }
    if (g_claim_logs < 5) {
        g_claim_logs++;
        std::vector<const uint8_t*> bodies;
        for (const Part& pt : u.parts) bodies.push_back(pt.body);
        int refs = 0;
        int inside = int(joints_among(bodies, refs).size()); // one link per reference from one of these bodies
        swlog("object push: claimed %s %u: %d joint refs, %d of them to bodies outside it", u.vehicle ? "vehicle" : "object", u.id,
              refs, refs - inside);
        const Part& p = u.parts[0];
        swlog("object push: assembly bodies %zu; original filter group 0x%x mask 0x%x", u.parts.size(),
              p.saved_group, p.saved_mask);
        swlog("object push: claimed %s %u (%zu bod%s): first body flags 0x%x (scene object 0x%x), activation %d, type %d, "
              "inverse mass %.4g, joints %d",
              u.vehicle ? "vehicle" : "object", u.id, u.parts.size(), u.parts.size() == 1 ? "y" : "ies", p.saved_body_flags,
              p.saved_so_flags, *reinterpret_cast<const int*>(p.body + kBodyActivation),
              *reinterpret_cast<const int*>(p.body + kBodyType), *reinterpret_cast<const double*>(p.body + kBodyInvMass),
              *reinterpret_cast<const int*>(p.body + kBodyConstraintCount));
    }
    for (const Part& p : u.parts)
        if (p.saved_body_flags & 1) return false; // static: not ours to move
    float zero[3] = {};
    for (Part& p : u.parts) {
        unfreeze(scene, p);
        if (p.kind >= kJointPart || (p.kind == 0 && !(p.saved_body_flags & (kFlagDriven | kFlagKinematic)))) continue; // keep native wheel/box velocities
        g_set_lin(scene, p.so, zero);
        g_set_ang(scene, p.so, zero); // wakes it
    }
    u.since = u.last_touch = now;
    g_units.push_back(std::move(u));
    (g_units.back().vehicle ? g_taken_vehicles : g_taken)++;
    return true;
}

// The unit's bodies as they are now, or false if it is gone or changed (then it is forgotten, nothing restored).
bool still_there(const RingView& objects, const RingView& vehicles, const Unit& u)
{
    if (!u.vehicle) {
        const Part& p = u.parts[0];
        uint8_t* e = find_entry(objects, u.id);
        return e == u.entry && scene_object_of(e, p.kind) == p.so && *reinterpret_cast<uint8_t* const*>(p.so + kSoBody) == p.body;
    }
    if (find_vehicle(vehicles, u.id) != u.entry) return false;
    RingView b;
    if (!vehicle_bodies(u.entry, b) || b.count > u.parts.size()) return false;
    Unit current;
    for (uint32_t i = 0; i < b.count; i++) {
        Part p;
        p.so = vehicle_body_so(b, i);
        p.body = p.so ? *reinterpret_cast<uint8_t* const*>(p.so + kSoBody) : nullptr;
        p.kind = uint8_t(kVehiclePart + i);
        if (!p.body) return false;
        current.parts.push_back(p);
    }
    if (!complete_assembly(current) || current.parts.size() != u.parts.size()) return false;
    for (size_t i = 0; i < current.parts.size(); i++)
        if (current.parts[i].so != u.parts[i].so || current.parts[i].body != u.parts[i].body ||
            current.parts[i].kind != u.parts[i].kind) return false;
    return true;
}

Box unit_box(const Unit& u)
{
    Box all{}, b;
    bool first = true;
    for (const Part& p : u.parts)
        if (body_box(p.body, b)) grow(all, b, first), first = false;
    return all;
}

// A touched unit: refresh it if owned, else claim it when the rules allow. standing: the character stands on it (that
// is no push: an owned unit isn't kept for it, so one walked onto is handed back as if let go).
void on_touch(uint8_t* scene, Unit cand, DWORD now, bool standing)
{
    if (!cand.vehicle && held_movable(cand.entry)) return;
    for (Unit& u : g_units) {
        if (u.vehicle != cand.vehicle || u.entry != cand.entry || u.parts[0].so != cand.parts[0].so) continue;
        if (standing && (u.parts[0].saved_body_flags & kFlagDriven)) return;
        u.last_touch = now;
        if (u.releasing_since) { // touched again while releasing: own it again
            u.releasing_since = 0;
            for (Part& p : u.parts) unfreeze(scene, p);
        }
        return;
    }
    int loose = 0, vehicles = 0;
    for (const Unit& u : g_units) (u.vehicle ? vehicles : loose)++;
    Box box = unit_box(cand);
    auto no = [&](Refusal why) { refuse(why, cand.vehicle, cand.id, cand.parts.size(), box); };
    if (cand.vehicle ? vehicles >= kMaxVehiclesOwned : loose >= kMaxLooseOwned) return no(kRefLimit);
    if (g_blocked.count(unit_key(cand))) return no(kRefBlocked);
    if (box.mn[1] < kClaimAboveSeaM) return no(kRefWater);
    if (standing && (*reinterpret_cast<const int*>(cand.parts[0].body + kBodyFlags) & kFlagDriven)) return no(kRefStanding);
    // Ordinary movable boxes already simulate locally. Waiting for a still replay makes a pallet
    // unclaimable once the player's first contact has set it moving. Claim that existing simulation
    // immediately; only frozen/replayed objects need the two-tick stillness check.
    bool native_box = !cand.vehicle && cand.parts[0].kind == 0 &&
                      !(*reinterpret_cast<const int*>(cand.parts[0].body + kBodyFlags) & (kFlagDriven | kFlagKinematic));
    // still over two touching ticks
    const uint8_t* seen_key = cand.vehicle ? cand.entry : cand.parts[0].so;
    std::vector<double> p;
    for (const Part& pt : cand.parts) p.insert(p.end(), reinterpret_cast<const double*>(pt.body + kBodyOrigin),
                                               reinterpret_cast<const double*>(pt.body + kBodyOrigin) + 3);
    auto seen = g_seen.find(seen_key);
    bool still = false;
    if (seen != g_seen.end() && now - seen->second.at <= kStillGapMs && seen->second.p.size() == p.size()) {
        still = true;
        for (size_t i = 0; i + 2 < p.size() && still; i += 3) {
            double d[3] = {p[i] - seen->second.p[i], p[i + 1] - seen->second.p[i + 1], p[i + 2] - seen->second.p[i + 2]};
            still = length3(d) < kStillM;
        }
    }
    bool first = seen == g_seen.end() || now - seen->second.at > kStillGapMs;
    Seen& sn = g_seen[seen_key];
    sn.p = std::move(p);
    sn.at = now;
    if (!still && !native_box) {
        if (!first) no(kRefMoving);
        return;
    }
    if (cand.vehicle && cand.parts.size() > 1) {
        std::vector<const uint8_t*> bodies;
        for (const Part& pt : cand.parts) bodies.push_back(pt.body);
        int total = 0;
        if (!all_connected(int(bodies.size()), joints_among(bodies, total))) {
            g_no_joints++;
            no(kRefJoints);
            if (g_joint_logs < 3) {
                g_joint_logs++;
                swlog("object push: vehicle %u (%zu bodies) not claimed: its bodies aren't all joined on this PC (%d joint refs)",
                      cand.id, bodies.size(), total);
            }
            g_blocked[unit_key(cand)] = now; // don't check it every tick
            return;
        }
    }
    // Any links remaining outside the completed assembly still prevent ownership. The legacy
    // outside_joints override is kept for loose equipment; vehicle assemblies always validate their anchors.
    if (!g_outside_ok) {
        std::vector<const uint8_t*> bodies;
        for (const Part& pt : cand.parts) bodies.push_back(pt.body);
        std::vector<const uint8_t*> out = joined_outside(bodies);
        if (!out.empty()) {
            if (g_refusal_logs[kRefOutside] < 3) {
                char d[300];
                describe_body(out[0], d, sizeof d);
                swlog("object push: %s %u (%zu bod%s) is joined to %zu bod%s outside it; the first: %s", cand.vehicle ? "vehicle" : "object",
                      cand.id, bodies.size(), bodies.size() == 1 ? "y" : "ies", out.size(), out.size() == 1 ? "y" : "ies", d);
            }
            no(kRefOutside);
            g_blocked[unit_key(cand)] = now; // don't check it every tick
            return;
        }
    }
    uint64_t key = unit_key(cand);
    bool vehicle = cand.vehicle;
    uint32_t id = cand.id;
    size_t nb = cand.parts.size();
    if (take(scene, std::move(cand), now)) g_seen.erase(seen_key);
    else {
        refuse(kRefStatic, vehicle, id, nb, box);
        g_blocked[key] = now; // static: don't try it every tick
    }
}

// What the character touches, from physics_fixes' touch census (every body, not only the two lists'): counted by
// where it is, and anything in neither list logged once (the first 15), so a session shows what pallets are.
bool is_character_class(const uint8_t* body)
{
    char cls[96];
    class_name(*reinterpret_cast<const uint8_t* const*>(body + kBodyUser), cls, sizeof cls);
    return strstr(cls, "character") != nullptr;
}

void census_touched(const RingView& objects, const RingView& vehicles, DWORD now)
{
    if (now - g_census_at < 250) return;
    g_census_at = now;
    if (!g_touched) {
        if (g_touched_looked && now - g_touched_looked < 5000) return;
        g_touched_looked = now;
        HMODULE m = GetModuleHandleW(L"physics_fixes.dll");
        g_touched = m ? reinterpret_cast<Touched_t>(GetProcAddress(m, "swml_touched_bodies")) : nullptr;
        static bool told = false;
        if (!g_touched) {
            if (!told) swlog("object push: no touch census (needs physics_fixes 0.25.3 or later): bodies outside the lists aren't logged");
            told = true;
            return;
        }
        swlog("object push: physics_fixes' touch census found");
    }
    const void* list[32];
    int n = g_touched(list, 32);
    for (int i = 0; i < n; i++) {
        const uint8_t* body = static_cast<const uint8_t*>(list[i]);
        const uint8_t* so = *reinterpret_cast<const uint8_t* const*>(body + kBodyUser);
        if (!so || *reinterpret_cast<const uint8_t* const*>(so + kSoBody) != body) continue; // gone since the scan
        bool found = false;
        for (uint32_t e = 0; e < objects.count && !found; e++)
            if (uint8_t* entry = objects.at(e))
                for (int k = 0; k < 3 && !found; k++) found = scene_object_of(entry, k) == so;
        if (found) {
            g_touch_objects.insert(body);
            continue;
        }
        for (uint32_t v = 0; v < vehicles.count && !found; v++) {
            RingView br;
            if (uint8_t* veh = vehicles.at(v); veh && vehicle_bodies(veh, br))
                for (uint32_t j = 0; j < br.count && !found; j++) found = vehicle_body_so(br, j) == so;
        }
        if (found) {
            g_touch_vehicles.insert(body);
            continue;
        }
        if (is_character_class(body)) continue; // another player or a crew member
        g_touch_neither.insert(body);
        if (g_neither_logs < 15 && g_neither_logged.insert(so).second) {
            g_neither_logs++;
            char d[300];
            describe_body(body, d, sizeof d);
            swlog("object push: touched a body in neither list: %s", d);
            auto proxy = *reinterpret_cast<const uint8_t* const*>(body + kBodyProxy);
            auto owner = *reinterpret_cast<const uint8_t* const*>(so + 0xc0);
            int owner_type = owner ? *reinterpret_cast<const int*>(owner) : -1;
            int movable_type = -1;
            if (owner && owner_type == 3) {
                auto movable = owner - 0x220;
                if (*reinterpret_cast<const uint8_t* const*>(movable + 0x268) == so)
                    movable_type = *reinterpret_cast<const int*>(movable + 0x260);
            }
            swlog("object push: unlisted body filters group 0x%x mask 0x%x; owner type %d, movable kind 0x%x",
                  proxy ? *reinterpret_cast<const int*>(proxy + kProxyGroup) : 0,
                  proxy ? *reinterpret_cast<const int*>(proxy + kProxyMask) : 0, owner_type, movable_type);
        }
    }
}

#include "host_box_sync.inc"
#include "large_vehicle_push.inc"

void client_tick(uint8_t* world)
{
    uint8_t* scene = world + world_scene();
    DWORD now = tick_now();
    RingView objects, vehicles;
    if (!ring_at(world + world_ring(), objects, 1000000) || !ring_at(world + world_vehicles(), vehicles, 1000000)) return;
    bool host_ok = g_host_cap_at && now - g_host_cap_at < kHostCapFreshMs;
    // denies from the host: the whole unit goes back
    for (const WireDeny& d : g_denied_in)
        for (auto it = g_units.begin(); it != g_units.end(); ++it) {
            bool hit = false;
            for (const Part& p : it->parts) hit = hit || (it->id == d.id && p.kind == d.kind);
            if (!hit) continue;
            g_blocked[unit_key(*it)] = now;
            if (still_there(objects, vehicles, *it)) restore_all(scene, *it);
            g_units.erase(it);
            g_denies_got++;
            break;
        }
    g_denied_in.clear();
    for (auto it = g_blocked.begin(); it != g_blocked.end();) it = now - it->second > kDenyBlockMs ? g_blocked.erase(it) : ++it;

    Box me[3];
    int nme = 0;
    if (host_ok)
        if (const uint8_t* ch = local_character(); ch && !*reinterpret_cast<void* const*>(ch + kCharSeat)) nme = character_boxes(ch, me);
    auto touching = [&](const Box& b) {
        for (int j = 0; j < nme; j++)
            if (boxes_touch(me[j], b, kTouchMargin)) return true;
        return false;
    };
    // Standing on a body: the feet (the lowest of the character's boxes) at most kStandM under its top, their middle
    // above its footprint. Walking into a low pallet from the side leaves the middle outside it.
    int low = 0;
    for (int j = 1; j < nme; j++)
        if (me[j].mn[1] < me[low].mn[1]) low = j;
    auto stands_on = [&](const Box& b) {
        if (!nme) return false;
        double fx = (me[low].mn[0] + me[low].mx[0]) / 2, fz = (me[low].mn[2] + me[low].mx[2]) / 2;
        return me[low].mn[1] >= b.mx[1] - kStandM && fx >= b.mn[0] && fx <= b.mx[0] && fz >= b.mn[2] && fz <= b.mx[2];
    };

    if (nme) {
        for (uint32_t i = 0; i < objects.count; i++) {
            uint8_t* e = objects.at(i);
            if (!e) continue;
            for (int k = 0; k < 3; k++) {
                uint8_t* so = scene_object_of(e, k);
                uint8_t* body = so ? *reinterpret_cast<uint8_t**>(so + kSoBody) : nullptr;
                Box b;
                if (!body || !body_box(body, b) || !box_ok(b, 1e6) || !touching(b)) continue;
                if (!box_ok(b, kMaxExtent)) {
                    refuse(kRefSize, false, id_of(e), 1, b);
                    continue;
                }
                Unit u;
                u.id = id_of(e), u.entry = e;
                Part p;
                p.so = so, p.body = body, p.kind = uint8_t(k);
                u.parts.push_back(p);
                on_touch(scene, std::move(u), now, stands_on(b));
            }
        }
        for (uint32_t i = 0; g_vehicles_on && i < vehicles.count; i++) {
            uint8_t* v = vehicles.at(i);
            RingView br;
            if (!v || !vehicle_bodies(v, br)) continue;
            bool too_many = br.count > uint32_t(kMaxVehicleBodies);
            Unit u;
            u.vehicle = true, u.id = *reinterpret_cast<const uint32_t*>(v + kVehicleId), u.entry = v;
            Box all{};
            bool ok = true, touch = false, standing = false;
            for (uint32_t j = 0; j < br.count && ok; j++) {
                Part p;
                p.so = vehicle_body_so(br, j);
                p.body = p.so ? *reinterpret_cast<uint8_t**>(p.so + kSoBody) : nullptr;
                p.kind = uint8_t(kVehiclePart + j);
                Box b;
                ok = p.body && body_box(p.body, b) && box_ok(b, 1e6);
                if (!ok) break;
                grow(all, b, j == 0);
                if (touching(b)) touch = true, standing = standing || stands_on(b);
                u.parts.push_back(p);
            }
            if (!ok) continue;
            bool assembly_near = touch;
            for (int j = 0; j < nme && !assembly_near; j++) assembly_near = boxes_touch(me[j], all, kMaxVehicleExtent);
            if (!assembly_near) continue;
            if (!too_many && !complete_assembly(u)) {
                refuse(kRefOutside, true, u.id, u.parts.size(), all);
                continue;
            }
            // The character can touch a wheel without touching the chassis.
            for (const Part& p : u.parts) {
                Box b;
                if (!body_box(p.body, b) || !box_ok(b, kMaxVehicleExtent)) { ok = false; break; }
                grow(all, b, false);
                if (touching(b)) touch = true, standing = standing || stands_on(b);
            }
            if (!ok || !touch) continue;
            if (too_many) refuse(kRefBodies, true, u.id, br.count, all);
            else if (!box_ok(all, kMaxVehicleExtent)) refuse(kRefSize, true, u.id, br.count, all);
            else on_touch(scene, std::move(u), now, standing);
        }
    }
    for (auto it = g_seen.begin(); it != g_seen.end();) it = now - it->second.at > 1000 ? g_seen.erase(it) : ++it;
    if (nme) census_touched(objects, vehicles, now);

    // owned: release, send
    g_out.clear();
    for (auto it = g_units.begin(); it != g_units.end();) {
        Unit& u = *it;
        if (!still_there(objects, vehicles, u)) {
            // A wheel/joint can disappear while the chassis survives. Restore surviving root bodies;
            // forgetting the assembly must not leave its chassis permanently unfrozen.
            RingView br;
            if (u.vehicle && find_vehicle(vehicles, u.id) == u.entry && vehicle_bodies(u.entry, br))
                for (uint32_t j = 0; j < br.count; j++) {
                    auto so = vehicle_body_so(br, j);
                    for (Part& p : u.parts)
                        if (so == p.so && so && *reinterpret_cast<uint8_t* const*>(so + kSoBody) == p.body) restore(scene, p);
                }
            it = g_units.erase(it);
            g_lost++;
            continue;
        }
        if (!u.vehicle && held_movable(u.entry)) {
            restore_all(scene,u);
            g_seen.erase(u.parts[0].so);
            swlog("object push: native pickup owns object %u; touch ownership released",u.id);
            it=g_units.erase(it);++g_released;continue;
        }
        if (!host_ok) { // host gone or not modded any more: give it back now
            restore_all(scene, u);
            it = g_units.erase(it);
            g_released++;
            continue;
        }
        if (!u.support_logged && now - u.since >= 100) {
            const Part& p = u.parts[0];
            auto proxy = *reinterpret_cast<const uint8_t* const*>(p.body + kBodyProxy);
            swlog("object push: live owned %s %u: flags 0x%x, group 0x%x mask 0x%x, y %.3f",
                  u.vehicle ? "vehicle" : "object", u.id, *reinterpret_cast<const int*>(p.body + kBodyFlags),
                  proxy ? *reinterpret_cast<const int*>(proxy + kProxyGroup) : 0,
                  proxy ? *reinterpret_cast<const int*>(proxy + kProxyMask) : 0,
                  *reinterpret_cast<const double*>(p.body + kBodyOrigin + 8));
            u.support_logged = true;
        }
        bool rest = true;
        size_t first = g_out.size();
        for (const Part& p : u.parts) {
            WireState s{};
            s.id = u.id, s.kind = p.kind;
            body_state(p.body, s);
            double d[3] = {s.p[0] - p.start_p[0], s.p[1] - p.start_p[1], s.p[2] - p.start_p[2]};
            u.moved_m = std::max(u.moved_m, length3(d));
            double v[3] = {s.v[0], s.v[1], s.v[2]}, sp[3] = {s.w[0], s.w[1], s.w[2]};
            rest = rest && length3(v) < kRestSpeed && length3(sp) < kRestSpin;
            if (p.kind < kJointPart) g_out.push_back(s); // native wheels follow the root by joints
        }
        bool wet = unit_box(u).mn[1] < kReleaseAboveSeaM;
        if (!u.releasing_since && (wet || now - u.last_touch > (rest ? kReleaseAfterMs : kForceReleaseMs))) {
            u.releasing_since = now;
            restore_all(scene, u);
        }
        if (u.releasing_since && now - u.releasing_since > kReleasingMs) {
            rebase_handoff(scene,u); // refresh the suppressed target once more before native replay resumes
            g_c_max_moved = std::max(g_c_max_moved, u.moved_m);
            g_out.resize(first);
            it = g_units.erase(it);
            g_released++;
            continue;
        }
        for (size_t i = first; i < g_out.size(); i++) g_out[i].flags = u.releasing_since ? kStateReleasing : 0;
        ++it;
    }
    if (!g_out.empty()) g_out_seq++;
    follow_host_boxes(scene,objects,now,host_ok);
    if(host_ok)collect_vehicle_forces(vehicles);
}

// --- host -------------------------------------------------------------------------------------------------------------

void deny(int peer, uint32_t id, uint8_t kind)
{
    g_deny_out[peer].push_back(WireDeny{id, kind});
    g_h_denied++;
}

// The host's scene object for a claimed part.
uint8_t* host_part(const RingView& objects, const RingView& vehicles, uint32_t id, uint8_t kind)
{
    if (kind <= 2) {
        uint8_t* e = find_entry(objects, id);
        return e ? scene_object_of(e, kind) : nullptr;
    }
    uint8_t* v = find_vehicle(vehicles, id);
    RingView b;
    return v && vehicle_bodies(v, b) ? vehicle_body_so(b, uint32_t(kind - kVehiclePart)) : nullptr;
}

// Push watch: tracked objects near remote players (update thread only).
struct Watch {
    float y[kWatchTicks];
    int n = 0, next = 0;
    DWORD near_at = 0, logged_at = 0;
};
std::map<uint64_t, Watch> g_watch;

void watch_object(uint64_t key, const double p[3], double dist, bool owned, DWORD now)
{
    Watch& w = g_watch[key];
    w.near_at = now;
    w.y[w.next] = float(p[1]);
    w.next = (w.next + 1) % kWatchTicks;
    w.n = std::min(w.n + 1, kWatchTicks);
    float top = -1e30f;
    int top_age = 0;
    for (int i = 0; i < w.n; i++) {
        int idx = (w.next - 1 - i + kWatchTicks) % kWatchTicks;
        if (w.y[idx] > top) top = w.y[idx], top_age = i;
    }
    if (top - p[1] > kSinkM && now - w.logged_at > 5000 && g_watch_lines < 20) {
        w.logged_at = now;
        g_watch_lines++;
        swlog("push watch: object %llu kind %d dropped %.2f m in %d ticks with a remote player %.1f m away (%s), now at (%.1f, "
              "%.1f, %.1f)",
              static_cast<unsigned long long>(key >> 8), int(key & 0xff), top - p[1], top_age, dist,
              owned ? "a client owns it" : "the host simulates it", p[0], p[1], p[2]);
        w.n = 0;
    }
}

void host_tick(uint8_t* world)
{
    uint8_t* scene = world + world_scene();
    DWORD now = tick_now();
    RingView objects, vehicles;
    if (!ring_at(world + world_ring(), objects, 1000000) || !ring_at(world + world_vehicles(), vehicles, 1000000)) return;
    const uint8_t* me_ch = local_character();
    Box me[3];
    int nme = me_ch ? character_boxes(me_ch, me) : 0;
    for (auto it = g_claims.begin(); it != g_claims.end();) {
        Claim& c = it->second;
        uint8_t* so = host_part(objects, vehicles, c.s.id, c.s.kind);
        uint8_t* body = so ? *reinterpret_cast<uint8_t**>(so + kSoBody) : nullptr;
        if (c.s.kind==0 && held_movable(find_entry(objects,c.s.id))) {
            deny(c.peer,c.s.id,c.s.kind);it=g_claims.erase(it);continue;
        }
        if (!body || now - c.at > kClaimStaleMs) {
            it = g_claims.erase(it);
            continue;
        }
        double cur_p[3];
        memcpy(cur_p, body + kBodyOrigin, 24);
        double rows[12];
        memcpy(rows, body + kBodyBasis, sizeof rows);
        float cur_q[4];
        basis_to_quat(rows, cur_q);
        double d[3] = {c.s.p[0] - cur_p[0], c.s.p[1] - cur_p[1], c.s.p[2] - cur_p[2]};
        double dist = length3(d);
        bool mine = false;
        Box b;
        if (nme && body_box(body, b))
            for (int j = 0; j < nme && !mine; j++) mine = boxes_touch(me[j], b, kTouchMargin);
        int flags = *reinterpret_cast<const volatile int*>(body + kBodyFlags);
        if (dist > (c.steering ? kLostM : kFirstClaimM) || mine || (flags & 1)) {
            deny(c.peer, c.s.id, c.s.kind);
            it = g_claims.erase(it);
            continue;
        }
        if (!c.steering) {
            c.steering = true;
            c.started = now;
            g_h_claims++;
            if (g_host_claim_logs < 5) {
                g_host_claim_logs++;
                swlog("object push: host follows %s %u part %d for peer%d: body flags 0x%x, inverse mass %.4g, %.2f m from "
                      "the client's copy",
                      c.s.kind <= 2 ? "object" : "vehicle", c.s.id, c.s.kind, c.peer, flags,
                      *reinterpret_cast<const double*>(body + kBodyInvMass), dist);
            }
        }
        float v[3], w[3];
        steer(c.s, (now - c.at) / 1000.0, cur_p, cur_q, v, w, c.err_m, c.err_rad);
        g_h_max_err = std::max(g_h_max_err, c.err_m);
        g_set_lin(scene, so, v);
        g_set_ang(scene, so, w); // wakes it
        g_h_steer++;
        if ((c.s.flags & kStateReleasing) && ((c.err_m < kDoneM && c.err_rad < kDoneRad) || now - c.started > 120000)) {
            g_h_done++;
            it = g_claims.erase(it);
            continue;
        }
        ++it;
    }

    // push watch and authoritative box snapshots near remote players
    double rc[16][3];
    int nrc = remote_character_positions(rc, 16);
    collect_host_boxes(objects,rc,nrc,now);
    apply_vehicle_forces(scene,vehicles,rc,nrc,now);
    if (nrc)
        for (uint32_t i = 0; i < objects.count; i++) {
            uint8_t* e = objects.at(i);
            if (!e) continue;
            for (int k = 0; k < 3; k++) {
                uint8_t* so = scene_object_of(e, k);
                uint8_t* body = so ? *reinterpret_cast<uint8_t**>(so + kSoBody) : nullptr;
                if (!body) continue;
                double p[3];
                memcpy(p, body + kBodyOrigin, 24);
                double best = 1e30;
                for (int j = 0; j < nrc; j++) {
                    double dd[3] = {p[0] - rc[j][0], p[1] - rc[j][1], p[2] - rc[j][2]};
                    best = std::min(best, length3(dd));
                }
                if (best > kWatchRadius) continue;
                uint64_t key = key_of(id_of(e), uint8_t(k));
                watch_object(key, p, best, g_claims.count(key) != 0, now);
            }
        }
    for (auto it = g_watch.begin(); it != g_watch.end();) it = now - it->second.near_at > 3000 ? g_watch.erase(it) : ++it;
}

void maybe_log_stats()
{
    DWORD now = tick_now();
    if (!g_last_stats) g_last_stats = now;
    if (now - g_last_stats < 30000) return;
    g_last_stats = now;
    bool host = *g_is_host != 0;
    if (host && (g_h_claims || g_h_denied || g_h_batches || g_watch_lines))
        swlog("object push 30s (host): %llu bodies taken over by clients, %llu state batches, %llu steering ticks (largest "
              "error %.2f m), %llu handed back, %llu denied; %llu push watch lines",
              static_cast<unsigned long long>(g_h_claims), static_cast<unsigned long long>(g_h_batches),
              static_cast<unsigned long long>(g_h_steer), g_h_max_err, static_cast<unsigned long long>(g_h_done),
              static_cast<unsigned long long>(g_h_denied), static_cast<unsigned long long>(g_watch_lines));
    if (!host && (g_taken || g_taken_vehicles || g_released || g_denies_got || g_lost || g_no_joints))
        swlog("object push 30s (client): %llu objects and %llu vehicles claimed, %llu handed back (farthest moved %.2f m), "
              "%llu denied by the host, %llu removed while ours, %llu vehicles skipped without joints, %llu host targets "
              "dropped, %llu batches sent; %zu owned now",
              static_cast<unsigned long long>(g_taken), static_cast<unsigned long long>(g_taken_vehicles),
              static_cast<unsigned long long>(g_released), g_c_max_moved, static_cast<unsigned long long>(g_denies_got),
              static_cast<unsigned long long>(g_lost), static_cast<unsigned long long>(g_no_joints),
              static_cast<unsigned long long>(g_dropped_kin), static_cast<unsigned long long>(g_batches), g_units.size());
    if (!host) {
        char why[400];
        int used = 0;
        why[0] = 0;
        for (int i = 0; i < kRefCount; i++)
            if (g_refused[i]) used += snprintf(why + used, sizeof why - used, "%s%s %llu", used ? ", " : "", kRefusalNames[i],
                                               static_cast<unsigned long long>(g_refused[i]));
        if (used) swlog("object push 30s (client): touched but not claimed (ticks): %s", why);
        if (!g_touch_objects.empty() || !g_touch_vehicles.empty() || !g_touch_neither.empty())
            swlog("object push 30s (client): bodies touched (touch census): %zu loose objects, %zu vehicle bodies, %zu in neither list",
                  g_touch_objects.size(), g_touch_vehicles.size(), g_touch_neither.size());
        g_touch_objects.clear(), g_touch_vehicles.clear(), g_touch_neither.clear();
    }
    if (g_box_packets || g_box_applied || g_box_owned_skipped)
        swlog("pallet sync 30s (%s): %llu host packets, %llu unowned updates applied, %llu owned updates ignored; largest pre-update error %.3f m",
              host?"host":"client",(unsigned long long)g_box_packets,(unsigned long long)g_box_applied,
              (unsigned long long)g_box_owned_skipped,g_box_max_error);
    g_box_packets=g_box_applied=g_box_owned_skipped=0;g_box_max_error=0;
    if(g_force_samples || g_force_packets || g_force_applied || g_force_rejected)
        swlog("large vehicle push 30s (%s): %llu contact samples, %llu force packets, %llu host force applications, %llu rejected; largest force %.1f N",
              host?"host":"client",(unsigned long long)g_force_samples,(unsigned long long)g_force_packets,
              (unsigned long long)g_force_applied,(unsigned long long)g_force_rejected,g_force_max_n);
    g_force_samples=g_force_packets=g_force_applied=g_force_rejected=0;g_force_max_n=0;
    memset(g_refused, 0, sizeof g_refused);
    g_taken = g_taken_vehicles = g_released = g_denies_got = g_lost = g_dropped_kin = g_batches = g_no_joints = 0;
    g_h_claims = g_h_steer = g_h_denied = g_h_done = g_h_batches = g_watch_lines = 0;
    g_h_max_err = g_c_max_moved = 0;
}

// Under g_mu (taken by the caller: a fault here must not leave it held).
void tick_safe(uint8_t* world)
{
    __try {
        if (*g_is_host) host_tick(world);
        else client_tick(world);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool logged = false;
        if (!logged) swlog("object push: a read faulted (left alone this tick)");
        logged = true;
    }
}

void __fastcall pre_step_detour(uint8_t* world, uint32_t tick)
{
    g_update_thread = GetCurrentThreadId();
    if (world) {
        std::lock_guard lock(g_mu);
        tick_safe(world);
        DWORD now = tick_now();
        g_client_physics_at.store(g_on && g_is_host && !*g_is_host && g_host_cap_at &&
                                 now - g_host_cap_at < kHostCapFreshMs ? now : 0, std::memory_order_release);
        maybe_log_stats();
    }
    g_orig_pre(world, tick);
}

void __fastcall post_kinematic_detour(uint8_t* scene, uint8_t* so, const double* transform)
{
    // The pre-step's replays run on the update thread right after client_tick, the only place g_units changes, so no
    // lock there (the network thread never touches g_units). Posts from any other thread pass untouched.
    if (so && !*g_is_host && GetCurrentThreadId() == g_update_thread && is_owned_so(so)) {
        for(auto& u:g_units)for(auto& p:u.parts)if(p.so==so) {
            bool valid=true;for(int i=0;i<16;i++)valid=valid && std::isfinite(transform[i]);
            if(valid){memcpy(p.replay,transform,sizeof p.replay);p.replay_valid=true;}
        }
        g_dropped_kin++;
        return;
    }
    g_orig_kin(scene, so, transform);
}

void __fastcall post_transform_detour(uint8_t* scene, uint8_t* so, const double* transform)
{
    if (so && !*g_is_host && GetCurrentThreadId() == g_update_thread && is_owned_so(so)) {
        for(auto& u:g_units)for(auto& p:u.parts)if(p.so==so) {
            bool valid=true;for(int i=0;i<16;i++)valid=valid && std::isfinite(transform[i]);
            if(valid){memcpy(p.replay,transform,sizeof p.replay);p.replay_valid=true;}
        }
        g_dropped_kin++;
        return;
    }
    g_orig_transform(scene, so, transform);
}

bool install()
{
    g_on = GetPrivateProfileIntW(L"push", L"objects", 1, settings_ini()) != 0;
    g_vehicles_on = GetPrivateProfileIntW(L"push", L"vehicles", 1, settings_ini()) != 0;
    g_outside_ok = GetPrivateProfileIntW(L"push", L"outside_joints", 0, settings_ini()) != 0;
    if (!g_on) {
        swlog("object push: off in settings.ini");
        return true;
    }
    g_is_host = game_ptr<const volatile uint8_t*>(kIsHost);
    g_set_flags = game_ptr<SetFlags_t>(kSetCollisionFlags);
    g_set_masks = game_ptr<SetMasks_t>(kSetCollisionMasks);
    g_set_lin = game_ptr<SetVec_t>(kSetLinearVelocity);
    g_set_ang = game_ptr<SetVec_t>(kSetAngularVelocity);
    g_large_push_on=GetPrivateProfileIntW(L"push",L"large_vehicles",1,settings_ini())!=0;
    g_add_force=game_ptr<AddForce_t>(0x1401a7470);
    if(!g_add_force)g_large_push_on=false;
    if (!g_is_host || !g_set_flags || !g_set_masks || !g_set_lin || !g_set_ang) {
        g_on = false;
        return false;
    }
    if (!hook_game(kPostTransform, reinterpret_cast<void*>(&post_transform_detour), reinterpret_cast<void**>(&g_orig_transform)) ||
        !hook_game(kPostKinematic, reinterpret_cast<void*>(&post_kinematic_detour), reinterpret_cast<void**>(&g_orig_kin)) ||
        !hook_game(kPreStep, reinterpret_cast<void*>(&pre_step_detour), reinterpret_cast<void**>(&g_orig_pre))) {
        g_on = false;
        return false;
    }
    swlog("object push: on (a client pushes movable boxes, loose objects%s it touches itself, the host follows; \"object push 30s\" lines)",
          g_vehicles_on ? " and small vehicles" : "");
    return true;
}

HookRegistrar reg{{"object push", /*needs_known_build=*/true, &install}};

} // namespace

// --- network thread (redundancy.cpp) ----------------------------------------------------------------------------------

bool push_enabled() { return g_on; }

bool push_hosting() { return g_on && g_is_host && *g_is_host; }

void push_on_host_cap()
{
    std::lock_guard lock(g_mu);
    g_host_cap_at = tick_now();
}

uint32_t push_client_payload(uint8_t* out, uint32_t cap)
{
    std::lock_guard lock(g_mu);
    if (g_out_seq == g_out_sent_seq) return 0;
    g_out_sent_seq = g_out_seq;
    uint32_t n = pack_states(g_out_seq, g_out, out, cap);
    g_batches += n != 0;
    return n;
}

void push_on_states(int peer, const uint8_t* data, uint32_t size)
{
    uint32_t seq;
    std::vector<WireState> states;
    if (!unpack_states(data, size, seq, states)) return;
    DWORD now = tick_now();
    std::lock_guard lock(g_mu);
    // Late or repeated batch (unreliable channel). After 2 s of silence any number starts again: a client that restarted
    // its game numbers from 1.
    PeerSeq& ps = g_peer_seq[peer];
    if (ps.at && now - ps.at < 2000 && int32_t(seq - ps.seq) <= 0) return;
    ps.seq = seq, ps.at = now;
    g_h_batches++;
    for (const WireState& s : states) {
        uint64_t key = key_of(s.id, s.kind);
        auto it = g_claims.find(key);
        if (it != g_claims.end() && it->second.peer != peer && now - it->second.at <= kClaimStaleMs) {
            deny(peer, s.id, s.kind); // someone else has it
            continue;
        }
        if ((it == g_claims.end() || it->second.peer != peer) && (s.flags & kStateReleasing)) continue; // handed back already
        if (it == g_claims.end() || it->second.peer != peer) {
            Claim c;
            c.peer = peer;
            it = g_claims.insert_or_assign(key, c).first;
        }
        it->second.seq = seq;
        it->second.s = s;
        it->second.at = now;
    }
}

uint32_t push_take_denies(int peer, uint8_t* out, uint32_t cap)
{
    std::lock_guard lock(g_mu);
    auto it = g_deny_out.find(peer);
    if (it == g_deny_out.end() || it->second.empty()) return 0;
    uint32_t n = std::min<uint32_t>(uint32_t(it->second.size()), cap / uint32_t(sizeof(WireDeny)));
    memcpy(out, it->second.data(), n * sizeof(WireDeny));
    it->second.erase(it->second.begin(), it->second.begin() + n);
    return n * uint32_t(sizeof(WireDeny));
}

void push_on_denies(const uint8_t* data, uint32_t size)
{
    std::lock_guard lock(g_mu);
    for (uint32_t off = 0; off + sizeof(WireDeny) <= size; off += sizeof(WireDeny)) {
        WireDeny d;
        memcpy(&d, data + off, sizeof d);
        g_denied_in.push_back(d);
    }
}

// Physics Fixes: the native client task hard-codes a 60 Hz step, regardless of the host's rate.
// A walking client that simulates pushable props needs host-like collision substeps.
extern "C" __declspec(dllexport) int __cdecl swml_client_push_ready() {
    DWORD at = g_client_physics_at.load(std::memory_order_acquire);
    return at && tick_now() - at < 1000;
}

// Network thread: drain the current host snapshot in MTU-sized chunks. Each object carries the batch sequence;
// receivers order per object, so out-of-order chunks do not hide unrelated objects.
uint32_t push_host_payload(uint8_t* out,uint32_t cap) {
    std::lock_guard lock(g_mu);
    if(!g_on || !g_is_host || !*g_is_host || tick_now()-g_host_box_at>kHostBoxFreshMs ||
       g_host_box_cursor>=g_host_box_out.size() || cap<sizeof(WireHeader)+sizeof(WireState))return 0;
    size_t n=std::min({g_host_box_out.size()-g_host_box_cursor,size_t(kMaxWire),
                      (cap-sizeof(WireHeader))/sizeof(WireState)});
    WireHeader h{g_host_box_seq,uint16_t(n)};memcpy(out,&h,sizeof h);
    memcpy(out+sizeof h,g_host_box_out.data()+g_host_box_cursor,n*sizeof(WireState));
    g_host_box_cursor+=n;++g_box_packets;return uint32_t(sizeof h+n*sizeof(WireState));
}
void push_on_host_states(const uint8_t* data,uint32_t size) {
    uint32_t seq;std::vector<WireState> states;
    if(!unpack_states(data,size,seq,states))return;
    DWORD now=tick_now();std::lock_guard lock(g_mu);
    if(!g_on || !g_is_host || *g_is_host)return;
    for(const auto& s:states) {
        if(s.kind!=0)continue;
        double norm=0;bool valid=true;
        for(double x:s.p)valid=valid && std::fabs(x)<1e6;
        for(float x:s.q)norm+=double(x)*x;
        if(!valid || norm<.5 || norm>1.5)continue;
        auto key=key_of(s.id,s.kind);auto it=g_host_boxes.find(key);
        if(it!=g_host_boxes.end() && now-it->second.at<=kHostBoxFreshMs && int32_t(seq-it->second.seq)<=0)continue;
        if(it==g_host_boxes.end() && g_host_boxes.size()>=256)continue;
        g_host_boxes.insert_or_assign(key,HostBoxState{s,seq,now});
    }
    ++g_box_packets;
}

uint32_t push_client_force_payload(uint8_t* out,uint32_t cap) {
    std::lock_guard lock(g_mu);
    if(!g_large_push_on || !g_force_out_seq || g_force_sent_seq==g_force_out_seq)return 0;
    uint32_t bytes=uint32_t(sizeof(WireHeader)+g_force_out.size()*sizeof(WireVehicleForce));if(bytes>cap)return 0;
    WireHeader h{g_force_out_seq,uint16_t(g_force_out.size())};memcpy(out,&h,sizeof h);
    if(!g_force_out.empty())memcpy(out+sizeof h,g_force_out.data(),g_force_out.size()*sizeof(WireVehicleForce));
    g_force_sent_seq=g_force_out_seq;++g_force_packets;return bytes;
}
void push_on_forces(int peer,const uint8_t* data,uint32_t bytes) {
    if(bytes<sizeof(WireHeader))return;WireHeader h;memcpy(&h,data,sizeof h);
    if(h.count>16 || bytes!=sizeof h+h.count*sizeof(WireVehicleForce))return;
    std::vector<WireVehicleForce> forces(h.count);if(h.count)memcpy(forces.data(),data+sizeof h,h.count*sizeof(WireVehicleForce));
    for(const auto& f:forces) {
        if(f.body>=4096)return;
        for(int k=0;k<3;k++)if(!std::isfinite(f.point[k]) || std::fabs(f.point[k])>1000 || !std::isfinite(f.force[k]) || std::fabs(f.force[k])>100000)return;
    }
    DWORD now=tick_now();std::lock_guard lock(g_mu);if(!g_large_push_on || !g_is_host || !*g_is_host)return;
    auto& p=g_peer_forces[peer];if(p.at && now-p.at<=kLargePushFreshMs && int32_t(h.seq-p.seq)<=0)return;
    p.seq=h.seq;p.at=now;p.forces=std::move(forces);++g_force_packets;
}
