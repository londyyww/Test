// LAST LIGHT - World Simulation & Scavenging: executable scenarios A-D.
//
//   make scenarios          # all four
//   ./build/scenarios C     # one scenario
//
// A and B run the full environmental stack (power grid, AIS, hazards, weather,
// squads) through MissionWorldHost. C exercises the loot core on its own;
// D runs the whole Dead Frequency map in the world simulation with nobody near
// the relay station. Robot movement is a straight-line stand-in for Unreal AI.
// Each scenario prints a determinism hash; identical runs print identical hashes.
#include <cstdio>
#include <cstring>

#include "StackRig.h"

using namespace wstest;
using llloot::LootAction;
using llloot::LootResultCode;

namespace {

double Sec(llworld::Tick t) { return static_cast<double>(t) / 30.0; }

void PrintLoot(const llloot::LootResult* r, const char* indent = "      ") {
    if (r == nullptr) {
        std::printf("%s(not searched)\n", indent);
        return;
    }
    static const char* reasons[] = {"", "profile empty", "already scavenged", "emptied on evacuation", "destroyed", "nothing rolled"};
    if (r->items.empty()) {
        std::printf("%sempty: %s\n", indent, reasons[static_cast<int>(r->empty)]);
        return;
    }
    for (const auto& i : r->items) {
        std::printf("%s%-26s x%-3u %-18s %-11s integrity %.2f", indent, i.itemId.c_str(), i.quantity, i.categoryId.c_str(), llloot::ToString(i.rarity),
                    i.condition.integrity);
        if (i.condition.fill < 1.0) std::printf("  fill %.2f", i.condition.fill);
        std::printf("  [%s]\n", llloot::ToString(i.condition.grade));
    }
    std::printf("%s(%u/%u slots)\n", indent, r->usedSlots, r->capacity);
}

// Prints world-simulation activity as it happens.
class Narrator final : public llsim::IWorldSimObserver {
public:
    explicit Narrator(const llsim::WorldSimulation& w, const char* filter = nullptr) : w_(w), filter_(filter) {}
    void OnSimEvent(const llsim::SimEvent& e) override {
        if (e.entity == llsim::kNone || !Wanted(e.entity)) return;
        const std::string& id = w_.Config().entities[e.entity].id;
        switch (e.type) {
            case llsim::SimEventType::StateChanged:
                std::printf("t=%7.1fs  world  %-14s %s -> %s\n", Sec(e.tick), id.c_str(), llsim::ToString(static_cast<llsim::EntityState>(e.from)),
                            llsim::ToString(static_cast<llsim::EntityState>(e.to)));
                break;
            case llsim::SimEventType::FuelLow:
            case llsim::SimEventType::FuelEmpty:
            case llsim::SimEventType::BackupStarted:
            case llsim::SimEventType::InstructionQueued:
            case llsim::SimEventType::InstructionDelivered:
                std::printf("t=%7.1fs  world  %-14s %s\n", Sec(e.tick), id.c_str(), llsim::ToString(e.type));
                break;
            default: break;
        }
    }
    void OnPatrolDirective(const llsim::PatrolDirective& d) override {
        if (!Wanted(d.group)) return;
        static const char* kinds[] = {"move to", "hold at", "RESPOND to", "resume route at"};
        const char* area = d.area < w_.Config().areas.size() ? w_.Config().areas[d.area].id.c_str() : "?";
        std::printf("t=%7.1fs  patrol %-14s %s %s%s\n", Sec(d.tick), w_.Config().entities[d.group].id.c_str(), kinds[static_cast<int>(d.kind)], area,
                    d.central ? " (central command)" : "");
    }

private:
    bool Wanted(std::uint32_t e) const { return filter_ == nullptr || w_.Config().entities[e].id.rfind(filter_, 0) == 0; }
    const llsim::WorldSimulation& w_;
    const char* filter_;
};

// ---------------------------------------------------------------- A
std::uint64_t ScenarioA() {
    std::printf("\n=== SCENARIO A - Abandoned military depot (Dead Frequency) ===\n");
    StackRig r({"dead_frequency_depot.ini"}, "scenarios/depot_power.ini", "scenarios/depot_world.ini", 0xDF01);
    r.PrintErrors();
    if (!r.ok) return 0;
    Narrator n(*r.sim, "DF_");
    r.sim->SetObserver(&n);
    r.AddRobot(1, "SENTINEL", {1500, 2400, 0});
    r.AddRobot(2, "SENTINEL", {0, -2500, 0});
    r.AddRobot(3, "WRAITH", {3000, 1200, 0});
    r.Join(1, {0, -1500, 0});
    r.Step(20);
    std::printf("t=%7.1fs  ** players enter the depot yard: DF_DEPOT tier %s, armory %s\n", Sec(r.host->Now()),
                llsim::ToString(r.sim->TierOf(r.E("DF_DEPOT"))), llsim::ToString(r.sim->TierOf(r.E("DF_STORAGE"))));
    r.Move(1, r.Near("DF_ARMORY_CRATE"));
    std::printf("t=%7.1fs  ** search armory crate -> %s\n", Sec(r.host->Now()), llloot::ToString(r.Loot(1, LootAction::Search, "DF_ARMORY_CRATE")));
    r.Move(1, {-250, 300, 0});
    std::printf("t=%7.1fs  ** start depot generator -> %s\n", Sec(r.host->Now()),
                llenv::ToString(r.Facility(1, "GEN_DEPOT", llenv::InteractionAction::Activate)));
    r.Step(90);
    std::printf("t=%7.1fs  ** rack lock powered: %s (loot AccessChanged events: %zu)\n", Sec(r.host->Now()),
                r.host->IsPowered("ARMORY_LOCK") ? "yes" : "no", r.lootRec.Count(llloot::LootEventType::AccessChanged));
    const std::uint32_t patrol = r.E("DF_PATROL");
    while (r.sim->View(patrol).area != r.sim->FindArea("DF_YARD") && r.host->Now() < 3000) r.Step();
    std::printf("t=%7.1fs  ** patrol is sweeping the yard\n", Sec(r.host->Now()));
    r.Move(1, r.Near("DF_ARMORY_CRATE"));
    std::vector<ais::PerceptionRecord> heard;
    r.wi->Acoustic().SetPerceptionObserver(&heard);
    std::printf("t=%7.1fs  ** search armory crate -> %s (AIS event %llu)\n", Sec(r.host->Now()),
                llloot::ToString(r.Loot(1, LootAction::Search, "DF_ARMORY_CRATE")), static_cast<unsigned long long>(r.host->LastLootAcousticEvent()));
    const ais::EventId ev = r.host->LastLootAcousticEvent();
    r.Step(100);
    r.wi->Acoustic().SetPerceptionObserver(nullptr);
    std::set<ais::EntityId> robots;
    for (const auto& p : heard) {
        if (p.eventId == ev) robots.insert(p.robotId);
    }
    for (ais::EntityId id : robots) {
        const llworld::RobotOrder o = r.wi->OrderFor(id);
        std::printf("t=%7.1fs  robot  #%u heard the crate: awareness %s\n", Sec(r.host->Now()), id, ais::ToString(o.awareness));
    }
    std::printf("t=%7.1fs  ** armory crate contents (raid seed 0xDF01):\n", Sec(r.host->Now()));
    PrintLoot(r.loot->Contents(r.loot->Find("DF_ARMORY_CRATE"), 1));
    llworld::StateHasher h;
    h.Add(r.sim->StateHash());
    h.Add(r.loot->StateHash());
    h.Add(robots.size());
    return h.Value();
}

// ---------------------------------------------------------------- B
std::uint64_t ScenarioB() {
    std::printf("\n=== SCENARIO B - Industrial facility, generator on its last fuel ===\n");
    StackRig r({"industrial_facility.ini"}, "scenarios/industrial_power.ini", "scenarios/industrial_world.ini", 0x1D02);
    r.PrintErrors();
    if (!r.ok) return 0;
    Narrator n(*r.sim, "IND_");
    r.sim->SetObserver(&n);
    r.AddRobot(1, "SENTINEL", {63500, 3500, 0});
    r.AddRobot(2, "WRAITH", {60000, 0, 0});
    r.Join(1, {59500, -1500, 0});
    const llenv::NodeIndex gen = r.wi->Facility()->Find("GEN_IND");
    const llenv::NodeIndex emerg = r.wi->Facility()->Find("EMERG_IND");
    for (int s = 0; s < 4; ++s) {
        r.Step(450);
        std::printf("t=%7.1fs  grid   GEN_IND %s, fuel %.0f ticks (power grid owns fuel)\n", Sec(r.host->Now()),
                    llenv::ToString(r.wi->Facility()->State(gen).gen), std::max(0.0, r.wi->Facility()->State(gen).fuel));
    }
    r.Step(30);
    std::printf("t=%7.1fs  grid   emergency light on battery: %s; illumination at hall %.2f\n", Sec(r.host->Now()),
                r.wi->Facility()->State(emerg).onEmergency ? "yes" : "no", r.wi->Facility()->IlluminationAt({60000, 0, 0}));
    r.Move(1, r.Near("IND_DAMAGED_TOOLBOX"));
    std::printf("t=%7.1fs  ** search damaged toolbox -> %s\n", Sec(r.host->Now()), llloot::ToString(r.Loot(1, LootAction::Search, "IND_DAMAGED_TOOLBOX")));
    r.Step(60);
    PrintLoot(r.loot->Contents(r.loot->Find("IND_DAMAGED_TOOLBOX"), 1));
    llworld::StateHasher h;
    h.Add(r.sim->StateHash());
    h.Add(r.loot->StateHash());
    return h.Value();
}

// ---------------------------------------------------------------- C
std::uint64_t ScenarioC() {
    std::printf("\n=== SCENARIO C - Abandoned medical facility ===\n");
    const Content c = Load({"medical_facility.ini"});
    if (!c.ok) return 0;
    llworld::StateHasher h;
    std::uint64_t runHash[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        Access access;
        MemInventory inv;
        llloot::ContainerService svc(c.loot, Catalog());
        svc.SetAccessPort(&access);
        svc.SetGrantPort(&inv);
        svc.BeginRaid(0x3ED);
        std::uint32_t seq = 0;
        auto at = [&svc](const char* id) { return svc.Def(svc.Find(id)).position + llworld::Vec3{60, 0, 0}; };
        auto req = [&svc, &seq](LootAction a, const char* id) { return svc.Submit(llloot::LootRequest{1, ++seq, svc.Find(id), a, 0}); };
        svc.ConnectPlayer(1, 101, at("MED_TREATMENT_CABINET"));
        llworld::Tick now = 0;
        auto wait = [&svc, &now](llworld::Tick t) {
            for (llworld::Tick i = 0; i < t; ++i) svc.Step(++now);
        };
        const char* containers[] = {"MED_TREATMENT_CABINET", "MED_WARD_CABINET", "MED_PHARMACY_CABINET", "MED_XRAY_CABINET", "MED_WARD_FRIDGE",
                                    "MED_CORRIDOR_SHELF",    "MED_NURSE_LOCKER", "MED_HIDDEN_CACHE"};
        for (const char* id : containers) {
            svc.UpdatePlayerPosition(1, at(id));
            LootResultCode code = req(LootAction::Search, id);
            if (run == 0) std::printf("  %-22s %-18s search -> %s\n", id, llloot::ToString(svc.Def(svc.Find(id)).condition), llloot::ToString(code));
            if (code == LootResultCode::RejectedObstructed) {
                req(LootAction::ClearObstruction, id);
                wait(70);
                code = req(LootAction::Search, id);
                if (run == 0) std::printf("  %-22s cleared debris, search -> %s\n", id, llloot::ToString(code));
            }
            if (code == LootResultCode::RejectedHidden) {
                code = req(LootAction::Discover, id);
                if (run == 0) std::printf("  %-22s optional interaction: discover -> %s\n", id, llloot::ToString(code));
                code = req(LootAction::Search, id);
            }
            wait(80);
            if (run == 0) PrintLoot(svc.Contents(svc.Find(id), 1));
        }
        runHash[run] = svc.StateHash();
        h.Add(runHash[run]);
        if (run == 1) std::printf("  replay with the same raid seed: %s\n", runHash[0] == runHash[1] ? "identical containers and contents" : "MISMATCH");
    }
    return h.Value();
}

// ---------------------------------------------------------------- D
std::uint64_t ScenarioD() {
    std::printf("\n=== SCENARIO D - World activity without players nearby ===\n");
    const Content c = Load({kAllLocations[0], kAllLocations[1], kAllLocations[2], kAllLocations[3], kAllLocations[4]});
    if (!c.ok) return 0;
    llsim::WorldSimulation w(c.sim);
    Narrator n(w, "RLY_");
    w.SetObserver(&n);
    w.BeginRaid(0xD0D0);
    // Squad loots the residential block, 2.8 km from the relay station.
    w.SetPlayers({{-60000, 0, 0}, {-60300, 400, 0}, {-59800, -200, 300}});
    w.AdvanceTo(12000);
    const std::uint32_t gen = w.Find("RLY_GEN");
    std::uint64_t relayUpdates = 0;
    std::size_t relayEntities = 0;
    for (std::uint32_t i = 0; i < w.EntityCount(); ++i) {
        if (w.Config().entities[i].id.rfind("RLY_", 0) != 0) continue;
        relayUpdates += w.View(i).updates;
        ++relayEntities;
    }
    std::printf("t=%7.1fs  ** relay station: %zu entities, %llu scheduled updates in 400 s (full rate would be %llu)\n", Sec(w.Now()), relayEntities,
                static_cast<unsigned long long>(relayUpdates), static_cast<unsigned long long>(relayEntities * 12000ull));
    std::printf("t=%7.1fs  ** RLY_GEN tier %s, last update t=%.1fs\n", Sec(w.Now()), llsim::ToString(w.TierOf(gen)), Sec(w.View(gen).lastUpdate));
    // Players travel to the relay.
    w.SetPlayers({{199000, 199500, 0}});
    w.AdvanceTo(12016);
    std::printf("t=%7.1fs  ** players arrive; the station exposes its current state:\n", Sec(w.Now()));
    for (std::uint32_t i = 0; i < w.EntityCount(); ++i) {
        const llsim::EntityView v = w.View(i);
        if (v.def->id.rfind("RLY_", 0) != 0) continue;
        std::printf("      %-13s %-12s tier %-6s", v.def->id.c_str(), llsim::ToString(v.state), llsim::ToString(v.tier));
        if (v.fuelSeconds >= 0.0) std::printf(" fuel %.1f s", v.fuelSeconds);
        std::printf("\n");
    }
    const auto& st = w.Stats();
    std::printf("  totals: %zu entities, %llu updates (high %llu / medium %llu / low %llu), %llu timers, %llu transitions\n", w.EntityCount(),
                static_cast<unsigned long long>(st.Updates()), static_cast<unsigned long long>(st.updatesByTier[0]),
                static_cast<unsigned long long>(st.updatesByTier[1]), static_cast<unsigned long long>(st.updatesByTier[2]),
                static_cast<unsigned long long>(st.timersFired), static_cast<unsigned long long>(st.transitions));
    return w.StateHash();
}

}  // namespace

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    llworld::StateHasher all;
    auto run = [&](char id, std::uint64_t (*fn)()) {
        if (only != nullptr && std::strchr(only, id) == nullptr) return;
        const std::uint64_t h = fn();
        std::printf("--- scenario %c hash %016llx\n", id, static_cast<unsigned long long>(h));
        all.Add(h);
    };
    run('A', ScenarioA);
    run('B', ScenarioB);
    run('C', ScenarioC);
    run('D', ScenarioD);
    std::printf("\ncombined hash %016llx\n", static_cast<unsigned long long>(all.Value()));
    return 0;
}
