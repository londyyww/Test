# Environmental Systems — Architecture

## Principles

* **One authority.** `Facility` runs on the server. Clients only send
  `InteractionRequest`s and mirror `NodeReplicationState`s.
* **Event-driven.** Nothing is simulated per frame. Work happens when an
  interaction, damage, or a scheduled timer changes something. Idle ticks only
  check the timer heap's top and any active cooperative channels.
* **Data, not code.** A facility is an INI file of typed nodes wired with
  `fed_by`. No map names or node ids appear in C++.
* **Consequences are first-class.** Power changes cascade into objects, objects
  emit sounds, and sounds reach robots through AIS.

## Model

```
 sources ──fed_by──► distribution ──fed_by──► consumers / interactive objects
 Generator           Junction                 Door, SecurityGate, Light, EmergencyLight,
 BackupGenerator     Breaker (rating, trip,   Machinery, Terminal, Alarm, SecuritySensor,
 Battery              fuse)                   CommArray, ExtractionPoint
```

* Power flows one way along `fed_by` edges (validated acyclic). A node may
  have several feeders (redundant buses).
* Every node belongs to a named **network**. Edges across networks are
  rejected, so independent grids (main base vs. battery perimeter) stay
  independent. Each network is solved separately, and only when marked dirty.

### Power solve (per dirty network)

1. **Energise:** breadth-first from sources that produce output (running,
   healthy or damaged generators; switched-on batteries with charge) through
   conducting distribution (junction intact; breaker closed, not tripped, fuse
   present, intact). Consumers are leaves.
2. **Islands:** union-find over live edges. Supply is pooled within an island.
   Empty batteries that are wired to a live island join it so they can charge.
3. **Allocate:** generator supply first, then batteries cover the deficit.
   Consumers are served highest `priority` first (first-fit), and the rest are
   shed. Surplus recharges batteries up to their charge rate.
4. **Overload:** if requested demand exceeds `(supply) × overload_tolerance`,
   every running generator in that island gets an `OverloadTrip` timer. That
   gives a warning window before it trips. Breakers whose downstream *served*
   load exceeds their rating get a `BreakerTrip` timer. Both timers are
   cancelled if the load drops in time.
5. **Batteries:** a constant rate until the next re-solve; an empty/full timer
   is scheduled analytically (no per-tick integration).
6. **Commit + behaviour:** consumers whose power changed run their behaviour
   hook (below), which may dirty the network again. The solve loop is capped at
   `maxSolveIterations` per tick.
7. **Backup logic:** a second energise pass with main generators only. A
   `BackupGenerator` with `auto_start` schedules a start after
   `auto_start_delay_ticks` when its bus loses primary power. It only does this
   once it has seen primary power (a base that was already dead does not
   self-start). It stops again when primary power returns, if
   `auto_stop_on_restore`.

### Timers

A min-heap of `(tick, seq)` with per-node, per-kind generation counters for
O(1) cancellation. Stale entries are skipped and compacted when the heap
exceeds `8 × nodes + 64`. Timer kinds: generator started, fuel empty, overload
trip, backup auto-start, battery empty/full, breaker trip, door motion done,
alarm pulse/end, emergency battery end, noise pulse.

## Object behaviour (reacts to real power)

