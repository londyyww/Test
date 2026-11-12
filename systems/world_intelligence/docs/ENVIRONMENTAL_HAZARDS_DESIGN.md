# Environmental Hazards — Design

## Goal

The facility is dangerous without robots. Hazards come from infrastructure
(live cables, gas lines, machinery), the weather, and player or robot
actions, and they change routes, visibility, sensors and robot behaviour.
These are gameplay approximations: no chemistry and no physics, just
data-driven rules with bounded propagation.

## Model

Each hazard is a designer-placed **site** (`[hazard.<ID>]`).

| Field group | Keys |
|---|---|
| Identity | `type`, `position`, `radius_cm`, `max_radius_cm`, `severity` |
| Activation | `starts_active`, `duration_ticks` (fire fuel / leak length), `player_triggerable` |
| Propagation | `flammable`, `spread_radius_cm`, `spread_interval_ticks`, `spread_chance`, `growth_per_second` |
| Dependencies | `power_node` (power-grid id), `exposed` (weather applies) |
| Interaction rules | `ignition_threshold`, `ignites`, `smoke`, `alarm`, `extinguish_item`, `route_id`, `damages_nodes` |
| Effects | `damage_per_second`, `burst_damage`, `visibility_reduction`, `thermal_clutter`, `movement_penalty`, noise keys |
| Structural | `stress_warn`, `stress_collapse`, `stress_per_loud_event`, `stress_per_fire_second`, `stress_per_wind_second` |
| Persistence | `persistence = Mission \| Facility` |

States: `Dormant` (inactive/stable), `Active`, `Suppressed` (dependency
missing, e.g. no power), `Spent` (closed, extinguished, burned out,
collapsed).

## Hazard types and rules

| Type | Behaviour |
|---|---|
| **Electrical** (damaged cable) | Live only while its `power_node` is energized. Cutting power suppresses it; restoring power re-arms it. Wet ground extends its radius (`wet_electrical_radius_mul × wetness`). Periodic arc noise. It is an ignition source. |
| **GasLeak** | Concentration grows per second; wind lowers its cap outdoors. Above `ignition_threshold` it ignites if any *nearby* ignition source is active (electrical arc, fire, running machinery) or a Gunfire/Explosion event lands inside it. Ignition is a one-off flash (burst damage, loud event, node damage) and may light one designated fire site (`ignites`). Players can close the valve, or open it if `player_triggerable`. |
| **Fire** | Burns `duration_ticks` of fuel; exposed fires burn faster in rain (`fire_rain_suppression`). It may spread only to `flammable` sites within `spread_radius_cm`, rolled deterministically every `spread_interval_ticks` and scaled by the weather's `fire_spread_mul`. It drives a linked smoke hazard, damages listed power-grid nodes, blocks its route, clutters thermal sensors, and can be extinguished with an item. Burned-out sites never re-ignite. |
| **Smoke** | Density follows its fire; dissipates after the fire ends (faster in wind); radius grows with wind. Main source of visibility loss. |
| **Structural** | Always monitored. Stress comes from nearby active fires, loud world events (explosion 1.0, gas ignition 1.0, collapse 1.0, gunfire 0.15, thunder 0.05) and storm wind if exposed. At `stress_warn` it creaks: becomes Active, emits an event and noise, and is dangerous to stand under. At `stress_collapse` it collapses: loud event, burst damage, damage to listed nodes (e.g. cutting a cable), route permanently blocked. |
| **Chemical** | Radius grows toward `max_radius_cm`; wet weather spreads it faster. Damage over time and a movement penalty. |
| **Machinery** | Active while its power-grid machinery node is running. |

There are no unbounded chain reactions: spread and ignition only follow
neighbour lists precomputed from designer-placed sites, a gas leak ignites
once, and a burned-out fire never re-lights.

## Interfaces

* `IHazardWorld` (port): resolve node ids, energized/running state, damage
  nodes, trigger alarms, emit sounds, check and consume items. Implemented
  over llenv + AIS in `integration/`; tests use a fake.
* `WorldEventBus`: hazards consume the previous tick's events (power,
  alarms, gunfire, explosions, collapses) and publish activation, spread,
  ignition, collapse, route and exposure events.
* `ExposureAt(pos)` and `SamplePlayers()`: damage per second, visibility,
  thermal clutter and movement penalty. Player doses are produced at
  `exposure_interval_ticks`. AIS does not apply damage; the host damage
  system does (UNVERIFIED adapter).

## Multiplayer

* Server-authoritative: only `HazardSystem` mutates state; clients receive
  `HazardReplicationState` deltas (state, intensity byte, radius, flags,
  version).
* Duplicate activation: `Activate(hazard, causeId)` keeps a bounded ring of
  causes, and an active hazard cannot be activated twice.
* Player requests: per-player sequence dedup (32-entry window), token-bucket
  rate limit, range check, item check; at most 4 players are tracked.
  Simultaneous requests are resolved by arrival order. The second valve
  opening is rejected as `RejectedInvalidState`.
* Bounded memory: fixed hazard set, neighbour lists built once, a cause ring,
  bursts cleared every exposure sample, and a capped event bus.

## Persistence

* `Mission` hazards reset every mission and are never written to profiles.
* `Facility` hazards export durable damage (`collapsed`, `burnedOut`,
  `stress`) through `ExportFacilityDamage`. `ImportFacilityDamage` is only
  allowed before the first `Step`. A collapsed truss keeps its route blocked
  next mission.

## Known limitations

* Positions are points with radii. There are no volumes, occlusion or
  indoor/outdoor zones beyond the `exposed` flag.
* Smoke does not drift with wind direction; it only spreads radially.
* Fire damage to nodes is linear per second; no flammability of the power
  grid itself.
* Electrical/machinery dependencies refresh on facility events. A junction
  that changes energized state without any consumer event (no consumers
  downstream) is picked up on the next facility event or weather change.
* Damage is reported, not applied: player health lives in an unverified
  damage system.
