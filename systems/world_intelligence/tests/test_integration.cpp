// Cross-system integration: full stack (AIS + power grid + weather + hazards + squads)
// driven from the shipped data files. Scenarios A, B, C plus edge cases.
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "TestFramework.h"
#include "WorldIntelligence.h"

using namespace llworld;

namespace {

class Inventory final : public llenv::IPlayerInventory {
public:
    bool HasItem(llenv::PlayerId p, std::string_view i) const override {
        auto it = items.find({p, std::string(i)});
        return it != items.end() && it->second > 0;
    }
    bool ConsumeItem(llenv::PlayerId p, std::string_view i) override {
        auto it = items.find({p, std::string(i)});
        if (it == items.end() || it->second <= 0) return false;
        --it->second;
        return true;
    }
    std::map<std::pair<llenv::PlayerId, std::string>, int> items;
};

struct Stack {
    WorldIntelligenceConfig cfg;
    llenv::FacilityDef facilityDef;
    ais::AisConfig aisCfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment env;
    Inventory inv;
    std::unique_ptr<WorldIntelligence> wi;
    std::array<std::size_t, kWorldEventTypeCount> seen{};
    std::map<RobotId, CommsHealth> comms;
    std::uint32_t seq = 0;
    bool loaded = false;
    std::size_t counted = 0;

    explicit Stack(bool robots = true) {
        std::string text;
        std::vector<std::string> errors;
        loaded = ReadFile(WI_DATA_DIR "/industrial_site_world.ini", text) && LoadWorldConfig(text, cfg, errors) &&
                 llenv::LoadFacilityFile(WI_DATA_DIR "/industrial_site.ini", facilityDef).ok;
        for (const auto& e : errors) std::printf("    config: %s\n", e.c_str());
        wi = std::make_unique<WorldIntelligence>(cfg, aisCfg, env, &facilityDef, &inv);
        for (const auto& e : wi->Errors()) std::printf("    wi: %s\n", e.c_str());
        if (robots) {
            wi->AddRobot(1, "SENTINEL", {500, 1500, 0});
            wi->AddRobot(2, "SENTINEL", {3500, -1500, 0});
            wi->AddRobot(3, "WRAITH", {-1500, 0, 0});
            wi->AddRobot(4, "SENTINEL", {6500, 0, 0});  // squad role WARDEN, Sentinel hearing
            for (RobotId r = 1; r <= 4; ++r) comms[r] = CommsHealth::Intact;
        }
    }
    llenv::Facility& F() { return *wi->Facility(); }
    llenv::NodeIndex N(const char* id) { return F().Find(id); }
    void Step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            // Count events the game published between ticks, then the tick's own.
            const auto& pending = wi->Bus().Current();
            for (std::size_t k = counted; k < pending.size(); ++k) ++seen[static_cast<std::size_t>(pending[k].type)];
            wi->Step();
            for (const auto& e : wi->Bus().Current()) ++seen[static_cast<std::size_t>(e.type)];
            counted = wi->Bus().Current().size();
            for (auto& [id, health] : comms) {
                const RobotOrder o = wi->OrderFor(id);
                const ais::Vec3 h = wi->Acoustic().HostState(id)->position;
                Vec3 p{h.x, h.y, h.z};
                if (o.hasMoveTarget) {
                    const Vec3 d = o.moveTarget - p;
                    const double len = d.Length();
                    p = len <= 14.0 ? o.moveTarget : p + d * (14.0 / len);
                }
                wi->SetRobotBody(id, p, true, health);
            }
        }
    }
    std::size_t Seen(WorldEventType t) const { return seen[static_cast<std::size_t>(t)]; }
    llenv::InteractionResult Player(llenv::PlayerId p, const char* node, llenv::InteractionAction a) {
        llenv::InteractionRequest q;
        q.player = p;
        q.sequence = ++seq;
        q.node = N(node);
        q.action = a;
        return F().Submit(q);
    }
    Vec3 Pos(const char* node) {
        const llenv::Vec3 v = F().Def(N(node)).position;
        return {v.x, v.y, v.z};
    }
};

