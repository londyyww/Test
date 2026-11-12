// LAST LIGHT - Environmental Interaction & Power Grid (ENV)
// Server-authoritative facility simulation: power grid solve (event driven),
// interactive objects, cooperative channels, replication deltas and
// mission/world persistence.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "llenv/EnvTypes.h"
#include "llenv/FacilityDefinition.h"
#include "llenv/Ports.h"

namespace llenv {

enum class AuthorityRole : std::uint8_t { Server, Client };

enum class GeneratorState : std::uint8_t { Off = 0, Starting, Running, Tripped, Destroyed };
enum class DoorState : std::uint8_t { Closed = 0, Opening, Open, Closing };
const char* ToString(GeneratorState s);
const char* ToString(DoorState s);

enum class TimerKind : std::uint8_t {
    GeneratorStarted = 0,
    FuelEmpty,
    OverloadTrip,
    BackupAutoStart,
    BatteryEmpty,
    BatteryFull,
    BreakerTrip,
    DoorMotionDone,
    AlarmPulse,
    AlarmEnd,
    EmergencyEnd,
    NoisePulse,
};
inline constexpr std::size_t kTimerKindCount = 12;

struct NodeState {
    // Shared
    double health = 1.0;
    bool destroyed = false;
    bool energized = false;  // connected to a live source
    bool powered = false;    // energized AND allocated its demand
    bool everPowered = false;
    bool switchedOn = false;  // operator intent (lights, machinery, comms, battery)
    std::uint32_t version = 1;
    bool replicationDirty = true;
    double progress = 0.0;  // manual/repair/hack/call work accumulated

    // Generators
    GeneratorState gen = GeneratorState::Off;
    bool autoStarted = false;
    double fuel = -1.0;
    Tick fuelStamp = 0;
    std::uint32_t startAttempts = 0;

    // Battery
    double charge = 0.0;
    double batteryRateKw = 0.0;  // >0 discharging, <0 charging
    Tick chargeStamp = 0;

    // Breaker
    bool closed = true;
    bool tripped = false;
    bool fuseInstalled = true;
    double loadKw = 0.0;

    // Doors / gates
    DoorState door = DoorState::Closed;
    bool lockCommanded = false;  // the lock wants to be engaged
    bool lockBroken = false;     // forced; permanent for the mission

    // Lights / sensors / terminals / alarms / extraction
    bool lit = false;
    bool onEmergency = false;
    bool armCommanded = false;
    bool hacked = false;
    bool alarmActive = false;
    bool completed = false;
    bool soundActive = false;  // continuous sound currently reported to the sink

