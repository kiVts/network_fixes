
#include "hooks.h"
#include "log.h"
#include "net_shared.h"

#include "libdeflate.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {

// --- Steamworks bits we need (values from steamnetworkingtypes.h) -------------------------
constexpr int kResultOK = 1;
constexpr int kResultLimitExceeded = 25;
constexpr int kStateConnected = 3;

constexpr int kCfgSendBufferSize = 9;
constexpr int kCfgSendRateMin = 10;
constexpr int kCfgSendRateMax = 11;
constexpr int kCfgNagleTime = 12;
constexpr int kCfgMtuPacketSize = 32; // MTU_PacketSize: largest UDP payload Steam sends (default 1300)
constexpr int kCfgRecvBufferSize = 47;
constexpr int kCfgRecvBufferMessages = 48;
constexpr int kCfgRecvMaxMessageSize = 49;
constexpr int kScopeGlobal = 1;
// Direct connections (steamnetworkingtypes.h). ICE_Enable: -1 default, 0 off, 1 relay, 2 private, 4 public, all.
constexpr int kCfgIceEnable = 104;
constexpr int kCfgIcePenalty = 105;
constexpr int kCfgSdrPenalty = 106;
constexpr int32_t kIceEnableAll = 0x7fffffff;
// Steam's own P2P / ICE log (LogLevel_P2PRendezvous) through SetDebugOutputFunction: 5 = Msg, 6 = Verbose.
constexpr int kCfgLogP2P = 17;
constexpr int kCfgStunList = 103; // P2P_STUN_ServerList (string, comma-separated host:port)
constexpr int kDbgMsg = 5, kDbgWarning = 4;
// SteamNetConnectionInfo_t (696 bytes): m_addrRemote +148 (16-byte IPv6 / v4-mapped, port +164), m_idPOPRelay +172,
// m_nFlags +440 (Relayed = 16, Fast = 8).
constexpr size_t kInfoSize = 696, kInfoAddr = 148, kInfoPort = 164, kInfoPopRelay = 172, kInfoFlags = 440;
constexpr int kFlagRelayed = 16;

using GetIface_t = void*(__cdecl*)();
using SetGlobalInt32_t = bool(__cdecl*)(void* utils, int value_id, int32_t value);
using GetConfigValue_t = int(__cdecl*)(void* utils, int value_id, int scope, intptr_t scope_obj, int* out_type,
                                       void* result, size_t* result_size);

struct RealTimeStatus {
    int state, ping;
    float quality_local, quality_remote, out_packets, out_bytes, in_packets, in_bytes;
    int send_rate, pending_unreliable, pending_reliable, sent_unacked_reliable;
    long long queue_time_usec;
    uint32_t reserved[16];
};
using GetSessionInfo_t = int(__fastcall*)(void* self, const void* identity, void* info, RealTimeStatus* status);
constexpr size_t kMsgTimeReceived = 0xa0;

// --- settings (settings.ini, [net]) ---------------------------------------------------
int g_send_buffer_kb = 0;
int g_recv_buffer_kb = 0;
bool g_rate_control = true;
int g_fixed_rate_kb = 0; // only used with rate_control=0
double g_start_rate = 256 * 1024.0, g_min_rate = 32 * 1024.0, g_max_rate = 8192 * 1024.0;
int g_pace_ms = 40; // 0 = no tick priority
int g_nagle_us = 0; // -1 = Steam default (5000)
// Largest UDP payload Steam sends (0 = Steam's 1300). 2026-10-31 21:20: a direct route over Tailscale (MTU 1280, IPv6)
// passed 1232-byte payloads and lost every bigger one (IPv6 isn't fragmented on the way, and the fragments the sender
// makes didn't get through): Steam's 1300-byte data packets all vanished while its small pings / acks arrived, so
// the connection looked alive and the world download delivered 0 KB/s until the join timed out. 1200 fits any path
// that carries IPv6's minimum 1280 (1232 + 48 bytes of headers), relay or direct.
int g_mtu = 1200;
bool g_direct = false;  // allow ICE (direct UDP) for every peer, not only Steam friends / the Steam client's default
int g_sdr_penalty = 10; // ms added to the relay's ping when Steam picks a route: direct wins unless it's > 10 ms worse
int g_ice_log = 1;      // Steam's rendezvous / ICE messages into network_fixes.log ("steam:" lines): 0 off, 1 normal, 2 verbose (route choice)
char g_stun[512] = ""; // [net] stun_servers; "" = keep Steam's own
bool g_redundancy = true;
bool g_log_channels = true; // [mp] channels
constexpr int64_t kPaceMinBytes = 32 * 1024;

void* g_msgs = nullptr;
void* g_utils = nullptr;
void* g_vtable[16];
Send_t g_orig_send = nullptr;
Receive_t g_orig_recv = nullptr;
GetSessionInfo_t g_get_info = nullptr;
SetGlobalInt32_t g_set_int = nullptr;

// --- peers ----------------------------------------------------------------------------
// SteamNetworkingIdentity is 136 bytes; compared on type + size + SteamID or IPv6/port (first 32 bytes), never logged.
constexpr size_t kIdentitySize = 136;
constexpr int kMaxPeers = 16;
constexpr int kTicksPerWindow = 5; // 200 ms ticks -> 1 s measurement windows
constexpr int kBwWindows = 6;      // max filter over the last 6 busy windows

struct Peer {
    uint8_t id[kIdentitySize];
    bool used = false;
    uint64_t added = 0;  // bytes Steam accepted since the last tick (guarded by g_mutex)
    uint32_t held = 0;   // bulk sends held back by pacing since the last tick (guarded by g_mutex)
    uint32_t held_window = 0; // controller thread: held sends since the last status line
    // controller-thread state
    bool live = false, have_last = false;
    int64_t last_outstanding = 0;
    int64_t last_queued = 0, last_unacked = 0; // for the rate measurement (in-flight allowance applied to both)
    double win_delivered = 0, win_dt = 0;
    double win_busy_delivered = 0, win_busy_dt = 0;
    int win_ticks = 0, win_busy_ticks = 0;
    bool warm = true; // the next busy window is this peer's first after an idle one
    double bw[kBwWindows] = {};
    int bw_next = 0;
    double d_last = 0;
    bool in_transfer = false;
    double xfer_bytes = 0, xfer_time = 0;
    // 1.11.11: windows the controller doesn't measure. Stalled: the peer sent nothing on channel 0 for > kGapMs during
    // the window (frozen game, timing out, gone); its Steam queue stops moving, which read as a 0-38 KB/s path and set
    // the global rate to the 32 KB/s floor (2026-11-10 03:57). Drain: the backlog emptied before the window ended; the
    // queue's drain counts as that window's delivery and doubled the rate at the end of a 545 KB/s transfer
    // (issue_1 16:34:43, 1024 -> 2048 KB/s).
    bool win_stalled = false, last_tick_busy = false;
    uint32_t skipped_stalled = 0, skipped_drain = 0; // since the last status line
    RealTimeStatus st{};
    // "net channels 5s" (guarded by g_mutex): accepted sends and received messages per channel, and the gaps between
    // channel-0 arrivals by Steam's receive time (host -> player: one per host tick).
    uint32_t out_n[16] = {}, in_n[16] = {};
    uint64_t out_b[16] = {}, in_b[16] = {};
    int64_t ch0_last = 0, ch0_gap = 0;
    uint32_t ch0_gaps100 = 0;
    // "net gap": channel 0 silent for > kGapMs (game updates from the host / inputs from a player)
    bool gap_logged = false;   // the current silence was already reported while it lasted
    int64_t gap_ended_ms = 0;  // a reported (or long) silence just ended: its length, for the controller to log
    ULONGLONG gap_last_log = 0;
    uint32_t gaps_suppressed = 0;
};
constexpr int64_t kGapMs = 500;
std::mutex g_mutex;
Peer g_peers[kMaxPeers];