bool AnyIntentNear(Stack& s, const Vec3& where, double radius, std::initializer_list<IntentKind> kinds) {
    for (RobotId r = 1; r <= 4; ++r) {
        const TacticalIntent* t = s.wi->Squads().Intent(r);
        if (t == nullptr) continue;
        for (IntentKind k : kinds) {
            if (t->kind == k && Distance(t->target, where) <= radius) return true;
        }
    }
    return false;
}

}  // namespace

AIS_TEST(Integration_DataFilesLoadAndWire) {
    Stack s;
    CHECK(s.loaded);
    CHECK(s.wi->IsValid());
    CHECK(s.wi->Hazards().Count() == 9);
    s.Step(2);
    CHECK(s.F().State(s.N("GEN_A")).gen == llenv::GeneratorState::Running);
    CHECK(s.wi->Hazards().State(s.wi->Hazards().Find("CABLE_ARC")).state == HazardState::Active);
    CHECK(s.wi->Squads().IsConnected(4));  // via the powered relay
    // Unknown section kinds and bad facility references are rejected.
    WorldIntelligenceConfig c;
    std::vector<std::string> errors;
    CHECK(!LoadWorldConfig("[wether]\nx = 1\n", c, errors));
    WorldIntelligenceConfig c2;
    std::vector<std::string> e2;
    CHECK(LoadWorldConfig("[hazard.H]\ntype = Electrical\nposition = 0,0,0\npower_node = NOPE\n", c2, e2));
    ais::BoxAcousticEnvironment env;
    ais::AisConfig ac = ais::AisConfig::MakeDefault();
    llenv::FacilityDef fd;
    llenv::LoadFacilityFile(WI_DATA_DIR "/industrial_site.ini", fd);
    WorldIntelligence bad(c2, ac, env, &fd, nullptr);
    CHECK(!bad.IsValid());
}

AIS_TEST(Integration_WeatherDrivesAcousticMaskingButPuddlesSplash) {
    // Footstep 9 m from a Sentinel: heard in clear weather, masked by a storm.
    // No power grid here: a running generator would mask it on its own.
    auto heard = [](WeatherKind kind, bool waitForPuddles) {
        WorldIntelligenceConfig cfg;
        cfg.weather.autoAdvance = false;
        ais::AisConfig ac = ais::AisConfig::MakeDefault();
        ais::BoxAcousticEnvironment env;
        WorldIntelligence wi(cfg, ac, env, nullptr, nullptr);
        wi.AddRobot(1, "SENTINEL", {0, 0, 0});
        wi.Weather().RequestWeather(kind, 1, 0);
        for (int i = 0; i < (waitForPuddles ? 30 * 40 : 40); ++i) wi.Step();
        if (waitForPuddles) {
            wi.Weather().RequestWeather(WeatherKind::Clear, 1, wi.CurrentTick());
            for (int i = 0; i < 40; ++i) wi.Step();  // rain stops, ground still wet
        }
        std::vector<ais::PerceptionRecord> seen;
        wi.Acoustic().SetPerceptionObserver(&seen);
        wi.PlayerFootstep(7, {900, 0, 0}, ais::SurfaceType::Concrete);
        wi.Step();
        wi.Step();
        wi.Acoustic().SetPerceptionObserver(nullptr);
        return seen.empty() ? -1e9 : seen.front().perceived.effectiveSnrDb;
    };
    const double clear = heard(WeatherKind::Clear, false);
    const double storm = heard(WeatherKind::Storm, false);
    const double wet = heard(WeatherKind::HeavyRain, true);
    CHECK(clear > 0.0);
    CHECK(storm < -1e8);  // not perceived at all
    CHECK(wet > clear);   // splashing through puddles after the rain is louder
}

