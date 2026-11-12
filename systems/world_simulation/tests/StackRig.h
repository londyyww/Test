// Full-stack rig: existing environmental systems (PR #1-#3) + the world loot &
// simulation cores, wired by MissionWorldHost. Shared by the integration tests
// and the scenario runner. Robot movement is a straight-line stand-in for
// Unreal navigation (14 cm/tick toward the AIS / squad move target).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "MissionWorldHost.h"
#include "WsTestHelpers.h"

namespace wstest {

class FacilityInventory final : public llenv::IPlayerInventory {
public:
    bool HasItem(llenv::PlayerId p, std::string_view i) const override { return items.count({p, std::string(i)}) != 0; }
    bool ConsumeItem(llenv::PlayerId p, std::string_view i) override { return items.erase({p, std::string(i)}) != 0; }
    std::set<std::pair<llenv::PlayerId, std::string>> items;
};

struct StackRig {
    Content content;
    llworld::WorldIntelligenceConfig wcfg;
    llenv::FacilityDef fdef;
    ais::AisConfig aisCfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment env;
    FacilityInventory inv;
    MemInventory grants;
    Recorder lootRec;
    SimRecorder simRec;
    std::unique_ptr<llworld::WorldIntelligence> wi;
    std::unique_ptr<llsim::WorldSimulation> sim;
    std::unique_ptr<llloot::ContainerService> loot;
    std::unique_ptr<llws::MissionWorldHost> host;
    std::vector<llworld::RobotId> robots;
    std::vector<std::string> errors;
    std::map<llworld::PlayerId, std::uint32_t> facilitySeq;
    std::map<llworld::PlayerId, std::uint32_t> lootSeq;
    std::map<llworld::WorldEventType, int> worldEvents;
    bool ok = false;
    std::size_t counted = 0;
    StackRig(const StackRig&) = delete;  // the existing systems hold references into the rig
    StackRig& operator=(const StackRig&) = delete;

    // locationFiles: data/locations/*.ini; power/world: paths relative to data/ (or absolute).
    StackRig(std::vector<const char*> locationFiles, const std::string& power, const std::string& world, std::uint64_t raidSeed) {
        std::vector<std::string> paths{Data("common.ini")};
        for (const char* f : locationFiles) paths.push_back(Data(std::string(f).find('/') != std::string::npos ? std::string(f) : std::string("locations/") + f));
        ok = llcontent::LoadAndValidate(paths, Catalog(), content.loot, content.sim, errors);
        std::string text;
        const std::string worldPath = world.front() == '/' ? world : Data(world);
        ok = ok && llworld::ReadFile(worldPath, text) && llworld::LoadWorldConfig(text, wcfg, errors);
        const llenv::LoadResult f = llenv::LoadFacilityFile(power.front() == '/' ? power : Data(power), fdef);
        for (const auto& e : f.errors) errors.push_back("facility: " + e);
        ok = ok && f.ok;
        if (!ok) return;
        wi = std::make_unique<llworld::WorldIntelligence>(wcfg, aisCfg, env, &fdef, &inv);
        for (const auto& e : wi->Errors()) errors.push_back("wi: " + e);
        sim = std::make_unique<llsim::WorldSimulation>(content.sim);
        for (const auto& e : sim->Errors()) errors.push_back("sim: " + e);
        loot = std::make_unique<llloot::ContainerService>(content.loot, Catalog());
        host = std::make_unique<llws::MissionWorldHost>(*wi, *sim, *loot, aisCfg, &inv);
        loot->SetGrantPort(&grants);
        loot->SetObserver(&lootRec);
        sim->SetObserver(&simRec);
        for (const auto& e : host->ValidateBindings()) errors.push_back("binding: " + e);
        ok = wi->IsValid() && sim->IsValid() && errors.empty();
        sim->BeginRaid(raidSeed);
        loot->BeginRaid(raidSeed);
    }
    void PrintErrors() const {
        for (const auto& e : errors) std::printf("    rig: %s\n", e.c_str());
    }

    void AddRobot(llworld::RobotId id, const char* profile, const llworld::Vec3& p) {
        if (wi->AddRobot(id, profile, p)) robots.push_back(id);
    }
    void Step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            const auto& pending = wi->Bus().Current();
            for (std::size_t k = counted; k < pending.size(); ++k) ++worldEvents[pending[k].type];
            host->Step();
            counted = wi->Bus().Current().size();
            for (const auto& e : wi->Bus().Current()) ++worldEvents[e.type];
            for (llworld::RobotId id : robots) {
                const llworld::RobotOrder o = wi->OrderFor(id);
                const ais::Vec3 h = wi->Acoustic().HostState(id)->position;
                llworld::Vec3 p{h.x, h.y, h.z};
                if (o.hasMoveTarget) {
                    const llworld::Vec3 d = o.moveTarget - p;
                    const double len = d.Length();
                    p = len <= 14.0 ? o.moveTarget : p + d * (14.0 / len);
                }
                wi->SetRobotBody(id, p, true, llworld::CommsHealth::Intact);
            }
        }
    }
    bool Join(llworld::PlayerId p, const llworld::Vec3& at) { return host->ConnectPlayer(p, 100 + p, at); }
    void Move(llworld::PlayerId p, const llworld::Vec3& at) { host->MovePlayer(p, at); }
    llworld::Vec3 Near(const char* container) const { return loot->Def(loot->Find(container)).position + llworld::Vec3{60, 0, 0}; }
    llenv::InteractionResult Facility(llworld::PlayerId p, const char* node, llenv::InteractionAction a) {
        llenv::InteractionRequest r;
        r.player = p;
        r.sequence = ++facilitySeq[p];
        r.node = wi->Facility()->Find(node);
        r.action = a;
        return wi->Facility()->Submit(r);
    }
    llloot::LootResultCode Loot(llworld::PlayerId p, llloot::LootAction a, const char* container, std::uint32_t slot = 0) {
        return loot->Submit(llloot::LootRequest{p, ++lootSeq[p], loot->Find(container), a, slot});
    }
    std::uint32_t E(const char* id) const { return sim->Find(id); }
    llsim::EntityState S(const char* id) const { return sim->StateOf(sim->Find(id)); }
    llworld::Vec3 RobotPos(llworld::RobotId id) const {
        const ais::Vec3 h = wi->Acoustic().HostState(id)->position;
        return {h.x, h.y, h.z};
    }
};

}  // namespace wstest
