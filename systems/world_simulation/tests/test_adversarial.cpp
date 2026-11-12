// Adversarial tests for the portable cores (loot, containers, world simulation,
// content loader): hostile input, duplicates, ordering, flooding, boundaries.
#include <cmath>
#include <limits>

#include "WsTestHelpers.h"

using namespace wstest;

namespace {

const Content& AllContent() {
    static Content c = Load({kAllLocations[0], kAllLocations[1], kAllLocations[2], kAllLocations[3], kAllLocations[4]});
    return c;
}

const std::string kMin = "[content]\nversion = 1\n[area.A]\ncenter = 0,0,0\nradius_cm = 500\n";

struct Svc {
    Access access;
    MemInventory inv;
    Recorder rec;
    llloot::ContainerService s;
    std::map<llloot::PlayerId, std::uint32_t> seq;
    llloot::Tick now = 0;
    Svc() : s(AllContent().loot, Catalog()) {
        s.SetAccessPort(&access);
        s.SetGrantPort(&inv);
        s.SetObserver(&rec);
        s.SetSoundSink(&rec);
        s.BeginRaid(31337);
    }
    std::uint32_t C(const char* id) const { return s.Find(id); }
    llworld::Vec3 At(const char* id) const { return s.Def(C(id)).position + llworld::Vec3{40, 0, 0}; }
    llloot::LootResultCode Do(llloot::PlayerId p, llloot::LootAction a, std::uint32_t c, std::uint32_t slot = 0) {
        return s.Submit(llloot::LootRequest{p, ++seq[p], c, a, slot});
    }
    void Run(llloot::Tick t) {
        for (llloot::Tick i = 0; i < t; ++i) s.Step(++now);
    }
};

class FakeInfra final : public llsim::IWorldInfrastructurePort {
public:
    llsim::InfraSample SampleNode(std::string_view n) const override {
        auto it = nodes.find(std::string(n));
        return it == nodes.end() ? llsim::InfraSample{} : it->second;
    }
    bool HazardActive(std::string_view h) const override { return hazards.count(std::string(h)) != 0; }
    std::map<std::string, llsim::InfraSample> nodes;
    std::set<std::string> hazards;
};

}  // namespace

// ---------------------------------------------------------------- configuration boundaries

AIS_TEST(Adv_ExtremeTickValuesAreRejectedByValidation) {
    const std::string u64max = "18446744073709551615";
    CHECK(AnyContains(Problems(kMin + "[entity.F]\nkind = Facility\narea = A\n[schedule.S]\nat_ticks = 10\nrepeat_ticks = " + u64max +
                               "\ndo = set:F=Alarm\n"),
                      "repeat_ticks"));
    CHECK(AnyContains(Problems(kMin + "[entity.F]\nkind = Facility\narea = A\n[schedule.S]\nat_ticks = " + u64max + "\ndo = set:F=Alarm\n"),
                      "at_ticks"));
    CHECK(AnyContains(Problems(kMin + "[tier.Low]\ninterval_ticks = 9223372036854775808\n"), "interval_ticks"));
    CHECK(AnyContains(Problems(kMin + "[entity.P]\nkind = PatrolGroup\narea = A\nroute = A\ntravel_ticks = " + u64max + "\n"), "travel_ticks"));
    CHECK(AnyContains(Problems(kMin + "[entity.F]\nkind = Facility\narea = A\n[rule.R]\non = state:F=Alarm\ndo = set:F=Lockdown\ndelay_ticks = " + u64max +
                               "\n"),
                      "delay_ticks"));
    CHECK(AnyContains(Problems(kMin + "[entity.G]\nkind = Generator\narea = A\nfuel_seconds = 1e30\n"), "fuel_seconds"));
    CHECK(AnyContains(Problems(kMin + "[entity.H]\nkind = HazardZone\narea = A\nactive_ticks = " + u64max + "\ndormant_ticks = 5\n"), "active_ticks"));
    CHECK(AnyContains(Problems(kMin + "[container_type.T]\ncategory = Shelf\nsearch_ticks = " + u64max + "\n"), "search_ticks"));
    CHECK(AnyContains(Problems(kMin + "[simulation]\nobserved_window_ticks = " + u64max + "\n"), "observed_window_ticks"));
}

