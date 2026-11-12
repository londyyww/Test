// Mission world simulation: scheduling tiers, autonomous activity, events,
// lifecycle, determinism and bounded work.
#include "WsTestHelpers.h"

using namespace wstest;
using namespace llsim;

namespace {

const Content& Relay() {
    static Content c = Load({"dead_frequency_relay_station.ini"});
    return c;
}

const Vec3 kRelaySite{200000, 200000, 0};
constexpr Tick kMin = 1800;  // ticks per minute @30 Hz

struct Sim {
    WorldSimulation w;
    SimRecorder rec;
    explicit Sim(const SimConfig& cfg, std::uint64_t seed = 42) : w(cfg) {
        for (const auto& e : w.Errors()) std::printf("    sim: %s\n", e.c_str());
        w.SetObserver(&rec);
        w.BeginRaid(seed);
    }
    std::uint32_t E(const char* id) const { return w.Find(id); }
    EntityState S(const char* id) const { return w.StateOf(E(id)); }
};

}  // namespace

AIS_TEST(Sim_ScheduledUpdatesFollowTierIntervals) {
    Sim s(Relay().sim);
    CHECK(s.w.IsValid());
    s.w.AdvanceTo(6000);
    // No players anywhere: everything is Low tier (600-tick interval).
    for (std::uint32_t i = 0; i < s.w.EntityCount(); ++i) {
        const EntityView v = s.w.View(i);
        CHECK(v.tier == Tier::Low);
        CHECK_MSG(v.updates >= 9 && v.updates <= 11, v.def->id);
        CHECK(s.w.Now() - v.lastUpdate <= 600);
    }
    CHECK_EQ(s.w.Stats().updatesByTier[0], 0u);
    CHECK(s.w.Stats().updatesByTier[2] <= s.w.EntityCount() * 11);
}

AIS_TEST(Sim_PriorityTiersFollowPlayersObservationAndThreats) {
    Sim s(Relay().sim);
    s.w.SetPlayers({kRelaySite + Vec3{1000, 0, 0}});
    s.w.AdvanceTo(30);
    CHECK(s.w.AreaTier(s.w.FindArea("RLY_SITE")) == Tier::High);
    CHECK(s.w.AreaTier(s.w.FindArea("RLY_CP_EAST")) == Tier::Medium);  // 6 km.. no: 60 m away, inside Medium radius
    CHECK(s.w.TierOf(s.E("RLY_GEN")) == Tier::High);
    const std::uint32_t before = s.w.View(s.E("RLY_GEN")).updates;
    s.w.AdvanceTo(330);
    // High tier: every 3 ticks.
    CHECK_NEAR(s.w.View(s.E("RLY_GEN")).updates - before, 100, 2);
    // Players leave: back to Low after the next relevance pass.
    s.w.SetPlayers({});
    s.w.AdvanceTo(360);
    CHECK(s.w.TierOf(s.E("RLY_GEN")) == Tier::Low);
    // A player observing a distant entity promotes just that entity, temporarily.
    s.w.ReportObserved(s.E("RLY_COMMS"));
    CHECK(s.w.TierOf(s.E("RLY_COMMS")) == Tier::High);
    CHECK(s.w.TierOf(s.E("RLY_GEN")) == Tier::Low);
    s.w.AdvanceTo(360 + 300 + 30);
    CHECK(s.w.TierOf(s.E("RLY_COMMS")) == Tier::Low);
    CHECK(s.rec.Count(SimEventType::TierChanged) >= 4);
}

