// Squad coordination: reports, sharing, assignment, conflicts, decay, comms, autonomy.
#include <array>
#include <memory>
#include <string>

#include "TestFramework.h"
#include "llworld/Squad.h"

using namespace llworld;

namespace {

class FakeRelay final : public ISquadWorld {
public:
    bool up = true;
    bool RelayOperational(std::string_view) const override { return up; }
};

SquadConfig FourBots(bool withRelay = false) {
    SquadConfig c;
    c.roles = SquadConfig::DefaultRoles();
    RoleDef warden;
    warden.name = "WARDEN";
    warden.canInvestigate = false;
    warden.canDefend = true;
    warden.commsRangeCm = 2500;
    c.roles.push_back(warden);
    SquadDef s;
    s.id = "ALPHA";
    s.members = {{1, "SENTINEL", {{0, 0, 0}, {1000, 0, 0}}}, {2, "SENTINEL", {{0, 1000, 0}}}, {3, "WRAITH", {{-1000, 0, 0}}}, {4, "WARDEN", {{0, -1000, 0}}}};
    s.defendPoint = {0, -500, 0};
    if (withRelay) {
        s.relayNode = "RELAY";
        s.relayPosition = {0, 0, 0};
        s.relayRangeCm = 20000;
    }
    c.squads = {s};
    c.minPatrolFraction = 0.25;
    return c;
}

struct Rig {
    WorldEventBus bus{512};
    FakeRelay relay;
    SquadConfig cfg;
    std::unique_ptr<SquadCoordinator> sq;
    std::array<std::size_t, kWorldEventTypeCount> seen{};  // accumulated over the whole run
    Tick t = 0;
    explicit Rig(SquadConfig c) : cfg(std::move(c)) { sq = std::make_unique<SquadCoordinator>(cfg, &bus, &relay); }
    void Place(RobotId id, Vec3 p, CommsHealth comms = CommsHealth::Intact, bool alive = true) { sq->UpdateRobot(id, p, alive, comms); }
    void Run(int n, double weather = 1.0) {
        for (int i = 0; i < n; ++i) {
            bus.BeginTick(++t);
            sq->Step(t, weather);
            for (const auto& e : bus.Current()) ++seen[static_cast<std::size_t>(e.type)];
        }
    }
    ReportResult Hear(RobotId who, std::uint64_t id, Vec3 where, double conf, ThreatClass threat = ThreatClass::Movement,
                      ReportKind kind = ReportKind::Acoustic) {
        ContactReport r;
        r.reporter = who;
        r.reportId = id;
        r.estimate = where;
        r.confidence = conf;
        r.uncertaintyCm = 300;
        r.threat = threat;
        r.kind = kind;
        r.tick = t;
        return sq->Report(r);
    }
    IntentKind K(RobotId id) const { return sq->Intent(id)->kind; }
};

}  // namespace

AIS_TEST(Squad_ReportIsSharedWithLatencyAndLowerConfidence) {
    Rig r(FourBots());
    r.Place(1, {0, 0, 0});
    r.Place(2, {500, 0, 0});
    r.Place(3, {-500, 0, 0});
    r.Place(4, {0, -500, 0});
    r.Run(1);
    CHECK(r.Hear(2, 1, {3000, 0, 0}, 0.8) == ReportResult::Accepted);
    CHECK(r.sq->KnownContacts(2)->size() == 1);
    CHECK(r.sq->KnownContacts(1)->empty());  // not yet: latency
    r.Run(15);
    CHECK(r.sq->KnownContacts(1)->size() == 1);
    const Contact& own = r.sq->KnownContacts(2)->front();
    const Contact& heard = r.sq->KnownContacts(1)->front();
    CHECK(heard.confidence < own.confidence);  // second-hand
    CHECK(r.sq->ReportsShared() == 3);
}