AIS_TEST(Adv_HugeLootWeightsCannotOverflowTheTotal) {
    const auto p = Problems(kMin + "[table.T]\nentry = mech.bearing_set | weight=1e308\nentry = mech.spring_pack | weight=1e308\n");
    CHECK(AnyContains(p, "weight"));
}

AIS_TEST(Adv_MalformedAndMissingConfiguration) {
    llloot::LootConfig loot;
    llsim::SimConfig sim;
    std::vector<std::string> errors;
    CHECK(!llcontent::LoadContentFile(Data("does/not/exist.ini"), loot, sim, errors));
    CHECK(AnyContains(errors, "cannot read file"));
    const char* inputs[] = {
        "",
        "[content",
        "version = 1",
        "[content]\nversion = one\n",
        "[content]\nversion = 1\n[entity.X]\nkind\n",
        "[content]\nversion = 1\n[]\n",
        "[content]\nversion = 1\n[table.T]\nentry = | weight=3\n",
        "[content]\nversion = 1\n[table.T]\nentry = a | weight=\n",
        "[content]\nversion = 1\n[table.T]\nentry = a | qty=9-\n",
        "[content]\nversion = 1\n[container.C]\nposition = 1,2\naccess = work:abc\n",
        "[content]\nversion = 1\n[entity.E]\nkind = Generator\narea = A\nfuel_seconds = nan\n",
        "[content]\nversion = 1\n[entity.E]\nkind = Generator\narea = A\nburn_rate = inf\n",
        "[content]\nversion = 1\n[location.L]\nrarity_weights = Common\n",
        "\x01\x02\xff\xfe[\x00]",
    };
    for (const char* in : inputs) {
        Content c;
        std::vector<std::string> e;
        const bool ok = llcontent::LoadContentText(in, "fuzz", c.loot, c.sim, e);
        CHECK_MSG(!ok && !e.empty(), in);
    }
    // A long garbage line and many sections stay bounded and diagnosable.
    std::string big = "[content]\nversion = 1\n";
    big += "[table.T]\nentry = " + std::string(100000, 'x') + " | weight=1\n";
    Content c;
    std::vector<std::string> e;
    llcontent::LoadContentText(big, "big", c.loot, c.sim, e);
    for (const auto& m : llloot::ValidateLoot(c.loot, Catalog())) e.push_back(m);
    CHECK(AnyContains(e, "item id not found"));
}

AIS_TEST(Adv_InvalidReferencesEverywhere) {
    const auto p = Problems(kMin + "[entity.P]\nkind = PatrolGroup\narea = NOPE\nroute = A\ncomms = MISSING\n"
                                   "[entity.G]\nkind = Generator\narea = A\nbackup_for = GHOST\n"
                                   "[rule.R]\non = signal:Teleport\ndo = set:P=Active\n"
                                   "[schedule.S]\nat_ticks = 5\ndo = retask:P=A+NOWHERE\n");
    CHECK(AnyContains(p, "unknown area 'NOPE'"));
    CHECK(AnyContains(p, "comms references missing entity 'MISSING'"));
    CHECK(AnyContains(p, "backup_for references missing entity 'GHOST'"));
    CHECK(AnyContains(p, "unknown signal 'Teleport'"));
    CHECK(AnyContains(p, "unknown area 'NOWHERE'"));
}

// ---------------------------------------------------------------- containers: identity, duplicates, flooding

AIS_TEST(Adv_InvalidPlayerAndContainerIdentifiers) {
    Svc v;
    CHECK(!v.s.ConnectPlayer(0, 1, {0, 0, 0}));
    CHECK(!v.s.ConnectPlayer(1, 101, {std::nan(""), 0, 0}));
    CHECK(v.s.ConnectPlayer(1, 101, v.At("DF_CP_NORTH_LOCKER")));
    CHECK(!v.s.UpdatePlayerPosition(1, {0, std::numeric_limits<double>::infinity(), 0}));
    using R = llloot::LootResultCode;
    CHECK(v.s.Submit({0, 1, 0, llloot::LootAction::Search, 0}) == R::RejectedUnknownPlayer);
    CHECK(v.s.Submit({0xFFFFFFFFu, 1, 0, llloot::LootAction::Search, 0}) == R::RejectedUnknownPlayer);
    CHECK(v.Do(1, llloot::LootAction::Search, 0xFFFFFFFFu) == R::RejectedUnknownContainer);
    CHECK(v.Do(1, llloot::LootAction::Search, static_cast<std::uint32_t>(v.s.Count())) == R::RejectedUnknownContainer);
    CHECK(v.Do(1, llloot::LootAction::TakeItem, v.C("DF_CP_NORTH_LOCKER"), 0xFFFFFFFFu) == R::RejectedNotSearched);
    CHECK(v.s.Submit({1, 999, v.C("DF_CP_NORTH_LOCKER"), static_cast<llloot::LootAction>(200), 0}) == R::RejectedNotApplicable);
    // Queries with bad ids are safe.
    CHECK(v.s.Contents(0xFFFFFFFFu, 1) == nullptr);
    CHECK(v.s.CheckAccess(0xFFFFFFFFu, 1) == R::RejectedUnknownContainer);
    CHECK(v.s.Slot(0xFFFFFFFFu, 1, 0).item == nullptr);
    CHECK_EQ(v.s.Claimant(0xFFFFFFFFu, 1), 0u);
}

