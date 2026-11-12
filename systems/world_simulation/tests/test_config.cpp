// Configuration pipeline: shipped content is valid; every class of authoring
// mistake produces a specific diagnostic.
#include "WsTestHelpers.h"

using namespace wstest;

namespace {

// Minimal valid location used as a base for negative tests.
const std::string kBase = R"(
[content]
version = 1
[location.T_LOC]
type = Workshop
profile.Toolbox = T_PROFILE
[table.T_TABLE]
entry = mech.bearing_set | weight=10 | qty=1-2
entry = mech.spring_pack | weight=5
[profile.T_PROFILE]
pool = T_TABLE | guaranteed | rolls=1
[container.T_BOX]
location = T_LOC
type = TOOLBOX
position = 0, 0, 0
[area.T_AREA]
center = 0, 0, 0
radius_cm = 500
[entity.T_FAC]
kind = Facility
area = T_AREA
)";

}  // namespace

AIS_TEST(Config_ShippedContentIsValidIndividuallyAndTogether) {
    CHECK(Catalog().Size() >= 40);
    for (const char* f : kAllLocations) {
        const Content c = Load({f});
        CHECK_MSG(c.ok, f);
    }
    const Content all = Load({kAllLocations[0], kAllLocations[1], kAllLocations[2], kAllLocations[3], kAllLocations[4]});
    CHECK(all.ok);
    CHECK_EQ(all.loot.locations.size(), 6u);
    CHECK(all.loot.containers.size() >= 25u);
    CHECK(all.sim.entities.size() >= 20u);
    // Every location type in the example map has a distinct profile set.
    CHECK(all.loot.FindLocation("DF_DEPOT") != llloot::kNone);
    CHECK(all.loot.FindLocation("RES_BLOCK") != llloot::kNone);
}

AIS_TEST(Config_BaseFixtureIsValid) {
    const auto p = Problems(kBase);
    for (const auto& m : p) std::printf("    %s\n", m.c_str());
    CHECK(p.empty());
}

AIS_TEST(Config_UnsupportedVersionAndMissingVersion) {
    std::string v2 = kBase;
    v2.replace(v2.find("version = 1"), 11, "version = 2");
    CHECK(AnyContains(Problems(v2), "unsupported content version 2"));
    std::string none = kBase;
    none.replace(none.find("[content]\nversion = 1"), 21, "");
    CHECK(AnyContains(Problems(none), "missing [content] version"));
}

AIS_TEST(Config_UnknownSectionsAndKeysAreReported) {
    CHECK(AnyContains(Problems(kBase + "[lootz.X]\nfoo = 1\n"), "unknown section [lootz.X]"));
    CHECK(AnyContains(Problems(kBase + "[entity.T_GEN]\nkind = Generator\narea = T_AREA\nfuel = 5\n"), "unknown key 'fuel'"));
    CHECK(AnyContains(Problems(kBase + "[table.T2]\nentry = mech.bearing_set | wieght=3\n"), "unknown entry option 'wieght'"));
}

AIS_TEST(Config_DuplicateIds) {
    const auto p = Problems(kBase + "[container.T_BOX]\nlocation = T_LOC\ntype = TOOLBOX\nposition = 1,1,0\n[entity.T_FAC]\nkind = Facility\narea = T_AREA\n");
    CHECK(AnyContains(p, "duplicate container id 'T_BOX'"));
    CHECK(AnyContains(p, "duplicate entity id 'T_FAC'"));
}

AIS_TEST(Config_MissingItemReferences) {
    const auto p = Problems(kBase + "[table.T_BAD]\nentry = mech.unobtainium | weight=1\n");
    CHECK(AnyContains(p, "entry 'mech.unobtainium': item id not found in the item catalog"));
}

AIS_TEST(Config_InvalidProbabilityWeights) {
    CHECK(AnyContains(Problems(kBase + "[table.T_W]\nentry = mech.bearing_set | weight=0\n"), "weight must be a finite number > 0"));
    CHECK(AnyContains(Problems(kBase + "[table.T_W]\nentry = mech.bearing_set | weight=-3\n"), "weight must be a finite number > 0"));
    CHECK(AnyContains(Problems(kBase + "[profile.T_P2]\npool = T_TABLE | chance=1.5\n"), "chance must be within 0..1"));
    CHECK(AnyContains(Problems(kBase + "[profile.T_P2]\nempty_chance = 2\npool = T_TABLE\n"), "empty_chance must be within 0..1"));
    CHECK(AnyContains(Problems(kBase + "[location.T_L2]\ntype = Residential\nrarity_weights = Common:0, Uncommon:0, Rare:0, Exceptional:0\n"),
                      "all rarity weights are zero"));
    CHECK(AnyContains(Problems(kBase + "[profile.T_P2]\npool = T_TABLE | guaranteed | chance=0.5\n"), "a guaranteed pool cannot have chance < 1"));
}

