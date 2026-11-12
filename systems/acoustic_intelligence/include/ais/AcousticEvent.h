// LAST LIGHT - Acoustic Intelligence System (AIS)
// Acoustic stimulus categories and the authoritative event record.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ais/AisTypes.h"

namespace ais {

enum class StimulusCategory : std::uint8_t {
    Gunfire = 0,
    Footstep,
    Sprint,
    JumpLanding,
    Door,
    BreakObject,
    MetalImpact,
    Explosion,
    Machinery,
    Generator,
    Environmental,
    Decoy,
};
inline constexpr std::size_t kCategoryCount = 12;
template <class T>
using CategoryArray = std::array<T, kCategoryCount>;
inline constexpr std::size_t Index(StimulusCategory c) { return static_cast<std::size_t>(c); }

// Surface the sound was produced on (footsteps, landings, impacts).
enum class SurfaceType : std::uint8_t {
    None = 0,
    Concrete,
    Metal,
    Wood,
    Gravel,
    Grass,
    Water,
    Glass,
    Debris,
};
inline constexpr std::size_t kSurfaceCount = 9;
inline constexpr std::size_t Index(SurfaceType s) { return static_cast<std::size_t>(s); }

const char* ToString(StimulusCategory c);
const char* ToString(SurfaceType s);
bool ParseCategory(std::string_view name, StimulusCategory& out);
bool ParseSurface(std::string_view name, SurfaceType& out);

// One server-authoritative acoustic event. Created only by server gameplay
// code (or the decoy system) and never by clients.
struct AcousticEvent {
    EventId id = 0;  // assigned by AcousticEventBus on acceptance
    // Ground truth. Robots never see `category` or `artificiality` directly.
    StimulusCategory category = StimulusCategory::Environmental;
    // What the sound resembles. Equals `category` except for decoys, which
    // mimic other signatures.
    StimulusCategory apparentCategory = StimulusCategory::Environmental;
    Vec3 position;
    Tick emitTick = 0;
    double loudnessDb = 0.0;      // gameplay source level at 1 m
    BandArray spectrumDb{0.0, 0.0, 0.0};  // per-band offset relative to loudness
    SurfaceType surface = SurfaceType::None;
    Tick durationTicks = 1;
    EntityId sourceId = kNoEntity;  // optional identity; 0 = anonymous
    double artificiality = 0.0;     // 0 natural .. 1 obviously synthetic
};

}  // namespace ais
