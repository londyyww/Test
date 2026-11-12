# Mission World Simulation

`llsim::WorldSimulation` keeps a lightweight, authoritative model of what Dead
Frequency is doing during a raid, including the parts nobody is looking at.

## Scope boundaries

| The world simulation… | …and leaves this to |
|---|---|
| tracks entity state, intentions, schedules and rules | — |
| mirrors bound power-grid nodes (generators, networks, alarms, comms masts) | `llenv::Facility` (power distribution, fuel of bound generators) |
| models fuel analytically for **abstract**, unloaded facilities | — |
| consumes hazard activation and weather severity as signals | `llworld::HazardSystem`, `llworld::WeatherSystem` |
| emits mission-level `PatrolDirective`s (move to / hold / respond / resume) | `llworld::SquadCoordinator`, AIS and Unreal AI (navigation, perception, tactics) |
| reacts to disturbances (loot searches, gunfire, explosions) | AIS (actual propagation and hearing) |
| exposes `BeginRaid / CompleteMission / ResetWorld / ApplyMissionOverride` | Mission Director (objectives, mission state machine) |

There is no second mission state machine and no tactical AI here.

## Entities

Kinds: `PatrolGroup`, `Facility`, `Generator`, `PowerNetwork`, `SecuritySystem`,
`Machinery`, `Storage`, `CommsStation`, `HazardZone`.

Each entity exposes, through `EntityView`: id, kind, **effective state**, **base
state**, **tier** (simulation priority), **last update tick**, current area,
dependencies, pending timers (active events), fuel, comms link and pending
instructions. **Transition rules** come from built-in kind semantics plus
data-driven `[rule.*]`.

States: `Nominal, Active, Standby, Degraded, Offline, Alarm, Lockdown, Interrupted, Responding`.
Each kind accepts a subset; validation rejects the others.

### Base vs effective state

The *base* state is what the entity itself is: latched by signals, rules, timers
or bound infrastructure. The *effective* state also accounts for dependencies:

| Kind | Effective state |
|---|---|
| PowerNetwork (abstract) | primary generator up → Nominal; only backup → Degraded; none → Offline; upstream network offline → Offline |
| SecuritySystem / CommsStation / Machinery | Offline when a network dependency is Offline, else base |
| Storage | Offline without power; Lockdown when a Facility dependency is in Lockdown |
| Facility | Lockdown (sticky) > Alarm (security alarm or active hazard) > Offline (all networks down) > Degraded (any network degraded/down) > base |
| PatrolGroup | Offline / Responding (own) > Interrupted (comms dependency not Nominal) > Active |
| Generator / HazardZone | base |

Changes propagate **event-driven** through the reverse dependency graph, with no
global scans. The graph is validated acyclic, and derivation runs in topological
order at raid start.

### Bound vs abstract entities

`bind = node:<facility node>` or `bind = hazard:<hazard id>` makes an entity a
mirror of an existing system. The host pushes changes (`NotifyBoundChanged`,
signals) and the entity re-samples on its scheduled updates. Rules may not
`set/start/stop` a bound entity; validation rejects it.

Unbound (abstract) entities are simulated here. This is how a facility 2.8 km
away keeps running without loading its power grid.

## Autonomous activity

| Activity | Mechanism |
|---|---|
| Robot patrol scheduling, movement between security areas | route of areas, `travel_ticks` + `dwell_ticks` per leg; position is an analytic function of time, so coarse and fine updates agree |
| Generator fuel consumption | `fuel(t) = fuel₀ − (t − t₀)·burn`, with exact-time `FuelLow` / `FuelEmpty` timers; no per-tick integration |
| Backup generator activation | primary leaves Active/Degraded → `BackupStart` after `start_delay_ticks`; primary returns → backup back to Standby |
| Facility power degradation | network/facility derivation (Degraded on backup, Offline when dark) |
| Security system activation | `AlarmRaised` signal (from the power grid alarm node or a schedule) |
| Communication interruptions | `CommsLost` / network loss → patrol `Interrupted`; central instructions are queued (bounded, oldest dropped) and delivered on reconnect |
| Environmental hazard changes | bound hazard signals, or an abstract `active_ticks` / `dormant_ticks` cycle |
| Scheduled patrol changes | `[schedule.*]` with `retask:GROUP=AREA+AREA` (central: needs comms) |
| Facility lockdown | rule actions, e.g. `on = state:SEC=Alarm` → `set:FAC=Lockdown, alert:FAC` |
| Alarm / noise attracts a patrol | `alert:` dispatches the nearest group; a group within `respond_radius_cm` responds on its own, otherwise central command sends the nearest group (needs comms). `ReportDisturbance` responds only within earshot (radius scales with loudness) |

### Rules and schedules

```ini
[rule.RLY_LOCKDOWN]
on = state:RLY_SECURITY=Alarm          ; or signal:KIND / signal:KIND@ENTITY
require = state:RLY_PATROL!=Offline    ; optional conditions
do = set:RLY_FACILITY=Lockdown, alert:RLY_FACILITY
delay_ticks = 0
once = true

[schedule.RLY_SHIFT_CHANGE]
at_ticks = 3600
repeat_ticks = 0
do = retask:RLY_PATROL=RLY_CP_WEST+RLY_SITE
```

Actions: `set:E=S`, `start:GEN`, `stop:GEN`, `alert:E`, `retask:G=A+B`, `signal:KIND[@E]`.
Rule chains that could re-trigger forever are rejected unless one of them is `once`.

## Raid lifecycle and persistence

| Phase | API | Notes |
|---|---|---|
| Initial world state | constructor / `ResetWorld()` | authored states, no events |
| Active raid simulation | `BeginRaid(seed, persisted)` + `Step()` | staggered first updates (seeded), timers scheduled |
| Temporary mission changes | `ApplyMissionOverride(e, state, ticks)` | override layer; reverts on expiry or raid end; never persisted |
| Mission completion | `CompleteMission()` | freezes; state stays queryable |
| World reset | `ResetWorld()` | back to authored initial state |
| Persistent facility state | `ExportPersistentState()` / `BeginRaid(seed, records)` | only `persist = true` entities: base state + fuel |

**What resets after extraction (V1):** everything except records the caller
chooses to keep. That covers patrol positions and routes, alarms, lockdowns,
overrides, comms interruptions, hazard cycles, pending instructions and all
container search state (loot is per raid).

**What future systems may preserve:** the `PersistentEntityRecord`s of entities
marked `persist = true` (generator fuel and state in the examples). This module
only exports and imports them. Storage, profile ownership and cross-raid policy
belong to the persistence service; no global world persistence was added.
Power-grid durable state keeps using the existing `llenv::WorldStateRecord`.
