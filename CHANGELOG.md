1.11.3: multiplayer workbench desync and catch-up stability. Preserves host pacing depth reports while the client is inside a workbench or map (`map_inline_active() && depth <= 14`), preventing the host pacing timer from surging to 62.5 ticks/s and causing runaway backlog desync. Dynamic catch-up headroom scales the pacing ceiling to 40 ms (`catchup_max_tick_ms`) when a client falls behind (> 60 ticks), and the baseline pacing floor is raised to 33 ms (`max_tick_ms`). Redundancy resync now monotonically prunes stale past delivered twin receipts in `vote()` and `on_reliable()`, preventing permanent resync lockout after sequence gaps. Playout buffer decision state and game pointer are bound to thread-local storage, eliminating cross-thread race conditions during catch-up. Peer identity keys are expanded to 32 bytes, supporting direct IPv6 and Tailscale routes without `/64` subnet collisions.

1.11.2: stability audit. A reliable host update is suppressed only when its exact payload was already delivered as a redundant copy; a mismatch stops copies, reports a fault and retains the reliable update. Receipts survive copy-epoch resets. Copy queues and each poll round are bounded, malformed bundles are rejected before delivery, and sequence rollover starts a new epoch. Steam initialization now participates in shutdown synchronization. Editor catch-up suppression works even if the native replay cap cannot install, and the cap's thread scope is cleared after a native exception. Already-applied incorrect copies cannot be rolled back; live multiplayer soak testing remains necessary.

1.11.1: fixes a stutter on a joining player that could start after a lag event and last until they left the game: updates alternating between playing nothing and playing up to 9 host ticks at once. The playout buffer kept the pace it measured while the host was slow and could never measure a new one, because the queue overflowed first. It now forgets that pace with the backlog and stops trusting it as soon as ticks pile up. Only the joining player needs this version.

1.11.0: the touch census (`[touch]`), the client's 180 Hz push substeps (`[push] client_physics_hz`), the vehicle push force and their traces moved here from Physics Fixes. They still run inside its physics hooks, so pushing needs both mods, as before.

1.10.0: provides the SDK v2 namespaced application-message transport through this mod's existing channel-11 receiver. Negotiated peers receive reliable, bounded SDK messages; application callbacks run on the game lane. Session generations reject stale endpoints, Steam backpressure retains unsent packets, and accepted sends complete without replay. Native tick-copy/push message types are unchanged. Both communicating peers need the v2 core and this transport for SDK messaging; ordinary network fixes retain their existing compatibility. See [the SDK guide](../../sdk/README.md) for API and application acknowledgement requirements. Adapter tests use simulated Steam IO; live remote validation remains necessary.

1.9.11: fixes the recorded character launch when a client-owned roller reaches water or relinquishes ownership. Suppressed native targets remain cached; handoff rebases root bodies to those targets and passive wheels coherently through native direct-transform events before restoring driven flags. The driven target is set to the same pose, preventing a stale network correction from becoming collision velocity. Physics Fixes 0.25.10 and its read-only launch recorder are retained.

1.9.12: first implementation of background editor scheduling, OFF by default. [net] editor_background=1 audits
preview/live game ownership without changing scheduling. Mode 2 is a development prototype: vehicle editor only,
two exact game builds, distinct preview game world required. It reuses the native update worker, moves the editor
callback to each main-thread frame, suppresses its replay-thread invocation and joins before state exit/shutdown.
Shared preview/live worlds are refused and keep 1.9.11's inline fallback. Worker, blocked-replay, zero-tick,
startup-failure and teardown tests pass. Scene/asset ownership and multiplayer validation are unfinished;
this is implementation groundwork, not a completed fix for ordinary shared-world editors.

