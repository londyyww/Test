# Unreal Engine 5.8.4 Integration Contract — Environmental & World Intelligence

**Status: prepared, not verified.** Nothing here was compiled against Unreal
Engine, run in PIE, or exercised over a real network. UE class and function
names are proposals that must be checked against 5.8.4. Mission Director,
Tactical AI, Robot Damage System and LLSignalNetwork do not exist in this
repository; their adapters are marked **UNVERIFIED**.

## Modules

```
Source/LastLightWorld/
  Private/Core/          ← include/llworld + src (no UE types, builds with -fno-exceptions -fno-rtti)
  Private/Integration/   ← integration/WorldIntelligence (depends on the AIS and Environment modules from PR #1/#2)
  Public/LLWorldIntelligenceSubsystem.h
  Public/LLHazardActor.h, LLWeatherSettings.h, LLSquadComponent.h
```

## ULLWorldIntelligenceSubsystem (server only)

```cpp
// ILLUSTRATIVE — NOT COMPILED
UCLASS()
class ULLWorldIntelligenceSubsystem : public UTickableWorldSubsystem {
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;  // false on NM_Client
    virtual void Tick(float DeltaTime) override;  // fixed 30 Hz WorldIntelligence::Step(), capped catch-up
private:
    TUniquePtr<llworld::WorldIntelligence> World;  // owns AIS world + facility for the level
};
```

* **Construction:** build `WorldIntelligenceConfig` from data assets or
  the shipped INI format, the `llenv::FacilityDef` from infrastructure
  actors (PR #2 contract), and an `IAcousticEnvironment`. In the core this
  is `ais::BoxAcousticEnvironment`. In UE, an environment that exposes
  `SetWeather` and an ambient registry is required (see the AIS contract);
  `WorldIntelligence` currently takes a `BoxAcousticEnvironment&`. That
  needs a small refactor to an interface when the UE environment exists.
* **Players:** each tick, `SetPlayers` with pawn positions (≤ 4).
  Footsteps go through `PlayerFootstep`, generated server-side from
  movement, so puddles and weather apply. Weapons call `ReportGunfire` /
  `ReportExplosion` (LLProjectileBallistics, UNVERIFIED).
* **Robots:** `AddRobot(id, aisProfile, pos)` on spawn. Each tick,
  `SetRobotBody(id, pos, alive, CommsHealth)`, where `CommsHealth` comes
  from the Robot Damage System (UNVERIFIED: map the comms component's
  damage state to Intact/Damaged/Destroyed). Read `OrderFor(id)` and hand
  `{intent, moveTarget, radius}` to the robot's behaviour tree or
  Tactical AI (UNVERIFIED). For example, MoveTo with acceptance radius;
  Observe means MoveTo then focus on the contact; Defend means hold cover
  near the target. Project targets to the navmesh.
* **Hazards:** `ALLHazardActor` per `[hazard.ID]` for visuals (fire/smoke
  Niagara, sparks), driven by replicated `HazardReplicationState`. Damage
  from `PlayerExposure` events (magnitude = dose) is applied by the damage
  system (UNVERIFIED). Route blocking maps to nav modifiers or blocked
  smart links per `route_id`.
* **Weather:** visuals and audio (rain, fog volume, wind) on clients from a
  replicated `WeatherSnapshot` subset (from, to, blend, wetness), updated
  only when `version` changes.

## Replication

| Data | How |
|---|---|
| Weather | One replicated struct on a level-wide actor; changes ~1 Hz at most during transitions |
| Hazards | `CollectReplication` deltas → per-hazard actor replicated struct (dormancy friendly) |
| Squad intents | Server only. Clients see robot behaviour, never orders or contact boards |
| Power grid / AIS | As in PR #1 / #2 contracts |

There is no client path into any of these systems. Player hazard actions
(extinguish, valve) arrive as a Server RPC and are resolved through
`HazardSystem::Interact`, with the player id taken from the connection.

## Mission Director (UNVERIFIED)

* `RequestWeather(kind, ticks)` for scripted weather.
* `Hazards().Activate(id, causeId)` for scripted incidents. Use a stable
  `causeId` so retries de-duplicate.
* Subscribe to `WorldEventBus` events (e.g. `StructuralCollapse`,
  `RouteBlocked`, `AlarmRaised`) for objectives.
* Persistence: `Hazards().ExportFacilityDamage()` at mission end, stored
  per facility in the world save (not in player profiles).
  `ImportFacilityDamage` runs before the first tick.

## LLSignalNetwork (UNVERIFIED, not present)

Squad comms are computed internally from range, weather, damage and the
power-grid relay. If LLSignalNetwork exists, implement its link test
inside `SquadCoordinator::ComputeComms` through a new port, and route
`SquadSignalPort` delivery through it. Do not run two networks.

## Acceptance checklist (not done)

- [ ] Compiles in UE 5.8.4 (editor + dedicated server).
- [ ] Subsystem absent on clients; no server state reachable from client RPCs.
- [ ] Storm/fog visuals match `WeatherSnapshot`; footsteps splash when `puddles`.
- [ ] Hazard actors replicate; damage applied via the damage system.
- [ ] Robots follow `OrderFor` through the navmesh; observe/flank points projected.
- [ ] 1–4 player PIE + net emulation; deterministic replay on the same build.
- [ ] Server cost measured on an i5-10400F with the target robot and hazard counts.
