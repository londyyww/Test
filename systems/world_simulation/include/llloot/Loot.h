// LAST LIGHT - World Loot & Scavenging (portable C++17 core, no Unreal headers).
//
// Data model, configuration validation and deterministic loot generation.
// This module GENERATES and EXPOSES loot results only. It owns no inventory,
// stash, profile or item database: item identity, stacking and footprint come
// from the authoritative catalog through IItemCatalog, and items move into a
// player's possession only through ILootGrantPort (see ContainerService.h).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "llworld/Ini.h"
#include "llworld/WorldTypes.h"

namespace llloot {

using llworld::PlayerId;
using llworld::Tick;
using llworld::Vec3;

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;
inline constexpr std::uint32_t kLootConfigVersion = 1;
inline constexpr Tick kMaxConfigTicks = 1ull << 32;  // same bound as the world simulation
inline constexpr double kMaxWeight = 1e6;            // keeps weight totals finite and precise

// Stable across platforms and compilers (FNV-1a 64); used for seeds and grant ids.
inline constexpr std::uint64_t StableHash(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

// SplitMix64 stream. Standard <random> distributions are implementation-defined,
// so they would break cross-platform reproduction of a raid seed.
class DetRng {
public:
    explicit DetRng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t Next() {
        s_ += 0x9E3779B97F4A7C15ull;
        return llworld::Mix64(s_);
    }
    double Unit() { return llworld::HashToUnit(Next()); }  // [0, 1)
    std::uint32_t Range(std::uint32_t lo, std::uint32_t hi) {  // inclusive
        if (hi <= lo) return lo;
        const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1u;
        return lo + static_cast<std::uint32_t>(Next() % span);
    }
    double Between(double lo, double hi) { return hi <= lo ? lo : lo + (hi - lo) * Unit(); }

private:
    std::uint64_t s_;
};

// ---------------------------------------------------------------- enums

enum class Rarity : std::uint8_t { Common = 0, Uncommon, Rare, Exceptional };
inline constexpr std::size_t kRarityCount = 4;

enum class LocationType : std::uint8_t {
    MilitaryDepot = 0,
    MilitaryCheckpoint,
    Workshop,
    MedicalFacility,
    Residential,
    Industrial,
    CommunicationsStation,
    VehicleWreck,
    UndergroundStorage,
    HiddenCache,
};
inline constexpr std::size_t kLocationTypeCount = 10;

enum class ContainerCategory : std::uint8_t {
    Locker = 0,
    StorageCabinet,
    MilitaryCrate,
    MedicalContainer,
    Toolbox,
    Shelf,
    VehicleCompartment,
    HiddenStash,
    EquipmentCase,
};
inline constexpr std::size_t kContainerCategoryCount = 9;

// Physical state of the container itself (not of the items inside).
enum class ContainerCondition : std::uint8_t { Intact = 0, Damaged, PartiallyDestroyed, Destroyed };
inline constexpr std::size_t kContainerConditionCount = 4;

enum class OwnershipPolicy : std::uint8_t {
    Shared,     // one set of contents; first come, first served per item
    PerPlayer,  // every player gets an independently generated instance
};

// Structured condition grade derived from integrity; gameplay code reads the
// numbers, the grade is a convenience for UI and rules.
enum class ConditionGrade : std::uint8_t { Pristine = 0, Intact, Worn, Damaged, Degraded };

enum class AccessKind : std::uint8_t {
    Power,      // ref = infrastructure node that must be powered (electronic lock)
    Tool,       // ref = item id the searcher must carry (damaged lock, pry bar)
    Clearance,  // ref = clearance tag (restricted military access)
    Discovery,  // hidden: must be discovered before it can be searched
    Work,       // partially destroyed: debris must be cleared (workTicks)
    WorldSeal,  // ref = world-simulation entity; sealed while it is in lockdown
};
inline constexpr std::size_t kAccessKindCount = 6;

const char* ToString(Rarity r);
const char* ToString(LocationType t);
const char* ToString(ContainerCategory c);
const char* ToString(ContainerCondition c);
const char* ToString(OwnershipPolicy p);
const char* ToString(ConditionGrade g);
const char* ToString(AccessKind k);
bool Parse(std::string_view s, Rarity& out);
bool Parse(std::string_view s, LocationType& out);
bool Parse(std::string_view s, ContainerCategory& out);
bool Parse(std::string_view s, ContainerCondition& out);
bool Parse(std::string_view s, OwnershipPolicy& out);

// ---------------------------------------------------------------- catalog adapter

// What the loot system needs to know about an item. Supplied by the
// authoritative item catalog; never defined by loot data.
struct ItemDescriptor {
    std::string categoryId;      // must match a [category.*] in loot data
    std::uint32_t maxStack = 1;  // quantity cap per generated stack
    std::uint32_t slotSize = 1;  // container capacity units per stack
};

class IItemCatalog {
public:
    virtual ~IItemCatalog() = default;
    virtual bool Describe(std::string_view itemId, ItemDescriptor& out) const = 0;
};

// ---------------------------------------------------------------- definitions

struct ConditionRangeDef {
    std::string id;
    double integrityMin = 1.0;
    double integrityMax = 1.0;
    double fillMin = 1.0;  // fraction of contents remaining (supplies, batteries)
    double fillMax = 1.0;
};

struct CategoryDef {
    std::string id;
    std::string defaultCondition;  // ConditionRangeDef id
    bool usesFill = false;         // partially depleted supplies are meaningful
};

struct ContainerTypeDef {
    std::string id;
    ContainerCategory category = ContainerCategory::Shelf;
    std::uint32_t capacity = 8;  // slot units
    Tick searchTicks = 60;
    double searchNoiseDb = 45.0;  // rummaging, at 1 m
    Tick forceTicks = 90;         // extra time when a Tool requirement is used
    double forceNoiseDb = 78.0;   // prying a lock
    double interactRangeCm = 200.0;
};

struct TableEntryDef {
    std::string itemId;
    double weight = 1.0;
    Rarity rarity = Rarity::Common;
    std::uint32_t qtyMin = 1;
    std::uint32_t qtyMax = 1;
    std::string condition;  // empty = category default
    std::string group;      // optional: marks a dependency group as produced
    std::string exclusive;  // optional: at most one drop with this tag per container
    std::string requiresGroup;  // optional: group that must already be produced (Unreal builds as C++20: avoid `requires`)
};

struct TableDef {
    std::string id;
    std::vector<TableEntryDef> entries;
};

struct PoolDef {
    std::string table;
    double chance = 1.0;
    std::uint32_t rollsMin = 1;
    std::uint32_t rollsMax = 1;
    bool guaranteed = false;  // always rolls, claims capacity first
    bool unique = true;       // the same entry cannot be picked twice by this pool
    std::string group;
    std::string exclusive;
    std::string requiresGroup;
};

struct ProfileDef {
    std::string id;
    double emptyChance = 0.0;  // container already stripped (story: "someone was here first")
    std::vector<PoolDef> pools;
};

struct LocationDef {
    std::string id;
    LocationType type = LocationType::Residential;
    std::array<std::string, kContainerCategoryCount> profiles{};  // default profile per container category
    std::array<double, kRarityCount> rarityWeights{{1.0, 1.0, 1.0, 1.0}};
    double chanceScale = 1.0;      // scales optional pool chances
    double integrityShift = 0.0;   // e.g. damp basement -0.1
    double scavengedChance = 0.0;  // extra chance a container is already empty
    OwnershipPolicy ownership = OwnershipPolicy::Shared;
};

struct AccessRequirement {
    AccessKind kind = AccessKind::Power;
    std::string ref;
    Tick workTicks = 0;
};

struct ContainerDef {
    std::string id;
    std::string location;
    std::string type;
    std::string profile;  // empty = location default for the type's category
    Vec3 position;
    ContainerCondition condition = ContainerCondition::Intact;
    std::vector<AccessRequirement> access;
    bool hasOwnership = false;
    OwnershipPolicy ownership = OwnershipPolicy::Shared;
    bool startsEmpty = false;  // authored story beat: already looted
};

struct LootConfig {
    std::uint32_t version = 0;  // 0 = no [content] version seen
    std::vector<CategoryDef> categories;
    std::vector<ConditionRangeDef> conditions;
    std::vector<ContainerTypeDef> containerTypes;
    std::vector<TableDef> tables;
    std::vector<ProfileDef> profiles;
    std::vector<LocationDef> locations;
    std::vector<ContainerDef> containers;
    // Capacity reduction per container condition (Destroyed holds nothing).
    std::array<double, kContainerConditionCount> capacityFactor{{1.0, 1.0, 0.5, 0.0}};
    std::array<double, kContainerConditionCount> integrityFactor{{1.0, 0.85, 0.6, 0.0}};

    std::uint32_t FindCategory(std::string_view id) const;
    std::uint32_t FindCondition(std::string_view id) const;
    std::uint32_t FindContainerType(std::string_view id) const;
    std::uint32_t FindTable(std::string_view id) const;
    std::uint32_t FindProfile(std::string_view id) const;
    std::uint32_t FindLocation(std::string_view id) const;
    std::uint32_t FindContainer(std::string_view id) const;
    // Profile a container uses after applying its location default. kNone if unresolved.
    std::uint32_t ProfileFor(const ContainerDef& c) const;
    OwnershipPolicy OwnershipFor(const ContainerDef& c) const;
    std::uint32_t EffectiveCapacity(const ContainerDef& c) const;
};

// Consumes [loot], [category.*], [condition.*], [container_type.*], [table.*],
// [profile.*], [location.*] and [container.*]. Other sections are ignored here;
// the content loader rejects sections nobody claims.
bool IsLootSection(std::string_view name);
void LoadLootSections(const llworld::IniDocument& doc, LootConfig& cfg, std::vector<std::string>& errors);

// Semantic validation. Every message names the offending definition.
std::vector<std::string> ValidateLoot(const LootConfig& cfg, const IItemCatalog& catalog);

// ---------------------------------------------------------------- results

struct ItemCondition {
    double integrity = 1.0;  // 0..1, mapped by the item contract to its own representation
    double fill = 1.0;       // 0..1 remaining contents; 1 for categories without fill
    ConditionGrade grade = ConditionGrade::Pristine;
};
ConditionGrade GradeFor(double integrity);

struct LootItem {
    std::string itemId;
    std::string categoryId;
    std::uint32_t quantity = 1;
    ItemCondition condition;
    Rarity rarity = Rarity::Common;
    std::uint32_t slotSize = 1;
    std::uint32_t pool = kNone;  // profile pool that produced it
};

enum class EmptyReason : std::uint8_t { None = 0, ProfileEmpty, Scavenged, AuthoredEmpty, Destroyed, NothingRolled };

struct LootResult {
    std::string containerId;
    std::uint64_t seed = 0;
    std::uint32_t capacity = 0;
    std::uint32_t usedSlots = 0;
    std::uint32_t capacityRejected = 0;  // picks that did not fit
    EmptyReason empty = EmptyReason::None;
    std::vector<LootItem> items;
    std::uint64_t Hash() const;
};

// Seed for one container instance. recipientKey is 0 for shared containers and
// a stable per-player key for PerPlayer instances.
std::uint64_t ContainerSeed(std::uint64_t raidSeed, std::string_view containerId, std::uint64_t recipientKey);

// Pure function of (config, catalog, container, seed): same inputs, same output.
LootResult GenerateLoot(const LootConfig& cfg, const IItemCatalog& catalog, const ContainerDef& container, std::uint64_t seed);

}  // namespace llloot
