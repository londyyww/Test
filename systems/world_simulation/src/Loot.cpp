// LAST LIGHT - World Loot & Scavenging: definitions, validation, generation.
#include "llloot/Loot.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace llloot {

using llworld::IniBinder;
using llworld::IniDocument;
using llworld::IniSection;

namespace {

constexpr const char* kRarityNames[kRarityCount] = {"Common", "Uncommon", "Rare", "Exceptional"};
constexpr const char* kLocationNames[kLocationTypeCount] = {
    "MilitaryDepot", "MilitaryCheckpoint",    "Workshop",     "MedicalFacility",    "Residential",
    "Industrial",    "CommunicationsStation", "VehicleWreck", "UndergroundStorage", "HiddenCache",
};
constexpr const char* kCategoryNames[kContainerCategoryCount] = {
    "Locker", "StorageCabinet", "MilitaryCrate", "MedicalContainer", "Toolbox", "Shelf", "VehicleCompartment", "HiddenStash", "EquipmentCase",
};
constexpr const char* kConditionNames[kContainerConditionCount] = {"Intact", "Damaged", "PartiallyDestroyed", "Destroyed"};
constexpr const char* kGradeNames[5] = {"Pristine", "Intact", "Worn", "Damaged", "Degraded"};
constexpr const char* kAccessNames[kAccessKindCount] = {"Power", "Tool", "Clearance", "Discovery", "Work", "WorldSeal"};

template <std::size_t N>
bool ParseName(std::string_view s, const char* const (&names)[N], std::size_t& out) {
    for (std::size_t i = 0; i < N; ++i) {
        if (s == names[i]) {
            out = i;
            return true;
        }
    }
    return false;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    while (true) {
        const std::size_t c = s.find(sep);
        out.push_back(llworld::TrimView(c == std::string_view::npos ? s : s.substr(0, c)));
        if (c == std::string_view::npos) break;
        s.remove_prefix(c + 1);
    }
    return out;
}

bool ParseU32(std::string_view s, std::uint32_t& out) {
    double d = 0.0;
    if (!llworld::ParseNumber(s, d) || d < 0.0 || d > 4294967295.0 || std::floor(d) != d) return false;
    out = static_cast<std::uint32_t>(d);
    return true;
}

// "a-b" or "a" (non-negative values only).
bool ParseRangeU32(std::string_view s, std::uint32_t& lo, std::uint32_t& hi) {
    const std::size_t dash = s.find('-');
    if (dash == std::string_view::npos) {
        if (!ParseU32(s, lo)) return false;
        hi = lo;
        return true;
    }
    return ParseU32(s.substr(0, dash), lo) && ParseU32(s.substr(dash + 1), hi);
}

bool ParseRangeD(std::string_view s, double& lo, double& hi) {
    const std::size_t dash = s.find('-', 1);
    if (dash == std::string_view::npos) {
        if (!llworld::ParseNumber(s, lo)) return false;
        hi = lo;
        return true;
    }
    return llworld::ParseNumber(s.substr(0, dash), lo) && llworld::ParseNumber(s.substr(dash + 1), hi);
}

bool ParseFlagValue(std::string_view v, bool& out) {
    if (v == "true" || v == "1" || v == "yes") return (out = true, true);
    if (v == "false" || v == "0" || v == "no") return (out = false, true);
    return false;
}

// "Name:value, Name:value" keyed by an enum name table.
template <std::size_t N, std::size_t M>
bool ParseNamedNumbers(std::string_view v, const char* const (&names)[N], std::array<double, M>& out) {
    static_assert(N == M, "table size");
    for (std::string_view part : Split(v, ',')) {
        if (part.empty()) continue;
        const std::size_t colon = part.find(':');
        if (colon == std::string_view::npos) return false;
        std::size_t idx = 0;
        if (!ParseName(llworld::TrimView(part.substr(0, colon)), names, idx)) return false;
        if (!llworld::ParseNumber(part.substr(colon + 1), out[idx])) return false;
    }
    return true;
}

// "item | key=value | flag" -> first token + options. Returns false on a malformed option.
struct Options {
    std::string head;
    std::vector<std::pair<std::string, std::string>> kv;
};
bool ParseOptions(std::string_view v, Options& out) {
    const auto parts = Split(v, '|');
    out.head = std::string(parts[0]);
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].empty()) return false;
        const std::size_t eq = parts[i].find('=');
        if (eq == std::string_view::npos) {
            out.kv.emplace_back(std::string(parts[i]), "true");
        } else {
            out.kv.emplace_back(std::string(llworld::TrimView(parts[i].substr(0, eq))), std::string(llworld::TrimView(parts[i].substr(eq + 1))));
        }
    }
    return true;
}

