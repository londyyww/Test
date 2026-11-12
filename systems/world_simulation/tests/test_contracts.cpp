// Cross-system contract + multiplayer authority tests over the real PR #1-#4
// interfaces. Each test names the boundary it pins down.
#include <cmath>
#include <limits>

#include "StackRig.h"

using namespace wstest;
using llloot::LootAction;
using llloot::LootResultCode;
using llsim::EntityState;

namespace {

const char* kWiData = WS_DATA_DIR "/../../world_intelligence/data";

std::unique_ptr<StackRig> DepotRig(bool robots = true) {
    auto r = std::make_unique<StackRig>(std::vector<const char*>{"dead_frequency_depot.ini"}, "scenarios/depot_power.ini", "scenarios/depot_world.ini",
                                        2026);
    r->PrintErrors();
    if (robots) {
        r->AddRobot(1, "SENTINEL", {1500, 2400, 0});
        r->AddRobot(2, "SENTINEL", {0, -2500, 0});
    }
    return r;
}

std::unique_ptr<StackRig> Ps9Rig() {
    auto r = std::make_unique<StackRig>(std::vector<const char*>{"scenarios/pumping_station_9_overlay.ini"}, std::string(kWiData) + "/industrial_site.ini",
                                        std::string(kWiData) + "/industrial_site_world.ini", 5);
    r->PrintErrors();
    return r;
}

void StartDepotGenerator(StackRig& r) {
    r.Step(1);
    r.Move(1, {-250, 300, 0});
    r.Facility(1, "GEN_DEPOT", llenv::InteractionAction::Activate);
    r.Step(90);
}

// Footsteps at a fixed spot; returns how many reached robot 1's perception.
std::size_t FootstepsHeard(bool storm) {
    auto rig = DepotRig(false);
    StackRig& r = *rig;
    r.AddRobot(1, "SENTINEL", {0, 0, 0});
    r.Join(1, {900, 0, 0});
    if (storm) {
        r.wi->Weather().RequestWeather(llworld::WeatherKind::Storm, 30, r.wi->CurrentTick());
        for (int i = 0; i < 120; ++i) r.host->Step();  // stationary listener: robot self-noise would mask footsteps
    } else {
        for (int i = 0; i < 120; ++i) r.host->Step();  // stationary listener: robot self-noise would mask footsteps
    }
    std::vector<ais::PerceptionRecord> heard;
    r.wi->Acoustic().SetPerceptionObserver(&heard);
    for (int i = 0; i < 20; ++i) {
        r.wi->PlayerFootstep(1, {900, 0, 0}, ais::SurfaceType::Concrete);
        for (int k = 0; k < 15; ++k) r.host->Step();
    }
    r.wi->Acoustic().SetPerceptionObserver(nullptr);
    std::size_t n = 0;
    for (const auto& p : heard) n += p.robotId == 1 ? 1u : 0u;
    return n;
}

}  // namespace

// ---------------------------------------------------------------- PR #1-#3 contracts consumed by the host

AIS_TEST(Contract_WeatherReducesAcousticDetection) {
    const std::size_t clear = FootstepsHeard(false);
    const std::size_t storm = FootstepsHeard(true);
    std::printf("    footsteps at 9 m heard: clear %zu, storm %zu\n", clear, storm);
    CHECK(clear > 0u);
    CHECK(storm < clear);
}

AIS_TEST(Contract_GunfireDrivesSquadInvestigation) {
    auto rig = DepotRig();
    StackRig& r = *rig;
    CHECK(r.Join(1, {1500, 1500, 0}));
    r.Step(5);
    CHECK(r.wi->OrderFor(1).awareness == ais::AwarenessState::Unaware);
    r.wi->ReportGunfire(1, {1500, 1500, 0});
    r.Step(60);
    const llworld::TacticalIntent* intent = r.wi->Squads().Intent(1);
    CHECK(r.wi->OrderFor(1).awareness != ais::AwarenessState::Unaware);
    CHECK(intent != nullptr && intent->kind != llworld::IntentKind::Patrol);
    // Gunfire also reaches mission-level world state through the host.
    CHECK(r.S("DF_PATROL") == EntityState::Responding || r.sim->Stats().directives > 0u);
}

