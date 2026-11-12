// LAST LIGHT - Acoustic Intelligence System (AIS)
#include "ais/AcousticConfig.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

#include "ais/DecoySystem.h"

namespace ais {

namespace {

constexpr const char* kCategoryNames[kCategoryCount] = {
    "Gunfire", "Footstep",  "Sprint",    "JumpLanding", "Door",          "BreakObject",
    "MetalImpact", "Explosion", "Machinery", "Generator",   "Environmental", "Decoy",
    "Alarm",
};
constexpr const char* kSurfaceNames[kSurfaceCount] = {
    "None", "Concrete", "Metal", "Wood", "Gravel", "Grass", "Water", "Glass", "Debris",
};

CategoryDescriptor Cat(double loudness, BandArray spectrum, Tick duration, double threat,
                       double priority, bool hostile, bool regular, double artificial = 0.0) {
    CategoryDescriptor d;
    d.loudnessDb = loudness;
    d.spectrumDb = spectrum;
    d.durationTicks = duration;
    d.threat = threat;
    d.queuePriorityDb = priority;
    d.hostile = hostile;
    d.regularityIsSuspicious = regular;
    d.defaultArtificiality = artificial;
    return d;
}

SurfaceDescriptor Surf(double offset, BandArray shift) {
    SurfaceDescriptor s;
    s.loudnessOffsetDb = offset;
    s.spectrumShiftDb = shift;
    return s;
}

MaterialDescriptor Mat(const char* name, BandArray loss, BandArray perMeter) {
    MaterialDescriptor m;
    m.name = name;
    m.lossDb = loss;
    m.lossDbPerMeter = perMeter;
    return m;
}

}  // namespace

const char* ToString(StimulusCategory c) {
    const std::size_t i = Index(c);
    return i < kCategoryCount ? kCategoryNames[i] : "Invalid";
}

const char* ToString(SurfaceType s) {
    const std::size_t i = Index(s);
    return i < kSurfaceCount ? kSurfaceNames[i] : "Invalid";
}

bool ParseCategory(std::string_view name, StimulusCategory& out) {
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        if (name == kCategoryNames[i]) {
            out = static_cast<StimulusCategory>(i);
            return true;
        }
    }
    return false;
}

bool ParseSurface(std::string_view name, SurfaceType& out) {
    for (std::size_t i = 0; i < kSurfaceCount; ++i) {
        if (name == kSurfaceNames[i]) {
            out = static_cast<SurfaceType>(i);
            return true;
        }
    }
    return false;
}

RobotSensoryProfile MakeSentinelProfile() {
    RobotSensoryProfile p;
    p.name = "SENTINEL";
    p.bandSensitivityDb = {10.0, 10.0, 10.0};  // balanced hearing
    p.detectionSnrDb = 6.0;
    p.localizationSkill = 1.0;
    p.sensoryUpdateIntervalTicks = 3;
    p.maxInvestigationRangeCm = 4000.0;  // moderate
    p.repetitionBonus = 0.45;            // strong at spotting repeated disturbances
    p.repetitionCap = 5;
    p.traceMergeRadiusCm = 700.0;
    p.confidenceHalfLifeTicks = 360.0;
    p.decoyDiscrimination = 0.35;
    p.artificialInterestScale = 0.25;
    p.broadcastsReports = false;
    p.receivesReports = true;
    p.objectiveCommitment = 1.0;
    return p;
}

RobotSensoryProfile MakeWraithProfile() {
    RobotSensoryProfile p;
    p.name = "WRAITH";
    p.bandSensitivityDb = {2.0, 10.0, 13.0};  // high-band specialist, poor through walls
    p.detectionSnrDb = 5.0;
    p.localizationSkill = 1.4;
    p.sensoryUpdateIntervalTicks = 2;
    p.maxInvestigationRangeCm = 7000.0;
    p.repetitionBonus = 0.15;
    p.repetitionCap = 3;
    p.confidenceHalfLifeTicks = 240.0;
    p.suspicionHalfLifeTicks = 200.0;
    // Hunts electronic and mechanical signatures; treats a synthetic signal as
    // evidence that a player placed it nearby rather than as noise to ignore.
    p.categoryInterest[Index(StimulusCategory::MetalImpact)] = 1.3;
    p.categoryInterest[Index(StimulusCategory::Machinery)] = 1.4;
    p.categoryInterest[Index(StimulusCategory::Generator)] = 1.5;
    p.categoryInterest[Index(StimulusCategory::Decoy)] = 1.5;
    p.categoryInterest[Index(StimulusCategory::Footstep)] = 0.8;
    p.decoyDiscrimination = 0.85;
    p.artificialInterestScale = 1.4;
    p.searchRadiusMaxCm = 2200.0;
    p.searchPointCount = 6;
    p.broadcastsReports = true;
    p.broadcastMinConfidence = 0.45;
    p.objectiveCommitment = 0.7;
    return p;
}

