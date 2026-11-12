// LAST LIGHT - Environmental Interaction & Power Grid (ENV)
// Outbound events and inbound service ports. These are the only seams to
// other systems (Acoustic Intelligence, tactical AI, missions, inventory,
// replication). Implementations for the real project systems are adapters.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "llenv/EnvTypes.h"

namespace llenv {

// --- Acoustic output ------------------------------------------------------
enum class SoundKind : std::uint8_t {
    GeneratorStart,     // engine cranking up
    GeneratorRunning,   // periodic identifiable signature
    GeneratorFailure,   // failed start / overload trip: a loud bang
    MachineryRunning,
    DoorMotor,
    DoorForced,         // pried open: metallic shriek
    BreakerTrip,        // electrical bang
    AlarmPulse,
    ExtractionBeacon,
    CommBurst,
};
const char* ToString(SoundKind k);

struct EnvSoundEvent {
    SoundKind kind = SoundKind::GeneratorRunning;
    NodeIndex node = kInvalidNode;
    Vec3 position;
    double loudnessDb = 0.0;  // gameplay dB at 1 m
    Tick durationTicks = 1;
    Tick tick = 0;
};

// Implemented by the Acoustic Intelligence bridge (bridge/AisAcousticBridge).
class IAcousticSink {
public:
    virtual ~IAcousticSink() = default;
    virtual void OnSound(const EnvSoundEvent& e) = 0;
    // Continuous masking/attracting sources (generators, machinery).
    virtual void OnContinuousSound(NodeIndex node, const Vec3& position, double loudnessDb, bool active) = 0;
};

// --- Gameplay events (tactical AI, missions, navigation, UI, telemetry) ----
enum class FacilityEventType : std::uint8_t {
    PowerChanged,        // value = 1 powered / 0 unpowered
    GeneratorState,      // value = GeneratorState
    BreakerState,        // value = 1 closed+conducting / 0 open or tripped
    BreakerTripped,
    NetworkOverloaded,   // value = demand/supply ratio * 1000
    DoorState,           // value = DoorState
    NavBlockChanged,     // value = 1 blocked / 0 passable
    LightChanged,        // value = illumination * 1000
    AlarmTriggered,
    AlarmSilenced,
    AlertLevelChanged,   // value = number of active alarms
    SecurityArmed,       // value = 1 armed / 0 disarmed
    Damaged,
    Destroyed,
    Repaired,
    ObjectiveSignal,     // text = signal name
    InteractionCompleted,
};
const char* ToString(FacilityEventType t);

struct FacilityEvent {
    FacilityEventType type = FacilityEventType::PowerChanged;
    NodeIndex node = kInvalidNode;
    Tick tick = 0;
    std::int64_t value = 0;
    std::string text;
};

class IFacilityObserver {
public:
    virtual ~IFacilityObserver() = default;
    virtual void OnFacilityEvent(const FacilityEvent& e) = 0;
};

// --- Player inventory (owned by the profile/inventory system) -------------
// The facility only asks and consumes; it never writes the profile itself.
class IPlayerInventory {
public:
    virtual ~IPlayerInventory() = default;
    virtual bool HasItem(PlayerId player, std::string_view item) const = 0;
    virtual bool ConsumeItem(PlayerId player, std::string_view item) = 0;
};

// --- Interaction requests ---------------------------------------------------
enum class InteractionAction : std::uint8_t {
    Activate,     // start generator, close breaker, open door, switch on, use terminal command
    Deactivate,   // stop generator, open breaker, close door, switch off
    ResetBreaker, // clear a trip
    Crank,        // join a manual multi-player channel (unpowered doors/gates)
    Force,        // pry a locked, unpowered lock with a tool (noisy)
    Repair,       // channel; may need an item
    Refuel,       // consumes fuel item
    InstallFuse,  // consumes fuse item
    Hack,         // channel on terminals with hackTicks > 0
    Command,      // terminal: operate controlled node `targetIndex`
    TriggerAlarm, // alarm panel / terminal
    SilenceAlarm,
    CallExtraction,  // channel
    CancelChannel,
};
const char* ToString(InteractionAction a);

struct InteractionRequest {
    PlayerId player = kNoPlayer;
    std::uint32_t sequence = 0;  // per-player, client-generated, for dedup
    NodeIndex node = kInvalidNode;
    InteractionAction action = InteractionAction::Activate;
    std::uint32_t expectedVersion = 0;  // 0 = don't care; else optimistic concurrency
    NodeIndex commandTarget = kInvalidNode;  // for Command
    InteractionAction commandAction = InteractionAction::Activate;
};

enum class InteractionResult : std::uint8_t {
    Applied = 0,
    ChannelStarted,
    ChannelJoined,
    RejectedNotAuthority,
    RejectedUnknownPlayer,
    RejectedDisconnected,
    RejectedRateLimited,
    RejectedDuplicate,
    RejectedUnknownNode,
    RejectedOutOfRange,
    RejectedPermission,
    RejectedStaleVersion,
    RejectedUnsupported,
    RejectedNoPower,
    RejectedLocked,
    RejectedMissingItem,
    RejectedDestroyed,
    RejectedBusy,
    RejectedInvalidState,
};
inline constexpr std::size_t kInteractionResultCount = 19;
const char* ToString(InteractionResult r);
inline constexpr bool Succeeded(InteractionResult r) {
    return r == InteractionResult::Applied || r == InteractionResult::ChannelStarted ||
           r == InteractionResult::ChannelJoined;
}

}  // namespace llenv