AIS_TEST(Contract_CommsLossBreaksSquadLinkUntilRelayPowered) {
    auto rig = DepotRig();
    StackRig& r = *rig;
    CHECK(r.Join(1, {0, -1500, 0}));
    r.Step(2);
    r.wi->SetRobotBody(1, {1500, 2400, 0}, true, llworld::CommsHealth::Intact);
    r.wi->SetRobotBody(2, {0, -2500, 0}, true, llworld::CommsHealth::Intact);
    for (int i = 0; i < 31; ++i) r.host->Step();  // squad links are recomputed every coordination interval
    // Dark depot: the radio mast is down and the robots are ~51 m apart (SENTINEL range 50 m).
    CHECK(!r.host->IsPowered("RADIO_DEPOT"));
    CHECK(!r.wi->Squads().CanCommunicate(1, 2));
    CHECK(r.S("DF_COMMS") == EntityState::Offline);
    CHECK(r.S("DF_PATROL") == EntityState::Interrupted);
    StartDepotGenerator(r);
    CHECK(r.host->IsPowered("RADIO_DEPOT"));
    CHECK(r.wi->Squads().CanCommunicate(1, 2));
    CHECK(r.S("DF_PATROL") != EntityState::Interrupted);
}

AIS_TEST(Contract_StormInterruptionIsEdgeTriggeredAndSurvivesResampling) {
    auto rig = Ps9Rig();
    StackRig& r = *rig;
    CHECK(r.ok);
    // A player right next to the relay keeps it at High tier (re-sampled every 3 ticks).
    CHECK(r.Join(1, {1000, 800, 0}));
    r.Step(5);
    CHECK(r.sim->TierOf(r.E("PS9_RELAY")) == llsim::Tier::High);
    CHECK(r.wi->Weather().RequestWeather(llworld::WeatherKind::Storm, 30, r.wi->CurrentTick()));
    r.Step(300);
    CHECK(r.S("PS9_RELAY") == EntityState::Interrupted);
    // WeatherChanged is published at transition start and end; the rule fired once.
    std::size_t fired = 0;
    for (const auto& e : r.simRec.events) fired += e.type == llsim::SimEventType::RuleFired ? 1u : 0u;
    CHECK_EQ(fired, 1u);
}

AIS_TEST(Contract_GameEventFloodCannotStarveInfrastructureEvents) {
    StackRig r({"industrial_facility.ini"}, "scenarios/industrial_power.ini", "scenarios/industrial_world.ini", 9);
    CHECK(r.ok);
    CHECK(r.Join(1, {60000, -1000, 0}));
    r.Step(1780);
    for (int t = 0; t < 40; ++t) {
        for (int k = 0; k < 400; ++k) r.wi->ReportGunfire(1, {60000.0 + k, -1000, 0});
        r.Step(1);
    }
    CHECK(r.worldEvents[llworld::WorldEventType::PowerLost] >= 1);
    CHECK(r.S("IND_GEN") == EntityState::Offline);
    CHECK(r.wi->Bus().Dropped() > 0u);  // the flood itself was clipped
}

AIS_TEST(Contract_LootRetryDoesNotDuplicateAcousticEvents) {
    auto rig = DepotRig();
    StackRig& r = *rig;
    CHECK(r.Join(1, {0, -1500, 0}));
    StartDepotGenerator(r);
    r.Move(1, r.Near("DF_ARMORY_CRATE"));
    const std::uint32_t c = r.loot->Find("DF_ARMORY_CRATE");
    CHECK(r.loot->Submit({1, 500, c, LootAction::Search, 0}) == LootResultCode::Started);
    const std::uint64_t sounds = r.host->LootSoundsForwarded();
    const std::uint64_t accepted = r.wi->Acoustic().Bus().Stats().Count(ais::SubmitResult::Accepted);
    for (int i = 0; i < 20; ++i) CHECK(r.loot->Submit({1, 500, c, LootAction::Search, 0}) == LootResultCode::RejectedDuplicate);
    CHECK(r.loot->Submit({1, 501, c, LootAction::Search, 0}) == LootResultCode::RejectedBusy);
    CHECK_EQ(r.host->LootSoundsForwarded(), sounds);
    CHECK_EQ(r.wi->Acoustic().Bus().Stats().Count(ais::SubmitResult::Accepted), accepted);
}