AIS_TEST(Integration_VisibilityAndThermalAreSeparateAndBounded) {
    Stack s;
    s.Step(2);
    const Vec3 yard{-1000, 2500, 0};
    const double clearVis = s.wi->VisualDetectionRangeCm(5000, {0, 2500, 0}, yard);
    const double clearThermal = s.wi->ThermalDetectionRangeCm(5000, {0, 2500, 0}, yard);
    s.wi->Weather().RequestWeather(WeatherKind::Fog, 1, s.wi->CurrentTick());
    s.Step(40);
    const double fogVis = s.wi->VisualDetectionRangeCm(5000, {0, 2500, 0}, yard);
    const double fogThermal = s.wi->ThermalDetectionRangeCm(5000, {0, 2500, 0}, yard);
    CHECK(fogVis < clearVis * 0.5);
    CHECK(fogThermal > clearThermal * 0.8);  // fog barely matters to thermal
    // Lit areas are easier to see into than dark ones.
    const double lit = s.wi->VisualDetectionRangeCm(5000, {0, 0, 0}, {1800, 0, 0});
    const double dark = s.wi->VisualDetectionRangeCm(5000, {0, 0, 0}, {-3000, -3000, 0});
    CHECK(lit > dark);
    // Never invisible at arm's length.
    CHECK(s.wi->VisualDetectionRangeCm(10, {0, 0, 0}, {0, 0, 0}) >= s.cfg.minDetectionRangeCm);
}

AIS_TEST(Integration_ScenarioA_StormGeneratorFailureEmergencyLightsAlarmSquad) {
    Stack s;
    s.Step(30);
    CHECK(s.wi->Weather().RequestWeather(WeatherKind::Storm, 300, s.wi->CurrentTick()));
    s.Step(400);
    CHECK(s.env.Weather().rain > 0.9);  // AIS noise floors follow the weather
    CHECK(s.wi->Weather().AcousticReportConfidenceScale(6000) < 0.6);
    bool investigatedAlarm = false;
    for (int i = 0; i < 1500 && !investigatedAlarm; ++i) {
        s.Step();
        investigatedAlarm = s.Seen(WorldEventType::AlarmRaised) > 0 &&
                            AnyIntentNear(s, s.Pos("ALARM_PLANT"), 2500, {IntentKind::Investigate, IntentKind::Search, IntentKind::Observe});
    }
    CHECK(s.F().State(s.N("GEN_A")).destroyed);          // exposed, damaged generator failed
    CHECK(s.Seen(WorldEventType::PowerLost) >= 1);
    CHECK(s.Seen(WorldEventType::EmergencyLighting) >= 1);
    CHECK(s.Seen(WorldEventType::AlarmRaised) >= 1);      // security reboot when backup restored power
    CHECK(investigatedAlarm);
}

AIS_TEST(Integration_ScenarioB_CableHazardPlayerAlarmSquadSplitsWork) {
    Stack s;
    s.F().ConnectPlayer(1, {2050, 150, 0}, 0);
    s.wi->SetPlayers({{1, {2050, 150, 0}}});
    s.Step(30);
    CHECK(s.wi->HazardDangerAt({2050, 150, 0}) > 0.0);
    CHECK(s.wi->Hazards().DoseOf(1) > 0.0);  // the frayed cable hurts
    s.F().UpdatePlayerPosition(1, {2200, 300, 0});
    s.wi->SetPlayers({{1, {2200, 300, 0}}});
    CHECK(s.Player(1, "ALARM_PLANT", llenv::InteractionAction::TriggerAlarm) == llenv::InteractionResult::Applied);
    bool split = false;
    for (int i = 0; i < 600 && !split; ++i) {
        s.Step();
        int investigating = 0, observing = 0, patrolling = 0;
        for (RobotId r = 1; r <= 4; ++r) {
            const IntentKind k = s.wi->Squads().Intent(r)->kind;
            investigating += (k == IntentKind::Investigate || k == IntentKind::Search) ? 1 : 0;
            observing += k == IntentKind::Observe ? 1 : 0;
            patrolling += k == IntentKind::Patrol ? 1 : 0;
        }
        split = investigating >= 1 && observing >= 1 && patrolling >= 1;
    }
    CHECK(split);  // one goes in, the scout watches, someone keeps the patrol
    CHECK(s.wi->ReportsFromAcoustics() > 0);
}

