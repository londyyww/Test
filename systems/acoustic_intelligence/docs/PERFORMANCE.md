# AIS — Performance Considerations

## Where the cost is

AIS runs **on the server CPU only**. It uses no GPU and does no audio DSP, so
the GTX 1660 Super 6GB target is unaffected except via a listen-server host's
CPU budget. Per step:

| Stage | Cost | Bound |
|---|---|---|
| Event intake | O(dedup history + tracked sources) per submit, worst case linear scans of 512 / 1024 small structs | `dedup_history`, `max_tracked_sources`, `max_accepted_per_tick` |
| Drain | sort of ≤ `queue_capacity` (256) events | `max_drain_per_tick` (64) |
| Broadphase | grid rebuild O(R log R) + cell lookups per event | per-band audible radius; linear fallback when the radius covers more cells than robots exist |
| Perception | one propagation evaluation **= one occlusion trace** per (event, candidate robot) | `max_perception_checks_per_tick` (1024); excess events are deferred, then expire |
| Brains | O(inbox × memory) per update | `sensory_update_interval_ticks` (2–4) staggered by id; `inbox_capacity` 16; `memory_capacity` 8 |
| Reports | O(reports × robots) | rate limit per sender, `max_reports_in_flight` |

In Unreal, the occlusion trace dominates. Budget it as
`max_perception_checks_per_tick × ticks_per_second` traces/s (default worst
case ≈ 30k/s, typical far lower). Lower `ticks_per_second` (e.g. 15),
`max_perception_checks_per_tick`, or the robots' update rate if profiling
shows pressure.

## Measured (standalone core only)

Sandbox x86-64 CPU (model not reported), GCC 13 `-O2`, single thread, the
`BoxAcousticEnvironment` with 200 walls (linear wall scan per trace), 8
players emitting footsteps every tick plus gunfire 3×/s, 3000 ticks:

| Robots | Avg step | Perception checks |
|---|---|---|
| 16 | 18 µs | 28,855 |
| 64 | 80 µs | 138,164 |
| 256 | 246 µs | 385,538 |

The full test suite (52 tests, including multi-thousand-tick simulations)
runs in about 15 ms. **These numbers do not include Unreal trace costs and
were not measured on the target hardware.** The benchmark was a scratch
program and is not shipped; the 64-robot broadphase test
(`World_ManyRobotsManyEventsStayWithinBudget`) guards against regressions in
culling.

## Memory

Fixed after warm-up: bus queue 256 events, dedup ring 512, buckets 1024,
per robot 16 inbox + 8 traces + search points. Roughly < 200 KB for 64 robots.
Scratch vectors are reused per step; the steady state does not allocate,
apart from growth of the cosmetic cue list up to `listeners × cap`.

## Bandwidth

Only cosmetic cues are candidates for replication, filtered by listener
distance and capped per client per tick (`max_cosmetic_cues_per_client_per_tick`,
default 16). Weapon sounds already replicated by gameplay should be excluded
on the Unreal side.

## Tuning knobs (data, no recompile)

`[world] ticks_per_second, max_perception_checks_per_tick, grid_cell_cm`,
`[bus] queue_capacity, max_drain_per_tick, source_burst, source_refill_per_tick`,
`[robot.X] sensory_update_interval_ticks, inbox_capacity, memory_capacity`.