AIS_TEST(Contract_HazardSignalsDeliveredOnceDespiteDuplicateCauses) {
    auto rig = Ps9Rig();
    StackRig& r = *rig;
    r.Step(3);
    const std::uint32_t fire = r.wi->Hazards().Find("FIRE_STORE");
    r.wi->Hazards().Activate(fire, 7);
    r.wi->Hazards().Activate(fire, 7);
    r.wi->Hazards().Activate(fire, 8);
    r.Step(3);
    std::size_t changes = 0;
    for (const auto& e : r.simRec.events) changes += (e.type == llsim::SimEventType::StateChanged && e.entity == r.E("PS9_STORE_FIRE")) ? 1u : 0u;
    CHECK_EQ(changes, 1u);
    CHECK(r.S("PS9_FACILITY") == EntityState::Alarm);
}

// ---------------------------------------------------------------- multiplayer authority

AIS_TEST(Multiplayer_ClientRolesCannotChangeAuthoritativeState) {
    llenv::FacilityDef def;
    CHECK(llenv::LoadFacilityFile(Data("scenarios/depot_power.ini"), def).ok);
    llenv::Facility client(def, llenv::AuthorityRole::Client);
    CHECK(!client.ConnectPlayer(1, {0, 0, 0}, 0xFFFFFFFFu));
    llenv::InteractionRequest q;
    q.player = 1;
    q.sequence = 1;
    q.node = client.Find("GEN_DEPOT");
    q.action = llenv::InteractionAction::Activate;
    CHECK(client.Submit(q) == llenv::InteractionResult::RejectedNotAuthority);

    ais::AisConfig cfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment env;
    ais::AcousticWorld server(cfg, env, ais::AuthorityRole::Server, nullptr);
    const ais::AcousticEvent e = ais::MakeEvent(cfg, ais::StimulusCategory::Gunfire, {0, 0, 0}, 0, ais::SurfaceType::None, 7);
    CHECK(server.SubmitEvent(e, ais::EventOrigin::ClientClaim) == ais::SubmitResult::RejectedNotAuthoritative);

    llloot::ContainerServiceConfig sc;
    sc.serverAuthority = false;
    const Content content = Load({"medical_facility.ini"});
    llloot::ContainerService loot(content.loot, Catalog(), sc);
    loot.BeginRaid(1);
    loot.ConnectPlayer(1, 101, {0, 60000, 0});
    CHECK(loot.Submit({1, 1, 0, LootAction::Search, 0}) == LootResultCode::RejectedNotAuthority);
}

