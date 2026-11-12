# Verification Report — World Simulation & Scavenging V1

Branch `opus/world-simulation-scavenging-v1`, based on the PR #3 head
`05678ef46b7a4cdc6adbfbe93a0d6599c13cc751`.

Environment: Linux sandbox (Modal, 2 vCPU, CPU model not exposed), GCC 13.3.0,
CMake 4.4.4. **No Unreal Engine, no UBT/UHT, no editor, no network play-test.**

## What was run

| Check | Command | Result |
|---|---|---|
| Portable core tests (no AIS / power grid linked) | `make unit` | **47 tests, 44,661 checks, 0 failed** |
| Full-stack tests (core + PR #1–#3 + integration) | `make test` | **54 tests, 44,737 checks, 0 failed** |
| AddressSanitizer + UndefinedBehaviorSanitizer | `make sanitize` | both suites pass, no reports |
| Strict warnings as errors | every target (`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast …`) | clean |
| No exceptions / no RTTI | `make ue-compat` | core + integration compile with `-fno-exceptions -fno-rtti` |
| CMake build + CTest | `cmake -S . -B build && cmake --build build && ctest` | 2/2 pass |
| Scenarios A–D | `make scenarios` (twice, outputs compared with `cmp`) | byte-identical output, combined hash stable |
| Regression: AIS (PR #1) | `make -C ../acoustic_intelligence test` | 52 tests, 2,533 checks, 0 failed |
| Regression: power grid (PR #2) | `make -C ../environmental_systems test` | 37 tests, 697 checks, 0 failed |
| Regression: world intelligence (PR #3) | `make -C ../world_intelligence test` | 34 + 44 tests (222 + 288 checks), 0 failed |

No existing test was modified or removed. No file outside
`systems/world_simulation/` was changed, apart from the new archive.

## Test coverage map

**Loot (`test_loot.cpp`):** deterministic generation for every container;
raid-seed reproduction and variation; location profiles (a depot crate always has
ammunition, flats never have military items); weighted distribution within ±3%
of configured weights over 6,000 raids; location rarity weights; structured
condition and fill ranges and grades; capacity never exceeded, with overflow
counted; explicit empty reasons (authored, destroyed, scavenged, profile);
guaranteed drops; mutually exclusive drops; group dependencies; container
condition effects.

**Containers (`test_containers.cpp`):** service equals pure generator;
duplicate requests (same sequence, zero sequence, re-search, re-take);
simultaneous searches by four players have one winner; simultaneous takes;
shared vs per-player instances, with ownership independent of connection order;
power, clearance, tool, seal, hidden and obstructed access; interrupted search
(disconnect, out of range, cancel) recovers identical contents; grant retry after
a lost response applies the item exactly once; inventory-full returns the item;
client authority rejected; player limit and recipient-key stability; raid
lifecycle.

**World simulation (`test_sim.cpp`):** tier intervals; tier promotion and demotion
by players, observation and threat; generator fuel (analytic), low-fuel and
backup; power-state propagation; patrol transitions that do not depend on tier;
scheduled retask; comms interruption queues and later delivers central orders,
with a bounded queue; alarm → lockdown → central patrol response; disturbances
heard only in earshot; a distant facility needing over 300× fewer updates;
override expiry, completion freeze, persistence export and import, world reset;
deterministic replay (state hashes plus event log); bounded queues and budgets
without starvation; hazard-zone cycle.

**Configuration (`test_config.cpp`):** all shipped content valid individually and
together; unsupported or missing version; unknown sections and keys; duplicate
ids; missing item references; invalid weights and probabilities; impossible
capacities and quantities; contradictory exclusivity; out-of-order and circular
loot dependencies; unresolved containers; invalid intervals; missing and circular
entity dependencies; invalid schedules; circular rule chains; rules targeting
bound entities.

**Integration (`test_integration.cpp`):** see `CROSS_SYSTEM_INTEGRATION.md`.

## Scenario results (`make scenarios`)

| Scenario | Observed |
|---|---|
| A — military depot | The crate is rejected `RejectedNoPower` while the depot is dark. Starting the generator through the existing facility interaction powers the rack lock (1 `AccessChanged`). The world simulation brings grid, comms and patrol online. The patrol enters the yard, the search starts, and the patrol gets `RESPOND to DF_ARMORY` (local). Two robots perceive the search's AIS event (awareness Suspicious). The crate yields 7.62×39 ammo, gas-mask filters, radio batteries and a damaged signal processor (seed 0xDF01) |
| B — industrial facility | The power grid burns 1,800 ticks of fuel. At 60 s the generator goes off, grid, press and facility go Offline, and the emergency light runs on battery. The blackout rule moves the patrol to the loading dock. The damaged toolbox yields a worn gear assembly |
| C — medical facility | Eight containers with different outcomes: stocked, damaged, evacuated (empty), destroyed, obstructed (cleared, then searched), scavenged, the staff locker, and the hidden cache (discovered, then searched). Replaying with the same seed gives identical containers and contents |
| D — distant facility | Players stay in the residential block, 2.8 km away. Relay fuel goes low at 180 s and empty at 240 s, backup starts at 243 s, the patrol shift changes at 120 s, and an intrusion alarm at 360 s causes lockdown and a central patrol response. The relay's 8 entities used **316** scheduled updates over 400 s (full rate: 96,000). On arrival the state shows immediately: generator Offline, backup Active (fuel 1,642.5 s), grid Degraded, facility and storage in Lockdown |

## Benchmarks (portable core CPU only)

`make bench`: 10 simulated minutes (18,000 steps at 30 Hz), four players walking
across a ~10 km synthetic map, clusters of 10 entities (generator + backup,
network, security, comms, hazard cycle, facility, storage, machinery, patrol)
with lockdown rules and scheduled alarms. Single thread, `-O2`.

| Entities | Total ms (18,000 steps) | Mean µs / step | p99 µs | Max µs | Scheduled updates | Per-tick equivalent | Timers | Transitions |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 2.7 | 0.11 | 1.47 | 6.8 | 15,354 | 1,800,000 | 143 | 381 |
| 500 | 7.9 | 0.39 | 3.01 | 21.9 | 62,920 | 9,000,000 | 607 | 1,450 |
| 1,000 | 14.2 | 0.74 | 4.05 | 43.1 | 117,135 | 18,000,000 | 1,213 | 2,904 |
| 5,000 | 72.4 | 3.97 | 14.78 | 187.6 | 534,538 | 90,000,000 | 6,040 | 14,524 |

At 5,000 entities: updates by tier High / Medium / Low were 36,720 / 220,550 /
277,268. 9 updates were deferred by the per-step budget, with 0 queue overflows.

Loot generation: 540,000 containers (27 containers × 20,000 raids) in 220.8 ms,
**0.41 µs per container** (1.36 items on average).

**Interpretation limits.** These numbers are wall-clock timings of the portable
C++ core in a shared, burstable sandbox whose CPU model is not exposed; expect
run-to-run variance. They exclude Unreal's game thread, actors, components,
replication, navigation, AIS propagation and rendering. They are **not** a
measurement on the i5-10400F / GTX 1660 Super target and support **no FPS claim**.

## Remaining limitations

* **Unreal:** not compiled with UBT/UHT, not run in the editor, and no Insights
  profile. The contract in `UNREAL_INTEGRATION_CONTRACT.md` is a specification.
* **Multiplayer:** duplicate, simultaneous, interrupted and retried requests are
  verified in local deterministic simulation only. No real network, latency,
  packet loss or reconnect testing.
* **Unavailable systems:** Mission Director, Tactical AI, LLSignalNetwork, Robot
  Damage System, the item catalog and the inventory and persistence services are
  not in this repository. They are reached only through documented adapters
  (`IItemCatalog`, `ILootGrantPort`, lifecycle API, `PatrolDirective`,
  `PersistentEntityRecord`). The adapters are verified against test doubles,
  not the real services.
* **Squad routing:** `SquadCoordinator` has no API to change patrol routes, so
  world-simulation patrol directives are published for a consumer rather than
  pushed into squads. Robot reactions in the tests come from the existing AIS and
  squad behaviour.
* **Abstract vs bound facilities:** in V1 a facility is either abstract (fuel
  modelled here) or bound to a loaded power grid for the whole raid. Handing
  abstract state to a facility that streams in mid-raid is not implemented.
* **Low-tier directive timing:** directives for Low-tier patrols are emitted at
  their coarse update (≤ 20 s late), by design. Promotion catches up exactly.
* **Positions:** container and area positions come from content. Keeping them
  in sync with placed Unreal actors is an integration decision (see the contract).
* **Robot movement** in scenarios and tests is a straight-line stand-in for Unreal navigation.
