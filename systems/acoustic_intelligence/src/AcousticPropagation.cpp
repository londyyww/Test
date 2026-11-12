// LAST LIGHT - Acoustic Intelligence System (AIS)
#include "ais/AcousticPropagation.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ais {

bool SegmentAabb(const Vec3& from, const Vec3& to, const Aabb& box, double& tEnter, double& tExit) {
    const std::array<double, 3> o{from.x, from.y, from.z};
    const std::array<double, 3> d{to.x - from.x, to.y - from.y, to.z - from.z};
    const std::array<double, 3> lo{box.min.x, box.min.y, box.min.z};
    const std::array<double, 3> hi{box.max.x, box.max.y, box.max.z};
    double t0 = 0.0;
    double t1 = 1.0;
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        const double inv = 1.0 / d[i];
        double ta = (lo[i] - o[i]) * inv;
        double tb = (hi[i] - o[i]) * inv;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    tEnter = t0;
    tExit = t1;
    return true;
}

std::size_t BoxAcousticEnvironment::TraceOcclusion(const Vec3& from, const Vec3& to,
                                                   OcclusionHit* outHits, std::size_t maxHits) const {
    const double length = Distance(from, to);
    std::size_t count = 0;
    for (const Wall& w : walls_) {
        if (count >= maxHits) break;
        double t0 = 0.0;
        double t1 = 0.0;
        if (!SegmentAabb(from, to, w.box, t0, t1)) continue;
        const double thickness = (t1 - t0) * length;
        if (thickness <= 0.0) continue;  // grazing contact
        outHits[count].material = w.material;
        outHits[count].thicknessCm = thickness;
        ++count;
    }
    return count;
}

ZoneInfo BoxAcousticEnvironment::ZoneAt(const Vec3& position) const {
    for (const Zone& z : zones_) {
        if (z.box.Contains(position)) return ZoneInfo{z.interior, z.zoneId};
    }
    return ZoneInfo{};
}

BandArray BoxAcousticEnvironment::AmbientNoiseAt(const Vec3& position) const {
    BandArray total{-200.0, -200.0, -200.0};
    for (const AmbientSource& a : ambient_) {
        const double d = std::max(Distance(a.position, position), referenceCm_);
        const double spread = 20.0 * std::log10(d / referenceCm_);
        for (std::size_t b = 0; b < kBandCount; ++b) {
            total[b] = PowerSumDb(total[b], a.loudnessDb + a.spectrumDb[b] - spread);
        }
    }
    return total;
}

BandArray AcousticPropagationModel::SourceBandLevels(const AcousticEvent& event) const {
    const std::size_t s = Index(event.surface);
    const SurfaceDescriptor& surf = s < kSurfaceCount ? config_.surfaces[s] : config_.surfaces[0];
    BandArray out{};
    for (std::size_t b = 0; b < kBandCount; ++b) {
        out[b] = event.loudnessDb + event.spectrumDb[b] + surf.loudnessOffsetDb + surf.spectrumShiftDb[b];
    }
    return out;
}

BandArray AcousticPropagationModel::NoiseFloorAt(const Vec3& listener, bool interior) const {
    const PropagationConfig& p = config_.propagation;
    const WeatherState w = env_.Weather();
    const double rain = Clamp01(w.rain);
    const double wind = Clamp01(w.wind);
    const BandArray ambient = env_.AmbientNoiseAt(listener);
    BandArray floor = interior ? p.interiorNoiseFloorDb : p.exteriorNoiseFloorDb;
    const double weatherCut = interior ? p.interiorWeatherReductionDb : 0.0;
    for (std::size_t b = 0; b < kBandCount; ++b) {
        // Weather intensity scales noise energy, so 0 contributes nothing.
        if (rain > 0.0) floor[b] = PowerSumDb(floor[b], p.rainNoiseDb[b] + 10.0 * std::log10(rain) - weatherCut);
        if (wind > 0.0) floor[b] = PowerSumDb(floor[b], p.windNoiseDb[b] + 10.0 * std::log10(wind) - weatherCut);
        floor[b] = PowerSumDb(floor[b], ambient[b]);
    }
    return floor;
}