std::vector<DecoyDefinition> MakeDefaultDecoys() {
    std::vector<DecoyDefinition> out;
    DecoyDefinition timed;
    timed.name = "TimedNoiseEmitter";
    timed.pattern = DecoyPattern::TimedBurst;
    timed.apparentCategory = StimulusCategory::BreakObject;
    timed.loudnessDb = 82.0;
    timed.spectrumDb = {0.0, 0.0, 2.0};
    timed.artificiality = 0.35;
    timed.armDelayTicks = 90;
    timed.lifetimeTicks = 120;
    timed.maxEmissions = 1;
    out.push_back(timed);

    DecoyDefinition metal;
    metal.name = "RepeatingMetallic";
    metal.pattern = DecoyPattern::RepeatingMetallic;
    metal.apparentCategory = StimulusCategory::MetalImpact;
    metal.loudnessDb = 74.0;
    metal.spectrumDb = {-6.0, 0.0, 4.0};
    metal.artificiality = 0.55;
    metal.armDelayTicks = 15;
    metal.intervalTicks = 24;
    metal.jitterTicks = 0;  // metronomic: the tell
    metal.lifetimeTicks = 900;
    out.push_back(metal);

    DecoyDefinition mech;
    mech.name = "RecordedMechanical";
    mech.pattern = DecoyPattern::RecordedMechanical;
    mech.apparentCategory = StimulusCategory::Machinery;
    mech.loudnessDb = 70.0;
    mech.spectrumDb = {3.0, 0.0, -4.0};
    mech.artificiality = 0.2;
    mech.armDelayTicks = 10;
    mech.intervalTicks = 40;
    mech.jitterTicks = 12;
    mech.lifetimeTicks = 1200;
    mech.durationTicks = 30;
    out.push_back(mech);

    DecoyDefinition radio;
    radio.name = "EmergencyRadio";
    radio.pattern = DecoyPattern::EmergencyRadio;
    radio.apparentCategory = StimulusCategory::Decoy;
    radio.loudnessDb = 68.0;
    radio.spectrumDb = {-10.0, 2.0, 0.0};
    radio.artificiality = 0.8;
    radio.armDelayTicks = 30;
    radio.intervalTicks = 90;
    radio.jitterTicks = 6;
    radio.lifetimeTicks = 1800;
    radio.durationTicks = 45;
    out.push_back(radio);
    return out;
}