bool ParseEntry(std::string_view v, TableEntryDef& e, std::string& why) {
    Options o;
    if (!ParseOptions(v, o) || o.head.empty()) return (why = "expected 'item_id | weight=N | ...'", false);
    e.itemId = o.head;
    for (const auto& [k, val] : o.kv) {
        bool ok = true;
        if (k == "weight") ok = llworld::ParseNumber(val, e.weight);
        else if (k == "rarity") ok = Parse(val, e.rarity);
        else if (k == "qty") ok = ParseRangeU32(val, e.qtyMin, e.qtyMax);
        else if (k == "condition") e.condition = val;
        else if (k == "group") e.group = val;
        else if (k == "exclusive") e.exclusive = val;
        else if (k == "requires") e.requiresGroup = val;
        else return (why = "unknown entry option '" + k + "'", false);
        if (!ok) return (why = "invalid value '" + val + "' for '" + k + "'", false);
    }
    return true;
}

bool ParsePool(std::string_view v, PoolDef& p, std::string& why) {
    Options o;
    if (!ParseOptions(v, o)) return (why = "malformed pool", false);
    // The head token is "table=ID" or just "ID".
    std::string head = o.head;
    if (head.rfind("table=", 0) == 0) head = std::string(llworld::TrimView(std::string_view(head).substr(6)));
    if (head.empty()) return (why = "pool needs a table", false);
    p.table = head;
    bool chanceGiven = false;
    for (const auto& [k, val] : o.kv) {
        bool ok = true;
        if (k == "chance") ok = (chanceGiven = true, llworld::ParseNumber(val, p.chance));
        else if (k == "rolls") ok = ParseRangeU32(val, p.rollsMin, p.rollsMax);
        else if (k == "guaranteed") ok = ParseFlagValue(val, p.guaranteed);
        else if (k == "unique") ok = ParseFlagValue(val, p.unique);
        else if (k == "group") p.group = val;
        else if (k == "exclusive") p.exclusive = val;
        else if (k == "requires") p.requiresGroup = val;
        else return (why = "unknown pool option '" + k + "'", false);
        if (!ok) return (why = "invalid value '" + val + "' for '" + k + "'", false);
    }
    if (p.guaranteed && chanceGiven && p.chance < 1.0) return (why = "a guaranteed pool cannot have chance < 1", false);
    return true;
}

bool ParseAccess(std::string_view v, std::vector<AccessRequirement>& out) {
    out.clear();
    for (std::string_view part : Split(v, ',')) {
        if (part.empty()) continue;
        const std::size_t colon = part.find(':');
        const std::string_view kind = llworld::TrimView(part.substr(0, colon));
        const std::string_view ref = colon == std::string_view::npos ? std::string_view{} : llworld::TrimView(part.substr(colon + 1));
        AccessRequirement a;
        if (kind == "power") a.kind = AccessKind::Power;
        else if (kind == "tool") a.kind = AccessKind::Tool;
        else if (kind == "clearance") a.kind = AccessKind::Clearance;
        else if (kind == "discovery") a.kind = AccessKind::Discovery;
        else if (kind == "work") a.kind = AccessKind::Work;
        else if (kind == "seal") a.kind = AccessKind::WorldSeal;
        else return false;
        if (a.kind == AccessKind::Work) {
            std::uint32_t t = 0;
            if (!ParseU32(ref, t)) return false;
            a.workTicks = t;
        } else if (a.kind != AccessKind::Discovery) {
            if (ref.empty()) return false;
            a.ref = std::string(ref);
        }
        out.push_back(a);
    }
    return true;
}

template <class T>
std::uint32_t FindById(const std::vector<T>& v, std::string_view id) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].id == id) return static_cast<std::uint32_t>(i);
    }
    return kNone;
}

template <class T>
void CheckDuplicates(const std::vector<T>& v, const char* kind, std::vector<std::string>& out) {
    // O(n log n): content can hold tens of thousands of containers.
    std::vector<std::size_t> order(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&v](std::size_t a, std::size_t b) { return v[a].id < v[b].id; });
    for (std::size_t k = 0; k < order.size(); ++k) {
        const std::string& id = v[order[k]].id;
        if (id.empty()) out.push_back(std::string(kind) + " with an empty id");
        else if (k > 0 && v[order[k - 1]].id == id && (k < 2 || v[order[k - 2]].id != id))
            out.push_back("duplicate " + std::string(kind) + " id '" + id + "'");
    }
}

bool Contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool Prob(double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; }

// Guaranteed pools first, then optional pools, each in declaration order.
std::vector<std::uint32_t> PoolOrder(const ProfileDef& p) {
    std::vector<std::uint32_t> order;
    order.reserve(p.pools.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < p.pools.size(); ++i) {
            if (p.pools[i].guaranteed == (pass == 0)) order.push_back(static_cast<std::uint32_t>(i));
        }
    }
    return order;
}

}  // namespace