AIS_TEST(Integration_ScenarioC_FireSmokeRouteChoiceAndIsolatedRobot) {
    Stack s;
    s.Step(30);
    const Vec3 store = s.Pos("DOOR_HALL");
    (void)store;
    const double riskEastBefore = s.wi->RouteRisk(2);
    CHECK(std::isfinite(riskEastBefore));
    const HazardSystem& hz = s.wi->Hazards();
    CHECK(s.wi->Hazards().Activate(hz.Find("FIRE_STORE"), 1));
    s.Step(60);
    // Smoke: eyes are nearly useless, thermal less so (but fire clutters it).
    const VisibilitySample v = s.wi->VisibilityAt({2600, -100, 0});
    CHECK(v.smoke > 0.4);
    CHECK(v.visualRangeFactor < 0.5);
    CHECK(s.wi->IsRouteBlocked(2));
    CHECK(std::isinf(s.wi->RouteRisk(2)));
    CHECK(std::isfinite(s.wi->RouteRisk(1)));  // players reroute through the yard
    // Robots heard the fire only as noise with uncertainty, never its position.
    bool fireTrace = false;
    for (RobotId r = 1; r <= 4; ++r) {
        for (const auto& t : s.wi->Acoustic().Brain(r)->Traces()) {
            if (Distance(Vec3{t.estimate.x, t.estimate.y, t.estimate.z}, {2500, -300, 0}) < 3000 && t.uncertaintyCm > 50) fireTrace = true;
        }
    }
    CHECK(fireTrace);
    // Comms failure: the relay loses power; the far WARDEN is cut off.
    CHECK(s.F().ServerApplyDamage(s.N("COMMS_RELAY"), 1.0));
    s.Step(16);
    CHECK(!s.wi->Squads().IsConnected(4));
    const std::uint32_t v4 = s.wi->Squads().Intent(4)->version;
    s.wi->ReportGunfire(9, {1000, 1500, 0});  // new hostile contact near robot 1
    s.Step(60);
    CHECK_EQ(s.wi->Squads().Intent(4)->version, v4);  // never got the new orders
    CHECK(s.wi->Squads().KnownContacts(4) != nullptr);
    s.Step(static_cast<int>(s.cfg.squads.staleOrderTicks));
    CHECK(s.wi->Squads().Intent(4)->kind == IntentKind::LocalAutonomous);
}

AIS_TEST(Integration_ElectricalHazardFollowsPowerShutdown) {
    Stack s;
    s.F().ConnectPlayer(1, s.F().Def(s.N("BRK_PLANT")).position, 0);
    s.Step(20);
    const std::uint32_t arc = s.wi->Hazards().Find("CABLE_ARC");
    CHECK(s.wi->Hazards().State(arc).state == HazardState::Active);
    CHECK(s.Player(1, "BRK_PLANT", llenv::InteractionAction::Deactivate) == llenv::InteractionResult::Applied);
    s.Step(2);
    CHECK(s.wi->Hazards().State(arc).state == HazardState::Suppressed);
    CHECK(s.wi->HazardDangerAt({2100, 100, 0}) == 0.0);
    CHECK(s.F().State(s.N("EMERG_HALL")).lit);  // emergency lighting on the dead circuit
    CHECK(s.Player(1, "BRK_PLANT", llenv::InteractionAction::Activate) == llenv::InteractionResult::Applied);
    s.Step(2);
    CHECK(s.wi->Hazards().State(arc).state == HazardState::Active);  // restoring power re-arms the cable
}

AIS_TEST(Integration_CollapseCutsCableAndKillsArc) {
    Stack s;
    s.Step(20);
    const std::uint32_t arc = s.wi->Hazards().Find("CABLE_ARC");
    for (int i = 0; i < 4; ++i) {
        s.wi->ReportExplosion({2600, -200, 0}, 300);
        s.Step(20);
    }
    CHECK(s.wi->Hazards().State(s.wi->Hazards().Find("ROOF_TRUSS")).collapsed);
    CHECK(s.F().State(s.N("CABLE_EAST")).destroyed);
    s.Step(2);
    CHECK(s.wi->Hazards().State(arc).state == HazardState::Suppressed);
    CHECK(s.wi->IsRouteBlocked(2));
}