AisConfig AisConfig::MakeDefault() {
    AisConfig c;
    using SC = StimulusCategory;
    // loudness, spectrum{low,mid,high}, duration, threat, priority, hostile, regular
    c.categories[Index(SC::Gunfire)] = Cat(100.0, {0.0, 0.0, -3.0}, 1, 1.0, 30.0, true, false);
    c.categories[Index(SC::Footstep)] = Cat(40.0, {-6.0, 0.0, -3.0}, 1, 0.55, 0.0, false, false);
    c.categories[Index(SC::Sprint)] = Cat(55.0, {-4.0, 0.0, -2.0}, 1, 0.7, 2.0, false, false);
    c.categories[Index(SC::JumpLanding)] = Cat(60.0, {3.0, 0.0, -3.0}, 1, 0.6, 2.0, false, false);
    c.categories[Index(SC::Door)] = Cat(58.0, {0.0, 0.0, 0.0}, 6, 0.6, 4.0, false, true);
    c.categories[Index(SC::BreakObject)] = Cat(75.0, {0.0, 0.0, 3.0}, 3, 0.75, 8.0, false, true);
    c.categories[Index(SC::MetalImpact)] = Cat(72.0, {-6.0, 0.0, 4.0}, 1, 0.6, 6.0, false, true);
    c.categories[Index(SC::Explosion)] = Cat(125.0, {6.0, 0.0, -6.0}, 6, 1.0, 40.0, true, false);
    c.categories[Index(SC::Machinery)] = Cat(70.0, {4.0, 0.0, -6.0}, 30, 0.25, 0.0, false, true);
    c.categories[Index(SC::Generator)] = Cat(68.0, {6.0, 0.0, -8.0}, 30, 0.35, 0.0, false, true);
    c.categories[Index(SC::Environmental)] = Cat(55.0, {0.0, 0.0, 0.0}, 3, 0.1, -10.0, false, false);
    c.categories[Index(SC::Decoy)] = Cat(70.0, {-6.0, 2.0, 0.0}, 30, 0.6, 0.0, false, true, 0.6);
    // Legitimate facility alarm: loud, mid/high heavy, periodic by nature (not a decoy tell).
    c.categories[Index(SC::Alarm)] = Cat(95.0, {-8.0, 0.0, 2.0}, 45, 0.85, 20.0, false, false);

    using ST = SurfaceType;
    c.surfaces[Index(ST::None)] = Surf(0.0, {0.0, 0.0, 0.0});
    c.surfaces[Index(ST::Concrete)] = Surf(0.0, {0.0, 0.0, 1.0});
    c.surfaces[Index(ST::Metal)] = Surf(8.0, {-2.0, 0.0, 6.0});
    c.surfaces[Index(ST::Wood)] = Surf(3.0, {2.0, 0.0, -1.0});
    c.surfaces[Index(ST::Gravel)] = Surf(5.0, {0.0, 0.0, 4.0});
    c.surfaces[Index(ST::Grass)] = Surf(-6.0, {0.0, 0.0, -4.0});
    c.surfaces[Index(ST::Water)] = Surf(4.0, {0.0, 2.0, 2.0});
    c.surfaces[Index(ST::Glass)] = Surf(10.0, {-4.0, 0.0, 8.0});
    c.surfaces[Index(ST::Debris)] = Surf(6.0, {0.0, 2.0, 3.0});

    c.materials = {
        Mat("Default", {10.0, 15.0, 20.0}, {10.0, 15.0, 20.0}),
        Mat("Concrete", {25.0, 35.0, 45.0}, {20.0, 30.0, 40.0}),
        Mat("Brick", {20.0, 30.0, 40.0}, {15.0, 25.0, 35.0}),
        Mat("Wood", {8.0, 14.0, 20.0}, {10.0, 15.0, 20.0}),
        Mat("MetalSheet", {10.0, 20.0, 30.0}, {30.0, 45.0, 60.0}),
        Mat("Glass", {12.0, 18.0, 25.0}, {20.0, 30.0, 40.0}),
        Mat("Foliage", {1.0, 3.0, 6.0}, {0.5, 1.5, 3.0}),
        Mat("Fabric", {2.0, 4.0, 8.0}, {2.0, 4.0, 8.0}),
    };
    c.robotProfiles = {MakeSentinelProfile(), MakeWraithProfile()};
    c.decoys = MakeDefaultDecoys();
    return c;
}

