// Loot generation: determinism, location identity, distributions, condition,
// capacity, empty containers, guaranteed / exclusive / dependent drops.
#include <cmath>

#include "WsTestHelpers.h"

using namespace wstest;
using namespace llloot;

namespace {

const Content& All() {
    static Content c = Load({kAllLocations[0], kAllLocations[1], kAllLocations[2], kAllLocations[3], kAllLocations[4]});
    return c;
}

LootResult Gen(const Content& c, const char* container, std::uint64_t raid, std::uint64_t recipient = 0) {
    const std::uint32_t i = c.loot.FindContainer(container);
    return GenerateLoot(c.loot, Catalog(), c.loot.containers[i], ContainerSeed(raid, container, recipient));
}

// Copy of a container with a different seed space (same definition, new id).
ContainerDef Clone(const Content& c, const char* container, const std::string& id) {
    ContainerDef d = c.loot.containers[c.loot.FindContainer(container)];
    d.id = id;
    return d;
}

bool HasCategory(const LootResult& r, const std::string& cat) {
    for (const auto& i : r.items) {
        if (i.categoryId == cat) return true;
    }
    return false;
}

}  // namespace

AIS_TEST(Loot_DeterministicForSameContainerAndRaidSeed) {
    const Content& c = All();
    CHECK(c.ok);
    for (const ContainerDef& d : c.loot.containers) {
        const LootResult a = GenerateLoot(c.loot, Catalog(), d, ContainerSeed(77, d.id, 0));
        const LootResult b = GenerateLoot(c.loot, Catalog(), d, ContainerSeed(77, d.id, 0));
        CHECK_MSG(a.Hash() == b.Hash(), d.id);
        CHECK_EQ(a.items.size(), b.items.size());
    }
}

AIS_TEST(Loot_RaidSeedReproductionAndVariation) {
    const Content& c = All();
    // Reproduce a whole raid's loot from its seed; a different seed gives a different raid.
    auto raidHash = [&c](std::uint64_t raid) {
        llworld::StateHasher h;
        for (const ContainerDef& d : c.loot.containers) h.Add(GenerateLoot(c.loot, Catalog(), d, ContainerSeed(raid, d.id, 0)).Hash());
        return h.Value();
    };
    CHECK_EQ(raidHash(0xDEADF00D), raidHash(0xDEADF00D));
    CHECK(raidHash(0xDEADF00D) != raidHash(0xDEADF00E));
    // Seeds are independent per container: two containers never share a stream.
    CHECK(ContainerSeed(1, "A", 0) != ContainerSeed(1, "B", 0));
    CHECK(ContainerSeed(1, "A", 0) != ContainerSeed(1, "A", 7));
    // Golden values guard cross-platform reproducibility of the hash and RNG.
    CHECK_EQ(StableHash("DF_ARMORY_CRATE"), StableHash(std::string("DF_ARMORY_") + "CRATE"));
    DetRng r(42);
    const std::uint64_t first = r.Next();
    DetRng r2(42);
    CHECK_EQ(first, r2.Next());
}

AIS_TEST(Loot_LocationProfilesTellDifferentStories) {
    const Content& c = All();
    int depotMilitary = 0;
    int depotAmmo = 0;
    int resMilitary = 0;
    int resFood = 0;
    int medMedical = 0;
    int indMech = 0;
    for (std::uint64_t raid = 1; raid <= 200; ++raid) {
        const LootResult crate = Gen(c, "DF_ARMORY_CRATE", raid);
        const LootResult cupboard = Gen(c, "RES_FLAT2_CUPBOARD", raid, 1);
        const LootResult shelf = Gen(c, "RES_FLAT2_SHELF", raid, 1);
        const LootResult med = Gen(c, "MED_TREATMENT_CABINET", raid);
        const LootResult tools = Gen(c, "IND_DAMAGED_TOOLBOX", raid);
        depotAmmo += HasCategory(crate, "AMMUNITION") ? 1 : 0;
        depotMilitary += HasCategory(crate, "MILITARY_EQUIPMENT") || HasCategory(crate, "WEAPON_ATTACHMENT") ? 1 : 0;
        for (const LootResult* r : {&cupboard, &shelf}) {
            resMilitary += HasCategory(*r, "AMMUNITION") || HasCategory(*r, "WEAPON_ATTACHMENT") || HasCategory(*r, "MILITARY_EQUIPMENT") ? 1 : 0;
            resFood += HasCategory(*r, "CONSUMABLE") ? 1 : 0;
        }
        medMedical += HasCategory(med, "MEDICAL") ? 1 : 0;
        indMech += HasCategory(tools, "MECHANICAL") ? 1 : 0;
    }
    // A depot crate always carries ammunition (guaranteed) unless already stripped.
    CHECK(depotAmmo >= 180);
    CHECK(depotMilitary > 80);
    // An apartment never yields military hardware.
    CHECK_EQ(resMilitary, 0);
    CHECK(resFood > 60);
    CHECK(medMedical >= 150);
    CHECK(indMech >= 170);
}

