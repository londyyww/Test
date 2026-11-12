# Cross-System Integration

## Layers

```
               ┌───────────────────────── integration/WorldIntelligence ─────────────────────────┐
               │                                                                                  │
  Weather ─────┤ WeatherChanged → AIS WeatherState(rain,wind)   WindGust/Thunder → AIS events      │
  (llworld)    │ electrical_exposure → llenv ServerApplyDamage(exposed, already-damaged nodes)    │
               │                                                                                  │
  Hazards ─────┤ IHazardWorld = FacilityPorts: energized/running ← llenv, DamageNode/TriggerAlarm │
  (llworld)    │ → llenv, hazard sounds → AIS events                                              │
               │                                                                                  │
  Squads ──────┤ ISquadWorld = FacilityPorts: relay powered? ← llenv CommArray                    │
  (llworld)    │ AIS traces → ContactReports (confidence × weather)   intents → AIS priority      │
               │ SquadSignalPort filters AIS robot-to-robot sharing by squad comms                │
               │                                                                                  │
  Power grid ──┤ FacilityEvents → WorldEvents (AlarmRaised, PowerLost/Restored, EmergencyLighting)│
  (llenv, #2)  │ sounds → existing AisAcousticBridge → AIS                                        │
               │                                                                                  │
  AIS (#1) ────┤ perception, robot brains, decoys (unchanged)                                     │
               └──────────────────────────────────────────────────────────────────────────────────┘
```

The three cores (`src/`) do not include AIS or llenv headers. They are
tested alone (`wi_unit_tests`) and only meet the existing systems through
ports in `integration/`.

## Shared event and state interface

* `WorldEventBus` (`include/llworld/WorldEvents.h`): typed `WorldEvent`
  (id, tick, type, source system, position, magnitude, radius, subject,
  aux). It is bounded per tick (`max_events_per_tick`); invalid
  (non-finite) events are dropped and counted. Events published in tick
  *T* are read by systems in tick *T+1* (`Previous()`), so outcomes never
  depend on step order within a tick.
* `IEnvironmentQuery`: `VisibilityAt` (visual and thermal factors,
  illumination, fog, smoke), `Acoustics`, `SurfaceAt`, `HazardDangerAt`,
  `IsRouteBlocked`. `WorldIntelligence` implements it by composing
  weather, hazards and power-grid lighting. Helpers:
  `VisualDetectionRangeCm`, `ThermalDetectionRangeCm`, `RouteRisk`.

## Tick order (server, fixed 30 Hz)

1. `bus.BeginTick`
2. Weather step, then forward to AIS weather, gust/thunder AIS events, and
   infrastructure exposure.
3. Power grid step (its events go to the bus via `FacilityPorts`, sounds
   to AIS via the PR #2 bridge).
4. Hazards step (reads last tick's events), then player exposure sampling.
5. AIS step (perception, brains, filtered robot sharing).
6. Every coordination interval: AIS traces → squad reports.
7. Squad step (comms, sharing, assignment), then AIS objective priorities.

## Scenarios (all executable: `make demo`, and tested in `tests/test_integration.cpp`)

| Scenario | Chain | Test |
|---|---|---|
| **A** Storm | `RequestWeather(Storm)` → AIS rain/wind floors rise → distant reports ×0.42 confidence → exposed damaged `GEN_A` loses health → destroyed (`PowerLost`) → cable arc suppressed, `EmergencyLighting` → backup restores power → `ALARM_PLANT` security reboot (`AlarmRaised`) → AIS robots hear the siren → squad assigns Investigate/Search/Observe near the alarm. The storm also shrinks radio range so the distant WARDEN loses comms. | `Integration_ScenarioA_…` |
| **B** Industrial building | Player stands at the live `CABLE_ARC` and takes a dose → pulls `ALARM_PLANT` (power-grid interaction) → AIS events → robots' traces become squad reports → squad splits: one investigates/searches, the WRAITH observes, at least one keeps patrolling. | `Integration_ScenarioB_…` |
| **C** Fire | `FIRE_STORE` activates → smoke (visual ×0.41, thermal ×0.51) → east route blocked, `RouteRisk(east)=∞` → players take the west yard → robots get uncertain fire-noise traces → `COMMS_RELAY` destroyed → far WARDEN disconnected, keeps its stale order, never sees the new gunfire contact, then switches to LocalAutonomous. | `Integration_ScenarioC_…` |

Additional cross-system tests: storm masking vs puddle footsteps, visual
vs thermal separation, power shutdown suppressing and restoring the cable
hazard, a truss collapse cutting the cable (killing the arc and blocking
the route), many simultaneous events plus a flood of 5000 footsteps and
invalid inputs, and bit-identical full-stack replay with two players.

## Changes to existing systems

None. PR #1 and PR #2 code is used as-is. Their test suites (52 and 37
tests) were re-run on this branch and pass.