AIS_TEST(Sim_GeneratorFuelConsumptionLowFuelAndBackup) {
    Sim s(Relay().sim);
    const std::uint32_t gen = s.E("RLY_GEN");
    CHECK_NEAR(s.w.View(gen).fuelSeconds, 240.0, 1e-9);
    s.w.AdvanceTo(kMin);
    CHECK_NEAR(s.w.View(gen).fuelSeconds, 180.0, 1e-6);  // evaluated analytically, no per-tick integration
    CHECK(s.S("RLY_GEN") == EntityState::Active);
    s.w.AdvanceTo(5399);
    CHECK(s.S("RLY_GEN") == EntityState::Active);
    s.w.AdvanceTo(5400);  // 60 s left
    CHECK(s.S("RLY_GEN") == EntityState::Degraded);
    CHECK_EQ(s.rec.Count(SimEventType::FuelLow, gen), 1u);
    s.w.AdvanceTo(7200);
    CHECK(s.S("RLY_GEN") == EntityState::Offline);
    CHECK_EQ(s.w.View(gen).fuelSeconds, 0.0);
    CHECK(s.S("RLY_BACKUP") == EntityState::Standby);
    s.w.AdvanceTo(7290);  // backup start delay
    CHECK(s.S("RLY_BACKUP") == EntityState::Active);
    CHECK_EQ(s.rec.Count(SimEventType::BackupStarted), 1u);
    s.w.AdvanceTo(7290 + 300);
    CHECK_NEAR(s.w.View(s.E("RLY_BACKUP")).fuelSeconds, 1800.0 - 10.0, 1e-6);
}

AIS_TEST(Sim_PowerStateChangesPropagateThroughDependencies) {
    Sim s(Relay().sim);
    CHECK(s.S("RLY_GRID") == EntityState::Nominal);
    s.w.AdvanceTo(7200);
    CHECK(s.S("RLY_GRID") == EntityState::Offline);
    CHECK(s.S("RLY_SECURITY") == EntityState::Offline);
    CHECK(s.S("RLY_COMMS") == EntityState::Offline);
    CHECK(s.S("RLY_FACILITY") == EntityState::Offline);
    CHECK(s.S("RLY_STORAGE") == EntityState::Offline);
    CHECK(s.S("RLY_PATROL") == EntityState::Interrupted);  // no comms, still patrolling on its own
    s.w.AdvanceTo(7290);
    CHECK(s.S("RLY_GRID") == EntityState::Degraded);  // backup only
    CHECK(s.S("RLY_COMMS") == EntityState::Nominal);
    CHECK(s.S("RLY_FACILITY") == EntityState::Degraded);
    CHECK(s.S("RLY_PATROL") == EntityState::Active);
}

AIS_TEST(Sim_PatrolTransitionsAreTierIndependent) {
    Sim near(Relay().sim);
    Sim far(Relay().sim);
    near.w.SetPlayers({kRelaySite, kRelaySite + Vec3{6000, 0, 0}, kRelaySite - Vec3{6000, 0, 0}});
    const std::uint32_t g = near.E("RLY_PATROL");
    // Route CP_EAST -> SITE, 900 travel + 600 dwell per leg (cycle 3000 ticks).
    std::vector<std::pair<Tick, std::string>> expect = {{100, "RLY_CP_EAST"}, {1000, "RLY_CP_EAST"}, {1600, "RLY_SITE"}, {2500, "RLY_SITE"}, {3100, "RLY_CP_EAST"}};
    for (const auto& [t, area] : expect) {
        near.w.AdvanceTo(t);
        CHECK_MSG(near.w.View(g).area == near.w.FindArea(area.c_str()), area);
    }
    CHECK(near.rec.directives.size() >= 5);
    CHECK(near.rec.directives[0].kind == DirectiveKind::MoveTo);
    CHECK(near.rec.directives[1].kind == DirectiveKind::Hold);
    CHECK(!near.rec.directives[0].central);
    // Low tier sees the same answer whenever it is asked.
    far.w.AdvanceTo(3100);
    far.w.ReportObserved(g);  // promotion = immediate catch-up update
    far.w.Step();
    near.w.Step();
    CHECK_EQ(far.w.View(g).area, near.w.View(g).area);
    CHECK(far.w.View(g).updates < near.w.View(g).updates / 5);
}

