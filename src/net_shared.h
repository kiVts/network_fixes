#pragma once
#include <windows.h>
#include <cstdint>

using Send_t = int(__fastcall*)(void* self, const void* identity, const void* data, uint32_t size, int flags, int channel);
using Receive_t = int(__fastcall*)(void* self, int channel, void** out, int max);

// steamnet.cpp -> playout.cpp: Steam's receive time (SteamNetworkingMessage_t::m_usecTimeReceived) of every
// channel-0 message handed to the game (one per server tick on a client). Game network thread.
void playout_on_arrival(int64_t usec_received);
bool playout_wants_arrivals();
void playout_take_arrivals(uint64_t& n, double& longest_gap_ms); // mp_watch.cpp

// redundancy.cpp (game network thread, except dup_log_status / redundancy_active).
void dup_init(void* utils, void* alloc_message_fn);
bool dup_enabled();
bool redundancy_active(); // redundant copies arrived in the last 2 s
void dup_on_sent(void* self, const void* identity, const void* data, uint32_t size, Send_t send);
void dup_poll(void* self, Receive_t recv, Send_t send);
int dup_receive_ch0(void* self, void** out, int max, Receive_t recv);
void dup_log_status();

// Host pacing (pacing.cpp). Client: playout.cpp's latest tick queue depth, if this is a client (catch-up ran in the
// last second). Host: the largest backlog a peer reported within max_age_ms (-1 if none), and how many reported.
bool playout_client_depth(int& depth);
// Client: the game simulates on the render thread (full-screen map or another screen such as the editor in front).
bool map_inline_active();
int dup_max_peer_backlog(DWORD max_age_ms, int& peers);
// Host: this peer sent a backlog report within max_age_ms, i.e. it is in the game and simulating (a joining player
// downloading the world, or one with the map / editor open, doesn't report).
bool dup_peer_reporting(const void* identity, DWORD max_age_ms);

// steamnet.cpp: bytes received on channels 1..7 so far (session data: world snapshot, vehicles, mod files).
uint64_t net_bulk_received();
// steamnet.cpp: the largest send queue Steam reports among the connected peers, in ms (-1: none), and among the peers
// that are in the game (dup_peer_reporting). Updated 5 times a second.
int net_send_queue_ms();
int net_send_queue_playing_ms();

// remote_push.cpp (host): where the remote players' characters were put in the last 500 ms; returns how many (<= max).
int remote_character_positions(double out[][3], int max);

// object_push.cpp <-> redundancy.cpp (network thread; object_push locks its own state).
bool push_enabled();
bool push_hosting();                                            // on, and this PC hosts
void push_on_host_cap();                                        // client: the host said it follows pushed objects
uint32_t push_client_payload(uint8_t* out, uint32_t cap);       // client: the newest batch of owned objects, once
void push_on_states(int peer, const uint8_t* data, uint32_t size); // host: a client's batch
uint32_t push_take_denies(int peer, uint8_t* out, uint32_t cap);   // host: refusals for this peer
void push_on_denies(const uint8_t* data, uint32_t size);           // client: refusals from the host

// version_check.cpp: this machine's mod manifest (built once), and a peer's manifest received on channel 11.
#include <string>
const std::string& version_manifest();
void version_on_manifest(int peer, const char* data, size_t size);

// Pallet/native box states omitted by vanilla's cylinder-only object replay.
uint32_t push_host_payload(uint8_t* out,uint32_t cap);
void push_on_host_states(const uint8_t* data,uint32_t size);

uint32_t push_client_force_payload(uint8_t* out,uint32_t cap);
void push_on_forces(int peer,const uint8_t* data,uint32_t bytes);

// env_sync.cpp <-> redundancy.cpp (network thread): shared sea state and the tornado weather fix.
bool env_available();                                        // installed on this build
bool env_hosting();                                          // this PC hosts (or plays alone)
uint32_t env_host_payload(uint8_t* out, uint32_t cap);       // host: ocean wind target + flags (12 bytes), or 0
void env_on_host(const uint8_t* data, uint32_t size);        // client: the host's
void env_set_peers_capable(bool all);                        // host: every connected player has it