// ---------------------------------------------------------------- names

const char* ToString(Rarity r) { return static_cast<std::size_t>(r) < kRarityCount ? kRarityNames[static_cast<std::size_t>(r)] : "?"; }
const char* ToString(LocationType t) {
    return static_cast<std::size_t>(t) < kLocationTypeCount ? kLocationNames[static_cast<std::size_t>(t)] : "?";
}
const char* ToString(ContainerCategory c) {
    return static_cast<std::size_t>(c) < kContainerCategoryCount ? kCategoryNames[static_cast<std::size_t>(c)] : "?";
}
const char* ToString(ContainerCondition c) {
    return static_cast<std::size_t>(c) < kContainerConditionCount ? kConditionNames[static_cast<std::size_t>(c)] : "?";
}
const char* ToString(OwnershipPolicy p) { return p == OwnershipPolicy::Shared ? "Shared" : "PerPlayer"; }
const char* ToString(ConditionGrade g) { return static_cast<std::size_t>(g) < 5 ? kGradeNames[static_cast<std::size_t>(g)] : "?"; }
const char* ToString(AccessKind k) { return static_cast<std::size_t>(k) < kAccessKindCount ? kAccessNames[static_cast<std::size_t>(k)] : "?"; }

#define LLLOOT_PARSE(Type, table)                                \
    bool Parse(std::string_view s, Type& out) {                  \
        std::size_t i = 0;                                       \
        if (!ParseName(s, table, i)) return false;               \
        out = static_cast<Type>(i);                              \
        return true;                                             \
    }
LLLOOT_PARSE(Rarity, kRarityNames)
LLLOOT_PARSE(LocationType, kLocationNames)
LLLOOT_PARSE(ContainerCategory, kCategoryNames)
LLLOOT_PARSE(ContainerCondition, kConditionNames)
#undef LLLOOT_PARSE

bool Parse(std::string_view s, OwnershipPolicy& out) {
    if (s == "Shared") return (out = OwnershipPolicy::Shared, true);
    if (s == "PerPlayer") return (out = OwnershipPolicy::PerPlayer, true);
    return false;
}

ConditionGrade GradeFor(double integrity) {
    if (integrity >= 0.9) return ConditionGrade::Pristine;
    if (integrity >= 0.7) return ConditionGrade::Intact;
    if (integrity >= 0.45) return ConditionGrade::Worn;
    if (integrity >= 0.2) return ConditionGrade::Damaged;
    return ConditionGrade::Degraded;
}

// ---------------------------------------------------------------- lookups

std::uint32_t LootConfig::FindCategory(std::string_view id) const { return FindById(categories, id); }
std::uint32_t LootConfig::FindCondition(std::string_view id) const { return FindById(conditions, id); }
std::uint32_t LootConfig::FindContainerType(std::string_view id) const { return FindById(containerTypes, id); }
std::uint32_t LootConfig::FindTable(std::string_view id) const { return FindById(tables, id); }
std::uint32_t LootConfig::FindProfile(std::string_view id) const { return FindById(profiles, id); }
std::uint32_t LootConfig::FindLocation(std::string_view id) const { return FindById(locations, id); }
std::uint32_t LootConfig::FindContainer(std::string_view id) const { return FindById(containers, id); }

std::uint32_t LootConfig::ProfileFor(const ContainerDef& c) const {
    if (!c.profile.empty()) return FindProfile(c.profile);
    const std::uint32_t loc = FindLocation(c.location);
    const std::uint32_t type = FindContainerType(c.type);
    if (loc == kNone || type == kNone) return kNone;
    const std::string& p = locations[loc].profiles[static_cast<std::size_t>(containerTypes[type].category)];
    return p.empty() ? kNone : FindProfile(p);
}

OwnershipPolicy LootConfig::OwnershipFor(const ContainerDef& c) const {
    if (c.hasOwnership) return c.ownership;
    const std::uint32_t loc = FindLocation(c.location);
    return loc == kNone ? OwnershipPolicy::Shared : locations[loc].ownership;
}

std::uint32_t LootConfig::EffectiveCapacity(const ContainerDef& c) const {
    const std::uint32_t type = FindContainerType(c.type);
    if (type == kNone) return 0;
    const double f = capacityFactor[static_cast<std::size_t>(c.condition)];
    return static_cast<std::uint32_t>(std::floor(static_cast<double>(containerTypes[type].capacity) * std::max(0.0, f)));
}

// ---------------------------------------------------------------- loading

bool IsLootSection(std::string_view n) {
    return n == "loot" || n.rfind("category.", 0) == 0 || n.rfind("condition.", 0) == 0 || n.rfind("container_type.", 0) == 0 ||
           n.rfind("table.", 0) == 0 || n.rfind("profile.", 0) == 0 || n.rfind("location.", 0) == 0 || n.rfind("container.", 0) == 0;
}

