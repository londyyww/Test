// LAST LIGHT - Environmental & World Intelligence (WI)
// The shared event and environmental-state interface between weather,
// hazards, squads and the existing systems (AIS, power grid).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "llworld/WorldTypes.h"

namespace llworld {

enum class WorldSystem : std::uint8_t { Weather, Hazards, Squad, Facility, Acoustic, Game };

enum class WorldEventType : std::uint8_t {
    WeatherChanged,      // magnitude = new rain, aux = WeatherKind of target
    WindGust,            // magnitude = loudness dB
    Thunder,             // magnitude = loudness dB
    HazardActivated,     // subject = hazard index
    HazardDeactivated,
    HazardEscalated,     // e.g. structure stressed, gas concentration critical
    FireSpread,          // subject = new fire, aux = source fire
    GasIgnition,         // magnitude = loudness dB, radius = blast radius
    StructuralCollapse,  // magnitude = loudness dB
    RouteBlocked,        // aux = route id
    RouteCleared,
    PlayerExposure,      // subject = player, magnitude = damage this sample
    AlarmRaised,         // from the power grid
    PowerLost,           // generator stopped/tripped/destroyed
    PowerRestored,
    EmergencyLighting,
    CommsLost,           // subject = robot
    CommsRestored,
    AssignmentChanged,   // subject = robot, aux = IntentKind
    Gunfire,             // reported by the game (ignition source / structural stress)
    Explosion,           // reported by the game
};
inline constexpr std::size_t kWorldEventTypeCount = 21;
const char* ToString(WorldEventType t);

struct WorldEvent {
    std::uint64_t id = 0;  // assigned by the bus
    Tick tick = 0;
    WorldEventType type = WorldEventType::WeatherChanged;
    WorldSystem source = WorldSystem::Game;
    Vec3 position;
    double magnitude = 0.0;
    double radiusCm = 0.0;
    std::uint32_t subject = kInvalidIndex;
    std::uint32_t aux = 0;
};

// Bounded, deterministic event bus. Systems publish during their step; events
// become readable through Previous() on the next tick, so processing order
// never depends on who stepped first within a tick.
class WorldEventBus {
public:
    explicit WorldEventBus(std::size_t maxEventsPerTick = 256) : cap_(maxEventsPerTick) {}

    void BeginTick(Tick now);
    // Returns 0 when dropped (cap reached or invalid data), else the event id.
    std::uint64_t Publish(WorldEvent e);
    const std::vector<WorldEvent>& Previous() const { return previous_; }
    const std::vector<WorldEvent>& Current() const { return current_; }

    std::uint64_t Published() const { return published_; }
    std::uint64_t Dropped() const { return dropped_; }
    std::size_t Capacity() const { return cap_; }

private:
    std::size_t cap_;
    Tick tick_ = 0;
    std::uint64_t nextId_ = 1;
    std::vector<WorldEvent> current_;
    std::vector<WorldEvent> previous_;
    std::uint64_t published_ = 0;
    std::uint64_t dropped_ = 0;
};

// --- Environmental state queries (read-only, used by AI, players and UI) ---

struct VisibilitySample {
    double visualRangeFactor = 1.0;   // multiplies a sensor's base visual range
    double thermalRangeFactor = 1.0;  // separate from visual on purpose
    double illumination = 0.0;        // artificial light level (power grid)
    double fog = 0.0;
    double smoke = 0.0;
};

struct AcousticConditions {
    double rain = 0.0;  // fed into AIS noise floors
    double wind = 0.0;
    double wetness = 0.0;
};

struct SurfaceConditions {
    double wetness = 0.0;  // 0 dry .. 1 standing water
    bool puddles = false;  // footsteps splash (louder) but rain may mask them
};

class IEnvironmentQuery {
public:
    virtual ~IEnvironmentQuery() = default;
    virtual VisibilitySample VisibilityAt(const Vec3& position) const = 0;
    virtual AcousticConditions Acoustics() const = 0;
    virtual SurfaceConditions SurfaceAt(const Vec3& position) const = 0;
    virtual double HazardDangerAt(const Vec3& position) const = 0;  // damage per second
    virtual bool IsRouteBlocked(std::uint32_t routeId) const = 0;
};

}  // namespace llworld
