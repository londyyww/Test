// LAST LIGHT - Environmental & World Intelligence (WI)
#include "llworld/Hazards.h"

#include <algorithm>
#include <cmath>

namespace llworld {

namespace {
constexpr const char* kTypeNames[kHazardTypeCount] = {"GasLeak", "Fire", "Smoke", "Electrical", "Structural", "Chemical", "Machinery"};

std::uint32_t Bit(HazardType t) { return 1u << static_cast<std::uint32_t>(t); }
}  // namespace

const char* ToString(HazardType t) {
    const auto i = static_cast<std::size_t>(t);
    return i < kHazardTypeCount ? kTypeNames[i] : "Invalid";
}
bool ParseHazardType(std::string_view s, HazardType& out) {
    for (std::size_t i = 0; i < kHazardTypeCount; ++i) {
        if (s == kTypeNames[i]) {
            out = static_cast<HazardType>(i);
            return true;
        }
    }
    return false;
}
const char* ToString(HazardState s) {
    switch (s) {
        case HazardState::Dormant: return "Dormant";
        case HazardState::Active: return "Active";
        case HazardState::Suppressed: return "Suppressed";
        case HazardState::Spent: return "Spent";
    }
    return "Unknown";
}
const char* ToString(HazardRequestResult r) {
    switch (r) {
        case HazardRequestResult::Applied: return "Applied";
        case HazardRequestResult::RejectedDuplicate: return "RejectedDuplicate";
        case HazardRequestResult::RejectedRateLimited: return "RejectedRateLimited";
        case HazardRequestResult::RejectedUnknown: return "RejectedUnknown";
        case HazardRequestResult::RejectedOutOfRange: return "RejectedOutOfRange";
        case HazardRequestResult::RejectedInvalidState: return "RejectedInvalidState";
        case HazardRequestResult::RejectedMissingItem: return "RejectedMissingItem";
        case HazardRequestResult::RejectedUnsupported: return "RejectedUnsupported";
        case HazardRequestResult::RejectedTooManyPlayers: return "RejectedTooManyPlayers";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

std::vector<std::string> ValidateHazards(const HazardConfig& c) {
    std::vector<std::string> e;
    auto require = [&e](bool ok, const std::string& m) {
        if (!ok) e.push_back(m);
    };
    require(c.ticksPerSecond > 0.0 && c.updateIntervalTicks >= 1 && c.exposureIntervalTicks >= 1, "hazards: tick settings invalid");
    require(c.smokeDissipateSeconds > 0.0 && c.wetElectricalRadiusMul >= 0.0, "hazards: smoke/electrical settings invalid");
    require(c.maxPlayers >= 1 && c.maxPlayers <= 4, "hazards: max_players must be 1..4");
    require(c.requestBurst >= 1.0 && c.requestRefillPerTick >= 0.0 && c.causeHistory >= 1, "hazards: request limits invalid");
    auto find = [&c](const std::string& id) -> const HazardDef* {
        for (const auto& h : c.hazards) {
            if (h.id == id) return &h;
        }
        return nullptr;
    };
    for (std::size_t i = 0; i < c.hazards.size(); ++i) {
        const HazardDef& h = c.hazards[i];
        const std::string n = "hazard." + h.id + ": ";
        require(!h.id.empty(), "hazard with empty id");
        for (std::size_t j = 0; j < i; ++j) require(c.hazards[j].id != h.id, n + "duplicate id");
        require(h.position.IsFinite(), n + "position must be finite");
        require(h.radiusCm > 0.0, n + "radius_cm must be > 0");
        require(h.maxRadiusCm == 0.0 || h.maxRadiusCm >= h.radiusCm, n + "max_radius_cm must be >= radius_cm");
        require(h.severity >= 0.0 && h.severity <= 1.0, n + "severity must be in [0,1]");
        require(h.spreadChance >= 0.0 && h.spreadChance <= 1.0, n + "spread_chance must be in [0,1]");
        require(h.spreadRadiusCm >= 0.0 && (h.spreadRadiusCm == 0.0 || h.spreadIntervalTicks >= 1), n + "spread settings invalid");
        require(h.growthPerSecond >= 0.0, n + "growth_per_second must be >= 0");
        require(h.ignitionThreshold > 0.0 && h.ignitionThreshold <= 1.0, n + "ignition_threshold must be in (0,1]");
        require(h.damagePerSecond >= 0.0 && h.burstDamage >= 0.0 && h.nodeDamagePerSecond >= 0.0 && h.burstNodeDamage >= 0.0,
                n + "damage values must be >= 0");
        require(h.visibilityReduction >= 0.0 && h.visibilityReduction <= 1.0, n + "visibility_reduction must be in [0,1]");
        require(h.thermalClutter >= 0.0 && h.thermalClutter <= 1.0 && h.movementPenalty >= 0.0 && h.movementPenalty <= 1.0,
                n + "thermal_clutter/movement_penalty must be in [0,1]");
        require(h.interactRangeCm > 0.0, n + "interact_range_cm must be > 0");
        require(h.noiseIntervalTicks == 0 || h.noiseDb > 0.0, n + "noise_interval_ticks needs noise_db");
        if (h.type == HazardType::Electrical || h.type == HazardType::Machinery) {
            require(!h.powerNode.empty(), n + "Electrical/Machinery hazards need power_node");
        }
        if (h.type == HazardType::Structural) {
            require(h.stressWarn > 0.0 && h.stressWarn < h.stressCollapse, n + "need 0 < stress_warn < stress_collapse");
        }
        require(!h.flammable || h.type == HazardType::Fire, n + "only Fire sites can be flammable");
        if (!h.ignites.empty()) {
            const HazardDef* t = find(h.ignites);
            require(h.type == HazardType::GasLeak, n + "only gas leaks can ignite another hazard");
            require(t != nullptr && t->type == HazardType::Fire, n + "ignites must reference a Fire hazard");
        }
        if (!h.smoke.empty()) {
            const HazardDef* t = find(h.smoke);
            require(h.type == HazardType::Fire, n + "only fires can drive smoke");
            require(t != nullptr && t->type == HazardType::Smoke, n + "smoke must reference a Smoke hazard");
        }
    }
    return e;
}

void LoadHazards(const IniDocument& doc, HazardConfig& c, std::vector<std::string>& errors) {
    for (const IniSection& s : doc.sections) {
        if (s.name == "hazards") {
            IniBinder b;
            b.Num("ticks_per_second", c.ticksPerSecond);
            b.Ticks("update_interval_ticks", c.updateIntervalTicks);
            b.Ticks("exposure_interval_ticks", c.exposureIntervalTicks);
            b.Num("wet_electrical_radius_mul", c.wetElectricalRadiusMul);
            b.Num("smoke_dissipate_seconds", c.smokeDissipateSeconds);
            b.Apply(s, errors);
            continue;
        }
        if (s.name.rfind("hazard.", 0) != 0) continue;
        HazardDef h;
        h.id = s.name.substr(7);
        bool sawType = false;
        IniBinder b;
        b.Custom("type", [&h, &sawType](std::string_view v) {
            sawType = true;
            return ParseHazardType(v, h.type);
        });
        b.Point("position", h.position);
        b.Num("radius_cm", h.radiusCm);
        b.Num("max_radius_cm", h.maxRadiusCm);
        b.Num("severity", h.severity);
        b.Flag("starts_active", h.startsActive);
        b.Ticks("duration_ticks", h.durationTicks);
        b.Flag("flammable", h.flammable);
        b.Num("spread_radius_cm", h.spreadRadiusCm);
        b.Ticks("spread_interval_ticks", h.spreadIntervalTicks);
        b.Num("spread_chance", h.spreadChance);
        b.Num("growth_per_second", h.growthPerSecond);
        b.Str("power_node", h.powerNode);
        b.Flag("exposed", h.exposed);
        b.Num("ignition_threshold", h.ignitionThreshold);
        b.Str("ignites", h.ignites);
        b.Str("smoke", h.smoke);
        b.Str("alarm", h.alarm);
        b.Str("extinguish_item", h.extinguishItem);
        b.Flag("player_triggerable", h.playerTriggerable);
        b.Num("interact_range_cm", h.interactRangeCm);
        b.U32("route_id", h.routeId);
        b.List("damages_nodes", h.damagesNodes);
        b.Num("node_damage_per_second", h.nodeDamagePerSecond);
        b.Num("burst_node_damage", h.burstNodeDamage);
        b.Num("damage_per_second", h.damagePerSecond);
        b.Num("burst_damage", h.burstDamage);
        b.Num("visibility_reduction", h.visibilityReduction);
        b.Num("thermal_clutter", h.thermalClutter);
        b.Num("movement_penalty", h.movementPenalty);
        b.Num("activation_noise_db", h.activationNoiseDb);
        b.Num("noise_db", h.noiseDb);
        b.Ticks("noise_interval_ticks", h.noiseIntervalTicks);
        b.Num("stress_warn", h.stressWarn);
        b.Num("stress_collapse", h.stressCollapse);
        b.Num("stress_per_loud_event", h.stressPerLoudEvent);
        b.Num("stress_per_fire_second", h.stressPerFireSecond);
        b.Num("stress_per_wind_second", h.stressPerWindSecond);
        b.Custom("persistence", [&h](std::string_view v) {
            if (v == "Mission") h.persistence = HazardPersistence::Mission;
            else if (v == "Facility") h.persistence = HazardPersistence::Facility;
            else return false;
            return true;
        });
        b.Apply(s, errors);
        if (!sawType) errors.push_back("line " + std::to_string(s.line) + ": [" + s.name + "] needs a type");
        c.hazards.push_back(h);
    }
}

// ---------------------------------------------------------------------------
// Construction: resolve references and precompute bounded neighbour lists
// ---------------------------------------------------------------------------

HazardSystem::HazardSystem(const HazardConfig& config, WorldEventBus* bus, IHazardWorld* world)
    : cfg_(config), bus_(bus), world_(world) {
    errors_ = ValidateHazards(cfg_);
    const std::size_t n = cfg_.hazards.size();
    nodeOf_.assign(n, -1);
    damageNodes_.resize(n);
    alarmOf_.assign(n, -1);
    ignitesOf_.assign(n, kInvalidIndex);
    smokeOf_.assign(n, kInvalidIndex);
    parentFire_.assign(n, kInvalidIndex);
    spreadTo_.resize(n);
    ignitionSources_.resize(n);
    nearbyFires_.resize(n);
    if (!errors_.empty()) return;

    for (std::uint32_t i = 0; i < n; ++i) {
        const HazardDef& h = cfg_.hazards[i];
        auto resolve = [this, &h](const std::string& id, const char* what) {
            if (id.empty()) return -1;
            const int k = world_ != nullptr ? world_->ResolveNode(id) : -1;
            if (k < 0) errors_.push_back("hazard." + h.id + ": " + what + " '" + id + "' not found in infrastructure");
            return k;
        };
        nodeOf_[i] = resolve(h.powerNode, "power_node");
        alarmOf_[i] = resolve(h.alarm, "alarm");
        for (const auto& d : h.damagesNodes) damageNodes_[i].push_back(resolve(d, "damages_nodes"));
        if (!h.ignites.empty()) ignitesOf_[i] = Find(h.ignites);
        if (!h.smoke.empty()) {
            smokeOf_[i] = Find(h.smoke);
            parentFire_[smokeOf_[i]] = i;
        }
    }
    // O(n^2) once at load; hazards per facility are expected in the hundreds.
    for (std::uint32_t i = 0; i < n; ++i) {
        const HazardDef& a = cfg_.hazards[i];
        for (std::uint32_t j = 0; j < n; ++j) {
            if (i == j) continue;
            const HazardDef& b = cfg_.hazards[j];
            const double d = Distance(a.position, b.position);
            if (a.type == HazardType::Fire && b.type == HazardType::Fire && b.flammable && a.spreadRadiusCm > 0.0 && d <= a.spreadRadiusCm) {
                spreadTo_[i].push_back(j);
            }
            const double reach = std::max(a.radiusCm, a.maxRadiusCm) + b.radiusCm;
            const bool igniter = b.type == HazardType::Electrical || b.type == HazardType::Fire || b.type == HazardType::Machinery;
            if (a.type == HazardType::GasLeak && igniter && d <= reach) ignitionSources_[i].push_back(j);
            if (a.type == HazardType::Structural && b.type == HazardType::Fire && d <= a.radiusCm + b.radiusCm) nearbyFires_[i].push_back(j);
        }
    }
    rt_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        rt_[i].radius = cfg_.hazards[i].radiusCm;
        if (cfg_.hazards[i].type == HazardType::Structural) structures_.push_back(i);
    }
    causes_.assign(cfg_.causeHistory, {kInvalidIndex, 0});
}

std::uint32_t HazardSystem::Find(std::string_view id) const {
    for (std::uint32_t i = 0; i < cfg_.hazards.size(); ++i) {
        if (cfg_.hazards[i].id == id) return i;
    }
    return kInvalidIndex;
}

// ---------------------------------------------------------------------------
// State changes
// ---------------------------------------------------------------------------

void HazardSystem::Changed(std::uint32_t i) {
    ++rt_[i].version;
    rt_[i].dirty = true;
}

void HazardSystem::Publish(WorldEventType type, std::uint32_t i, double magnitude, double radius, std::uint32_t aux) {
    if (bus_ == nullptr) return;
    WorldEvent e;
    e.type = type;
    e.source = WorldSystem::Hazards;
    e.position = cfg_.hazards[i].position;
    e.magnitude = magnitude;
    e.radiusCm = radius;
    e.subject = i;
    e.aux = aux;
    bus_->Publish(e);
}

void HazardSystem::SetState(std::uint32_t i, HazardState s) {
    HazardRuntime& r = rt_[i];
    if (r.state == s) return;
    const bool wasActive = r.state == HazardState::Active;
    r.state = s;
    Changed(i);
    auto it = std::lower_bound(active_.begin(), active_.end(), i);
    if (s == HazardState::Active && (it == active_.end() || *it != i)) active_.insert(it, i);
    if (s != HazardState::Active && it != active_.end() && *it == i) active_.erase(it);
    const HazardDef& d = cfg_.hazards[i];
    if (wasActive && s != HazardState::Active) {
        Publish(WorldEventType::HazardDeactivated, i, 0.0, 0.0, static_cast<std::uint32_t>(s));
        if (r.routeBlocked && !r.collapsed) {
            r.routeBlocked = false;
            Publish(WorldEventType::RouteCleared, i, 0.0, 0.0, d.routeId);
        }
    }
}

bool HazardSystem::Activate(std::uint32_t i, std::uint64_t causeId) {
    if (!IsValid() || i >= rt_.size()) return false;
    if (causeId != 0) {
        for (const auto& c : causes_) {
            if (c.first == i && c.second == causeId) return false;  // same cause reported twice
        }
        causes_[causeHead_] = {i, causeId};
        causeHead_ = (causeHead_ + 1) % causes_.size();
    }
    return ActivateInternal(i, now_);
}

bool HazardSystem::ActivateInternal(std::uint32_t i, Tick now) {
    HazardRuntime& r = rt_[i];
    const HazardDef& d = cfg_.hazards[i];
    if (r.state == HazardState::Active || r.collapsed || r.burnedOut) return false;
    if (d.type == HazardType::Structural) return false;  // structures fail through stress only
    if (nodeOf_[i] >= 0 && world_ != nullptr) {
        const bool live = d.type == HazardType::Machinery ? world_->NodeRunning(nodeOf_[i]) : world_->NodeEnergized(nodeOf_[i]);
        if (!live) {
            SetState(i, HazardState::Suppressed);
            return false;
        }
    }
    SetState(i, HazardState::Active);
    r.activatedTick = now;
    r.escalated = false;
    r.radius = d.radiusCm;
    r.intensity = d.type == HazardType::GasLeak ? 0.0 : d.severity;
    r.fuel = d.durationTicks > 0 ? static_cast<double>(d.durationTicks) : -1.0;
    r.nextSpread = now + d.spreadIntervalTicks;
    r.nextNoise = d.noiseIntervalTicks > 0 ? now + d.noiseIntervalTicks : 0;
    if (d.activationNoiseDb > 0.0 && world_ != nullptr) {
        const HazardSound s = d.type == HazardType::Fire ? HazardSound::FireIgnite
                              : d.type == HazardType::Electrical ? HazardSound::ElectricalArc
                              : d.type == HazardType::Machinery ? HazardSound::MachineryGrind
                                                                : HazardSound::GasHiss;
        world_->EmitSound(s, d.position, d.activationNoiseDb, 3, i);
    }
    if (alarmOf_[i] >= 0 && world_ != nullptr) world_->TriggerAlarm(alarmOf_[i]);
    if (d.routeId != 0 && d.type == HazardType::Fire) {
        r.routeBlocked = true;
        Publish(WorldEventType::RouteBlocked, i, 0.0, 0.0, d.routeId);
    }
    Publish(WorldEventType::HazardActivated, i, d.severity, d.radiusCm, static_cast<std::uint32_t>(d.type));
    if (smokeOf_[i] != kInvalidIndex) ActivateInternal(smokeOf_[i], now);
    return true;
}

void HazardSystem::Ignite(std::uint32_t g, Tick now) {
    const HazardDef& d = cfg_.hazards[g];
    const double loud = d.activationNoiseDb > 0.0 ? d.activationNoiseDb + 20.0 : 110.0;
    SetState(g, HazardState::Spent);
    rt_[g].intensity = 0.0;
    if (world_ != nullptr) {
        world_->EmitSound(HazardSound::GasIgnition, d.position, loud, 6, g);
        for (int node : damageNodes_[g]) {
            if (node >= 0 && d.burstNodeDamage > 0.0) world_->DamageNode(node, d.burstNodeDamage);
        }
    }
    bursts_.push_back(Burst{d.position, std::max(rt_[g].radius, d.radiusCm) * 1.5, d.burstDamage});
    Publish(WorldEventType::GasIgnition, g, loud, rt_[g].radius * 1.5);
    if (ignitesOf_[g] != kInvalidIndex) ActivateInternal(ignitesOf_[g], now);
}

void HazardSystem::Collapse(std::uint32_t i, Tick now) {
    HazardRuntime& r = rt_[i];
    const HazardDef& d = cfg_.hazards[i];
    (void)now;
    r.collapsed = true;
    const double loud = d.activationNoiseDb > 0.0 ? d.activationNoiseDb : 105.0;
    if (world_ != nullptr) {
        world_->EmitSound(HazardSound::StructureCollapse, d.position, loud, 10, i);
        for (int node : damageNodes_[i]) {
            if (node >= 0 && d.burstNodeDamage > 0.0) world_->DamageNode(node, d.burstNodeDamage);
        }
        if (alarmOf_[i] >= 0) world_->TriggerAlarm(alarmOf_[i]);
    }
    bursts_.push_back(Burst{d.position, d.radiusCm, d.burstDamage});
    SetState(i, HazardState::Spent);
    Publish(WorldEventType::StructuralCollapse, i, loud, d.radiusCm);
    if (d.routeId != 0) {
        r.routeBlocked = true;  // debris stays
        Publish(WorldEventType::RouteBlocked, i, 0.0, 0.0, d.routeId);
    }
}

bool HazardSystem::IsIgnitionSource(std::uint32_t j) const {
    return rt_[j].state == HazardState::Active && cfg_.hazards[j].type != HazardType::Smoke;
}

void HazardSystem::ApplyLoudEvent(const WorldEvent& e) {
    double weight = 0.0;
    switch (e.type) {
        case WorldEventType::Explosion:
        case WorldEventType::GasIgnition:
        case WorldEventType::StructuralCollapse: weight = 1.0; break;
        case WorldEventType::Gunfire: weight = 0.15; break;
        case WorldEventType::Thunder: weight = 0.05; break;
        default: return;
    }
    for (std::uint32_t s : structures_) {
        HazardRuntime& r = rt_[s];
        if (r.collapsed || (e.source == WorldSystem::Hazards && e.subject == s)) continue;
        const HazardDef& d = cfg_.hazards[s];
        if (Distance(e.position, d.position) > d.radiusCm * 2.0 + e.radiusCm) continue;
        r.stress += d.stressPerLoudEvent * weight;
        Changed(s);
    }
    // Gunfire and explosions are ignition sources for gas that is concentrated enough.
    if (e.type == WorldEventType::Gunfire || e.type == WorldEventType::Explosion) {
        for (std::uint32_t g : std::vector<std::uint32_t>(active_)) {
            const HazardDef& d = cfg_.hazards[g];
            if (d.type != HazardType::GasLeak || rt_[g].state != HazardState::Active) continue;
            if (rt_[g].intensity >= d.ignitionThreshold && Distance(e.position, d.position) <= rt_[g].radius + e.radiusCm) Ignite(g, now_);
        }
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void HazardSystem::Step(Tick now, const WeatherSnapshot& w) {
    if (!IsValid()) return;
    now_ = now;
    weather_ = w;
    if (!started_) {
        started_ = true;
        nextUpdate_ = now;
        nextExposure_ = now;
        for (std::uint32_t i = 0; i < rt_.size(); ++i) {
            if (cfg_.hazards[i].startsActive) ActivateInternal(i, now);
        }
    }
    if (bus_ != nullptr) {
        for (const WorldEvent& e : bus_->Previous()) {
            switch (e.type) {
                case WorldEventType::PowerLost:
                case WorldEventType::PowerRestored:
                case WorldEventType::AlarmRaised: dependencyDirty_ = true; break;
                default: ApplyLoudEvent(e); break;
            }
        }
    }
    if (w.version != lastWeatherVersion_) {
        lastWeatherVersion_ = w.version;
        dependencyDirty_ = true;
    }
    if (dependencyDirty_ && world_ != nullptr) {
        dependencyDirty_ = false;
        for (std::uint32_t i = 0; i < rt_.size(); ++i) {
            if (nodeOf_[i] < 0) continue;
            const HazardDef& d = cfg_.hazards[i];
            const bool live = d.type == HazardType::Machinery ? world_->NodeRunning(nodeOf_[i]) : world_->NodeEnergized(nodeOf_[i]);
            const HazardState s = rt_[i].state;
            if (live && (s == HazardState::Suppressed || (s == HazardState::Dormant && (d.startsActive || d.type == HazardType::Machinery)))) {
                ActivateInternal(i, now);
            } else if (!live && s == HazardState::Active) {
                SetState(i, HazardState::Suppressed);  // power off: cable goes dead, crusher stops
            }
        }
    }
    if (now < nextUpdate_) return;
    const double dt = static_cast<double>(cfg_.updateIntervalTicks);
    nextUpdate_ = now + cfg_.updateIntervalTicks;
    ++updates_;
    const std::vector<std::uint32_t> snapshot = active_;  // may change while updating
    for (std::uint32_t i : snapshot) {
        if (rt_[i].state == HazardState::Active) UpdateHazard(i, now, dt, w);
    }
    // Structures accumulate stress from nearby fires and wind even while stable.
    for (std::uint32_t s : structures_) {
        HazardRuntime& r = rt_[s];
        if (r.collapsed) continue;
        const HazardDef& d = cfg_.hazards[s];
        const double secs = dt / cfg_.ticksPerSecond;
        double add = 0.0;
        for (std::uint32_t f : nearbyFires_[s]) {
            if (rt_[f].state == HazardState::Active) add += d.stressPerFireSecond * rt_[f].intensity * secs;
        }
        if (d.exposed) add += d.stressPerWindSecond * w.wind * secs;
        if (add > 0.0) {
            r.stress += add;
            Changed(s);
        }
        if (!r.escalated && r.stress >= d.stressWarn) {
            r.escalated = true;
            SetState(s, HazardState::Active);  // creaking, dangerous to stand under
            Publish(WorldEventType::HazardEscalated, s, r.stress);
            if (world_ != nullptr) world_->EmitSound(HazardSound::StructureCreak, d.position, d.noiseDb > 0.0 ? d.noiseDb : 70.0, 15, s);
        }
        if (r.stress >= d.stressCollapse) Collapse(s, now);
    }
}

void HazardSystem::UpdateHazard(std::uint32_t i, Tick now, double dt, const WeatherSnapshot& w) {
    HazardRuntime& r = rt_[i];
    const HazardDef& d = cfg_.hazards[i];
    const double secs = dt / cfg_.ticksPerSecond;
    const double rain = d.exposed ? w.rain : 0.0;
    const double wind = d.exposed ? w.wind : 0.0;
    const auto before = static_cast<int>(r.intensity * 255.0);
    const double radiusBefore = r.radius;

    if (d.durationTicks > 0 && d.type != HazardType::Fire && now >= r.activatedTick + d.durationTicks) {
        SetState(i, HazardState::Spent);
        return;
    }
    switch (d.type) {
        case HazardType::GasLeak: {
            // Wind disperses the cloud: lower peak concentration outdoors.
            const double cap = 1.0 - 0.6 * wind;
            r.intensity = std::min(cap, r.intensity + d.growthPerSecond * secs);
            if (!r.escalated && r.intensity >= d.ignitionThreshold) {
                r.escalated = true;
                Publish(WorldEventType::HazardEscalated, i, r.intensity);
            }
            if (r.intensity >= d.ignitionThreshold) {
                for (std::uint32_t s : ignitionSources_[i]) {
                    if (IsIgnitionSource(s)) {
                        Ignite(i, now);
                        return;
                    }
                }
            }
            break;
        }
        case HazardType::Fire: {
            if (r.fuel >= 0.0) {
                r.fuel -= dt * (1.0 + rain * w.fireRainSuppression);
                if (r.fuel <= 0.0) {
                    r.burnedOut = true;
                    r.intensity = 0.0;
                    SetState(i, HazardState::Spent);
                    return;
                }
            }
            if (world_ != nullptr && d.nodeDamagePerSecond > 0.0) {
                for (int node : damageNodes_[i]) {
                    if (node >= 0) world_->DamageNode(node, d.nodeDamagePerSecond * r.intensity * secs);
                }
            }
            if (!spreadTo_[i].empty() && now >= r.nextSpread) {
                r.nextSpread = now + d.spreadIntervalTicks;
                const double chance = d.spreadChance * (d.exposed ? w.fireSpreadMul : 1.0);
                for (std::uint32_t j : spreadTo_[i]) {
                    if (rt_[j].state == HazardState::Active || rt_[j].burnedOut) continue;
                    const double roll = HashToUnit(HashCombine(HashCombine(cfg_.seed, (static_cast<std::uint64_t>(i) << 32) | j), now));
                    if (roll < chance && ActivateInternal(j, now)) Publish(WorldEventType::FireSpread, j, 0.0, 0.0, i);
                }
            }
            break;
        }
        case HazardType::Smoke: {
            const std::uint32_t f = parentFire_[i];
            const bool fed = f != kInvalidIndex && rt_[f].state == HazardState::Active;
            r.radius = d.radiusCm * (1.0 + 0.5 * wind);
            if (fed) {
                r.intensity = d.severity * rt_[f].intensity * (1.0 - 0.5 * wind);
            } else {
                r.intensity -= secs * (1.0 + 2.0 * wind) / cfg_.smokeDissipateSeconds;
                if (r.intensity <= 0.0) {
                    r.intensity = 0.0;
                    SetState(i, HazardState::Dormant);
                    return;
                }
            }
            break;
        }
        case HazardType::Electrical:
            r.radius = d.radiusCm * (1.0 + cfg_.wetElectricalRadiusMul * w.wetness * (d.exposed ? 1.0 : 0.0));
            break;
        case HazardType::Chemical: {
            const double cap = d.maxRadiusCm > 0.0 ? d.maxRadiusCm : d.radiusCm;
            r.radius = std::min(cap, r.radius + d.growthPerSecond * secs * (0.2 + (d.exposed ? w.wetness : 0.0)));
            break;
        }
        case HazardType::Structural:
        case HazardType::Machinery:
            break;
    }
    if (r.nextNoise != 0 && now >= r.nextNoise && world_ != nullptr) {
        r.nextNoise = now + d.noiseIntervalTicks;
        const HazardSound s = d.type == HazardType::Fire ? HazardSound::FireCrackle
                              : d.type == HazardType::Electrical ? HazardSound::ElectricalArc
                              : d.type == HazardType::Machinery ? HazardSound::MachineryGrind
                              : d.type == HazardType::Structural ? HazardSound::StructureCreak
                                                                 : HazardSound::GasHiss;
        world_->EmitSound(s, d.position, d.noiseDb, d.noiseIntervalTicks, i);
    }
    if (static_cast<int>(r.intensity * 255.0) != before || std::fabs(r.radius - radiusBefore) >= 10.0) Changed(i);
}

// ---------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------

HazardSystem::PlayerSlot* HazardSystem::SlotFor(PlayerId p) {
    for (auto& s : players_) {
        if (s.id == p) return &s;
    }
    if (players_.size() >= cfg_.maxPlayers) {
        // Full: reuse a slot held by an id that is not in the current session sample,
        // so bogus or departed ids cannot lock real players out permanently.
        auto stale = std::find_if(players_.begin(), players_.end(),
                                  [this](const PlayerSlot& s) { return std::find(present_.begin(), present_.end(), s.id) == present_.end(); });
        if (stale == players_.end() || std::find(present_.begin(), present_.end(), p) == present_.end()) return nullptr;
        players_.erase(stale);
    }
    PlayerSlot s;
    s.id = p;
    s.tokens = cfg_.requestBurst;
    s.stamp = now_;
    players_.push_back(s);
    std::sort(players_.begin(), players_.end(), [](const PlayerSlot& a, const PlayerSlot& b) { return a.id < b.id; });
    for (auto& q : players_) {
        if (q.id == p) return &q;
    }
    return nullptr;
}

HazardRequestResult HazardSystem::Interact(const HazardRequest& q, Tick now) {
    using R = HazardRequestResult;
    if (!IsValid() || q.player == 0 || !q.playerPosition.IsFinite()) return R::RejectedUnknown;
    PlayerSlot* p = SlotFor(q.player);
    if (p == nullptr) return R::RejectedTooManyPlayers;
    const auto end = p->recent.begin() + static_cast<std::ptrdiff_t>(p->count);
    if (std::find(p->recent.begin(), end, q.sequence) != end) return R::RejectedDuplicate;
    if (now > p->stamp) {
        p->tokens = std::min(cfg_.requestBurst, p->tokens + cfg_.requestRefillPerTick * static_cast<double>(now - p->stamp));
        p->stamp = now;
    }
    if (p->tokens < 1.0) return R::RejectedRateLimited;
    p->tokens -= 1.0;
    p->recent[p->head] = q.sequence;
    p->head = (p->head + 1) % p->recent.size();
    p->count = std::min(p->count + 1, p->recent.size());

    if (q.hazard >= rt_.size()) return R::RejectedUnknown;
    const HazardDef& d = cfg_.hazards[q.hazard];
    HazardRuntime& r = rt_[q.hazard];
    if (Distance(q.playerPosition, d.position) > d.interactRangeCm + r.radius) return R::RejectedOutOfRange;
    now_ = now;
    switch (q.action) {
        case HazardAction::Extinguish:
            if (d.type != HazardType::Fire) return R::RejectedUnsupported;
            if (r.state != HazardState::Active) return R::RejectedInvalidState;
            if (!d.extinguishItem.empty() && (world_ == nullptr || !world_->ConsumeItem(q.player, d.extinguishItem))) return R::RejectedMissingItem;
            r.intensity = 0.0;
            SetState(q.hazard, HazardState::Spent);  // may re-ignite later; not burned out
            return R::Applied;
        case HazardAction::CloseValve:
            if (d.type != HazardType::GasLeak) return R::RejectedUnsupported;
            if (r.state != HazardState::Active) return R::RejectedInvalidState;
            SetState(q.hazard, HazardState::Spent);
            return R::Applied;
        case HazardAction::Trigger:
            if (!d.playerTriggerable) return R::RejectedUnsupported;
            return ActivateInternal(q.hazard, now) ? R::Applied : R::RejectedInvalidState;
    }
    return R::RejectedUnsupported;
}

HazardExposure HazardSystem::ExposureAt(const Vec3& pos) const {
    HazardExposure x;
    for (std::uint32_t i : active_) {
        const HazardRuntime& r = rt_[i];
        const HazardDef& d = cfg_.hazards[i];
        const double dist = Distance(pos, d.position);
        if (dist > r.radius) continue;
        const double falloff = 1.0 - 0.5 * dist / r.radius;
        double dmg = d.damagePerSecond * falloff;
        if (d.type == HazardType::GasLeak) dmg *= Clamp01(r.intensity / d.ignitionThreshold);
        if (d.type == HazardType::Fire || d.type == HazardType::Smoke) dmg *= r.intensity;
        x.damagePerSecond += dmg;
        x.visibilityReduction = std::max(x.visibilityReduction, d.visibilityReduction * r.intensity * falloff);
        x.thermalClutter = std::max(x.thermalClutter, d.thermalClutter * r.intensity);
        x.movementPenalty = std::max(x.movementPenalty, d.movementPenalty);
        x.typesMask |= Bit(d.type);
    }
    return x;
}

void HazardSystem::SamplePlayers(const std::vector<PlayerSample>& players, Tick now) {
    if (!IsValid()) return;
    present_.clear();
    for (const PlayerSample& p : players) present_.push_back(p.player);
    if (now < nextExposure_) return;
    nextExposure_ = now + cfg_.exposureIntervalTicks;
    const double secs = static_cast<double>(cfg_.exposureIntervalTicks) / cfg_.ticksPerSecond;
    for (const PlayerSample& p : players) {
        if (!p.position.IsFinite()) continue;
        const HazardExposure x = ExposureAt(p.position);
        double dose = x.damagePerSecond * secs;
        for (const Burst& b : bursts_) {
            if (Distance(b.position, p.position) <= b.radius) dose += b.damage;
        }
        if (dose <= 0.0) continue;
        PlayerSlot* slot = SlotFor(p.player);
        if (slot != nullptr) slot->dose += dose;
        if (bus_ != nullptr) {
            WorldEvent e;
            e.type = WorldEventType::PlayerExposure;
            e.source = WorldSystem::Hazards;
            e.position = p.position;
            e.magnitude = dose;
            e.subject = p.player;
            e.aux = x.typesMask;
            bus_->Publish(e);
        }
    }
    bursts_.clear();
}

double HazardSystem::DoseOf(PlayerId player) const {
    for (const auto& s : players_) {
        if (s.id == player) return s.dose;
    }
    return 0.0;
}

bool HazardSystem::IsRouteBlocked(std::uint32_t routeId) const {
    if (routeId == 0) return false;
    for (std::uint32_t i = 0; i < rt_.size(); ++i) {
        if (rt_[i].routeBlocked && cfg_.hazards[i].routeId == routeId) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Replication & persistence
// ---------------------------------------------------------------------------

void HazardSystem::CollectReplication(std::vector<HazardReplicationState>& out) {
    for (std::uint32_t i = 0; i < rt_.size(); ++i) {
        HazardRuntime& r = rt_[i];
        if (!r.dirty) continue;
        r.dirty = false;
        HazardReplicationState s;
        s.hazard = i;
        s.state = static_cast<std::uint8_t>(r.state);
        s.intensity = static_cast<std::uint8_t>(Clamp01(r.intensity) * 255.0);
        s.radiusDm = static_cast<std::uint16_t>(std::min(65535.0, r.radius / 10.0));
        s.flags = static_cast<std::uint8_t>((r.collapsed ? 1 : 0) | (r.burnedOut ? 2 : 0) | (r.routeBlocked ? 4 : 0) | (r.escalated ? 8 : 0));
        s.version = r.version;
        out.push_back(s);
    }
}

std::vector<HazardDamageRecord> HazardSystem::ExportFacilityDamage() const {
    std::vector<HazardDamageRecord> out;
    for (std::uint32_t i = 0; i < rt_.size(); ++i) {
        if (cfg_.hazards[i].persistence != HazardPersistence::Facility) continue;
        const HazardRuntime& r = rt_[i];
        if (!r.collapsed && !r.burnedOut && r.stress <= 0.0) continue;
        out.push_back(HazardDamageRecord{cfg_.hazards[i].id, r.collapsed, r.burnedOut, r.stress});
    }
    return out;
}

bool HazardSystem::ImportFacilityDamage(const std::vector<HazardDamageRecord>& records) {
    if (started_ || !IsValid()) return false;
    for (const auto& rec : records) {
        const std::uint32_t i = Find(rec.id);
        if (i == kInvalidIndex || cfg_.hazards[i].persistence != HazardPersistence::Facility || !std::isfinite(rec.stress)) continue;
        HazardRuntime& r = rt_[i];
        r.stress = std::max(0.0, rec.stress);
        r.burnedOut = rec.burnedOut;
        r.collapsed = rec.collapsed && cfg_.hazards[i].type == HazardType::Structural;
        if (r.collapsed) {
            r.state = HazardState::Spent;
            r.routeBlocked = cfg_.hazards[i].routeId != 0;
        }
        if (r.burnedOut) r.state = HazardState::Spent;
        Changed(i);
    }
    return true;
}

std::uint64_t HazardSystem::StateHash() const {
    StateHasher h;
    for (const HazardRuntime& r : rt_) {
        h.Add(static_cast<std::uint64_t>(r.state));
        h.AddDouble(r.intensity);
        h.AddDouble(r.radius);
        h.AddDouble(r.stress);
        h.AddDouble(r.fuel);
        h.Add((r.collapsed ? 1u : 0u) | (r.burnedOut ? 2u : 0u) | (r.routeBlocked ? 4u : 0u) | (r.escalated ? 8u : 0u));
    }
    for (const auto& p : players_) h.AddDouble(p.dose);
    return h.Value();
}

}  // namespace llworld