AIS_TEST(Multiplayer_PlayerIdentityCannotBeSubstituted) {
    auto rig = DepotRig(false);
    StackRig& r = *rig;
    CHECK(r.Join(1, r.Near("DF_CP_NORTH_LOCKER")));
    CHECK(r.Join(2, {0, -6000, 0}));
    r.Step(1);
    const std::uint32_t c = r.loot->Find("DF_CP_NORTH_LOCKER");
    CHECK(r.Loot(1, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    // Player 2 cannot cancel player 1's claim, and range is checked against the
    // server's position for the submitting player, not anything the client sends.
    CHECK(r.Loot(2, LootAction::CancelSearch, "DF_CP_NORTH_LOCKER") == LootResultCode::RejectedNotApplicable);
    CHECK_EQ(r.loot->Claimant(c, 2), 1u);
    CHECK(r.Loot(2, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::RejectedOutOfRange);
    // Facility interactions use the session position too.
    CHECK(r.Facility(2, "GEN_DEPOT", llenv::InteractionAction::Activate) == llenv::InteractionResult::RejectedOutOfRange);
    // Unknown and zero ids are rejected everywhere.
    CHECK(r.loot->Submit({77, 1, c, LootAction::Search, 0}) == LootResultCode::RejectedUnknownPlayer);
    CHECK(r.Facility(77, "GEN_DEPOT", llenv::InteractionAction::Activate) == llenv::InteractionResult::RejectedUnknownPlayer);
    CHECK(r.Facility(0, "GEN_DEPOT", llenv::InteractionAction::Activate) == llenv::InteractionResult::RejectedUnknownPlayer);
}

AIS_TEST(Multiplayer_DisconnectLeavesNoLocksAndDropsRelevance) {
    auto rig = DepotRig(false);
    StackRig& r = *rig;
    CHECK(r.Join(1, r.Near("DF_CP_NORTH_LOCKER")));
    r.Step(20);
    const std::uint32_t yard = r.sim->FindArea("DF_CHECKPOINT_N");
    CHECK(r.sim->AreaTier(yard) == llsim::Tier::High);
    const std::uint32_t c = r.loot->Find("DF_CP_NORTH_LOCKER");
    CHECK(r.Loot(1, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    r.host->DisconnectPlayer(1);
    CHECK_EQ(r.loot->Claimant(c, 0), 0u);
    r.Step(20);
    CHECK(r.sim->AreaTier(yard) != llsim::Tier::High);
    // Another player can now take the container.
    CHECK(r.Join(2, r.Near("DF_CP_NORTH_LOCKER")));
    CHECK(r.Loot(2, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    // A refused connection leaves nothing half-connected in the facility.
    for (llworld::PlayerId p = 3; p <= 5; ++p) r.Join(p, {0, 0, 0});
    CHECK(!r.Join(6, {0, 0, 0}));
    CHECK(r.Facility(6, "GEN_DEPOT", llenv::InteractionAction::Activate) == llenv::InteractionResult::RejectedUnknownPlayer);
}

AIS_TEST(Multiplayer_FacilitySequencesNearUint32MaxAreAccepted) {
    auto rig = DepotRig(false);
    StackRig& r = *rig;
    CHECK(r.Join(1, {-250, 300, 0}));
    r.Step(1);
    llenv::InteractionRequest q;
    q.player = 1;
    q.node = r.wi->Facility()->Find("BRK_DEPOT");
    q.action = llenv::InteractionAction::Deactivate;
    q.sequence = 0xFFFFFFF0u;
    r.Move(1, {0, 200, 0});
    CHECK(r.wi->Facility()->Submit(q) != llenv::InteractionResult::RejectedDuplicate);
    q.sequence = 0xFFFFFFFFu;
    q.action = llenv::InteractionAction::Activate;
    CHECK(r.wi->Facility()->Submit(q) != llenv::InteractionResult::RejectedDuplicate);
    CHECK(r.wi->Facility()->Submit(q) == llenv::InteractionResult::RejectedDuplicate);
}

AIS_TEST(Multiplayer_FacilitySessionHistoryIsBounded) {
    llenv::FacilityDef def;
    CHECK(llenv::LoadFacilityFile(Data("scenarios/depot_power.ini"), def).ok);
    llenv::Facility f(def);
    f.Step();
    for (llenv::PlayerId id = 1; id <= 2000; ++id) {
        CHECK(f.ConnectPlayer(id, {0, 0, 0}, 0));
        CHECK(f.DisconnectPlayer(id));
    }
    CHECK(f.Capture().players.size() <= 16u);
}

AIS_TEST(Multiplayer_BogusHazardRequestsCannotExhaustPlayerSlots) {
    auto rig = Ps9Rig();
    StackRig& r = *rig;
    CHECK(r.Join(1, {2100, 300, 0}));
    r.Step(2);
    const std::uint32_t arc = r.wi->Hazards().Find("CABLE_ARC");
    for (llworld::PlayerId bogus = 100; bogus < 110; ++bogus)
        r.wi->Hazards().Interact(llworld::HazardRequest{bogus, 1, arc, llworld::HazardAction::Trigger, {2100, 100, 0}}, r.wi->CurrentTick());
    const auto res =
        r.wi->Hazards().Interact(llworld::HazardRequest{1, 1, arc, llworld::HazardAction::Trigger, {2100, 300, 0}}, r.wi->CurrentTick());
    CHECK(res != llworld::HazardRequestResult::RejectedTooManyPlayers);
}

// ---------------------------------------------------------------- boundaries in PR #1-#2

AIS_TEST(Boundary_AisZeroCapacityAndEvictedDuplicates) {
    ais::AisConfig cfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment env;
    cfg.bus.queueCapacity = 0;
    {
        ais::AcousticWorld w(cfg, env, ais::AuthorityRole::Server, nullptr);
        const ais::AcousticEvent e = ais::MakeEvent(cfg, ais::StimulusCategory::Gunfire, {0, 0, 0}, 0, ais::SurfaceType::None, 7);
        CHECK(w.SubmitEvent(e, ais::EventOrigin::ServerAuthoritative) == ais::SubmitResult::RejectedQueueFull);
    }
    cfg.bus.queueCapacity = 2;
    ais::AcousticWorld w(cfg, env, ais::AuthorityRole::Server, nullptr);
    const ais::AcousticEvent quiet = ais::MakeEvent(cfg, ais::StimulusCategory::Footstep, {0, 0, 0}, 0, ais::SurfaceType::Concrete, 5);
    CHECK(w.SubmitEvent(quiet, ais::EventOrigin::ServerAuthoritative) == ais::SubmitResult::Accepted);
    CHECK(w.SubmitEvent(ais::MakeEvent(cfg, ais::StimulusCategory::Gunfire, {100, 0, 0}, 0, ais::SurfaceType::None, 6),
                        ais::EventOrigin::ServerAuthoritative) == ais::SubmitResult::Accepted);
    CHECK(w.SubmitEvent(ais::MakeEvent(cfg, ais::StimulusCategory::Gunfire, {200, 0, 0}, 0, ais::SurfaceType::None, 8),
                        ais::EventOrigin::ServerAuthoritative) == ais::SubmitResult::AcceptedEvictedLower);
    // The footstep was evicted unheard: a repeat must not be reported as merged into it.
    CHECK(w.SubmitEvent(quiet, ais::EventOrigin::ServerAuthoritative) != ais::SubmitResult::MergedDuplicate);
}

AIS_TEST(Boundary_ExtremeNumericInputsAreSafe) {
    ais::AisConfig cfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment env;
    ais::AcousticWorld w(cfg, env, ais::AuthorityRole::Server, nullptr);
    w.AddRobot(1, "SENTINEL", {1e300, -1e300, 0});
    w.AddRobot(2, "SENTINEL", {0, 0, 0});
    w.SubmitEvent(ais::MakeEvent(cfg, ais::StimulusCategory::Gunfire, {0, 0, 0}, 0, ais::SurfaceType::None, 9), ais::EventOrigin::ServerAuthoritative);
    for (int i = 0; i < 5; ++i) w.Step();
    CHECK(w.HostState(2) != nullptr);
    // A generator with an absurd fuel load schedules a saturated timer, not UB.
    llenv::FacilityDef def;
    CHECK(llenv::LoadFacilityFile(Data("scenarios/industrial_power.ini"), def).ok);
    def.nodes[static_cast<std::size_t>(def.Find("GEN_IND"))].fuelTicks = 1e300;
    llenv::Facility f(def);
    for (int i = 0; i < 5; ++i) f.Step();
    CHECK(f.State(f.Find("GEN_IND")).gen == llenv::GeneratorState::Running);
}

AIS_TEST(Boundary_FullStackDeterministicReplay) {
    auto run = []() {
        auto rig = DepotRig();
        StackRig& r = *rig;
        r.Join(1, {0, -1500, 0});
        StartDepotGenerator(r);
        r.Move(1, r.Near("DF_ARMORY_CRATE"));
        r.Loot(1, LootAction::Search, "DF_ARMORY_CRATE");
        r.wi->ReportGunfire(1, {1500, 500, 0});
        r.Step(400);
        llworld::StateHasher h;
        h.Add(r.wi->StateHash());
        h.Add(r.sim->StateHash());
        h.Add(r.loot->StateHash());
        return h.Value();
    };
    CHECK_EQ(run(), run());
}