int AisConfig::FindMaterial(std::string_view name) const {
    for (std::size_t i = 0; i < materials.size(); ++i) {
        if (materials[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

const RobotSensoryProfile* AisConfig::FindProfile(std::string_view name) const {
    for (const auto& p : robotProfiles) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const DecoyDefinition* AisConfig::FindDecoy(std::string_view name) const {
    for (const auto& d : decoys) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

std::vector<std::string> ValidateConfig(const AisConfig& c) {
    std::vector<std::string> e;
    auto require = [&e](bool ok, const std::string& what) {
        if (!ok) e.push_back(what);
    };
    require(!c.materials.empty(), "materials: at least the Default material is required");
    require(c.materials.size() <= 0xFFFFu, "materials: too many materials");
    require(c.propagation.referenceDistanceCm > 0.0, "propagation.reference_distance_cm must be > 0");
    require(c.propagation.exteriorSpreadingFactor > 0.0 && c.propagation.interiorSpreadingFactor > 0.0,
            "propagation: spreading factors must be > 0");
    require(c.propagation.maxAudibleRadiusCm > 0.0, "propagation.max_audible_radius_cm must be > 0");
    require(c.bus.queueCapacity > 0 && c.bus.maxDrainPerTick > 0 && c.bus.maxAcceptedPerTick > 0,
            "bus: capacities must be > 0");
    require(c.bus.dedupHistory > 0 && c.bus.maxTrackedSources > 0, "bus: history sizes must be > 0");
    require(c.bus.sourceBurst >= 1.0 && c.bus.sourceRefillPerTick >= 0.0, "bus: token bucket invalid");
    require(c.world.ticksPerSecond > 0.0 && c.world.gridCellCm > 0.0, "world: tick rate and grid cell must be > 0");
    require(c.world.maxPerceptionChecksPerTick > 0, "world.max_perception_checks_per_tick must be > 0");
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        const auto& d = c.categories[i];
        const std::string n = std::string("category.") + ToString(static_cast<StimulusCategory>(i));
        require(d.threat >= 0.0 && d.threat <= 1.0, n + ".threat must be in [0,1]");
        require(d.durationTicks >= 1, n + ".duration_ticks must be >= 1");
        require(d.defaultArtificiality >= 0.0 && d.defaultArtificiality <= 1.0, n + ".artificiality must be in [0,1]");
    }
    for (std::size_t i = 0; i < c.robotProfiles.size(); ++i) {
        const auto& p = c.robotProfiles[i];
        const std::string n = "robot." + p.name;
        require(!p.name.empty(), "robot profile with empty name");
        for (std::size_t j = 0; j < i; ++j) {
            require(c.robotProfiles[j].name != p.name, n + ": duplicate profile name");
        }
        require(p.suspiciousThreshold > 0.0 && p.suspiciousThreshold < p.investigateThreshold &&
                    p.investigateThreshold < p.combatThreshold && p.combatThreshold <= 100.0,
                n + ": thresholds must satisfy 0 < suspicious < investigate < combat <= 100");
        // Design rule: one sound can never take an unaware robot to combat.
        require(p.maxSingleEventGain > 0.0 && p.maxSingleEventGain < p.combatThreshold - p.suspiciousThreshold,
                n + ".max_single_event_gain must be below combat - suspicious threshold");
        require(p.sensoryUpdateIntervalTicks >= 1, n + ".sensory_update_interval_ticks must be >= 1");
        require(p.inboxCapacity >= 1 && p.memoryCapacity >= 1, n + ": inbox/memory capacity must be >= 1");
        require(p.localizationSkill > 0.0, n + ".localization_skill must be > 0");
        require(p.confidenceHalfLifeTicks > 0.0 && p.suspicionHalfLifeTicks > 0.0, n + ": half-lives must be > 0");
        require(p.searchPointCount >= 1 && p.searchDurationTicks >= 1, n + ": search needs points and duration");
        require(p.searchRadiusMinCm > 0.0 && p.searchRadiusMinCm <= p.searchRadiusMaxCm, n + ": search radii invalid");
        require(p.forgetConfidence >= 0.0 && p.forgetConfidence < p.lostConfidence && p.lostConfidence < 1.0,
                n + ": forget < lost < 1 required");
        require(p.decoyDiscrimination >= 0.0 && p.decoyDiscrimination <= 1.0, n + ".decoy_discrimination must be in [0,1]");
        require(p.sharedReportWeight >= 0.0 && p.sharedReportWeight <= 1.0, n + ".shared_report_weight must be in [0,1]");
        require(p.arrivalRadiusCm > 0.0 && p.maxInvestigationRangeCm > 0.0, n + ": ranges must be > 0");
        for (double v : p.categoryInterest) require(v >= 0.0, n + ": interest must be >= 0");
    }
    for (const auto& d : c.decoys) {
        const std::string n = "decoy." + d.name;
        require(d.intervalTicks >= 1, n + ".interval_ticks must be >= 1");
        require(d.lifetimeTicks >= 1, n + ".lifetime_ticks must be >= 1");
        require(d.jitterTicks < d.intervalTicks, n + ".jitter_ticks must be < interval_ticks");
        require(d.artificiality >= 0.0 && d.artificiality <= 1.0, n + ".artificiality must be in [0,1]");
    }
    return e;
}

AcousticEvent MakeEvent(const AisConfig& config, StimulusCategory category, const Vec3& position,
                        Tick tick, SurfaceType surface, EntityId sourceId) {
    const CategoryDescriptor& d = config.categories[Index(category)];
    AcousticEvent e;
    e.category = category;
    e.apparentCategory = category;
    e.position = position;
    e.emitTick = tick;
    e.loudnessDb = d.loudnessDb;
    e.spectrumDb = d.spectrumDb;
    e.surface = surface;
    e.durationTicks = d.durationTicks;
    e.sourceId = sourceId;
    e.artificiality = d.defaultArtificiality;
    return e;
}

// ---------------------------------------------------------------------------
// INI loader
// ---------------------------------------------------------------------------
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool ParseDouble(std::string_view s, double& out) {
    const std::string tmp(Trim(s));
    if (tmp.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(tmp.c_str(), &end);
    if (errno != 0 || end != tmp.c_str() + tmp.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool ParseUnsigned(std::string_view s, std::uint64_t& out) {
    const std::string tmp(Trim(s));
    if (tmp.empty() || tmp[0] == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(tmp.c_str(), &end, 0);
    if (errno != 0 || end != tmp.c_str() + tmp.size()) return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

bool ParseBool(std::string_view s, bool& out) {
    const std::string_view t = Trim(s);
    if (t == "true" || t == "1" || t == "yes") { out = true; return true; }
    if (t == "false" || t == "0" || t == "no") { out = false; return true; }
    return false;
}

bool ParseBand(std::string_view s, BandArray& out) {
    BandArray tmp{};
    std::size_t i = 0;
    while (true) {
        const std::size_t comma = s.find(',');
        const std::string_view part = comma == std::string_view::npos ? s : s.substr(0, comma);
        if (i >= kBandCount || !ParseDouble(part, tmp[i])) return false;
        ++i;
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    if (i != kBandCount) return false;
    out = tmp;
    return true;
}

// Maps keys of one section onto fields. Each setter returns false on a parse
// error; a missing key is reported separately as "unknown key".
class Binder {
public:
    using Setter = std::function<bool(std::string_view)>;
    void Num(const char* key, double& f) { add(key, [&f](std::string_view v) { return ParseDouble(v, f); }); }
    void Band(const char* key, BandArray& f) { add(key, [&f](std::string_view v) { return ParseBand(v, f); }); }
    void Bool(const char* key, bool& f) { add(key, [&f](std::string_view v) { return ParseBool(v, f); }); }
    template <class U>
    void Uint(const char* key, U& f) {
        add(key, [&f](std::string_view v) {
            std::uint64_t t = 0;
            if (!ParseUnsigned(v, t)) return false;
            if (t > static_cast<std::uint64_t>(static_cast<U>(~U{0}))) return false;
            f = static_cast<U>(t);
            return true;
        });
    }
    void Category(const char* key, StimulusCategory& f) {
        add(key, [&f](std::string_view v) { return ParseCategory(Trim(v), f); });
    }
    void Custom(std::string key, Setter s) { entries_.emplace_back(std::move(key), std::move(s)); }

    // 1 = ok, 0 = parse error, -1 = unknown key
    int Apply(std::string_view key, std::string_view value) const {
        for (const auto& e : entries_) {
            if (e.first == key) return e.second(value) ? 1 : 0;
        }
        return -1;
    }

private:
    void add(const char* key, Setter s) { entries_.emplace_back(key, std::move(s)); }
    std::vector<std::pair<std::string, Setter>> entries_;
};

void BindProfile(Binder& b, RobotSensoryProfile& p) {
    b.Band("band_sensitivity_db", p.bandSensitivityDb);
    b.Num("detection_snr_db", p.detectionSnrDb);
    b.Num("localization_skill", p.localizationSkill);
    b.Uint("sensory_update_interval_ticks", p.sensoryUpdateIntervalTicks);
    b.Uint("inbox_capacity", p.inboxCapacity);
    b.Num("base_event_gain", p.baseEventGain);
    b.Num("suspicion_gain_scale", p.suspicionGainScale);
    b.Num("max_single_event_gain", p.maxSingleEventGain);
    b.Num("repetition_bonus", p.repetitionBonus);
    b.Uint("repetition_cap", p.repetitionCap);
    b.Num("decoy_discrimination", p.decoyDiscrimination);
    b.Num("artificial_evidence_threshold", p.artificialEvidenceThreshold);
    b.Num("artificial_interest_scale", p.artificialInterestScale);
    b.Num("habituation_factor", p.habituationFactor);
    b.Uint("memory_capacity", p.memoryCapacity);
    b.Num("trace_merge_radius_cm", p.traceMergeRadiusCm);
    b.Num("confidence_half_life_ticks", p.confidenceHalfLifeTicks);
    b.Num("suspicion_half_life_ticks", p.suspicionHalfLifeTicks);
    b.Num("uncertainty_growth_cm_per_tick", p.uncertaintyGrowthCmPerTick);
    b.Num("min_uncertainty_cm", p.minUncertaintyCm);
    b.Num("forget_confidence", p.forgetConfidence);
    b.Num("lost_confidence", p.lostConfidence);
    b.Num("suspicious_threshold", p.suspiciousThreshold);
    b.Num("investigate_threshold", p.investigateThreshold);
    b.Num("combat_threshold", p.combatThreshold);
    b.Num("calm_hysteresis", p.calmHysteresis);
    b.Num("combat_max_uncertainty_cm", p.combatMaxUncertaintyCm);
    b.Uint("combat_lose_ticks", p.combatLoseTicks);
    b.Num("max_investigation_range_cm", p.maxInvestigationRangeCm);
    b.Num("arrival_radius_cm", p.arrivalRadiusCm);
    b.Num("retarget_hysteresis", p.retargetHysteresis);
    b.Uint("search_duration_ticks", p.searchDurationTicks);
    b.Uint("search_point_count", p.searchPointCount);
    b.Num("search_radius_min_cm", p.searchRadiusMinCm);
    b.Num("search_radius_max_cm", p.searchRadiusMaxCm);
    b.Bool("broadcasts_reports", p.broadcastsReports);
    b.Bool("receives_reports", p.receivesReports);
    b.Num("broadcast_min_confidence", p.broadcastMinConfidence);
    b.Num("shared_report_weight", p.sharedReportWeight);
    b.Num("shared_uncertainty_inflation_cm", p.sharedUncertaintyInflationCm);
    b.Uint("support_claim_ticks", p.supportClaimTicks);
    b.Num("objective_commitment", p.objectiveCommitment);
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        double& f = p.categoryInterest[i];
        b.Custom(std::string("interest.") + ToString(static_cast<StimulusCategory>(i)),
                 [&f](std::string_view v) { return ParseDouble(v, f); });
    }
}

void BindCategory(Binder& b, CategoryDescriptor& d) {
    b.Num("loudness_db", d.loudnessDb);
    b.Band("spectrum_db", d.spectrumDb);
    b.Uint("duration_ticks", d.durationTicks);
    b.Num("threat", d.threat);
    b.Num("queue_priority_db", d.queuePriorityDb);
    b.Bool("hostile", d.hostile);
    b.Bool("regularity_is_suspicious", d.regularityIsSuspicious);
    b.Num("artificiality", d.defaultArtificiality);
}

void BindDecoy(Binder& b, DecoyDefinition& d) {
    b.Custom("pattern", [&d](std::string_view v) {
        const std::string_view t = Trim(v);
        if (t == "TimedBurst") d.pattern = DecoyPattern::TimedBurst;
        else if (t == "RepeatingMetallic") d.pattern = DecoyPattern::RepeatingMetallic;
        else if (t == "RecordedMechanical") d.pattern = DecoyPattern::RecordedMechanical;
        else if (t == "EmergencyRadio") d.pattern = DecoyPattern::EmergencyRadio;
        else return false;
        return true;
    });
    b.Category("apparent_category", d.apparentCategory);
    b.Num("loudness_db", d.loudnessDb);
    b.Band("spectrum_db", d.spectrumDb);
    b.Num("artificiality", d.artificiality);
    b.Uint("arm_delay_ticks", d.armDelayTicks);
    b.Uint("interval_ticks", d.intervalTicks);
    b.Uint("jitter_ticks", d.jitterTicks);
    b.Uint("lifetime_ticks", d.lifetimeTicks);
    b.Uint("max_emissions", d.maxEmissions);
    b.Uint("duration_ticks", d.durationTicks);
}

void BindPropagation(Binder& b, PropagationConfig& p) {
    b.Num("reference_distance_cm", p.referenceDistanceCm);
    b.Num("exterior_spreading_factor", p.exteriorSpreadingFactor);
    b.Num("interior_spreading_factor", p.interiorSpreadingFactor);
    b.Band("air_absorption_db_per_100m", p.airAbsorptionDbPer100m);
    b.Num("interior_exterior_transition_db", p.interiorExteriorTransitionDb);
    b.Uint("max_occlusion_hits", p.maxOcclusionHits);
    b.Num("max_occlusion_loss_db", p.maxOcclusionLossDb);
    b.Band("exterior_noise_floor_db", p.exteriorNoiseFloorDb);
    b.Band("interior_noise_floor_db", p.interiorNoiseFloorDb);
    b.Band("rain_noise_db", p.rainNoiseDb);
    b.Band("wind_noise_db", p.windNoiseDb);
    b.Band("rain_attenuation_db_per_100m", p.rainAttenuationDbPer100m);
    b.Band("wind_attenuation_db_per_100m", p.windAttenuationDbPer100m);
    b.Num("interior_weather_reduction_db", p.interiorWeatherReductionDb);
    b.Num("localization_base_error_fraction", p.localizationBaseErrorFraction);
    b.Num("localization_min_error_cm", p.localizationMinErrorCm);
    b.Num("localization_per_occluder_scale", p.localizationPerOccluderScale);
    b.Num("localization_interior_scale", p.localizationInteriorScale);
    b.Num("localization_snr_reference_db", p.localizationSnrReferenceDb);
    b.Num("max_audible_radius_cm", p.maxAudibleRadiusCm);
}

void BindBus(Binder& b, BusConfig& c) {
    b.Uint("queue_capacity", c.queueCapacity);
    b.Uint("max_drain_per_tick", c.maxDrainPerTick);
    b.Uint("max_accepted_per_tick", c.maxAcceptedPerTick);
    b.Uint("dedup_window_ticks", c.dedupWindowTicks);
    b.Num("dedup_radius_cm", c.dedupRadiusCm);
    b.Uint("dedup_history", c.dedupHistory);
    b.Num("source_burst", c.sourceBurst);
    b.Num("source_refill_per_tick", c.sourceRefillPerTick);
    b.Uint("max_tracked_sources", c.maxTrackedSources);
    b.Uint("max_event_age_ticks", c.maxEventAgeTicks);
    b.Uint("max_future_ticks", c.maxFutureTicks);
    b.Num("min_loudness_db", c.minLoudnessDb);
    b.Num("max_loudness_db", c.maxLoudnessDb);
    b.Uint("max_duration_ticks", c.maxDurationTicks);
}

void BindWorld(Binder& b, WorldConfig& w) {
    b.Num("ticks_per_second", w.ticksPerSecond);
    b.Num("grid_cell_cm", w.gridCellCm);
    b.Uint("max_perception_checks_per_tick", w.maxPerceptionChecksPerTick);
    b.Uint("world_seed", w.worldSeed);
    b.Num("cosmetic_radius_scale", w.cosmeticRadiusScale);
    b.Uint("max_cosmetic_cues_per_client_per_tick", w.maxCosmeticCuesPerClientPerTick);
    b.Uint("report_latency_ticks", w.reportLatencyTicks);
    b.Num("report_range_cm", w.reportRangeCm);
    b.Uint("max_reports_in_flight", w.maxReportsInFlight);
    b.Uint("report_min_interval_ticks", w.reportMinIntervalTicks);
}

}  // namespace

ConfigLoadResult LoadConfigFromIni(std::string_view text, AisConfig& config) {
    ConfigLoadResult result;
    auto fail = [&result](std::size_t line, const std::string& msg) {
        result.ok = false;
        result.errors.push_back("line " + std::to_string(line) + ": " + msg);
    };

    Binder binder;
    bool sectionValid = false;
    std::string sectionName;
    std::size_t lineNo = 0;

    while (!text.empty()) {
        ++lineNo;
        const std::size_t nl = text.find('\n');
        std::string_view line = nl == std::string_view::npos ? text : text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        const std::size_t comment = line.find_first_of(";#");
        if (comment != std::string_view::npos) line = line.substr(0, comment);
        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                fail(lineNo, "malformed section header");
                sectionValid = false;
                continue;
            }
            sectionName = std::string(Trim(line.substr(1, line.size() - 2)));
            binder = Binder{};
            sectionValid = true;
            const std::size_t dot = sectionName.find('.');
            const std::string kind = sectionName.substr(0, dot);
            const std::string name = dot == std::string::npos ? std::string{} : sectionName.substr(dot + 1);
            if (sectionName == "propagation") {
                BindPropagation(binder, config.propagation);
            } else if (sectionName == "bus") {
                BindBus(binder, config.bus);
            } else if (sectionName == "world") {
                BindWorld(binder, config.world);
            } else if (kind == "category" && !name.empty()) {
                StimulusCategory c{};
                if (!ParseCategory(name, c)) {
                    fail(lineNo, "unknown category '" + name + "'");
                    sectionValid = false;
                } else {
                    BindCategory(binder, config.categories[Index(c)]);
                }
            } else if (kind == "surface" && !name.empty()) {
                SurfaceType s{};
                if (!ParseSurface(name, s)) {
                    fail(lineNo, "unknown surface '" + name + "'");
                    sectionValid = false;
                } else {
                    SurfaceDescriptor& d = config.surfaces[Index(s)];
                    binder.Num("loudness_offset_db", d.loudnessOffsetDb);
                    binder.Band("spectrum_shift_db", d.spectrumShiftDb);
                }
            } else if (kind == "material" && !name.empty()) {
                int idx = config.FindMaterial(name);
                if (idx < 0) {
                    MaterialDescriptor m;
                    m.name = name;
                    config.materials.push_back(m);
                    idx = static_cast<int>(config.materials.size() - 1);
                }
                MaterialDescriptor& m = config.materials[static_cast<std::size_t>(idx)];
                binder.Band("loss_db", m.lossDb);
                binder.Band("loss_db_per_meter", m.lossDbPerMeter);
            } else if (kind == "robot" && !name.empty()) {
                RobotSensoryProfile* p = nullptr;
                for (auto& existing : config.robotProfiles) {
                    if (existing.name == name) p = &existing;
                }
                if (p == nullptr) {
                    RobotSensoryProfile fresh;
                    // `base = SENTINEL` must come first to inherit; otherwise defaults.
                    fresh.name = name;
                    config.robotProfiles.push_back(fresh);
                    p = &config.robotProfiles.back();
                }
                RobotSensoryProfile* target = p;
                AisConfig* cfg = &config;
                binder.Custom("base", [target, cfg](std::string_view v) {
                    const RobotSensoryProfile* base = cfg->FindProfile(Trim(v));
                    if (base == nullptr || base == target) return false;
                    const std::string keep = target->name;
                    *target = *base;
                    target->name = keep;
                    return true;
                });
                BindProfile(binder, *target);
            } else if (kind == "decoy" && !name.empty()) {
                DecoyDefinition* d = nullptr;
                for (auto& existing : config.decoys) {
                    if (existing.name == name) d = &existing;
                }
                if (d == nullptr) {
                    DecoyDefinition fresh;
                    fresh.name = name;
                    config.decoys.push_back(fresh);
                    d = &config.decoys.back();
                }
                BindDecoy(binder, *d);
            } else {
                fail(lineNo, "unknown section '" + sectionName + "'");
                sectionValid = false;
            }
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            fail(lineNo, "expected key = value");
            continue;
        }
        if (!sectionValid) {
            fail(lineNo, "key outside a valid section");
            continue;
        }
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));
        const int r = binder.Apply(key, value);
        if (r < 0) fail(lineNo, "unknown key '" + std::string(key) + "' in [" + sectionName + "]");
        if (r == 0) fail(lineNo, "invalid value '" + std::string(value) + "' for '" + std::string(key) + "'");
    }

    for (const auto& msg : ValidateConfig(config)) {
        result.ok = false;
        result.errors.push_back("validation: " + msg);
    }
    return result;
}

ConfigLoadResult LoadConfigFromFile(const std::string& path, AisConfig& config) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ConfigLoadResult r;
        r.ok = false;
        r.errors.push_back("cannot open '" + path + "'");
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    return LoadConfigFromIni(text, config);
}

}  // namespace ais