AIS_TEST(Squad_OneInvestigatesOneObservesOthersKeepPatrolling) {
    Rig r(FourBots());
    r.Place(1, {0, 0, 0});
    r.Place(2, {2000, 0, 0});
    r.Place(3, {-500, 500, 0});
    r.Place(4, {0, -500, 0});
    r.Run(1);
    r.Hear(1, 1, {3000, 0, 0}, 0.9);
    r.Run(30);
    CHECK(r.K(2) == IntentKind::Investigate);  // closest capable robot
    CHECK(r.K(3) == IntentKind::Observe);      // the scout watches from standoff
    CHECK(r.K(1) == IntentKind::Patrol);       // keeps its responsibility
    CHECK(r.K(4) == IntentKind::Patrol);       // WARDEN cannot investigate
    const TacticalIntent* obs = r.sq->Intent(3);
    CHECK(Distance(obs->target, {3000, 0, 0}) > 1500.0);
    CHECK(r.sq->ObjectivePriorityFor(2) < r.sq->ObjectivePriorityFor(1));
    CHECK(r.sq->ObjectivePriorityFor(3) > r.sq->ObjectivePriorityFor(1));
    // Arrival turns investigation into a search; negative result restores patrols.
    r.Place(2, {3000, 50, 0});
    r.Run(15);
    CHECK(r.K(2) == IntentKind::Search);
    r.Hear(2, 2, {3000, 0, 0}, 0.0, ThreatClass::Movement, ReportKind::NegativeSearch);
    r.Run(30);
    CHECK(r.K(2) == IntentKind::Patrol);
    CHECK(r.K(3) == IntentKind::Patrol);
}

AIS_TEST(Squad_ConflictingReportsCreateSeparateContactsAndPriorities) {
    Rig r(FourBots());
    r.Place(1, {0, 0, 0});
    r.Place(2, {500, 0, 0});
    r.Place(3, {-500, 0, 0});
    r.Place(4, {0, -500, 0});
    r.Run(1);
    r.Hear(1, 1, {4000, 0, 0}, 0.6, ThreatClass::Disturbance);
    r.Hear(2, 1, {-4000, 0, 0}, 0.6, ThreatClass::Hostile);
    r.Run(30);
    CHECK(r.sq->KnownContacts(1)->size() == 2);
    // Hostile contact is served first: its investigator is the closer of 1/2 to it.
    int investigating = 0;
    for (RobotId id : {1u, 2u, 3u}) investigating += r.K(id) == IntentKind::Investigate ? 1 : 0;
    CHECK(investigating >= 1);
    bool hostileCovered = false;
    for (RobotId id : {1u, 2u, 3u}) {
        const TacticalIntent* t = r.sq->Intent(id);
        hostileCovered = hostileCovered || (t->kind != IntentKind::Patrol && t->target.x < 0);
    }
    CHECK(hostileCovered);
    // Same place reported by two robots fuses into one contact with both sources.
    Rig f(FourBots());
    f.Place(1, {0, 0, 0});
    f.Place(2, {500, 0, 0});
    f.Run(1);
    f.Hear(1, 1, {3000, 0, 0}, 0.5);
    f.Hear(2, 1, {3200, 100, 0}, 0.5);
    f.Run(15);
    CHECK(f.sq->KnownContacts(1)->size() == 1);
    CHECK(f.sq->KnownContacts(1)->front().sources == 3u);
    CHECK(f.sq->KnownContacts(1)->front().confidence > 0.5);
}

AIS_TEST(Squad_DuplicateAndInvalidReportsAreRejected) {
    Rig r(FourBots());
    r.Run(1);
    CHECK(r.Hear(1, 9, {0, 0, 0}, 0.5) == ReportResult::Accepted);
    CHECK(r.Hear(1, 9, {0, 0, 0}, 0.5) == ReportResult::Duplicate);
    CHECK(r.Hear(2, 9, {0, 0, 0}, 0.5) == ReportResult::Accepted);  // ids are per reporter
    CHECK(r.Hear(99, 1, {0, 0, 0}, 0.5) == ReportResult::UnknownRobot);
    CHECK(r.Hear(1, 10, {std::nan(""), 0, 0}, 0.5) == ReportResult::Invalid);
    CHECK(r.Hear(1, 11, {0, 0, 0}, 1.5) == ReportResult::Invalid);
    r.Place(3, {0, 0, 0}, CommsHealth::Intact, false);
    CHECK(r.Hear(3, 1, {0, 0, 0}, 0.5) == ReportResult::RobotDisabled);
}