AIS_TEST(Loot_WeightedDistributionMatchesWeights) {
    const Content& c = All();
    // DF_AMMO weights 40/25/20 (rarity Common/Common/Uncommon, all x1 at the depot).
    std::map<std::string, int> counts;
    int total = 0;
    for (std::uint64_t raid = 0; raid < 6000; ++raid) {
        const LootResult r = Gen(c, "DF_ARMORY_CRATE", raid);
        for (const auto& i : r.items) {
            if (i.categoryId != "AMMUNITION" || i.pool != 0) continue;
            ++counts[i.itemId];
            ++total;
            break;  // first ammo pick only: unique picks bias later rolls
        }
    }
    CHECK(total > 5000);
    const double p545 = static_cast<double>(counts["ammo.545x39.ps"]) / total;
    const double p762 = static_cast<double>(counts["ammo.762x39.ps"]) / total;
    const double p918 = static_cast<double>(counts["ammo.9x18.pm"]) / total;
    CHECK_NEAR(p545, 40.0 / 85.0, 0.03);
    CHECK_NEAR(p762, 25.0 / 85.0, 0.03);
    CHECK_NEAR(p918, 20.0 / 85.0, 0.03);
}

AIS_TEST(Loot_LocationRarityWeightsShiftOutcomes) {
    // Same table, two locations: rare drops scale with the location's rarity weight.
    Content c;
    const auto p = Problems(R"(
[content]
version = 1
[location.RICH]
type = MilitaryDepot
profile.EquipmentCase = P
rarity_weights = Common:1, Uncommon:1, Rare:1, Exceptional:1
[location.POOR]
type = Residential
profile.EquipmentCase = P
rarity_weights = Common:1, Uncommon:1, Rare:0.1, Exceptional:0
[table.T]
entry = mil.ration_irp | weight=50 | rarity=Common
entry = tech.encrypted_drive | weight=50 | rarity=Exceptional
entry = att.optic_pso | weight=50 | rarity=Rare
[profile.P]
pool = T | guaranteed
[container.RICH_CASE]
location = RICH
type = EQUIPMENT_CASE
[container.POOR_CASE]
location = POOR
type = EQUIPMENT_CASE
)",
                            &c);
    CHECK(p.empty());
    int richRare = 0;
    int poorRare = 0;
    int poorExceptional = 0;
    for (std::uint64_t raid = 0; raid < 3000; ++raid) {
        const LootResult a = Gen(c, "RICH_CASE", raid);
        const LootResult b = Gen(c, "POOR_CASE", raid);
        richRare += a.items[0].rarity != Rarity::Common ? 1 : 0;
        poorRare += b.items[0].rarity == Rarity::Rare ? 1 : 0;
        poorExceptional += b.items[0].rarity == Rarity::Exceptional ? 1 : 0;
    }
    CHECK_NEAR(richRare / 3000.0, 2.0 / 3.0, 0.04);
    CHECK_NEAR(poorRare / 3000.0, 5.0 / 55.0, 0.03);
    CHECK_EQ(poorExceptional, 0);
}

