# World Simulation Scheduler

The scheduler decides **when** each entity gets CPU time. Most state changes do
not need scheduled updates at all: they are exact-time timers or event-driven
transitions. Periodic updates exist to re-sample bound infrastructure and to keep
mission-level intentions fresh around players.

## Tiers

| Tier | Default interval | Who |
|---|---|---|
| **High** | 3 ticks (10 Hz) | areas within 30 m of a player, entities observed or interacted with in the last 10 s, threatened entities (alarm, lockdown, responding patrols, active hazards) in nearby areas |
| **Medium** | 30 ticks (1 Hz) | areas within 90 m, `objective = true` areas, threatened entities anywhere |
| **Low** | 600 ticks (20 s) | everything else: distant facilities, inactive storage, background activity |

All values are configurable in `[tier.High|Medium|Low]` (`interval_ticks`,
`radius_cm`). Validation enforces `High ≤ Medium ≤ Low` intervals and `High ≤ Medium` radii.

### Relevance pass (no global scans)

Every `relevance_interval_ticks` (default 15), **area** tiers are recomputed from
at most 4 players: O(areas × players). Only entities in areas whose tier changed
are re-tiered, using the per-area membership lists. Observation and interaction
boosts live in a separate bounded list and expire in the same pass. A patrol
moves between area lists as it advances.

Promotion schedules an **immediate** catch-up update, so players approaching a
facility see its current state on the next tick. Demotion simply stretches the
next update.

## Queues

Two `BoundedScheduler` min-heaps ordered by `(due tick, insertion sequence)`, so
ties resolve identically on every platform:

* **timers**: FuelLow, FuelEmpty, BackupStart, ResponseEnd, HazardCycle,
  OverrideExpire, delayed rules and schedules. These are correctness events
  evaluated at their own due tick, so deferral never changes their outcome.
* **updates**: one live periodic update per entity.

Cancellation uses per-entity generation counters. Stale entries are skipped when
popped, or compacted when the heap is full. Capacity is fixed
(`queue_capacity`). Validation requires `≥ 2·entities + schedules + rules`, and
runtime overflow is counted in `SimStats::queueOverflows`.

### Per-step budgets

| Budget | Default | On exhaustion |
|---|---|---|
| `max_timers_per_step` | 512 | remaining timers wait one tick (`deferredTimers`) |
| `max_updates_per_step` | 256 | remaining updates wait one tick, in due order (`deferredUpdates`); nobody starves |
| `max_transitions_per_step` | 256 | remaining derivations continue next tick (`deferredTransitions`) |
| `max_pending_instructions` | 4 per patrol | oldest held order dropped (`InstructionDropped`) |
| `event_log_capacity` | 1024 | ring buffer, oldest overwritten |

### First update stagger

At `BeginRaid`, entity *i*'s first update is `1 + Mix64(seed ⊕ i) mod interval`.
Thousands of entities therefore never land on the same tick, and the spread is
reproducible.

## Determinism

Simulation time is an integer tick counter advanced only by `Step()`. Nothing
reads wall-clock time, iteration orders are fixed (vectors, sorted bind index,
heap with sequence tiebreak), and all randomness is seeded. Patrol positions and
fuel levels are closed-form functions of time, so the answer at tick *T* is the
same whether an entity was updated every 3 ticks or every 600. The tests check
this directly with `Sim_PatrolTransitionsAreTierIndependent` and
`Sim_DeterministicReplay`.

**Observable difference by tier:** directives for a Low-tier patrol are emitted
when its coarse update runs, so up to 20 s after the leg boundary. That is the
point: nobody is there to see it. Promotion produces an exact catch-up.

## Measured cost (portable core only)

See `VERIFICATION_REPORT.md` for the benchmark table (100 / 500 / 1,000 / 5,000
entities). These are CPU timings of the portable C++ core in a Linux sandbox.
They are **not** Unreal frame-time or FPS measurements.
