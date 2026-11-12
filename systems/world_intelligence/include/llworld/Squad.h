// LAST LIGHT - Environmental & World Intelligence (WI)
// Robot squad coordination: imperfect shared threat picture, comms topology
// (radio range, damage, weather, powered relays), task assignment and local
// autonomy. Produces tactical intents; movement/navigation stay in Unreal.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "llworld/Ini.h"
#include "llworld/WorldEvents.h"
#include "llworld/WorldTypes.h"

namespace llworld {

enum class ThreatClass : std::uint8_t { Disturbance = 0, Movement, Infrastructure, Hostile };
inline constexpr std::size_t kThreatClassCount = 4;
const char* ToString(ThreatClass t);

enum class ReportKind : std::uint8_t { Acoustic, Visual, NegativeSearch };

enum class IntentKind : std::uint8_t { Patrol = 0, Investigate, Search, Observe, Flank, Defend, LocalAutonomous };
const char* ToString(IntentKind k);

enum class CommsHealth : std::uint8_t { Intact, Damaged, Destroyed };

struct RoleDef {
    std::string name;
    bool canInvestigate = true;
    bool canObserve = false;
    bool canFlank = false;
    bool canDefend = true;
    double investigateWeight = 1.0;  // higher = preferred investigator
    double commsRangeCm = 6000.0;
    double observationStandoffCm = 1500.0;
    double reportQuality = 1.0;      // scales confidence of this role's own reports
};

struct SquadMemberDef {
    RobotId robot = 0;
    std::string role;
    std::vector<Vec3> patrol;
};

struct SquadDef {
    std::string id;
    std::vector<SquadMemberDef> members;
    std::string relayNode;  // optional power-grid CommArray acting as a relay
    Vec3 relayPosition;
    double relayRangeCm = 0.0;
    Vec3 defendPoint;
};

struct SquadConfig {
    std::vector<RoleDef> roles;
    std::vector<SquadDef> squads;
    double ticksPerSecond = 30.0;
    Tick coordinationIntervalTicks = 15;
    Tick reportLatencyTicks = 6;
    double mergeRadiusCm = 800.0;
    double confidenceHalfLifeTicks = 900.0;
    double forgetConfidence = 0.08;
    double investigateMinConfidence = 0.25;
    double sharedConfidenceMul = 0.85;   // second-hand information is worth less
    double negativeReportFactor = 0.25;  // "searched, found nothing"
    double minPatrolFraction = 0.34;     // share of a group that keeps patrolling
    double hostileFlankConfidence = 0.6;
    double assignmentHysteresis = 0.5;
    double arrivalRadiusCm = 200.0;
    Tick staleOrderTicks = 300;          // isolated robots keep old orders this long
    double damagedCommsRangeMul = 0.5;
    double damagedDropChance = 0.35;     // per coordination step
    std::size_t maxContacts = 12;
    std::size_t maxPendingReports = 128;
    std::size_t dedupHistory = 256;
    std::array<double, kThreatClassCount> threatWeights{0.4, 0.7, 0.8, 1.0};
    std::uint64_t seed = 0x5351414Dull;

