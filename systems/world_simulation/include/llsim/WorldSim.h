// LAST LIGHT - Mission World Simulation (portable C++17 core, no Unreal headers).
//
// Keeps a lightweight, authoritative model of what the abandoned world is doing
// during a raid: patrol schedules, generator fuel, facility security, comms,
// hazard-zone activity. It manages activity and intention only:
//   * power distribution stays in llenv::Facility (bound entities read it),
//   * hazards and weather stay in llworld (consumed as signals),
//   * robot squads, navigation and perception stay in llworld / Unreal AI
//     (this module emits PatrolDirectives they may act on),
//   * objectives stay in the Mission Director (it calls the lifecycle API).
//
// Nothing runs per frame for every entity. Entities get scheduled updates at a
// tier-dependent interval; everything else is an exact-time timer (fuel empty,
// response finished, schedule) or an event-driven transition.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "llsim/Scheduler.h"
#include "llworld/Ini.h"
#include "llworld/WorldTypes.h"

namespace llsim {

using llworld::Tick;
using llworld::Vec3;

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;
inline constexpr std::uint32_t kSimConfigVersion = 1;

enum class EntityKind : std::uint8_t {
    PatrolGroup = 0,
    Facility,
    Generator,
    PowerNetwork,
    SecuritySystem,
    Machinery,
    Storage,
    CommsStation,
    HazardZone,
};
inline constexpr std::size_t kEntityKindCount = 9;

enum class EntityState : std::uint8_t {
    Nominal = 0,
    Active,
    Standby,
    Degraded,
    Offline,
    Alarm,
    Lockdown,
    Interrupted,
    Responding,
};
inline constexpr std::size_t kEntityStateCount = 9;

enum class Tier : std::uint8_t { High = 0, Medium, Low };
inline constexpr std::size_t kTierCount = 3;

enum class SignalKind : std::uint8_t {
    PowerLost = 0,
    PowerRestored,
    AlarmRaised,
    AlarmCleared,
    CommsLost,
    CommsRestored,
    HazardActivated,
    HazardCleared,
    WeatherSevere,
    WeatherCleared,
    Disturbance,  // noise / interaction reported by game systems (loot searches, gunfire)
};
inline constexpr std::size_t kSignalKindCount = 11;

const char* ToString(EntityKind k);
const char* ToString(EntityState s);
const char* ToString(Tier t);
const char* ToString(SignalKind s);
bool Parse(std::string_view s, EntityKind& out);
bool Parse(std::string_view s, EntityState& out);
bool Parse(std::string_view s, SignalKind& out);
bool StateAllowed(EntityKind k, EntityState s);

// ---------------------------------------------------------------- configuration

struct TierDef {
    Tick intervalTicks = 30;
    double radiusCm = 0.0;  // player distance to an area's edge (High / Medium only)
};

struct AreaDef {
    std::string id;
    Vec3 center;
    double radiusCm = 1000.0;
    bool objective = false;  // affects current objectives: never below Medium
};

struct EntityDef {
    std::string id;
    EntityKind kind = EntityKind::Facility;
    std::string area;
    bool hasPosition = false;
    Vec3 position;
    std::vector<std::string> dependsOn;
    std::string bind;  // "node:<facility node>" or "hazard:<hazard id>"; empty = abstract
    std::string initialState;
    bool persist = false;  // eligible for ExportPersistentState()
    // Generator
    double fuelSeconds = -1.0;  // < 0 unlimited
    double burnRate = 1.0;      // fuel seconds per simulated second
    double lowFuelSeconds = 0.0;
    std::string backupFor;
    Tick startDelayTicks = 60;
    bool running = true;
    // Patrol group
    std::vector<std::string> route;
    Tick dwellTicks = 600;
    Tick travelTicks = 600;
    double respondRadiusCm = 4000.0;
    Tick respondHoldTicks = 900;
    std::string comms;
    // Hazard zone autonomous cycle (0 = only external signals)
    Tick activeTicks = 0;
    Tick dormantTicks = 0;
};

struct ScheduleDef {
    std::string id;
    Tick atTicks = 0;
    Tick repeatTicks = 0;
    std::vector<std::string> actions;
};

struct RuleDef {
    std::string id;
    std::string on;
    std::vector<std::string> requires_;  // trailing underscore: `requires` is a C++20 keyword
    std::vector<std::string> actions;
    Tick delayTicks = 0;
    bool once = false;
};

struct SimConfig {
    std::uint32_t version = 0;
    double ticksPerSecond = 30.0;
    std::array<TierDef, kTierCount> tiers{{TierDef{3, 3000.0}, TierDef{30, 9000.0}, TierDef{300, 0.0}}};
    std::size_t maxUpdatesPerStep = 256;
    std::size_t maxTimersPerStep = 512;
    std::size_t queueCapacity = 16384;
    Tick relevanceIntervalTicks = 15;
    Tick observedWindowTicks = 300;
    std::size_t maxTransitionsPerStep = 256;
    std::size_t eventLogCapacity = 512;
    std::size_t maxPendingInstructions = 4;
    std::vector<AreaDef> areas;
    std::vector<EntityDef> entities;
    std::vector<ScheduleDef> schedules;
    std::vector<RuleDef> rules;
};

bool IsSimSection(std::string_view name);
void LoadSimSections(const llworld::IniDocument& doc, SimConfig& cfg, std::vector<std::string>& errors);
std::vector<std::string> ValidateSim(const SimConfig& cfg);

// ---------------------------------------------------------------- ports

struct InfraSample {
    bool known = false;
    bool powered = false;
    bool running = false;  // generators / machinery
    bool alarm = false;    // alarm panels
};

// Read-only view of systems that own bound entities (power grid, hazards).
class IWorldInfrastructurePort {
public:
    virtual ~IWorldInfrastructurePort() = default;
    virtual InfraSample SampleNode(std::string_view node) const = 0;
    virtual bool HazardActive(std::string_view hazard) const = 0;
};

enum class DirectiveKind : std::uint8_t { MoveTo, Hold, Respond, ResumeRoute };

// Mission-level intention for a patrol group. Unreal AI / the squad layer decides
// how robots get there; this is not a movement or tactics command.
struct PatrolDirective {
    std::uint32_t group = kNone;
    DirectiveKind kind = DirectiveKind::MoveTo;
    std::uint32_t area = kNone;
    Vec3 target;
    bool central = false;  // issued by central command (needed comms)
    Tick tick = 0;
};

enum class SimEventType : std::uint8_t {
    StateChanged,          // from/to = EntityState
    Directive,             // aux = DirectiveKind, area in `area`
    InstructionQueued,     // comms down: central order held
    InstructionDelivered,
    InstructionDropped,    // pending queue full
    RuleFired,             // aux = rule index
    ScheduleFired,         // aux = schedule index
    TierChanged,           // from/to = Tier
    FuelLow,
    FuelEmpty,
    BackupStarted,
    OverrideApplied,
    OverrideExpired,
};
const char* ToString(SimEventType t);

struct SimEvent {
    Tick tick = 0;
    SimEventType type = SimEventType::StateChanged;
    std::uint32_t entity = kNone;
    std::uint8_t from = 0;
    std::uint8_t to = 0;
    std::uint32_t aux = 0;
    std::uint32_t area = kNone;
};

class IWorldSimObserver {
public:
    virtual ~IWorldSimObserver() = default;
    virtual void OnSimEvent(const SimEvent& e) = 0;
    virtual void OnPatrolDirective(const PatrolDirective& d) = 0;
};

// ---------------------------------------------------------------- runtime

enum class RaidPhase : std::uint8_t { Initial, Active, Completed };

struct EntityView {
    const EntityDef* def = nullptr;
    EntityState state = EntityState::Nominal;      // effective
    EntityState baseState = EntityState::Nominal;  // own state before dependencies
    Tier tier = Tier::Low;
    Tick lastUpdate = 0;
    std::uint32_t area = kNone;  // current area (moves with patrols)
    std::uint32_t updates = 0;
    double fuelSeconds = -1.0;   // generators, evaluated at the current tick
    bool commsConnected = true;  // patrol groups
    std::size_t pendingInstructions = 0;
    bool overridden = false;
    std::vector<std::uint32_t> activeTimers;  // TimerKind values pending
};

struct PersistentEntityRecord {
    std::string id;
    EntityState state = EntityState::Nominal;
    double fuelSeconds = -1.0;
};

struct SimStats {
    std::uint64_t steps = 0;
    std::array<std::uint64_t, kTierCount> updatesByTier{};
    std::uint64_t timersFired = 0;
    std::uint64_t staleSkipped = 0;
    std::uint64_t deferredUpdates = 0;
    std::uint64_t deferredTimers = 0;
    std::uint64_t transitions = 0;
    std::uint64_t deferredTransitions = 0;
    std::uint64_t queueOverflows = 0;
    std::uint64_t rulesFired = 0;
    std::uint64_t directives = 0;
    std::uint64_t relevancePasses = 0;
    std::uint64_t Updates() const { return updatesByTier[0] + updatesByTier[1] + updatesByTier[2]; }
};

class WorldSimulation {
public:
    explicit WorldSimulation(const SimConfig& config);
    ~WorldSimulation();
    WorldSimulation(const WorldSimulation&) = delete;
    WorldSimulation& operator=(const WorldSimulation&) = delete;

