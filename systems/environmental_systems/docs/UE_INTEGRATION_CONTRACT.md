# Environmental Systems — Unreal Engine 5.8.4 Integration Contract

**Status: specification only. Nothing here was compiled against Unreal or run
in the editor.** UE class/function names are proposals based on general UE5
conventions and must be checked against 5.8.4. Mission systems, Tactical AI,
LLSignalNetwork, Robot Damage System and profile/persistence systems were
**not present in the repository**; every binding to them is marked
**UNVERIFIED**.

## 1. Module layout

```
Source/LastLightEnvironment/
  Private/Core/                  ← include/llenv + src verbatim (no UE types)
  Private/Bridge/                ← bridge/AisAcousticBridge (depends on the AIS module)
  Public/LLFacilitySubsystem.h   UWorldSubsystem (server) owning llenv::Facility instances
  Public/LLFacilityDefinition.h  UPrimaryDataAsset holding the facility (or an imported INI)
  Public/LLInfrastructureActor.h Base actor per node: mesh, FX, interaction prompt, replication
  Public/LLInfrastructureState.h USTRUCT mirror of NodeReplicationState
```

The core builds with `-fno-exceptions -fno-rtti` (`make ue-compat`) and has no
global state.

## 2. Authoring

* Designers place `ALLInfrastructureActor`s (or subclasses: door, generator,
  light…) and give each a **NodeId**. The facility definition (data asset or
  INI) carries type, network, `fed_by`, capacities and rules. Actor transform
  supplies `position` at load.
* No map references in C++: the subsystem builds a `FacilityDef` from all
  infrastructure actors/data in the loaded level and calls
  `ValidateFacility`. Errors are reported as map-check warnings and the
  facility does not start.
* Several facilities per level are allowed (one `Facility` each, distinct
  `facility.id`).

## 3. ULLFacilitySubsystem (server only)

```cpp
// ILLUSTRATIVE — NOT COMPILED
UCLASS()
class ULLFacilitySubsystem : public UTickableWorldSubsystem {
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;   // false when the world is NM_Client
    virtual void Tick(float DeltaTime) override;                          // fixed 30 Hz Step(); cap catch-up steps
    ELLInteractionResult HandleInteraction(APlayerController* PC, const FLLInteractionRequest& Request);
    void ApplyInfrastructureDamage(FName NodeId, float Amount);           // from damage pipeline (UNVERIFIED)
private:
    TArray<TUniquePtr<llenv::Facility>> Facilities;
};
```

* **Players:** on `PostLogin`/`Logout` call `ConnectPlayer`/`DisconnectPlayer`
  with a server-assigned id (1–4). Push pawn locations each step via
  `UpdatePlayerPosition` (4 calls per tick).
* **Requests:** the client sends a **Server RPC** on its PlayerController:
  `ServerInteract(FName NodeId, ELLAction Action, uint32 Sequence, uint32 ExpectedVersion, FName CommandTarget)`.
  The server resolves the player id from the connection, **never from the
  payload**, then calls `Facility::Submit`. The result may be sent back with an
  unreliable Client RPC for UI feedback.
* **Hold interactions** (crank, repair, hack, call): the client sends the
  start once; the server keeps the player in the channel while they stay in
  range and connected; release → `CancelChannel`.
* **Permissions:** map squad roles or class perks to the `required_permissions`
  bitmask (profile system — UNVERIFIED).

## 4. Replication

* Each `ALLInfrastructureActor` has a replicated `FLLInfrastructureState`
  (`ReplicatedUsing=OnRep_State`). Each step the subsystem calls
  `CollectReplication` and writes only changed nodes into their actors, so
  normal actor relevancy and dormancy apply. Consider `DORM_DormantAll` plus
  `FlushNetDormancy` on change.
* `OnRep_State` drives visuals: door animation, light components, siren
  FX/audio, generator exhaust. Clients **never** call facility logic.
* An optional client-side `llenv::Facility(AuthorityRole::Client)` can mirror
  state for UI/prediction through `ApplyReplication`; it cannot change power.

## 5. Acoustic Intelligence

Instantiate `AisAcousticBridge` with the server's `ais::AcousticWorld` and
set it as the facility's `IAcousticSink`. The UE `IAcousticEnvironment`
implementation (see the AIS contract) must expose an ambient registry
equivalent to `BoxAcousticEnvironment::ClearAmbient/AddAmbient` so running
generators mask nearby sounds. Cosmetic playback of facility sounds stays
client-side via `OnRep_State` and AIS cosmetic cues — no duplicate multicast.

## 6. Other systems (UNVERIFIED adapters)

| System | Contract |
|---|---|
| Mission system | Subscribe to `IFacilityObserver`; `ObjectiveSignal` events (`text` = signal name, e.g. `extraction_called`, `comms_online`) advance objectives. Mission start/end decides `ImportWorldState` / `ExportWorldState(extracted)`. |
| Tactical AI | `AlertLevelChanged` → patrol/alert state; `NavBlockChanged` → update nav modifiers or smart links for doors/gates/machinery; `IlluminationAt` → robot vision multiplier & player visibility; `NetworkOverloaded`/`BreakerTripped` → optional "investigate infrastructure" tasks. |
| LLSignalNetwork | Not referenced. If comm arrays should boost or jam robot comms, an adapter listens for `CommArray` power/switch changes and calls into LLSignalNetwork. Do not duplicate its messaging. |
| Robot Damage System / weapons | Explosions and bullet hits on infrastructure call `ApplyInfrastructureDamage` → `ServerApplyDamage`. Robots shooting generators is a valid tactic. |
| Profile / inventory | Implement `IPlayerInventory` over the real inventory. Keycards and tools are checked (`HasItem`); fuel, fuses and parts are consumed (`ConsumeItem`) — whether that consumption is permanent is the inventory system's rule. Never write switch states to profiles. |
| World save | Store the `WorldStateRecord::Serialize()` text per `facility.id` in the world/save-game layer (not per player). |

## 7. Acceptance checklist (not yet done)

- [ ] Module compiles for Win64 editor + dedicated server.
- [ ] Subsystem absent on clients (PIE, 1 listen server + 3 clients).
- [ ] Server RPC ignores spoofed player ids; rate limiting holds under a spam macro.
- [ ] Door/gate nav modifiers update when state changes.
- [ ] Two players crank a gate together over a lossy connection (net emulation).
- [ ] Disconnect mid-crank / host migration (if supported) restores via `Capture/Restore`.
- [ ] World state saved/restored across two missions on the same map.
- [ ] Server cost measured with 300+ nodes on the target i5-10400F.
