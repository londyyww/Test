# LAST LIGHT — Environmental & World Intelligence V1

Three connected server-side gameplay subsystems plus the integration layer
that ties them to the existing Acoustic Intelligence System (AIS, PR #1) and
the Environmental Power Grid (llenv, PR #2):

| Subsystem | Core (portable, no AIS/llenv dependency) | Design doc |
|---|---|---|
| Environmental Hazards | `include/llworld/Hazards.h`, `src/Hazards.cpp` | [docs/ENVIRONMENTAL_HAZARDS_DESIGN.md](docs/ENVIRONMENTAL_HAZARDS_DESIGN.md) |
| Robot Squad Coordination | `include/llworld/Squad.h`, `src/Squad.cpp` | [docs/ROBOT_SQUAD_COORDINATION_DESIGN.md](docs/ROBOT_SQUAD_COORDINATION_DESIGN.md) |
| Dynamic Weather | `include/llworld/Weather.h`, `src/Weather.cpp` | [docs/DYNAMIC_WEATHER_DESIGN.md](docs/DYNAMIC_WEATHER_DESIGN.md) |
| Shared events & environment query | `include/llworld/WorldEvents.h`, `src/Ini.cpp` | [docs/CROSS_SYSTEM_INTEGRATION.md](docs/CROSS_SYSTEM_INTEGRATION.md) |
| Integration with AIS + power grid | `integration/WorldIntelligence.{h,cpp}` | [docs/CROSS_SYSTEM_INTEGRATION.md](docs/CROSS_SYSTEM_INTEGRATION.md) |

Also: [docs/UNREAL_INTEGRATION_CONTRACT.md](docs/UNREAL_INTEGRATION_CONTRACT.md),
[docs/VERIFICATION_REPORT.md](docs/VERIFICATION_REPORT.md).

## Dependency chain

```
kamiros (base) ← PR #1 AIS ← PR #2 Power Grid ← this branch (opus/environmental-world-intelligence-v1)
```

This work is stacked on PR #2 and must merge after it. It reuses AIS
(weather noise floors, signal port, robot priority lever) and the power grid
(energized/running state, damage, alarms, illumination, comm relays) without
modifying either.

## Build & test

```bash
cd systems/world_intelligence
make test        # core-only unit tests + full-stack tests, -Werror strict warnings
make demo        # scenarios A, B, C  (./build/world_demo B for one)
make sanitize    # AddressSanitizer + UndefinedBehaviorSanitizer
make ue-compat   # core + integration with -fno-exceptions -fno-rtti
cmake -S . -B build-cmake && cmake --build build-cmake && ctest --test-dir build-cmake
```

The Makefile and CMake expect `../acoustic_intelligence` and
`../environmental_systems` next to this folder (overridable via `AIS_DIR` /
`ENV_DIR`).

## Example data

* `data/industrial_site.ini` — power grid for "Pumping Station 9" (llenv format).
* `data/industrial_site_world.ini` — weather, hazards, squads (incl. a
  data-only `WARDEN` role) and player routes.

## Status summary

| | Status |
|---|---|
| Implemented | Weather, hazards, squads, shared bus/query, AIS + power-grid integration |
| Unit-tested | 34 tests on the portable core (no AIS/llenv linked) |
| Simulation-tested | 10 full-stack tests incl. scenarios A/B/C, plus `world_demo` |
| Unreal integration prepared | Contract in `docs/UNREAL_INTEGRATION_CONTRACT.md`; core builds with `-fno-exceptions -fno-rtti` |
| Unreal integration verified | **No** — no Unreal compile, PIE, or networked session was possible |
| Not yet verified | Target hardware, MSVC, real multiplayer, Mission Director / Tactical AI / Robot Damage / LLSignalNetwork adapters (systems not present) |
