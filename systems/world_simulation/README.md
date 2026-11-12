# LAST LIGHT — World Simulation & Scavenging Framework V1

Two connected, portable C++17 gameplay subsystems for LAST LIGHT: Echoes of Tomorrow.

* **World Loot & Scavenging** (`llloot`): data-driven, deterministic loot
  generation and a server-authoritative container interaction framework. It
  generates and exposes loot; the authoritative inventory and item catalog stay
  external (`ILootGrantPort`, `IItemCatalog`).
* **Mission World Simulation** (`llsim`): tiered, event-driven simulation of
  patrols, generators, power networks, security, comms, storage, machinery and
  hazard zones that keeps running when players are elsewhere.
* **Integration** (`integration/MissionWorldHost`): binds both to the acoustic
  system, power grid, hazards, weather and squads from PR #1–#3 through explicit ports.

No Unreal headers. Strict warnings, ASan/UBSan clean, builds without exceptions and RTTI.

## Build and run

```sh
cd systems/world_simulation
make test        # core-only unit tests + full-stack tests
make scenarios   # scenarios A-D (deterministic output)
make bench       # portable-core CPU benchmark (100 / 500 / 1,000 / 5,000 entities)
make sanitize    # ASan + UBSan
make ue-compat   # -fno-exceptions -fno-rtti compile check
make regress     # PR #1-#3 suites
# or: cmake -S . -B build && cmake --build build && (cd build && ctest)
```

Requires the sibling directories `../world_intelligence`, `../environmental_systems`
and `../acoustic_intelligence`. The portable core only needs
`../world_intelligence/include` and `src/Ini.cpp`, for the shared INI convention.

## Layout

```
include/llloot/   Loot.h (definitions, validation, generation), ContainerService.h
include/llsim/    WorldSim.h (config, entities, lifecycle), Scheduler.h (bounded heap)
include/llcontent/Content.h (one loader, one version, both subsystems)
src/              implementations
integration/      MissionWorldHost (ports to PR #1-#3)
data/common.ini   tiers, loot vocabulary, container types
data/locations/   dead_frequency_depot, dead_frequency_relay_station, industrial_facility, medical_facility, residential_building
data/scenarios/   power-grid and world-intelligence data for the full-stack scenarios
data/fixtures/    TEST FIXTURE item catalog (not an item database)
tests/            unit, config, integration tests; FixtureItemCatalog; StackRig
examples/         scenarios.cpp, benchmark.cpp
docs/             design, configuration, simulation, scheduler, integration, Unreal contract, verification report
```

Start with `docs/VERIFICATION_REPORT.md` for what was and was not verified.