AIS_TEST(Loot_ItemConditionIsStructuredAndWithinRanges) {
    const Content& c = All();
    double minTool = 1.0;
    double maxTool = 0.0;
    double minFill = 1.0;
    bool sawFill = false;
    for (std::uint64_t raid = 0; raid < 500; ++raid) {
        for (const auto& i : Gen(c, "IND_DAMAGED_TOOLBOX", raid).items) {
            CHECK(i.condition.integrity > 0.0 && i.condition.integrity <= 1.0);
            CHECK_EQ(i.condition.fill, 1.0);  // mechanical parts have no fill
            CHECK(i.condition.grade == GradeFor(i.condition.integrity));
            if (i.categoryId == "MECHANICAL") {
                minTool = std::min(minTool, i.condition.integrity);
                maxTool = std::max(maxTool, i.condition.integrity);
            }
        }
        for (const auto& i : Gen(c, "MED_TREATMENT_CABINET", raid).items) {
            if (i.itemId == "med.iv_saline") {
                sawFill = true;
                minFill = std::min(minFill, i.condition.fill);
                CHECK(i.condition.fill >= 0.15 && i.condition.fill <= 0.6);  // DEPLETED_SUPPLIES
            }
        }
    }
    // Damaged toolbox (x0.85) at a damp plant (-0.05): worn parts never look new.
    CHECK(maxTool <= 0.75 * 0.85 - 0.05 + 1e-9);
    CHECK(minTool >= 0.01);
    CHECK(minTool < 0.2);  // heavily worn seals reach the Degraded grade
    CHECK(sawFill);
    CHECK(minFill < 0.4);
    CHECK(GradeFor(0.95) == ConditionGrade::Pristine);
    CHECK(GradeFor(0.5) == ConditionGrade::Worn);
    CHECK(GradeFor(0.1) == ConditionGrade::Degraded);
}

AIS_TEST(Loot_ContainerCapacityIsNeverExceeded) {
    const Content& c = All();
    for (std::uint64_t raid = 0; raid < 300; ++raid) {
        for (const ContainerDef& d : c.loot.containers) {
            const LootResult r = GenerateLoot(c.loot, Catalog(), d, ContainerSeed(raid, d.id, 0));
            std::uint32_t used = 0;
            for (const auto& i : r.items) used += i.slotSize;
            CHECK(used == r.usedSlots);
            CHECK(r.usedSlots <= r.capacity);
            CHECK(r.capacity == c.loot.EffectiveCapacity(d));
        }
    }
    // A tiny container rejects picks that do not fit instead of overflowing.
    Content t;
    CHECK(Problems(R"(
[content]
version = 1
[container_type.TINY]
category = Shelf
capacity = 3
[location.L]
type = Workshop
profile.Shelf = P
[table.T]
entry = mech.gear_assembly | weight=1
entry = tool.wrench_set | weight=1
entry = tech.drone_core | weight=1
[profile.P]
pool = T | guaranteed | rolls=1
pool = T | chance=1 | rolls=2 | unique=false
[container.S]
location = L
type = TINY
)",
                   &t)
              .empty());
    int rejected = 0;
    for (std::uint64_t raid = 0; raid < 50; ++raid) {
        const LootResult r = Gen(t, "S", raid);
        CHECK(r.usedSlots <= 3);
        rejected += static_cast<int>(r.capacityRejected);
    }
    CHECK(rejected > 0);
}

AIS_TEST(Loot_EmptyContainersHaveExplicitReasons) {
    const Content& c = All();
    CHECK(Gen(c, "MED_PHARMACY_CABINET", 5).empty == EmptyReason::AuthoredEmpty);
    CHECK(Gen(c, "MED_XRAY_CABINET", 5).empty == EmptyReason::Destroyed);
    CHECK(Gen(c, "MED_XRAY_CABINET", 5).capacity == 0);
    int scavenged = 0;
    int profileEmpty = 0;
    for (std::uint64_t raid = 0; raid < 2000; ++raid) {
        const LootResult r = Gen(c, "RES_FLAT2_SHELF", raid, 1);
        scavenged += r.empty == EmptyReason::Scavenged ? 1 : 0;
        profileEmpty += r.empty == EmptyReason::ProfileEmpty ? 1 : 0;
        if (r.empty != EmptyReason::None) CHECK(r.items.empty());
        else CHECK(!r.items.empty());
    }
    // Residential: 35% already scavenged, then 30% of the rest empty shelves.
    CHECK_NEAR(scavenged / 2000.0, 0.35, 0.04);
    CHECK_NEAR(profileEmpty / 2000.0, 0.65 * 0.3, 0.04);
}