    static std::vector<RoleDef> DefaultRoles();
};

std::vector<std::string> ValidateSquads(const SquadConfig& c);
// Sections: [squads], [role.<NAME>], [squad.<ID>].
void LoadSquads(const IniDocument& doc, SquadConfig& c, std::vector<std::string>& errors);

// Power-grid port: is the relay powered and switched on?
class ISquadWorld {
public:
    virtual ~ISquadWorld() = default;
    virtual bool RelayOperational(std::string_view relayNode) const = 0;
};

struct ContactReport {
    RobotId reporter = 0;
    std::uint64_t reportId = 0;  // unique per reporter (dedup)
    ReportKind kind = ReportKind::Acoustic;
    ThreatClass threat = ThreatClass::Disturbance;
    Vec3 estimate;
    double uncertaintyCm = 500.0;
    double confidence = 0.5;
    Tick tick = 0;
};

// What a robot believes. Never ground truth.
struct Contact {
    std::uint32_t id = 0;
    ThreatClass threat = ThreatClass::Disturbance;
    Vec3 estimate;
    double uncertaintyCm = 0.0;
    double confidence = 0.0;
    Tick firstTick = 0;
    Tick lastTick = 0;
    std::uint32_t reports = 0;
    std::uint32_t sources = 0;  // bitmask of squad member slots
    bool cleared = false;       // searched with no result since the last positive report
};

struct TacticalIntent {
    IntentKind kind = IntentKind::Patrol;
    Vec3 target;
    double radiusCm = 0.0;
    std::uint32_t contactId = 0;
    bool fromSquad = true;  // false = robot's own decision
    Tick issuedTick = 0;
    std::uint32_t version = 0;
};

enum class ReportResult : std::uint8_t { Accepted, Duplicate, UnknownRobot, RobotDisabled, Invalid };
const char* ToString(ReportResult r);

class SquadCoordinator {
public:
    SquadCoordinator(const SquadConfig& config, WorldEventBus* bus, ISquadWorld* world);

    bool IsValid() const { return errors_.empty(); }
    const std::vector<std::string>& Errors() const { return errors_; }

    // Robot Damage System / pawn state (UNVERIFIED producer in Unreal).
    bool UpdateRobot(RobotId robot, const Vec3& position, bool alive, CommsHealth comms);
    ReportResult Report(const ContactReport& report);
    void Step(Tick now, double weatherCommsMul);

    const TacticalIntent* Intent(RobotId robot) const;
    const std::vector<Contact>* KnownContacts(RobotId robot) const;
    bool IsConnected(RobotId robot) const;  // reachable from its squad leader
    bool CanCommunicate(RobotId a, RobotId b) const;
    double ObjectivePriorityFor(RobotId robot) const;  // AIS RobotHostState lever
    std::uint32_t SquadOf(RobotId robot) const;

    std::uint64_t ReportsShared() const { return shared_; }
    std::uint64_t ReportsDropped() const { return dropped_; }
    std::uint64_t StateHash() const;

private:
    struct Member {
        RobotId id = 0;
        std::uint32_t squad = 0;
        std::uint32_t slot = 0;
        std::uint32_t role = 0;
        Vec3 position;
        bool alive = true;
        CommsHealth comms = CommsHealth::Intact;
        std::vector<Contact> board;
        std::uint32_t nextContactId = 1;
        TacticalIntent intent;
        Tick lastOrderTick = 0;
        std::uint32_t component = 0;
        bool connected = true;
        std::size_t patrolIndex = 0;
    };
    struct Pending {
        ContactReport report;
        std::size_t member = 0;
        Tick deliverAt = 0;
    };

    Member* FindMember(RobotId id);
    const Member* FindMember(RobotId id) const;
    void Fuse(Member& m, const ContactReport& r, double confidence, std::uint32_t sourceSlot);
    void Decay(Member& m, double dtTicks);
    void ComputeComms(std::uint32_t squad, double weatherCommsMul, Tick now);
    void Assign(std::uint32_t squad, Tick now);
    void SetIntent(Member& m, const TacticalIntent& intent, Tick now);
    TacticalIntent PatrolIntent(Member& m) const;

    SquadConfig cfg_;
    WorldEventBus* bus_;
    ISquadWorld* world_;
    std::vector<std::string> errors_;
    std::vector<Member> members_;                        // sorted by robot id
    std::vector<std::vector<std::size_t>> squadMembers_;  // indices into members_
    std::vector<Pending> pending_;
    std::vector<std::pair<RobotId, std::uint64_t>> seen_;
    std::size_t seenHead_ = 0;
    Tick lastStep_ = 0;
    Tick nextStep_ = 0;
    bool started_ = false;
    std::uint64_t shared_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace llworld
