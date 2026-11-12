// LAST LIGHT - Acoustic Intelligence System (AIS)
// All tunable gameplay coefficients, plus an INI loader so designers can
// override them without recompiling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ais/AcousticEvent.h"
#include "ais/AisTypes.h"
#include "ais/RobotSensoryProfile.h"

namespace ais {

using MaterialId = std::uint16_t;
inline constexpr MaterialId kDefaultMaterial = 0;

struct CategoryDescriptor {
    double loudnessDb = 60.0;
    BandArray spectrumDb{0.0, 0.0, 0.0};
    Tick durationTicks = 1;
    double threat = 0.5;           // 0..1, scales suspicion gain
    double queuePriorityDb = 0.0;  // bonus when the bus must choose what to keep
    bool hostile = false;          // may justify acoustic-only combat
    bool regularityIsSuspicious = false;  // perfectly periodic repeats look artificial
    double defaultArtificiality = 0.0;
};

struct SurfaceDescriptor {
    double loudnessOffsetDb = 0.0;
    BandArray spectrumShiftDb{0.0, 0.0, 0.0};
};

struct MaterialDescriptor {
    std::string name = "Default";
    BandArray lossDb{10.0, 15.0, 20.0};          // fixed loss per crossing
    BandArray lossDbPerMeter{10.0, 15.0, 20.0};  // extra loss per metre of thickness
};

struct PropagationConfig {
    double referenceDistanceCm = 100.0;
    double exteriorSpreadingFactor = 1.0;  // multiplies 20*log10(d/ref)
    double interiorSpreadingFactor = 0.85;  // rooms contain energy
    BandArray airAbsorptionDbPer100m{1.0, 3.0, 6.0};
    double interiorExteriorTransitionDb = 6.0;
    std::size_t maxOcclusionHits = 8;
    double maxOcclusionLossDb = 70.0;

    BandArray exteriorNoiseFloorDb{25.0, 22.0, 18.0};
    BandArray interiorNoiseFloorDb{18.0, 16.0, 14.0};
    BandArray rainNoiseDb{28.0, 40.0, 46.0};   // at rain = 1
    BandArray windNoiseDb{44.0, 34.0, 24.0};   // at wind = 1
    BandArray rainAttenuationDbPer100m{0.0, 0.5, 1.5};
    BandArray windAttenuationDbPer100m{0.5, 1.0, 2.0};
    double interiorWeatherReductionDb = 15.0;

    double localizationBaseErrorFraction = 0.08;
    double localizationMinErrorCm = 50.0;
    double localizationPerOccluderScale = 0.6;
    double localizationInteriorScale = 1.4;
    double localizationSnrReferenceDb = 20.0;

    double maxAudibleRadiusCm = 40000.0;  // broadphase cap
};

struct BusConfig {
    std::size_t queueCapacity = 256;
    std::size_t maxDrainPerTick = 64;
    std::size_t maxAcceptedPerTick = 128;
    Tick dedupWindowTicks = 2;
    double dedupRadiusCm = 75.0;
    std::size_t dedupHistory = 512;
    double sourceBurst = 12.0;
    double sourceRefillPerTick = 0.5;
    std::size_t maxTrackedSources = 1024;
    Tick maxEventAgeTicks = 30;
    Tick maxFutureTicks = 0;
    double minLoudnessDb = 0.0;
    double maxLoudnessDb = 180.0;
    Tick maxDurationTicks = 3000;
    Vec3 worldMin{-1.0e7, -1.0e7, -1.0e6};
    Vec3 worldMax{1.0e7, 1.0e7, 1.0e6};
};

struct WorldConfig {
    double ticksPerSecond = 30.0;
    double gridCellCm = 2500.0;
    std::size_t maxPerceptionChecksPerTick = 1024;
    std::uint64_t worldSeed = 0x4C4153544C494748ull;
    // Cosmetic replication relevancy (client audio only, never AI input).
    double cosmeticRadiusScale = 1.0;
    std::size_t maxCosmeticCuesPerClientPerTick = 16;
    // Local signal relay defaults (the LLSignalNetwork adapter may ignore).
    Tick reportLatencyTicks = 6;
    double reportRangeCm = 15000.0;
    std::size_t maxReportsInFlight = 256;
    Tick reportMinIntervalTicks = 30;
};

enum class DecoyPattern : std::uint8_t {
    TimedBurst,          // single loud event after a fuse
    RepeatingMetallic,   // perfectly regular impacts (easy to see through)
    RecordedMechanical,  // jittered playback of a real recording
    EmergencyRadio,      // periodic transmission bursts
};

struct DecoyDefinition {
    std::string name = "Decoy";
    DecoyPattern pattern = DecoyPattern::TimedBurst;
    StimulusCategory apparentCategory = StimulusCategory::MetalImpact;
    double loudnessDb = 70.0;
    BandArray spectrumDb{0.0, 0.0, 0.0};
    double artificiality = 0.5;
    Tick armDelayTicks = 0;
    Tick intervalTicks = 30;
    Tick jitterTicks = 0;
    Tick lifetimeTicks = 900;
    std::uint32_t maxEmissions = 0;  // 0 = until lifetime ends
    Tick durationTicks = 1;
};

struct AisConfig {
    CategoryArray<CategoryDescriptor> categories{};
    std::array<SurfaceDescriptor, kSurfaceCount> surfaces{};
    std::vector<MaterialDescriptor> materials;  // index = MaterialId
    std::vector<RobotSensoryProfile> robotProfiles;
    std::vector<DecoyDefinition> decoys;
    PropagationConfig propagation;
    BusConfig bus;
    WorldConfig world;

    static AisConfig MakeDefault();

    int FindMaterial(std::string_view name) const;
    const RobotSensoryProfile* FindProfile(std::string_view name) const;
    const DecoyDefinition* FindDecoy(std::string_view name) const;
};

// Returns human-readable problems; empty means the config is usable.
std::vector<std::string> ValidateConfig(const AisConfig& config);

struct ConfigLoadResult {
    bool ok = true;
    std::vector<std::string> errors;
};

// Overlays INI text onto `config` (start from MakeDefault()). Unknown sections
// or keys are errors so typos never silently fall back to defaults.
ConfigLoadResult LoadConfigFromIni(std::string_view text, AisConfig& config);
ConfigLoadResult LoadConfigFromFile(const std::string& path, AisConfig& config);

// Fills an event from category defaults. Surface modifiers are applied later
// by the propagation model, not baked in here.
AcousticEvent MakeEvent(const AisConfig& config, StimulusCategory category, const Vec3& position,
                        Tick tick, SurfaceType surface = SurfaceType::None,
                        EntityId sourceId = kNoEntity);

}  // namespace ais