AIS_TEST(Sim_ScheduledRetaskAndCommunicationInterruption) {
    {
        Sim s(Relay().sim);
        s.w.AdvanceTo(3600);  // shift change delivered over a working link
        CHECK_EQ(s.rec.Count(SimEventType::ScheduleFired), 1u);
        s.w.AdvanceTo(3700);
        s.w.ReportObserved(s.E("RLY_PATROL"));
        s.w.Step();
        CHECK(s.w.View(s.E("RLY_PATROL")).area == s.w.FindArea("RLY_CP_WEST"));
    }
    Sim s(Relay().sim);
    const std::uint32_t g = s.E("RLY_PATROL");
    s.w.AdvanceTo(3000);
    s.w.NotifySignalForEntity(SignalKind::CommsLost, s.E("RLY_COMMS"));
    CHECK(s.S("RLY_COMMS") == EntityState::Interrupted);
    CHECK(s.S("RLY_PATROL") == EntityState::Interrupted);
    CHECK(!s.w.View(g).commsConnected);
    s.w.AdvanceTo(3700);
    // Central order could not reach the group: it is held, the old route continues.
    CHECK_EQ(s.w.View(g).pendingInstructions, 1u);
    CHECK_EQ(s.rec.Count(SimEventType::InstructionQueued, g), 1u);
    s.w.ReportObserved(g);
    s.w.Step();
    CHECK(s.w.View(g).area == s.w.FindArea("RLY_CP_EAST"));
    s.w.NotifySignalForEntity(SignalKind::CommsRestored, s.E("RLY_COMMS"));
    CHECK(s.S("RLY_PATROL") == EntityState::Active);
    CHECK_EQ(s.w.View(g).pendingInstructions, 0u);
    CHECK_EQ(s.rec.Count(SimEventType::InstructionDelivered, g), 1u);
    CHECK(s.w.View(g).area == s.w.FindArea("RLY_CP_WEST"));
    // The pending queue is bounded: overflow drops the oldest order.
    s.w.NotifySignalForEntity(SignalKind::CommsLost, s.E("RLY_COMMS"));
    for (int k = 0; k < 7; ++k) s.w.NotifySignal(SignalKind::AlarmRaised, "RLY_FACILITY");
    CHECK(s.w.View(g).pendingInstructions <= s.w.Config().maxPendingInstructions);
}

AIS_TEST(Sim_AlarmDrivesLockdownAndPatrolResponse) {
    Sim s(Relay().sim);
    const std::uint32_t g = s.E("RLY_PATROL");
    s.w.AdvanceTo(10799);
    CHECK(s.S("RLY_FACILITY") == EntityState::Degraded);
    s.w.AdvanceTo(10800);
    CHECK(s.S("RLY_SECURITY") == EntityState::Alarm);
    CHECK(s.S("RLY_FACILITY") == EntityState::Lockdown);
    CHECK(s.S("RLY_STORAGE") == EntityState::Lockdown);
    CHECK(s.S("RLY_PATROL") == EntityState::Responding);
    PatrolDirective d;
    CHECK(s.w.LastDirective(g, d));
    CHECK(d.kind == DirectiveKind::Respond);
    CHECK(d.central);  // too far to hear it: central command sent it
    CHECK(d.area == s.w.FindArea("RLY_SITE"));
    // Threatened entities are promoted even with nobody around.
    CHECK(s.w.TierOf(s.E("RLY_FACILITY")) == Tier::Medium);
    // Rule fires once.
    s.w.NotifySignal(SignalKind::AlarmCleared, "RLY_SECURITY");
    s.w.NotifySignal(SignalKind::AlarmRaised, "RLY_SECURITY");
    CHECK_EQ(s.w.Stats().rulesFired, 1u);
    // Response ends after travel + hold; the group resumes its route.
    s.w.AdvanceTo(10800 + 900 + 900 + 1);
    CHECK(s.S("RLY_PATROL") == EntityState::Active);
    CHECK(s.w.LastDirective(g, d) && d.kind != DirectiveKind::Respond);
}

