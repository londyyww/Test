// LAST LIGHT - Environmental Systems -> Acoustic Intelligence bridge.
// The only code that knows both modules. The facility emits llenv sound
// events; this adapter turns them into server-authoritative AIS events and
// keeps AIS ambient masking in sync with running generators/machinery.
#pragma once

#include <cstdint>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/AcousticPropagation.h"
#include "ais/AcousticWorld.h"
#include "llenv/Ports.h"

namespace llenv {

class AisAcousticBridge final : public IAcousticSink {
public:
    // `ambientEnvironment` may be null; in Unreal the IAcousticEnvironment
    // implementation should expose an equivalent ambient registry.
    AisAcousticBridge(const ais::AisConfig& config, ais::AcousticWorld& world,
                      ais::BoxAcousticEnvironment* ambientEnvironment,
                      ais::EntityId sourceIdBase = 0xE0000000u);

    void OnSound(const EnvSoundEvent& e) override;
    void OnContinuousSound(NodeIndex node, const Vec3& position, double loudnessDb, bool active) override;

    // Level ambience that is not owned by the facility (rivers, wind turbines).
    void AddStaticAmbient(const ais::AmbientSource& source);

    static ais::StimulusCategory MapCategory(SoundKind kind);

    std::uint64_t Submitted() const { return submitted_; }
    std::uint64_t Accepted() const { return accepted_; }
    std::size_t ActiveAmbientCount() const { return ambient_.size(); }

private:
    void RebuildAmbient();

    const ais::AisConfig& config_;
    ais::AcousticWorld& world_;
    ais::BoxAcousticEnvironment* env_;
    ais::EntityId sourceIdBase_;
    struct Entry {
        NodeIndex node;
        ais::AmbientSource source;
    };
    std::vector<Entry> ambient_;
    std::vector<ais::AmbientSource> static_;
    std::uint64_t submitted_ = 0;
    std::uint64_t accepted_ = 0;
};

}  // namespace llenv
