// LAST LIGHT - Environmental & World Intelligence: standalone simulation.
//
// Runs the three integration scenarios on "Pumping Station 9" with four robots
// (2 SENTINEL, 1 WRAITH, 1 data-defined WARDEN) and up to two players.
// Robot movement is a straight-line stand-in for Unreal navigation.
//
//   make demo            # all scenarios
//   ./build/world_demo B # one scenario
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "WorldIntelligence.h"

using namespace llworld;

namespace {

class Inventory final : public llenv::IPlayerInventory {
public:
    bool HasItem(llenv::PlayerId, std::string_view) const override { return false; }
    bool ConsumeItem(llenv::PlayerId, std::string_view) override { return false; }
};

const char* kRobotNames[5] = {"", "SENTINEL-1", "SENTINEL-2", "WRAITH-3", "WARDEN-4"};

struct Sim {
    WorldIntelligenceConfig cfg;
    llenv::FacilityDef facilityDef;
    ais::AisConfig aisCfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment env;
    Inventory inv;
    std::unique_ptr<WorldIntelligence> wi;
    std::map<RobotId, CommsHealth> comms;
    std::map<RobotId, std::pair<IntentKind, ais::AwarenessState>> last;
    std::uint32_t seq = 0;
    std::size_t counted = 0;

    bool Load() {
        std::string text;
        std::vector<std::string> errors;
        if (!ReadFile(WI_DATA_DIR "/industrial_site_world.ini", text) || !LoadWorldConfig(text, cfg, errors)) {
            for (const auto& e : errors) std::fprintf(stderr, "world config: %s\n", e.c_str());
            return false;
        }
        const llenv::LoadResult f = llenv::LoadFacilityFile(WI_DATA_DIR "/industrial_site.ini", facilityDef);
        for (const auto& e : f.errors) std::fprintf(stderr, "facility: %s\n", e.c_str());
        if (!f.ok) return false;
        wi = std::make_unique<WorldIntelligence>(cfg, aisCfg, env, &facilityDef, &inv);
        for (const auto& e : wi->Errors()) std::fprintf(stderr, "wiring: %s\n", e.c_str());
        if (!wi->IsValid()) return false;
        wi->AddRobot(1, "SENTINEL", {500, 1500, 0});
        wi->AddRobot(2, "SENTINEL", {3500, -1500, 0});
        wi->AddRobot(3, "WRAITH", {-1500, 0, 0});
        wi->AddRobot(4, "SENTINEL", {6500, 0, 0});
        for (RobotId r = 1; r <= 4; ++r) comms[r] = CommsHealth::Intact;
        return true;
    }
    double Sec() const { return static_cast<double>(wi->CurrentTick()) / 30.0; }
    void Note(const char* text) const { std::printf("t=%6.1fs  ** %s\n", Sec(), text); }

    void PrintEvent(const WorldEvent& e) const {
        using T = WorldEventType;
        switch (e.type) {
            case T::WeatherChanged:
            case T::PlayerExposure:
            case T::AssignmentChanged:
            case T::Gunfire:
            case T::Explosion: return;
            default: break;
        }
        std::string what;
        if (e.source == WorldSystem::Hazards && e.subject < wi->Config().hazards.hazards.size()) what = wi->Config().hazards.hazards[e.subject].id;
        if (e.source == WorldSystem::Facility && wi->Facility() != nullptr) what = wi->Facility()->Def(e.subject).id;
        if (e.type == T::CommsLost || e.type == T::CommsRestored) what = kRobotNames[e.subject <= 4 ? e.subject : 0];
        if (e.type == T::RouteBlocked || e.type == T::RouteCleared) what += " route " + std::to_string(e.aux);
        if (e.type == T::WindGust) what = e.aux != 0 ? "loose roof sheet (metallic)" : "fence";
        std::printf("t=%6.1fs   world   %-18s %s\n", Sec(), ToString(e.type), what.c_str());
    }