AIS_TEST(Squad_ConfidenceDecaysAndContactsAreForgotten) {
    Rig r(FourBots());
    r.Run(1);
    r.Hear(1, 1, {3000, 0, 0}, 0.8);
    const double c0 = r.sq->KnownContacts(1)->front().confidence;
    r.Run(static_cast<int>(r.cfg.confidenceHalfLifeTicks));
    CHECK(!r.sq->KnownContacts(1)->empty());
    CHECK(r.sq->KnownContacts(1)->front().confidence < c0 * 0.55);
    r.Run(static_cast<int>(r.cfg.confidenceHalfLifeTicks) * 4);
    CHECK(r.sq->KnownContacts(1)->empty());
    CHECK(r.K(2) == IntentKind::Patrol);  // patrol restored once the trail went cold
}

AIS_TEST(Squad_RangeLossIsolatesRobotThenItActsAlone) {
    Rig r(FourBots());
    r.Place(1, {0, 0, 0});
    r.Place(2, {500, 0, 0});
    r.Place(3, {-500, 0, 0});
    r.Place(4, {0, -500, 0});
    r.Run(15);
    CHECK(r.sq->IsConnected(4));
    r.Place(4, {0, -10000, 0});  // beyond every radio
    r.Run(15);
    CHECK(!r.sq->IsConnected(4));
    CHECK(!r.sq->CanCommunicate(1, 4));
    CHECK_EQ(r.seen[static_cast<std::size_t>(WorldEventType::CommsLost)], std::size_t{1});
    // New information never reaches it.
    r.Hear(1, 1, {3000, 0, 0}, 0.9);
    r.Run(30);
    CHECK(r.sq->KnownContacts(4)->empty());
    CHECK(r.K(4) == IntentKind::Patrol);  // stale order still being executed
    r.Run(static_cast<int>(r.cfg.staleOrderTicks) + 30);
    CHECK(r.K(4) == IntentKind::LocalAutonomous);
    CHECK(!r.sq->Intent(4)->fromSquad);
    CHECK_NEAR(r.sq->ObjectivePriorityFor(4), 0.0, 1e-12);
    // It still uses what it heard itself.
    r.Hear(4, 1, {0, -11000, 0}, 0.7);
    r.Run(15);
    CHECK(Distance(r.sq->Intent(4)->target, {0, -11000, 0}) < 1.0);
    // Back in range: rejoins and receives orders again.
    r.Place(4, {0, -500, 0});
    r.Run(15);
    CHECK(r.sq->IsConnected(4));
    CHECK(r.sq->Intent(4)->fromSquad);
}

AIS_TEST(Squad_DamagedCommsShrinkRangeAndDropIntermittently) {
    Rig r(FourBots());
    r.Place(1, {0, 0, 0});
    r.Place(2, {3500, 0, 0});  // inside 5000 cm, outside 2500 cm
    r.Place(3, {-500, 0, 0});
    r.Place(4, {0, -500, 0});
    r.Run(15);
    CHECK(r.sq->CanCommunicate(1, 2));
    r.Place(2, {3500, 0, 0}, CommsHealth::Damaged);
    r.Run(15);
    CHECK(!r.sq->CanCommunicate(1, 2));
    r.Place(2, {1000, 0, 0}, CommsHealth::Damaged);
    int up = 0;
    for (int i = 0; i < 40; ++i) {
        r.Run(15);
        up += r.sq->CanCommunicate(1, 2) ? 1 : 0;
    }
    CHECK(up > 15 && up < 40);  // works most of the time, drops sometimes
    r.Place(2, {1000, 0, 0}, CommsHealth::Destroyed);
    r.Run(15);
    CHECK(!r.sq->CanCommunicate(1, 2));
    CHECK(!r.sq->IsConnected(2));
}