Peer* find_peer_locked(const void* identity)
{
    Peer* slot = nullptr;
    for (Peer& p : g_peers) {
        if (p.used && memcmp(p.id, identity, 32) == 0) return &p;
        if (!p.used && !slot) slot = &p;
    }
    if (!slot) {
        for (Peer& p : g_peers) {
            if (!p.live) { // reuse a closed session's slot
                slot = &p;
                break;
            }
        }
    }
    if (!slot) return nullptr;
    *slot = Peer{};
    memcpy(slot->id, identity, kIdentitySize);
    slot->used = true;
    return slot;
}

// --- latency breakdown ("latency 5s") ----------------------------------------------------
// Where a round trip goes, besides the wire. Steam's ping is the wire RTT (plus Steam's own processing); on top of it
// a message waits in Steam's receive queue until the game's network thread polls (recv wait: Steam's receive time
// m_usecTimeReceived -> the game takes it, same clock via GetLocalTimestamp), and on the sending side it waits for
// the game's next send of that kind (ch0 send gap: time between the game's tick sends) and in Steam's send queue
// (queue_time_usec). Host and client on one PC: ping ~0, so whatever the in-game ping shows beyond that is these.
using GetTimestamp_t = int64_t(__cdecl*)(void* utils);
GetTimestamp_t g_now_usec = nullptr;
struct LatStat {
    uint64_t n = 0;
    double sum = 0, max = 0;
    uint32_t hist[64] = {}; // 0.5 ms buckets, last = 31.5+ ms
    void add(double ms)
    {
        n++;
        sum += ms;
        max = std::max(max, ms);
        hist[std::min(63, static_cast<int>(ms * 2.0))]++;
    }
    double pct(double q) const
    {
        uint64_t want = static_cast<uint64_t>(q * double(n)), seen = 0;
        for (int i = 0; i < 64; i++)
            if ((seen += hist[i]) > want) return (i + 1) * 0.5;
        return 32;
    }
};
std::mutex g_lat_mutex;
LatStat g_wait_ch0, g_wait_other, g_poll_gap, g_send_gap;
int64_t g_last_poll = 0, g_last_ch0_send = 0;

void lat_on_received(int channel, void** out, int n)
{
    if (!g_now_usec || n <= 0 || !out) return;
    int64_t now = g_now_usec(g_utils);
    std::lock_guard lock(g_lat_mutex);
    for (int i = 0; i < n; i++) {
        if (!out[i]) continue;
        int64_t got = *reinterpret_cast<const int64_t*>(static_cast<const uint8_t*>(out[i]) + kMsgTimeReceived);
        double ms = double(now - got) / 1000.0;
        if (ms < 0 || ms > 10000) continue;
        (channel == 0 ? g_wait_ch0 : g_wait_other).add(ms);
    }
}

void lat_on_poll(int channel)
{
    if (!g_now_usec || channel != 0) return;
    int64_t now = g_now_usec(g_utils);
    std::lock_guard lock(g_lat_mutex);
    if (g_last_poll) g_poll_gap.add(double(now - g_last_poll) / 1000.0);
    g_last_poll = now;
}

void lat_on_ch0_send()
{
    if (!g_now_usec) return;
    int64_t now = g_now_usec(g_utils);
    std::lock_guard lock(g_lat_mutex);
    if (g_last_ch0_send && now - g_last_ch0_send > 500) g_send_gap.add(double(now - g_last_ch0_send) / 1000.0); // one tick's sends to several peers count once
    if (now - g_last_ch0_send > 500) g_last_ch0_send = now;
}

std::string lat_line(const char* name, const LatStat& s)
{
    char b[160];
    if (!s.n) snprintf(b, sizeof b, "  %s -", name);
    else snprintf(b, sizeof b, "  %s avg %.1f p50 %.1f p95 %.1f max %.1f ms (%llu)", name, s.sum / s.n, s.pct(0.5), s.pct(0.95), s.max,
                  static_cast<unsigned long long>(s.n));
    return b;
}

// Which route each peer's connection takes: logged when it changes and in every "latency 5s" line.
// (controller thread)
int g_path_flags[kMaxPeers];
bool g_path_known[kMaxPeers];

std::string peer_path(int i, const RealTimeStatus& st)
{
    alignas(8) uint8_t info[1024] = {};
    RealTimeStatus dummy{};
    if (g_get_info(g_msgs, g_peers[i].id, info, &dummy) != kStateConnected) return "?";
    int flags = *reinterpret_cast<const int*>(info + kInfoFlags);
    uint32_t pop = *reinterpret_cast<const uint32_t*>(info + kInfoPopRelay);
    char popname[5] = {char(pop >> 24), char(pop >> 16), char(pop >> 8), char(pop), 0};
    for (char& c : popname)
        if (c && (c < 32 || c > 126)) c = '?';
    // Remote address: only whether it's local/LAN, never the address itself.
    const uint8_t* a = info + kInfoAddr;
    bool v4 = !memcmp(a, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);
    const char* where = !v4 ? "remote" : a[12] == 127 ? "this PC" : (a[12] == 10 || (a[12] == 192 && a[13] == 168) || (a[12] == 172 && (a[13] & 0xf0) == 16)) ? "LAN" : "internet";
    char b[200];
    if (flags & kFlagRelayed)
        snprintf(b, sizeof b, "RELAYED via Steam (relay %s)", pop ? popname : "?");
    else
        snprintf(b, sizeof b, "DIRECT (%s)", where);
    if (!g_path_known[i] || ((g_path_flags[i] ^ flags) & kFlagRelayed)) {
        swlog("net path peer%d: %s, ping %d ms, flags 0x%x", i + 1, b, st.ping, flags); // no description: it names the Steam ID
        g_path_known[i] = true;
        g_path_flags[i] = flags;
    }
    return b;
}