PropagationResult AcousticPropagationModel::Evaluate(const AcousticEvent& event, const Vec3& listener) const {
    const PropagationConfig& p = config_.propagation;
    PropagationResult r;
    r.distanceCm = Distance(event.position, listener);

    const ZoneInfo srcZone = env_.ZoneAt(event.position);
    const ZoneInfo lisZone = env_.ZoneAt(listener);
    r.sourceInterior = srcZone.interior;
    r.listenerInterior = lisZone.interior;
    r.crossesInteriorBoundary = srcZone.zoneId != lisZone.zoneId && (srcZone.interior || lisZone.interior);

    const double spreadFactor = (srcZone.interior && lisZone.interior && srcZone.zoneId == lisZone.zoneId)
                                    ? p.interiorSpreadingFactor
                                    : p.exteriorSpreadingFactor;
    const double d = std::max(r.distanceCm, p.referenceDistanceCm);
    const double spreading = 20.0 * spreadFactor * std::log10(d / p.referenceDistanceCm);
    const double hundredMetres = r.distanceCm / 10000.0;

    std::array<OcclusionHit, 16> hits{};
    const std::size_t maxHits = std::min<std::size_t>(p.maxOcclusionHits, hits.size());
    r.occluderCount = env_.TraceOcclusion(event.position, listener, hits.data(), maxHits);

    BandArray occlusion{0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < r.occluderCount; ++i) {
        const std::size_t m = hits[i].material < config_.materials.size() ? hits[i].material : 0;
        const MaterialDescriptor& mat = config_.materials[m];
        const double metres = hits[i].thicknessCm / 100.0;
        for (std::size_t b = 0; b < kBandCount; ++b) {
            occlusion[b] += mat.lossDb[b] + mat.lossDbPerMeter[b] * metres;
        }
    }

    const WeatherState w = env_.Weather();
    const bool exposed = !srcZone.interior || !lisZone.interior;
    const BandArray source = SourceBandLevels(event);
    for (std::size_t b = 0; b < kBandCount; ++b) {
        occlusion[b] = std::min(occlusion[b], p.maxOcclusionLossDb);
        double loss = spreading + occlusion[b] + p.airAbsorptionDbPer100m[b] * hundredMetres;
        if (exposed) {
            loss += (Clamp01(w.rain) * p.rainAttenuationDbPer100m[b] +
                     Clamp01(w.wind) * p.windAttenuationDbPer100m[b]) * hundredMetres;
        }
        if (r.crossesInteriorBoundary) loss += p.interiorExteriorTransitionDb;
        r.receivedDb[b] = source[b] - loss;
    }
    r.occlusionLossMidDb = occlusion[static_cast<std::size_t>(Band::Mid)];
    r.noiseFloorDb = NoiseFloorAt(listener, lisZone.interior);
    for (std::size_t b = 0; b < kBandCount; ++b) r.snrDb[b] = r.receivedDb[b] - r.noiseFloorDb[b];
    return r;
}

double AcousticPropagationModel::MaxAudibleRadiusCm(const AcousticEvent& event, const BandArray& thresholdsDb) const {
    const PropagationConfig& p = config_.propagation;
    const BandArray source = SourceBandLevels(event);
    const double minSpread = std::min(p.exteriorSpreadingFactor, p.interiorSpreadingFactor);
    double headroomDb = -1e9;
    for (std::size_t b = 0; b < kBandCount; ++b) headroomDb = std::max(headroomDb, source[b] - thresholdsDb[b]);
    if (headroomDb <= 0.0) return p.referenceDistanceCm;
    // Conservative: ignores air absorption, so it never under-estimates.
    const double exponent = headroomDb / (20.0 * minSpread);
    if (exponent > 10.0) return p.maxAudibleRadiusCm;
    return std::min(p.maxAudibleRadiusCm, p.referenceDistanceCm * std::pow(10.0, exponent));
}

double AcousticPropagationModel::LocalizationErrorCm(const PropagationResult& r, double effectiveSnrDb,
                                                     Tick durationTicks) const {
    const PropagationConfig& p = config_.propagation;
    double err = std::max(p.localizationMinErrorCm, r.distanceCm * p.localizationBaseErrorFraction);
    err *= 1.0 + p.localizationPerOccluderScale * static_cast<double>(r.occluderCount);
    if (r.listenerInterior || r.sourceInterior) err *= p.localizationInteriorScale;
    // Faint sounds are hard to place.
    const double snrShortfall = std::max(0.0, p.localizationSnrReferenceDb - effectiveSnrDb);
    err *= 1.0 + snrShortfall / p.localizationSnrReferenceDb;
    // Sustained sounds give the listener time to triangulate.
    const double seconds = static_cast<double>(durationTicks) / std::max(1.0, config_.world.ticksPerSecond);
    err *= std::max(0.5, 1.0 / std::sqrt(1.0 + seconds));
    return err;
}

}  // namespace ais
