# World Loot & Scavenging — Design

Part of **LAST LIGHT — World Simulation & Scavenging Framework V1** (`systems/world_simulation`).
Portable C++17, no Unreal headers, server-authoritative, deterministic.

## Goal

Loot tells the story of a place. A Dead Frequency armory crate holds ammunition and
signals gear. A flat in the residential block holds tea, tins and a diary. A field
clinic's cabinets were half emptied during the evacuation. The framework turns
data that describes *a place and its history* into reproducible container
contents. It never owns what players keep.

## What this system is — and is not

| Owned here | Not owned here (adapter instead) |
|---|---|
| Location / profile / table / container definitions | Item identity, stacking, footprint → `IItemCatalog` |
| Deterministic generation of container contents | Inventory, stashes, equipment ownership → `ILootGrantPort` |
| Container search state for one raid | Profiles, persistence, saving |
| Atomic search claims, duplicate rejection | Power state → `ILootAccessPort::IsPowered` (power grid) |
| Per-item hand-off reservations with stable grant ids | Tools / keycards → `ILootAccessPort::HasItem/HasClearance` (inventory) |
| Search sounds as events | Acoustic propagation → AIS via `ILootSoundSink` |

Loot data references **stable item ids** (`ammo.545x39.ps`, `med.surgical_kit`),
never display names. Validation resolves every id against `IItemCatalog`. The
repository ships `tests/fixtures/FixtureItemCatalog.h` and
`data/fixtures/item_catalog_fixture.ini` **for tests and examples only**. That
fixture mirrors the three fields the loot system reads. It is not an item database
and must not be used in production builds.

## Data model

```
LocationDef (type, rarity weights, chance scale, integrity shift, scavenged chance, ownership)
  └─ default ProfileDef per ContainerCategory
ContainerTypeDef (category, capacity, search/force ticks & noise, interact range)
ContainerDef (id, location, type, [profile], position, condition, access[], [ownership], starts_empty)
ProfileDef (empty chance, pools[])
  └─ PoolDef (table, chance, rolls, guaranteed, unique, group, exclusive, requires)
       └─ TableDef → TableEntryDef (item, weight, rarity, qty range, condition, group, exclusive, requires)
CategoryDef (default condition range, uses_fill)   ConditionRangeDef (integrity range, fill range)
```

* **Location types:** MilitaryDepot, MilitaryCheckpoint, Workshop, MedicalFacility,
  Residential, Industrial, CommunicationsStation, VehicleWreck, UndergroundStorage,
  HiddenCache.
* **Container categories:** Locker, StorageCabinet, MilitaryCrate, MedicalContainer,
  Toolbox, Shelf, VehicleCompartment, HiddenStash, EquipmentCase.
* **Item categories** are data (`[category.*]`). The example set covers
  ammunition, medical, mechanical, electrical, tools, military equipment, weapon
  attachments, armor attachments, recovered technology, intelligence and
  consumables. Adding a category is a data change.

## Generation algorithm (`GenerateLoot`)

A pure function of `(config, catalog, container, seed)`:

1. `seed = HashCombine(HashCombine(raidSeed, FNV1a(containerId)), recipientKey)`.
   `recipientKey` is 0 for shared containers and a stable per-player key for
   per-player containers.
2. A destroyed container or an authored `starts_empty` container returns empty
   with an explicit reason.
3. Roll the location `scavenged_chance` (skipped for hidden containers: earlier
   scavengers never found them), then the profile `empty_chance`.
4. Process pools: **guaranteed pools first**, then optional pools, each group in
   declaration order. Each pool has its own RNG stream derived from the seed, so
   editing one pool never reshuffles another.
5. A pool is skipped when its `requires` group has not been produced or its
   `exclusive` tag is taken. Optional pools roll `chance × location.chance_scale`.
6. Each roll picks an entry by `weight × location.rarity_weight[rarity]`,
   excluding already-picked entries (when `unique`), entries whose exclusive tag is
   taken, and entries whose dependency is unmet.
7. Quantity is uniform in the entry range, capped by the catalog stack limit.
   Condition is integrity from the range (×container condition factor + location
   shift) plus fill for categories with `uses_fill`.