AIS_TEST(Adv_PlayersRotateBetweenRaids) {
    Svc v;
    for (llloot::PlayerId p = 1; p <= 4; ++p) CHECK(v.s.ConnectPlayer(p, 100 + p, v.At("DF_CP_NORTH_LOCKER")));
    for (llloot::PlayerId p = 1; p <= 4; ++p) v.s.DisconnectPlayer(p);  // squad extracted
    v.s.EndRaid();
    v.s.BeginRaid(2);
    // A long-lived server hosts different squads in later raids.
    for (llloot::PlayerId p = 5; p <= 8; ++p) CHECK(v.s.ConnectPlayer(p, 100 + p, v.At("DF_CP_NORTH_LOCKER")));
    CHECK(v.Do(5, llloot::LootAction::Search, v.C("DF_CP_NORTH_LOCKER")) == llloot::LootResultCode::Started);
}

AIS_TEST(Adv_ReconnectWithNewPlayerIdKeepsOwnershipAndReleasesLocks) {
    Svc v;
    const std::uint32_t flat = v.C("RES_FLAT2_CUPBOARD");
    CHECK(v.s.ConnectPlayer(1, 101, v.At("RES_FLAT2_CUPBOARD")));
    CHECK(v.Do(1, llloot::LootAction::Search, flat) == llloot::LootResultCode::Started);
    v.Run(100);
    const llloot::LootResult* before = v.s.Contents(flat, 1);
    CHECK(before != nullptr);
    const std::uint64_t h = before != nullptr ? before->Hash() : 0;
    // Mid-search disconnect of a shared container leaves no claim behind.
    const std::uint32_t locker = v.C("RES_CARETAKER_LOCKER");
    v.s.UpdatePlayerPosition(1, v.At("RES_CARETAKER_LOCKER"));
    CHECK(v.Do(1, llloot::LootAction::Search, locker) == llloot::LootResultCode::Started);
    v.s.DisconnectPlayer(1);
    CHECK_EQ(v.s.Claimant(locker, 0), 0u);
    // The engine hands out a new PlayerId on reconnect; the party slot (recipient key) is stable.
    CHECK(v.s.ConnectPlayer(9, 101, v.At("RES_FLAT2_CUPBOARD")));
    const llloot::LootResult* after = v.s.Contents(flat, 9);
    CHECK(after != nullptr && after->Hash() == h);
    CHECK_MSG(v.Do(9, llloot::LootAction::Search, flat) == llloot::LootResultCode::RejectedAlreadySearched, llloot::ToString(v.s.Submit({9, 777, flat, llloot::LootAction::Search, 0})));
    // The old id is gone: it cannot act any more.
    CHECK(v.s.Submit({1, 5000, flat, llloot::LootAction::TakeItem, 0}) == llloot::LootResultCode::RejectedUnknownPlayer);
    // A connected player's key cannot be hijacked by another id.
    CHECK(!v.s.ConnectPlayer(10, 101, v.At("RES_FLAT2_CUPBOARD")));
}