    void Step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            wi->Step();
            for (const auto& e : wi->Bus().Current()) PrintEvent(e);
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
                const auto key = std::make_pair(o.intent, o.awareness);
                if (last[id] != key) {
                    last[id] = key;
                    std::printf("t=%6.1fs   robot   %-11s intent=%-15s hearing=%-17s order-from=%s%s\n", Sec(), kRobotNames[id], ToString(o.intent),
                                ais::ToString(o.awareness), o.source == OrderSource::Squad ? "squad" : (o.source == OrderSource::Acoustic ? "own-ears" : "-"),
                                wi->Squads().IsConnected(id) ? "" : "  [NO COMMS]");
                }
            }
        }
    }
    llenv::InteractionResult Player(llenv::PlayerId p, const char* node, llenv::InteractionAction a) {
        llenv::InteractionRequest q;
        q.player = p;
        q.sequence = ++seq;
        q.node = wi->Facility()->Find(node);
        q.action = a;
        return wi->Facility()->Submit(q);
    }
};

void ScenarioA() {
    std::printf("\n=== SCENARIO A: storm, generator failure, emergency light, alarm, squad ===\n");
    Sim s;
    if (!s.Load()) return;
    s.Step(30);
    s.Note("a storm rolls in");
    s.wi->Weather().RequestWeather(WeatherKind::Storm, 300, s.wi->CurrentTick());
    s.Step(330);
    std::printf("           AIS weather now rain=%.2f wind=%.2f; distant report confidence x%.2f; radio range x%.2f\n", s.env.Weather().rain,
                s.env.Weather().wind, s.wi->Weather().AcousticReportConfidenceScale(6000), s.wi->Weather().Snapshot().commsRangeMul);
    s.Step(30 * 30);
    const auto& gen = s.wi->Facility()->State(s.wi->Facility()->Find("GEN_A"));
    std::printf("           GEN_A health %.2f (%s)\n", gen.health, llenv::ToString(gen.gen));
}

void ScenarioB() {
    std::printf("\n=== SCENARIO B: player in the hall, live cable, industrial alarm, squad splits work ===\n");
    Sim s;
    if (!s.Load()) return;
    s.wi->Facility()->ConnectPlayer(1, {2050, 150, 0}, 0);
    s.wi->SetPlayers({{1, {2050, 150, 0}}});
    s.Step(30);
    std::printf("           player at the cable: danger %.1f HP/s, dose so far %.1f\n", s.wi->HazardDangerAt({2050, 150, 0}), s.wi->Hazards().DoseOf(1));
    s.wi->Facility()->UpdatePlayerPosition(1, {2200, 300, 0});
    s.wi->SetPlayers({{1, {2200, 300, 0}}});
    s.Note("player pulls the industrial alarm");
    std::printf("           -> %s\n", llenv::ToString(s.Player(1, "ALARM_PLANT", llenv::InteractionAction::TriggerAlarm)));
    s.Step(30 * 25);
}

void ScenarioC() {
    std::printf("\n=== SCENARIO C: fire, smoke, rerouting, incomplete senses, comms failure ===\n");
    Sim s;
    if (!s.Load()) return;
    s.Step(30);
    std::printf("           route risk before: west=%.1f east=%.1f\n", s.wi->RouteRisk(1), s.wi->RouteRisk(2));
    s.Note("damaged wiring ignites the store room");
    s.wi->Hazards().Activate(s.wi->Hazards().Find("FIRE_STORE"), 1);
    s.Step(60);
    const VisibilitySample v = s.wi->VisibilityAt({2600, -100, 0});
    std::printf("           in the smoke: visual x%.2f, thermal x%.2f (fire clutters thermal)\n", v.visualRangeFactor, v.thermalRangeFactor);
    const double west = s.wi->RouteRisk(1), east = s.wi->RouteRisk(2);
    std::printf("           route risk now: west=%.1f east=%s -> players take the %s route\n", west, std::isinf(east) ? "BLOCKED" : "open",
                west < east ? "west yard" : "east hall");
    s.Note("the fire reaches the relay's feed: COMMS_RELAY destroyed");
    s.wi->Facility()->ServerApplyDamage(s.wi->Facility()->Find("COMMS_RELAY"), 1.0);
    s.Step(20);
    s.Note("a player fires near SENTINEL-1");
    s.wi->ReportGunfire(9, {1000, 1500, 0});
    s.Step(30 * 25);
}

}  // namespace


int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    if (only == nullptr || std::strcmp(only, "A") == 0) ScenarioA();
    if (only == nullptr || std::strcmp(only, "B") == 0) ScenarioB();
    if (only == nullptr || std::strcmp(only, "C") == 0) ScenarioC();
    return 0;
}
