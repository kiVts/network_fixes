
#include "log.h"
#include "net_shared.h"
#include "sdk.h"
#include "network_wire.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace {

constexpr int kChannel = 11;
constexpr uint32_t kMagic = 0x44525753; // "SWRD"
constexpr uint8_t kVersion = 1;
constexpr uint8_t kHello = 1, kTicks = 2, kBacklog = 3; // kBacklog: uint32 queue depth after the header (pacing.cpp)
constexpr uint8_t kManifest = 4;                         // text after the header: mods and versions (version_check.cpp)
// Distant AI logic in multiplayer (physics_fixes [lod] logic): every PC that runs it says so once a second (kLodCap); the
// host sends the vehicles it lets run their logic every Nth tick, with its tick, ~10 times a second (kLodSet, physics_fixes'
// payload after the header). See lod_poll().
constexpr uint8_t kLodCap = 5, kLodSet = 6;
constexpr DWORD kLodCapEveryMs = 1000, kLodSetEveryMs = 100, kLodFreshMs = 3000;
// Clients push loose objects (object_push.cpp): the host says once a second that it follows them (kPushCap); a client
// sends its owned objects' states every tick to such a host (kPushState, object_push's payload after the header); the
// host refuses claims with kPushDeny (payload: object keys).
constexpr uint8_t kPushCap = 7, kPushState = 8, kPushDeny = 9, kPushHostState = 10, kPushForce = 11;
constexpr uint8_t kSdkCap = 12, kSdkMessage = 13;
constexpr DWORD kPushCapEveryMs = 1000, kPushFreshMs = 3000;
constexpr int kSendReliable = 8;
constexpr DWORD kBacklogEveryMs = 250;
constexpr int kSendUnreliableNoNagle = 1;
constexpr uint32_t kMaxCopy = 1000;   // larger messages go reliable only
constexpr uint32_t kMaxPacket = 1100; // stay inside one UDP packet
constexpr int kCopies = 3;            // each message rides in this many consecutive packets
constexpr int kMaxHellos = 5;
constexpr DWORD kEpochIdleMs = 3000;
// At most one copy packet per this many ms per peer. A client sends a channel-0 message every frame, and on the
// loading screen that is ~1300 fps: 2026-10-22 02:53:59 a joining client sent 3230 copy packets (347 KB, 5x the
// originals) in 5 s, a 10 s queue at its upload rate. Skipping a packet is the same as losing one: the messages it
// would have carried come in the next packet (each rides in kCopies) or from the reliable stream. Host tick updates
// (16-22 ms apart) are never skipped.
constexpr double kMinCopyGapMs = 8;
constexpr size_t kIdentitySize = 136;
// SteamNetworkingMessage_t
constexpr size_t kMsgData = 0x0, kMsgSize = 0x8, kMsgConn = 0xc, kMsgIdentity = 0x10, kMsgTime = 0xa0,
                 kMsgRelease = 0xb8, kMsgChannel = 0xc0;
constexpr size_t kHashRing = 1024;
constexpr int kVotesNeeded = 8;
constexpr size_t kMaxVoteOffsets = 64; // distinct offsets voted for before the count starts over
constexpr size_t kSyncCopies = 4096;   // copy hashes kept while not synced
constexpr int kPollBatches = 4; // never starve native receives/sends while channel 11 stays busy

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t version, type;
    uint16_t count;
    uint32_t epoch;
};
struct Entry {
    uint32_t seq;
    uint32_t hash;
    uint16_t size;
};
#pragma pack(pop)

