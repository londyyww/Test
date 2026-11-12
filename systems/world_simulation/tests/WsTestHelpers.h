// Shared helpers for the world simulation & scavenging tests.
#pragma once

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "TestFramework.h"
#include "fixtures/FixtureItemCatalog.h"
#include "llcontent/Content.h"
#include "llloot/ContainerService.h"
#include "llsim/WorldSim.h"

namespace wstest {

inline std::string Data(const std::string& rel) { return std::string(WS_DATA_DIR) + "/" + rel; }

inline const llfixture::FixtureItemCatalog& Catalog() {
    static llfixture::FixtureItemCatalog cat = [] {
        llfixture::FixtureItemCatalog c;
        std::vector<std::string> errors;
        c.Load(Data("fixtures/item_catalog_fixture.ini"), errors);
        for (const auto& e : errors) std::printf("    catalog: %s\n", e.c_str());
        return c;
    }();
    return cat;
}

struct Content {
    llloot::LootConfig loot;
    llsim::SimConfig sim;
    std::vector<std::string> errors;
    bool ok = false;
};

// common.ini plus the given location files, loaded and validated.
inline Content Load(std::initializer_list<const char*> files, bool print = true) {
    Content c;
    std::vector<std::string> paths{Data("common.ini")};
    for (const char* f : files) paths.push_back(Data(std::string("locations/") + f));
    c.ok = llcontent::LoadAndValidate(paths, Catalog(), c.loot, c.sim, c.errors);
    if (print) {
        for (const auto& e : c.errors) std::printf("    content: %s\n", e.c_str());
    }
    return c;
}

// common.ini plus inline text; returns validation messages (empty = valid).
inline std::vector<std::string> Problems(const std::string& text, Content* out = nullptr) {
    Content c;
    std::vector<std::string> errors;
    llcontent::LoadContentFile(Data("common.ini"), c.loot, c.sim, errors);
    if (llcontent::LoadContentText(text, "inline", c.loot, c.sim, errors)) {
        for (const auto& m : llloot::ValidateLoot(c.loot, Catalog())) errors.push_back("loot: " + m);
        for (const auto& m : llsim::ValidateSim(c.sim)) errors.push_back("simulation: " + m);
    }
    if (out != nullptr) *out = c;
    return errors;
}

inline bool AnyContains(const std::vector<std::string>& v, const std::string& needle) {
    return std::any_of(v.begin(), v.end(), [&needle](const std::string& s) { return s.find(needle) != std::string::npos; });
}

// Stand-in for the authoritative inventory: idempotent per grant id.
class MemInventory final : public llloot::ILootGrantPort {
public:
    llloot::GrantOutcome Grant(const llloot::LootGrant& g) override {
        ++calls;
        if (failNext > 0) {
            --failNext;
            if (applyBeforeFailing) applied.emplace(g.grantId, g);  // applied, but the response was lost
            return llloot::GrantOutcome::Unavailable;
        }
        if (applied.count(g.grantId) != 0) return llloot::GrantOutcome::AlreadyApplied;
        if (full.count(g.player) != 0) return llloot::GrantOutcome::Rejected;
        applied.emplace(g.grantId, g);
        received[g.player].push_back(g);
        return llloot::GrantOutcome::Accepted;
    }
    std::map<std::uint64_t, llloot::LootGrant> applied;
    std::map<llloot::PlayerId, std::vector<llloot::LootGrant>> received;
    std::set<llloot::PlayerId> full;
    int failNext = 0;
    bool applyBeforeFailing = false;
    int calls = 0;
};

class Access final : public llloot::ILootAccessPort {
public:
    bool IsPowered(std::string_view n) const override { return powered.count(std::string(n)) != 0; }
    bool HasItem(llloot::PlayerId p, std::string_view i) const override { return items.count({p, std::string(i)}) != 0; }
    bool HasClearance(llloot::PlayerId p, std::string_view t) const override { return HasItem(p, t); }
    bool IsSealed(std::string_view e) const override { return sealed.count(std::string(e)) != 0; }
    std::set<std::string> powered;
    std::set<std::pair<llloot::PlayerId, std::string>> items;
    std::set<std::string> sealed;
};

class Recorder final : public llloot::ILootObserver, public llloot::ILootSoundSink {
public:
    void OnLootEvent(const llloot::LootEvent& e) override { events.push_back(e); }
    void OnLootSound(const llloot::LootSound& s) override { sounds.push_back(s); }
    std::size_t Count(llloot::LootEventType t) const {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [t](const auto& e) { return e.type == t; }));
    }
    std::vector<llloot::LootEvent> events;
    std::vector<llloot::LootSound> sounds;
};

class SimRecorder final : public llsim::IWorldSimObserver {
public:
    void OnSimEvent(const llsim::SimEvent& e) override { events.push_back(e); }
    void OnPatrolDirective(const llsim::PatrolDirective& d) override { directives.push_back(d); }
    std::size_t Count(llsim::SimEventType t, std::uint32_t entity = llsim::kNone) const {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [t, entity](const auto& e) {
            return e.type == t && (entity == llsim::kNone || e.entity == entity);
        }));
    }
    std::vector<llsim::SimEvent> events;
    std::vector<llsim::PatrolDirective> directives;
};

inline constexpr const char* kAllLocations[] = {"dead_frequency_depot.ini", "dead_frequency_relay_station.ini", "industrial_facility.ini",
                                                "medical_facility.ini", "residential_building.ini"};

}  // namespace wstest
