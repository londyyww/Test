// LAST LIGHT - Environmental & World Intelligence: integration layer.
// The only code that knows the Acoustic Intelligence System (AIS), the power
// grid (llenv) and the three WI subsystems. It wires them through explicit
// ports and the shared WorldEventBus, in a fixed deterministic order.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AisAcousticBridge.h"
#include "ais/AcousticConfig.h"
#include "ais/AcousticPropagation.h"
#include "ais/AcousticWorld.h"
#include "ais/SignalRelay.h"
#include "llenv/Facility.h"
#include "llworld/Hazards.h"
#include "llworld/Squad.h"
#include "llworld/Weather.h"
#include "llworld/WorldEvents.h"

namespace llworld {

struct RouteDef {
    std::uint32_t id = 0;
    std::string name;
    std::vector<Vec3> waypoints;
};

struct WorldIntelligenceConfig {
    WeatherConfig weather = WeatherConfig::MakeDefault();
    HazardConfig hazards;
    SquadConfig squads;
    std::vector<std::string> exposedNodes;  // power-grid nodes open to the weather
    std::vector<RouteDef> routes;
    std::size_t maxEventsPerTick = 256;
    double reportMinConfidence = 0.3;
    double minDetectionRangeCm = 300.0;  // nobody is invisible at arm's length
    WorldIntelligenceConfig() { squads.roles = SquadConfig::DefaultRoles(); }
};

// Parses one INI containing [world], [route.*], weather, hazard and squad
// sections. Unknown section kinds are errors.
bool LoadWorldConfig(const std::string& text, WorldIntelligenceConfig& cfg, std::vector<std::string>& errors);

enum class OrderSource : std::uint8_t { Squad, Acoustic, Idle };

// What the host AI should do with a robot this tick.
struct RobotOrder {
    OrderSource source = OrderSource::Idle;
    IntentKind intent = IntentKind::Patrol;
    bool hasMoveTarget = false;
    Vec3 moveTarget;
    double radiusCm = 0.0;
    ais::AwarenessState awareness = ais::AwarenessState::Unaware;
};

class WorldIntelligence final : public IEnvironmentQuery {
public:
    // `aisEnv` holds level geometry; `facilityDef` may be null (no power grid).
    WorldIntelligence(const WorldIntelligenceConfig& config, const ais::AisConfig& aisConfig, ais::BoxAcousticEnvironment& aisEnv,
                      const llenv::FacilityDef* facilityDef, llenv::IPlayerInventory* inventory);
    ~WorldIntelligence() override;
    WorldIntelligence(const WorldIntelligence&) = delete;
    WorldIntelligence& operator=(const WorldIntelligence&) = delete;

    bool IsValid() const { return errors_.empty(); }
    const std::vector<std::string>& Errors() const { return errors_; }
    const WorldIntelligenceConfig& Config() const { return cfg_; }

    // Robots must also appear in a squad definition to be coordinated.
    bool AddRobot(RobotId id, const char* aisProfile, const Vec3& position);
    bool SetRobotBody(RobotId id, const Vec3& position, bool alive, CommsHealth comms);
    void SetPlayers(const std::vector<PlayerSample>& players);

    // Server-side game inputs.
    ais::SubmitResult PlayerFootstep(PlayerId player, const Vec3& position, ais::SurfaceType surface, double loudnessOffsetDb = 0.0);
    void ReportGunfire(PlayerId shooter, const Vec3& position);
    void ReportExplosion(const Vec3& position, double radiusCm);

    void Step();
    Tick CurrentTick() const { return tick_; }

    RobotOrder OrderFor(RobotId robot) const;
    double RouteRisk(std::uint32_t routeId) const;  // +inf when blocked
    double VisualDetectionRangeCm(double baseRangeCm, const Vec3& observer, const Vec3& target) const;
    double ThermalDetectionRangeCm(double baseRangeCm, const Vec3& observer, const Vec3& target) const;

    // IEnvironmentQuery
    VisibilitySample VisibilityAt(const Vec3& position) const override;
    AcousticConditions Acoustics() const override;
    SurfaceConditions SurfaceAt(const Vec3& position) const override;
    double HazardDangerAt(const Vec3& position) const override;
    bool IsRouteBlocked(std::uint32_t routeId) const override;

    WorldEventBus& Bus() { return bus_; }
    WeatherSystem& Weather() { return *weather_; }
    HazardSystem& Hazards() { return *hazards_; }
    SquadCoordinator& Squads() { return *squads_; }
    ais::AcousticWorld& Acoustic() { return *ais_; }
    llenv::Facility* Facility() { return facility_.get(); }
    const llenv::AisAcousticBridge* FacilityBridge() const { return bridge_.get(); }

    std::uint64_t StateHash() const;
    std::uint64_t ReportsFromAcoustics() const { return acousticReports_; }

private:
    class FacilityPorts;
    class SquadSignalPort;
    void ForwardWeather();
    void FeedSquadsFromAcoustics();
    void PushSquadPriorities();
    void SubmitAcoustic(ais::StimulusCategory c, const Vec3& p, double db, ais::EntityId source, ais::Tick duration);

    WorldIntelligenceConfig cfg_;
    const ais::AisConfig& aisCfg_;
    ais::BoxAcousticEnvironment& aisEnv_;
    std::vector<std::string> errors_;
    WorldEventBus bus_;
    std::unique_ptr<llenv::Facility> facility_;
    std::unique_ptr<FacilityPorts> ports_;
    std::unique_ptr<SquadSignalPort> signal_;
    std::unique_ptr<ais::AcousticWorld> ais_;
    std::unique_ptr<llenv::AisAcousticBridge> bridge_;
    std::unique_ptr<WeatherSystem> weather_;
    std::unique_ptr<HazardSystem> hazards_;
    std::unique_ptr<SquadCoordinator> squads_;
    std::vector<int> exposedNodes_;
    std::vector<PlayerSample> players_;
    std::vector<RobotId> robots_;
    struct Reported {
        RobotId robot;
        std::uint32_t trace;
        Tick lastDirect;
        std::uint32_t fruitless;
    };
    std::vector<Reported> reported_;
    std::uint32_t lastWeatherVersion_ = 0xFFFFFFFFu;
    Tick tick_ = 0;
    Tick nextExposureTick_ = 0;
    std::uint64_t acousticReports_ = 0;
};

}  // namespace llworld
