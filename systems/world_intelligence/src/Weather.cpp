// LAST LIGHT - Environmental & World Intelligence (WI)
#include "llworld/Weather.h"

#include <algorithm>
#include <cmath>

namespace llworld {

namespace {
constexpr const char* kKindNames[kWeatherKindCount] = {"Clear", "LightRain", "HeavyRain", "Fog", "StrongWind", "Storm"};

WeatherProfile P(double rain, double wind, double fog, double vis, double thermal, double comms, double wet, double exposure,
                 double fireSpread, double suppression, Tick gust, Tick jitter, double gustDb, Tick thunder, double thunderDb,
                 Tick holdMin, Tick holdMax, std::array<double, kWeatherKindCount> next) {
    WeatherProfile p;
    p.rain = rain;
    p.wind = wind;
    p.fog = fog;
    p.visualRangeMul = vis;
    p.thermalRangeMul = thermal;
    p.commsRangeMul = comms;
    p.wetnessPerTick = wet;
    p.electricalExposure = exposure;
    p.fireSpreadMul = fireSpread;
    p.fireRainSuppression = suppression;
    p.gustIntervalTicks = gust;
    p.gustJitterTicks = jitter;
    p.gustLoudnessDb = gustDb;
    p.thunderIntervalTicks = thunder;
    p.thunderLoudnessDb = thunderDb;
    p.holdMinTicks = holdMin;
    p.holdMaxTicks = holdMax;
    p.nextWeights = next;
    return p;
}
}  // namespace

const char* ToString(WeatherKind k) {
    const auto i = static_cast<std::size_t>(k);
    return i < kWeatherKindCount ? kKindNames[i] : "Invalid";
}

bool ParseWeatherKind(std::string_view s, WeatherKind& out) {
    for (std::size_t i = 0; i < kWeatherKindCount; ++i) {
        if (s == kKindNames[i]) {
            out = static_cast<WeatherKind>(i);
            return true;
        }
    }
    return false;
}

WeatherConfig WeatherConfig::MakeDefault() {
    WeatherConfig c;
    //                              rain  wind  fog  vis   therm comms wet     expo   fire  supp  gust jit  gDb thun thDb hold        next: Clr  LRn  HRn  Fog  Wnd  Stm
    c.profiles[0] = P(0.0, 0.10, 0.0, 1.00, 1.00, 1.00, 0.0, 0.000, 1.0, 0.0, 0, 0, 0, 0, 0, 2700, 5400, {0.0, 0.35, 0.0, 0.3, 0.35, 0.0});
    c.profiles[1] = P(0.35, 0.20, 0.05, 0.85, 0.90, 1.00, 0.0004, 0.000, 0.7, 0.5, 0, 0, 0, 0, 0, 1800, 3600, {0.4, 0.0, 0.35, 0.25, 0.0, 0.0});
    c.profiles[2] = P(0.90, 0.40, 0.10, 0.60, 0.70, 0.90, 0.0012, 0.004, 0.3, 2.0, 900, 300, 66, 0, 0, 1800, 3600, {0.0, 0.6, 0.0, 0.0, 0.0, 0.4});
    c.profiles[3] = P(0.0, 0.05, 0.85, 0.30, 0.90, 1.00, 0.0002, 0.000, 1.0, 0.0, 0, 0, 0, 0, 0, 1800, 4500, {0.6, 0.4, 0.0, 0.0, 0.0, 0.0});
    c.profiles[4] = P(0.0, 0.85, 0.0, 0.95, 1.00, 0.95, 0.0, 0.001, 1.8, 0.0, 240, 120, 72, 0, 0, 1200, 2700, {0.5, 0.0, 0.2, 0.0, 0.0, 0.3});
    c.profiles[5] = P(1.00, 0.90, 0.20, 0.45, 0.65, 0.60, 0.0015, 0.020, 0.5, 2.5, 180, 90, 76, 600, 115, 1200, 2700, {0.0, 0.0, 0.7, 0.0, 0.3, 0.0});
    return c;
}

std::vector<std::string> ValidateWeather(const WeatherConfig& c) {
    std::vector<std::string> e;
    auto require = [&e](bool ok, const std::string& m) {
        if (!ok) e.push_back(m);
    };
    require(c.transitionTicks >= 1 && c.updateIntervalTicks >= 1, "weather: transition/update ticks must be >= 1");
    require(c.dryingPerTick >= 0.0 && c.puddleWetness > 0.0 && c.puddleWetness <= 1.0, "weather: drying/puddle values invalid");
    require(c.minVisualRangeFactor > 0.0 && c.minVisualRangeFactor <= 1.0, "weather: min_visual_range_factor must be in (0,1]");
    require(c.minThermalRangeFactor > 0.0 && c.minThermalRangeFactor <= 1.0, "weather: min_thermal_range_factor must be in (0,1]");
    require(c.acousticConfidenceRefCm > 0.0 && c.acousticConfidenceLoss >= 0.0 && c.acousticConfidenceLoss < 1.0,
            "weather: acoustic confidence settings invalid");
    require(c.boundsMin.x <= c.boundsMax.x && c.boundsMin.y <= c.boundsMax.y, "weather: bounds invalid");
    for (std::size_t k = 0; k < kWeatherKindCount; ++k) {
        const WeatherProfile& p = c.profiles[k];
        const std::string n = std::string("weather.") + kKindNames[k] + ": ";
        for (double v : {p.rain, p.wind, p.fog}) require(v >= 0.0 && v <= 1.0, n + "rain/wind/fog must be in [0,1]");
        for (double v : {p.visualRangeMul, p.thermalRangeMul, p.commsRangeMul}) require(v > 0.0 && v <= 1.5, n + "range multipliers must be in (0,1.5]");
        require(p.wetnessPerTick >= 0.0 && p.electricalExposure >= 0.0 && p.fireSpreadMul >= 0.0 && p.fireRainSuppression >= 0.0,
                n + "rates must be >= 0");
        require(p.holdMinTicks >= 1 && p.holdMinTicks <= p.holdMaxTicks, n + "hold_min_ticks must be in [1, hold_max_ticks]");
        require(p.gustIntervalTicks == 0 || p.gustJitterTicks < p.gustIntervalTicks, n + "gust_jitter_ticks must be < gust_interval_ticks");
        double sum = 0.0;
        for (double w : p.nextWeights) {
            require(w >= 0.0, n + "next weights must be >= 0");
            sum += w;
        }
        require(sum > 0.0, n + "needs at least one positive next weight");
    }
    return e;
}

void LoadWeather(const IniDocument& doc, WeatherConfig& c, std::vector<std::string>& errors) {
    for (const IniSection& s : doc.sections) {
        if (s.name == "weather") {
            IniBinder b;
            b.Custom("initial", [&c](std::string_view v) { return ParseWeatherKind(v, c.initial); });
            b.Flag("auto_advance", c.autoAdvance);
            b.Ticks("transition_ticks", c.transitionTicks);
            b.Ticks("update_interval_ticks", c.updateIntervalTicks);
            b.Num("drying_per_tick", c.dryingPerTick);
            b.Num("puddle_wetness", c.puddleWetness);
            b.Num("min_visual_range_factor", c.minVisualRangeFactor);
            b.Num("min_thermal_range_factor", c.minThermalRangeFactor);
            b.Num("acoustic_confidence_ref_cm", c.acousticConfidenceRefCm);
            b.Num("acoustic_confidence_loss", c.acousticConfidenceLoss);
            b.Point("bounds_min", c.boundsMin);
            b.Point("bounds_max", c.boundsMax);
            b.Custom("seed", [&c](std::string_view v) {
                double d = 0.0;
                if (!ParseNumber(v, d) || d < 0.0 || d != std::floor(d)) return false;
                c.seed = static_cast<std::uint64_t>(d);
                return true;
            });
            b.Apply(s, errors);
        } else if (s.name.rfind("weather.", 0) == 0) {
            WeatherKind k{};
            if (!ParseWeatherKind(s.name.substr(8), k)) {
                errors.push_back("line " + std::to_string(s.line) + ": unknown weather kind in [" + s.name + "]");
                continue;
            }
            WeatherProfile& p = c.profiles[static_cast<std::size_t>(k)];
            IniBinder b;
            b.Num("rain", p.rain);
            b.Num("wind", p.wind);
            b.Num("fog", p.fog);
            b.Num("visual_range_mul", p.visualRangeMul);
            b.Num("thermal_range_mul", p.thermalRangeMul);
            b.Num("comms_range_mul", p.commsRangeMul);
            b.Num("wetness_per_tick", p.wetnessPerTick);
            b.Num("electrical_exposure", p.electricalExposure);
            b.Num("fire_spread_mul", p.fireSpreadMul);
            b.Num("fire_rain_suppression", p.fireRainSuppression);
            b.Ticks("gust_interval_ticks", p.gustIntervalTicks);
            b.Ticks("gust_jitter_ticks", p.gustJitterTicks);
            b.Num("gust_loudness_db", p.gustLoudnessDb);
            b.Ticks("thunder_interval_ticks", p.thunderIntervalTicks);
            b.Num("thunder_loudness_db", p.thunderLoudnessDb);
            b.Ticks("hold_min_ticks", p.holdMinTicks);
            b.Ticks("hold_max_ticks", p.holdMaxTicks);
            for (std::size_t n = 0; n < kWeatherKindCount; ++n) {
                double& w = p.nextWeights[n];
                b.Custom(std::string("next.") + kKindNames[n], [&w](std::string_view v) { return ParseNumber(v, w); });
            }
            b.Apply(s, errors);
        } else if (s.name.rfind("gust.", 0) == 0) {
            GustAnchor a;
            IniBinder b;
            b.Point("position", a.position);
            b.Flag("metallic", a.metallic);
            b.Apply(s, errors);
            c.anchors.push_back(a);
        }
    }
}

// ---------------------------------------------------------------------------

WeatherSystem::WeatherSystem(const WeatherConfig& config, WorldEventBus* bus) : cfg_(config), bus_(bus) {
    snap_.from = snap_.to = cfg_.initial;
    Recompute();
}

double WeatherSystem::Roll() { return HashToUnit(HashCombine(cfg_.seed, rng_++)); }

Tick WeatherSystem::RollHold(WeatherKind k) {
    const WeatherProfile& p = cfg_.profiles[static_cast<std::size_t>(k)];
    const Tick span = p.holdMaxTicks - p.holdMinTicks;
    return p.holdMinTicks + static_cast<Tick>(Roll() * static_cast<double>(span + 1));
}

WeatherKind WeatherSystem::RollNext(WeatherKind k) {
    const auto& w = cfg_.profiles[static_cast<std::size_t>(k)].nextWeights;
    double total = 0.0;
    for (double v : w) total += v;
    double r = Roll() * total;
    for (std::size_t i = 0; i < kWeatherKindCount; ++i) {
        if (r < w[i]) return static_cast<WeatherKind>(i);
        r -= w[i];
    }
    return k;
}

void WeatherSystem::Recompute() {
    const WeatherProfile& a = cfg_.profiles[static_cast<std::size_t>(snap_.from)];
    const WeatherProfile& b = cfg_.profiles[static_cast<std::size_t>(snap_.to)];
    const double t = snap_.blend;
    snap_.rain = Lerp(a.rain, b.rain, t);
    snap_.wind = Lerp(a.wind, b.wind, t);
    snap_.fog = Lerp(a.fog, b.fog, t);
    snap_.visualRangeMul = Lerp(a.visualRangeMul, b.visualRangeMul, t);
    snap_.thermalRangeMul = Lerp(a.thermalRangeMul, b.thermalRangeMul, t);
    snap_.commsRangeMul = Lerp(a.commsRangeMul, b.commsRangeMul, t);
    snap_.electricalExposure = Lerp(a.electricalExposure, b.electricalExposure, t);
    snap_.fireSpreadMul = Lerp(a.fireSpreadMul, b.fireSpreadMul, t);
    snap_.fireRainSuppression = Lerp(a.fireRainSuppression, b.fireRainSuppression, t);
    snap_.wetnessPerTick = Lerp(a.wetnessPerTick, b.wetnessPerTick, t);
    ++snap_.version;
}

void WeatherSystem::StartTransition(WeatherKind target, Tick duration, Tick now) {
    if (inTransition_) {
        // Re-target from wherever we are: freeze the current blend as the new base.
        snap_.from = snap_.blend >= 0.5 ? snap_.to : snap_.from;
    }
    snap_.to = target;
    snap_.blend = 0.0;
    inTransition_ = true;
    transitionStart_ = now;
    transitionLength_ = std::max<Tick>(1, duration);
    nextUpdate_ = now;
    Recompute();
}

void WeatherSystem::Reschedule(Tick now) {
    const WeatherProfile& p = cfg_.profiles[static_cast<std::size_t>(Dominant())];
    if (p.gustIntervalTicks > 0 && !cfg_.anchors.empty()) {
        if (nextGust_ == 0) {
            const double j = (Roll() * 2.0 - 1.0) * static_cast<double>(p.gustJitterTicks);
            nextGust_ = now + static_cast<Tick>(std::max(1.0, static_cast<double>(p.gustIntervalTicks) + j));
        }
    } else {
        nextGust_ = 0;
    }
    if (p.thunderIntervalTicks > 0) {
        if (nextThunder_ == 0) nextThunder_ = now + p.thunderIntervalTicks / 2 + static_cast<Tick>(Roll() * static_cast<double>(p.thunderIntervalTicks));
    } else {
        nextThunder_ = 0;
    }
}

bool WeatherSystem::RequestWeather(WeatherKind target, Tick duration, Tick now) {
    if (static_cast<std::size_t>(target) >= kWeatherKindCount) return false;
    if (inTransition_ ? snap_.to == target : snap_.from == target) return false;
    StartTransition(target, duration == 0 ? cfg_.transitionTicks : duration, now);
    if (bus_ != nullptr) {
        WorldEvent e;
        e.type = WorldEventType::WeatherChanged;
        e.source = WorldSystem::Weather;
        e.magnitude = snap_.rain;
        e.aux = static_cast<std::uint32_t>(target);
        bus_->Publish(e);
    }
    return true;
}

void WeatherSystem::Step(Tick now) {
    bool changed = false;
    if (!started_) {
        started_ = true;
        lastWetTick_ = now;
        holdUntil_ = now + RollHold(snap_.from);
        nextUpdate_ = now + cfg_.updateIntervalTicks;
        Reschedule(now);
        changed = true;
    }
    if (now >= nextUpdate_) {
        nextUpdate_ = now + cfg_.updateIntervalTicks;
        ++updates_;
        const WeatherKind dominantBefore = Dominant();
        if (inTransition_) {
            snap_.blend = Clamp01(static_cast<double>(now - transitionStart_) / static_cast<double>(transitionLength_));
            if (snap_.blend >= 1.0) {
                snap_.from = snap_.to;
                snap_.blend = 0.0;
                inTransition_ = false;
                holdUntil_ = now + RollHold(snap_.from);
            }
            Recompute();
            changed = true;
        } else if (cfg_.autoAdvance && now >= holdUntil_) {
            const WeatherKind next = RollNext(snap_.from);
            if (next != snap_.from) {
                StartTransition(next, cfg_.transitionTicks, now);
                changed = true;
            } else {
                holdUntil_ = now + RollHold(snap_.from);
            }
        }
        // Wetness integrates at the coarse update rate only.
        const double dt = static_cast<double>(now - lastWetTick_);
        lastWetTick_ = now;
        const double before = snap_.wetness;
        if (snap_.wetnessPerTick > 0.0) snap_.wetness = Clamp01(snap_.wetness + snap_.wetnessPerTick * dt);
        else snap_.wetness = Clamp01(snap_.wetness - cfg_.dryingPerTick * dt);
        if (std::fabs(before - snap_.wetness) > 1e-12) {
            ++snap_.version;
            changed = true;
        }
        if (Dominant() != dominantBefore) {
            nextGust_ = 0;
            nextThunder_ = 0;
        }
        Reschedule(now);
    }

    const WeatherProfile& p = cfg_.profiles[static_cast<std::size_t>(Dominant())];
    if (nextGust_ != 0 && now >= nextGust_ && bus_ != nullptr) {
        const std::size_t idx = static_cast<std::size_t>(Roll() * static_cast<double>(cfg_.anchors.size())) % cfg_.anchors.size();
        WorldEvent e;
        e.type = WorldEventType::WindGust;
        e.source = WorldSystem::Weather;
        e.position = cfg_.anchors[idx].position;
        e.magnitude = p.gustLoudnessDb;
        e.aux = cfg_.anchors[idx].metallic ? 1u : 0u;
        e.subject = static_cast<std::uint32_t>(idx);
        bus_->Publish(e);
        nextGust_ = 0;
        Reschedule(now);
    }
    if (nextThunder_ != 0 && now >= nextThunder_ && bus_ != nullptr) {
        WorldEvent e;
        e.type = WorldEventType::Thunder;
        e.source = WorldSystem::Weather;
        e.position = Vec3{Lerp(cfg_.boundsMin.x, cfg_.boundsMax.x, Roll()), Lerp(cfg_.boundsMin.y, cfg_.boundsMax.y, Roll()), 0.0};
        e.magnitude = p.thunderLoudnessDb;
        bus_->Publish(e);
        nextThunder_ = 0;
        Reschedule(now);
    }
    if (changed && bus_ != nullptr) {
        WorldEvent e;
        e.type = WorldEventType::WeatherChanged;
        e.source = WorldSystem::Weather;
        e.magnitude = snap_.rain;
        e.aux = static_cast<std::uint32_t>(snap_.to);
        bus_->Publish(e);
    }
}

double WeatherSystem::VisualRangeFactor() const { return std::max(cfg_.minVisualRangeFactor, snap_.visualRangeMul); }
double WeatherSystem::ThermalRangeFactor() const { return std::max(cfg_.minThermalRangeFactor, snap_.thermalRangeMul); }

double WeatherSystem::AcousticReportConfidenceScale(double distanceCm) const {
    const double noise = Clamp01(0.6 * snap_.rain + 0.4 * snap_.wind);
    const double far = Clamp01(distanceCm / cfg_.acousticConfidenceRefCm);
    return std::max(0.2, 1.0 - cfg_.acousticConfidenceLoss * noise * far);
}

SurfaceConditions WeatherSystem::Surface() const {
    SurfaceConditions s;
    s.wetness = snap_.wetness;
    s.puddles = snap_.wetness >= cfg_.puddleWetness;
    return s;
}

std::uint64_t WeatherSystem::StateHash() const {
    StateHasher h;
    h.Add(static_cast<std::uint64_t>(snap_.from));
    h.Add(static_cast<std::uint64_t>(snap_.to));
    h.AddDouble(snap_.blend);
    h.AddDouble(snap_.wetness);
    h.AddDouble(snap_.rain);
    h.AddDouble(snap_.wind);
    h.Add(rng_);
    h.Add(nextGust_);
    h.Add(nextThunder_);
    h.Add(holdUntil_);
    return h.Value();
}

}  // namespace llworld
