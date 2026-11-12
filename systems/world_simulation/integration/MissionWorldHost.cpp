// LAST LIGHT - Mission world host implementation.
#include "MissionWorldHost.h"

#include <algorithm>

#include "ais/AcousticConfig.h"

namespace llws {

namespace {
// Gameplay dB at 1 m reported to the world simulation for game-reported combat noise.
constexpr double kGunfireDb = 140.0;
constexpr double kExplosionDb = 150.0;
}  // namespace

MissionWorldHost::MissionWorldHost(llworld::WorldIntelligence& wi, llsim::WorldSimulation& sim, llloot::ContainerService& loot,
                                   const ais::AisConfig& aisCfg, llenv::IPlayerInventory* inventory)
    : wi_(wi), sim_(sim), loot_(loot), aisCfg_(aisCfg), inv_(inventory) {
    loot_.SetAccessPort(this);
    loot_.SetSoundSink(this);
    sim_.SetInfrastructurePort(this);
    const llsim::SimConfig& sc = sim_.Config();
    for (const llsim::EntityDef& e : sc.entities) {
        if (e.bind.rfind("node:", 0) == 0) AddWatch(e.bind.substr(5));
    }
    for (std::uint32_t c = 0; c < loot_.Count(); ++c) {
        for (const llloot::AccessRequirement& a : loot_.Def(c).access) {
            if (a.kind == llloot::AccessKind::Power) AddWatch(a.ref);
        }
    }
}

void MissionWorldHost::AddWatch(const std::string& node) {
    for (const Watch& w : watches_) {
        if (w.node == node) return;
    }
    Watch w;
    w.node = node;
    if (llenv::Facility* f = wi_.Facility()) {
        w.index = f->Find(node);
        if (w.index != llenv::kInvalidNode) {
            const llsim::InfraSample s = SampleNode(node);
            w.powered = s.powered;
            w.running = s.running;
            w.alarm = s.alarm;
        }
    }
    watches_.push_back(w);
}

std::vector<std::string> MissionWorldHost::ValidateBindings() const {
    std::vector<std::string> out;
    const llenv::Facility* f = wi_.Facility();
    for (const Watch& w : watches_) {
        if (f == nullptr || w.index == llenv::kInvalidNode) out.push_back("power-grid node '" + w.node + "' is referenced by content but not defined in the facility");
    }
    for (const llsim::EntityDef& e : sim_.Config().entities) {
        if (e.bind.rfind("hazard:", 0) == 0 && wi_.Hazards().Find(e.bind.substr(7)) == llworld::kInvalidIndex)
            out.push_back("entity '" + e.id + "' binds to unknown hazard '" + e.bind.substr(7) + "'");
    }
    for (std::uint32_t c = 0; c < loot_.Count(); ++c) {
        for (const llloot::AccessRequirement& a : loot_.Def(c).access) {
            if (a.kind == llloot::AccessKind::WorldSeal && sim_.Find(a.ref) == llsim::kNone)
                out.push_back("container '" + loot_.Def(c).id + "' is sealed by unknown world entity '" + a.ref + "'");
        }
    }
    return out;
}

bool MissionWorldHost::ConnectPlayer(llworld::PlayerId id, std::uint64_t key, const llworld::Vec3& p, std::uint32_t permissions) {
    if (llenv::Facility* f = wi_.Facility()) f->ConnectPlayer(id, llenv::Vec3{p.x, p.y, p.z}, permissions);
    if (!loot_.ConnectPlayer(id, key, p)) return false;
    MovePlayer(id, p);
    return true;
}

void MissionWorldHost::MovePlayer(llworld::PlayerId id, const llworld::Vec3& p) {
    if (llenv::Facility* f = wi_.Facility()) f->UpdatePlayerPosition(id, llenv::Vec3{p.x, p.y, p.z});
    loot_.UpdatePlayerPosition(id, p);
    auto it = std::find_if(players_.begin(), players_.end(), [id](const auto& e) { return e.first == id; });
    if (it == players_.end()) players_.emplace_back(id, p);
    else it->second = p;
    std::vector<llworld::Vec3> positions;
    std::vector<llworld::PlayerSample> samples;
    for (const auto& [pid, pos] : players_) {
        positions.push_back(pos);
        llworld::PlayerSample s;
        s.player = pid;
        s.position = pos;
        samples.push_back(s);
    }
    sim_.SetPlayers(positions);
    wi_.SetPlayers(samples);
}

void MissionWorldHost::Step() {
    // Events the game published between ticks sit in the bus until the tick rotates them.
    TranslateEvents(consumed_);
    wi_.Step();
    TranslateEvents(0);
    consumed_ = wi_.Bus().Current().size();
    PollWatches();
    sim_.Step();
    loot_.Step(wi_.CurrentTick());
}

void MissionWorldHost::TranslateEvents(std::size_t from) {
    using T = llworld::WorldEventType;
    const llenv::Facility* f = wi_.Facility();
    const std::vector<llworld::WorldEvent>& events = wi_.Bus().Current();
    for (std::size_t idx = from; idx < events.size(); ++idx) {
        const llworld::WorldEvent& e = events[idx];
        std::string ref;
        if (e.source == llworld::WorldSystem::Facility && f != nullptr && e.subject < f->NodeCount()) ref = f->Def(e.subject).id;
        if (e.source == llworld::WorldSystem::Hazards && e.subject < wi_.Hazards().Count()) ref = wi_.Hazards().Def(e.subject).id;
        llsim::SignalKind k = llsim::SignalKind::Disturbance;
        switch (e.type) {
            case T::AlarmRaised: k = llsim::SignalKind::AlarmRaised; break;
            case T::PowerLost: k = llsim::SignalKind::PowerLost; break;
            case T::PowerRestored: k = llsim::SignalKind::PowerRestored; break;
            case T::HazardActivated: k = llsim::SignalKind::HazardActivated; break;
            case T::HazardDeactivated: k = llsim::SignalKind::HazardCleared; break;
            case T::WeatherChanged:
                k = e.aux == static_cast<std::uint32_t>(llworld::WeatherKind::Storm) ? llsim::SignalKind::WeatherSevere : llsim::SignalKind::WeatherCleared;
                ref.clear();
                break;
            case T::Gunfire: sim_.ReportDisturbance(e.position, kGunfireDb); continue;
            case T::Explosion:
            case T::GasIgnition: sim_.ReportDisturbance(e.position, kExplosionDb); continue;
            default: continue;
        }
        ++signalsForwarded_;
        sim_.NotifySignal(k, ref, e.position, e.magnitude);
    }
}

// Only nodes that content actually references are polled: O(watched), not O(facility).
void MissionWorldHost::PollWatches() {
    for (Watch& w : watches_) {
        if (w.index == llenv::kInvalidNode) continue;
        const llsim::InfraSample s = SampleNode(w.node);
        if (s.powered == w.powered && s.running == w.running && s.alarm == w.alarm) continue;
        w.powered = s.powered;
        w.running = s.running;
        w.alarm = s.alarm;
        sim_.NotifyBoundChanged(w.node);
        loot_.NotifyInfrastructureChanged(w.node);
    }
}

bool MissionWorldHost::IsPowered(std::string_view node) const { return SampleNode(node).powered; }

bool MissionWorldHost::HasItem(llworld::PlayerId player, std::string_view item) const { return inv_ != nullptr && inv_->HasItem(player, item); }

// Clearances are keycard items held by the authoritative inventory.
bool MissionWorldHost::HasClearance(llworld::PlayerId player, std::string_view tag) const { return HasItem(player, tag); }

bool MissionWorldHost::IsSealed(std::string_view entity) const {
    const std::uint32_t i = sim_.Find(entity);
    return i != llsim::kNone && sim_.StateOf(i) == llsim::EntityState::Lockdown;
}

void MissionWorldHost::OnLootSound(const llloot::LootSound& s) {
    // Searches are handled objects; forcing locks and clearing debris are metal on metal.
    const ais::StimulusCategory cat = s.kind == llloot::LootSoundKind::Search ? ais::StimulusCategory::BreakObject : ais::StimulusCategory::MetalImpact;
    ais::AcousticEvent e = ais::MakeEvent(aisCfg_, cat, ais::Vec3{s.position.x, s.position.y, s.position.z}, wi_.Acoustic().CurrentTick(),
                                          ais::SurfaceType::Metal, s.player);
    e.loudnessDb = s.loudnessDb;
    e.durationTicks = std::clamp<ais::Tick>(s.durationTicks, 1, aisCfg_.bus.maxDurationTicks);
    wi_.Acoustic().SubmitEvent(e, ais::EventOrigin::ServerAuthoritative, &lastLootEvent_);
    sim_.ReportDisturbance(s.position, s.loudnessDb);
    ++soundsForwarded_;
}

llsim::InfraSample MissionWorldHost::SampleNode(std::string_view node) const {
    llsim::InfraSample out;
    const llenv::Facility* f = wi_.Facility();
    if (f == nullptr) return out;
    const llenv::NodeIndex i = f->Find(node);
    if (i == llenv::kInvalidNode) return out;
    const llenv::NodeState& s = f->State(i);
    const llenv::NodeType t = f->Def(i).type;
    out.known = true;
    out.powered = llenv::IsSource(t) ? (s.gen == llenv::GeneratorState::Running || s.charge > 0.0) : s.powered;
    out.running = llenv::IsSource(t) ? s.gen == llenv::GeneratorState::Running : s.switchedOn;
    out.alarm = s.alarmActive;
    return out;
}

bool MissionWorldHost::HazardActive(std::string_view hazard) const {
    const std::uint32_t i = wi_.Hazards().Find(hazard);
    return i != llworld::kInvalidIndex && wi_.Hazards().State(i).state == llworld::HazardState::Active;
}

}  // namespace llws
