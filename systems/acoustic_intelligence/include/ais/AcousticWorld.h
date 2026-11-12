// LAST LIGHT - Acoustic Intelligence System (AIS)
// Server-side orchestrator: drains authoritative events, runs broadphase and
// propagation, feeds robot brains, relays signal-network reports and selects
// cosmetic audio cues per client. Robot decisions never depend on client data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/AcousticEventBus.h"
#include "ais/AcousticPropagation.h"
#include "ais/DecoySystem.h"
#include "ais/RobotAcousticBrain.h"
#include "ais/SignalRelay.h"

namespace ais {

enum class AuthorityRole : std::uint8_t { Server, Client };

// A connected player's listener, used only to decide which cosmetic cues to
// replicate. Never read by robot brains.
struct ClientListener {
    std::uint32_t clientId = 0;
    Vec3 position;
};

// Cosmetic replication record. Carries the apparent signature only, so a
// client cannot learn which sounds are decoys or what any robot perceived.
struct CosmeticAudioCue {
    std::uint32_t clientId = 0;
    EventId eventId = 0;
    StimulusCategory apparentCategory = StimulusCategory::Environmental;
    SurfaceType surface = SurfaceType::None;
    Vec3 position;
    double loudnessDb = 0.0;
};

struct WorldStats {
    std::uint64_t eventsProcessed = 0;
    std::uint64_t perceptionChecks = 0;
    std::uint64_t occlusionTraces = 0;
    std::uint64_t perceptionsDelivered = 0;
    std::uint64_t eventsDeferred = 0;
    std::uint64_t reportsPublished = 0;
    std::uint64_t reportsDelivered = 0;
    std::uint64_t brainUpdates = 0;
    std::uint64_t cosmeticCues = 0;
};

// Observer for perceptions (debug drawing, telemetry, tests).
struct PerceptionRecord {
    EntityId robotId = kNoEntity;
    EventId eventId = 0;
    Vec3 truePosition;  // for debugging only; never given to the brain
    PerceivedStimulus perceived;
};

class AcousticWorld {
public:
    AcousticWorld(const AisConfig& config, const IAcousticEnvironment& environment,
                  AuthorityRole role = AuthorityRole::Server,
                  ISignalNetworkPort* signalNetwork = nullptr);

    AuthorityRole Role() const { return role_; }
    Tick CurrentTick() const { return tick_; }
    const AisConfig& Config() const { return config_; }

    // Robots ------------------------------------------------------------------
    bool AddRobot(EntityId robotId, std::string_view profileName, const Vec3& position);
    bool AddRobot(EntityId robotId, const RobotSensoryProfile& profile, const Vec3& position);
    bool RemoveRobot(EntityId robotId);
    bool SetHostState(EntityId robotId, const RobotHostState& host);
    const RobotHostState* HostState(EntityId robotId) const;
    const RobotAcousticBrain* Brain(EntityId robotId) const;
    const AcousticDirective* Directive(EntityId robotId) const;
    std::size_t RobotCount() const { return robots_.size(); }

    // Events ------------------------------------------------------------------
    SubmitResult SubmitEvent(const AcousticEvent& event, EventOrigin origin,
                             EventId* outId = nullptr);
    DecoySystem& Decoys() { return decoys_; }
    const AcousticEventBus& Bus() const { return bus_; }

    // Cosmetic replication -----------------------------------------------------
    void SetClientListeners(const std::vector<ClientListener>& listeners);
    const std::vector<CosmeticAudioCue>& CosmeticCuesThisTick() const { return cues_; }

    // Simulation ----------------------------------------------------------------
    // Advances one fixed simulation tick. No-op for AuthorityRole::Client.
    void Step();

    void SetPerceptionObserver(std::vector<PerceptionRecord>* sink) { observer_ = sink; }
    const WorldStats& Stats() const { return stats_; }
    std::uint64_t StateHash() const;

private:
    struct RobotSlot {
        EntityId id = kNoEntity;
        RobotHostState host;
        std::unique_ptr<RobotAcousticBrain> brain;
    };

    RobotSlot* FindSlot(EntityId id);
    const RobotSlot* FindSlot(EntityId id) const;
    void RebuildGrid();
    void QueryGrid(const Vec3& center, double radius, std::vector<std::size_t>& out) const;
    void ProcessEvent(const AcousticEvent& event);
    void EmitCosmetic(const AcousticEvent& event, double audibleRadius);
    void DeliverReports();
    void RecomputeBestThreshold();

    const AisConfig& config_;
    const IAcousticEnvironment& env_;
    AuthorityRole role_;
    AcousticPropagationModel propagation_;
    AcousticEventBus bus_;
    DecoySystem decoys_;
    std::unique_ptr<LocalSignalRelay> ownedRelay_;
    ISignalNetworkPort* relay_;

    Tick tick_ = 0;
    std::vector<RobotSlot> robots_;  // sorted by id for determinism
    BandArray bestThresholdDb_{0.0, 0.0, 0.0};

    struct GridCell {
        std::int64_t cx;
        std::int64_t cy;
        std::size_t robotIndex;
    };
    std::vector<GridCell> grid_;  // sorted by (cx, cy, robotIndex)

    std::vector<ClientListener> listeners_;
    std::vector<CosmeticAudioCue> cues_;
    std::vector<std::size_t> cuesPerListener_;

    std::vector<AcousticEvent> drainScratch_;
    std::vector<std::size_t> candidateScratch_;
    std::vector<DeliveredReport> reportScratch_;
    std::vector<RelayReceiver> receiverScratch_;

    std::vector<PerceptionRecord>* observer_ = nullptr;
    WorldStats stats_;
};

}  // namespace ais