AIS_TEST(Sim_DisturbanceAttractsOnlyPatrolsInEarshot) {
    Sim s(Relay().sim);
    s.w.AdvanceTo(1000);  // patrol holding at CP_EAST
    s.w.ReportDisturbance(kRelaySite - Vec3{6000, 0, 0}, 85.0);  // 120 m away: out of earshot
    CHECK(s.S("RLY_PATROL") == EntityState::Active);
    s.w.ReportDisturbance(kRelaySite + Vec3{6000, 1500, 0}, 40.0);  // quiet: radius shrinks to 1333 cm
    CHECK(s.S("RLY_PATROL") == EntityState::Active);
    s.w.ReportDisturbance(kRelaySite + Vec3{6000, 1500, 0}, 82.0);
    CHECK(s.S("RLY_PATROL") == EntityState::Responding);
    PatrolDirective d;
    CHECK(s.w.LastDirective(s.E("RLY_PATROL"), d) && d.kind == DirectiveKind::Respond && !d.central);
}

AIS_TEST(Sim_InactiveDistantFacilityNeedsNoFullRateUpdates) {
    Sim s(Relay().sim);
    s.w.AdvanceTo(12000);
    const std::uint64_t updates = s.w.Stats().Updates();
    const std::uint64_t fullRate = 12000ull * s.w.EntityCount();
    std::printf("    relay: %llu scheduled updates vs %llu at full rate\n", static_cast<unsigned long long>(updates), static_cast<unsigned long long>(fullRate));
    CHECK(updates * 200 < fullRate);  // >200x fewer updates than per-tick simulation
    // Yet all autonomous activity happened.
    CHECK(s.S("RLY_GEN") == EntityState::Offline);
    CHECK(s.S("RLY_BACKUP") == EntityState::Active);
    CHECK(s.S("RLY_FACILITY") == EntityState::Lockdown);
    // Players approach: state is exposed immediately and correctly.
    s.w.SetPlayers({kRelaySite});
    s.w.AdvanceTo(12016);
    CHECK(s.w.TierOf(s.E("RLY_GEN")) == Tier::High);
    CHECK_NEAR(s.w.View(s.E("RLY_BACKUP")).fuelSeconds, 1800.0 - (12016.0 - 7290.0) / 30.0, 1e-6);
}

AIS_TEST(Sim_WorldResetAndLifecycle) {
    Sim fresh(Relay().sim);
    const std::uint64_t initial = fresh.w.StateHash();
    Sim s(Relay().sim);
    s.w.AdvanceTo(11000);
    CHECK(s.S("RLY_FACILITY") == EntityState::Lockdown);
    // Temporary mission override reverts by itself.
    CHECK(s.w.ApplyMissionOverride(s.E("RLY_COMMS"), EntityState::Interrupted, 300));
    CHECK(s.S("RLY_COMMS") == EntityState::Interrupted);
    s.w.AdvanceTo(11301);
    CHECK(s.S("RLY_COMMS") == EntityState::Nominal);
    CHECK(!s.w.ApplyMissionOverride(s.E("RLY_COMMS"), EntityState::Responding, 0));
    // Completion freezes the world.
    s.w.CompleteMission();
    const std::uint64_t frozen = s.w.StateHash();
    s.w.AdvanceTo(20000);
    s.w.Step();
    CHECK(s.w.Phase() == RaidPhase::Completed);
    CHECK_EQ(s.w.StateHash(), frozen);
    // Persistent facility state is only exported for entities that opted in.
    const auto records = s.w.ExportPersistentState();
    CHECK_EQ(records.size(), 2u);
    // Reset: back to the authored initial state.
    s.w.ResetWorld();
    CHECK(s.w.Phase() == RaidPhase::Initial);
    CHECK(s.S("RLY_GEN") == EntityState::Active);
    s.w.BeginRaid(42);
    CHECK_EQ(s.w.StateHash(), initial);
    // Importing the persisted state: the drained primary stays empty next raid.
    Sim next(Relay().sim);
    next.w.BeginRaid(43, records);
    CHECK(next.S("RLY_GEN") == EntityState::Offline);
    CHECK_EQ(next.w.View(next.E("RLY_GEN")).fuelSeconds, 0.0);
}

