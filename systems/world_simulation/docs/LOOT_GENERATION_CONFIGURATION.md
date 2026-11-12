# Loot Generation Configuration

All world content uses the existing LAST LIGHT INI convention: the
`llworld::ParseIni` / `IniBinder` parser shared with the power grid and
world-intelligence data. Comments start with `;` or `#`. One file may hold loot
sections and world-simulation sections. The loader rejects sections that no
subsystem claims.

## Files and loading

```
data/common.ini                        shared vocabulary: [simulation], [tier.*], [loot], [category.*], [condition.*], [container_type.*]
data/locations/<location>.ini          one place: [location.*], [table.*], [profile.*], [container.*] + world entities
data/fixtures/item_catalog_fixture.ini TEST FIXTURE for IItemCatalog (not an item database)
```

```cpp
llloot::LootConfig loot;  llsim::SimConfig sim;  std::vector<std::string> errors;
bool ok = llcontent::LoadAndValidate({"data/common.ini", "data/locations/medical_facility.ini"},
                                     catalog, loot, sim, errors);
```

Every file must start with:

```ini
[content]
version = 1
```

Any other version is rejected with `unsupported content version N (this build reads version 1)`.

## Sections

### `[loot]`
| key | meaning |
|---|---|
| `capacity_factor` | `Intact:1, Damaged:1, PartiallyDestroyed:0.5, Destroyed:0` |
| `integrity_factor` | item integrity multiplier per container condition |

### `[category.<ID>]`
`default_condition = <condition id>`, `uses_fill = true|false`.

### `[condition.<ID>]`
`integrity = min-max`, `fill = min-max` (both 0..1).

### `[container_type.<ID>]`
`category` (ContainerCategory), `capacity` (slots, 1..256), `search_ticks`,
`search_noise_db`, `force_ticks`, `force_noise_db`, `interact_range_cm`.

### `[table.<ID>]`
One `entry` line per drop:
```ini
entry = med.morphine_syrette | weight=10 | qty=1-2 | rarity=Rare | condition=STERILE | exclusive=SURGICAL | group=DRUGS | requires=KIT
```
`weight` > 0. `rarity` ∈ Common, Uncommon, Rare, Exceptional. `qty` ≤ the catalog stack limit.

### `[profile.<ID>]`
```ini
empty_chance = 0.1
pool = MED_CACHE_KITS | guaranteed | rolls=1 | group=KIT
pool = MED_DRUGS      | chance=0.8 | rolls=1-2 | requires=KIT
pool = MED_BASIC      | guaranteed | unique=false
```
Guaranteed pools are processed before optional pools. `unique` (default true)
prevents a pool picking the same entry twice.

### `[location.<ID>]`
`type`, `profile.<ContainerCategory> = <profile>`, `rarity_weights = Common:1, Uncommon:0.5, Rare:0.1, Exceptional:0`,
`chance_scale`, `integrity_shift` (-1..1), `scavenged_chance`, `ownership = Shared|PerPlayer`.

### `[container.<ID>]`
`location`, `type`, `profile` (optional override), `position = x, y, z` (cm),
`condition = Intact|Damaged|PartiallyDestroyed|Destroyed`, `ownership` (override),
`starts_empty`, and `access`, a comma-separated list of:
`power:<node>`, `tool:<item>`, `clearance:<tag>`, `discovery`, `work:<ticks>`, `seal:<world entity>`.

## Validation (diagnostics name the offending definition)

| Problem | Example message |
|---|---|
| Unsupported / missing version | `unsupported content version 2 (this build reads version 1)` |
| Unknown section or key | `line 12: unknown key 'wieght' ...`, `unknown section [lootz.X]` |
| Duplicate ids | `duplicate container id 'T_BOX'` |
| Missing item reference | `loot table 'T_BAD' entry 'mech.unobtainium': item id not found in the item catalog` |
| Invalid weights / probabilities | `weight must be a finite number > 0`, `chance must be within 0..1`, `all rarity weights are zero; nothing could ever drop` |
| Impossible capacity | `guaranteed drops of profile 'T_HEAVY' need 10 slots but the Intact container holds 6` |
| Impossible quantity | `max quantity 3 exceeds the catalog stack limit 1` |
| Too many unique rolls | `3 unique rolls but the table has only 2 entries` |
| Contradictory exclusivity | `two guaranteed pools share exclusive tag 'ONE'; only one can ever drop` |
| Dependency cycles / ordering | `requires group 'LATER' which is only produced later or by itself (circular or out-of-order dependency)` |
| Guaranteed-on-optional | `guaranteed pool depends on group 'OPT' that is not guaranteed` |
| Unresolved container | `location 'T_LOC' has no profile.Shelf and the container sets none` |
| Bad ranges | `integrity range must satisfy 0 <= min <= max <= 1` |

The integration host additionally checks that every `power:` node, every `bind`
and every `seal:` reference exists in the loaded map (`MissionWorldHost::ValidateBindings`).

## Determinism contract

* The same `(content, catalog answers, raid seed, container id, recipient key)` gives the same `LootResult`.
* Containers do not share RNG streams, and pools within a profile do not either.
* Changing one container's or pool's data does not alter other containers' results.
* `LootResult::Hash()` is a stable fingerprint for replay checks and telemetry.
