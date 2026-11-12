// LAST LIGHT - Acoustic Intelligence System (AIS)
// Server-side deployable acoustic decoys. Decoys emit ordinary server events
// through the bus; robots judge them with the same imperfect hearing as any
// other sound.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/AcousticEventBus.h"

namespace ais {

// Decoy entity ids live in their own range so they never collide with
// gameplay actor ids handed to the bus.
inline constexpr EntityId kDecoyIdBase = 0xD0000000u;

std::vector<DecoyDefinition> MakeDefaultDecoys();

class DecoySystem {
public:
    explicit DecoySystem(const AisConfig& config, std::size_t maxActive = 32)
        : config_(config), maxActive_(maxActive) {}

    // Returns kNoEntity if the definition is invalid or the active cap is hit.
    EntityId Deploy(const DecoyDefinition& definition, const Vec3& position, EntityId owner,
                    Tick now);
    EntityId Deploy(std::string_view definitionName, const Vec3& position, EntityId owner, Tick now);
    bool Destroy(EntityId decoyId);

    // Emits due events into the bus and retires expired decoys.
    void Update(Tick now, AcousticEventBus& bus);

    std::size_t ActiveCount() const { return active_.size(); }
    bool IsActive(EntityId decoyId) const;
    std::uint64_t TotalEmissions() const { return totalEmissions_; }

private:
    struct ActiveDecoy {
        EntityId id = kNoEntity;
        EntityId owner = kNoEntity;
        DecoyDefinition def;
        Vec3 position;
        Tick deployTick = 0;
        Tick nextEmitTick = 0;
        std::uint32_t emissions = 0;
    };
    Tick NextInterval(const ActiveDecoy& d) const;

    const AisConfig& config_;
    std::size_t maxActive_;
    std::vector<ActiveDecoy> active_;
    EntityId nextId_ = kDecoyIdBase + 1;
    std::uint64_t totalEmissions_ = 0;
};

}  // namespace ais
