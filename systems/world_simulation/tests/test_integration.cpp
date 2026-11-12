// Cross-system integration through MissionWorldHost: power grid, AIS, hazards,
// weather and squads from PR #1-#3 driving loot access and world state.
#include "StackRig.h"

using namespace wstest;
using llloot::LootAction;
using llloot::LootResultCode;
using llsim::EntityState;

namespace {

const char* kWiData = WS_DATA_DIR "/../../world_intelligence/data";

std::unique_ptr<StackRig> Depot(std::uint64_t seed = 2026) {
    auto p = std::make_unique<StackRig>(std::vector<const char*>{"dead_frequency_depot.ini"}, "scenarios/depot_power.ini", "scenarios/depot_world.ini", seed);
    StackRig& r = *p;
    r.PrintErrors();
    r.AddRobot(1, "SENTINEL", {1500, 2400, 0});
    r.AddRobot(2, "SENTINEL", {0, -2500, 0});
    r.AddRobot(3, "WRAITH", {3000, 1200, 0});
    return p;
}

void RestoreDepotPower(StackRig& r, llworld::PlayerId p) {
    r.Step(1);  // the facility accepts interactions after its first tick
    r.Move(p, {-250, 300, 0});
    const llenv::InteractionResult res = r.Facility(p, "GEN_DEPOT", llenv::InteractionAction::Activate);
    CHECK_MSG(res == llenv::InteractionResult::Applied || res == llenv::InteractionResult::ChannelStarted, llenv::ToString(res));
    r.Step(90);
}

}  // namespace

AIS_TEST(Integration_BindingsResolveAgainstTheFacility) {
    auto rig = Depot();
    StackRig& r = *rig;
    CHECK(r.ok);
    CHECK(r.host->ValidateBindings().empty());
    // A container gated by a node that does not exist is reported.
    llloot::LootConfig bad = r.content.loot;
    bad.containers[0].access = {llloot::AccessRequirement{llloot::AccessKind::Power, "NO_SUCH_NODE", 0}};
    llloot::ContainerService svc(bad, Catalog());
    llws::MissionWorldHost h(*r.wi, *r.sim, svc, r.aisCfg, &r.inv);
    CHECK(AnyContains(h.ValidateBindings(), "power-grid node 'NO_SUCH_NODE'"));
}

AIS_TEST(Integration_PowerRestorationUnlocksContainer) {
    auto rig = Depot();
    StackRig& r = *rig;
    CHECK(r.ok);
    CHECK(r.Join(1, r.Near("DF_ARMORY_CRATE")));
    r.Step(5);
    CHECK(r.Loot(1, LootAction::Search, "DF_ARMORY_CRATE") == LootResultCode::RejectedNoPower);
    CHECK(r.S("DF_GRID") == EntityState::Offline);
    CHECK(r.S("DF_COMMS") == EntityState::Offline);
    RestoreDepotPower(r, 1);
    CHECK(r.wi->Facility()->State(r.wi->Facility()->Find("ARMORY_LOCK")).powered);
    // The power grid change reached both cores through the host, event-driven.
    CHECK_EQ(r.lootRec.Count(llloot::LootEventType::AccessChanged), 1u);
    CHECK(r.S("DF_GEN") == EntityState::Active);
    CHECK(r.S("DF_GRID") == EntityState::Nominal);
    CHECK(r.S("DF_COMMS") == EntityState::Nominal);
    r.Move(1, r.Near("DF_ARMORY_CRATE"));
    CHECK(r.Loot(1, LootAction::Search, "DF_ARMORY_CRATE") == LootResultCode::Started);
    r.Step(100);
    const llloot::LootResult* res = r.loot->Contents(r.loot->Find("DF_ARMORY_CRATE"), 1);
    CHECK(res != nullptr);
    if (res == nullptr) return;
    const auto& d = r.loot->Def(r.loot->Find("DF_ARMORY_CRATE"));
    CHECK_EQ(res->Hash(), llloot::GenerateLoot(r.content.loot, Catalog(), d, llloot::ContainerSeed(2026, d.id, 0)).Hash());
}