    std::array<std::uint32_t, kTimerKindCount> timerGen{};
    std::array<bool, kTimerKindCount> timerPending{};
};

struct TimerEntry {
    Tick tick = 0;
    std::uint64_t seq = 0;
    NodeIndex node = kInvalidNode;
    TimerKind kind = TimerKind::GeneratorStarted;
    std::uint32_t gen = 0;
};

enum class ChannelKind : std::uint8_t { Crank, Repair, Hack, CallExtraction };

struct Channel {
    NodeIndex node = kInvalidNode;
    ChannelKind kind = ChannelKind::Crank;
    std::vector<PlayerId> participants;  // sorted
    PlayerId starter = kNoPlayer;
};

struct PlayerSession {
    PlayerId id = kNoPlayer;
    bool connected = false;
    Vec3 position;
    std::uint32_t permissions = 0;
    double tokens = 0.0;
    Tick tokenStamp = 0;
    std::uint32_t highestSeq = 0;
    std::array<std::uint32_t, 64> recent{};  // ring of recently seen sequence numbers
    std::size_t recentCount = 0;
    std::size_t recentHead = 0;
};

struct NetworkStatus {
    std::string name;
    double supplyKw = 0.0;
    double demandKw = 0.0;
    double servedKw = 0.0;
    std::size_t islands = 0;
    std::size_t shedConsumers = 0;
    bool overloaded = false;
};

struct FacilityConfig {
    std::size_t maxPlayers = 4;
    double requestBurst = 6.0;
    double requestRefillPerTick = 0.25;  // 7.5 requests/s sustained at 30 Hz
    double rangeTolerance = 1.15;        // latency slack on interaction range
    std::size_t maxEventsPerTick = 256;
    std::size_t maxSoundsPerTick = 32;
    std::size_t maxSolveIterations = 4;
};

struct FacilityStats {
    std::uint64_t powerSolves = 0;
    std::uint64_t timersFired = 0;
    std::uint64_t staleTimersSkipped = 0;
    std::uint64_t eventsEmitted = 0;
    std::uint64_t eventsDropped = 0;
    std::uint64_t soundsEmitted = 0;
    std::uint64_t soundsDropped = 0;
    std::array<std::uint64_t, kInteractionResultCount> results{};
    std::uint64_t Count(InteractionResult r) const { return results[static_cast<std::size_t>(r)]; }
};

// Compact per-node replication record (client mirrors this, nothing else).
struct NodeReplicationState {
    NodeIndex node = kInvalidNode;
    std::uint8_t primaryState = 0;  // GeneratorState or DoorState
    std::uint16_t flags = 0;
    std::uint8_t progress = 0;      // 0..255 of the current channel
    std::uint32_t version = 0;
};
namespace RepFlag {
inline constexpr std::uint16_t Powered = 1u << 0;
inline constexpr std::uint16_t Energized = 1u << 1;
inline constexpr std::uint16_t Locked = 1u << 2;
inline constexpr std::uint16_t Lit = 1u << 3;
inline constexpr std::uint16_t AlarmActive = 1u << 4;
inline constexpr std::uint16_t Tripped = 1u << 5;
inline constexpr std::uint16_t OnOrClosed = 1u << 6;  // switched on / breaker closed
inline constexpr std::uint16_t Damaged = 1u << 7;
inline constexpr std::uint16_t Destroyed = 1u << 8;
inline constexpr std::uint16_t Armed = 1u << 9;
inline constexpr std::uint16_t Hacked = 1u << 10;
inline constexpr std::uint16_t Completed = 1u << 11;
inline constexpr std::uint16_t Emergency = 1u << 12;
}  // namespace RepFlag

// Durable, between-mission state. Only fields that represent physical
// condition, never switch positions.
struct WorldNodeRecord {
    std::string id;
    double health = 1.0;
    bool fuseInstalled = true;
    double fuel = -1.0;
    bool lockBroken = false;
};
struct WorldStateRecord {
    std::uint32_t formatVersion = 1;
    std::string facilityId;
    std::vector<WorldNodeRecord> nodes;
    std::string Serialize() const;
    static bool Parse(std::string_view text, WorldStateRecord& out, std::string* error = nullptr);
};
struct ImportReport {
    bool ok = false;
    std::size_t applied = 0;
    std::vector<std::string> ignored;  // unknown ids or nodes whose policy is Mission
    std::string error;
};

// Full in-mission state for checkpoints / host restore. Not for profiles.
struct MissionSnapshot {
    Tick tick = 0;
    bool started = false;
    std::vector<NodeState> nodes;
    std::vector<TimerEntry> timers;
    std::uint64_t timerSeq = 0;
    std::vector<Channel> channels;
    std::vector<PlayerSession> players;
};

class Facility {
public:
    Facility(const FacilityDef& def, AuthorityRole role = AuthorityRole::Server, FacilityConfig config = {});

    bool IsValid() const { return errors_.empty(); }
    const std::vector<std::string>& Errors() const { return errors_; }
    AuthorityRole Role() const { return role_; }

    void SetAcousticSink(IAcousticSink* sink) { sound_ = sink; }
    void SetObserver(IFacilityObserver* observer) { observer_ = observer; }
    void SetInventory(IPlayerInventory* inventory) { inventory_ = inventory; }

    // Players (server) ---------------------------------------------------------
    bool ConnectPlayer(PlayerId id, const Vec3& position, std::uint32_t permissions);
    bool DisconnectPlayer(PlayerId id);
    bool UpdatePlayerPosition(PlayerId id, const Vec3& position);
    const PlayerSession* Player(PlayerId id) const;

    // Interactions: the only path from a client into the simulation.
    InteractionResult Submit(const InteractionRequest& request);

    // Server-side gameplay hooks (damage system, mission scripts, overlaps).
    bool ServerApplyDamage(NodeIndex node, double amount);
    bool ServerTriggerAlarm(NodeIndex alarm);
    bool ServerNotifyPlayerPresence(PlayerId player, const Vec3& position);

    // Simulation -------------------------------------------------------------------
    void Step();
    Tick CurrentTick() const { return tick_; }

    // Queries ------------------------------------------------------------------------
    std::size_t NodeCount() const { return def_.nodes.size(); }
    NodeIndex Find(std::string_view id) const;
    const NodeDef& Def(NodeIndex i) const { return def_.nodes[i]; }
    const NodeState& State(NodeIndex i) const { return nodes_[i]; }
    bool IsLocked(NodeIndex door) const;
    bool IsPassable(NodeIndex door) const;
    bool BlocksNavigation(NodeIndex node) const;
    double IlluminationAt(const Vec3& position) const;
    std::size_t ActiveAlarms() const;
    const std::vector<NetworkStatus>& Networks() const { return networks_; }
    const std::vector<Channel>& Channels() const { return channels_; }
    const FacilityStats& Stats() const { return stats_; }