8. A pick that does not fit the remaining capacity is counted in
   `capacityRejected`; it never overflows.

The RNG is SplitMix64 with integer range reduction. Standard `<random>`
distributions are implementation-defined and would break cross-platform
reproduction, so they are not used.

### Item condition

`ItemCondition { integrity 0..1, fill 0..1, grade }` is gameplay data:

* **integrity** is the normalised physical state. The authoritative item contract
  maps it to its own representation. This module adds no durability or repair system.
* **fill** is the remaining contents for supplies (saline bags, rations, battery
  charge). It is 1.0 for categories without `uses_fill`.
* **grade**: Pristine ≥ 0.9 > Intact ≥ 0.7 > Worn ≥ 0.45 > Damaged ≥ 0.2 > Degraded.

Container condition also matters. `Damaged` multiplies integrity by 0.85.
`PartiallyDestroyed` halves capacity and multiplies integrity by 0.6. `Destroyed`
holds nothing. All factors are configurable in `[loot]`.

## Container interaction (`ContainerService`)

The server owns one `ContainerService` per raid.

| Action | Effect |
|---|---|
| `Search` | Atomic claim → timed search (+force time when a tool requirement is used) → contents generated and revealed |
| `CancelSearch` | Releases the claim; nothing revealed |
| `Discover` | Marks a hidden container found (optional interaction) |
| `ClearObstruction` | Timed work on partially destroyed storage |
| `TakeItem` | Per-slot hand-off to the authoritative inventory via `ILootGrantPort` |

**Access requirements** (`access = ...`): `power:<node>` (electronic lock, latches
open once released), `tool:<item>` (damaged lock, loud), `clearance:<tag>`
(restricted military access), `discovery` (hidden), `work:<ticks>` (debris), and
`seal:<world entity>` (sealed while that world-simulation entity is in Lockdown).

### Multiplayer guarantees (1–4 players)

* **Server authority.** A service built with `serverAuthority = false` rejects
  everything. Clients never generate or hold contents.
* **Duplicate requests.** Each player has a ring of the 64 most recent sequence
  numbers, and sequence 0 is invalid. A replayed packet returns
  `RejectedDuplicate` with no side effects.
* **Simultaneous searches.** Requests are processed serially, and the first one
  sets `Searching` with a claimant. Concurrent requests get `RejectedBusy`. A
  searched shared container returns `RejectedAlreadySearched`.
* **Simultaneous takes.** The slot is `Reserved` before the inventory is called,
  so the second taker gets `RejectedSlotTaken`.
* **Never twice.** Each item instance has a stable `grantId` derived from the
  container seed and slot. The inventory port must treat it idempotently.
  `Unavailable` keeps the slot reserved and retries with the same id.
  `AlreadyApplied` (lost response) resolves to Taken. `Rejected` (no space)
  returns the item to the container.
* **Interrupted interactions.** Disconnecting, walking out of range or cancelling
  releases the claim. Nothing is revealed, and the next search regenerates
  identical contents because generation is deterministic.
* **Ownership policy.** `Shared` gives one instance per container. `PerPlayer`
  gives one instance per player, seeded by a stable recipient key such as the
  party slot, so connection order does not matter. Location default and
  per-container override are both supported; for example, the residential
  caretaker locker is shared inside a per-player building.

## Example content

| File | Location type(s) | Story beats |
|---|---|---|
| `dead_frequency_depot.ini` | MilitaryDepot, MilitaryCheckpoint | powered armory rack, keycard locker, damaged signals case, collapsed cabinet, crate sealed by lockdown, hidden guard stash |
| `industrial_facility.ini` | Industrial | worn tools, damaged toolbox with degraded parts |
| `medical_facility.ini` | MedicalFacility | evacuated pharmacy, damaged ward cabinet, fallen fridge, burned X-ray cabinet, hidden nurse cache (one of two kits) |
| `residential_building.ini` | Residential | per-player flats, heavily scavenged, damp |
| `dead_frequency_relay_station.ini` | CommunicationsStation | equipment sealed by lockdown (scenario D) |
