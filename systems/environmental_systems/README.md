# LAST LIGHT — Environmental Interaction & Power Grid V1

Abandoned military infrastructure as a live, interconnected system. Generators,
breakers, batteries, doors, gates, lights, alarms, sensors, terminals, comms
and extraction points are wired together in data. Everything responds to its
**actual** power state, and every change can have side effects: noise that
robots hear, alarms that reboot when power returns, breakers that trip when
someone switches on one light too many.

This folder contains a standalone, engine-agnostic C++17 core, a bridge to the
Acoustic Intelligence System (`../acoustic_intelligence`), tests, an example
facility and simulation, and the Unreal Engine 5.8.4 integration contract.

## Status at a glance

| Area | Status |
|---|---|
| Power grid: main/backup generators, batteries, junctions, breakers, consumers, multiple independent networks | **Implemented + tested** |
| Shortages (priority load shedding), breaker overload trips, generator overload trips, fuel, damage/destroy/repair | **Implemented + tested** |
| Backup auto-start on primary loss / auto-stop on restore; battery discharge & recharge | **Implemented + tested** |
| Objects: doors (fail-safe / fail-secure, keycard, crank, force), security gates (2-player crank), lights, emergency lights, machinery, terminals (hack + remote commands), alarms, security sensors, comm arrays, extraction points | **Implemented + tested** |
| Acoustic output (generator start/running/backfire, machinery, door motor/forced, breaker trip, alarm, beacon) and continuous masking sources | **Implemented + tested**, incl. through the AIS bridge with real AIS robots |
| Server-authoritative interactions: range, permissions, dedup, rate limiting, optimistic versioning, 1–4 players, disconnect/reconnect | **Implemented + tested** in-process |
| Replication deltas and client mirror | **Implemented + tested** in-process (no network transport) |
| Persistence: mission-scoped vs world (`World`, `WorldOnExtraction`) durable fields; mission snapshot/restore | **Implemented + tested** |
| Data-driven facility definitions with strict validation (cycles, cross-network links, typos…) | **Implemented + tested** |
| Example "Relay Outpost" facility + simulation with 3 players, 2 Sentinels, 1 Wraith | **Simulated** (`examples/facility_demo.cpp`) |
| Unreal subsystem, actors, replication, save integration | **Specified only** — not compiled, not run |
| Mission system, Tactical AI, LLSignalNetwork, Robot Damage System, profile/persistence systems | **Not present in this repository — integrations unverified** |
| Real multiplayer (network latency, packet loss, listen-server host migration) | **Not validated** |

### Existing project systems

This branch is stacked on the Acoustic Intelligence System (AIS) PR. AIS is
the only LAST LIGHT gameplay system that exists in the repository, and it is
reused through `bridge/AisAcousticBridge`. One additive change was made to AIS:
a new `Alarm` stimulus category, so sirens are not mislabelled as machinery.
All 52 AIS tests still pass. Mission systems, tactical AI, LLSignalNetwork,
Robot Damage System and profile/persistence systems were **not found**. The
facility exposes ports for them (`IFacilityObserver`, `IPlayerInventory`,
`ServerApplyDamage`, `ExportWorldState`) and does not call any invented API.

## Build & test

```bash
cd systems/environmental_systems
make test        # 37 tests / ~700 checks, -Werror strict warnings, includes AIS bridge tests
make demo        # Relay Outpost simulation timeline
make sanitize    # ASan + UBSan
make ue-compat   # core + bridge with -fno-exceptions -fno-rtti

cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release   # AIS_DIR defaults to ../acoustic_intelligence
cmake --build build-cmake && ctest --test-dir build-cmake
```

Verified here: GCC 13.3 (Linux x86-64), CMake 4.4. MSVC is configured but
**not tried**.

## Layout

```
include/llenv/   EnvTypes, FacilityDefinition (data + loader), Ports (events, sinks, requests), Facility
src/             implementation (no dependency on AIS)
bridge/          AisAcousticBridge: llenv sounds -> AIS events + ambient masking
data/            relay_outpost.ini - example facility (pure data)
tests/           test_power, test_objects, test_coop, test_bridge
examples/        facility_demo.cpp
docs/            ARCHITECTURE, UE_INTEGRATION_CONTRACT, PERFORMANCE, KNOWN_LIMITATIONS
```

## Test coverage map

| Requirement | Tests |
|---|---|
| Generator activation/shutdown | `Generator_ActivationStartupAndShutdown`, `Generator_FuelRunsOut…` |
| Power distribution / multiple circuits | `Distribution_BreakerIsolatesOnlyItsBranch`, `Distribution_MultipleNetworks…` |
| Power overload | `Overload_BreakerTrips…`, `Overload_ShortageSheds…` |
| Damaged generators | `Damage_ReducesOutput…`, `Damage_StartFailuresAreDeterministic` |
| Backup power | `Backup_AutoStarts…`, `Backup_DoesNotStart…`, `Battery_Discharges…` |
| Door behaviour | `Door_*`, `Gate_RequiresTwoPlayers…` |
| Emergency lighting | `EmergencyLighting_ActivatesOnLoss…` |
| Alarm activation / acoustic events | `Alarm_*`, `Machinery_*`, `Bridge_*` |
| Simultaneous interactions / duplicates | `Coop_SimultaneousToggle…`, `Coop_DuplicateRequests…`, `Gate_…` |
| Network authority | `Authority_ClientsCannotChangePower…`, `Coop_RangePermission…` |
| Player disconnection | `Coop_DisconnectPausesSharedWork…` |
| State restoration | `Persistence_*` |
| Invalid configurations | `Config_InvalidDefinitionsAreRejected` |
| Event flooding | `Flooding_*` |
| Deterministic scenarios | `Determinism_FacilityScenarioReplaysIdentically`, `Damage_StartFailuresAreDeterministic` |
| Event-driven cost | `Performance_EventDrivenIdleTicksDoNotResolve` |