void log_latency()
{
    std::string line;
    {
        std::lock_guard lock(g_lat_mutex);
        if (!g_wait_ch0.n && !g_wait_other.n && !g_send_gap.n) return;
        line = lat_line("recv wait ch0", g_wait_ch0) + lat_line("other", g_wait_other) + lat_line("| game poll gap", g_poll_gap) +
               lat_line("| ch0 send gap", g_send_gap);
        g_wait_ch0 = g_wait_other = g_poll_gap = g_send_gap = LatStat{};
    }
    for (int i = 0; i < kMaxPeers; i++) {
        Peer& p = g_peers[i];
        if (!p.used) continue;
        RealTimeStatus st{};
        if (g_get_info(g_msgs, p.id, nullptr, &st) != kStateConnected) continue;
        char b[160];
        snprintf(b, sizeof b, " | peer%d Steam ping %d ms (%s), send queue %.1f ms", i + 1, st.ping, peer_path(i, st).c_str(),
                 double(st.queue_time_usec) / 1000.0);
        line += b;
    }
    swlog("latency 5s:%s", line.c_str());
}

// Game network thread: Steam's receive time of each tick update, for the client playout buffer.
// Also the redundant tick copies: our channel is read when the game polls channel 15 (first in its loop),
// and channel 0 is handed to the game as the merged stream (redundancy.cpp).
std::atomic<uint64_t> g_bulk_in{0}; // bytes received on channels 1..7 (world, vehicles, mod files), for mp_watch's join line

void count_received(int channel, void** out, int n)
{
    if (n > 0 && out && channel >= 1 && channel <= 7) {
        uint64_t b = 0;
        for (int i = 0; i < n; i++)
            if (out[i]) b += uint32_t(*reinterpret_cast<const int*>(static_cast<const uint8_t*>(out[i]) + 0x8));
        g_bulk_in.fetch_add(b, std::memory_order_relaxed);
    }
    // Channel 0 is always tracked: the rate controller needs to know which peers still answer (Peer::win_stalled).
    if (n <= 0 || !out || channel < 0 || channel > 15 || (!g_log_channels && channel != 0)) return;
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < n; i++) {
        const uint8_t* m = static_cast<const uint8_t*>(out[i]);
        if (!m) continue;
        Peer* p = find_peer_locked(m + 0x10); // SteamNetworkingMessage_t::m_identityPeer
        if (!p) continue;
        if (g_log_channels) {
            p->in_n[channel]++;
            p->in_b[channel] += uint32_t(*reinterpret_cast<const int*>(m + 0x8));
        }
        if (channel == 0) {
            int64_t t = *reinterpret_cast<const int64_t*>(m + kMsgTimeReceived);
            if (p->ch0_last && t > p->ch0_last) {
                int64_t gap = t - p->ch0_last;
                p->ch0_gap = std::max(p->ch0_gap, gap);
                if (gap > 100000) p->ch0_gaps100++;
                if (gap / 1000 > kGapMs || p->gap_logged) p->gap_ended_ms = gap / 1000;
            }
            p->ch0_last = t;
        }
    }
}

int __fastcall counted_send(void* self, const void* identity, const void* data, uint32_t size, int flags, int channel)
{
    int r = g_orig_send(self, identity, data, size, flags, channel);
    if (r == kResultOK && identity && channel >= 0 && channel < 16) {
        std::lock_guard lock(g_mutex);
        if (Peer* p = find_peer_locked(identity)) {
            p->added += size;
            p->out_n[channel]++, p->out_b[channel] += size;
        }
    }
    return r;
}

int __fastcall counted_recv(void* self, int channel, void** out, int max)
{
    int n = g_orig_recv(self, channel, out, max);
    count_received(channel, out, n);
    return n;
}

int __fastcall recv_detour(void* self, int channel, void** out, int max)
{
    if (channel == 15 && dup_enabled()) dup_poll(self, &counted_recv, &counted_send);
    lat_on_poll(channel);
    int n = channel == 0 && dup_enabled() ? dup_receive_ch0(self, out, max, g_orig_recv) : g_orig_recv(self, channel, out, max);
    lat_on_received(channel, out, n);
    count_received(channel, out, n);
    if (channel == 0 && out) {
        for (int i = 0; i < n; i++) {
            if (out[i]) playout_on_arrival(*reinterpret_cast<const int64_t*>(static_cast<const uint8_t*>(out[i]) + kMsgTimeReceived));
        }
    }
    return n;
}

// Game network thread. Channel 0 = tick updates (and the join snapshot), always sent at once.
bool hold_bulk(const void* identity, uint32_t size)
{
    RealTimeStatus st{};
    if (g_get_info(g_msgs, identity, nullptr, &st) != kStateConnected) return false;
    int64_t pending = st.pending_reliable;
    if (pending <= 0) return false; // never hold when Steam has nothing queued: the transfer always moves
    int64_t budget = std::max(kPaceMinBytes, static_cast<int64_t>(st.send_rate) * g_pace_ms / 1000);
    return pending + size > budget;
}

bool g_measure_compress = true;
std::atomic<uint64_t> g_cz_n{0}, g_cz_in{0}, g_cz_out1{0}, g_cz_out6{0}, g_cz_us1{0}, g_cz_us6{0};
uint32_t g_cz_count = 0; // game network thread