    bool IsValid() const { return errors_.empty(); }
    const std::vector<std::string>& Errors() const { return errors_; }
    void SetInfrastructurePort(IWorldInfrastructurePort* p) { infra_ = p; }
    void SetObserver(IWorldSimObserver* o) { observer_ = o; }

    // Raid lifecycle (driven by the Mission Director; this is not a mission state machine).
    bool BeginRaid(std::uint64_t raidSeed, const std::vector<PersistentEntityRecord>& persisted = {});
    void CompleteMission();  // freezes the simulation; state stays queryable
    void ResetWorld();       // back to the authored initial state
    RaidPhase Phase() const { return phase_; }
    std::vector<PersistentEntityRecord> ExportPersistentState() const;
    // Temporary mission change; reverts after durationTicks (0 = until raid end).
    bool ApplyMissionOverride(std::uint32_t entity, EntityState state, Tick durationTicks);

    // Inputs from game systems --------------------------------------------------
    void SetPlayers(const std::vector<Vec3>& positions);  // up to 4
    void ReportObserved(std::uint32_t entity);            // seen by a player
    void ReportInteraction(std::uint32_t entity);         // player interacting with it
    void NotifySignal(SignalKind kind, std::string_view bindRef, const Vec3& position = {}, double magnitude = 0.0);
    void NotifySignalForEntity(SignalKind kind, std::uint32_t entity);
    void NotifyBoundChanged(std::string_view bindRef);  // infrastructure changed: resample now
    void ReportDisturbance(const Vec3& position, double loudnessDb);