AIS_TEST(Integration_MultipleSimultaneousEventsAndFlooding) {
    Stack s;
    s.F().ConnectPlayer(1, s.F().Def(s.N("ALARM_PLANT")).position, 0);
    s.Step(20);
    // Same tick: explosion, gunfire, gas valve, alarm, generator damage, storm.
    s.wi->ReportExplosion({2500, -200, 0}, 400);
    s.wi->ReportGunfire(1, {2400, -200, 0});
    s.wi->Hazards().Activate(s.wi->Hazards().Find("GAS_STORE"), 5);
    s.wi->Hazards().Activate(s.wi->Hazards().Find("GAS_STORE"), 5);  // duplicate cause
    s.Player(1, "ALARM_PLANT", llenv::InteractionAction::TriggerAlarm);
    s.F().ServerApplyDamage(s.N("GEN_A"), 0.2);
    s.wi->Weather().RequestWeather(WeatherKind::Storm, 60, s.wi->CurrentTick());
    // Flood: thousands of footsteps and invalid inputs.
    for (int i = 0; i < 5000; ++i) s.wi->PlayerFootstep(static_cast<PlayerId>(1 + i % 6), {static_cast<double>(i), 0, 0}, ais::SurfaceType::Metal);
    s.wi->ReportExplosion({std::nan(""), 0, 0}, std::nan(""));
    CHECK(!s.wi->SetRobotBody(1, {std::nan(""), 0, 0}, true, CommsHealth::Intact));
    CHECK(!s.wi->SetRobotBody(99, {0, 0, 0}, true, CommsHealth::Intact));
    s.wi->SetPlayers({{1, {0, 0, 0}}, {2, {0, 0, 0}}, {3, {0, 0, 0}}, {4, {0, 0, 0}}, {5, {0, 0, 0}}, {6, {std::nan(""), 0, 0}}});
    s.Step(600);
    CHECK(s.wi->Acoustic().Bus().QueueSize() <= s.aisCfg.bus.queueCapacity);
    CHECK(s.wi->Acoustic().Bus().Stats().Count(ais::SubmitResult::RejectedRateLimited) > 0);
    CHECK(s.wi->Bus().Current().size() <= s.wi->Bus().Capacity());
    CHECK(std::isfinite(static_cast<double>(s.wi->StateHash())));
    CHECK(s.Seen(WorldEventType::Explosion) == 1);  // the NaN explosion was dropped
    for (RobotId r = 1; r <= 4; ++r) {
        const ais::Vec3 p = s.wi->Acoustic().HostState(r)->position;
        CHECK(std::isfinite(p.x) && std::isfinite(p.y));
    }
}

AIS_TEST(Integration_FullStackDeterministicReplayWithPlayers) {
    auto run = []() {
        Stack s;
        s.F().ConnectPlayer(1, {2050, 150, 0}, 0);
        s.F().ConnectPlayer(2, {-500, 1500, 0}, 0);
        std::vector<std::uint64_t> hashes;
        for (int t = 0; t < 1500; ++t) {
            const Vec3 p1{2050.0 + t * 0.5, 150, 0};
            const Vec3 p2{-500.0 + t, 1500, 0};
            s.F().UpdatePlayerPosition(1, {p1.x, p1.y, p1.z});
            s.F().UpdatePlayerPosition(2, {p2.x, p2.y, p2.z});
            s.wi->SetPlayers({{1, p1}, {2, p2}});
            if (t % 15 == 0) s.wi->PlayerFootstep(2, p2, ais::SurfaceType::Gravel);
            if (t == 100) s.wi->Weather().RequestWeather(WeatherKind::HeavyRain, 200, s.wi->CurrentTick());
            if (t == 300) s.Player(1, "ALARM_PLANT", llenv::InteractionAction::TriggerAlarm);
            if (t == 500) s.wi->Hazards().Activate(s.wi->Hazards().Find("FIRE_STORE"), 1);
            if (t == 800) s.comms[3] = CommsHealth::Damaged;
            s.Step();
            if (t % 50 == 0) hashes.push_back(s.wi->StateHash());
        }
        return hashes;
    };
    const auto a = run();
    const auto b = run();
    CHECK(a == b);
    CHECK(a.front() != a.back());
}
