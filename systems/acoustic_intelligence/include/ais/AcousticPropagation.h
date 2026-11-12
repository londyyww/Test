// LAST LIGHT - Acoustic Intelligence System (AIS)
// Gameplay approximation of sound propagation. Not a physical simulator: a
// straight-line occlusion trace plus per-band coefficients designers can tune.
#pragma once

#include <cstddef>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/AcousticEvent.h"
#include "ais/AisTypes.h"

namespace ais {

struct OcclusionHit {
    MaterialId material = kDefaultMaterial;
    double thicknessCm = 0.0;
};

struct ZoneInfo {
    bool interior = false;
    std::uint32_t zoneId = 0;  // 0 = open exterior
};

struct WeatherState {
    double rain = 0.0;  // 0..1
    double wind = 0.0;  // 0..1
};

// Persistent masking sound (running generator, machinery hall, waterfall).
struct AmbientSource {
    Vec3 position;
    double loudnessDb = 60.0;
    BandArray spectrumDb{0.0, 0.0, 0.0};
};

// The only world queries AIS needs. Unreal implements this with line traces
// against an acoustic trace channel and volume lookups; the standalone build
// uses BoxAcousticEnvironment.
class IAcousticEnvironment {
public:
    virtual ~IAcousticEnvironment() = default;
    // Writes at most `maxHits` hits along from->to; returns the count written.
    virtual std::size_t TraceOcclusion(const Vec3& from, const Vec3& to, OcclusionHit* outHits,
                                       std::size_t maxHits) const = 0;
    virtual ZoneInfo ZoneAt(const Vec3& position) const = 0;
    virtual WeatherState Weather() const = 0;
    // Ambient masking contribution at a listener (excluding weather/base floor).
    virtual BandArray AmbientNoiseAt(const Vec3& position) const = 0;
};

struct Aabb {
    Vec3 min;
    Vec3 max;
    bool Contains(const Vec3& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z &&
               p.z <= max.z;
    }
};

// Slab test; returns entry/exit parameters along the segment in [0,1].
bool SegmentAabb(const Vec3& from, const Vec3& to, const Aabb& box, double& tEnter, double& tExit);

// Reference environment made of axis-aligned walls and zones. Used by tests,
// the example simulation and as a server-side fallback.
class BoxAcousticEnvironment final : public IAcousticEnvironment {
public:
    struct Wall {
        Aabb box;
        MaterialId material = kDefaultMaterial;
    };
    struct Zone {
        Aabb box;
        std::uint32_t zoneId = 1;
        bool interior = true;
    };

    void AddWall(const Aabb& box, MaterialId material) { walls_.push_back({box, material}); }
    void AddZone(const Aabb& box, std::uint32_t zoneId, bool interior) {
        zones_.push_back({box, zoneId, interior});
    }
    void AddAmbient(const AmbientSource& source) { ambient_.push_back(source); }
    void ClearAmbient() { ambient_.clear(); }
    void SetWeather(const WeatherState& weather) { weather_ = weather; }
    void SetSpreadingReferenceCm(double cm) { referenceCm_ = cm; }

    std::size_t TraceOcclusion(const Vec3& from, const Vec3& to, OcclusionHit* outHits,
                               std::size_t maxHits) const override;
    ZoneInfo ZoneAt(const Vec3& position) const override;
    WeatherState Weather() const override { return weather_; }
    BandArray AmbientNoiseAt(const Vec3& position) const override;

private:
    std::vector<Wall> walls_;
    std::vector<Zone> zones_;
    std::vector<AmbientSource> ambient_;
    WeatherState weather_;
    double referenceCm_ = 100.0;
};

struct PropagationResult {
    BandArray receivedDb{};
    BandArray noiseFloorDb{};
    BandArray snrDb{};  // received - floor (listener sensitivity not applied)
    double distanceCm = 0.0;
    std::size_t occluderCount = 0;
    double occlusionLossMidDb = 0.0;
    bool listenerInterior = false;
    bool sourceInterior = false;
    bool crossesInteriorBoundary = false;
};

class AcousticPropagationModel {
public:
    AcousticPropagationModel(const AisConfig& config, const IAcousticEnvironment& environment)
        : config_(config), env_(environment) {}

    // Source band levels at 1 m including surface modifiers.
    BandArray SourceBandLevels(const AcousticEvent& event) const;
    PropagationResult Evaluate(const AcousticEvent& event, const Vec3& listener) const;
    BandArray NoiseFloorAt(const Vec3& listener, bool interior) const;

    // Free-field radius beyond which no band reaches its threshold
    // (floor - sensitivity + detection, best case per band). Broadphase only.
    double MaxAudibleRadiusCm(const AcousticEvent& event, const BandArray& thresholdsDb) const;

    // Localisation error radius for a perceived event (before robot skill).
    double LocalizationErrorCm(const PropagationResult& result, double effectiveSnrDb,
                               Tick durationTicks) const;

private:
    const AisConfig& config_;
    const IAcousticEnvironment& env_;
};

}  // namespace ais