1.9.11: client editors no longer run the map's extra catch-up calls. The first native replay batch is also limited
to one host tick per editor frame (previously up to ten, outside map_catchup_ms's control). All host updates stay
queued and normal catch-up resumes on exit. [net] editor_catchup_ticks=0 restores the old behavior. The native cap
redirect is checked by instruction pattern and scoped to the editor's thread; hosts and fullscreen maps keep their
existing behavior. This bounds replay batches, not the cost of a single world tick or spawn; it does not move world
simulation off the render thread. Native stub and transition tests pass; multiplayer editor validation is pending.

1.9.10: held movable objects are excluded from touch ownership, host steering and pallet snapshots so native pickup has sole control. Physics Fixes includes a read-only 32-call local-character launch history (velocity, forces, facing, held item and contacts), dumped on abnormal walking speeds. This is diagnostic coverage for the reported roller launch, not a verified launch fix.

1.9.9: large vehicles retain host authority. The client relays its solved horizontal contact reaction (normal plus friction) for unowned vehicle bodies; the host applies a bounded force at the contact point. No vehicle-size ownership limit is raised. Forces expire after 150 ms, total at most 1500 N per player, and require a nearby remote character and a point within the host body AABB. [push] large_vehicles=0 disables the relay in Network Fixes.

1.9.8: synchronize nearby native movable boxes (including pallets) from host to clients. Vanilla only replays cylinders. Unowned boxes follow host snapshots through native queued transforms and velocities; active client owners ignore host echoes. Snapshots are bounded, MTU-sized, ordered per object and expire after 400 ms. Requires Physics Fixes 0.25.8 for stable client ground contacts.

1.9.7: compatible multiplayer pushing enables host-like 180 Hz collision substeps while the client walks. Native client physics otherwise hard-codes 60 Hz even when the host and UI use 180 Hz. Elapsed time is unchanged. Physics setting [push] client_physics_hz=0 disables refinement. Original GJK and native solver A/B tests did not prevent sinking; analytic contacts and parallel solver are restored.

1.9.6: enable collision filters for passive wheel helpers too (the replay confirmed their client mask was zero).
Physics Fixes 0.25.5 adds temporary push-support contact diagnostics under [touch] support_audit=1.

1.9.5: ordinary pallet boxes are claimed on first contact, without resetting their native velocity. They already
simulate locally; the replay-stillness gate otherwise rejected the push that was setting them in motion.

1.9.4: ownership now changes collision filters as well as body flags, using the native queued remove/reinsert event.
Client vehicle bodies are created with group 0x80 and mask 0x20, which permits character collisions but excludes
terrain. Clearing the driven flag alone made them fall through the floor. Their original filters are restored at
release. Native movable boxes are now included: the previous 0xa3 test selected cylinders and rejected pallets.
The 1.9.3 tile-editor scenery lookup was the wrong container and has been removed. Live ownership logs include the
actual body flags and group/mask after the physics queue applies the changes.

1.9.3: experimental separate-scenery lookup, superseded by the native movable-box lookup in 1.9.4.
Direct transform suppression and the passive wheel assembly handling remain.
Small vehicles include their entire joint-connected assembly. Passive native wheels outside the vehicle body-record
ring no longer cause a blanket refusal. Their own dynamics stay intact on each PC; only bodies with actual network
IDs are sent to the host. Driven/kinematic external anchors, static anchors and oversized assemblies stay refused.
Standing on a replay-driven vehicle still does not initiate ownership. In-game validation is pending.

1.9.2: object push no longer takes over a unit joined to bodies outside it (a bench's wheels: they kept following the
host's late states and flung it; `[push] outside_joints=1` for the old behaviour), nor what your character stands on.
With physics_fixes 0.25.3 it also counts every body your character touches and logs those in neither of the game's lists
("touched a body in neither list"), to find out what pallets are.

1.9.1: exports `swml_remote_characters` (remote players' characters, for physics_fixes' host puppet rule); object push logs
why touched objects / vehicles weren't claimed ("not claimed (...)" lines and a 30 s count per reason) and, for the first
claims, joints leading out of the unit.

1.9.0: clients push small vehicles too (`[push] vehicles=1`): carts, small boats on land, anything up to 8 m and 10
bodies that nobody drives. A client's copy of a vehicle replays the host's 0x81 targets the same frozen way as a pallet, so
the vehicle is taken over as one unit (every body, a deny for any body hands back all of them). A vehicle of several
bodies is only claimed when Bullet joints link all of them on the client (else they would fall apart); a vehicle refused
for that is logged. Not while seated, and nothing whose lowest point is under 1 m above the sea (buoyancy stays the
host's); reaching the water hands it back at once.

1.8.0: clients push loose objects (`[push] objects=1`). A client's copy of a pallet or crate replays the host's states as
a frozen body (collision flag 0x800: no velocity, no gravity, contacts not written back), so walking into it was walking
into a wall. While a client's own character touches one, that client now simulates it (the host's targets for it are
dropped, the flags cleared, the body woken) and sends its state every tick; the host steers its own copy onto it and
keeps sending it to everyone. Released 1 s after the last touch at rest. The host refuses a claim when another player
has it, its own character touches it or the copies are too far apart. Both PCs need this version. "push watch" lines
(host) log any loose object that drops over 0.5 m next to a remote player. "object push 30s" lines.

1.7.5: own animation is client-side (`[input] local_animations=1`). A client's character animation only changed when
the host's set_animation_state echo came back (one round trip + playout late; the event's handler is the only thing
that writes a character's animation). Now the client's own request is applied to its own character at once, and the
host's echoes of it are skipped.

1.7.4: Steam's packets capped at 1200 bytes (`[net] mtu_packet_size`). With direct connections on, Steam picked a
route over Tailscale (IPv6, MTU 1280) where every packet over 1280 bytes was lost: pings and acks got through, the
world download delivered 0 KB/s and the join timed out (2026-10-31 21:20). Steam sends up to 1300 bytes of payload.

1.7.3: measurements for less data per tick (analysis/TICK_DATA_PLAN.md stage 0, log only): "tick bytes 5s" (host:
bytes per player per tick on channels 0 and 1 by message type, and how many vehicle corrections repeat a pose already
sent to that player) and "tick compress 5s" (every 32nd channel-0 message compressed at libdeflate levels 1 and 6).
`[measure] tick_bytes`, `tick_compress`.

1.7.2: host pacing also watches the upload: a playing player's send queue over `pacing_queue_ms` (250) counts as that
many ticks behind, so the world slows slightly instead of that player falling 20-56 s behind (2026-10-31: the friend's
own queue was empty while 8 MB waited in the host's Steam buffer, and pacing saw nothing). Only players that report
(in the game) count, so a joining player's world download doesn't slow anyone. "host pacing" lines show the send queue.

1.7.1: vehicle corrections follow the upload. 1.6.0's every-tick corrections (5x the game's) took the host's tick stream
to the friend from ~4 KB/s to ~700 KB/s once 81 vehicle bodies were next to them (2026-10-31), over a Steam relay
carrying 620-820 KB/s: an 8 MB send queue, everything 8-12 s late on their side (vehicles appearing a few at a time,
teleports and respawns not answering, walking in bursts). Now the interval rises step by step while any player's send
queue is over 100 ms (up to every 10 ticks) and comes back down once it stays empty (`[net] correction_max_ticks`,
`correction_queue_ms`; "vehicle corrections:" lines).

1.7.0: own echo (`[input] skip_own_echo`, `local_seat_look`) and local first (`[input] local_seats`, `local_buttons`,
`local_tool_sounds`, `local_ladders`, `local_scopes`, `local_doors`) for clients. Fixes from the first session
(2026-10-31): own echo now finds your character as the local peer's entry in the player ring (it tested +0x288, which
a client's own character never has: 0 echoes were skipped, walking and turning still came back a round trip late);
tool sounds copy the sound's name (it was taken for a position: the early sound read the request's freed text and its
echo never matched, so each played twice); "local first 30s" also reports copies that aged out; host pacing never runs
on a client (joining right after hosting, the client waited 6.6-7.5 ms per tick for 8 s with the host's old period).

1.6.3: a client with the full-screen map or editor open stops reporting its backlog, so host pacing no longer slows the world for everyone while they look at the map / build.

1.6.2: host pacing's wait shows in the F1 profiler as its own zone, "Waiting for Client", instead of idle time.

1.6.1: playout buffer 1 tick (min 1, max 20 ms) instead of 2-3 ticks and up to 150 ms: ~20-30 ms less delay for a joining player; a late update shows as a short freeze. `playout_max_ms=150` for the old behaviour.

1.6.0: host sends nearby vehicles' corrections every tick instead of every 5th, with a reach tick 1 ahead instead of 5: clients (modded or not) show nearby vehicles, their own included, ~64 ms sooner. `[net] correction_min_ticks` (5 = the game's).

1.5.10: runs on the 2026-10-28 game build (0x6ae218ec) through the SDK port table; the main loop grew 48 bytes (map_catchup range check). Rebuild the friend kit too.

# Network Fixes (host)

Smoother multiplayer for players with high ping, fast mod downloads when joining, and no long
slowdown while a big vehicle spawns.

- **Adaptive send rate.** Steam sends at one fixed speed (default 256 KB/s per player) and has no
  congestion control. If a player's connection can't take that speed, packets are lost and Steam
  resends them over and over: measured on a 200 ms ping connection, Steam put ~4 MB/s on the wire
  but only 110-190 KB/s arrived. This mod measures what each player actually receives and keeps the
  send rate there: it doubles the rate every second while everything arrives, settles on the measured
  capacity once it doesn't, and probes a little higher every few seconds.
- **Game updates first.** Steam sends everything to a player in one queue, in order. When a ship
  spawns, the game used to drop megabytes of vehicle data into that queue at once, and every game
  update behind it waited until all of it was sent: at high ping, seconds of a frozen and then
  fast-forwarding game. Now bulk data (vehicles, mod files) waits in the game's own queue and is fed
  to Steam a little at a time (at most ~40 ms of sending ahead), so game updates get through within
  ~40 ms. The game also cuts messages into 16 KB pieces instead of 511 KB ones, so an update never
  waits behind half a megabyte. Players without the mod receive these pieces normally.
- **Clients can push things.** The game moves other players' characters by teleporting them every
  tick and zeroing their velocity, so on the host (which owns all physics) a client walking into a
  crate or small boat carried no momentum: only the host could push things. Other players'
  characters now keep their walking speed (up to 15 m/s; teleports and respawns excluded). Only the
  host needs it, and only the host does it. Off with `remote_push=0` under `[push]`.
- **1 ms network loop.** The game's network thread is meant to check for messages every 1 ms, but
  on Windows its `Sleep(1)` lasts a whole timer tick (usually 15.6 ms). Every message waited up to
  that long on each PC before being sent or read, and updates arrived in uneven bunches. The log line
  `network loop Sleep(1) takes ... ms` shows what it was on your PC.
- **Redundant game updates** (needs this mod on both ends). Steam delivers everything in order, so
  one lost packet stops the game until it has been resent, at least one round trip later: at 200 ms
  ping a ~250 ms freeze, then a fast-forward. Every small game update (the host's per-tick update,
  the player's per-tick inputs) is now also sent a second way that doesn't wait for resends, bundled
  with the two before it, so a lost packet costs 16 ms instead. The game receives exactly the same
  messages in the same order as before, just without the wait; each copy is checked against the
  real message. Offline test at 200 ms ping and 2% loss: worst 1% of updates 347 -> 115 ms late.
  Players without the mod get nothing extra.
- **Playout buffer** (joining players). The game plays the host's updates with no safety margin, so
  any late update freezes it, then it plays up to 10x fast to catch up. This keeps just enough
  updates queued to cover the jitter measured on the connection and changes the game speed slightly
  (0.8x..1.5x) instead. It only steers while redundant updates arrive (without them it would add
  delay for little gain); otherwise it only logs (`playout 10s: ...`).
- **Drift measurement** (joining players, off by default: `[forecast] measure=1`). Logs only, changes
  nothing. Each time the host corrects a vehicle, it compares the host's position for each body with your
  PC's own and logs the error every 10 s (`forecast measure 10s: ...`): moving and resting bodies, in cm
  and degrees, as is and extrapolated by 1 and 2 ticks. This is stage 0 of `analysis/FORECAST_PLAN.md`
  (client prediction): the drift it measures is the smallest correction prediction could ever reach.
- **How far behind each player is** (host). Every player's game reports how many of the host's updates it
  has received but not played yet (the "+Nf" in the player list). The host now logs it every 5 s while anyone
  is behind (`players 5s: ...`, once a minute otherwise), and `player N fell behind` / `caught up` when it
  crosses 2 s. Works for players without any mod.
- **Version check** (both ends). Players with this mod tell each other which swml mods and versions they run,
  plus the few settings that change the simulation itself. `versions: peerN ...` in `network_fixes.log` lists every
  difference; `MUST MATCH` marks a mod every player needs (game data) that one end lacks or runs in another
  version. Other differences are informational: a joining player's game replays the host's physics.
- **Map catch-up** (joining players). While the full-screen map is open the game simulates on the drawing
  thread, one round per frame, so a player who only just keeps up falls behind the moment the map opens
  (2026-10-23: 5 fps for 95 s, then a rejoin). Now, while more than 14 updates are waiting, it plays more
  rounds per frame, up to `map_catchup_ms` (100 ms) of the frame. `map open:` lines show when it happens.
- **Pushing things, smoother** (1.4.0). The push velocity is measured over the last 5 ticks instead of 1: the
  host's copy of a remote player moves in uneven steps (its transforms arrive every 5th tick over the network),
  and one-tick steps turned into jolts of up to 15 m/s that shoved pallets and crates in ways the player never
  did. A player riding something faster than 15 m/s is left alone (it used to be pushed back into the deck).
- **Distant AI logic in multiplayer** (1.5.9). With physics_fixes 0.21+ and its `[lod] logic` on, every PC says so on
  channel 11 once a second, and the host sends the vehicles it lets run their logic every Nth tick (with its tick)
  about 10 times a second; each client decimates exactly those. The host only does it while every connected player
  runs it. `net lod 5s:` lines count the sets sent and received.
- **Behind episodes and host tick numbers** (1.5.6). A joining player 30 ticks behind (the player list's "desynced")
  gets `behind:` lines: the last 3 s of catch-up calls in 250 ms steps (host ticks received, ticks simulated, apply and
  simulate time, queue, calls on the render thread, longest call), then a summary when it is over. `mp host 5s`,
  `mp client 5s`, `mp stall` and the episode lines name the host tick, so the host's and the player's logs line up
  exactly whatever their clocks say.
- **Gap reports** (1.5.5). `net gap peerN:` the moment channel 0 from a player goes quiet for over 500 ms (the host's
  tick updates on a joining player, a player's inputs on the host): what Steam says about the connection right then
  (ping, quality, packets and KB/s both ways, what waits to be sent, unacked, queue time), and when it resumed. Steam's
  own warnings now always reach the log (`steam: [4] ...`, at most 30 lines a minute).
- **"CATCHING UP..." on screen** (1.5.4, joining player). Red text at the top middle while your game replays a backlog of
  the host's ticks (more than 30 queued; hidden again at 14): after joining, a stall, or with the map open. Real text
  (a TrueType font through Windows, drawn over the frame). `[mp] catchup_banner=0` turns it off.
- **Join timeline** (1.5.3, joining player, `[mp] log`). `join:` lines from the moment the game reports "loaded" to
  the host (from then on every host tick waits for this player to replay it): when the first tick was simulated,
  how much world / vehicle data came in and when it stopped, the backlog's peak, and when it caught up, or
  `STILL BEHIND` after 3 minutes.
- **Faster joins at high ping** (1.5.2). The send rate controller misread every transfer's first second at
  216 ms ping: about one round trip of data is on the wire before any of it is acknowledged, so it looked like a
  slow path and the rate settled at 192-256 KB/s on a connection that carries 700-1000. A joining player's world
  and vehicles (~4 MB) took ~11 s instead of ~4-5 s, and every second of that is host ticks they replay after
  loading. Now bytes in flight count as in flight, and a session starts near the capacity the last one measured.
  The joining player's playout buffer also ignores the burst of updates that arrives after loading (it used to
  read it as 1.5-2.6 s of jitter and buffer the maximum for ~20 s after every join).
- **Audit fixes** (1.5.1). Remote push runs only on the host (a client's copies of other characters are puppets, and
  loose objects are the host's). Host pacing is off by default (`host_pacing=0`): a last resort, it slows the world for
  everyone. `net channels 5s` now includes this mod's own channel 11 (copies, hellos, reports, manifests), which also
  counts toward the send rate controller. `players 5s` ticks/s is measured over the real time between looks, and
  `mp host 5s` is only logged while players are connected.
- **Stress-test timing** (1.5.0, both ends, `[mp]`). Where each tick goes, every 5 s: `mp host 5s` (host tick,
  simulate, players' requests, joins), `mp client 5s` (host updates arrived and the longest gap, ticks simulated,
  queue, catch-up calls, apply and simulate per tick), `mp stall` for any tick or catch-up over 250 ms, and
  `net channels 5s` (per player and channel both ways). How to read them: `analysis/LOGGING.md`. The audit
  (`[audit] enabled`) is now off by default.
- **Send buffer** (default 512 KB): a bigger buffer keeps file transfers flowing.
- **Receive buffer** (default 1 MB): more room when you are the one joining.
- **No Nagle delay** on the game's event messages (Steam holds them up to 5 ms by default).

Updates first and the 1 ms loop help on both ends: on the host for what it sends to players, on a
player's PC for what that player sends (e.g. a vehicle spawned from their workbench) and for how
fast received updates are picked up. The send rate matters only on the host, for players who join
after the game started with this mod. Steam's rate setting is global, so with several players
downloading at once the slowest one sets the pace.

`network_fixes.log` shows the rate decisions (`net: send rate ...`), a status line per busy player every 5 s
(including how many bulk sends waited so updates could go first), a summary after every big transfer
(`transfer done: ... KB/s`) and the network loop timing every minute (`net loop 60s: ...`).

Settings (settings.ini, [net]): `send_buffer_kb`, `recv_buffer_kb`, `rate_control` (1 adaptive,
0 fixed `send_rate_kb`), `start_rate_kb`, `min_rate_kb`, `max_rate_kb`, `pace_ms` (0 = updates first
off), `piece_kb` (0 = game's 511 KB pieces), `net_loop_us` (0 = game's Sleep(1)), `nagle_us`
(-1 = Steam default), `redundancy` (0 = off), `playout` (1 = with redundancy, 2 = always, 0 = log
only), `playout_min_ticks`, `playout_max_ms`, `map_catchup_ms` (0 = off). `net redundancy peerN 5s:` lines count how many updates
came from the copies and how late the normal stream would have been for them. `[forecast] measure`
(0 = off) turns on the drift measurement.
