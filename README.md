# Stormworks Network Fixes

The host fullscreen map now supports bounded simulation batches on the drawing thread, helping the world keep pace when the map renders below the simulation rate. The default host_map_budget_ms=32 limits extra work, with at most four ticks per frame; 0 restores vanilla behavior. Host pacing still applies. An overloaded world or an expensive native tick can still slow or stall simulation. Automated checks pass; live map/world validation remains pending.

The stability update verifies exact delivered copies before suppressing reliable updates, bounds receive work and copy queues, synchronizes network startup with shutdown, and hardens editor catch-up handling. Detected mismatches are reported through SWML. Already-applied incorrect copies cannot be rolled back, and a single expensive world tick can still delay an editor frame. Regression tests pass; live multiplayer soak testing remains pending.
Smoother multiplayer for players with high ping, fast mod downloads when joining, and no long slowdown while a big vehicle spawns.

Vanilla sends everything to each player at one fixed speed through one queue. If a connection can't keep up, Steam resends packets over and over: on a 200 ms connection, Steam put about 4 MB/s on the wire while only 110-190 KB/s arrived. A single ship spawn could also block every game update behind megabytes of vehicle data.

- **Adaptive send rate:** measures what each player actually receives and sends at that speed
- **Game updates first:** vehicles and mod files are sent a little at a time, so updates get through in about 40 ms
- **Lost packet recovery:** one lost packet no longer freezes the game for a round trip
- **Steady playback:** clients play ticks at an even pace instead of freezing and fast-forwarding
- **Fresher vehicles:** nearby vehicles are corrected up to every tick instead of every 5 when the connection can take it
- **Faster network loop:** 1 ms instead of Windows' usual 15.6 ms
- **Pushing:** other players' characters keep their momentum on the host, so clients can push crates and small vehicles

The host needs this mod for most of it. Players without it can still join. Features like lost packet recovery and steady playback need it on both ends.

Client pushing of objects and small vehicles needs [Physics Fixes](https://github.com/kiVts/physics_fixes) as well, on both PCs.

This will likely trigger your antivirus, but there are no viruses in here. If a game crashes feel free to make an issue here and [join the discord](https://discord.gg/SsT4Y6Y3Zm) so the process to fixing them is quicker.

**THIS IS A  WIP MOD. IT IS UNSTABLE.**

To use, install [SWML](https://github.com/kiVts/swml) and add the zip from releases here. I also recommend installing [No Crash Uploads](https://github.com/kiVts/no_crashlog_upload) so you don't upload a crash report with an injected DLL to Geometa (a crash is overwhelmingly likely to be due to one of these mods). Consider other mods as well!

[Physics Fixes](https://github.com/kiVts/physics_fixes)
[Rotation Keybinds](https://github.com/kiVts/editor_rotation_rings)
[Composite Tracer](https://github.com/kiVts/composite_tracer)

Like what I do?

<a href="https://buymeacoffee.com/kivts">
  <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" width="120" alt="Buy Me a Coffee">
</a>

## Turret and laser diagnostics

For a turret/laser disagreement, enable the temporary read-only aim trace on the
host and every observing client, using the same mod version. Add this to this
mod's `settings.ini`, then restart the game:

```ini
[aim_trace]
enabled=1
every_ticks=6
vehicle_id=0
```

Start with the same settings that showed the problem. With the client standing
clear of the turret, the host should aim down, hold for five seconds, aim up,
hold again, and repeat. Note the time and whether the physical turret head also
points differently, or just the beam. Keep a recording of both views if possible.
The trace compares internal aim, rotation inputs, stabilization, component mount
orientation and body/correction transforms at shared host ticks. It records all
sub-bodies of vehicles containing camera/laser components. It does not change
simulation state or need Physics Fixes to be enabled.

Check `logs/mods/network_fixes.log` for `aim trace: on` and increasing `records`
in `aim trace status`. `faults` should stay zero; `dropped` or `truncated` means
coverage is incomplete. Nonzero `busy_skips` also means a concurrent sample was
skipped. Stay in the live world during the aim test. In a busy world, set `vehicle_id` to the affected vehicle's
ID from the initial trace and restart. Builds without verified field layouts
leave the trace off and explain why in the log.

After reproducing, preserve each machine's `network_fixes.log`, `physics_fixes.log`
if enabled, and `logs/swml.log`, labelled host/client and player name. Include the
affected vehicle file or Workshop link. Set `enabled=0` afterwards and restart;
traces are verbose. A restart archives previous logs, so collect them first.

## Control-loop diagnostics

For train ECU/control oscillation, update this mod and Physics Fixes, then enable
this capture in **Network Fixes** `settings.ini` on host and observers and restart:

```ini
[logic_trace]
enabled=1
vehicle_id=0
every_ticks=1
max_mb=256
```

Zero selects train vehicles automatically. A specific `vehicle_id` selects that
vehicle instead. Use the configuration that exhibits the problem: the recorder
does not switch logic modes. It records published body velocity and native port
values before/after component execution, including numeric/bool ports and every
number channel and bool bit in composite ports. Worker, unit, execution order,
shared host tick and actual logic scheduling/fallback reason accompany records.
Native-loop fallback has only whole-tick snapshots. Private Lua/PID state is not
included, so a differing output locates an observed boundary rather than proving
the underlying defect by itself.

Files are `logs/logic_trace_<UTC timestamp>_<pid>.jsonl`. Preserve them alongside
the network, physics and loader logs and the affected train file/Workshop link.
Keep a short recording of the set speed, measured speed/RPS, fuel, air and brake
outputs during the symptom. Collect the files from every participant.

The writer runs separately and never makes the game wait for disk output.
Unchanged signals are compressed with periodic keyframes. Bounds are 128
components, 64 supported ports/component, 1024 ports total and 128 bodies; queues
have 16 producer lanes of 4096 records. Status records report dropped data, read
faults, truncation, skipped concurrent captures, unsupported ports and observed
capture-boundary cost. Nonzero loss/fault/truncation makes coverage incomplete.
The file-size limit stops capture and is reported in the network log. If no
components are selected, specify the train's vehicle ID. Disable capture after
the reproduction. Discovery refuses unverified layouts.

Aim capture also records the host's per-peer correction positions, allowing
host/observer proximity to be checked against actual body-update targets. When
either trace is active, event-application stalls include native event type counts;
these identify a slow batch and are not per-event timings.