AIS_TEST(Integration_SearchEmitsAcousticEventAndAttractsPatrol) {
    auto rig = Depot();
    StackRig& r = *rig;
    CHECK(r.ok);
    CHECK(r.Join(1, {0, -1500, 0}));
    RestoreDepotPower(r, 1);
    // Wait for the mission-level patrol to come through the yard.
    const std::uint32_t patrol = r.E("DF_PATROL");
    for (int i = 0; i < 1600 && r.sim->View(patrol).area != r.sim->FindArea("DF_YARD"); ++i) r.Step();
    CHECK(r.sim->View(patrol).area == r.sim->FindArea("DF_YARD"));
    CHECK(r.S("DF_PATROL") == EntityState::Active);
    const std::uint64_t aisEventsBefore = r.wi->Acoustic().Bus().Stats().Count(ais::SubmitResult::Accepted);
    r.Move(1, r.Near("DF_ARMORY_CRATE"));
    CHECK(r.Loot(1, LootAction::Search, "DF_ARMORY_CRATE") == LootResultCode::Started);
    // The search is an AIS stimulus (server-authoritative) ...
    CHECK(r.host->LootSoundsForwarded() >= 1u);
    CHECK(r.wi->Acoustic().Bus().Stats().Count(ais::SubmitResult::Accepted) > aisEventsBefore);
    // ... the nearby world-simulation patrol responds to it locally ...
    CHECK(r.S("DF_PATROL") == EntityState::Responding);
    llsim::PatrolDirective d;
    CHECK(r.sim->LastDirective(patrol, d));
    CHECK(d.kind == llsim::DirectiveKind::Respond && !d.central);
    CHECK(llworld::Distance(d.target, r.loot->Def(r.loot->Find("DF_ARMORY_CRATE")).position) < 1.0);
    // ... and robots in earshot perceive that exact AIS event (not some earlier noise).
    std::vector<ais::PerceptionRecord> heard;
    r.wi->Acoustic().SetPerceptionObserver(&heard);
    const ais::EventId searchEvent = r.host->LastLootAcousticEvent();
    CHECK(searchEvent != 0);
    r.Step(30);
    r.wi->Acoustic().SetPerceptionObserver(nullptr);
    std::set<ais::EntityId> listeners;
    for (const auto& p : heard) {
        if (p.eventId == searchEvent) listeners.insert(p.robotId);
    }
    std::printf("    search event heard by %zu robot(s)\n", listeners.size());
    CHECK(!listeners.empty());
    for (ais::EntityId id : listeners) CHECK(r.wi->OrderFor(id).awareness != ais::AwarenessState::Unaware);
}

AIS_TEST(Integration_FacilityAlarmLocksDownAndSealsStorage) {
    auto rig = Depot();
    StackRig& r = *rig;
    CHECK(r.Join(1, {0, -1500, 0}));
    RestoreDepotPower(r, 1);
    r.Move(1, r.Near("DF_STORE_CRATE"));
    r.Step(2);
    CHECK(!r.host->IsSealed("DF_STORAGE"));
    CHECK(r.wi->Facility()->ServerTriggerAlarm(r.wi->Facility()->Find("ALARM_DEPOT")));
    r.Step(3);
    CHECK(r.worldEvents[llworld::WorldEventType::AlarmRaised] >= 1);
    CHECK(r.S("DF_SECURITY") == EntityState::Alarm);
    CHECK(r.S("DF_DEPOT") == EntityState::Lockdown);
    CHECK(r.S("DF_STORAGE") == EntityState::Lockdown);
    CHECK(r.host->IsSealed("DF_STORAGE"));
    CHECK(r.Loot(1, LootAction::Search, "DF_STORE_CRATE") == LootResultCode::RejectedSealed);
}

