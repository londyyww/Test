# AIS — Unreal Engine 5.8.4 Integration Contract

**Status: specification only.** Nothing in this document has been compiled
against Unreal Engine or run in the editor. Class names and engine calls
below are proposals based on general UE5 conventions and must be checked
against the 5.8.4 headers. Project-specific systems (LLTacticalDecision,
LLSignalNetwork, LLProjectileBallistics, Robot Damage System, existing robot
AI, audio event interfaces) **were not present in the repository** that AIS
was developed in; every binding to them is marked **UNVERIFIED** and must be
adapted to their real APIs instead of duplicating them.

## 1. Module layout

```
Source/LastLightAcoustics/
  LastLightAcoustics.Build.cs        Runtime module; deps: Core, CoreUObject, Engine, (AIModule if bridging)
  Private/Core/                      ← copy of include/ais + src verbatim (no UE types inside)
  Public/LLAcousticSubsystem.h       UWorld subsystem owning ais::AcousticWorld
  Public/LLAcousticEmitterComponent.h
  Public/LLAcousticSensorComponent.h
  Public/LLAcousticDecoyActor.h
  Public/LLAcousticSettings.h        UDeveloperSettings / data asset → ais::AisConfig
  Private/LLAcousticEnvironment.cpp  IAcousticEnvironment over UE traces/volumes
  Private/LLSignalNetworkAcousticPort.cpp   ISignalNetworkPort over LLSignalNetwork (UNVERIFIED)
```

The core compiles with `-fno-exceptions -fno-rtti` (`make ue-compat`), uses
only the standard library, and has no global state, so it can live in a
UE module's Private folder. Keep it free of UE types so the standalone tests
stay authoritative.

## 2. Unit and type mapping

| AIS | Unreal |
|---|---|
| `ais::Vec3` (cm, +Z up, double) | `FVector` (UE5 LWC doubles, cm) — copy X/Y/Z |
| `ais::Tick` | Server-side fixed counter advanced by the subsystem (see §3). **Do not** use `FPlatformTime` or `GetTimeSeconds()` as event time. |
| `ais::EntityId` (uint32) | A stable server-assigned id per actor (e.g. a map `TWeakObjectPtr<AActor>` ↔ id inside the subsystem). Not `GetUniqueID()` across sessions. |
| `SurfaceType` | Map from `EPhysicalSurface` via settings table |
| `MaterialId` | Map from `UPhysicalMaterial` (or a dedicated acoustic material asset) via settings table |
| `StimulusCategory` | `UENUM` mirror with static_assert on count |

## 3. ULLAcousticSubsystem (server only)

```cpp
// ILLUSTRATIVE — NOT COMPILED
UCLASS()
class ULLAcousticSubsystem : public UTickableWorldSubsystem {
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;  // false unless the world's net mode is not NM_Client
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;  // build AisConfig from settings, ValidateConfig(), log errors
    virtual void Tick(float DeltaTime) override;  // accumulate; call World->Step() at FixedTickRate (default 30 Hz), max N steps/frame
    virtual TStatId GetStatId() const override;

    // Server gameplay entry point. Returns ais::SubmitResult as a UENUM.
    ELLAcousticSubmitResult ReportSound(ELLStimulusCategory Category, const FVector& Location,
                                        AActor* Source, EPhysicalSurface Surface, float LoudnessOffsetDb = 0.f);

    int64 GetSimTick() const;
    const ais::AcousticDirective* GetDirective(const AActor* Robot) const;
private:
    ais::AisConfig Config;
    TUniquePtr<FLLAcousticEnvironment> Environment;
    TUniquePtr<ais::AcousticWorld> World;
};
```

Contract:

* **Fixed step.** Accumulate `DeltaTime`, call `Step()` at `ticks_per_second`.
  Cap catch-up steps per frame (e.g. 4) to avoid spirals on hitches; extra
  time is dropped, not accumulated forever.
* **Event time** is `World->CurrentTick()` at submission.
* **Robot host state** is pushed every step from the robot pawn/controller:
  location, `objectivePriority` (from LLTacticalDecision — UNVERIFIED), and
  visual contact (from the existing sight/perception — UNVERIFIED).
* **No client path.** There is no Server RPC that forwards a client's sound.
  If something must be client-initiated (e.g. a thrown object), the server
  re-derives the event from its own simulation of that action.

## 4. Event producers