    void Step();
    void AdvanceTo(Tick tick);
    Tick Now() const { return now_; }

    // Queries -------------------------------------------------------------------
    std::uint32_t Find(std::string_view id) const;
    std::uint32_t FindArea(std::string_view id) const;
    std::size_t EntityCount() const;
    EntityView View(std::uint32_t entity) const;
    EntityState StateOf(std::uint32_t entity) const;
    Tier TierOf(std::uint32_t entity) const;
    Tier AreaTier(std::uint32_t area) const;
    std::vector<SimEvent> RecentEvents() const;  // bounded ring, oldest first
    std::uint64_t EventsLogged() const { return logged_; }
    bool LastDirective(std::uint32_t group, PatrolDirective& out) const;
    const SimStats& Stats() const { return stats_; }
    std::size_t QueueSize() const;
    std::uint64_t StateHash() const;
    const SimConfig& Config() const { return cfg_; }

    struct Impl;

private:
    SimConfig cfg_;
    std::vector<std::string> errors_;
    std::unique_ptr<Impl> impl_;
    IWorldInfrastructurePort* infra_ = nullptr;
    IWorldSimObserver* observer_ = nullptr;
    RaidPhase phase_ = RaidPhase::Initial;
    Tick now_ = 0;
    std::vector<SimEvent> log_;
    std::size_t logHead_ = 0;
    std::uint64_t logged_ = 0;
    SimStats stats_;
    friend struct Impl;
};

}  // namespace llsim
