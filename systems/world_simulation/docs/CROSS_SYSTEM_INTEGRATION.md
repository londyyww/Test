# Cross-System Integration

`integration/MissionWorldHost` connects the two new cores to the systems
delivered in PR #1–#3. It owns none of them and copies none of their logic; every
connection is an explicit port.

```
            ┌──────────────── llworld::WorldIntelligence (PR #3) ────────────────┐
            │ WeatherSystem   HazardSystem   SquadCoordinator   AIS (PR #1)       │
            │                  llenv::Facility power grid (PR #2)                 │
            └───────┬───────────────────┬──────────────────────┬────────────────┘
      WorldEventBus │      node state   │ SampleNode / poll    │ SubmitEvent (AIS)
     (Current tick) │      (watched)    │                      │
            ┌───────▼───────────────────▼──────┐        ┌──────┴──────────────┐
            │        MissionWorldHost          │◄───────┤ ILootSoundSink      │
            │  ILootAccessPort                 │        └─────────────────────┘
            │  IWorldInfrastructurePort        │
            └───────┬───────────────────┬──────┘
                    │ signals,          │ IsPowered / HasItem / HasClearance / IsSealed
                    │ disturbances      │ NotifyInfrastructureChanged
            ┌───────▼────────┐   ┌──────▼─────────────┐
            │ llsim::World-  │   │ llloot::Container- │──► ILootGrantPort (authoritative inventory)
            │ Simulation     │◄──┤ Service            │──► IItemCatalog  (authoritative catalog)
            └────────────────┘   └────────────────────┘
                 PatrolDirective ──► Unreal AI / squad layer (consumer decides)
```

## Tick order (server)

1. Translate world events the game published **between ticks** (gunfire,
   explosions, hazard activations) from the bus, before the tick rotates them away.
2. `WorldIntelligence::Step()`: weather → power grid → hazards → AIS → squads (existing order).
3. Translate this tick's world events into world-simulation signals.
4. Poll the **watched** power-grid nodes, meaning only those referenced by
   content. Changes fan out to `WorldSimulation::NotifyBoundChanged` and
   `ContainerService::NotifyInfrastructureChanged`.
5. `WorldSimulation::Step()`.
6. `ContainerService::Step(now)`.

## Mappings

| Source | Event / query | Destination |
|---|---|---|
| Power grid alarm node | `AlarmRaised` (world bus) | `SignalKind::AlarmRaised` on the bound SecuritySystem |
| Power grid generator | `PowerLost` / `PowerRestored` | bound Generator state |
| Any power-grid node referenced by content | node `powered` / generator `Running` / `alarmActive` (polled) | bound entities re-sample; power-gated containers emit `AccessChanged` |
| Hazard system | `HazardActivated` / `HazardDeactivated` | bound HazardZone (`hazard:<id>`) |
| Weather | `WeatherChanged` (target Storm / other) | global `WeatherSevere` / `WeatherCleared` signals for rules |
| Game gunfire / explosions / gas ignition | world bus | `ReportDisturbance` (140 / 150 dB) |
| Container search / force / debris | `ILootSoundSink` | AIS `SubmitEvent` (BreakObject or MetalImpact, ServerAuthoritative) **and** `ReportDisturbance` |
| Container `power:` requirement | `IsPowered` | facility node `powered` |
| Container `tool:` / `clearance:` | `HasItem` | existing `llenv::IPlayerInventory` (keycards are items) |
| Container `seal:` | `IsSealed` | world-simulation entity effective state == Lockdown |
| Player positions | `ConnectPlayer` / `MovePlayer` | facility, containers, world-simulation relevance, WI hazard sampling |

## Verified behaviours (integration tests)

| Test | What it proves |
|---|---|
| `Integration_PowerRestorationUnlocksContainer` | Starting the generator through the existing facility interaction powers the rack lock; the container receives `AccessChanged`; the world simulation mirrors the generator, network and comms; contents match the pure generator |
| `Integration_SearchEmitsAcousticEventAndAttractsPatrol` | The search becomes an accepted AIS event; robots perceive **that** event ID; the nearby mission patrol responds locally |
| `Integration_FacilityAlarmLocksDownAndSealsStorage` | A power-grid alarm drives security → depot lockdown → storage lockdown → container sealed |
| `Integration_GeneratorFailureChangesFacilityActivity` | Fuel runs out in the power grid; emergency lighting activates; world entities go Offline; the blackout rule reassigns the patrol |
| `Integration_HazardAndWeatherEventsInfluenceWorldState` | A hazard-system fire raises a facility alarm; a weather-system storm interrupts mission comms through a data rule, which clears when the weather does |
| `Integration_DistantFacilityAdvancesWithoutFullRateUpdates` | The relay station completes its fuel and backup cycle at Low tier (≤ 14 updates) while the depot runs at High tier |
| `Integration_BindingsResolveAgainstTheFacility` | Content referencing missing nodes is reported |

## Narrow adapters created (and why)

| Adapter | Reason |
|---|---|
| `IItemCatalog` | No item catalog API exists in the repository. The loot system needs category, stack limit and footprint only |
| `ILootGrantPort` | No inventory transaction API exists. Must be idempotent per `grantId` |
| `ILootAccessPort` | Single place for power, tools, clearances and lockdowns, implemented by the host from existing systems |
| `IWorldInfrastructurePort` | Read-only node and hazard sampling for bound entities |
| `PatrolDirective` / `IWorldSimObserver` | `SquadCoordinator` exposes no route-assignment API, so directives are published for Unreal AI or the squad layer to consume. Changing squad routes would require a new squad API (not invented here) |

## Systems referenced but not available in this repository

Mission Director, Tactical AI, LLSignalNetwork, Robot Damage System, and the
player inventory and persistence services are **not** in this repository. Nothing
here calls them. The lifecycle API, `PatrolDirective`, `ILootGrantPort`,
`IItemCatalog` and `PersistentEntityRecord` are the documented seams where they
attach (see `UNREAL_INTEGRATION_CONTRACT.md`).
