# Stormworks Network Fixes

[![Join Discord](https://img.shields.io/badge/Join-Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/bX4RzunsxC)

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
