// LAST LIGHT - Acoustic Intelligence System (AIS)
#include "ais/AcousticWorld.h"

#include <algorithm>
#include <cmath>

namespace ais {

namespace {
constexpr double kTwoPi = 6.283185307179586;

std::int64_t CellOf(double v, double cell) { return static_cast<std::int64_t>(std::floor(v / cell)); }
}  // namespace

AcousticWorld::AcousticWorld(const AisConfig& config, const IAcousticEnvironment& environment, AuthorityRole role,
                             ISignalNetworkPort* signalNetwork)
    : config_(config),
      env_(environment),
      role_(role),
      propagation_(config, environment),
      bus_(config),
      decoys_(config),
      relay_(signalNetwork) {
    if (relay_ == nullptr) {
        ownedRelay_ = std::make_unique<LocalSignalRelay>(config_.world);
        relay_ = ownedRelay_.get();
    }
    RecomputeBestThreshold();
}

AcousticWorld::RobotSlot* AcousticWorld::FindSlot(EntityId id) {
    auto it = std::lower_bound(robots_.begin(), robots_.end(), id, [](const RobotSlot& s, EntityId v) { return s.id < v; });
    return (it != robots_.end() && it->id == id) ? &*it : nullptr;
}

const AcousticWorld::RobotSlot* AcousticWorld::FindSlot(EntityId id) const {
    auto it = std::lower_bound(robots_.begin(), robots_.end(), id, [](const RobotSlot& s, EntityId v) { return s.id < v; });
    return (it != robots_.end() && it->id == id) ? &*it : nullptr;
}

bool AcousticWorld::AddRobot(EntityId robotId, std::string_view profileName, const Vec3& position) {
    const RobotSensoryProfile* p = config_.FindProfile(profileName);
    return p != nullptr && AddRobot(robotId, *p, position);
}

bool AcousticWorld::AddRobot(EntityId robotId, const RobotSensoryProfile& profile, const Vec3& position) {
    if (role_ != AuthorityRole::Server || robotId == kNoEntity || !position.IsFinite() || FindSlot(robotId) != nullptr) {
        return false;
    }
    RobotSlot slot;
    slot.id = robotId;
    slot.host.position = position;
    slot.brain = std::make_unique<RobotAcousticBrain>(robotId, profile, config_.world.worldSeed);
    auto it = std::lower_bound(robots_.begin(), robots_.end(), robotId, [](const RobotSlot& s, EntityId v) { return s.id < v; });
    robots_.insert(it, std::move(slot));
    RecomputeBestThreshold();
    return true;
}

bool AcousticWorld::RemoveRobot(EntityId robotId) {
    auto it = std::lower_bound(robots_.begin(), robots_.end(), robotId, [](const RobotSlot& s, EntityId v) { return s.id < v; });
    if (it == robots_.end() || it->id != robotId) return false;
    robots_.erase(it);
    RecomputeBestThreshold();
    return true;
}

bool AcousticWorld::SetHostState(EntityId robotId, const RobotHostState& host) {
    RobotSlot* s = FindSlot(robotId);
    if (s == nullptr || !host.position.IsFinite() || !std::isfinite(host.objectivePriority)) return false;
    if (host.hasVisualContact && !host.visualContactPosition.IsFinite()) return false;
    s->host = host;
    return true;
}

const RobotHostState* AcousticWorld::HostState(EntityId robotId) const {
    const RobotSlot* s = FindSlot(robotId);
    return s != nullptr ? &s->host : nullptr;
}

const RobotAcousticBrain* AcousticWorld::Brain(EntityId robotId) const {
    const RobotSlot* s = FindSlot(robotId);
    return s != nullptr ? s->brain.get() : nullptr;
}

const AcousticDirective* AcousticWorld::Directive(EntityId robotId) const {
    const RobotSlot* s = FindSlot(robotId);
    return s != nullptr ? &s->brain->Directive() : nullptr;
}

SubmitResult AcousticWorld::SubmitEvent(const AcousticEvent& event, EventOrigin origin, EventId* outId) {
    if (role_ != AuthorityRole::Server) {
        if (outId != nullptr) *outId = 0;
        return SubmitResult::RejectedNotAuthoritative;
    }
    return bus_.Submit(event, origin, tick_, outId);
}

void AcousticWorld::SetClientListeners(const std::vector<ClientListener>& listeners) {
    listeners_ = listeners;
    std::sort(listeners_.begin(), listeners_.end(),
              [](const ClientListener& a, const ClientListener& b) { return a.clientId < b.clientId; });
}

void AcousticWorld::RecomputeBestThreshold() {
    const PropagationConfig& p = config_.propagation;
    BandArray best{1e9, 1e9, 1e9};
    for (const RobotSlot& s : robots_) {
        const RobotSensoryProfile& prof = s.brain->Profile();
        for (std::size_t b = 0; b < kBandCount; ++b) {
            // Weather and ambient only raise floors, so the base floor is the best case.
            const double floor = std::min(p.exteriorNoiseFloorDb[b], p.interiorNoiseFloorDb[b]);
            best[b] = std::min(best[b], floor - prof.bandSensitivityDb[b] + prof.detectionSnrDb);
        }
    }
    bestThresholdDb_ = robots_.empty() ? BandArray{0.0, 0.0, 0.0} : best;
}

void AcousticWorld::RebuildGrid() {
    grid_.clear();
    const double cell = config_.world.gridCellCm;
    for (std::size_t i = 0; i < robots_.size(); ++i) {
        const Vec3& p = robots_[i].host.position;
        grid_.push_back(GridCell{CellOf(p.x, cell), CellOf(p.y, cell), i});
    }
    std::sort(grid_.begin(), grid_.end(), [](const GridCell& a, const GridCell& b) {
        if (a.cx != b.cx) return a.cx < b.cx;
        if (a.cy != b.cy) return a.cy < b.cy;
        return a.robotIndex < b.robotIndex;
    });
}

void AcousticWorld::QueryGrid(const Vec3& center, double radius, std::vector<std::size_t>& out) const {
    out.clear();
    const double cell = config_.world.gridCellCm;
    const std::int64_t x0 = CellOf(center.x - radius, cell);
    const std::int64_t x1 = CellOf(center.x + radius, cell);
    const std::int64_t y0 = CellOf(center.y - radius, cell);
    const std::int64_t y1 = CellOf(center.y + radius, cell);
    const auto span = static_cast<double>(x1 - x0 + 1) * static_cast<double>(y1 - y0 + 1);
    const double r2 = radius * radius;
    if (span > static_cast<double>(grid_.size())) {
        // Huge radius (explosions): a linear pass is cheaper than visiting cells.
        for (std::size_t i = 0; i < robots_.size(); ++i) {
            if (DistanceSq(robots_[i].host.position, center) <= r2) out.push_back(i);
        }
        return;
    }
    for (std::int64_t cx = x0; cx <= x1; ++cx) {
        const GridCell lo{cx, y0, 0};
        auto it = std::lower_bound(grid_.begin(), grid_.end(), lo, [](const GridCell& a, const GridCell& b) {
            return a.cx != b.cx ? a.cx < b.cx : a.cy < b.cy;
        });
        for (; it != grid_.end() && it->cx == cx && it->cy <= y1; ++it) {
            if (DistanceSq(robots_[it->robotIndex].host.position, center) <= r2) out.push_back(it->robotIndex);
        }
    }
    std::sort(out.begin(), out.end());
}

void AcousticWorld::ProcessEvent(const AcousticEvent& e) {
    ++stats_.eventsProcessed;
    const CategoryDescriptor& cat = config_.categories[Index(e.apparentCategory)];
    for (std::size_t idx : candidateScratch_) {
        RobotSlot& slot = robots_[idx];
        RobotAcousticBrain& brain = *slot.brain;
        const RobotSensoryProfile& prof = brain.Profile();
        ++stats_.perceptionChecks;
        ++stats_.occlusionTraces;
        const PropagationResult r = propagation_.Evaluate(e, slot.host.position);
        double effective = -1e9;
        for (std::size_t b = 0; b < kBandCount; ++b) {
            effective = std::max(effective, r.snrDb[b] + prof.bandSensitivityDb[b]);
        }
        if (effective < prof.detectionSnrDb) continue;

        const double err = propagation_.LocalizationErrorCm(r, effective, e.durationTicks) / prof.localizationSkill;
        // Per (event, robot) noise: robots hearing the same sound disagree.
        const std::uint64_t h = HashCombine(HashCombine(config_.world.worldSeed, e.id), slot.id);
        const double angle = kTwoPi * HashToUnit(h);
        const double magnitude = err * (0.2 + 0.75 * HashToUnit(Mix64(h)));  // keeps |offset| < err incl. dz
        const double dz = (HashToUnit(Mix64(h + 1)) - 0.5) * err * 0.25;

        PerceivedStimulus s;
        s.eventId = e.id;
        s.apparentCategory = e.apparentCategory;
        s.estimatedPosition = e.position + Vec3{std::cos(angle) * magnitude, std::sin(angle) * magnitude, dz};
        s.uncertaintyCm = err;
        s.effectiveSnrDb = effective;
        s.artificialCue = e.artificiality * Clamp01((effective - prof.detectionSnrDb) / 15.0);
        s.threat = cat.threat;
        s.hostileCategory = cat.hostile;
        s.regularityIsSuspicious = cat.regularityIsSuspicious;
        s.tick = e.emitTick;
        brain.Enqueue(s);
        ++stats_.perceptionsDelivered;
        if (observer_ != nullptr) observer_->push_back(PerceptionRecord{slot.id, e.id, e.position, s});
    }
}

void AcousticWorld::EmitCosmetic(const AcousticEvent& e, double audibleRadius) {
    const double r2 = audibleRadius * audibleRadius;
    for (std::size_t i = 0; i < listeners_.size(); ++i) {
        if (cuesPerListener_[i] >= config_.world.maxCosmeticCuesPerClientPerTick) continue;
        if (DistanceSq(listeners_[i].position, e.position) > r2) continue;
        CosmeticAudioCue c;
        c.clientId = listeners_[i].clientId;
        c.eventId = e.id;
        c.apparentCategory = e.apparentCategory;
        c.surface = e.surface;
        c.position = e.position;
        c.loudnessDb = e.loudnessDb;
        cues_.push_back(c);
        ++cuesPerListener_[i];
        ++stats_.cosmeticCues;
    }
}

void AcousticWorld::DeliverReports() {
    receiverScratch_.clear();
    for (const RobotSlot& s : robots_) {
        receiverScratch_.push_back(RelayReceiver{s.id, s.host.position, s.brain->Profile().receivesReports});
    }
    reportScratch_.clear();
    relay_->Deliver(tick_, receiverScratch_, reportScratch_);
    for (const DeliveredReport& dr : reportScratch_) {
        RobotSlot* slot = FindSlot(dr.first);
        if (slot == nullptr || !slot->brain->Profile().receivesReports) continue;
        const AcousticReport& r = dr.second;
        const RobotSensoryProfile& prof = slot->brain->Profile();
        const CategoryDescriptor& cat = config_.categories[Index(r.category)];
        PerceivedStimulus s;
        s.apparentCategory = r.category;
        s.estimatedPosition = r.estimate;
        s.uncertaintyCm = r.uncertaintyCm + prof.sharedUncertaintyInflationCm;
        s.effectiveSnrDb = prof.detectionSnrDb + 18.0 * Clamp01(r.confidence);
        s.threat = cat.threat;
        s.hostileCategory = cat.hostile;
        s.regularityIsSuspicious = cat.regularityIsSuspicious;
        s.fromSharedReport = true;
        s.sharedSuspectedArtificial = r.suspectedArtificial;
        s.tick = r.sentTick;
        slot->brain->Enqueue(s);
        if (r.claimingInvestigation) slot->brain->NotifyAllyInvestigating(r.senderId, r.estimate, tick_);
        ++stats_.reportsDelivered;
    }
}

void AcousticWorld::Step() {
    if (role_ != AuthorityRole::Server) return;
    ++tick_;
    cues_.clear();
    cuesPerListener_.assign(listeners_.size(), 0);

    decoys_.Update(tick_, bus_);
    RebuildGrid();

    drainScratch_.clear();
    bus_.Drain(tick_, drainScratch_);
    const std::size_t budget = config_.world.maxPerceptionChecksPerTick;
    BandArray humanThreshold{};
    for (std::size_t b = 0; b < kBandCount; ++b) {
        humanThreshold[b] = std::min(config_.propagation.exteriorNoiseFloorDb[b], config_.propagation.interiorNoiseFloorDb[b]);
    }
    std::size_t checks = 0;
    for (std::size_t i = 0; i < drainScratch_.size(); ++i) {
        const AcousticEvent& e = drainScratch_[i];
        const double radius = robots_.empty() ? 0.0 : propagation_.MaxAudibleRadiusCm(e, bestThresholdDb_);
        QueryGrid(e.position, radius, candidateScratch_);
        if (checks > 0 && checks + candidateScratch_.size() > budget) {
            // Over budget: defer the rest (they still age out via the bus).
            for (std::size_t j = i; j < drainScratch_.size(); ++j) bus_.Requeue(drainScratch_[j]);
            stats_.eventsDeferred += drainScratch_.size() - i;
            break;
        }
        checks += candidateScratch_.size();
        ProcessEvent(e);
        if (!listeners_.empty()) {
            EmitCosmetic(e, propagation_.MaxAudibleRadiusCm(e, humanThreshold) * config_.world.cosmeticRadiusScale);
        }
    }

    DeliverReports();

    for (RobotSlot& slot : robots_) {
        if (!slot.brain->IsUpdateDue(tick_)) continue;
        AcousticReport report;
        bool hasReport = false;
        slot.brain->Update(tick_, slot.host, &report, &hasReport);
        ++stats_.brainUpdates;
        if (hasReport && relay_->Publish(report)) ++stats_.reportsPublished;
    }
}

std::uint64_t AcousticWorld::StateHash() const {
    StateHasher h;
    h.Add(tick_);
    for (std::size_t i = 0; i < kSubmitResultCount; ++i) h.Add(bus_.Stats().results[i]);
    h.Add(bus_.QueueSize());
    h.Add(decoys_.ActiveCount());
    h.Add(decoys_.TotalEmissions());
    for (const RobotSlot& s : robots_) s.brain->HashInto(h);
    return h.Value();
}

}  // namespace ais