| Producer | Hook | Notes |
|---|---|---|
| Weapon fire | Server fire path of the weapon / **LLProjectileBallistics (UNVERIFIED)** | One `Gunfire` per shot at muzzle; suppressor → negative loudness offset. Rate limiter tolerates full-auto (burst 12, refill 15/s at 30 Hz). |
| Bullet impacts | Server-side hit from **LLProjectileBallistics (UNVERIFIED)** | `MetalImpact` / `BreakObject` at impact with surface from the hit's physical material. Ricochets near robots are a strong tell. |
| Footsteps / sprint / landing | Server `UCharacterMovementComponent` state (speed, `IsFalling()` transitions) | Prefer movement-driven cadence on the server over anim notifies, which may not run on a dedicated server depending on animation tick settings. Crouch → large negative offset. |
| Doors / breakables | Server interaction / destruction code | `Door`, `BreakObject` |
| Explosions | Server damage application | `Explosion` |
| Machinery / generators | Interaction toggles → event; running machines → `IAcousticEnvironment::AmbientNoiseAt` | Starting a generator is suspicious; a running one masks. |
| Robot damage | **Robot Damage System (UNVERIFIED)** | Being hit should set host visual contact or high-threat state through the existing AI, not via fake sounds. Breaking robot armour can emit `MetalImpact`. |
| Decoys | `ALLAcousticDecoyActor` (server spawned) → `World->Decoys().Deploy()` | Destroying the actor calls `Destroy(id)`. |

## 5. FLLAcousticEnvironment (IAcousticEnvironment)

* `TraceOcclusion`: one segment query per (event, candidate robot) on a
  dedicated **acoustic trace channel** (to be created in project settings;
  name not verified). To get multiple walls with thickness, either trace
  forward and backward and pair entry/exit hits, or use a multi-trace on a
  channel where walls *overlap*. Ignore pawns and small props. Map each hit's
  physical material to an AIS `MaterialId`. Cap at `max_occlusion_hits`.
* `ZoneAt`: lookup against placed `ALLAcousticZoneVolume` actors
  (interior flag + zone id). Cache per actor per step where possible.
* `Weather`: read from the project's weather system (not verified).
* `AmbientNoiseAt`: sum of registered running machines near the listener.

Trace count per step is bounded by `max_perception_checks_per_tick`.

## 6. Robot side

`ULLAcousticSensorComponent` on robot pawns (server only) registers the
robot with a profile name (`SENTINEL`, `WRAITH`, data-defined) and pushes
host state each step.

The **directive is a recommendation**, consumed by the existing AI decision
layer — **LLTacticalDecision (UNVERIFIED)** — as one input among others:

| `AcousticDirective` field | Intended use |
|---|---|
| `state` | Blackboard enum / tactical state hint |
| `moveTarget`, `targetUncertaintyCm` | MoveTo location + acceptable radius / EQS around it |
| `lookAt` | Focus point while Suspicious |
| `reason` | Barks, debug, animation choices (e.g. `ObjectiveHeld` → head turn only) |
| `suspicion` | Alert meters / audio stingers |

AIS must not duplicate LLTacticalDecision: it decides *what the robot
believes it heard and whether that is worth checking*, while
LLTacticalDecision decides *how* to act (cover, routes, weapons, squad
roles). If LLTacticalDecision already has a search behaviour, feed it AIS's
search centre/radius instead of using AIS search points.

Engine note: UE ships `UAIPerceptionComponent` with `UAISense_Hearing`
(`ReportNoiseEvent`). AIS replaces that sense for robots because it needs
occlusion, bands, uncertainty, memory and decoy discrimination. If existing
robot AI already uses the hearing sense, migrate it or bridge it; do not run
both on the same robots.

## 7. Signal network port

`ISignalNetworkPort` (`Publish`, `Deliver`) must be implemented over the
project's **LLSignalNetwork (UNVERIFIED)**, honouring its range, jamming,
latency and bandwidth rules. If LLSignalNetwork already has robot-to-robot
messaging, `AcousticReport` should become one more message type there; do
not build a second network. The `LocalSignalRelay` is a test stand-in.

## 8. Replication of cosmetic audio

* AI decisions never replicate; only cosmetic cues do.
* Each step, feed `SetClientListeners` with player camera/pawn locations,
  then send `CosmeticCuesThisTick()` to each owning client with an
  **unreliable Client RPC on the PlayerController**, or let normal actor
  replication handle sounds that already exist (weapon fire FX are usually
  replicated by the weapon already — do not double-play them).
* Never `NetMulticast` every acoustic event.
* Playback goes through the project's **audio event interface (UNVERIFIED)**
  / MetaSounds, keyed by apparent category and surface.

## 9. Debugging

* `SetPerceptionObserver` → draw true vs estimated positions and uncertainty
  circles (`DrawDebugCircle`) in development builds only.
* `StateHash()` per step in logs for replay/desync checks between runs of the
  same server build.
* Expose `AcousticWorld::Stats()` and bus stats to `stat` groups or Insights.

## 10. Acceptance checklist for the Unreal pass (not yet done)

- [ ] Module compiles in UE 5.8.4 (Win64 editor + dedicated server targets).
- [ ] `ShouldCreateSubsystem` returns false on clients (verified in PIE with 2+ clients).
- [ ] Footsteps/gunfire produced on a **dedicated server** reach AIS.
- [ ] Occlusion trace pairing produces correct thickness on test walls.
- [ ] LLTacticalDecision / LLSignalNetwork adapters written against their real APIs.
- [ ] Cosmetic cues reach only nearby clients (net profiler).
- [ ] Server frame cost measured with 32+ robots on target hardware.
