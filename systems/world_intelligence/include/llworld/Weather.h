// LAST LIGHT - Environmental & World Intelligence (WI)
// Dynamic weather: data-driven states, deterministic Markov transitions,
// blended gameplay parameters and discrete wind/thunder events. Evaluated at
// a coarse interval (default 1 s), never per frame and never per robot.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "llworld/Ini.h"
#include "llworld/WorldEvents.h"
#include "llworld/WorldTypes.h"

namespace llworld {

enum class WeatherKind : std::uint8_t { Clear = 0, LightRain, HeavyRain, Fog, StrongWind, Storm };
inline constexpr std::size_t kWeatherKindCount = 6;
const char* ToString(WeatherKind k);
bool ParseWeatherKind(std::string_view s, WeatherKind& out);

// Each condition affects different channels; nothing is a blanket debuff.
struct WeatherProfile {
    double rain = 0.0;              // AIS rain noise floor + high-band attenuation
    double wind = 0.0;              // AIS wind noise floor (low band) + gusts
    double fog = 0.0;               // visual only
    double visualRangeMul = 1.0;    // ordinary vision
    double thermalRangeMul = 1.0;   // thermal sensors (fog barely matters, cold rain does)
    double commsRangeMul = 1.0;     // robot radio range (storm interference)
    double wetnessPerTick = 0.0;    // surfaces get wetter while it holds
    double electricalExposure = 0.0;  // health/s lost by exposed, already damaged infrastructure
    double fireSpreadMul = 1.0;
    double fireRainSuppression = 0.0;  // extra fuel burn for exposed fires (rain douses)
    Tick gustIntervalTicks = 0;
    Tick gustJitterTicks = 0;
    double gustLoudnessDb = 0.0;
    Tick thunderIntervalTicks = 0;
    double thunderLoudnessDb = 0.0;
    Tick holdMinTicks = 1800;
    Tick holdMaxTicks = 3600;
    std::array<double, kWeatherKindCount> nextWeights{};
};

struct GustAnchor {
    Vec3 position;
    bool metallic = false;  // loose sheet metal bangs (robots may investigate)
};

struct WeatherConfig {
    std::array<WeatherProfile, kWeatherKindCount> profiles{};
    WeatherKind initial = WeatherKind::Clear;
    bool autoAdvance = true;
    Tick transitionTicks = 600;
    Tick updateIntervalTicks = 30;
    double dryingPerTick = 0.0003;
    double puddleWetness = 0.6;
    double minVisualRangeFactor = 0.08;   // weather never makes anyone invisible
    double minThermalRangeFactor = 0.3;
    double acousticConfidenceRefCm = 5000.0;
    double acousticConfidenceLoss = 0.6;  // at full rain+wind and >= ref distance
    std::vector<GustAnchor> anchors;
    Vec3 boundsMin{-10000, -10000, 0};
    Vec3 boundsMax{10000, 10000, 0};
    std::uint64_t seed = 0x57454154ull;

    static WeatherConfig MakeDefault();
};

std::vector<std::string> ValidateWeather(const WeatherConfig& c);
// Sections: [weather] and [weather.<Kind>] and [gust.<name>].
void LoadWeather(const IniDocument& doc, WeatherConfig& c, std::vector<std::string>& errors);

struct WeatherSnapshot {
    WeatherKind from = WeatherKind::Clear;
    WeatherKind to = WeatherKind::Clear;
    double blend = 0.0;  // 0 = from, 1 = to
    double rain = 0.0, wind = 0.0, fog = 0.0;
    double visualRangeMul = 1.0, thermalRangeMul = 1.0, commsRangeMul = 1.0;
    double electricalExposure = 0.0, fireSpreadMul = 1.0, fireRainSuppression = 0.0, wetnessPerTick = 0.0;
    double wetness = 0.0;
    std::uint32_t version = 0;  // bumps whenever any value changes
};

class WeatherSystem {
public:
    WeatherSystem(const WeatherConfig& config, WorldEventBus* bus);

    void Step(Tick now);
    // Server / mission-director hook. Returns false for a duplicate request
    // (already at or heading to `target`) or invalid input.
    bool RequestWeather(WeatherKind target, Tick transitionTicks, Tick now);

    const WeatherSnapshot& Snapshot() const { return snap_; }
    WeatherKind Dominant() const { return snap_.blend >= 0.5 ? snap_.to : snap_.from; }
    bool InTransition() const { return inTransition_; }

    double VisualRangeFactor() const;
    double ThermalRangeFactor() const;
    // Distant acoustic reports are less trustworthy in rain and wind.
    double AcousticReportConfidenceScale(double distanceCm) const;
    SurfaceConditions Surface() const;

    std::uint64_t Updates() const { return updates_; }
    std::uint64_t StateHash() const;

private:
    double Roll();
    Tick RollHold(WeatherKind k);
    WeatherKind RollNext(WeatherKind k);
    void Recompute();
    void StartTransition(WeatherKind target, Tick duration, Tick now);
    void Reschedule(Tick now);

    WeatherConfig cfg_;
    WorldEventBus* bus_;
    WeatherSnapshot snap_;
    bool started_ = false;
    bool inTransition_ = false;
    Tick transitionStart_ = 0;
    Tick transitionLength_ = 1;
    Tick holdUntil_ = 0;
    Tick nextUpdate_ = 0;
    Tick nextGust_ = 0;
    Tick nextThunder_ = 0;
    Tick lastWetTick_ = 0;
    std::uint64_t rng_ = 0;
    std::uint64_t updates_ = 0;
};

}  // namespace llworld
