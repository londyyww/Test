// LAST LIGHT - Environmental Systems: example military facility simulation.
//
// Three players work through the data-defined "Relay Outpost" while two
// SENTINELs and a WRAITH listen through the Acoustic Intelligence bridge.
// Robot movement is a straight-line stand-in for Unreal navigation.
//
// Build/run: make demo
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "AisAcousticBridge.h"
#include "ais/AcousticWorld.h"
#include "llenv/Facility.h"

using namespace llenv;
using A = InteractionAction;

namespace {

class Inventory final : public IPlayerInventory {
public:
    bool HasItem(PlayerId p, std::string_view item) const override {
        auto it = items.find({p, std::string(item)});
        return it != items.end() && it->second > 0;
    }
    bool ConsumeItem(PlayerId p, std::string_view item) override {
        auto it = items.find({p, std::string(item)});
        if (it == items.end() || it->second <= 0) return false;
        --it->second;
        return true;
    }
    std::map<std::pair<PlayerId, std::string>, int> items;
};

// Prints gameplay-relevant facility events and forwards sound to the bridge.
class Narrator final : public IFacilityObserver, public IAcousticSink {
public:
    Narrator(const Facility*& f, IAcousticSink& next) : facility(f), forward(next) {}
    void OnFacilityEvent(const FacilityEvent& e) override {
        using T = FacilityEventType;
        if (e.type == T::PowerChanged || e.type == T::InteractionCompleted) return;
        if (e.type == T::LightChanged && facility->Def(e.node).type != NodeType::EmergencyLight) return;
        const std::string id = e.node == kInvalidNode ? "-" : facility->Def(e.node).id;
        std::string detail = e.text;
        if (e.type == T::GeneratorState) detail = ToString(static_cast<GeneratorState>(e.value)) + (e.text.empty() ? "" : " (" + e.text + ")");
        if (e.type == T::DoorState) detail = ToString(static_cast<DoorState>(e.value));
        if (e.type == T::LightChanged) detail = e.value > 0 ? "emergency ON" : "off (battery flat or mains back)";
        if (e.type == T::AlertLevelChanged || e.type == T::SecurityArmed || e.type == T::BreakerState || e.type == T::NavBlockChanged) {
            detail = std::to_string(e.value);
        }
        std::printf("t=%5.1fs   facility  %-18s %-15s %s\n", static_cast<double>(e.tick) / 30.0, ToString(e.type), id.c_str(), detail.c_str());
    }
    void OnSound(const EnvSoundEvent& e) override {
        const bool pulse = e.kind == SoundKind::GeneratorRunning || e.kind == SoundKind::MachineryRunning || e.kind == SoundKind::CommBurst;
        if (!pulse && (e.kind != SoundKind::AlarmPulse || !alarmAnnounced)) {
            std::printf("t=%5.1fs   sound     %-18s %-15s %.0f dB\n", static_cast<double>(e.tick) / 30.0, ToString(e.kind),
                        facility->Def(e.node).id.c_str(), e.loudnessDb);
            if (e.kind == SoundKind::AlarmPulse) alarmAnnounced = true;
        }
        forward.OnSound(e);
    }
    void OnContinuousSound(NodeIndex n, const Vec3& p, double db, bool active) override { forward.OnContinuousSound(n, p, db, active); }
    const Facility*& facility;
    IAcousticSink& forward;
    bool alarmAnnounced = false;
};

}  // namespace