AIS_TEST(Integration_GeneratorFailureChangesFacilityActivity) {
    StackRig r({"industrial_facility.ini"}, "scenarios/industrial_power.ini", "scenarios/industrial_world.ini", 9);
    r.PrintErrors();
    CHECK(r.ok);
    r.AddRobot(1, "SENTINEL", {63500, 3500, 0});
    CHECK(r.Join(1, {60000, -1000, 0}));
    r.Step(10);
    CHECK(r.S("IND_GEN") == EntityState::Active);
    CHECK(r.S("IND_GRID") == EntityState::Nominal);
    const llenv::NodeIndex emerg = r.wi->Facility()->Find("EMERG_IND");
    CHECK(!r.wi->Facility()->State(emerg).onEmergency);
    // 60 s of fuel: the power grid burns it; the world simulation only mirrors it.
    r.Step(1800);
    CHECK(r.worldEvents[llworld::WorldEventType::PowerLost] >= 1);
    CHECK(r.worldEvents[llworld::WorldEventType::EmergencyLighting] >= 1);
    CHECK(r.wi->Facility()->State(emerg).onEmergency);
    CHECK(r.S("IND_GEN") == EntityState::Offline);
    CHECK(r.S("IND_GRID") == EntityState::Offline);
    CHECK(r.S("IND_PRESS") == EntityState::Offline);
    CHECK(r.S("IND_FACILITY") == EntityState::Offline);
    // The blackout rule reassigned the patrol to the loading dock.
    llsim::PatrolDirective d;
    CHECK(r.sim->LastDirective(r.E("IND_PATROL"), d));
    CHECK(d.area == r.sim->FindArea("IND_LOADING"));
}

AIS_TEST(Integration_HazardAndWeatherEventsInfluenceWorldState) {
    StackRig r({"scenarios/pumping_station_9_overlay.ini"}, std::string(kWiData) + "/industrial_site.ini",
               std::string(kWiData) + "/industrial_site_world.ini", 5);
    r.PrintErrors();
    CHECK(r.ok);
    r.Step(5);
    CHECK(r.S("PS9_STORE_FIRE") == EntityState::Nominal);
    CHECK(r.S("PS9_FACILITY") == EntityState::Nominal);
    // A fire activated by the hazard system becomes a facility alarm.
    CHECK(r.wi->Hazards().Activate(r.wi->Hazards().Find("FIRE_STORE"), 1));
    r.Step(2);
    CHECK(r.S("PS9_STORE_FIRE") == EntityState::Active);
    CHECK(r.S("PS9_FACILITY") == EntityState::Alarm);
    // A storm from the weather system interrupts mission comms via a data rule.
    CHECK(r.S("PS9_RELAY") == EntityState::Nominal);
    CHECK(r.wi->Weather().RequestWeather(llworld::WeatherKind::Storm, 30, r.wi->CurrentTick()));
    r.Step(60);
    CHECK(r.host->SignalsForwarded() > 0u);
    CHECK(r.S("PS9_RELAY") == EntityState::Interrupted);
    CHECK(r.wi->Weather().RequestWeather(llworld::WeatherKind::Clear, 30, r.wi->CurrentTick()));
    r.Step(60);
    CHECK(r.S("PS9_RELAY") == EntityState::Nominal);
}

AIS_TEST(Integration_DistantFacilityAdvancesWithoutFullRateUpdates) {
    StackRig r({"dead_frequency_depot.ini", "dead_frequency_relay_station.ini"}, "scenarios/depot_power.ini", "scenarios/depot_world.ini", 77);
    CHECK(r.ok);
    CHECK(r.Join(1, {0, -1500, 0}));
    r.Step(7400);
    const std::uint32_t gen = r.E("RLY_GEN");
    // The relay ran its autonomous fuel / backup cycle while players were at the depot.
    CHECK(r.S("RLY_GEN") == EntityState::Offline);
    CHECK(r.S("RLY_BACKUP") == EntityState::Active);
    CHECK(r.sim->TierOf(gen) == llsim::Tier::Low);
    CHECK(r.sim->View(gen).updates <= 7400 / 600 + 2);
    // Depot entities near the player were simulated at High rate meanwhile.
    CHECK(r.sim->View(r.E("DF_DEPOT")).updates > 2000);
}