void LoadLootSections(const IniDocument& doc, LootConfig& cfg, std::vector<std::string>& errors) {
    auto suffix = [](const std::string& n) { return n.substr(n.find('.') + 1); };
    auto lineErr = [&errors](const IniSection& s, const std::string& msg) {
        errors.push_back("line " + std::to_string(s.line) + ": [" + s.name + "] " + msg);
    };
    for (const IniSection& s : doc.sections) {
        const std::string& n = s.name;
        if (n == "loot") {
            IniBinder b;
            b.Custom("capacity_factor", [&cfg](std::string_view v) { return ParseNamedNumbers(v, kConditionNames, cfg.capacityFactor); });
            b.Custom("integrity_factor", [&cfg](std::string_view v) { return ParseNamedNumbers(v, kConditionNames, cfg.integrityFactor); });
            b.Apply(s, errors);
        } else if (n.rfind("category.", 0) == 0) {
            CategoryDef c;
            c.id = suffix(n);
            IniBinder b;
            b.Str("default_condition", c.defaultCondition);
            b.Flag("uses_fill", c.usesFill);
            b.Apply(s, errors);
            cfg.categories.push_back(c);
        } else if (n.rfind("condition.", 0) == 0) {
            ConditionRangeDef c;
            c.id = suffix(n);
            IniBinder b;
            b.Custom("integrity", [&c](std::string_view v) { return ParseRangeD(v, c.integrityMin, c.integrityMax); });
            b.Custom("fill", [&c](std::string_view v) { return ParseRangeD(v, c.fillMin, c.fillMax); });
            b.Apply(s, errors);
            cfg.conditions.push_back(c);
        } else if (n.rfind("container_type.", 0) == 0) {
            ContainerTypeDef t;
            t.id = suffix(n);
            IniBinder b;
            b.Custom("category", [&t](std::string_view v) { return Parse(v, t.category); });
            b.U32("capacity", t.capacity);
            b.Ticks("search_ticks", t.searchTicks);
            b.Num("search_noise_db", t.searchNoiseDb);
            b.Ticks("force_ticks", t.forceTicks);
            b.Num("force_noise_db", t.forceNoiseDb);
            b.Num("interact_range_cm", t.interactRangeCm);
            b.Apply(s, errors);
            cfg.containerTypes.push_back(t);
        } else if (n.rfind("table.", 0) == 0) {
            TableDef t;
            t.id = suffix(n);
            for (const auto& e : s.entries) {
                if (e.key != "entry") {
                    errors.push_back("line " + std::to_string(e.line) + ": unknown key '" + e.key + "' in [" + n + "]");
                    continue;
                }
                TableEntryDef d;
                std::string why;
                if (ParseEntry(e.value, d, why)) {
                    t.entries.push_back(d);
                } else {
                    errors.push_back("line " + std::to_string(e.line) + ": [" + n + "] " + why);
                }
            }
            cfg.tables.push_back(t);
        } else if (n.rfind("profile.", 0) == 0) {
            ProfileDef p;
            p.id = suffix(n);
            for (const auto& e : s.entries) {
                std::string why;
                if (e.key == "empty_chance") {
                    if (!llworld::ParseNumber(e.value, p.emptyChance)) errors.push_back("line " + std::to_string(e.line) + ": invalid empty_chance");
                } else if (e.key == "pool") {
                    PoolDef pool;
                    if (ParsePool(e.value, pool, why)) {
                        p.pools.push_back(pool);
                    } else {
                        errors.push_back("line " + std::to_string(e.line) + ": [" + n + "] " + why);
                    }
                } else {
                    errors.push_back("line " + std::to_string(e.line) + ": unknown key '" + e.key + "' in [" + n + "]");
                }
            }
            cfg.profiles.push_back(p);
        } else if (n.rfind("location.", 0) == 0) {
            LocationDef l;
            l.id = suffix(n);
            IniBinder b;
            b.Custom("type", [&l](std::string_view v) { return Parse(v, l.type); });
            for (std::size_t c = 0; c < kContainerCategoryCount; ++c) b.Str((std::string("profile.") + kCategoryNames[c]).c_str(), l.profiles[c]);
            b.Custom("rarity_weights", [&l](std::string_view v) { return ParseNamedNumbers(v, kRarityNames, l.rarityWeights); });
            b.Num("chance_scale", l.chanceScale);
            b.Num("integrity_shift", l.integrityShift);
            b.Num("scavenged_chance", l.scavengedChance);
            b.Custom("ownership", [&l](std::string_view v) { return Parse(v, l.ownership); });
            b.Apply(s, errors);
            cfg.locations.push_back(l);
        } else if (n.rfind("container.", 0) == 0) {
            ContainerDef c;
            c.id = suffix(n);
            IniBinder b;
            b.Str("location", c.location);
            b.Str("type", c.type);
            b.Str("profile", c.profile);
            b.Point("position", c.position);
            b.Custom("condition", [&c](std::string_view v) { return Parse(v, c.condition); });
            b.Custom("access", [&c](std::string_view v) { return ParseAccess(v, c.access); });
            b.Custom("ownership", [&c](std::string_view v) { return (c.hasOwnership = true, Parse(v, c.ownership)); });
            b.Flag("starts_empty", c.startsEmpty);
            b.Apply(s, errors);
            if (c.location.empty()) lineErr(s, "needs a location");
            if (c.type.empty()) lineErr(s, "needs a type");
            cfg.containers.push_back(c);
        }
    }
}

