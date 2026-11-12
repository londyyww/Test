# LAST LIGHT — Acoustic Intelligence System (AIS) V1

Robots in LAST LIGHT hear before they see. AIS turns server-side gameplay
sounds into **imperfect, per-robot acoustic knowledge**: a noisy position
estimate with an uncertainty radius, a remembered "disturbance", rising
suspicion and an investigation recommendation. No robot is ever told where
the player is.

This folder is a standalone, engine-agnostic C++17 gameplay core plus an
Unreal Engine 5.8.4 integration contract. It does **not** contain Unreal code
that has been compiled.

## Status at a glance

| Area | Status |
|---|---|
| Event model, 12 stimulus categories, surfaces, decoy signatures | **Implemented + tested** |
| Propagation: distance, air absorption, occlusion, materials, interior/exterior, rain, wind, ambient masking | **Implemented + tested** (gameplay approximation, not acoustics) |
| Server event bus: authority, validation, dedup, token-bucket rate limiting, bounded priority queue | **Implemented + tested** incl. adversarial floods |
| Robot brain: memory traces, fusion, confidence decay, 6-state investigation machine, search, habituation | **Implemented + tested** |
| SENTINEL / WRAITH profiles + data-only third archetype (WARDEN in `data/ais_default.ini`) | **Implemented + tested** |
| Decoys (timed burst, repeating metallic, recorded mechanical, emergency radio), expiry, destroy | **Implemented + tested** |
| Robot-to-robot reports (range, latency, rate, capacity), flanking support roles | **Implemented + tested** with the local relay *stand-in* for LLSignalNetwork |
| Cosmetic cue relevancy per client (no AI data, no decoy truth) | **Implemented + tested** (selection only; no networking) |
| Deterministic replay (state hash per tick) | **Tested** on one compiler/platform (GCC 13, x86-64 Linux) |
| Multi-robot behaviour in a scripted compound | **Simulated** (`examples/acoustic_demo.cpp`, straight-line movement) |
| Unreal subsystem/components, traces, replication | **Specified only** (`docs/UE_INTEGRATION_CONTRACT.md`) — **not compiled, not run** |
| LLTacticalDecision, LLSignalNetwork, LLProjectileBallistics, Robot Damage System, existing robot AI, audio event interfaces | **Not found in this repository — integrations unverified**, see below |
| Multiplayer behaviour over a real network | **Not validated** |

### About the existing project systems

The task asked AIS to extend LLTacticalDecision, LLSignalNetwork,
LLProjectileBallistics, the Robot Damage System, existing robot AI and audio
event interfaces. This repository (and its other remote branch) currently
contains only the KS-74 rifle art; none of those systems' source is present.
So AIS does **not** call any of their APIs. Instead it exposes narrow ports
(`IAcousticEnvironment`, `ISignalNetworkPort`, `AcousticDirective`,
`CosmeticAudioCue`, `SubmitEvent`) that an adapter must bind once the real
code is available. Every binding in the contract is marked *UNVERIFIED*.

## Build & test

Requirements: any C++17 compiler. Verified here with GCC 13.3 on Linux.

```bash
cd systems/acoustic_intelligence
make test        # strict warnings as errors, 52 tests / ~2500 checks
make demo        # example simulation timeline
make sanitize    # tests under AddressSanitizer + UBSan
make ue-compat   # core built with -fno-exceptions -fno-rtti (Unreal-style flags)

# or CMake (verified with CMake 4.4 + GCC 13)
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake && ctest --test-dir build-cmake
```

Run a subset: `./build/ais_tests Decoy` (substring filter).
Demo with designer overrides: `./build/ais_demo data/ais_default.ini`.

MSVC flags are set in `CMakeLists.txt` (`/W4 /WX`) but **MSVC was not
available here and has not been tried**.

## Layout

```
include/ais/   public headers (engine-agnostic)
src/           implementation
tests/         dependency-free test suite
examples/      acoustic_demo.cpp — several robots reacting to a player
data/          ais_default.ini — designer overrides incl. a data-only archetype
docs/          ARCHITECTURE, UE_INTEGRATION_CONTRACT, PERFORMANCE, KNOWN_LIMITATIONS
```

## Test coverage map

| Requirement | Tests |
|---|---|
| Distance attenuation | `Propagation_DistanceAttenuation…`, `…ZeroDistance…` |
| Occlusion / material transmission | `Propagation_WallOccludes…`, `…MaterialTransmission…` |
| Interior/exterior, rain, wind, machinery | `…InteriorExterior…`, `…RainAndWind…`, `…AmbientMachinery…`, `…HeavyRain…` |
| Robot sensory profiles | `Profiles_*` |
| Multiple simultaneous sounds | `Brain_SimultaneousSounds…` |
| Event deduplication | `Bus_Deduplicates…` |
| Repeated sound detection | `Brain_RepeatedSoundsEscalate…` |
| Single distant sound ≠ combat | `Brain_SingleDistantSoundNeverTriggersCombat`, `Config_StrictLoader…` |
| Confidence decay | `Brain_ConfidenceAndSuspicionDecay…` |
| Investigation target updates | `Investigation_TargetUpdates…` |
| Lost-source searching | `Investigation_LostSourceSearch…`, `…SourceThatFades…` |
| Decoy detection / expiry | `Decoy_*` |
| Multiple robots, one event | `Network_MultipleRobotsOneEvent…`, `Network_SupportRobots…` |
| Multiplayer authority assumptions | `Authority_*`, `Bus_ClientClaims…` |
| Queue limits / flooding | `Bus_BoundedQueue…`, `Bus_AdversarialFlood…`, `Bus_OneSourceSpamming…`, `Brain_MemoryAndInboxAreBounded`, `World_PerceptionBudget…` |
| Deterministic replay | `Determinism_ReplayIsBitIdentical` |
