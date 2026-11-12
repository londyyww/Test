// LAST LIGHT - Environmental & World Intelligence: integration layer.
#include "WorldIntelligence.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace llworld {

namespace {

ais::Vec3 ToAis(const Vec3& v) { return ais::Vec3{v.x, v.y, v.z}; }
Vec3 FromAis(const ais::Vec3& v) { return Vec3{v.x, v.y, v.z}; }
llenv::Vec3 ToEnv(const Vec3& v) { return llenv::Vec3{v.x, v.y, v.z}; }
Vec3 FromEnv(const llenv::Vec3& v) { return Vec3{v.x, v.y, v.z}; }

constexpr ais::EntityId kHazardSourceBase = 0xC0000000u;
constexpr ais::EntityId kWeatherSourceBase = 0xB0000000u;

ThreatClass ThreatOf(ais::StimulusCategory c) {
    using C = ais::StimulusCategory;
    switch (c) {
        case C::Gunfire:
        case C::Explosion: return ThreatClass::Hostile;
        case C::Footstep:
        case C::Sprint:
        case C::JumpLanding:
        case C::Door:
        case C::BreakObject: return ThreatClass::Movement;
        case C::Alarm:
        case C::Generator:
        case C::Machinery: return ThreatClass::Infrastructure;
        case C::MetalImpact:
        case C::Environmental:
        case C::Decoy: return ThreatClass::Disturbance;
    }
    return ThreatClass::Disturbance;
}

ais::StimulusCategory CategoryOf(HazardSound s) {
    using C = ais::StimulusCategory;
    switch (s) {
        case HazardSound::FireIgnite: return C::BreakObject;
        case HazardSound::FireCrackle: return C::Environmental;
        case HazardSound::GasHiss: return C::Environmental;
        case HazardSound::GasIgnition: return C::Explosion;
        case HazardSound::ElectricalArc: return C::Machinery;
        case HazardSound::StructureCreak: return C::MetalImpact;
        case HazardSound::StructureCollapse: return C::BreakObject;
        case HazardSound::MachineryGrind: return C::Machinery;
    }
    return C::Environmental;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ports over the power grid (and AIS for hazard sounds)
// ---------------------------------------------------------------------------

class WorldIntelligence::FacilityPorts final : public llenv::IFacilityObserver, public IHazardWorld, public ISquadWorld {
public:
    FacilityPorts(WorldIntelligence& wi, llenv::IPlayerInventory* inv) : wi_(wi), inv_(inv) {}

    // llenv::IFacilityObserver -> shared world events
    void OnFacilityEvent(const llenv::FacilityEvent& e) override {
        using T = llenv::FacilityEventType;
        llenv::Facility* f = wi_.facility_.get();
        if (f == nullptr || e.node == llenv::kInvalidNode) return;
        WorldEvent w;
        w.source = WorldSystem::Facility;
        w.position = FromEnv(f->Def(e.node).position);
        w.subject = e.node;
        bool publish = true;
        switch (e.type) {
            case T::AlarmTriggered: w.type = WorldEventType::AlarmRaised; break;
            case T::GeneratorState: {
                const auto s = static_cast<llenv::GeneratorState>(e.value);
                if (s == llenv::GeneratorState::Running) w.type = WorldEventType::PowerRestored;
                else if (s == llenv::GeneratorState::Off || s == llenv::GeneratorState::Tripped || s == llenv::GeneratorState::Destroyed)
                    w.type = WorldEventType::PowerLost;
                else publish = false;
                break;
            }
            case T::LightChanged:
                publish = f->Def(e.node).type == llenv::NodeType::EmergencyLight && e.value > 0;
                w.type = WorldEventType::EmergencyLighting;
                break;
            default: publish = false; break;
        }
        if (wi_.hazards_ != nullptr) wi_.hazards_->NotifyInfrastructureChanged();
        if (publish) wi_.bus_.Publish(w);
    }

    // IHazardWorld
    int ResolveNode(std::string_view id) const override {
        if (wi_.facility_ == nullptr) return -1;
        const llenv::NodeIndex i = wi_.facility_->Find(id);
        return i == llenv::kInvalidNode ? -1 : static_cast<int>(i);
    }
    bool NodeEnergized(int node) const override {
        return wi_.facility_ != nullptr && wi_.facility_->State(static_cast<llenv::NodeIndex>(node)).energized;
    }
    bool NodeRunning(int node) const override {
        return wi_.facility_ != nullptr && wi_.facility_->State(static_cast<llenv::NodeIndex>(node)).soundActive;
    }
    void DamageNode(int node, double amount) override {
        if (wi_.facility_ != nullptr) wi_.facility_->ServerApplyDamage(static_cast<llenv::NodeIndex>(node), amount);
    }
    void TriggerAlarm(int node) override {
        if (wi_.facility_ != nullptr) wi_.facility_->ServerTriggerAlarm(static_cast<llenv::NodeIndex>(node));
    }
    void EmitSound(HazardSound s, const Vec3& p, double db, Tick duration, std::uint32_t hazard) override {
        wi_.SubmitAcoustic(CategoryOf(s), p, db, kHazardSourceBase + hazard, duration);
    }
    bool PlayerHasItem(PlayerId p, std::string_view item) const override { return inv_ != nullptr && inv_->HasItem(p, item); }
    bool ConsumeItem(PlayerId p, std::string_view item) override { return inv_ != nullptr && inv_->ConsumeItem(p, item); }

    // ISquadWorld: a relay works while its CommArray is powered and switched on.
    bool RelayOperational(std::string_view relay) const override {
        if (wi_.facility_ == nullptr) return false;
        const llenv::NodeIndex i = wi_.facility_->Find(relay);
        if (i == llenv::kInvalidNode) return false;
        const llenv::NodeState& s = wi_.facility_->State(i);
        return s.powered && s.switchedOn && !s.destroyed;
    }

private:
    WorldIntelligence& wi_;
    llenv::IPlayerInventory* inv_;
};

// AIS robot-to-robot acoustic sharing respects the squad comms topology.
class WorldIntelligence::SquadSignalPort final : public ais::ISignalNetworkPort {
public:
    SquadSignalPort(WorldIntelligence& wi, const ais::WorldConfig& c) : wi_(wi), relay_(c) {}
    bool Publish(const ais::AcousticReport& r) override { return relay_.Publish(r); }
    void Deliver(ais::Tick now, const std::vector<ais::RelayReceiver>& receivers, std::vector<ais::DeliveredReport>& out) override {
        std::vector<ais::DeliveredReport> raw;
        relay_.Deliver(now, receivers, raw);
        for (const auto& d : raw) {
            if (wi_.squads_ == nullptr || wi_.squads_->SquadOf(d.second.senderId) == kInvalidIndex ||
                wi_.squads_->CanCommunicate(d.second.senderId, d.first)) {
                out.push_back(d);
            }
        }
    }

private:
    WorldIntelligence& wi_;
    ais::LocalSignalRelay relay_;
};

// ---------------------------------------------------------------------------
// Configuration loading
// ---------------------------------------------------------------------------

bool LoadWorldConfig(const std::string& text, WorldIntelligenceConfig& cfg, std::vector<std::string>& errors) {
    const std::size_t before = errors.size();
    const IniDocument doc = ParseIni(text, errors);
    for (const IniSection& s : doc.sections) {
        const std::string& n = s.name;
        const bool known = n == "world" || n == "weather" || n == "hazards" || n == "squads" || n.rfind("weather.", 0) == 0 ||
                           n.rfind("gust.", 0) == 0 || n.rfind("hazard.", 0) == 0 || n.rfind("role.", 0) == 0 ||
                           n.rfind("squad.", 0) == 0 || n.rfind("route.", 0) == 0;
        if (!known) errors.push_back("line " + std::to_string(s.line) + ": unknown section [" + n + "]");
        if (n == "world") {
            IniBinder b;
            b.List("exposed_nodes", cfg.exposedNodes);
            b.Num("report_min_confidence", cfg.reportMinConfidence);
            b.Num("min_detection_range_cm", cfg.minDetectionRangeCm);
            b.Apply(s, errors);
        } else if (n.rfind("route.", 0) == 0) {
            RouteDef r;
            r.name = n.substr(6);
            IniBinder b;
            b.U32("id", r.id);
            b.Points("waypoints", r.waypoints);
            b.Apply(s, errors);
            if (r.id == 0) errors.push_back("[" + n + "] needs id >= 1");
            cfg.routes.push_back(r);
        }
    }
    LoadWeather(doc, cfg.weather, errors);
    LoadHazards(doc, cfg.hazards, errors);
    LoadSquads(doc, cfg.squads, errors);
    for (const auto& m : ValidateWeather(cfg.weather)) errors.push_back("validation: " + m);
    for (const auto& m : ValidateHazards(cfg.hazards)) errors.push_back("validation: " + m);
    for (const auto& m : ValidateSquads(cfg.squads)) errors.push_back("validation: " + m);
    return errors.size() == before;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WorldIntelligence::WorldIntelligence(const WorldIntelligenceConfig& config, const ais::AisConfig& aisConfig,
                                     ais::BoxAcousticEnvironment& aisEnv, const llenv::FacilityDef* facilityDef,
                                     llenv::IPlayerInventory* inventory)
    : cfg_(config), aisCfg_(aisConfig), aisEnv_(aisEnv), bus_(config.maxEventsPerTick) {
    if (facilityDef != nullptr) {
        facility_ = std::make_unique<llenv::Facility>(*facilityDef);
        for (const auto& e : facility_->Errors()) errors_.push_back("facility: " + e);
    }
    signal_ = std::make_unique<SquadSignalPort>(*this, aisCfg_.world);
    ais_ = std::make_unique<ais::AcousticWorld>(aisCfg_, aisEnv_, ais::AuthorityRole::Server, signal_.get());
    ports_ = std::make_unique<FacilityPorts>(*this, inventory);
    if (facility_ != nullptr) {
        bridge_ = std::make_unique<llenv::AisAcousticBridge>(aisCfg_, *ais_, &aisEnv_);
        facility_->SetAcousticSink(bridge_.get());
        facility_->SetObserver(ports_.get());
        facility_->SetInventory(inventory);
    }
    weather_ = std::make_unique<WeatherSystem>(cfg_.weather, &bus_);
    hazards_ = std::make_unique<HazardSystem>(cfg_.hazards, &bus_, ports_.get());
    squads_ = std::make_unique<SquadCoordinator>(cfg_.squads, &bus_, ports_.get());
    // Configured robots count only once the engine spawns them (AddRobot); until then a
    // declared member must not relay squad comms from a default position.
    for (const SquadDef& sq : cfg_.squads.squads) {
        for (const SquadMemberDef& m : sq.members) squads_->UpdateRobot(m.robot, Vec3{}, false, CommsHealth::Intact);
    }
    for (const auto& e : ValidateWeather(cfg_.weather)) errors_.push_back("weather: " + e);
    for (const auto& e : hazards_->Errors()) errors_.push_back("hazards: " + e);
    for (const auto& e : squads_->Errors()) errors_.push_back("squads: " + e);
    for (const auto& id : cfg_.exposedNodes) {
        const int k = ports_->ResolveNode(id);
        if (k < 0) errors_.push_back("world: exposed node '" + id + "' not found");
        else exposedNodes_.push_back(k);
    }
}

WorldIntelligence::~WorldIntelligence() = default;

bool WorldIntelligence::AddRobot(RobotId id, const char* aisProfile, const Vec3& position) {
    if (!ais_->AddRobot(id, aisProfile, ToAis(position))) return false;
    robots_.push_back(id);
    std::sort(robots_.begin(), robots_.end());
    ais::RobotHostState h;
    h.position = ToAis(position);
    h.objectivePriority = squads_->ObjectivePriorityFor(id);
    ais_->SetHostState(id, h);
    squads_->UpdateRobot(id, position, true, CommsHealth::Intact);
    return true;
}

bool WorldIntelligence::SetRobotBody(RobotId id, const Vec3& position, bool alive, CommsHealth comms) {
    const ais::RobotHostState* cur = ais_->HostState(id);
    if (cur == nullptr || !position.IsFinite()) return false;
    ais::RobotHostState h = *cur;
    h.position = ToAis(position);
    ais_->SetHostState(id, h);
    squads_->UpdateRobot(id, position, alive, comms);
    return true;
}

void WorldIntelligence::SetPlayers(const std::vector<PlayerSample>& players) {
    players_.clear();
    for (const auto& p : players) {
        if (p.position.IsFinite() && players_.size() < 4) players_.push_back(p);
    }
}

// ---------------------------------------------------------------------------
// Game inputs
// ---------------------------------------------------------------------------

void WorldIntelligence::SubmitAcoustic(ais::StimulusCategory c, const Vec3& p, double db, ais::EntityId source, ais::Tick duration) {
    ais::AcousticEvent e = ais::MakeEvent(aisCfg_, c, ToAis(p), ais_->CurrentTick(), ais::SurfaceType::None, source);
    e.loudnessDb = db;
    e.durationTicks = std::clamp<ais::Tick>(duration, 1, aisCfg_.bus.maxDurationTicks);
    ais_->SubmitEvent(e, ais::EventOrigin::ServerAuthoritative);
}

ais::SubmitResult WorldIntelligence::PlayerFootstep(PlayerId player, const Vec3& pos, ais::SurfaceType surface, double offsetDb) {
    // Puddles splash: louder than the dry surface, but rain may still mask it.
    ais::SurfaceType s = surface;
    if (weather_->Surface().puddles && surface != ais::SurfaceType::Metal && surface != ais::SurfaceType::Wood) s = ais::SurfaceType::Water;
    ais::AcousticEvent e = ais::MakeEvent(aisCfg_, ais::StimulusCategory::Footstep, ToAis(pos), ais_->CurrentTick(), s, player);
    e.loudnessDb += offsetDb;
    return ais_->SubmitEvent(e, ais::EventOrigin::ServerAuthoritative);
}

void WorldIntelligence::ReportGunfire(PlayerId shooter, const Vec3& pos) {
    SubmitAcoustic(ais::StimulusCategory::Gunfire, pos, aisCfg_.categories[ais::Index(ais::StimulusCategory::Gunfire)].loudnessDb, shooter, 1);
    WorldEvent w;
    w.type = WorldEventType::Gunfire;
    w.source = WorldSystem::Game;
    w.position = pos;
    w.radiusCm = 100.0;
    w.subject = shooter;
    bus_.Publish(w);
}

void WorldIntelligence::ReportExplosion(const Vec3& pos, double radiusCm) {
    SubmitAcoustic(ais::StimulusCategory::Explosion, pos, aisCfg_.categories[ais::Index(ais::StimulusCategory::Explosion)].loudnessDb,
                   0, 6);
    WorldEvent w;
    w.type = WorldEventType::Explosion;
    w.source = WorldSystem::Game;
    w.position = pos;
    w.radiusCm = std::isfinite(radiusCm) ? std::max(0.0, radiusCm) : 0.0;
    bus_.Publish(w);
}

// ---------------------------------------------------------------------------
// Step
// ---------------------------------------------------------------------------

void WorldIntelligence::ForwardWeather() {
    const WeatherSnapshot& w = weather_->Snapshot();
    if (w.version != lastWeatherVersion_) {
        lastWeatherVersion_ = w.version;
        aisEnv_.SetWeather(ais::WeatherState{w.rain, w.wind});  // AIS models the noise floors itself
    }
    for (const WorldEvent& e : bus_.Current()) {
        if (e.source != WorldSystem::Weather) continue;
        if (e.type == WorldEventType::WindGust) {
            SubmitAcoustic(e.aux != 0 ? ais::StimulusCategory::MetalImpact : ais::StimulusCategory::Environmental, e.position, e.magnitude,
                           kWeatherSourceBase + e.subject, 2);
        } else if (e.type == WorldEventType::Thunder) {
            SubmitAcoustic(ais::StimulusCategory::Environmental, e.position, e.magnitude, kWeatherSourceBase + 0xFFFFu, 30);
        }
    }
    // Storm exposure wears down exposed infrastructure that is already damaged.
    if (facility_ != nullptr && tick_ >= nextExposureTick_) {
        nextExposureTick_ = tick_ + cfg_.weather.updateIntervalTicks;
        const double seconds = static_cast<double>(cfg_.weather.updateIntervalTicks) / cfg_.hazards.ticksPerSecond;
        const double amount = w.electricalExposure * seconds;
        if (amount > 0.0) {
            for (int n : exposedNodes_) {
                const auto i = static_cast<llenv::NodeIndex>(n);
                const llenv::NodeState& s = facility_->State(i);
                if (!s.destroyed && s.health < facility_->Def(i).damagedThreshold) facility_->ServerApplyDamage(i, amount);
            }
        }
    }
}

void WorldIntelligence::FeedSquadsFromAcoustics() {
    const WeatherSnapshot& w = weather_->Snapshot();
    (void)w;
    for (RobotId id : robots_) {
        if (squads_->SquadOf(id) == kInvalidIndex) continue;
        const ais::RobotAcousticBrain* b = ais_->Brain(id);
        const ais::RobotHostState* h = ais_->HostState(id);
        if (b == nullptr || h == nullptr) continue;
        for (const ais::AcousticTrace& t : b->Traces()) {
            auto it = std::find_if(reported_.begin(), reported_.end(), [&](const Reported& r) { return r.robot == id && r.trace == t.traceId; });
            const bool fresh = t.directHits > 0 && (it == reported_.end() || t.lastDirectTick > it->lastDirect);
            const bool searchedEmpty = it != reported_.end() && t.fruitlessSearches > it->fruitless;
            if (!fresh && !searchedEmpty) continue;
            ContactReport r;
            r.reporter = id;
            r.estimate = FromAis(t.estimate);
            r.uncertaintyCm = t.uncertaintyCm;
            r.threat = ThreatOf(t.category);
            r.tick = tick_;
            if (searchedEmpty) {
                r.kind = ReportKind::NegativeSearch;
                r.confidence = 0.0;
            } else {
                // Distant sounds heard through rain and wind are less trustworthy.
                const double d = Distance(FromAis(h->position), r.estimate);
                r.confidence = Clamp01(t.confidence * weather_->AcousticReportConfidenceScale(d));
                if (r.confidence < cfg_.reportMinConfidence) continue;
            }
            r.reportId = HashCombine(HashCombine(t.traceId, t.lastDirectTick), t.fruitlessSearches);
            if (squads_->Report(r) == ReportResult::Accepted) ++acousticReports_;
            if (it == reported_.end()) reported_.push_back(Reported{id, t.traceId, t.lastDirectTick, t.fruitlessSearches});
            else {
                it->lastDirect = t.lastDirectTick;
                it->fruitless = t.fruitlessSearches;
            }
        }
    }
    // Forget bookkeeping for traces the robots no longer remember (bounded memory).
    reported_.erase(std::remove_if(reported_.begin(), reported_.end(),
                                   [this](const Reported& r) {
                                       const ais::RobotAcousticBrain* b = ais_->Brain(r.robot);
                                       return b == nullptr || b->FindTrace(r.trace) == nullptr;
                                   }),
                    reported_.end());
}

void WorldIntelligence::PushSquadPriorities() {
    for (RobotId id : robots_) {
        if (squads_->SquadOf(id) == kInvalidIndex) continue;
        ais::RobotHostState h = *ais_->HostState(id);
        h.objectivePriority = squads_->ObjectivePriorityFor(id);
        ais_->SetHostState(id, h);
    }
}

void WorldIntelligence::Step() {
    if (!IsValid()) return;
    ++tick_;
    bus_.BeginTick(tick_);
    weather_->Step(tick_);
    ForwardWeather();
    if (facility_ != nullptr) facility_->Step();
    hazards_->Step(tick_, weather_->Snapshot());
    hazards_->SamplePlayers(players_, tick_);
    ais_->Step();
    if (tick_ % cfg_.squads.coordinationIntervalTicks == 0) FeedSquadsFromAcoustics();
    squads_->Step(tick_, weather_->Snapshot().commsRangeMul);
    PushSquadPriorities();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

RobotOrder WorldIntelligence::OrderFor(RobotId robot) const {
    RobotOrder o;
    const ais::AcousticDirective* d = ais_->Directive(robot);
    if (d == nullptr) return o;
    o.awareness = d->state;
    const TacticalIntent* t = squads_->Intent(robot);
    const bool aisMoving = d->hasMoveTarget && (d->state == ais::AwarenessState::Investigating || d->state == ais::AwarenessState::Searching ||
                                                d->state == ais::AwarenessState::Combat || d->state == ais::AwarenessState::ReturningToPatrol);
    auto acoustic = [&]() {
        o.source = OrderSource::Acoustic;
        o.hasMoveTarget = d->hasMoveTarget;
        o.moveTarget = FromAis(d->moveTarget);
        o.radiusCm = d->targetUncertaintyCm;
    };
    if (t == nullptr) {
        if (aisMoving) acoustic();
        return o;
    }
    o.intent = t->kind;
    // Combat and the robot's own investigations (when allowed) take precedence.
    const bool ownCall = d->state == ais::AwarenessState::Combat ||
                         ((t->kind == IntentKind::LocalAutonomous || t->kind == IntentKind::Patrol) && aisMoving);
    if (ownCall) {
        acoustic();
        return o;
    }
    if (t->kind == IntentKind::LocalAutonomous) return o;  // nothing heard: hold
    o.source = OrderSource::Squad;
    o.hasMoveTarget = true;
    o.moveTarget = t->target;
    o.radiusCm = t->radiusCm;
    return o;
}

VisibilitySample WorldIntelligence::VisibilityAt(const Vec3& p) const {
    VisibilitySample v;
    const HazardExposure x = hazards_->ExposureAt(p);
    v.fog = weather_->Snapshot().fog;
    v.smoke = x.visibilityReduction;
    v.illumination = facility_ != nullptr ? facility_->IlluminationAt(ToEnv(p)) : 0.0;
    v.visualRangeFactor = std::max(cfg_.weather.minVisualRangeFactor, weather_->VisualRangeFactor() * (1.0 - 0.85 * v.smoke));
    // Thermal sees through smoke far better, but hot fires clutter it.
    v.thermalRangeFactor =
        std::max(cfg_.weather.minThermalRangeFactor, weather_->ThermalRangeFactor() * (1.0 - 0.25 * v.smoke) * (1.0 - 0.6 * x.thermalClutter));
    return v;
}

double WorldIntelligence::VisualDetectionRangeCm(double base, const Vec3& observer, const Vec3& target) const {
    const VisibilitySample a = VisibilityAt(observer);
    const VisibilitySample b = VisibilityAt(target);
    const double light = 0.6 + 0.4 * Clamp01(b.illumination);  // lit targets are easier to spot
    return std::max(cfg_.minDetectionRangeCm, base * std::min(a.visualRangeFactor, b.visualRangeFactor) * light);
}

double WorldIntelligence::ThermalDetectionRangeCm(double base, const Vec3& observer, const Vec3& target) const {
    const VisibilitySample a = VisibilityAt(observer);
    const VisibilitySample b = VisibilityAt(target);
    return std::max(cfg_.minDetectionRangeCm, base * std::min(a.thermalRangeFactor, b.thermalRangeFactor));
}

AcousticConditions WorldIntelligence::Acoustics() const {
    const WeatherSnapshot& w = weather_->Snapshot();
    return AcousticConditions{w.rain, w.wind, w.wetness};
}

SurfaceConditions WorldIntelligence::SurfaceAt(const Vec3&) const { return weather_->Surface(); }

double WorldIntelligence::HazardDangerAt(const Vec3& p) const { return hazards_->ExposureAt(p).damagePerSecond; }

bool WorldIntelligence::IsRouteBlocked(std::uint32_t routeId) const { return hazards_->IsRouteBlocked(routeId); }

double WorldIntelligence::RouteRisk(std::uint32_t routeId) const {
    for (const RouteDef& r : cfg_.routes) {
        if (r.id != routeId) continue;
        if (IsRouteBlocked(routeId)) return std::numeric_limits<double>::infinity();
        double risk = 0.0;
        for (const Vec3& p : r.waypoints) {
            risk += HazardDangerAt(p);
            risk += 20.0 * (1.0 - VisibilityAt(p).visualRangeFactor);  // blind spots are risky too
        }
        return risk;
    }
    return std::numeric_limits<double>::infinity();
}

std::uint64_t WorldIntelligence::StateHash() const {
    StateHasher h;
    h.Add(tick_);
    h.Add(weather_->StateHash());
    h.Add(hazards_->StateHash());
    h.Add(squads_->StateHash());
    h.Add(ais_->StateHash());
    if (facility_ != nullptr) h.Add(facility_->StateHash());
    return h.Value();
}

}  // namespace llworld