AIS_TEST(Sim_DeterministicReplay) {
    auto run = [](std::vector<std::uint64_t>& hashes, std::vector<SimEvent>& log) {
        Sim s(Relay().sim, 777);
        for (Tick t = 1; t <= 12000; ++t) {
            if (t == 2000) s.w.SetPlayers({kRelaySite + Vec3{3000, 0, 0}});
            if (t == 4000) s.w.ReportDisturbance(kRelaySite, 90.0);
            if (t == 6000) s.w.NotifySignalForEntity(SignalKind::CommsLost, s.E("RLY_COMMS"));
            if (t == 9000) s.w.SetPlayers({});
            s.w.Step();
            if (t % 1000 == 0) hashes.push_back(s.w.StateHash());
        }
        log = s.w.RecentEvents();
    };
    std::vector<std::uint64_t> h1;
    std::vector<std::uint64_t> h2;
    std::vector<SimEvent> l1;
    std::vector<SimEvent> l2;
    run(h1, l1);
    run(h2, l2);
    CHECK(h1 == h2);
    CHECK_EQ(l1.size(), l2.size());
    bool same = l1.size() == l2.size();
    for (std::size_t i = 0; same && i < l1.size(); ++i) same = l1[i].tick == l2[i].tick && l1[i].type == l2[i].type && l1[i].entity == l2[i].entity;
    CHECK(same);
}

AIS_TEST(Sim_BoundedQueuesAndPerStepBudgets) {
    // 400 abstract facilities all next to the players with a tiny update budget.
    std::string text = "[content]\nversion = 1\n[simulation]\nmax_updates_per_step = 20\nqueue_capacity = 900\nevent_log_capacity = 64\n"
                       "[area.A]\ncenter = 0,0,0\nradius_cm = 1000\n";
    for (int i = 0; i < 400; ++i) text += "[entity.F" + std::to_string(i) + "]\nkind = Facility\narea = A\n";
    Content c;
    CHECK(Problems(text, &c).empty());
    Sim s(c.sim);
    s.w.SetPlayers({Vec3{0, 0, 0}});
    s.w.AdvanceTo(600);
    CHECK(s.w.Stats().deferredUpdates > 0);
    CHECK(s.w.Stats().Updates() <= 600u * 20u);
    CHECK_EQ(s.w.Stats().queueOverflows, 0u);
    CHECK(s.w.QueueSize() <= 900u * 2u);
    CHECK(s.w.RecentEvents().size() <= 64u);
    // Fairness: nobody starves even when the budget is saturated.
    for (std::uint32_t i = 0; i < s.w.EntityCount(); ++i) CHECK(s.w.View(i).updates >= 10);
}

AIS_TEST(Sim_HazardZoneCycleAffectsFacility) {
    const Content c = Load({"medical_facility.ini"});
    Sim s(c.sim);
    CHECK(s.S("MED_FACILITY") == EntityState::Nominal);
    s.w.AdvanceTo(2700);
    CHECK(s.S("MED_CONTAMINATION") == EntityState::Active);
    CHECK(s.S("MED_FACILITY") == EntityState::Alarm);
    s.w.AdvanceTo(3600);
    CHECK(s.S("MED_CONTAMINATION") == EntityState::Nominal);
    CHECK(s.S("MED_FACILITY") == EntityState::Nominal);
}