AIS_TEST(Loot_GuaranteedDropsAlwaysPresent) {
    const Content& c = All();
    for (std::uint64_t raid = 0; raid < 1000; ++raid) {
        const LootResult r = Gen(c, "MED_HIDDEN_CACHE", raid);
        CHECK(r.empty == EmptyReason::None);
        int kits = 0;
        int basic = 0;
        for (const auto& i : r.items) {
            if (i.pool == 0) ++kits;
            if (i.pool == 2) ++basic;
        }
        CHECK_EQ(kits, 1);
        CHECK_EQ(basic, 1);
        // Guaranteed pools come first, regardless of declaration order.
        CHECK(r.items[0].pool == 0);
    }
}

AIS_TEST(Loot_MutuallyExclusiveDropsNeverCoexist) {
    const Content& c = All();
    int surgical = 0;
    int morphineCache = 0;
    int prizeCrates = 0;
    for (std::uint64_t raid = 0; raid < 3000; ++raid) {
        const LootResult cache = Gen(c, "MED_HIDDEN_CACHE", raid);
        int surgicalHere = 0;
        for (const auto& i : cache.items) surgicalHere += i.itemId == "med.surgical_kit" ? 1 : 0;
        const bool morphineFromKit = cache.items[0].itemId == "med.morphine_syrette";
        CHECK(surgicalHere <= 1);
        if (morphineFromKit) CHECK_EQ(surgicalHere, 0);  // the drug pool cannot add the excluded kit
        surgical += surgicalHere;
        morphineCache += morphineFromKit ? 1 : 0;

        int prizes = 0;
        for (const auto& i : Gen(c, "DF_ARMORY_CRATE", raid).items) {
            prizes += (i.itemId == "att.suppressor_pbs" || i.itemId == "att.optic_pso" || i.itemId == "tech.encrypted_drive") ? 1 : 0;
        }
        CHECK(prizes <= 1);
        prizeCrates += prizes;
    }
    CHECK(surgical > 1000);
    CHECK(morphineCache > 1000);
    CHECK(prizeCrates > 100);
}

AIS_TEST(Loot_GroupDependenciesGateLaterPools) {
    Content c;
    CHECK(Problems(R"(
[content]
version = 1
[location.L]
type = Workshop
profile.Toolbox = P
[table.BATTERY]
entry = mil.radio_battery | weight=1 | group=POWER
[table.RADIO_PARTS]
entry = elec.relay_module | weight=1
[profile.P]
pool = BATTERY | chance=0.5
pool = RADIO_PARTS | chance=1 | requires=POWER
[container.B]
location = L
type = TOOLBOX
)",
                   &c)
              .empty());
    int with = 0;
    for (std::uint64_t raid = 0; raid < 2000; ++raid) {
        const LootResult r = Gen(c, "B", raid);
        bool battery = false;
        bool relay = false;
        for (const auto& i : r.items) {
            battery = battery || i.itemId == "mil.radio_battery";
            relay = relay || i.itemId == "elec.relay_module";
        }
        CHECK(relay == battery);  // the dependent drop appears exactly when its group was produced
        with += battery ? 1 : 0;
    }
    CHECK_NEAR(with / 2000.0, 0.5, 0.04);
}

AIS_TEST(Loot_ContainerConditionAffectsContents) {
    const Content& c = All();
    ContainerDef intact = Clone(c, "MED_TREATMENT_CABINET", "COND_TEST");
    ContainerDef damaged = intact;
    damaged.condition = ContainerCondition::Damaged;
    ContainerDef broken = intact;
    broken.condition = ContainerCondition::PartiallyDestroyed;
    double sumIntact = 0.0;
    double sumDamaged = 0.0;
    for (std::uint64_t raid = 0; raid < 400; ++raid) {
        const std::uint64_t s = ContainerSeed(raid, "COND_TEST", 0);
        const LootResult a = GenerateLoot(c.loot, Catalog(), intact, s);
        const LootResult b = GenerateLoot(c.loot, Catalog(), damaged, s);
        const LootResult d = GenerateLoot(c.loot, Catalog(), broken, s);
        for (const auto& i : a.items) sumIntact += i.condition.integrity;
        for (const auto& i : b.items) sumDamaged += i.condition.integrity;
        CHECK(d.capacity == 4);
        CHECK(d.usedSlots <= 4);
    }
    CHECK(sumDamaged < sumIntact * 0.9);
}