AIS_TEST(Adv_SequenceReplayAfterRingWrapCannotDuplicateGrants) {
    Svc v;
    const std::uint32_t c = v.C("MED_TREATMENT_CABINET");
    CHECK(v.s.ConnectPlayer(1, 101, v.At("MED_TREATMENT_CABINET")));
    CHECK(v.s.Submit({1, 1, c, llloot::LootAction::Search, 0}) == llloot::LootResultCode::Started);
    v.Run(100);
    CHECK(v.s.Submit({1, 2, c, llloot::LootAction::TakeItem, 0}) == llloot::LootResultCode::Accepted);
    // Push the original sequence numbers out of the 64-entry ring, then replay them.
    for (std::uint32_t s = 100; s < 300; ++s) v.s.Submit({1, s, c, llloot::LootAction::Discover, 0});
    CHECK(v.s.Submit({1, 1, c, llloot::LootAction::Search, 0}) == llloot::LootResultCode::RejectedAlreadySearched);
    CHECK(v.s.Submit({1, 2, c, llloot::LootAction::TakeItem, 0}) == llloot::LootResultCode::RejectedSlotTaken);
    CHECK_EQ(v.inv.applied.size(), 1u);
    CHECK_EQ(v.inv.calls, 1);
    CHECK_EQ(v.s.GetStats().generated, 1u);
}

AIS_TEST(Adv_RequestFloodStaysBounded) {
    Svc v;
    for (llloot::PlayerId p = 1; p <= 4; ++p) CHECK(v.s.ConnectPlayer(p, 100 + p, v.At("DF_CP_NORTH_LOCKER")));
    const std::uint32_t c = v.C("DF_CP_NORTH_LOCKER");
    std::size_t started = 0;
    for (int i = 0; i < 50000; ++i) {
        const llloot::PlayerId p = static_cast<llloot::PlayerId>(1 + i % 4);
        // Every action except CancelSearch (a player may legitimately cancel their own search).
        const llloot::LootAction actions[] = {llloot::LootAction::Search, llloot::LootAction::Discover, llloot::LootAction::ClearObstruction, llloot::LootAction::TakeItem};
        started += v.Do(p, actions[i % 4], c, static_cast<std::uint32_t>(i % 7)) == llloot::LootResultCode::Started ? 1u : 0u;
        if (i % 1000 == 0) v.Run(1);
    }
    v.Run(200);
    // Exactly one generation; every grant unique; sounds bounded by accepted starts.
    CHECK_EQ(v.s.GetStats().generated, 1u);
    std::set<std::uint64_t> ids;
    for (const auto& [id, g] : v.inv.applied) ids.insert(id);
    CHECK_EQ(ids.size(), v.inv.applied.size());
    const llloot::LootResult* r = v.s.Contents(c, 1);
    CHECK(r != nullptr && v.inv.applied.size() <= r->items.size());
    CHECK(v.rec.sounds.size() <= 2 * started);
}

AIS_TEST(Adv_SimultaneousTakesFromSharedContainer) {
    Svc v;
    const std::uint32_t c = v.C("DF_CP_NORTH_STASH");
    for (llloot::PlayerId p = 1; p <= 4; ++p) CHECK(v.s.ConnectPlayer(p, 100 + p, v.At("DF_CP_NORTH_STASH")));
    CHECK(v.Do(1, llloot::LootAction::Discover, c) == llloot::LootResultCode::Accepted);
    CHECK(v.Do(2, llloot::LootAction::Search, c) == llloot::LootResultCode::Started);
    v.Run(100);
    const llloot::LootResult* r = v.s.Contents(c, 3);
    CHECK(r != nullptr);
    if (r == nullptr) return;
    // Every player hammers every slot in the same tick.
    for (int round = 0; round < 3; ++round) {
        for (std::uint32_t slot = 0; slot < r->items.size(); ++slot) {
            for (llloot::PlayerId p = 1; p <= 4; ++p) v.Do(p, llloot::LootAction::TakeItem, c, slot);
        }
    }
    CHECK_EQ(v.inv.applied.size(), r->items.size());
    std::size_t received = 0;
    for (const auto& [p, list] : v.inv.received) received += list.size();
    CHECK_EQ(received, r->items.size());
}