AIS_TEST(Config_ImpossibleCapacitiesAndQuantities) {
    // Five guaranteed 2-slot rolls (10 slots) cannot fit a 6-slot toolbox.
    const auto p = Problems(kBase +
                            "[table.T_BIG]\nentry = mech.gear_assembly | weight=1\nentry = tool.wrench_set | weight=1\nentry = tool.bolt_cutter | "
                            "weight=1\nentry = armor.plate_insert_steel | weight=1\nentry = tech.drone_core | weight=1\n"
                            "[profile.T_HEAVY]\npool = T_BIG | guaranteed | rolls=5\n"
                            "[container.T_BOX2]\nlocation = T_LOC\ntype = TOOLBOX\nprofile = T_HEAVY\nposition = 0,0,0\n");
    CHECK(AnyContains(p, "guaranteed drops of profile 'T_HEAVY' need 10 slots but the Intact container holds 6"));
    // A partially destroyed container halves capacity: 4 guaranteed slots no longer fit 3.
    const auto q = Problems(kBase + "[profile.T_FOUR]\npool = T_TABLE | guaranteed | rolls=2 | unique=false\npool = T_TABLE | guaranteed | rolls=2 | unique=false\n"
                                    "[container.T_BOX3]\nlocation = T_LOC\ntype = TOOLBOX\nprofile = T_FOUR\ncondition = PartiallyDestroyed\nposition = 0,0,0\n");
    CHECK(AnyContains(q, "holds 3"));
    CHECK(AnyContains(Problems(kBase + "[container_type.T_ZERO]\ncategory = Shelf\ncapacity = 0\n"), "capacity must be at least 1"));
    CHECK(AnyContains(Problems(kBase + "[table.T_Q]\nentry = med.surgical_kit | weight=1 | qty=1-3\n"), "exceeds the catalog stack limit 1"));
    CHECK(AnyContains(Problems(kBase + "[table.T_Q]\nentry = med.tourniquet | weight=1 | qty=2-1\n"), "quantity range must satisfy"));
    CHECK(AnyContains(Problems(kBase + "[profile.T_U]\npool = T_TABLE | rolls=3\n"), "3 unique rolls but the table has only 2 entries"));
}

AIS_TEST(Config_ContradictoryLootRules) {
    // Two guaranteed pools that exclude each other.
    CHECK(AnyContains(Problems(kBase + "[profile.T_X]\npool = T_TABLE | guaranteed | exclusive=ONE\npool = T_TABLE | guaranteed | exclusive=ONE\n"),
                      "two guaranteed pools share exclusive tag 'ONE'"));
    // Dependency on a group produced only later (or by the pool itself).
    CHECK(AnyContains(Problems(kBase + "[profile.T_D]\npool = T_TABLE | chance=0.5 | requires=LATER\npool = T_TABLE | chance=0.5 | group=LATER\n"),
                      "only produced later or by itself"));
    CHECK(AnyContains(Problems(kBase + "[profile.T_D]\npool = T_TABLE | chance=0.5 | group=SELF | requires=SELF\n"), "only produced later or by itself"));
    CHECK(AnyContains(Problems(kBase + "[profile.T_D]\npool = T_TABLE | requires=NOPE\n"), "requires unknown group 'NOPE'"));
    // A guaranteed drop cannot depend on an optional one.
    CHECK(AnyContains(Problems(kBase + "[profile.T_G]\npool = T_TABLE | chance=0.5 | group=OPT\npool = T_TABLE | guaranteed | requires=OPT\n"),
                      "guaranteed pool depends on group 'OPT' that is not guaranteed"));
    CHECK(AnyContains(Problems(kBase + "[condition.T_C]\nintegrity = 0.9-0.2\n"), "integrity range must satisfy"));
}

AIS_TEST(Config_ContainerReferencesAndProfiles) {
    const auto p = Problems(kBase + "[container.T_ORPHAN]\nlocation = NOWHERE\ntype = CRATE9\nposition = 0,0,0\n"
                                    "[container.T_NOPROF]\nlocation = T_LOC\ntype = SHELF\nposition = 0,0,0\n");
    CHECK(AnyContains(p, "unknown location 'NOWHERE'"));
    CHECK(AnyContains(p, "unknown container type 'CRATE9'"));
    CHECK(AnyContains(p, "has no profile.Shelf and the container sets none"));
    CHECK(AnyContains(Problems(kBase + "[container.T_A]\nlocation = T_LOC\ntype = TOOLBOX\naccess = magic:door\n"), "invalid value 'magic:door'"));
}