uint32_t hash_bytes(const void* p, uint32_t n)
{
    uint32_t h = 2166136261u ^ n;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (uint32_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

struct Key {
    uint8_t b[16]; // identity type, size, SteamID
    bool operator<(const Key& o) const { return memcmp(b, o.b, 16) < 0; }
};

struct Copy {
    uint32_t seq, hash;
    std::vector<uint8_t> data;
};

struct Peer {
    uint8_t identity[kIdentitySize];
    // sending
    std::atomic<bool> confirmed{false}; // said hello: reads channel 11
    bool manifest_sent = false;          // our mod manifest, once per session (cleared with a new epoch)
    int index = 0;                       // "peerN" in the log (order of first contact)
    int hellos_sent = 0;
    DWORD last_hello = 0, last_send = 0;
    LONGLONG last_copy_qpc = 0;
    std::atomic<uint64_t> copies_out{0}, copies_skipped{0};
    uint32_t epoch = 1;
    uint32_t next_send_seq = 0;
    std::deque<Copy> recent; // last kCopies small messages sent
    // receiving
    uint32_t their_epoch = 0;
    std::atomic<bool> aligned{false}, disabled{false};
    int64_t offset = 0;                     // S = r + offset
    uint64_t r = 0;                         // reliable channel-0 messages received
    uint32_t next_seq = 0;                  // next S to give the game
    std::map<uint32_t, void*> held;         // copies waiting for their turn (SteamNetworkingMessage_t*)
    std::map<uint32_t, uint32_t> copy_hash; // S -> hash carried by its copy
    uint32_t rel_hash[kHashRing] = {};      // hash of reliable message r (for syncing)
    std::map<int64_t, int> votes;
    std::map<uint32_t, int64_t> copy_time;  // S -> when the copy given to the game arrived (Steam usec)
    // Exact bytes already delivered from copies, keyed by reliable ordinal. Unlike
    // guessed sequence offsets, these survive epoch changes and can justify a drop.
    std::map<uint64_t, std::vector<uint8_t>> delivered;
    // stats
    std::atomic<uint64_t> rescued{0}, dropped_late{0}, copies_in{0}, stalls{0}, stall_ms{0}, stall_max_ms{0};
    // merge audit (network thread writes, status thread reads and resets): what went to the game, in what order
    std::atomic<uint64_t> a_copy{0}, a_rel{0}, a_plain{0}, a_gap{0}, a_repeat{0}, a_verified{0}, a_unrecorded{0}, a_resyncs{0};
    std::atomic<uint64_t> a_epochs{0};
    uint32_t a_last_s = 0;
    bool a_have_last = false;
    // host pacing: the backlog this peer last reported (read by the update thread)
    std::atomic<int> backlog{-1};
    std::atomic<DWORD> backlog_at{0};
    // distant AI logic: when this peer last said it runs it (kLodCap)
    std::atomic<DWORD> lod_cap_at{0};
    // object push: when this peer (the host) last said it follows pushed objects (kPushCap); network thread only
    DWORD push_cap_at = 0;
    DWORD sdk_cap_at = 0, sdk_cap_sent = 0;
    uint32_t sdk_remote_epoch = 0;
    bool sdk_registered = false;
    swml_peer sdk_peer{};
};
DWORD g_last_backlog_report = 0;
std::atomic<bool> g_manifest_due{true}; // a peer may be waiting for our manifest (set on confirmation / new epoch)

std::mutex g_peers_mu; // the map itself (the status thread iterates it)
std::map<Key, Peer*> g_peers;
std::deque<void*> g_ready; // messages for the game on channel 0, in order
void* g_utils = nullptr;
using Alloc_t = void*(__cdecl*)(void* utils, int size);
Alloc_t g_alloc = nullptr;
std::atomic<DWORD> g_last_active{0};
bool g_enabled = false;
DWORD g_net_thread = 0;
const swml_api_v2* g_sdk = nullptr;
swml_handle g_sdk_provider = 0, g_sdk_completed = 0;
swml_result g_sdk_completion = SWML_OK;

void sdk_disconnect(Peer& p) {
    if (p.sdk_registered && g_sdk)
        g_sdk->transport_peer(swml_self(), g_sdk_provider, &p.sdk_peer, 0);
    p.sdk_registered = false;
}

void sdk_poll(void* self, Send_t send) {
    if (!g_sdk || !g_sdk_provider) return;
    DWORD now = GetTickCount();
    {
        std::lock_guard lock(g_peers_mu);
        for (auto& [key, p] : g_peers) {
            bool active = p->confirmed && p->last_send && now - p->last_send < 5000;
            if (!active || !p->sdk_cap_at || now - p->sdk_cap_at > 5000) sdk_disconnect(*p);
            if (active && now - p->sdk_cap_sent >= 1000) {
                uint8_t buffer[sizeof(Header) + sizeof(uint32_t)];
                Header h{kMagic, kVersion, kSdkCap, 0, p->epoch};
                memcpy(buffer, &h, sizeof(h));
                uint32_t v = swml_wire::version;
                memcpy(buffer + sizeof(h), &v, sizeof(v));
                send(self, p->identity, buffer, sizeof(buffer), kSendUnreliableNoNagle, kChannel);
                p->sdk_cap_sent = now;
            }
        }
    }
    for (int work = 0; work < 8; ++work) {
        if (g_sdk_completed) {
            swml_result r = g_sdk->transport_complete(swml_self(), g_sdk_provider, g_sdk_completed, g_sdk_completion);
            if (r == SWML_LIMIT) break; // retry completion, never resend an already accepted message
            g_sdk_completed = 0;
        }
        swml_transport_packet packet{};
        if (g_sdk->transport_peek(swml_self(), g_sdk_provider, &packet) != SWML_OK) break;
        uint8_t identity[kIdentitySize]{};
        uint32_t epoch = 0;
        {
            std::lock_guard lock(g_peers_mu);
            Key key; memcpy(key.b, packet.peer.identity, 16);
            auto it = g_peers.find(key);
            if (it != g_peers.end() && it->second->sdk_registered &&
                it->second->sdk_peer.generation == packet.peer.generation) {
                memcpy(identity, it->second->identity, sizeof(identity)); epoch = it->second->epoch;
            }
        }
        g_sdk_completion = SWML_UNAVAILABLE;
        if (epoch) {
            uint8_t buffer[kMaxPacket];
            Header h{kMagic, kVersion, kSdkMessage, 0, epoch};
            memcpy(buffer, &h, sizeof(h));
            uint32_t n = swml_wire::encode(packet, buffer + sizeof(h), sizeof(buffer) - sizeof(h));
            int result = n ? send(self, identity, buffer, n + uint32_t(sizeof(h)), kSendReliable, kChannel) : 0;
            if (result == 25) break; // Steam backpressure; leave the unsent item at the head
            g_sdk_completion = result == 1 ? SWML_OK : SWML_IO_ERROR;
        }
        g_sdk_completed = packet.operation;
    }
}

Peer& peer_for(const void* identity)
{
    Key k;
    memcpy(k.b, identity, 16);
    std::lock_guard lock(g_peers_mu);
    auto it = g_peers.find(k);
    if (it != g_peers.end()) return *it->second;
    Peer* p = new Peer;
    memcpy(p->identity, identity, kIdentitySize);
    p->epoch = (GetTickCount() * 2654435761u) | 1;
    g_peers[k] = p;
    p->index = int(g_peers.size());
    return *p;
}

void release(void* msg)
{
    auto fn = *reinterpret_cast<void(__cdecl**)(void*)>(static_cast<uint8_t*>(msg) + kMsgRelease);
    if (fn) fn(msg);
}

void send_hello(void* self, Peer& p, Send_t send)
{
    Header h{kMagic, kVersion, kHello, 0, p.epoch};
    send(self, p.identity, &h, sizeof(h), kSendUnreliableNoNagle, kChannel);
    p.hellos_sent++;
    p.last_hello = GetTickCount();
}

// --- receiving ------------------------------------------------------------------------------------------

// Merge audit: every message handed to the game while synced must have S = the previous one + 1.
void audit_given(Peer& p, uint32_t s)
{
    if (p.a_have_last) {
        if (s == p.a_last_s) p.a_repeat.fetch_add(1, std::memory_order_relaxed);
        else if (s != p.a_last_s + 1) {
            p.a_gap.fetch_add(1, std::memory_order_relaxed);
            static int s_logged = 0; // the 5 s line counts the rest
            if (s_logged < 50) {
                s_logged++;
                swlog("net merge audit: message S=%u handed to the game after S=%u (expected %u)", s, p.a_last_s, p.a_last_s + 1);
            }
        }
    }
    p.a_last_s = s;
    p.a_have_last = true;
}

void flush_held(Peer& p)
{
    for (auto it = p.held.begin(); it != p.held.end() && it->first == p.next_seq; it = p.held.erase(it)) {
        if (p.delivered.size() >= kSyncCopies) break; // await reliable acknowledgements; bounded memory
        const int64_t ordinal = int64_t(it->first) - p.offset;
        if (ordinal < 0 || uint64_t(ordinal) < p.r) break;
        const auto* mb = static_cast<const uint8_t*>(it->second);
        const auto* bytes = *reinterpret_cast<const uint8_t* const*>(mb + kMsgData);
        const int size = *reinterpret_cast<const int*>(mb + kMsgSize);
        p.delivered.emplace(uint64_t(ordinal), std::vector<uint8_t>(bytes, bytes + size));
        p.copy_time[it->first] = *reinterpret_cast<const int64_t*>(static_cast<const uint8_t*>(it->second) + kMsgTime);
        g_ready.push_back(it->second);
        p.rescued.fetch_add(1, std::memory_order_relaxed);
        audit_given(p, it->first);
        p.a_copy.fetch_add(1, std::memory_order_relaxed);
        p.next_seq++;
    }
    while (!p.held.empty() && p.held.begin()->first < p.next_seq) { // can never be used any more
        release(p.held.begin()->second);
        p.held.erase(p.held.begin());
    }
    while (!p.copy_hash.empty() && p.copy_hash.begin()->first + 4096 < p.next_seq) p.copy_hash.erase(p.copy_hash.begin());
    while (!p.copy_time.empty() && p.copy_time.begin()->first + 4096 < p.next_seq) p.copy_time.erase(p.copy_time.begin());
}

void stop_copies(Peer& p, const char* why)
{
    swml_report_fault(swml(), swml_self());
    if (!p.disabled) swlog("net redundancy: copies from a peer switched off (%s); its reliable stream carries on", why);
    p.disabled = true;
    for (auto& [s, m] : p.held) release(m);
    p.held.clear();
}

void reset_receive(Peer& p, uint32_t epoch)
{
    for (auto& [s, m] : p.held) release(m);
    p.held.clear();
    p.copy_hash.clear();
    p.copy_time.clear();
    p.votes.clear();
    p.their_epoch = epoch;
    p.a_have_last = false;
    p.a_epochs.fetch_add(1, std::memory_order_relaxed);
    p.aligned = false;
    p.disabled = false;
    memset(p.rel_hash, 0, sizeof(p.rel_hash));
}

// One vote for "S = r + offset". Synced once an offset has kVotesNeeded votes and 3x the runner-up.
// Only unambiguous matches vote: a hash that matches more than one message in the window says nothing about the
// offset. 2026-10-22 03:41, host receiving a joining client's copies: on the loading screen the client sends nearly
// identical channel-0 messages every frame, every copy matched hundreds of reliable messages, the votes spread over
// hundreds of offsets (never 3:1), the vote map grew without bound while each vote scanned all of it, and the game's
// network thread (which runs this) ended up spending its time here: it stopped sending to the client (no world, the
// client stuck on the Spawning screen) and never noticed it leave. The map is also capped now.
bool vote(Peer& p, int64_t offset)
{
    if (!p.delivered.empty()) return false; // settle the previous epoch's twins before adopting another offset
    if (p.votes.size() >= kMaxVoteOffsets && !p.votes.count(offset)) p.votes.clear(); // no consensus: start over
    int mine = ++p.votes[offset];
    if (mine < kVotesNeeded) return false;
    int other = 0;
    for (auto& [o, n] : p.votes) {
        if (o != offset) other = std::max(other, n);
    }
    if (mine < 3 * other) return false;
    p.offset = offset;
    p.aligned = true;
    p.votes.clear();
    p.a_have_last = false;
    p.a_resyncs.fetch_add(1, std::memory_order_relaxed);
    swlog("net redundancy: in sync with a peer, using its redundant tick copies (offset %lld after %llu reliable messages, "
          "%d votes)", static_cast<long long>(offset), static_cast<unsigned long long>(p.r), mine);
    return true;
}

void on_copy(Peer& p, const Entry& e, const uint8_t* data, const uint8_t* src_msg)
{
    p.copies_in.fetch_add(1, std::memory_order_relaxed);
    if (p.disabled || e.size > kMaxCopy) return;
    if (hash_bytes(data, e.size) != e.hash) { stop_copies(p, "invalid copy checksum"); return; }
    if (p.aligned && (e.seq < p.next_seq || uint64_t(e.seq) - p.next_seq >= kSyncCopies)) return;
    bool fresh = p.copy_hash.emplace(e.seq, e.hash).second;
    if (!fresh) return;
    while (p.copy_hash.size() > kSyncCopies) p.copy_hash.erase(p.copy_hash.begin());
    if (!p.aligned) {
        while (p.copy_hash.size() > kSyncCopies) p.copy_hash.erase(p.copy_hash.begin()); // pruned by flush_held once synced
        uint64_t lo = p.r > kHashRing ? p.r - kHashRing : 0, match = 0;
        int matches = 0;
        for (uint64_t r = lo; r < p.r && matches < 2; r++) {
            if (p.rel_hash[r % kHashRing] == e.hash) {
                match = r;
                matches++;
            }
        }
        if (matches == 1 && vote(p, int64_t(e.seq) - int64_t(match)))
            p.next_seq = static_cast<uint32_t>(int64_t(p.r) + p.offset); // S of the next reliable message
        return;
    }
    if (e.seq < p.next_seq || p.held.size() >= kSyncCopies) return;
    void* m = g_alloc(g_utils, e.size);
    if (!m) return;
    uint8_t* mb = static_cast<uint8_t*>(m);
    memcpy(*reinterpret_cast<void**>(mb + kMsgData), data, e.size);
    *reinterpret_cast<int*>(mb + kMsgSize) = e.size;
    memcpy(mb + kMsgConn, src_msg + kMsgConn, 4);
    memcpy(mb + kMsgIdentity, src_msg + kMsgIdentity, kIdentitySize);
    memcpy(mb + kMsgTime, src_msg + kMsgTime, 8);
    *reinterpret_cast<int*>(mb + kMsgChannel) = 0;
    p.held[e.seq] = m;
    flush_held(p);
}

// --- distant AI logic (physics_fixes exports, looked up once physics_fixes is loaded) --------------------------------
using LodVersion_t = uint32_t(__cdecl*)();
using LodPeers_t = void(__cdecl*)(int connected, int capable);
using LodHostPayload_t = uint32_t(__cdecl*)(uint8_t* out, uint32_t cap);
using LodClientPayload_t = void(__cdecl*)(const uint8_t* data, uint32_t size);
LodVersion_t g_lod_version = nullptr;
LodPeers_t g_lod_peers = nullptr;
LodHostPayload_t g_lod_host = nullptr;
LodClientPayload_t g_lod_client = nullptr;
DWORD g_lod_looked = 0, g_lod_cap_sent = 0, g_lod_set_sent = 0;
std::atomic<uint64_t> g_lod_sets_out{0}, g_lod_sets_in{0};

bool lod_api()
{
    if (g_lod_version) return true;
    DWORD now = GetTickCount();
    if (g_lod_looked && now - g_lod_looked < 5000) return false;
    g_lod_looked = now;
    HMODULE m = GetModuleHandleW(L"physics_fixes.dll");
    if (!m) return false;
    auto v = reinterpret_cast<LodVersion_t>(GetProcAddress(m, "swml_lod_net_version"));
    auto pe = reinterpret_cast<LodPeers_t>(GetProcAddress(m, "swml_lod_net_peers"));
    auto h = reinterpret_cast<LodHostPayload_t>(GetProcAddress(m, "swml_lod_net_host_payload"));
    auto c = reinterpret_cast<LodClientPayload_t>(GetProcAddress(m, "swml_lod_net_client_payload"));
    if (!v || !pe || !h || !c) return false;
    g_lod_peers = pe, g_lod_host = h, g_lod_client = c;
    g_lod_version = v;
    swlog("net lod: physics_fixes distant AI logic found (it %s)", v() ? "is on: telling peers, and the host sends its far set"
                                                                        : "is off on this PC");
    return true;
}

void lod_on_set(const uint8_t* data, uint32_t size)
{
    if (!lod_api() || !g_lod_version()) return;
    g_lod_sets_in.fetch_add(1, std::memory_order_relaxed);
    g_lod_client(data, size);
}

// Every loop: tell modded peers we run it (1/s), report connected / capable peers to physics_fixes, and as the host send
// the far set to every capable peer (~10/s). A peer counts as connected while we sent it channel 0 within kLodFreshMs.
void lod_poll(void* self, Send_t send)
{
    if (!lod_api()) return;
    bool on = g_lod_version() != 0;
    DWORD now = GetTickCount();
    std::lock_guard lock(g_peers_mu);
    int connected = 0, capable = 0;
    for (auto& [k, p] : g_peers) {
        if (!p->last_send || now - p->last_send > kLodFreshMs) continue;
        connected++;
        DWORD at = p->lod_cap_at.load(std::memory_order_relaxed);
        if (p->confirmed && at && now - at <= kLodFreshMs) capable++;
    }
    g_lod_peers(connected, capable);
    if (!on) return;
    if (now - g_lod_cap_sent >= kLodCapEveryMs) {
        g_lod_cap_sent = now;
        for (auto& [k, p] : g_peers) {
            if (!p->confirmed) continue;
            Header h{kMagic, kVersion, kLodCap, 0, p->epoch};
            send(self, p->identity, &h, sizeof(h), kSendUnreliableNoNagle, kChannel);
        }
    }
    if (now - g_lod_set_sent < kLodSetEveryMs) return;
    g_lod_set_sent = now;
    uint8_t buf[kMaxPacket];
    uint32_t n = g_lod_host(buf + sizeof(Header), uint32_t(sizeof(buf) - sizeof(Header)));
    if (!n) return; // not the host, or nothing to send
    for (auto& [k, p] : g_peers) {
        DWORD at = p->lod_cap_at.load(std::memory_order_relaxed);
        if (!p->confirmed || !at || now - at > kLodFreshMs) continue;
        Header h{kMagic, kVersion, kLodSet, 0, p->epoch};
        memcpy(buf, &h, sizeof(h));
        send(self, p->identity, buf, uint32_t(sizeof(Header) + n), kSendUnreliableNoNagle, kChannel);
        g_lod_sets_out.fetch_add(1, std::memory_order_relaxed);
    }
}

// Every loop: as the host, say once a second that pushed objects are followed and send refusals; as a client, send the
// newest batch of owned objects to the host(s) that said so.
DWORD g_push_cap_sent = 0;
void push_poll(void* self, Send_t send)
{
    if (!push_enabled()) return;
    DWORD now = GetTickCount();
    uint8_t buf[kMaxPacket];
    std::lock_guard lock(g_peers_mu);
    if (push_hosting()) {
        bool cap = now - g_push_cap_sent >= kPushCapEveryMs;
        if (cap) g_push_cap_sent = now;
        uint8_t boxes[kMaxPacket];
        uint32_t box_bytes = push_host_payload(boxes+sizeof(Header),uint32_t(sizeof(boxes)-sizeof(Header)));
        for (auto& [k, p] : g_peers) {
            if (!p->confirmed) continue;
            if (box_bytes) {
                Header h{kMagic,kVersion,kPushHostState,0,p->epoch};
                memcpy(boxes,&h,sizeof h);
                send(self,p->identity,boxes,uint32_t(sizeof(Header)+box_bytes),kSendUnreliableNoNagle,kChannel);
            }
            if (cap) {
                Header h{kMagic, kVersion, kPushCap, 0, p->epoch};
                send(self, p->identity, &h, sizeof(h), kSendUnreliableNoNagle, kChannel);
            }
            uint32_t n = push_take_denies(p->index, buf + sizeof(Header), uint32_t(sizeof(buf) - sizeof(Header)));
            if (!n) continue;
            Header h{kMagic, kVersion, kPushDeny, 0, p->epoch};
            memcpy(buf, &h, sizeof(h));
            send(self, p->identity, buf, uint32_t(sizeof(Header) + n), kSendReliable, kChannel);
        }
        return;
    }
    uint8_t force_buf[kMaxPacket];
    uint32_t nf=push_client_force_payload(force_buf+sizeof(Header),uint32_t(sizeof(force_buf)-sizeof(Header)));
    uint32_t n = push_client_payload(buf + sizeof(Header), uint32_t(sizeof(buf) - sizeof(Header)));
    if (!n && !nf) return;
    for (auto& [k, p] : g_peers) {
        if (!p->confirmed || !p->push_cap_at || now - p->push_cap_at > kPushFreshMs) continue;
        if(n) {
            Header h{kMagic,kVersion,kPushState,0,p->epoch};memcpy(buf,&h,sizeof h);
            send(self,p->identity,buf,uint32_t(sizeof(Header)+n),kSendUnreliableNoNagle,kChannel);
        }
        if(nf) {
            Header h{kMagic,kVersion,kPushForce,0,p->epoch};memcpy(force_buf,&h,sizeof h);
            send(self,p->identity,force_buf,uint32_t(sizeof(Header)+nf),kSendUnreliableNoNagle,kChannel);
        }
    }
}

void on_packet(void* self, const uint8_t* msg, Send_t send)
{
    const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(msg + kMsgData);
    int size = *reinterpret_cast<const int*>(msg + kMsgSize);
    if (size < int(sizeof(Header))) return;
    Header h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kVersion) return;
    Peer& p = peer_for(msg + kMsgIdentity);
    if (h.type == kSdkCap) {
        if (!g_sdk || !g_sdk_provider || size != int(sizeof(Header) + sizeof(uint32_t)) ||
            !p.last_send || GetTickCount() - p.last_send >= 5000) return;
        uint32_t v = 0; memcpy(&v, data + sizeof(Header), sizeof(v));
        if (v != swml_wire::version) return;
        swml_peer endpoint{};
        memcpy(endpoint.identity, p.identity, 16);
        endpoint.generation = (uint64_t(p.epoch) << 32) | h.epoch;
        if (p.sdk_registered && p.sdk_peer.generation != endpoint.generation) sdk_disconnect(p);
        p.sdk_cap_at = GetTickCount(); p.sdk_remote_epoch = h.epoch; p.sdk_peer = endpoint;
        p.sdk_registered = g_sdk->transport_peer(swml_self(), g_sdk_provider, &p.sdk_peer, 1) == SWML_OK;
        return;
    }
    if (h.type == kSdkMessage) {
        if (!g_sdk || !p.sdk_registered || h.epoch != p.sdk_remote_epoch ||
            GetTickCount() - p.sdk_cap_at > 5000 || size > int(kMaxPacket)) return;
        swml_transport_packet packet{};
        if (swml_wire::decode(data + sizeof(Header), uint32_t(size) - uint32_t(sizeof(Header)), packet))
            g_sdk->transport_receive(swml_self(), g_sdk_provider, &p.sdk_peer, packet.topic, packet.data, packet.data_size);
        return;
    }
    if (h.type == kHello) {
        bool was = p.confirmed;
        p.confirmed = true;
        g_manifest_due = true;
        // Answer so they know about us too (bounded: kMaxHellos per peer in total).
        if (p.hellos_sent < kMaxHellos && (!was || GetTickCount() - p.last_hello > 1000)) send_hello(self, p, send);
        if (!was) swlog("net redundancy: a peer has it too - sending it redundant tick copies");
        return;
    }
    if (h.type == kManifest) {
        version_on_manifest(p.index, reinterpret_cast<const char*>(data + sizeof(Header)), size_t(size) - sizeof(Header));
        return;
    }
    if (h.type == kLodCap) {
        p.lod_cap_at.store(GetTickCount(), std::memory_order_relaxed);
        return;
    }
    if (h.type == kLodSet) {
        lod_on_set(data + sizeof(Header), uint32_t(size) - uint32_t(sizeof(Header)));
        return;
    }
    if (h.type == kPushCap) {
        p.push_cap_at = GetTickCount();
        push_on_host_cap();
        return;
    }
    if (h.type == kPushState) {
        push_on_states(p.index, data + sizeof(Header), uint32_t(size) - uint32_t(sizeof(Header)));
        return;
    }
    if (h.type == kPushForce) {
        if(push_hosting())push_on_forces(p.index,data+sizeof(Header),uint32_t(size)-uint32_t(sizeof(Header)));
        return;
    }
    if (h.type == kPushHostState) {
        if (!push_hosting() && p.push_cap_at && GetTickCount()-p.push_cap_at<=kPushFreshMs)
            push_on_host_states(data+sizeof(Header),uint32_t(size)-uint32_t(sizeof(Header)));
        return;
    }
    if (h.type == kPushDeny) {
        push_on_denies(data + sizeof(Header), uint32_t(size) - uint32_t(sizeof(Header)));
        return;
    }
    if (h.type == kBacklog) {
        if (size >= int(sizeof(Header) + sizeof(uint32_t))) {
            uint32_t depth;
            memcpy(&depth, data + sizeof(Header), sizeof(depth));
            p.backlog.store(int(std::min<uint32_t>(depth, 1u << 20)), std::memory_order_relaxed);
            p.backlog_at.store(GetTickCount(), std::memory_order_relaxed);
        }
        return;
    }
    if (h.type != kTicks) return;
    // Validate the entire bundle before changing merge state or delivering any entry.
    if (size > int(kMaxPacket) || !h.count || h.count > kCopies) return;
    size_t checked = sizeof(Header);
    for (unsigned i = 0; i < h.count; ++i) {
        if (checked + sizeof(Entry) > size_t(size)) return;
        Entry e; memcpy(&e, data + checked, sizeof(e)); checked += sizeof(e);
        if (e.size > kMaxCopy || checked + e.size > size_t(size) ||
            hash_bytes(data + checked, e.size) != e.hash) return;
        checked += e.size;
    }
    if (checked != size_t(size)) return;
    if (!p.confirmed.exchange(true)) g_manifest_due = true;
    if (h.epoch != p.their_epoch) reset_receive(p, h.epoch);
    g_last_active.store(GetTickCount(), std::memory_order_relaxed);
    size_t off = sizeof(Header);
    for (int i = 0; i < h.count; i++) {
        if (off + sizeof(Entry) > size_t(size)) return;
        Entry e;
        memcpy(&e, data + off, sizeof(e));
        off += sizeof(e);
        if (off + e.size > size_t(size)) return;
        on_copy(p, e, data + off, msg);
        off += e.size;
    }
}

void on_reliable(void* msg)
{
    uint8_t* mb = static_cast<uint8_t*>(msg);
    Peer& p = peer_for(mb + kMsgIdentity);
    const void* data = *reinterpret_cast<void**>(mb + kMsgData);
    uint32_t size = static_cast<uint32_t>(*reinterpret_cast<int*>(mb + kMsgSize));
    uint32_t h = hash_bytes(data, size);
    uint64_t r = p.r++;
    if (auto delivered = p.delivered.find(r); delivered != p.delivered.end()) {
        const bool same = delivered->second.size() == size &&
            (size == 0 || !memcmp(delivered->second.data(), data, size));
        p.delivered.erase(delivered);
        if (same) {
            const auto ct = p.copy_time.find(uint32_t(int64_t(r) + p.offset));
            if (ct != p.copy_time.end()) {
                const int64_t late_ms = (*reinterpret_cast<const int64_t*>(mb + kMsgTime) - ct->second) / 1000;
                if (late_ms > 30) {
                    p.stalls.fetch_add(1, std::memory_order_relaxed);
                    p.stall_ms.fetch_add(uint64_t(late_ms), std::memory_order_relaxed);
                    if (uint64_t(late_ms) > p.stall_max_ms.load()) p.stall_max_ms.store(late_ms);
                }
                p.copy_time.erase(ct);
            }
            p.dropped_late.fetch_add(1, std::memory_order_relaxed);
            p.a_verified.fetch_add(1, std::memory_order_relaxed);
            release(msg);
            if (p.aligned && !p.disabled) flush_held(p);
            return;
        }
        // A wrong copy may already have affected the world; it cannot be undone.
        // Still deliver the authoritative update instead of silently losing it.
        stop_copies(p, "delivered copy differed from reliable bytes; reliable update retained");
        p.aligned = false;
        p.votes.clear();
    }
    if (!p.aligned) {
        p.rel_hash[r % kHashRing] = h;
        if (!p.disabled && !p.copy_hash.empty()) {
            uint32_t newest = p.copy_hash.rbegin()->first, match = 0;
            int matches = 0;
            for (auto it = p.copy_hash.rbegin(); it != p.copy_hash.rend() && it->first + 256 > newest && matches < 2; ++it) {
                if (it->second == h) {
                    match = it->first;
                    matches++;
                }
            }
            if (matches == 1 && vote(p, int64_t(match) - int64_t(r)))
                p.next_seq = static_cast<uint32_t>(int64_t(r) + p.offset); // this message's S
        }
        if (!p.aligned) {
            g_ready.push_back(msg); // not synced: plain reliable stream
            p.a_plain.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    int64_t s = int64_t(r) + p.offset;
    auto ch = p.copy_hash.find(static_cast<uint32_t>(s));
    if (ch != p.copy_hash.end() && ch->second != h && !p.disabled) stop_copies(p, "a copy didn't match its reliable message");
    if (s < int64_t(p.next_seq)) {
        stop_copies(p, "sequence advanced without a verified delivered copy");
        p.aligned = false;
        p.votes.clear();
        p.a_unrecorded.fetch_add(1, std::memory_order_relaxed);
        g_ready.push_back(msg);
        p.a_plain.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (s > int64_t(p.next_seq)) {
        // Impossible with in-order reliable delivery; don't guess. Plain reliable stream from here.
        stop_copies(p, "numbering out of step");
        p.aligned = false;
        p.votes.clear();
        g_ready.push_back(msg);
        p.a_plain.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ready.push_back(msg);
    audit_given(p, uint32_t(s));
    p.a_rel.fetch_add(1, std::memory_order_relaxed);
    p.next_seq++;
    flush_held(p);
}

} // namespace

// --- steamnet.cpp interface ----------------------------------------------------------------------------

void dup_init(void* utils, void* alloc_fn)
{
    g_utils = utils;
    g_alloc = reinterpret_cast<Alloc_t>(alloc_fn);
    g_enabled = g_alloc != nullptr;
    g_sdk = swml_get_v2(swml());
    if (g_enabled && g_sdk && !g_sdk_provider) {
        swml_result result = g_sdk->transport_bind(swml_self(), &g_sdk_provider);
        swlog("SDK v2 transport: %s", result == SWML_OK ? "bound to NetworkFixes channel owner" : "unavailable");
    }
    swlog("net redundancy: %s", g_enabled ? "on (redundant tick copies, with players who have it too)"
                                          : "off (Steam AllocateMessage missing)");
}

bool dup_enabled() { return g_enabled; }

bool redundancy_active()
{
    DWORD last = g_last_active.load(std::memory_order_relaxed);
    return last && GetTickCount() - last < 2000;
}

// After the game sent a channel-0 message to a peer.
void dup_on_sent(void* self, const void* identity, const void* data, uint32_t size, Send_t send)
{
    if (!g_net_thread || GetCurrentThreadId() != g_net_thread) return; // only the game's network loop
    Peer& p = peer_for(identity);
    DWORD now = GetTickCount();
    if (p.last_send && now - p.last_send > kEpochIdleMs) { // (re)joined: the receiver syncs again
        sdk_disconnect(p);
        p.epoch += 2;
        p.recent.clear();
        // It may have come back without this mod: ask again instead of filling its unread channel 11 with copies.
        p.confirmed = false;
        p.hellos_sent = 0;
        p.manifest_sent = false;
        g_manifest_due = true;
    }
    p.last_send = now;
    if (p.next_send_seq == UINT32_MAX) {
        // Reserve the last value so next_seq never wraps while flushing copies.
        // Reliable ordinals continue monotonically; the new epoch learns its offset.
        sdk_disconnect(p);
        p.epoch += 2;
        p.next_send_seq = 0;
        p.recent.clear();
        p.manifest_sent = false;
        g_manifest_due = true;
    }
    uint32_t seq = p.next_send_seq++;
    if (!p.confirmed) {
        if (p.hellos_sent < kMaxHellos && now - p.last_hello > 1000) send_hello(self, p, send);
        return;
    }
    if (size <= kMaxCopy) {
        const uint8_t* b = static_cast<const uint8_t*>(data);
        p.recent.push_back(Copy{seq, hash_bytes(data, size), std::vector<uint8_t>(b, b + size)});
    }
    while (!p.recent.empty() && (p.recent.size() > size_t(kCopies) || p.recent.front().seq + kCopies <= seq)) p.recent.pop_front();
    if (p.recent.empty()) return;
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    if (p.last_copy_qpc && double(q.QuadPart - p.last_copy_qpc) * 1000.0 / double(f.QuadPart) < kMinCopyGapMs) {
        p.copies_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p.last_copy_qpc = q.QuadPart;
    p.copies_out.fetch_add(1, std::memory_order_relaxed);
    uint8_t buf[kMaxPacket];
    Header h{kMagic, kVersion, kTicks, 0, p.epoch};
    size_t off = sizeof(h);
    for (auto it = p.recent.rbegin(); it != p.recent.rend(); ++it) { // newest first: it always fits
        size_t need = sizeof(Entry) + it->data.size();
        if (off + need > kMaxPacket) break;
        Entry e{it->seq, it->hash, static_cast<uint16_t>(it->data.size())};
        memcpy(buf + off, &e, sizeof(e));
        memcpy(buf + off + sizeof(e), it->data.data(), it->data.size());
        off += need;
        h.count++;
    }
    memcpy(buf, &h, sizeof(h));
    send(self, p.identity, buf, static_cast<uint32_t>(off), kSendUnreliableNoNagle, kChannel);
}

// Game receive loop, channel 15 (polled first in every loop): read our channel.
// Client: tell every modded peer (the host) how many ticks are waiting in our queue, 4x a second.
void send_backlog(void* self, Send_t send)
{
    DWORD now = GetTickCount();
    if (now - g_last_backlog_report < kBacklogEveryMs) return;
    int depth;
    if (!playout_client_depth(depth)) return;
    // Map or editor open: the game simulates on the render thread at frame rate and falls behind by design; the host
    // slowing the world for everyone wouldn't help. No report: the host's pacing ages it out and eases back.
    if (map_inline_active()) return;
    g_last_backlog_report = now;
    uint8_t buf[sizeof(Header) + sizeof(uint32_t)];
    std::lock_guard lock(g_peers_mu);
    for (auto& [k, p] : g_peers) {
        if (!p->confirmed) continue;
        Header h{kMagic, kVersion, kBacklog, 0, p->epoch};
        uint32_t d = uint32_t(depth);
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), &d, sizeof(d));
        send(self, p->identity, buf, sizeof(buf), kSendUnreliableNoNagle, kChannel);
    }
}

// Our mod manifest to every peer that has this mod, once per session, reliably (version_check.cpp).
void send_manifests(void* self, Send_t send)
{
    if (!g_manifest_due.load(std::memory_order_relaxed)) return; // the network loop calls this every ~1 ms
    const std::string& m = version_manifest();
    std::vector<uint8_t> buf(sizeof(Header) + std::min<size_t>(m.size(), 4000));
    std::lock_guard lock(g_peers_mu);
    bool waiting = false;
    for (auto& [k, p] : g_peers) {
        if (p->manifest_sent) continue;
        if (!p->confirmed) continue; // its confirmation sets g_manifest_due again
        Header h{kMagic, kVersion, kManifest, 0, p->epoch};
        memcpy(buf.data(), &h, sizeof(h));
        memcpy(buf.data() + sizeof(h), m.data(), buf.size() - sizeof(h));
        if (send(self, p->identity, buf.data(), uint32_t(buf.size()), kSendReliable, kChannel) == 1) p->manifest_sent = true;
        else waiting = true;
    }
    g_manifest_due.store(waiting, std::memory_order_relaxed);
}

void dup_poll(void* self, Receive_t recv, Send_t send)
{
    g_net_thread = GetCurrentThreadId();
    send_backlog(self, send);
    send_manifests(self, send);
    lod_poll(self, send);
    push_poll(self, send);
    sdk_poll(self, send);
    void* msgs[64];
    for (int batch = 0; batch < kPollBatches; ++batch) {
        int n = recv(self, kChannel, msgs, 64);
        for (int i = 0; i < n; i++) {
            on_packet(self, static_cast<const uint8_t*>(msgs[i]), send);
            release(msgs[i]);
        }
        if (n < 64) break;
    }
}

// Game receive loop, channel 0: the merged stream. Returns max when more is waiting, so the game asks again.
int dup_receive_ch0(void* self, void** out, int max, Receive_t recv)
{
    if (int(g_ready.size()) < max) {
        void* msgs[64];
        int n = recv(self, 0, msgs, std::min(max, 64));
        for (int i = 0; i < n; i++) on_reliable(msgs[i]);
    }
    int k = 0;
    while (k < max && !g_ready.empty()) {
        out[k++] = g_ready.front();
        g_ready.pop_front();
    }
    return k;
}

bool dup_peer_reporting(const void* identity, DWORD max_age_ms)
{
    Key k;
    memcpy(k.b, identity, 16);
    std::lock_guard lock(g_peers_mu);
    auto it = g_peers.find(k);
    if (it == g_peers.end()) return false;
    DWORD at = it->second->backlog_at.load(std::memory_order_relaxed);
    return at && GetTickCount() - at <= max_age_ms;
}

int dup_max_peer_backlog(DWORD max_age_ms, int& peers)
{
    peers = 0;
    int worst = -1;
    DWORD now = GetTickCount();
    std::lock_guard lock(g_peers_mu);
    for (auto& [k, p] : g_peers) {
        DWORD at = p->backlog_at.load(std::memory_order_relaxed);
        if (!at || now - at > max_age_ms) continue;
        peers++;
        worst = std::max(worst, p->backlog.load(std::memory_order_relaxed));
    }
    return worst;
}

void dup_log_status()
{
    if (uint64_t out = g_lod_sets_out.exchange(0), in = g_lod_sets_in.exchange(0); out || in)
        swlog("net lod 5s: %llu far sets sent, %llu received", static_cast<unsigned long long>(out), static_cast<unsigned long long>(in));
    std::lock_guard lock(g_peers_mu);
    for (auto& [k, p] : g_peers) {
        int i = p->index;
        uint64_t rescued = p->rescued.exchange(0), late = p->dropped_late.exchange(0), in = p->copies_in.exchange(0);
        uint64_t stalls = p->stalls.exchange(0), stall_ms = p->stall_ms.exchange(0), stall_max = p->stall_max_ms.exchange(0);
        uint64_t out = p->copies_out.exchange(0), skipped = p->copies_skipped.exchange(0);
        if (out || skipped) {
            swlog("net redundancy peer%d 5s: %llu copy packets out, %llu skipped (at most one per %.0f ms)", i,
                  static_cast<unsigned long long>(out), static_cast<unsigned long long>(skipped), kMinCopyGapMs);
        }
        uint64_t a_copy = p->a_copy.exchange(0), a_rel = p->a_rel.exchange(0), a_plain = p->a_plain.exchange(0);
        uint64_t a_gap = p->a_gap.exchange(0), a_rep = p->a_repeat.exchange(0), a_ver = p->a_verified.exchange(0);
        uint64_t a_unrec = p->a_unrecorded.exchange(0), a_res = p->a_resyncs.exchange(0), a_ep = p->a_epochs.exchange(0);
        if (a_copy || a_rel || a_plain) {
            swlog("net merge audit peer%d 5s: channel 0 to the game: %llu (%.1f/s) = %llu from copies + %llu reliable while "
                  "synced + %llu plain while not; order: %llu gaps, %llu repeats; reliable twins of copies: %llu matched, %llu "
                  "without a copy record; resyncs %llu, epoch changes %llu; %s",
                  i, static_cast<unsigned long long>(a_copy + a_rel + a_plain), (a_copy + a_rel + a_plain) / 5.0,
                  static_cast<unsigned long long>(a_copy), static_cast<unsigned long long>(a_rel),
                  static_cast<unsigned long long>(a_plain), static_cast<unsigned long long>(a_gap),
                  static_cast<unsigned long long>(a_rep), static_cast<unsigned long long>(a_ver),
                  static_cast<unsigned long long>(a_unrec), static_cast<unsigned long long>(a_res),
                  static_cast<unsigned long long>(a_ep), p->disabled ? "copies OFF" : p->aligned ? "in sync" : "syncing");
        }
        if (in == 0 && rescued == 0) continue;
        swlog("net redundancy peer%d 5s: %llu copies in, %llu updates taken from a copy (%llu reliable duplicates "
              "dropped); %llu updates the reliable stream delivered > 30 ms late (avg %.0f ms, max %llu ms) came from "
              "copies instead; %s",
              i, static_cast<unsigned long long>(in), static_cast<unsigned long long>(rescued),
              static_cast<unsigned long long>(late), static_cast<unsigned long long>(stalls),
              stalls ? double(stall_ms) / double(stalls) : 0.0, static_cast<unsigned long long>(stall_max),
              p->disabled ? "copies OFF after a mismatch" : p->aligned ? "in sync" : "syncing");
    }
}
