# Environmental Systems — Performance

Target: GTX 1660 Super 6GB / i5-10400F / 16 GB. The system is **server CPU only**:
no GPU work, no per-frame simulation.

## Cost model

| Work | When | Cost |
|---|---|---|
| Idle step | every tick | timer-heap top check + active channels (≤ 4 players) |
| Power solve | only when a network is dirty (interaction, damage, timer) | O(nodes + edges) energise ×2, union-find, sort of island consumers, plus breaker downstream walks O(breakers × subtree) |
| Timers | scheduled events only | O(log n) heap ops; stale entries skipped/compacted |
| Interaction | per request (rate limited ≈ 7.5/s/player) | validation O(1) + action + possible solve |
| Illumination query | on demand | O(lights) |
| Replication | per step | O(nodes) dirty scan; only changed nodes emitted |

## Measured (standalone core, sandbox x86-64 CPU, GCC 13 -O2, single thread)

Synthetic facility: 1 generator, N breakers × 9 consumers (doors/lights/alarms).

| Nodes | Idle step | Breaker toggle + full re-solve + 4 steps |
|---|---|---|
| 51 | 0.013 µs | 2.5 µs |
| 201 | 0.013 µs | 8.9 µs |
| 601 | 0.013 µs | 27 µs |

The example Relay Outpost demo (2 networks, 26 nodes, ~118 s of play) did 12
power solves in total. The test `Performance_EventDrivenIdleTicksDoNotResolve`
checks that 3000 idle ticks on a 311-node facility trigger zero solves.

**Not measured on the target hardware or inside Unreal.** UE costs that will
dominate instead: actor replication of changed nodes, `OnRep` visuals, and
nav modifier updates — all event driven.

## Known hot spots / headroom

* `SolveNetwork` allocates scratch vectors sized to the facility per solve and
  walks each breaker's subtree separately. That is fine for hundreds of nodes;
  for thousands, cache per-breaker downstream lists and reuse scratch buffers.
* Splitting a large base into several networks keeps solves local.
* Event and sound output are capped per tick (`maxEventsPerTick`,
  `maxSoundsPerTick`), so a chain reaction cannot flood observers or AIS.
