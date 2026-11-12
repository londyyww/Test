// LAST LIGHT - Environmental Interaction & Power Grid (ENV)
// Data-driven facility definition. Level designers describe infrastructure in
// INI files (see data/); nothing here references a specific map.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "llenv/EnvTypes.h"

namespace llenv {

enum class NodeType : std::uint8_t {
    // Power sources
    Generator = 0,
    BackupGenerator,
    Battery,
    // Distribution
    Junction,
    Breaker,
    // Consumers / interactive objects
    Door,
    SecurityGate,
    Light,
    EmergencyLight,
    Machinery,
    Terminal,
    Alarm,
    CommArray,
    ExtractionPoint,
    SecuritySensor,
};
inline constexpr std::size_t kNodeTypeCount = 15;
const char* ToString(NodeType t);
bool ParseNodeType(std::string_view s, NodeType& out);

inline constexpr bool IsSource(NodeType t) {
    return t == NodeType::Generator || t == NodeType::BackupGenerator || t == NodeType::Battery;
}
inline constexpr bool IsDistribution(NodeType t) { return t == NodeType::Junction || t == NodeType::Breaker; }
inline constexpr bool IsConsumer(NodeType t) { return !IsSource(t) && !IsDistribution(t); }
inline constexpr bool IsDoorLike(NodeType t) { return t == NodeType::Door || t == NodeType::SecurityGate; }

enum class LockFailMode : std::uint8_t {
    FailSecure,  // electronic lock stays engaged without power
    FailSafe,    // lock releases without power
};

// What happens to this node's state between missions.
enum class PersistencePolicy : std::uint8_t {
    Mission,            // reset from the definition at every mission start
    World,              // durable fields (damage, repairs, fuses, fuel) persist
    WorldOnExtraction,  // durable fields persist only if the squad extracted
};

struct NodeDef {
    std::string id;
    NodeType type = NodeType::Junction;
    std::string network;
    std::vector<std::string> fedBy;  // upstream nodes (power flows fedBy -> this)
    Vec3 position;

    // Power
    double capacityKw = 0.0;  // source output, or breaker rated load
    double demandKw = 0.0;    // consumer draw while it wants power
    int priority = 50;        // 0..100, load shedding keeps higher first
    double health = 1.0;      // 0..1 initial; 0 = destroyed
    double damagedThreshold = 0.5;
    double damagedOutputFactor = 0.5;
    bool repairable = true;
    Tick repairTicks = 150;
    std::string repairItem;  // empty = no item needed

    // Generators
    bool startsRunning = false;
    Tick startupTicks = 90;
    double fuelTicks = -1.0;  // < 0 = unlimited
    std::string fuelItem;
    double refuelAmountTicks = 9000.0;
    double overloadTolerance = 1.15;  // demand/supply ratio that trips it
    Tick overloadTripTicks = 150;
    bool autoStart = false;           // backup generators: start on primary loss
    Tick autoStartDelayTicks = 60;
    bool autoStopOnRestore = true;
    double startFailChanceDamaged = 0.35;
    double noiseDb = 0.0;             // continuous acoustic signature
    Tick noisePulseTicks = 120;       // periodic identifiable signature pulses

    // Battery
    double batteryCapacityKwTicks = 0.0;
    double batteryInitialFraction = 1.0;
    double batteryChargeKw = 0.0;

    // Breakers
    bool startsClosed = true;
    Tick tripDelayTicks = 30;
    bool fuseInstalled = true;
    std::string fuseItem;

    // Doors / gates
    bool startsOpen = false;
    bool startsLocked = false;
    LockFailMode failMode = LockFailMode::FailSecure;
    Tick motorTicks = 45;
    double manualWork = 300.0;           // crank work units (1 per player-tick)
    std::uint32_t manualParticipants = 1;
    std::string keyItem;                 // needed to operate when powered & locked
    std::string forceItem;               // tool that forces a locked, unpowered lock
    std::string alarm;                   // alarm triggered when forced

    // Lights
    bool startsOn = true;
    double lightRadiusCm = 1200.0;
    double lightIntensity = 1.0;
    Tick emergencyBatteryTicks = 5400;
    double emergencyIntensity = 0.25;

    // Terminals
    std::vector<std::string> controls;  // nodes this terminal can command
    Tick hackTicks = 0;                 // 0 = instant use

    // Alarms
    Tick alarmIntervalTicks = 45;
    Tick alarmDurationTicks = 900;  // auto-silence after this; 0 = until silenced
    bool triggerOnPowerRestore = false;

    // Security sensors
    double sensorRadiusCm = 800.0;
    bool rearmOnPowerRestore = true;

    // Extraction / comms
    Tick callTicks = 300;
    std::string objectiveSignal;

    // Interaction rules
    double interactRangeCm = 250.0;
    std::uint32_t requiredPermissions = 0;  // bitmask the player must hold
    bool blocksNavWhenRunning = false;      // machinery
    PersistencePolicy persistence = PersistencePolicy::Mission;
};

struct FacilityDef {
    std::string facilityId = "facility";
    double ticksPerSecond = 30.0;
    std::uint64_t seed = 0x4C4C454E56ull;
    std::vector<NodeDef> nodes;

    int Find(std::string_view id) const;
};

std::vector<std::string> ValidateFacility(const FacilityDef& def);

struct LoadResult {
    bool ok = true;
    std::vector<std::string> errors;
};
// Parses a facility INI. Unknown sections/keys are errors. Validation runs
// at the end; `ok` is false if anything failed.
LoadResult LoadFacilityIni(std::string_view text, FacilityDef& out);
LoadResult LoadFacilityFile(const std::string& path, FacilityDef& out);

}  // namespace llenv