| Object | Powered | Unpowered |
|---|---|---|
| Door / SecurityGate | Motor open/close (`motor_ticks`); locked doors need `key_item` or a terminal; forcing is refused (live maglock) | Motor dead → manual **Crank** channel (`manual_work`, `manual_participants`). `FailSafe` locks release; `FailSecure` stay locked → **Force** with `force_item` (loud, triggers linked `alarm`) |
| Light | Lit if switched on | Dark |
| EmergencyLight | Standby | On (dim) for `emergency_battery_ticks` after a *loss* of mains; never on a base that was always dark |
| Machinery | Runs if switched on: continuous sound (masking) + periodic pulses; may block navigation | Silent. Switch intent is remembered, so restoring power restarts forgotten machines |
| Terminal | Usable; `Hack` channel if `hack_ticks`; `Command` remote actions on its `controls` | Refuses everything |
| Alarm | Pulses `alarm_interval_ticks`, auto-silences after `alarm_duration_ticks` | Latched but silent; resumes on restore. `trigger_on_power_restore` = security reboot |
| SecuritySensor | Armed: successful interactions / reported presence within radius trigger its alarm | Disarmed. Re-arms on restore if `rearm_on_power_restore` |
| CommArray | On → objective signal + periodic bursts | Off |
| ExtractionPoint | `CallExtraction` channel (co-op), objective signal + loud beacon | Channel pauses (progress kept) |
| Generators | Start (spin-up), run, fuel, refuel; damaged = reduced output + deterministic backfire chance; destroyed until repaired | — |
| Breaker | Open/close, trip, reset (closes; may trip again), fuse install | — |

Repairs are channels. The repair item is consumed only when the repair
completes, and a repair stalls at 100% until someone carrying the part is
present.

## Interactions & co-op

`Submit()` pipeline (in order): authority → mission started → known player →
connected → **duplicate** (per-player 64-entry sequence window; retransmits
and replays are idempotent) → **rate limit** (token bucket per player) → node
exists → **range** (`interact_range_cm × rangeTolerance`) → **permissions**
(bitmask) → **expectedVersion** (optimistic concurrency: two players acting
on the same stale state → the second gets `RejectedStaleVersion`) → action
preconditions (power, items, lock, state) → effect.

Multi-tick work uses **channels** (crank, repair, hack, call extraction).
Players join and leave. Progress per tick equals the number of participants
once the minimum is met. Progress is stored on the node, so it survives
disconnection, walking away, or power loss; it pauses instead. A player is in
at most one channel at a time.

Up to `maxPlayers` (4) may be connected. A reconnecting player keeps their
dedup history.

## Outputs

* `IFacilityObserver`: typed `FacilityEvent`s (power, generator, breaker,
  overload, door, nav block, lights, alarms, alert level, security armed,
  damage, repaired, objective signals). Capped per tick.
* `IAcousticSink`: discrete `EnvSoundEvent`s plus continuous sources. The
  `AisAcousticBridge` maps them to AIS categories (generator → `Generator`,
  alarm/beacon → `Alarm`, forced door/backfire → `MetalImpact`, breaker →
  `BreakObject`, door motor → `Door`, machinery/comms → `Machinery`). It
  submits them as server-authoritative AIS events with per-machine source ids
  and keeps AIS ambient masking in sync.
* Queries for AI/navigation: `IsPassable`, `BlocksNavigation`,
  `IlluminationAt` (robot vision / player stealth), `ActiveAlarms`,
  `Networks()`.

## Replication

Each node has a `version` and a dirty flag. `CollectReplication` returns only
changed nodes as 12-byte-ish records (state enum, 13 flag bits, progress
byte, version). Clients apply records with newer versions only; servers ignore
mirrored state.

## Persistence

| Layer | What | Where |
|---|---|---|
| Mission state | Everything (switches, doors, alarms, channels, timers) | In memory; `Capture/Restore` snapshot for checkpoints/host restore |
| World state | Durable condition only: health/destroyed, fuse installed, fuel, lock broken — for nodes with `persistence = World`, or `WorldOnExtraction` if the squad extracted | `ExportWorldState(extracted)` → text record; `ImportWorldState` only at mission start |
| Player inventory | Items | Owned by the profile/inventory system; the facility only calls `HasItem`/`ConsumeItem` |

Switch operations are never exported, so a new mission always starts from the
designer's definition plus the durable world damage and repairs.

## Determinism

Fixed ticks; no clocks or `rand()`. Damaged-start rolls hash
`(facility seed, node, attempt)`. Sorted iteration everywhere. Timers are
ordered by `(tick, seq)`. `StateHash()` supports replay comparison.
