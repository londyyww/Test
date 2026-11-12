// LAST LIGHT - Mission world host: binds the portable loot and world-simulation
// cores to the existing environmental stack (PR #1-#3) through explicit ports.
//
//   power grid (llenv::Facility)  -> ILootAccessPort::IsPowered, IWorldInfrastructurePort
//   hazards / weather (llworld)   -> world-simulation signals
//   acoustic intelligence (AIS)   <- container search sounds (ILootSoundSink)
//   player inventory (llenv port) -> tools and clearances (keycards)
//
// It owns none of those systems and duplicates none of their logic.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "WorldIntelligence.h"
#include "llloot/ContainerService.h"
#include "llsim/WorldSim.h"

namespace llws {

class MissionWorldHost final : public llloot::ILootAccessPort, public llloot::ILootSoundSink, public llsim::IWorldInfrastructurePort {
public:
    MissionWorldHost(llworld::WorldIntelligence& wi, llsim::WorldSimulation& sim, llloot::ContainerService& loot, const ais::AisConfig& aisCfg,
                     llenv::IPlayerInventory* inventory);

    // Checks every power node / hazard / world entity referenced by content exists in this map.
    std::vector<std::string> ValidateBindings() const;

    // One server tick: environment first, then world simulation, then containers.
    void Step();
    llworld::Tick Now() const { return wi_.CurrentTick(); }

    // Players: forwarded to the facility, containers and world-simulation relevance.
    bool ConnectPlayer(llworld::PlayerId id, std::uint64_t recipientKey, const llworld::Vec3& position, std::uint32_t permissions = 0xFFFFFFFFu);
    void MovePlayer(llworld::PlayerId id, const llworld::Vec3& position);
    // Releases the player from the facility (channels), containers (claims) and relevance.
    void DisconnectPlayer(llworld::PlayerId id);

    // ILootAccessPort
    bool IsPowered(std::string_view node) const override;
    bool HasItem(llworld::PlayerId player, std::string_view item) const override;
    bool HasClearance(llworld::PlayerId player, std::string_view tag) const override;
    bool IsSealed(std::string_view worldEntity) const override;
    // ILootSoundSink: searches become AIS stimuli and world-simulation disturbances.
    void OnLootSound(const llloot::LootSound& s) override;
    // IWorldInfrastructurePort
    llsim::InfraSample SampleNode(std::string_view node) const override;
    bool HazardActive(std::string_view hazard) const override;

    std::uint64_t LootSoundsForwarded() const { return soundsForwarded_; }
    std::uint64_t SignalsForwarded() const { return signalsForwarded_; }
    ais::EventId LastLootAcousticEvent() const { return lastLootEvent_; }

private:
    struct Watch {
        std::string node;
        llenv::NodeIndex index = llenv::kInvalidNode;
        bool powered = false;
        bool running = false;
        bool alarm = false;
    };
    void TranslateEvents(std::size_t from);
    void PollWatches();
    void AddWatch(const std::string& node);
    void PushPlayers();

    llworld::WorldIntelligence& wi_;
    llsim::WorldSimulation& sim_;
    llloot::ContainerService& loot_;
    const ais::AisConfig& aisCfg_;
    llenv::IPlayerInventory* inv_;
    std::vector<Watch> watches_;
    std::vector<std::pair<llworld::PlayerId, llworld::Vec3>> players_;
    std::uint64_t soundsForwarded_ = 0;
    std::uint64_t signalsForwarded_ = 0;
    std::size_t consumed_ = 0;  // bus events already translated this tick
    ais::EventId lastLootEvent_ = 0;
    int weatherSevere_ = -1;  // -1 unknown; weather signals are edge-triggered
};

}  // namespace llws