AIS_TEST(Adv_UnavailableInventoryNeverDuplicatesAndKeepsReservation) {
    Svc v;
    const std::uint32_t c = v.C("MED_TREATMENT_CABINET");
    CHECK(v.s.ConnectPlayer(1, 101, v.At("MED_TREATMENT_CABINET")));
    CHECK(v.s.ConnectPlayer(2, 102, v.At("MED_TREATMENT_CABINET")));
    v.Do(1, llloot::LootAction::Search, c);
    v.Run(100);
    v.inv.failNext = 1000000;  // inventory service down
    CHECK(v.Do(1, llloot::LootAction::TakeItem, c, 0) == llloot::LootResultCode::Started);
    v.s.DisconnectPlayer(1);
    v.Run(500);  // bounded retries per step
    CHECK(v.inv.calls <= 1 + 500 * 8);
    CHECK(v.Do(2, llloot::LootAction::TakeItem, c, 0) == llloot::LootResultCode::RejectedSlotTaken);
    CHECK_EQ(v.s.PendingGrants(), 1u);
    v.inv.failNext = 0;
    v.Run(1);
    CHECK_EQ(v.s.PendingGrants(), 0u);
    CHECK_EQ(v.inv.applied.size(), 1u);
}

AIS_TEST(Adv_ContainerServiceDeterministicReplay) {
    auto run = []() {
        Svc v;
        for (llloot::PlayerId p = 1; p <= 3; ++p) v.s.ConnectPlayer(p, 100 + p, v.At("RES_FLAT2_CUPBOARD"));
        for (int i = 0; i < 3000; ++i) {
            const llloot::PlayerId p = static_cast<llloot::PlayerId>(1 + (i * 7) % 3);
            const std::uint32_t c = static_cast<std::uint32_t>(static_cast<std::size_t>(i) * 13u % v.s.Count());
            if (i % 50 == 0) v.s.UpdatePlayerPosition(p, v.s.Def(c).position);
            v.Do(p, static_cast<llloot::LootAction>((i * 3) % 5), c, static_cast<std::uint32_t>(i % 4));
            v.Run(static_cast<llloot::Tick>(1 + i % 3));
        }
        return std::make_pair(v.s.StateHash(), v.inv.applied.size());
    };
    const auto a = run();
    const auto b = run();
    CHECK_EQ(a.first, b.first);
    CHECK_EQ(a.second, b.second);
}

// ---------------------------------------------------------------- world simulation: signals, ordering, capacity

AIS_TEST(Adv_StaleOrOutOfOrderInfrastructureSignalsFollowTheTruth) {
    Content c;
    CHECK(Problems(kMin + "[entity.G]\nkind = Generator\narea = A\nbind = node:GEN\n[entity.N]\nkind = PowerNetwork\narea = A\nbind = node:BUS\n"
                          "depends_on = G\n",
                   &c)
              .empty());
    FakeInfra infra;
    infra.nodes["GEN"] = llsim::InfraSample{true, true, true, false};
    infra.nodes["BUS"] = llsim::InfraSample{true, true, false, false};
    llsim::WorldSimulation w(c.sim);
    w.SetInfrastructurePort(&infra);
    w.BeginRaid(1);
    w.NotifyBoundChanged("GEN");
    w.NotifyBoundChanged("BUS");
    CHECK(w.StateOf(w.Find("G")) == llsim::EntityState::Active);
    // Restored then a stale "lost" arrives late: the generator is in fact running.
    w.NotifySignal(llsim::SignalKind::PowerRestored, "GEN");
    w.NotifySignal(llsim::SignalKind::PowerLost, "GEN");
    CHECK(w.StateOf(w.Find("G")) == llsim::EntityState::Active);
    // Duplicate delivery of a real loss is idempotent.
    infra.nodes["GEN"].running = false;
    infra.nodes["GEN"].powered = false;
    infra.nodes["BUS"].powered = false;
    for (int i = 0; i < 50; ++i) w.NotifySignal(llsim::SignalKind::PowerLost, "GEN");
    w.NotifyBoundChanged("BUS");
    CHECK(w.StateOf(w.Find("G")) == llsim::EntityState::Offline);
    CHECK(w.StateOf(w.Find("N")) == llsim::EntityState::Offline);
    std::size_t genChanges = 0;
    for (const auto& e : w.RecentEvents()) genChanges += e.type == llsim::SimEventType::StateChanged && e.entity == w.Find("G") ? 1u : 0u;
    CHECK_EQ(genChanges, 1u);
}