// ---------------------------------------------------------------- validation

std::vector<std::string> ValidateLoot(const LootConfig& cfg, const IItemCatalog& catalog) {
    std::vector<std::string> out;
    if (cfg.version != kLootConfigVersion) {
        out.push_back("unsupported content version " + std::to_string(cfg.version) + " (this build reads version " +
                      std::to_string(kLootConfigVersion) + ")");
    }
    CheckDuplicates(cfg.categories, "category", out);
    CheckDuplicates(cfg.conditions, "condition range", out);
    CheckDuplicates(cfg.containerTypes, "container type", out);
    CheckDuplicates(cfg.tables, "loot table", out);
    CheckDuplicates(cfg.profiles, "loot profile", out);
    CheckDuplicates(cfg.locations, "location", out);
    CheckDuplicates(cfg.containers, "container", out);

    for (std::size_t i = 0; i < kContainerConditionCount; ++i) {
        if (!std::isfinite(cfg.capacityFactor[i]) || cfg.capacityFactor[i] < 0.0 || cfg.capacityFactor[i] > 1.0)
            out.push_back(std::string("[loot] capacity_factor for ") + kConditionNames[i] + " must be within 0..1");
        if (!Prob(cfg.integrityFactor[i])) out.push_back(std::string("[loot] integrity_factor for ") + kConditionNames[i] + " must be within 0..1");
    }

    for (const ConditionRangeDef& c : cfg.conditions) {
        if (!Prob(c.integrityMin) || !Prob(c.integrityMax) || c.integrityMin > c.integrityMax)
            out.push_back("condition '" + c.id + "': integrity range must satisfy 0 <= min <= max <= 1");
        if (!Prob(c.fillMin) || !Prob(c.fillMax) || c.fillMin > c.fillMax)
            out.push_back("condition '" + c.id + "': fill range must satisfy 0 <= min <= max <= 1");
    }
    for (const CategoryDef& c : cfg.categories) {
        if (c.defaultCondition.empty()) out.push_back("category '" + c.id + "' needs default_condition");
        else if (cfg.FindCondition(c.defaultCondition) == kNone)
            out.push_back("category '" + c.id + "' references unknown condition range '" + c.defaultCondition + "'");
    }
    for (const ContainerTypeDef& t : cfg.containerTypes) {
        if (t.capacity == 0) out.push_back("container type '" + t.id + "': capacity must be at least 1");
        if (t.capacity > 256) out.push_back("container type '" + t.id + "': capacity " + std::to_string(t.capacity) + " exceeds the 256-slot limit");
        if (t.searchTicks == 0) out.push_back("container type '" + t.id + "': search_ticks must be at least 1");
        if (t.searchTicks > kMaxConfigTicks || t.forceTicks > kMaxConfigTicks) out.push_back("container type '" + t.id + "': search_ticks / force_ticks exceed the 2^32 tick limit");
        if (!std::isfinite(t.interactRangeCm) || t.interactRangeCm <= 0.0) out.push_back("container type '" + t.id + "': interact_range_cm must be > 0");
        if (!std::isfinite(t.searchNoiseDb) || !std::isfinite(t.forceNoiseDb)) out.push_back("container type '" + t.id + "': noise must be finite");
    }

    // Tables: item references resolved against the authoritative catalog.
    std::vector<std::uint32_t> minSlot(cfg.tables.size(), 0);
    for (std::size_t ti = 0; ti < cfg.tables.size(); ++ti) {
        const TableDef& t = cfg.tables[ti];
        if (t.entries.empty()) out.push_back("loot table '" + t.id + "' has no entries");
        std::uint32_t smallest = 0xFFFFFFFFu;
        for (const TableEntryDef& e : t.entries) {
            const std::string where = "loot table '" + t.id + "' entry '" + e.itemId + "'";
            if (!std::isfinite(e.weight) || e.weight <= 0.0 || e.weight > kMaxWeight) out.push_back(where + ": weight must be a finite number > 0 and <= 1e6");
            if (e.qtyMin == 0 || e.qtyMin > e.qtyMax) out.push_back(where + ": quantity range must satisfy 1 <= min <= max");
            if (!e.condition.empty() && cfg.FindCondition(e.condition) == kNone)
                out.push_back(where + ": unknown condition range '" + e.condition + "'");
            ItemDescriptor d;
            if (!catalog.Describe(e.itemId, d)) {
                out.push_back(where + ": item id not found in the item catalog");
                continue;
            }
            if (cfg.FindCategory(d.categoryId) == kNone)
                out.push_back(where + ": catalog category '" + d.categoryId + "' has no [category." + d.categoryId + "]");
            if (d.maxStack == 0 || d.slotSize == 0) out.push_back(where + ": catalog reports a zero stack or slot size");
            if (e.qtyMax > d.maxStack)
                out.push_back(where + ": max quantity " + std::to_string(e.qtyMax) + " exceeds the catalog stack limit " + std::to_string(d.maxStack));
            smallest = std::min(smallest, std::max<std::uint32_t>(1, d.slotSize));
        }
        minSlot[ti] = smallest == 0xFFFFFFFFu ? 1 : smallest;
    }

    // Profiles: probabilities, dependency order, exclusivity contradictions.
    std::vector<std::uint32_t> guaranteedFootprint(cfg.profiles.size(), 0);
    for (std::size_t pi = 0; pi < cfg.profiles.size(); ++pi) {
        const ProfileDef& p = cfg.profiles[pi];
        const std::string where = "loot profile '" + p.id + "'";
        if (!Prob(p.emptyChance)) out.push_back(where + ": empty_chance must be within 0..1");
        if (p.pools.empty()) out.push_back(where + " has no pools");
        std::vector<std::string> produced;          // groups available to later pools
        std::vector<std::string> guaranteedGroups;  // groups certainly produced
        std::vector<std::string> guaranteedExclusive;
        std::vector<std::string> allGroups;
        for (const PoolDef& pool : p.pools) {
            if (!pool.group.empty()) allGroups.push_back(pool.group);
            const std::uint32_t t = cfg.FindTable(pool.table);
            if (t != kNone) {
                for (const auto& e : cfg.tables[t].entries) {
                    if (!e.group.empty()) allGroups.push_back(e.group);
                }
            }
        }
        for (std::uint32_t idx : PoolOrder(p)) {
            const PoolDef& pool = p.pools[idx];
            const std::string pw = where + " pool " + std::to_string(idx + 1) + " (table '" + pool.table + "')";
            const std::uint32_t t = cfg.FindTable(pool.table);
            if (t == kNone) {
                out.push_back(pw + ": unknown loot table");
                continue;
            }
            const TableDef& table = cfg.tables[t];
            if (!Prob(pool.chance)) out.push_back(pw + ": chance must be within 0..1");
            if (pool.rollsMin > pool.rollsMax) out.push_back(pw + ": rolls min > max");
            if (pool.guaranteed && pool.rollsMin == 0) out.push_back(pw + ": a guaranteed pool needs at least one roll");
            if (pool.unique && pool.rollsMax > table.entries.size())
                out.push_back(pw + ": " + std::to_string(pool.rollsMax) + " unique rolls but the table has only " + std::to_string(table.entries.size()) +
                              " entries");
            if (!pool.requiresGroup.empty()) {
                if (!Contains(allGroups, pool.requiresGroup)) {
                    out.push_back(pw + ": requires unknown group '" + pool.requiresGroup + "'");
                } else if (!Contains(produced, pool.requiresGroup)) {
                    out.push_back(pw + ": requires group '" + pool.requiresGroup +
                                  "' which is only produced later or by itself (circular or out-of-order dependency)");
                }
                if (pool.guaranteed && !Contains(guaranteedGroups, pool.requiresGroup))
                    out.push_back(pw + ": guaranteed pool depends on group '" + pool.requiresGroup + "' that is not guaranteed");
            }
            if (!pool.exclusive.empty() && pool.guaranteed) {
                if (Contains(guaranteedExclusive, pool.exclusive))
                    out.push_back(pw + ": two guaranteed pools share exclusive tag '" + pool.exclusive + "'; only one can ever drop");
                guaranteedExclusive.push_back(pool.exclusive);
            }
            for (const TableEntryDef& e : table.entries) {
                if (!e.requiresGroup.empty() && !Contains(produced, e.requiresGroup))
                    out.push_back(pw + " entry '" + e.itemId + "': requires group '" + e.requiresGroup + "' that no earlier pool produces");
            }
            if (pool.guaranteed) {
                guaranteedFootprint[pi] += pool.rollsMin * minSlot[t];
                if (!pool.group.empty()) guaranteedGroups.push_back(pool.group);
            }
            if (!pool.group.empty()) produced.push_back(pool.group);
            for (const TableEntryDef& e : table.entries) {
                if (!e.group.empty()) produced.push_back(e.group);
            }
        }
    }

    for (const LocationDef& l : cfg.locations) {
        const std::string where = "location '" + l.id + "'";
        double sum = 0.0;
        for (double w : l.rarityWeights) {
            if (!std::isfinite(w) || w < 0.0 || w > kMaxWeight) out.push_back(where + ": rarity weights must be within 0..1e6");
            sum += w;
        }
        if (!(sum > 0.0)) out.push_back(where + ": all rarity weights are zero; nothing could ever drop");
        if (!std::isfinite(l.chanceScale) || l.chanceScale < 0.0) out.push_back(where + ": chance_scale must be >= 0");
        if (!Prob(l.scavengedChance)) out.push_back(where + ": scavenged_chance must be within 0..1");
        if (!std::isfinite(l.integrityShift) || l.integrityShift < -1.0 || l.integrityShift > 1.0)
            out.push_back(where + ": integrity_shift must be within -1..1");
        for (std::size_t c = 0; c < kContainerCategoryCount; ++c) {
            if (!l.profiles[c].empty() && cfg.FindProfile(l.profiles[c]) == kNone)
                out.push_back(where + ": profile." + kCategoryNames[c] + " references unknown profile '" + l.profiles[c] + "'");
        }
    }

    for (const ContainerDef& c : cfg.containers) {
        const std::string where = "container '" + c.id + "'";
        const std::uint32_t loc = cfg.FindLocation(c.location);
        const std::uint32_t type = cfg.FindContainerType(c.type);
        if (loc == kNone) out.push_back(where + ": unknown location '" + c.location + "'");
        if (type == kNone) out.push_back(where + ": unknown container type '" + c.type + "'");
        if (!c.position.IsFinite()) out.push_back(where + ": position must be finite");
        if (!c.profile.empty() && cfg.FindProfile(c.profile) == kNone) out.push_back(where + ": unknown profile '" + c.profile + "'");
        for (const AccessRequirement& a : c.access) {
            if (a.kind == AccessKind::Work && (a.workTicks == 0 || a.workTicks > kMaxConfigTicks)) out.push_back(where + ": work access needs 1..2^32 ticks");
        }
        if (loc == kNone || type == kNone) continue;
        const std::uint32_t prof = cfg.ProfileFor(c);
        if (prof == kNone) {
            if (c.profile.empty())
                out.push_back(where + ": location '" + c.location + "' has no profile." + ToString(cfg.containerTypes[type].category) +
                              " and the container sets none");
            continue;
        }
        if (c.condition == ContainerCondition::Destroyed || c.startsEmpty) continue;
        const std::uint32_t cap = cfg.EffectiveCapacity(c);
        if (guaranteedFootprint[prof] > cap)
            out.push_back(where + ": guaranteed drops of profile '" + cfg.profiles[prof].id + "' need " + std::to_string(guaranteedFootprint[prof]) +
                          " slots but the " + ToString(c.condition) + " container holds " + std::to_string(cap));
    }
    return out;
}

