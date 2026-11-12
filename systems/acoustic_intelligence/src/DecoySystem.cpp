// LAST LIGHT - Acoustic Intelligence System (AIS)
#include "ais/DecoySystem.h"

#include <algorithm>

namespace ais {

EntityId DecoySystem::Deploy(const DecoyDefinition& def, const Vec3& position, EntityId owner, Tick now) {
    if (active_.size() >= maxActive_ || !position.IsFinite()) return kNoEntity;
    if (def.intervalTicks < 1 || def.lifetimeTicks < 1 || def.jitterTicks >= def.intervalTicks) return kNoEntity;
    if (!(def.artificiality >= 0.0 && def.artificiality <= 1.0)) return kNoEntity;
    ActiveDecoy d;
    d.id = nextId_++;
    d.owner = owner;
    d.def = def;
    d.position = position;
    d.deployTick = now;
    d.nextEmitTick = now + def.armDelayTicks;
    active_.push_back(d);
    return d.id;
}

EntityId DecoySystem::Deploy(std::string_view name, const Vec3& position, EntityId owner, Tick now) {
    const DecoyDefinition* def = config_.FindDecoy(name);
    return def != nullptr ? Deploy(*def, position, owner, now) : kNoEntity;
}

bool DecoySystem::Destroy(EntityId decoyId) {
    const auto it = std::find_if(active_.begin(), active_.end(), [decoyId](const ActiveDecoy& d) { return d.id == decoyId; });
    if (it == active_.end()) return false;
    active_.erase(it);
    return true;
}

bool DecoySystem::IsActive(EntityId decoyId) const {
    return std::any_of(active_.begin(), active_.end(), [decoyId](const ActiveDecoy& d) { return d.id == decoyId; });
}

Tick DecoySystem::NextInterval(const ActiveDecoy& d) const {
    if (d.def.jitterTicks == 0) return d.def.intervalTicks;
    // Deterministic jitter in [-jitter, +jitter].
    const std::uint64_t h = HashCombine(HashCombine(config_.world.worldSeed, d.id), d.emissions);
    const Tick span = 2 * d.def.jitterTicks + 1;
    const Tick offset = h % span;
    return d.def.intervalTicks - d.def.jitterTicks + offset;
}

void DecoySystem::Update(Tick now, AcousticEventBus& bus) {
    for (ActiveDecoy& d : active_) {
        const bool alive = now < d.deployTick + d.def.lifetimeTicks;
        const bool budgetLeft = d.def.maxEmissions == 0 || d.emissions < d.def.maxEmissions;
        if (!alive || !budgetLeft || now < d.nextEmitTick) continue;
        AcousticEvent e;
        e.category = StimulusCategory::Decoy;
        e.apparentCategory = d.def.apparentCategory;
        e.position = d.position;
        e.emitTick = now;
        e.loudnessDb = d.def.loudnessDb;
        e.spectrumDb = d.def.spectrumDb;
        e.durationTicks = d.def.durationTicks;
        e.sourceId = d.id;
        e.artificiality = d.def.artificiality;
        bus.Submit(e, EventOrigin::ServerAuthoritative, now);
        ++d.emissions;
        ++totalEmissions_;
        d.nextEmitTick = now + NextInterval(d);
    }
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                 [now](const ActiveDecoy& d) {
                                     const bool expired = now + 1 >= d.deployTick + d.def.lifetimeTicks;
                                     const bool spent = d.def.maxEmissions != 0 && d.emissions >= d.def.maxEmissions;
                                     return expired || spent;
                                 }),
                  active_.end());
}

}  // namespace ais