int main() {
    FacilityDef def;
    const LoadResult load = LoadFacilityFile(ENV_DATA_DIR "/relay_outpost.ini", def);
    for (const auto& e : load.errors) std::fprintf(stderr, "facility: %s\n", e.c_str());
    if (!load.ok) return 1;

    ais::AisConfig aisCfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment acousticEnv;
    ais::AcousticWorld robots(aisCfg, acousticEnv);
    AisAcousticBridge bridge(aisCfg, robots, &acousticEnv);

    Facility facility(def);
    const Facility* fptr = &facility;
    Narrator narrator(fptr, bridge);
    Inventory inv;
    facility.SetObserver(&narrator);
    facility.SetAcousticSink(&narrator);
    facility.SetInventory(&inv);

    struct Bot {
        ais::EntityId id;
        const char* label;
        ais::Vec3 pos;
    };
    const std::vector<Bot> bots{{1, "SENTINEL-North", {2000, 2500, 0}}, {2, "SENTINEL-South", {3500, -2500, 0}}, {3, "WRAITH", {-1500, 0, 0}}};
    for (const Bot& b : bots) {
        robots.AddRobot(b.id, b.id == 3 ? "WRAITH" : "SENTINEL", b.pos);
        ais::RobotHostState h;
        h.position = b.pos;
        robots.SetHostState(b.id, h);
    }

    auto at = [&](const char* id) { return def.nodes[static_cast<std::size_t>(def.Find(id))].position; };
    facility.ConnectPlayer(1, at("GEN_MAIN"), 0);       // "Volk"
    facility.ConnectPlayer(2, at("BRK_INDUSTRIAL"), 0);  // "Sable"
    facility.ConnectPlayer(3, at("TERM_SEC"), 0);        // "Kit"
    inv.items[{2, "Fuse"}] = 1;
    inv.items[{1, "DieselCan"}] = 1;

    std::uint32_t seq = 0;
    auto act = [&](PlayerId p, const char* who, const char* node, A a, const char* why) {
        InteractionRequest q;
        q.player = p;
        q.sequence = ++seq;
        q.node = facility.Find(node);
        q.action = a;
        const InteractionResult r = facility.Submit(q);
        std::printf("t=%5.1fs > %-5s %-14s %-15s -> %-22s (%s)\n", static_cast<double>(facility.CurrentTick()) / 30.0, who,
                    ToString(a), node, ToString(r), why);
    };
    auto command = [&](PlayerId p, const char* who, const char* term, const char* target, A a, const char* why) {
        InteractionRequest q;
        q.player = p;
        q.sequence = ++seq;
        q.node = facility.Find(term);
        q.action = A::Command;
        q.commandTarget = facility.Find(target);
        q.commandAction = a;
        const InteractionResult r = facility.Submit(q);
        std::printf("t=%5.1fs > %-5s Command %-6s %-15s -> %-22s (%s)\n", static_cast<double>(facility.CurrentTick()) / 30.0, who,
                    ToString(a), target, ToString(r), why);
    };
    auto move = [&](PlayerId p, const char* node) { facility.UpdatePlayerPosition(p, at(node)); };

    std::vector<ais::AwarenessState> last(bots.size(), ais::AwarenessState::Unaware);
    const Vec3 gate = at("GATE_EXTRACT");

    for (int t = 0; t < 30 * 118; ++t) {
        switch (t) {
            case 30: act(1, "Volk", "GEN_MAIN", A::Activate, "bring the base back"); break;
            case 200:
                std::printf("           gate illumination now %.2f (was 0.00)\n", facility.IlluminationAt(gate));
                act(2, "Sable", "BRK_INDUSTRIAL", A::InstallFuse, "comms need the industrial circuit");
                break;
            case 230: act(3, "Kit", "TERM_SEC", A::Hack, "4 s hack"); break;
            case 500: command(3, "Kit", "TERM_SEC", "SENSOR_GATE", A::Deactivate, "blind the gate sensor first"); break;
            case 520: command(3, "Kit", "TERM_SEC", "GATE_EXTRACT", A::Activate, "open the extraction gate"); break;
            case 600:
                move(2, "LIGHT_YARD_4");
                act(2, "Sable", "LIGHT_YARD_4", A::Activate, "mistake: one light too many");
                break;
            case 1900:
                std::printf("           (main generator is about to run dry)\n");
                break;
            case 2550:
                move(1, "GEN_MAIN");
                act(1, "Volk", "GEN_MAIN", A::Refuel, "diesel can");
                act(1, "Volk", "GEN_MAIN", A::Activate, "restart main");
                break;
            case 2900:
                facility.UpdatePlayerPosition(1, at("EXTRACT_LIFT"));
                facility.UpdatePlayerPosition(3, at("EXTRACT_LIFT"));
                act(1, "Volk", "EXTRACT_LIFT", A::CallExtraction, "needs two");
                act(3, "Kit", "EXTRACT_LIFT", A::CallExtraction, "joins");
                break;
            default: break;
        }
        facility.Step();
        robots.Step();
        for (std::size_t i = 0; i < bots.size(); ++i) {
            const ais::AcousticDirective* d = robots.Directive(bots[i].id);
            ais::RobotHostState h = *robots.HostState(bots[i].id);
            if (d->hasMoveTarget) {
                const ais::Vec3 delta = d->moveTarget - h.position;
                const double len = delta.Length();
                h.position = len <= 14.0 ? d->moveTarget : h.position + delta * (14.0 / len);
            }
            robots.SetHostState(bots[i].id, h);
            const ais::RobotAcousticBrain* b = robots.Brain(bots[i].id);
            if (b->State() != last[i]) {
                last[i] = b->State();
                std::printf("t=%5.1fs   robot     %-18s -> %-17s reason=%s\n", static_cast<double>(facility.CurrentTick()) / 30.0,
                            bots[i].label, ais::ToString(b->State()), ais::ToString(d->reason));
            }
        }
    }

    std::printf("\nEnd state:\n");
    for (const char* id : {"GEN_MAIN", "GEN_BACKUP"}) {
        std::printf("  %-14s %s\n", id, ToString(facility.State(facility.Find(id)).gen));
    }
    for (const char* id : {"BRK_LIGHTS", "BRK_INDUSTRIAL"}) {
        const NodeState& s = facility.State(facility.Find(id));
        std::printf("  %-14s %s\n", id, s.tripped ? "TRIPPED" : (s.closed ? "closed" : "open"));
    }
    std::printf("  GATE_EXTRACT   %s, EXTRACT_LIFT %s, active alarms %zu\n",
                ToString(facility.State(facility.Find("GATE_EXTRACT")).door),
                facility.State(facility.Find("EXTRACT_LIFT")).completed ? "called" : "not called", facility.ActiveAlarms());
    for (const auto& n : facility.Networks()) {
        std::printf("  network %-10s supply %.1f kW, demand %.1f kW, shed %zu%s\n", n.name.c_str(), n.supplyKw, n.demandKw,
                    n.shedConsumers, n.overloaded ? ", OVERLOADED" : "");
    }
    const FacilityStats& st = facility.Stats();
    std::printf("\nStats: power solves=%llu timers=%llu sounds=%llu (to AIS accepted %llu/%llu) events=%llu\n",
                static_cast<unsigned long long>(st.powerSolves), static_cast<unsigned long long>(st.timersFired),
                static_cast<unsigned long long>(st.soundsEmitted), static_cast<unsigned long long>(bridge.Accepted()),
                static_cast<unsigned long long>(bridge.Submitted()), static_cast<unsigned long long>(st.eventsEmitted));
    std::printf("Facility hash %016llx, robots hash %016llx\n", static_cast<unsigned long long>(facility.StateHash()),
                static_cast<unsigned long long>(robots.StateHash()));
    return 0;
}