void sample_compress(const void* data, uint32_t size)
{
    if (size < 512 || (++g_cz_count & 31)) return;
    thread_local libdeflate_compressor* c1 = libdeflate_alloc_compressor(1);
    thread_local libdeflate_compressor* c6 = libdeflate_alloc_compressor(6);
    thread_local std::vector<uint8_t> out;
    if (!c1 || !c6) return;
    out.resize(libdeflate_zlib_compress_bound(c6, size));
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    size_t n1 = libdeflate_zlib_compress(c1, data, size, out.data(), out.size());
    QueryPerformanceCounter(&t1);
    size_t n6 = libdeflate_zlib_compress(c6, data, size, out.data(), out.size());
    QueryPerformanceCounter(&t2);
    if (!n1 || !n6) return;
    g_cz_n++;
    g_cz_in += size;
    g_cz_out1 += n1;
    g_cz_out6 += n6;
    g_cz_us1 += uint64_t((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    g_cz_us6 += uint64_t((t2.QuadPart - t1.QuadPart) * 1000000 / f.QuadPart);
}

void log_compress()
{
    uint64_t n = g_cz_n.exchange(0), in = g_cz_in.exchange(0), o1 = g_cz_out1.exchange(0), o6 = g_cz_out6.exchange(0),
             u1 = g_cz_us1.exchange(0), u6 = g_cz_us6.exchange(0);
    if (!n || !in) return;
    swlog("tick compress 5s: %llu samples of channel 0 (avg %.0f B): level 1 -> %.0f%% of the size in %.0f us, level 6 -> %.0f%% "
          "in %.0f us",
          static_cast<unsigned long long>(n), double(in) / double(n), 100.0 * double(o1) / double(in), double(u1) / double(n),
          100.0 * double(o6) / double(in), double(u6) / double(n));
}

int __fastcall send_detour(void* self, const void* identity, const void* data, uint32_t size, int flags, int channel)
{
    if (g_pace_ms > 0 && channel != 0 && identity && hold_bulk(identity, size)) {
        std::lock_guard lock(g_mutex);
        if (Peer* p = find_peer_locked(identity)) p->held++;
        return kResultLimitExceeded; // the game keeps it queued and retries on its next loop
    }
    int r = g_orig_send(self, identity, data, size, flags, channel);
    if (r == kResultOK && identity) {
        {
            std::lock_guard lock(g_mutex);
            if (Peer* p = find_peer_locked(identity)) {
                p->added += size;
                if (channel >= 0 && channel < 16) p->out_n[channel]++, p->out_b[channel] += size;
            }
        }
        if (channel == 0) lat_on_ch0_send();
        if (channel == 0 && g_measure_compress && data) sample_compress(data, size);
        if (channel == 0 && dup_enabled()) dup_on_sent(self, identity, data, size, &counted_send);
    }
    return r;
}

// --- rate control (controller thread only) ------------------------------------------------
enum class Mode { startup, probe_up, cruise };
const char* mode_name(Mode m) { return m == Mode::startup ? "startup" : m == Mode::probe_up ? "probe-up" : "cruise"; }
constexpr double kCruiseGain[] = {1.25, 0.85, 1, 1, 1, 1};
constexpr int kCruisePhases = sizeof(kCruiseGain) / sizeof(kCruiseGain[0]);

Mode g_mode = Mode::startup;
int g_phase = 0;
double g_rate = 0;        // last applied
double g_window_rate = 0; // in force during the window being measured
int g_idle_windows = 0;
double g_session_start = 0; // the start rate (start_rate()), restored after a player leaves / a long idle
bool g_peer_left = false;   // a connected peer's session ended since the last idle window

void apply_rate(double rate, const char* why)
{
    rate = std::clamp(rate, g_min_rate, g_max_rate);
    if (g_rate > 0 && std::abs(rate - g_rate) < g_rate * 0.02) return;
    int32_t v = static_cast<int32_t>(rate);
    // Keep min <= max at every step: lowering sets min first, raising sets max first.
    bool ok = rate < g_rate ? g_set_int(g_utils, kCfgSendRateMin, v) && g_set_int(g_utils, kCfgSendRateMax, v)
                            : g_set_int(g_utils, kCfgSendRateMax, v) && g_set_int(g_utils, kCfgSendRateMin, v);
    if (!ok) swlog("net: setting send rate %d KB/s FAILED", v / 1024);
    else if (why) swlog("net: send rate %.0f -> %.0f KB/s (%s: %s)", g_rate / 1024, rate / 1024, mode_name(g_mode), why);
    g_rate = rate;
}

double best_bw(const Peer& p)
{
    double m = 0;
    for (double b : p.bw) m = std::max(m, b);
    return m;
}

// A 1 s window is complete: pick the rate for the next one.
void save_capacity(double bw, bool measured);

void end_window()
{
    int busy = 0, counted = 0;
    bool kept_up = true, probe_ok = true;
    double bw = 1e18;
    for (Peer& p : g_peers) {
        if (!p.used || !p.live || p.win_ticks == 0) continue;
        p.d_last = p.win_dt > 0 ? p.win_delivered / p.win_dt : 0;
        // Backlogged for most of the window: what got delivered while busy is what the path carries at this rate.
        bool was_busy = p.win_busy_ticks * 2 > p.win_ticks;
        if (was_busy && p.win_stalled) {
            busy++; // a peer that stopped answering says nothing about the path: hold
            p.skipped_stalled++;
        } else if (was_busy && !p.last_tick_busy) {
            busy++; // the backlog drained inside the window: its delivery includes the drain, hold
            p.skipped_drain++;
        } else if (was_busy) {
            busy++;
            double d = p.win_busy_dt > 0 ? p.win_busy_delivered / p.win_busy_dt : 0;
            bool full = d >= 0.8 * g_window_rate;
            // A transfer's first window (after an idle one) may still be ramping up: it can only say "kept up".
            if (!p.warm || full) {
                counted++;
                p.bw[p.bw_next] = d;
                p.bw_next = (p.bw_next + 1) % kBwWindows;
                bw = std::min(bw, best_bw(p));
                kept_up = kept_up && full;
                probe_ok = probe_ok && d >= 0.9 * g_window_rate;
            }
        }
        p.warm = !was_busy || p.win_stalled; // after a stall, the first window back may still be ramping up
        p.win_stalled = false;
        p.win_delivered = p.win_dt = p.win_busy_delivered = p.win_busy_dt = 0;
        p.win_ticks = p.win_busy_ticks = 0;
    }
    if (busy > 0 && counted == 0) { // only warm-up / stalled / drain windows: hold
        g_idle_windows = 0;
        g_window_rate = g_rate;
        return;
    }
    if (busy == 0) {
        // Nothing queued: hold the rate. After a long pause the path may have changed, so the next
        // transfer probes upward again (gently, from the current rate).
        ++g_idle_windows;
        // A player left, or 30 s idle, with the rate under the session's start: whatever pulled it down (that player's
        // path, a stall) is gone, and the next join would crawl up x1.25/s from there (2026-11-10 03:59: 32 KB/s ->
        // a 1437 KB world in 11.2 s on a ~450 KB/s path). Start over from the session's start rate.
        if ((g_peer_left || g_idle_windows == 30) && g_rate < g_session_start * 0.98) {
            const char* why = g_peer_left ? "a player left" : "30 s idle";
            g_mode = Mode::startup;
            g_phase = 0;
            for (Peer& q : g_peers) memset(q.bw, 0, sizeof q.bw), q.bw_next = 0;
            apply_rate(g_session_start, why);
            g_window_rate = g_rate;
            g_peer_left = false;
            return;
        }
        g_peer_left = false;
        if (g_idle_windows == 30 && g_mode == Mode::cruise) g_mode = Mode::probe_up;
        g_window_rate = g_rate;
        return;
    }
    g_idle_windows = 0;
    if (g_mode != Mode::cruise) {
        if (kept_up) {
            apply_rate(g_rate * (g_mode == Mode::startup ? 2.0 : 1.25), "delivery keeps up");
        } else {
            g_mode = Mode::cruise;
            g_phase = 0;
            apply_rate(bw, "delivery fell behind, rate = measured capacity");
        }
    } else if (kCruiseGain[g_phase] > 1 && probe_ok) {
        g_mode = Mode::probe_up;
        apply_rate(g_rate * 1.25, "probe delivered in full");
    } else {
        g_phase = (g_phase + 1) % kCruisePhases;
        apply_rate(bw * kCruiseGain[g_phase], nullptr);
    }
    // Cruise: bw is the path's measured capacity. Startup / probe-up windows are no longer saved as a lower bound
    // (1.11.11): they read high and only ever raised the saved value, so a later session started at 2048 KB/s on a
    // ~450 KB/s path (2026-11-10 01:35). A completed transfer's average saves the lower bound instead (tick()).
    if (bw < 1e17 && g_mode == Mode::cruise) save_capacity(bw, true);
    g_window_rate = g_rate;
}

// The capacity the controller settled on, for the next session's start (this mod's status.ini, [rate]).
constexpr double kMaxRememberedStart = 2048 * 1024.0;
ULONGLONG g_last_save = 0;
double g_saved_kb = 0;

std::wstring status_ini() { return std::wstring(mod_dir()) + L"\\status.ini"; }

void save_capacity(double bw, bool measured)
{
    ULONGLONG now = GetTickCount64();
    double kb = bw / 1024;
    if (!measured && kb <= g_saved_kb) return; // a lower bound only ever raises it
    if (g_last_save && now - g_last_save < 30000 && measured) return;
    if (std::abs(kb - g_saved_kb) < g_saved_kb * 0.1) return;
    g_last_save = now;
    g_saved_kb = kb;
    wchar_t v[32];
    swprintf(v, 32, L"%d", static_cast<int>(kb));
    WritePrivateProfileStringW(L"rate", L"capacity_kb", v, status_ini().c_str());
}

// max(start_rate_kb, 0.75 x the remembered capacity), at most 2 MB/s and inside min..max.
double start_rate(double& remembered)
{
    remembered = GetPrivateProfileIntW(L"rate", L"capacity_kb", 0, status_ini().c_str()) * 1024.0;
    g_saved_kb = remembered / 1024;
    double r = std::max(g_start_rate, std::min(0.75 * remembered, kMaxRememberedStart));
    return std::clamp(r, g_min_rate, g_max_rate);
}

void tick(double dt)
{
    for (int i = 0; i < kMaxPeers; i++) {
        Peer& p = g_peers[i];
        uint64_t added;
        uint32_t held;
        int64_t ch0_last;
        uint8_t id[kIdentitySize];
        {
            std::lock_guard lock(g_mutex); // a send may be reusing this slot for a new peer
            if (!p.used) continue;
            added = p.added;
            held = p.held;
            ch0_last = p.ch0_last;
            p.added = 0;
            p.held = 0;
            memcpy(id, p.id, kIdentitySize);
        }
        RealTimeStatus st{};
        int state = g_get_info(g_msgs, id, nullptr, &st);
        const bool was_live = p.live;
        p.live = state > 0 && state < 4; // connecting / finding route / connected
        if (!p.live) {
            if (was_live) {
                g_peer_left = true; // end_window restores the start rate once everything is idle
                memset(p.bw, 0, sizeof p.bw), p.bw_next = 0;
                p.warm = true;
            }
            p.have_last = p.in_transfer = false;
            continue;
        }
        // Channel 0 from this peer (its inputs to the host / the host's ticks) silent for > kGapMs: it stopped
        // answering, and so did its acks. Unknown (no clock, nothing received yet) counts as answering.
        if (g_now_usec && ch0_last && (g_now_usec(g_utils) - ch0_last) / 1000 > kGapMs) p.win_stalled = true;
        p.st = st;
        // Unreliable sends count too: `added` includes them and they share the wire. Counting only the reliable
        // queue made a client whose upload was mostly unreliable measure ~7 KB/s "delivered" out of 32 KB/s on the
        // wire with no loss, and the cruise rate (gain x that) sat at the 32 KB/s floor for the whole session
        // (friend's Proton log 2026-10-22 02:37: 590 KB reliable + 1.9 MB unreliable queued, Steam said 78 s).
        int64_t queued = static_cast<int64_t>(st.pending_reliable) + st.pending_unreliable;
        int64_t outstanding = queued + st.sent_unacked_reliable;
        if (p.have_last) {
            // Unacked bytes up to about one round trip at the current rate are on the wire, not waiting: counting them
            // as outstanding made every transfer's first second look like a slow path (see the top of the file).
            double allow = g_window_rate * std::clamp(st.ping, 0, 1000) / 1000.0 * 1.25;
            auto waiting = [allow](int64_t q, int64_t unacked) { return double(q) + std::max(0.0, double(unacked) - allow); };
            const double raw =
                static_cast<double>(added) - (waiting(queued, st.sent_unacked_reliable) - waiting(p.last_queued, p.last_unacked));
            const double delivered = std::clamp(raw, 0.0, 4 * g_max_rate * dt);
            p.win_delivered += delivered;
            p.win_dt += dt;
            p.win_ticks++;
            // With pacing the backlog waits in the game's queues, so held sends count as busy too.
            p.last_tick_busy = st.pending_reliable > 0 || st.pending_unreliable > 0 || held > 0;
            if (p.last_tick_busy) {
                p.win_busy_ticks++;
                p.win_busy_delivered += delivered;
                p.win_busy_dt += dt;
            }
            // Unclamped: over a whole transfer the queue terms cancel out, so the sum is what actually got through
            // (clamping a tick where the queue grew faster than the game added counted its later drain twice).
            if (p.in_transfer) {
                p.xfer_bytes += raw;
                p.xfer_time += dt;
            }
        }
        p.have_last = true;
        p.last_outstanding = outstanding;
        p.last_queued = queued;
        p.last_unacked = st.sent_unacked_reliable;
        p.held_window += held;
        // Transfer summary: from a backlog of >= 128 KB (or sends held back) until it is (nearly) all delivered.
        if (!p.in_transfer && (outstanding >= 128 * 1024 || held > 0)) {
            p.in_transfer = true;
            p.xfer_bytes = p.xfer_time = 0;
        } else if (p.in_transfer && outstanding < 8 * 1024 && held == 0) {
            p.in_transfer = false;
            if (p.xfer_time > 0.5) {
                const double goodput = p.xfer_bytes / p.xfer_time;
                swlog("net peer%d: transfer done: %.0f KB in %.1f s = %.0f KB/s (send rate now %.0f KB/s, %.2fx that)", i + 1,
                      p.xfer_bytes / 1024, p.xfer_time, goodput / 1024, g_rate / 1024, goodput > 0 ? g_rate / goodput : 0.0);
                // What a whole transfer delivered is a true lower bound of the path (no in-flight guess in it).
                if (p.xfer_bytes >= 512 * 1024 && p.xfer_time >= 1.0) save_capacity(goodput, false);
            }
        }
    }
}

// The largest send queue Steam reports among the connected peers (ms; -1 none), for correction_rate.cpp, and among
// the peers in the game, for pacing.cpp (a joining player's queue holds the world snapshot). Stats thread.
std::atomic<int> g_queue_ms{-1}, g_queue_playing_ms{-1};
void update_queue()
{
    int worst = -1, playing = -1;
    for (int i = 0; i < kMaxPeers; i++) {
        uint8_t id[kIdentitySize];
        {
            std::lock_guard lock(g_mutex);
            if (!g_peers[i].used) continue;
            memcpy(id, g_peers[i].id, kIdentitySize);
        }
        RealTimeStatus st{};
        if (g_get_info(g_msgs, id, nullptr, &st) != kStateConnected) continue;
        int q = static_cast<int>(std::min<long long>(st.queue_time_usec / 1000, 1000000));
        worst = std::max(worst, q);
        if (dup_enabled() && dup_peer_reporting(id, 2000)) playing = std::max(playing, q); // g_mutex not held here
    }
    g_queue_ms.store(worst, std::memory_order_relaxed);
    g_queue_playing_ms.store(playing, std::memory_order_relaxed);
}

void log_status()
{
    for (int i = 0; i < kMaxPeers; i++) {
        Peer& p = g_peers[i];
        if (!p.used || !p.live) continue;
        const RealTimeStatus& st = p.st;
        uint32_t held = p.held_window;
        p.held_window = 0;
        const uint32_t stalled = p.skipped_stalled, drained = p.skipped_drain;
        p.skipped_stalled = p.skipped_drain = 0;
        // quiet when idle
        if (st.pending_reliable == 0 && st.pending_unreliable == 0 && st.sent_unacked_reliable < 16 * 1024 && held == 0 &&
            !stalled && !drained)
            continue;
        swlog("net peer%d: rate %.0f KB/s (%s, Steam reports %d)  delivered %.0f KB/s (best %.0f)  wire %.0f KB/s  "
              "backlog %d KB + %d KB unreliable + %d KB unacked  ping %d ms  quality %.2f/%.2f  bulk sends held %u (ticks first)  "
              "windows not measured: %u peer silent, %u queue drained",
              i + 1, g_rate / 1024, mode_name(g_mode), st.send_rate / 1024, p.d_last / 1024, best_bw(p) / 1024,
              st.out_bytes / 1024, st.pending_reliable / 1024, st.pending_unreliable / 1024, st.sent_unacked_reliable / 1024, st.ping,
              st.quality_local, st.quality_remote, held, stalled, drained);
    }
}

// "net gap": the moment channel 0 from a player goes silent for more than kGapMs (host -> player: the host's tick
// updates; player -> host: its inputs), what Steam says about the connection right then, and again when it resumes.
// The 5 s lines average a stall away; this is the state during it. At most one report per peer per 5 s.
void log_gaps()
{
    if (!g_now_usec) return;
    int64_t now = g_now_usec(g_utils);
    for (int i = 0; i < kMaxPeers; i++) {
        Peer& p = g_peers[i];
        int64_t silent_ms = 0, ended_ms = 0;
        bool report_now = false;
        {
            std::lock_guard lock(g_mutex);
            if (!p.used || !p.ch0_last) continue;
            silent_ms = (now - p.ch0_last) / 1000;
            ended_ms = p.gap_ended_ms;
            p.gap_ended_ms = 0;
            if (ended_ms) p.gap_logged = false;
            report_now = silent_ms > kGapMs && !p.gap_logged;
            if (!report_now && !ended_ms) continue;
            ULONGLONG t = GetTickCount64();
            if (report_now && t - p.gap_last_log < 5000) {
                p.gaps_suppressed++;
                p.gap_logged = true;
                continue;
            }
            if (report_now) p.gap_logged = true, p.gap_last_log = t;
        }
        if (!report_now) {
            if (ended_ms > kGapMs) swlog("net gap peer%d: channel 0 resumed after %lld ms", i + 1, static_cast<long long>(ended_ms));
            continue;
        }
        RealTimeStatus st{};
        int state = g_get_info(g_msgs, p.id, nullptr, &st);
        swlog("net gap peer%d: nothing on channel 0 for %lld ms (%s); Steam now: state %d, ping %d ms, quality %.2f local / %.2f "
              "remote, out %.0f pkt/s %.1f KB/s, in %.0f pkt/s %.1f KB/s, waiting %d KB reliable + %d KB unreliable, %d KB "
              "unacked, queue %.0f ms%s",
              i + 1, static_cast<long long>(silent_ms), dup_enabled() ? "redundant copies on" : "no redundant copies", state,
              st.ping, st.quality_local, st.quality_remote, st.out_packets, st.out_bytes / 1024, st.in_packets, st.in_bytes / 1024,
              st.pending_reliable / 1024, st.pending_unreliable / 1024, st.sent_unacked_reliable / 1024,
              double(st.queue_time_usec) / 1000.0,
              p.gaps_suppressed ? (" (" + std::to_string(p.gaps_suppressed) + " more gaps in the last 5 s)").c_str() : "");
        p.gaps_suppressed = 0;
    }
}

// Per player and channel, both ways, every 5 s (channels with traffic only).
void log_channels()
{
    if (!g_log_channels) return;
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kMaxPeers; i++) {
        Peer& p = g_peers[i];
        if (!p.used) continue;
        std::string out, in;
        char b[96];
        for (int c = 0; c < 16; c++) {
            if (p.out_n[c]) snprintf(b, sizeof b, " ch%d %u/%.0f KB", c, p.out_n[c], p.out_b[c] / 1024.0), out += b;
            if (p.in_n[c]) snprintf(b, sizeof b, " ch%d %u/%.0f KB", c, p.in_n[c], p.in_b[c] / 1024.0), in += b;
        }
        if (out.empty() && in.empty()) continue;
        swlog("net channels 5s peer%d: sent%s | received%s | ch0 arrivals: longest gap %.0f ms, %u gaps over 100 ms", i + 1,
              out.empty() ? " -" : out.c_str(), in.empty() ? " -" : in.c_str(), p.ch0_gap / 1000.0, p.ch0_gaps100);
        memset(p.out_n, 0, sizeof p.out_n), memset(p.in_n, 0, sizeof p.in_n);
        memset(p.out_b, 0, sizeof p.out_b), memset(p.in_b, 0, sizeof p.in_b);
        p.ch0_gap = 0;
        p.ch0_gaps100 = 0;
    }
}

void log_config(GetConfigValue_t get_cfg, const char* when)
{
    struct Item {
        int id;
        const char* name;
    } items[] = {{kCfgSendBufferSize, "SendBufferSize"}, {kCfgRecvBufferSize, "RecvBufferSize"},
                 {kCfgRecvMaxMessageSize, "RecvMaxMessageSize"}, {kCfgRecvBufferMessages, "RecvBufferMessages"},
                 {kCfgSendRateMin, "SendRateMin"}, {kCfgSendRateMax, "SendRateMax"}, {kCfgNagleTime, "NagleTime"},
                 {kCfgMtuPacketSize, "MTU_PacketSize"},
                 {kCfgIceEnable, "P2P_ICE_Enable"}, {kCfgIcePenalty, "P2P_ICE_Penalty"}, {kCfgSdrPenalty, "P2P_SDR_Penalty"}};
    std::string line;
    for (const Item& it : items) {
        int32_t value = 0;
        int type = 0;
        size_t size = sizeof(value);
        int r = get_cfg(g_utils, it.id, kScopeGlobal, 0, &type, &value, &size);
        char buf[96];
        if (r > 0) snprintf(buf, sizeof(buf), " %s=%d", it.name, value);
        else snprintf(buf, sizeof(buf), " %s=?(%d)", it.name, r);
        line += buf;
    }
    swlog("net: Steam config (%s):%s", when, line.c_str());
}

// Steam's debug output (its own threads): "steam: [level] text", at most 30 lines a minute.
bool g_steam_log_on = false;
void steam_output(int type, const char* msg)
{
    static std::atomic<ULONGLONG> minute{0};
    static std::atomic<int> lines{0}, dropped{0};
    ULONGLONG m = GetTickCount64() / 60000;
    if (minute.exchange(m) != m) {
        int d = dropped.exchange(0);
        lines = 0;
        if (d) swlog("steam: (%d more lines in the last minute not logged)", d);
    }
    if (lines.fetch_add(1) >= 30) {
        dropped++;
        return;
    }
    swlog("steam: [%d] %s", type, msg ? msg : "");
}

// The thread below calls into Steam every 200 ms and never ends on its own. At exit the game shuts Steam down while
// it may be mid-call: friend's Proton session 2026-10-22 02:17:34, access violation reading 0xbeef in
// lsteamclient.dll called from this thread. SteamAPI_Shutdown (hooked) now stops it first: it waits for the
// thread's current round (each round holds g_round) and the thread leaves before touching Steam again.
SRWLOCK g_round = SRWLOCK_INIT;
volatile LONG g_stopped = 0;
using Shutdown_t = void (*)();
Shutdown_t g_orig_shutdown;

struct SteamRound {
    bool active;
    SteamRound() { AcquireSRWLockExclusive(&g_round); active = InterlockedCompareExchange(&g_stopped, 0, 0) == 0; }
    ~SteamRound() { ReleaseSRWLockExclusive(&g_round); }
};

void stop_net_thread()
{
    InterlockedExchange(&g_stopped, 1);
    AcquireSRWLockExclusive(&g_round); // a round in progress finishes; later ones see g_stopped
    ReleaseSRWLockExclusive(&g_round);
}

void shutdown_detour()
{
    stop_net_thread();
    g_orig_shutdown();
}

DWORD WINAPI net_thread(void*)
{
    HMODULE api = nullptr;
    // Steam may still be initialising; wait up to ~2 minutes.
    for (int i = 0; i < 240 && (!g_msgs || !g_utils); i++) {
        if (InterlockedCompareExchange(&g_stopped, 0, 0)) return 0;
        if (!api) api = GetModuleHandleW(L"steam_api64.dll");
        if (api) {
            SteamRound round;
            if (!round.active) return 0;
            auto get_msgs = reinterpret_cast<GetIface_t>(GetProcAddress(api, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"));
            auto get_utils = reinterpret_cast<GetIface_t>(GetProcAddress(api, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004"));
            if (get_msgs) g_msgs = get_msgs();
            if (get_utils) g_utils = get_utils();
        }
        if (!g_msgs || !g_utils) Sleep(500);
    }
    if (!g_msgs || !g_utils) {
        swlog("net: Steam networking interfaces not available (api=%p msgs=%p utils=%p) - network fixes off", api, g_msgs,
              g_utils);
        return 0;
    }
    {
    SteamRound initialization;
    if (!initialization.active) return 0;
    g_set_int = reinterpret_cast<SetGlobalInt32_t>(GetProcAddress(api, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32"));
    auto get_cfg = reinterpret_cast<GetConfigValue_t>(GetProcAddress(api, "SteamAPI_ISteamNetworkingUtils_GetConfigValue"));
    if (!g_set_int || !get_cfg) {
        swlog("net: Steam config exports missing - network fixes off");
        return 0;
    }
    g_now_usec = reinterpret_cast<GetTimestamp_t>(GetProcAddress(api, "SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp"));
    log_config(get_cfg, "Steam defaults");
    if (g_send_buffer_kb > 0) g_set_int(g_utils, kCfgSendBufferSize, g_send_buffer_kb * 1024);
    if (g_recv_buffer_kb > 0) g_set_int(g_utils, kCfgRecvBufferSize, g_recv_buffer_kb * 1024);
    // Nagle only delays the game's event messages (channels 1..7; tick updates are sent with NoNagle):
    // up to 5 ms each by default.
    if (g_nagle_us >= 0) g_set_int(g_utils, kCfgNagleTime, g_nagle_us);
    if (g_mtu > 0)
        swlog("net: Steam packets at most %d bytes (MTU_PacketSize; fits IPv6 / Tailscale paths) %s", g_mtu,
              g_set_int(g_utils, kCfgMtuPacketSize, g_mtu) ? "set" : "FAILED to set");
    // Direct path: Steam's P2P connections go through its relays (SDR) unless both ends allow ICE. Measured
    // 2026-10-22: 41 ms ping to a server on the same PC = relayed. Both players need this (or Steam's "allow direct
    // connections" set) for a direct route; a vanilla peer keeps the relay. Connections made after this use it.
    if (g_direct) {
        bool ok = g_set_int(g_utils, kCfgIceEnable, kIceEnableAll) && g_set_int(g_utils, kCfgIcePenalty, 0) &&
                  g_set_int(g_utils, kCfgSdrPenalty, g_sdr_penalty);
        swlog("net: direct connections %s (ICE allowed for all peers, relay penalty %d ms); path per peer in \"net path\" lines",
              ok ? "on" : "FAILED to set", g_sdr_penalty);
        using SetStr_t = bool(__cdecl*)(void*, int, const char*);
        auto set_str = reinterpret_cast<SetStr_t>(GetProcAddress(api, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString"));
        // 2026-10-24: Steam's STUN server (103.10.125.24:3478) never answered from a peer's network, so it found no public
        // address and NAT traversal failed (5009), though UDP hole punching works there (Tailscale goes direct).
        if (g_stun[0] && set_str)
            swlog("net: STUN servers %s: %s", set_str(g_utils, kCfgStunList, g_stun) ? "set" : "FAILED to set", g_stun);
        using DbgFn = void (*)(int, const char*);
        using SetDbg_t = void(__cdecl*)(void*, int, DbgFn);
        auto set_dbg = reinterpret_cast<SetDbg_t>(GetProcAddress(api, "SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction"));
        if (g_ice_log > 0 && set_dbg) {
            g_set_int(g_utils, kCfgLogP2P, kDbgMsg + std::min(g_ice_log, 2) - 1);
            set_dbg(g_utils, kDbgMsg + std::min(g_ice_log, 2) - 1, &steam_output);
            g_steam_log_on = true;
            swlog("net: Steam P2P/ICE log on (\"steam:\" lines)");
        }
    }
    // Steam's own warnings and errors ("steam: [4] ..."), also without direct connections: timeouts, relay / config
    // fetch failures, its lock held for too long (its service thread stalled). At most 30 lines a minute.
    if (!g_steam_log_on) {
        using DbgFn = void (*)(int, const char*);
        using SetDbg_t = void(__cdecl*)(void*, int, DbgFn);
        auto set_dbg = reinterpret_cast<SetDbg_t>(GetProcAddress(api, "SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction"));
        if (set_dbg) {
            set_dbg(g_utils, kDbgWarning, &steam_output);
            swlog("net: Steam's warnings go to this log (\"steam:\" lines)");
        }
    }

    if (g_redundancy) {
        dup_init(g_utils, reinterpret_cast<void*>(GetProcAddress(api, "SteamAPI_ISteamNetworkingUtils_AllocateMessage")));
    }
    bool hook_recv = true; // playout arrivals, redundancy, and the latency breakdown
    if (g_rate_control || g_pace_ms > 0 || hook_recv) {
        void** vt = *static_cast<void***>(g_msgs);
        memcpy(g_vtable, vt, sizeof(g_vtable));
        g_orig_send = reinterpret_cast<Send_t>(vt[0]);
        g_get_info = reinterpret_cast<GetSessionInfo_t>(vt[5]);
        g_vtable[0] = reinterpret_cast<void*>(&send_detour);
        g_orig_recv = reinterpret_cast<Receive_t>(vt[1]);
        if (hook_recv) g_vtable[1] = reinterpret_cast<void*>(&recv_detour);
        InterlockedExchangePointer(static_cast<void**>(g_msgs), g_vtable);
        if (g_pace_ms > 0) {
            swlog("net: tick priority on: channels 1..7 wait in the game's queue while Steam holds > %d ms of sending "
                  "(min %lld KB)", g_pace_ms, kPaceMinBytes / 1024);
        }
    }

    if (!g_rate_control) {
        if (g_fixed_rate_kb > 0) {
            g_set_int(g_utils, kCfgSendRateMin, g_fixed_rate_kb * 1024);
            g_set_int(g_utils, kCfgSendRateMax, g_fixed_rate_kb * 1024);
        }
        log_config(get_cfg, "rate_control=0, fixed settings");
        if (!dup_enabled()) return 0;
    } else {
        double remembered = 0;
        g_session_start = start_rate(remembered);
        apply_rate(g_session_start, "start");
        g_window_rate = g_rate;
        log_config(get_cfg, "after fixes");
        swlog("net: adaptive send rate on: start %.0f KB/s (start_rate_kb %.0f, last measured capacity %.0f KB/s), range %.0f..%.0f KB/s",
              g_rate / 1024, g_start_rate / 1024, remembered / 1024, g_min_rate / 1024, g_max_rate / 1024);
    }

    }
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    for (int ticks = 1;; ticks++) {
        Sleep(200);
        AcquireSRWLockExclusive(&g_round);
        if (g_stopped) {
            ReleaseSRWLockExclusive(&g_round);
            swlog("net: Steam is shutting down, stats thread stopped");
            return 0;
        }
        auto now = clock::now();
        if (g_rate_control) {
            tick(std::chrono::duration<double>(now - last).count());
            if (ticks % kTicksPerWindow == 0) end_window();
        }
        last = now;
        update_queue();
        log_gaps();
        if (ticks % 25 == 0) { // every 5 s
            if (g_rate_control) log_status();
            dup_log_status();
            log_latency();
            log_channels();
            log_compress();
        }
        ReleaseSRWLockExclusive(&g_round);
    }
}

bool install()
{
    const wchar_t* ini = settings_ini();
    g_send_buffer_kb = GetPrivateProfileIntW(L"net", L"send_buffer_kb", 8192, ini);
    g_recv_buffer_kb = GetPrivateProfileIntW(L"net", L"recv_buffer_kb", 8192, ini);
    g_rate_control = GetPrivateProfileIntW(L"net", L"rate_control", 1, ini) != 0;
    g_fixed_rate_kb = GetPrivateProfileIntW(L"net", L"send_rate_kb", 0, ini);
    g_start_rate = GetPrivateProfileIntW(L"net", L"start_rate_kb", 256, ini) * 1024.0;
    g_min_rate = GetPrivateProfileIntW(L"net", L"min_rate_kb", 32, ini) * 1024.0;
    g_max_rate = std::max(g_min_rate, GetPrivateProfileIntW(L"net", L"max_rate_kb", 8192, ini) * 1024.0);
    g_pace_ms = std::max(0, static_cast<int>(GetPrivateProfileIntW(L"net", L"pace_ms", 40, ini)));
    g_nagle_us = static_cast<int>(GetPrivateProfileIntW(L"net", L"nagle_us", 0, ini));
    g_mtu = static_cast<int>(GetPrivateProfileIntW(L"net", L"mtu_packet_size", 1200, ini));
    if (g_mtu > 0) g_mtu = std::clamp(g_mtu, 576, 1300);
    g_direct = GetPrivateProfileIntW(L"net", L"direct_connect", 0, ini) != 0;
    g_sdr_penalty = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"net", L"relay_penalty_ms", 10, ini)), 0, 1000);
    g_ice_log = static_cast<int>(GetPrivateProfileIntW(L"net", L"ice_log", 1, ini));
    wchar_t stun[512];
    GetPrivateProfileStringW(L"net", L"stun_servers", L"", stun, 512, ini);
    WideCharToMultiByte(CP_UTF8, 0, stun, -1, g_stun, sizeof g_stun, nullptr, nullptr);
    g_redundancy = GetPrivateProfileIntW(L"net", L"redundancy", 1, ini) != 0;
    g_log_channels = GetPrivateProfileIntW(L"mp", L"channels", 1, ini) != 0;
    g_measure_compress = GetPrivateProfileIntW(L"measure", L"tick_compress", 1, ini) != 0;
    if (g_rate_control && g_fixed_rate_kb > 0) {
        swlog("net: send_rate_kb=%d ignored: the adaptive rate replaces it (rate_control=0 for a fixed rate)", g_fixed_rate_kb);
    }
    if (!hook_api(L"steam_api64.dll", "SteamAPI_Shutdown", reinterpret_cast<void*>(&shutdown_detour),
                  reinterpret_cast<void**>(&g_orig_shutdown))) {
        swlog("net: couldn't hook SteamAPI_Shutdown; the stats thread may still call Steam during exit");
    }
    // Steam may not be ready yet; the rest happens on a background thread.
    HANDLE t = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}

HookRegistrar reg{{"Steam send buffer / adaptive send rate", false, &install}};

} // namespace

uint64_t net_bulk_received() { return g_bulk_in.load(std::memory_order_relaxed); }
int net_send_queue_ms() { return g_queue_ms.load(std::memory_order_relaxed); }
int net_send_queue_playing_ms() { return g_queue_playing_ms.load(std::memory_order_relaxed); }