AIS_TEST(Squad_PoweredRelayBridgesDistanceAndPowerLossSplitsSquad) {
    Rig r(FourBots(true));
    r.Place(1, {0, 0, 0});
    r.Place(2, {15000, 0, 0});
    r.Place(3, {-15000, 0, 0});
    r.Place(4, {0, -15000, 0});
    r.Run(15);
    CHECK(r.sq->IsConnected(2) && r.sq->IsConnected(3) && r.sq->IsConnected(4));
    r.relay.up = false;  // power failure at the relay
    r.Run(15);
    CHECK(!r.sq->IsConnected(2) && !r.sq->IsConnected(3) && !r.sq->IsConnected(4));
    // Storm interference shrinks the relay's reach too.
    r.relay.up = true;
    r.Run(15, 0.5);
    CHECK(!r.sq->IsConnected(2));
    r.Run(15, 1.0);
    CHECK(r.sq->IsConnected(2));
}

AIS_TEST(Squad_HostileContactTriggersFlankAndDefence) {
    SquadConfig c = FourBots();
    c.minPatrolFraction = 0.0;
    Rig r(c);
    r.Place(1, {0, 0, 0});
    r.Place(2, {500, 0, 0});
    r.Place(3, {-500, 0, 0});
    r.Place(4, {0, -500, 0});
    r.Run(1);
    r.Hear(1, 1, {3000, 0, 0}, 0.95, ThreatClass::Hostile);
    r.Run(30);
    bool flank = false;
    for (RobotId id : {1u, 2u}) flank = flank || r.K(id) == IntentKind::Flank;
    CHECK(flank);
    CHECK(r.K(4) == IntentKind::Defend);
    CHECK(r.K(3) == IntentKind::Observe);
}

AIS_TEST(Squad_DataDrivenRolesAndValidation) {
    std::vector<std::string> errors;
    SquadConfig c;
    c.roles = SquadConfig::DefaultRoles();
    LoadSquads(ParseIni("[role.HOUND]\ncan_observe = true\ncomms_range_cm = 4000\n"
                        "[squad.B]\nmembers = 10:HOUND, 11:SENTINEL\npatrol.10 = 0,0,0 | 100,0,0\n",
                        errors),
               c, errors);
    CHECK(errors.empty());
    CHECK(ValidateSquads(c).empty());
    CHECK(c.squads[0].members[0].patrol.size() == 2);
    SquadConfig bad = c;
    bad.squads[0].members[1].role = "GHOST";
    CHECK(!ValidateSquads(bad).empty());
    bad = c;
    bad.squads.push_back(c.squads[0]);  // robot in two squads
    CHECK(!ValidateSquads(bad).empty());
    bad = c;
    bad.minPatrolFraction = 1.0;
    CHECK(!ValidateSquads(bad).empty());
    std::vector<std::string> e2;
    LoadSquads(ParseIni("[squad.C]\nmembers = x:SENTINEL\npatrol.5 = 0,0,0\n", e2), c, e2);
    CHECK(e2.size() >= 2);
    SquadCoordinator invalid(bad, nullptr, nullptr);
    CHECK(!invalid.IsValid());
}

AIS_TEST(Squad_DeterministicReplay) {
    auto run = []() {
        Rig r(FourBots());
        std::vector<std::uint64_t> h;
        for (int t = 0; t < 900; ++t) {
            const double x = 100.0 * t;
            r.Place(1, {x * 0.1, 0, 0}, t > 400 ? CommsHealth::Damaged : CommsHealth::Intact);
            r.Place(2, {2000, x * 0.05, 0});
            if (t % 60 == 0) r.Hear(static_cast<RobotId>(1 + (t / 60) % 3), static_cast<std::uint64_t>(t), {3000.0 + t, 500, 0}, 0.6);
            r.Run(1);
            h.push_back(r.sq->StateHash());
        }
        return h;
    };
    CHECK(run() == run());
}