AIS_TEST(Adv_DuplicateAndFloodedSignalsAreBounded) {
    Content c;
    CHECK(Problems(kMin + "[simulation]\nevent_log_capacity = 32\nmax_transitions_per_step = 16\n"
                          "[entity.S]\nkind = SecuritySystem\narea = A\n[entity.F]\nkind = Facility\narea = A\ndepends_on = S\n"
                          "[entity.C]\nkind = CommsStation\narea = A\n"
                          "[entity.P]\nkind = PatrolGroup\narea = A\nroute = A\ncomms = C\n"
                          "[rule.R]\non = state:S=Alarm\ndo = alert:F\n",
                   &c)
              .empty());
    llsim::WorldSimulation w(c.sim);
    SimRecorder rec;
    w.SetObserver(&rec);
    w.BeginRaid(3);
    w.NotifySignalForEntity(llsim::SignalKind::CommsLost, w.Find("C"));
    for (int i = 0; i < 20000; ++i) {
        w.NotifySignal(i % 2 == 0 ? llsim::SignalKind::AlarmRaised : llsim::SignalKind::AlarmCleared, "S");
        if (i % 100 == 0) w.Step();
    }
    CHECK(w.RecentEvents().size() <= 32u);
    CHECK(w.View(w.Find("P")).pendingInstructions <= c.sim.maxPendingInstructions);
    CHECK(w.QueueSize() <= 2 * c.sim.queueCapacity);
    CHECK_EQ(w.Stats().queueOverflows, 0u);
    // Unknown references and kinds are ignored, not crashes.
    w.NotifySignal(llsim::SignalKind::PowerLost, "");
    w.NotifySignal(static_cast<llsim::SignalKind>(250), "S");
    w.NotifySignalForEntity(llsim::SignalKind::AlarmRaised, 0xFFFFFFFFu);
    w.ReportObserved(0xFFFFFFFFu);
    CHECK(!w.ApplyMissionOverride(0xFFFFFFFFu, llsim::EntityState::Alarm, 5));
    w.ReportDisturbance({std::nan(""), 0, 0}, 90.0);
    w.ReportDisturbance({0, 0, 0}, std::numeric_limits<double>::infinity());
    w.SetPlayers({{std::nan(""), 0, 0}, {0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}, {4, 4, 4}});
    w.Step();
    CHECK(w.Phase() == llsim::RaidPhase::Active);
}

AIS_TEST(Adv_SchedulerCapacityExhaustionIsCountedNotFatal) {
    // Delayed rules re-armed by a flood exceed the timer queue: overflow is counted and the sim keeps running.
    std::string text = kMin + "[simulation]\nqueue_capacity = 40\n[entity.S]\nkind = SecuritySystem\narea = A\n";
    for (int i = 0; i < 4; ++i) text += "[entity.F" + std::to_string(i) + "]\nkind = Facility\narea = A\n";
    text += "[rule.D]\non = signal:Disturbance\ndo = signal:AlarmCleared@S\ndelay_ticks = 1000\n";
    Content c;
    CHECK(Problems(text, &c).empty());
    llsim::WorldSimulation w(c.sim);
    w.BeginRaid(1);
    for (int i = 0; i < 200; ++i) w.ReportDisturbance({0, 0, 0}, 50.0);
    CHECK(w.Stats().queueOverflows > 0u);
    CHECK(w.QueueSize() <= 80u);
    w.AdvanceTo(3000);
    CHECK(w.Phase() == llsim::RaidPhase::Active);
    for (std::uint32_t i = 0; i < w.EntityCount(); ++i) CHECK(w.View(i).updates > 0);
}

AIS_TEST(Adv_LongRaidsKeepAnalyticStateExact) {
    const Content c = Load({"dead_frequency_relay_station.ini"});
    llsim::WorldSimulation w(c.sim);
    w.BeginRaid(9);
    // ~28 simulated hours at Low tier.
    w.AdvanceTo(3000000);
    CHECK_EQ(w.Now(), 3000000u);
    const llsim::EntityView backup = w.View(w.Find("RLY_BACKUP"));
    CHECK(backup.state == llsim::EntityState::Offline);  // 1800 s of fuel long gone
    CHECK_EQ(backup.fuelSeconds, 0.0);
    // The lockdown never clears, so threatened entities stay at Medium (30 ticks); the rest stay Low.
    CHECK(w.Stats().Updates() < 3000000u / 30u * w.EntityCount() / 2u);
    // Patrol route arithmetic stays valid after millions of ticks.
    w.ReportObserved(w.Find("RLY_PATROL"));
    w.Step();
    CHECK(w.View(w.Find("RLY_PATROL")).area != llsim::kNone);
}