// ---------------------------------------------------------------- generation

std::uint64_t ContainerSeed(std::uint64_t raidSeed, std::string_view containerId, std::uint64_t recipientKey) {
    return llworld::HashCombine(llworld::HashCombine(raidSeed, StableHash(containerId)), recipientKey);
}

std::uint64_t LootResult::Hash() const {
    llworld::StateHasher h;
    h.Add(StableHash(containerId));
    h.Add(seed);
    h.Add(usedSlots);
    h.Add(static_cast<std::uint64_t>(empty));
    for (const LootItem& i : items) {
        h.Add(StableHash(i.itemId));
        h.Add(i.quantity);
        h.AddDouble(i.condition.integrity);
        h.AddDouble(i.condition.fill);
        h.Add(static_cast<std::uint64_t>(i.rarity));
    }
    return h.Value();
}

LootResult GenerateLoot(const LootConfig& cfg, const IItemCatalog& catalog, const ContainerDef& c, std::uint64_t seed) {
    LootResult r;
    r.containerId = c.id;
    r.seed = seed;
    r.capacity = cfg.EffectiveCapacity(c);
    const std::uint32_t loc = cfg.FindLocation(c.location);
    const std::uint32_t prof = cfg.ProfileFor(c);
    if (c.condition == ContainerCondition::Destroyed) return (r.empty = EmptyReason::Destroyed, r);
    if (c.startsEmpty) return (r.empty = EmptyReason::AuthoredEmpty, r);
    if (loc == kNone || prof == kNone) return (r.empty = EmptyReason::NothingRolled, r);
    const LocationDef& location = cfg.locations[loc];
    const ProfileDef& profile = cfg.profiles[prof];

    DetRng top(seed);
    // Hidden containers were never found by earlier scavengers.
    const bool hidden = std::any_of(c.access.begin(), c.access.end(), [](const AccessRequirement& a) { return a.kind == AccessKind::Discovery; });
    if (top.Unit() < location.scavengedChance && !hidden) return (r.empty = EmptyReason::Scavenged, r);
    if (top.Unit() < profile.emptyChance) return (r.empty = EmptyReason::ProfileEmpty, r);

    const double integrityFactor = cfg.integrityFactor[static_cast<std::size_t>(c.condition)];
    std::vector<std::string> produced;
    std::vector<std::string> exclusiveUsed;
    std::vector<double> weights;
    std::vector<char> picked;
    for (std::uint32_t idx : PoolOrder(profile)) {
        const PoolDef& pool = profile.pools[idx];
        const std::uint32_t t = cfg.FindTable(pool.table);
        if (t == kNone) continue;
        // Independent stream per pool: editing one pool never reshuffles another.
        DetRng rng(llworld::HashCombine(seed, 0x9001ull + idx));
        if (!pool.requiresGroup.empty() && !Contains(produced, pool.requiresGroup)) continue;
        if (!pool.exclusive.empty() && Contains(exclusiveUsed, pool.exclusive)) continue;
        const double chance = pool.guaranteed ? 1.0 : llworld::Clamp01(pool.chance * location.chanceScale);
        if (!pool.guaranteed && rng.Unit() >= chance) continue;
        const TableDef& table = cfg.tables[t];
        const std::uint32_t rolls = rng.Range(pool.rollsMin, pool.rollsMax);
        picked.assign(table.entries.size(), 0);
        bool any = false;
        for (std::uint32_t roll = 0; roll < rolls; ++roll) {
            weights.assign(table.entries.size(), 0.0);
            double total = 0.0;
            for (std::size_t e = 0; e < table.entries.size(); ++e) {
                const TableEntryDef& d = table.entries[e];
                if (pool.unique && picked[e]) continue;
                if (!d.exclusive.empty() && Contains(exclusiveUsed, d.exclusive)) continue;
                if (!d.requiresGroup.empty() && !Contains(produced, d.requiresGroup)) continue;
                weights[e] = d.weight * location.rarityWeights[static_cast<std::size_t>(d.rarity)];
                total += weights[e];
            }
            if (!(total > 0.0)) break;
            double x = rng.Unit() * total;
            std::size_t pick = table.entries.size();
            for (std::size_t e = 0; e < table.entries.size(); ++e) {
                if (weights[e] <= 0.0) continue;
                pick = e;  // last eligible absorbs rounding
                if (x < weights[e]) break;
                x -= weights[e];
            }
            if (pick == table.entries.size()) break;
            picked[pick] = 1;
            const TableEntryDef& d = table.entries[pick];
            ItemDescriptor desc;
            if (!catalog.Describe(d.itemId, desc)) continue;
            const std::uint32_t cat = cfg.FindCategory(desc.categoryId);
            const std::string& condId = !d.condition.empty() ? d.condition : (cat != kNone ? cfg.categories[cat].defaultCondition : d.condition);
            const std::uint32_t cond = cfg.FindCondition(condId);
            LootItem item;
            item.itemId = d.itemId;
            item.categoryId = desc.categoryId;
            item.rarity = d.rarity;
            item.pool = idx;
            item.slotSize = std::max<std::uint32_t>(1, desc.slotSize);
            item.quantity = std::min(rng.Range(d.qtyMin, d.qtyMax), std::max<std::uint32_t>(1, desc.maxStack));
            double integrity = 1.0;
            double fill = 1.0;
            if (cond != kNone) {
                const ConditionRangeDef& range = cfg.conditions[cond];
                integrity = rng.Between(range.integrityMin, range.integrityMax);
                fill = rng.Between(range.fillMin, range.fillMax);
            }
            item.condition.integrity = std::max(0.01, llworld::Clamp01(integrity * integrityFactor + location.integrityShift));
            item.condition.fill = (cat != kNone && cfg.categories[cat].usesFill) ? std::max(0.01, llworld::Clamp01(fill)) : 1.0;
            item.condition.grade = GradeFor(item.condition.integrity);
            if (r.usedSlots + item.slotSize > r.capacity) {
                ++r.capacityRejected;
                continue;
            }
            r.usedSlots += item.slotSize;
            if (!d.group.empty()) produced.push_back(d.group);
            if (!d.exclusive.empty()) exclusiveUsed.push_back(d.exclusive);
            r.items.push_back(std::move(item));
            any = true;
        }
        if (any) {
            if (!pool.group.empty()) produced.push_back(pool.group);
            if (!pool.exclusive.empty()) exclusiveUsed.push_back(pool.exclusive);
        }
    }
    if (r.items.empty()) r.empty = EmptyReason::NothingRolled;
    return r;
}

}  // namespace llloot
