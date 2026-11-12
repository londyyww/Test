// Shared fixtures for environmental-system tests.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llenv/Facility.h"

namespace envtest {

using namespace llenv;

// Builds one [node.X] section. `extra` is newline-separated key = value pairs.
inline std::string Node(const std::string& id, const std::string& type, const std::string& network,
                        const std::string& fedBy, const std::string& extra = "", const std::string& pos = "0, 0, 0") {
    std::string s = "[node." + id + "]\ntype = " + type + "\nnetwork = " + network + "\nposition = " + pos + "\n";
    if (!fedBy.empty()) s += "fed_by = " + fedBy + "\n";
    return s + extra + "\n";
}

inline std::string Header(const std::string& id = "test") { return "[facility]\nid = " + id + "\nseed = 99\n"; }

class FakeInventory final : public IPlayerInventory {
public:
    bool HasItem(PlayerId p, std::string_view item) const override {
        auto it = items.find({p, std::string(item)});
        return it != items.end() && it->second > 0;
    }
    bool ConsumeItem(PlayerId p, std::string_view item) override {
        ++consumeCalls;
        auto it = items.find({p, std::string(item)});
        if (it == items.end() || it->second <= 0) return false;
        --it->second;
        return true;
    }
    void Give(PlayerId p, const std::string& item, int n = 1) { items[{p, item}] += n; }
    int Count(PlayerId p, const std::string& item) const {
        auto it = items.find({p, item});
        return it == items.end() ? 0 : it->second;
    }
    std::map<std::pair<PlayerId, std::string>, int> items;
    int consumeCalls = 0;
};

class Recorder final : public IFacilityObserver, public IAcousticSink {
public:
    void OnFacilityEvent(const FacilityEvent& e) override { events.push_back(e); }
    void OnSound(const EnvSoundEvent& e) override { sounds.push_back(e); }
    void OnContinuousSound(NodeIndex node, const Vec3&, double, bool active) override { continuous.push_back({node, active}); }
    std::size_t CountEvents(FacilityEventType t, NodeIndex n = kInvalidNode) const {
        std::size_t c = 0;
        for (const auto& e : events) c += (e.type == t && (n == kInvalidNode || e.node == n)) ? 1 : 0;
        return c;
    }
    std::size_t CountSounds(SoundKind k, NodeIndex n = kInvalidNode) const {
        std::size_t c = 0;
        for (const auto& s : sounds) c += (s.kind == k && (n == kInvalidNode || s.node == n)) ? 1 : 0;
        return c;
    }
    std::vector<FacilityEvent> events;
    std::vector<EnvSoundEvent> sounds;
    std::vector<std::pair<NodeIndex, bool>> continuous;
};

struct Rig {
    FacilityDef def;
    LoadResult load;
    std::unique_ptr<Facility> f;
    FakeInventory inv;
    Recorder rec;
    std::map<PlayerId, std::uint32_t> seq;

    explicit Rig(const std::string& ini, AuthorityRole role = AuthorityRole::Server, FacilityConfig cfg = {}) {
        load = LoadFacilityIni(ini, def);
        f = std::make_unique<Facility>(def, role, cfg);
        f->SetObserver(&rec);
        f->SetAcousticSink(&rec);
        f->SetInventory(&inv);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    NodeIndex N(const char* id) const { return f->Find(id); }
    const NodeState& S(const char* id) const { return f->State(N(id)); }

    void Join(PlayerId p, const char* nearNode, std::uint32_t perms = 0) {
        f->ConnectPlayer(p, f->Def(N(nearNode)).position, perms);
    }
    void MoveTo(PlayerId p, const char* node) { f->UpdatePlayerPosition(p, f->Def(N(node)).position); }

    InteractionResult Do(PlayerId p, const char* node, InteractionAction a, std::uint32_t expectedVersion = 0) {
        InteractionRequest r;
        r.player = p;
        r.sequence = ++seq[p];
        r.node = N(node);
        r.action = a;
        r.expectedVersion = expectedVersion;
        return f->Submit(r);
    }
    InteractionResult Command(PlayerId p, const char* terminal, const char* target, InteractionAction a) {
        InteractionRequest r;
        r.player = p;
        r.sequence = ++seq[p];
        r.node = N(terminal);
        r.action = InteractionAction::Command;
        r.commandTarget = N(target);
        r.commandAction = a;
        return f->Submit(r);
    }
    void Run(int ticks) {
        for (int i = 0; i < ticks; ++i) f->Step();
    }
};

}  // namespace envtest