AIS_TEST(Config_InvalidSimulationIntervals) {
    CHECK(AnyContains(Problems(kBase + "[tier.Medium]\ninterval_ticks = 0\n"), "[tier.Medium] interval_ticks must be >= 1"));
    CHECK(AnyContains(Problems(kBase + "[tier.High]\ninterval_ticks = 900\n"), "tier intervals must satisfy High <= Medium <= Low"));
    CHECK(AnyContains(Problems(kBase + "[tier.Ultra]\ninterval_ticks = 1\n"), "unknown tier [tier.Ultra]"));
    CHECK(AnyContains(Problems(kBase + "[simulation]\nrelevance_interval_ticks = 0\n"), "relevance_interval_ticks must be >= 1"));
    CHECK(AnyContains(Problems(kBase + "[simulation]\nqueue_capacity = 1\n"), "queue_capacity 1 is too small"));
}

AIS_TEST(Config_MissingAndCircularDependencies) {
    CHECK(AnyContains(Problems(kBase + "[entity.T_SEC]\nkind = SecuritySystem\narea = T_AREA\ndepends_on = T_GHOST\n"), "missing dependency 'T_GHOST'"));
    const auto p = Problems(kBase + "[entity.T_A1]\nkind = PowerNetwork\narea = T_AREA\ndepends_on = T_A3\n"
                                    "[entity.T_A2]\nkind = PowerNetwork\narea = T_AREA\ndepends_on = T_A1\n"
                                    "[entity.T_A3]\nkind = PowerNetwork\narea = T_AREA\ndepends_on = T_A2\n");
    CHECK(AnyContains(p, "circular dependency"));
    CHECK(AnyContains(p, "T_A1"));
    CHECK(AnyContains(Problems(kBase + "[entity.T_S]\nkind = Storage\narea = T_AREA\ndepends_on = T_S\n"), "depends on itself"));
    CHECK(AnyContains(Problems(kBase + "[entity.T_B]\nkind = Generator\narea = T_AREA\nbackup_for = T_FAC\n"), "backup_for must name another Generator"));
    CHECK(AnyContains(Problems(kBase + "[entity.T_P]\nkind = PatrolGroup\narea = T_AREA\nroute = T_AREA, NOWHERE\n"),
                      "route references unknown area 'NOWHERE'"));
    CHECK(AnyContains(Problems(kBase + "[entity.T_K]\narea = T_AREA\n"), "needs kind"));
    CHECK(AnyContains(Problems(kBase + "[entity.T_W]\nkind = Storage\narea = T_AREA\nstate = Responding\n"), "state Responding is not valid for Storage"));
}

AIS_TEST(Config_InvalidActivitySchedulesAndRules) {
    const std::string patrol = "[entity.T_P]\nkind = PatrolGroup\narea = T_AREA\nroute = T_AREA\ndwell_ticks = 300\ntravel_ticks = 300\n";
    CHECK(AnyContains(Problems(kBase + patrol + "[schedule.T_S]\nat_ticks = 10\nrepeat_ticks = 100\ndo = retask:T_P=T_AREA\n"),
                      "needs 600 ticks per leg; the group could never complete a leg"));
    CHECK(AnyContains(Problems(kBase + "[schedule.T_S]\nat_ticks = 10\nrepeat_ticks = 1\ndo = set:T_FAC=Alarm\n"), "shorter than the High tier interval"));
    CHECK(AnyContains(Problems(kBase + "[schedule.T_S]\nat_ticks = 10\n"), "needs at least one action"));
    CHECK(AnyContains(Problems(kBase + "[schedule.T_S]\nat_ticks = 10\ndo = explode:T_FAC\n"), "unknown action 'explode'"));
    CHECK(AnyContains(Problems(kBase + "[schedule.T_S]\nat_ticks = 10\ndo = set:T_FAC=Responding\n"), "not valid for Facility"));
    CHECK(AnyContains(Problems(kBase + "[rule.T_R]\non = state:T_NOPE=Alarm\ndo = set:T_FAC=Alarm\n"), "unknown entity 'T_NOPE'"));
    // Two rules that keep re-triggering each other forever.
    const auto loop = Problems(kBase + "[rule.T_R1]\non = state:T_FAC=Alarm\ndo = set:T_FAC=Lockdown\n[rule.T_R2]\non = state:T_FAC=Lockdown\ndo = set:T_FAC=Alarm\n");
    CHECK(AnyContains(loop, "circular rule chain"));
    const auto broken = Problems(kBase + "[rule.T_R1]\non = state:T_FAC=Alarm\ndo = set:T_FAC=Lockdown\nonce = true\n[rule.T_R2]\non = state:T_FAC=Lockdown\ndo = set:T_FAC=Alarm\n");
    CHECK(!AnyContains(broken, "circular rule chain"));
    // Bound entities belong to the power grid: rules cannot set them directly.
    CHECK(AnyContains(Problems(kBase + "[entity.T_G]\nkind = Generator\narea = T_AREA\nbind = node:GEN\n[rule.T_R]\non = state:T_FAC=Alarm\ndo = stop:T_G\n"),
                      "bound to infrastructure"));
}