    // Replication -------------------------------------------------------------------
    // Server: appends changed nodes and clears their dirty flags.
    void CollectReplication(std::vector<NodeReplicationState>& out);
    // Client: mirrors replicated state; older versions are ignored.
    std::size_t ApplyReplication(const std::vector<NodeReplicationState>& states);
    NodeReplicationState MakeReplicationState(NodeIndex i) const;

    // Persistence ----------------------------------------------------------------------
    WorldStateRecord ExportWorldState(bool extracted) const;
    // Only valid at mission start (before the first Step).
    ImportReport ImportWorldState(const WorldStateRecord& record);
    MissionSnapshot Capture() const;
    bool Restore(const MissionSnapshot& snapshot);

    std::uint64_t StateHash() const;

private:

    // Power
    void MarkNetworkDirty(NodeIndex i);
    void SolveDirtyNetworks();
    void SolveNetwork(std::size_t network);
    void Energize(std::size_t network, bool includeBackup, std::vector<char>& energized) const;
    bool Conducts(NodeIndex i) const;
    double SourceOutput(NodeIndex i) const;
    bool WantsPower(NodeIndex i) const;
    void IntegrateFuel(NodeIndex i);
    void IntegrateBattery(NodeIndex i);
    void OnPowerChanged(NodeIndex i, bool wasPowered);

    // Timers
    void Schedule(NodeIndex i, TimerKind kind, Tick delay);
    void Cancel(NodeIndex i, TimerKind kind);
    bool Pending(NodeIndex i, TimerKind kind) const { return nodes_[i].timerPending[static_cast<std::size_t>(kind)]; }
    void FireTimers();
    void HandleTimer(NodeIndex i, TimerKind kind);

    // Behaviour
    void SetGenerator(NodeIndex i, GeneratorState s);
    InteractionResult StartGenerator(NodeIndex i, PlayerId by);
    void StopGenerator(NodeIndex i);
    void TripBreaker(NodeIndex i);
    InteractionResult OperateDoor(NodeIndex i, bool open, PlayerId by, bool remote);
    void FinishDoorMotion(NodeIndex i);
    void SetAlarm(NodeIndex i, bool active);
    void UpdateLight(NodeIndex i);
    void UpdateContinuousSound(NodeIndex i);
    void NotifySensors(const Vec3& position);
    InteractionResult ApplyAction(NodeIndex i, InteractionAction action, PlayerId by, bool remote);
    InteractionResult JoinChannel(NodeIndex i, ChannelKind kind, PlayerId player);
    void TickChannels();
    void CompleteChannel(const Channel& c);
    double ChannelRequired(NodeIndex i, ChannelKind kind) const;
    std::uint32_t ChannelMinParticipants(NodeIndex i, ChannelKind kind) const;
    void RemoveFromChannels(PlayerId player);

    // Output
    void Changed(NodeIndex i);
    void Emit(FacilityEventType type, NodeIndex i, std::int64_t value = 0, std::string text = {});
    void Sound(SoundKind kind, NodeIndex i, double loudnessDb, Tick duration = 1);
    InteractionResult Record(InteractionResult r);

    PlayerSession* FindPlayer(PlayerId id);
    bool IsDamaged(NodeIndex i) const;

    FacilityDef def_;
    AuthorityRole role_;
    FacilityConfig config_;
    std::vector<std::string> errors_;

    std::vector<NodeState> nodes_;
    std::vector<std::vector<NodeIndex>> children_;
    std::vector<std::vector<NodeIndex>> parents_;
    std::vector<std::size_t> networkOf_;
    std::vector<std::vector<NodeIndex>> networkNodes_;
    std::vector<NetworkStatus> networks_;
    std::vector<char> networkDirty_;
    std::vector<NodeIndex> lights_;
    std::vector<NodeIndex> sensors_;

    std::vector<TimerEntry> timers_;  // min-heap on (tick, seq)
    std::uint64_t timerSeq_ = 0;
    std::vector<Channel> channels_;
    std::vector<PlayerSession> players_;

    Tick tick_ = 0;
    bool started_ = false;
    std::size_t eventsThisTick_ = 0;
    std::size_t soundsThisTick_ = 0;

    IAcousticSink* sound_ = nullptr;
    IFacilityObserver* observer_ = nullptr;
    IPlayerInventory* inventory_ = nullptr;
    FacilityStats stats_;
    std::size_t lastActiveAlarms_ = 0;
};

}  // namespace llenv
