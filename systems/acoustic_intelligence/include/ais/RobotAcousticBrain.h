// LAST LIGHT - Acoustic Intelligence System (AIS)
// Per-robot acoustic memory and investigation state machine. A brain only
// ever receives noisy estimates; it never sees an event's true position,
// true category or source identity.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ais/AcousticEvent.h"
#include "ais/AisTypes.h"
#include "ais/RobotSensoryProfile.h"

namespace ais {

enum class AwarenessState : std::uint8_t {
    Unaware = 0,
    Suspicious,
    Investigating,
    Searching,
    Combat,
    ReturningToPatrol,
};
const char* ToString(AwarenessState s);

// What a robot "heard". Built by the world from propagation results.
struct PerceivedStimulus {
    EventId eventId = 0;
    StimulusCategory apparentCategory = StimulusCategory::Environmental;
    Vec3 estimatedPosition;
    double uncertaintyCm = 0.0;
    double effectiveSnrDb = 0.0;
    double artificialCue = 0.0;  // 0..1, how synthetic it sounded to this robot
    double threat = 0.0;         // category threat from config
    bool hostileCategory = false;
    bool regularityIsSuspicious = false;
    bool fromSharedReport = false;
    bool sharedSuspectedArtificial = false;
    Tick tick = 0;
};

// Signal-network payload. Carries an estimate, never ground truth.
struct AcousticReport {
    EntityId senderId = kNoEntity;
    Vec3 senderPosition;
    Vec3 estimate;
    double uncertaintyCm = 0.0;
    double confidence = 0.0;
    StimulusCategory category = StimulusCategory::Environmental;
    Tick sentTick = 0;
    bool claimingInvestigation = false;
    bool suspectedArtificial = false;
};

struct AcousticTrace {
    std::uint32_t traceId = 0;
    StimulusCategory category = StimulusCategory::Environmental;
    Vec3 estimate;
    double uncertaintyCm = 0.0;
    double confidence = 0.0;
    Tick firstTick = 0;
    Tick lastTick = 0;
    Tick lastDirectTick = 0;
    std::uint32_t hitCount = 0;
    std::uint32_t directHits = 0;
    double threat = 0.0;
    bool hostile = false;
    double artificialEvidence = 0.0;
    bool suspectedArtificial = false;
    std::uint32_t regularStreak = 0;
    Tick lastInterval = 0;
    std::uint32_t fruitlessSearches = 0;
};

// Host-provided facts about the robot body, refreshed each tick by the game
// (in Unreal: pawn location, LLTacticalDecision objective priority, sight).
struct RobotHostState {
    Vec3 position;
    double objectivePriority = 0.0;  // 0..100; higher = harder to distract
    bool hasVisualContact = false;
    Vec3 visualContactPosition;
};

enum class DirectiveReason : std::uint8_t {
    None = 0,
    HeardSomething,
    InvestigatingSound,
    TargetUpdated,
    ArrivedNoSource,
    SourceLost,
    SearchPoint,
    SearchExhausted,
    AcousticCombat,
    VisualCombat,
    CombatContactLost,
    ReturningHome,
    ObjectiveHeld,
    SupportingAlly,
    LostInterest,
};
const char* ToString(DirectiveReason r);

// Recommendation consumed by the host AI (LLTacticalDecision adapter). AIS
// never moves the robot itself.
struct AcousticDirective {
    AwarenessState state = AwarenessState::Unaware;
    DirectiveReason reason = DirectiveReason::None;
    bool hasMoveTarget = false;
    Vec3 moveTarget;
    bool hasLookAt = false;
    Vec3 lookAt;
    double suspicion = 0.0;
    double targetUncertaintyCm = 0.0;
    std::uint32_t traceId = 0;
};

class RobotAcousticBrain {
public:
    RobotAcousticBrain(EntityId robotId, const RobotSensoryProfile& profile, std::uint64_t seed);

    EntityId Id() const { return id_; }
    const RobotSensoryProfile& Profile() const { return profile_; }

    // Buffers a perception until the next sensory update. When full, the
    // weakest perception is dropped.
    void Enqueue(const PerceivedStimulus& stimulus);
    bool IsUpdateDue(Tick now) const;

    // Runs decay, memory fusion and the state machine. Optionally emits a
    // signal-network report.
    void Update(Tick now, const RobotHostState& host, AcousticReport* outReport,
                bool* outHasReport);

    // Another robot claims to be investigating near `area`.
    void NotifyAllyInvestigating(EntityId ally, const Vec3& area, Tick now);

    const AcousticDirective& Directive() const { return directive_; }
    AwarenessState State() const { return state_; }
    double Suspicion() const { return suspicion_; }
    const std::vector<AcousticTrace>& Traces() const { return traces_; }
    const AcousticTrace* FindTrace(std::uint32_t traceId) const;
    std::size_t InboxSize() const { return inbox_.size(); }
    std::size_t DroppedPerceptions() const { return droppedPerceptions_; }

    void HashInto(StateHasher& hasher) const;

private:
    void Decay(Tick now);
    void Absorb(const PerceivedStimulus& s, const RobotHostState& host, Tick now);
    AcousticTrace& MergeOrCreate(const PerceivedStimulus& s, Tick now, bool& outCreated);
    double InterestFor(const AcousticTrace& t) const;
    double ScoreTrace(const AcousticTrace& t, const Vec3& from) const;
    AcousticTrace* SelectTarget(const Vec3& from);
    bool CombatGate(const AcousticTrace* t, const Vec3& from) const;
    bool WantsToInvestigate(const AcousticTrace* t, const RobotHostState& host) const;
    void BeginSearch(const Vec3& center, double uncertaintyCm, Tick now);
    void EnterState(AwarenessState s, Tick now);
    void RunStateMachine(Tick now, const RobotHostState& host);
    Vec3 SupportOffset(const Vec3& target, double radius) const;

    EntityId id_;
    RobotSensoryProfile profile_;
    std::uint64_t seed_;

    std::vector<PerceivedStimulus> inbox_;
    std::size_t droppedPerceptions_ = 0;
    std::vector<AcousticTrace> traces_;
    std::uint32_t nextTraceId_ = 1;

    AwarenessState state_ = AwarenessState::Unaware;
    Tick stateEnterTick_ = 0;
    Tick lastUpdateTick_ = 0;
    bool hasUpdated_ = false;
    double suspicion_ = 0.0;

    std::uint32_t targetTraceId_ = 0;
    Vec3 lastTarget_;
    double lastTargetUncertainty_ = 0.0;
    bool hasPatrolAnchor_ = false;
    Vec3 patrolAnchor_;

    std::vector<Vec3> searchPoints_;
    std::size_t searchIndex_ = 0;
    std::uint32_t searchTraceId_ = 0;
    Vec3 searchCenter_;
    double searchRadius_ = 0.0;
    Tick lastCombatEvidenceTick_ = 0;

    EntityId supportingAlly_ = kNoEntity;
    Vec3 supportArea_;
    Tick supportUntil_ = 0;

    bool hasReported_ = false;
    Tick lastReportedTraceUpdate_ = 0;

    AcousticDirective directive_;
};

}  // namespace ais
