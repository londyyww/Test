// LAST LIGHT - Environmental Systems -> Acoustic Intelligence bridge.
#include "AisAcousticBridge.h"

#include <algorithm>

namespace llenv {

AisAcousticBridge::AisAcousticBridge(const ais::AisConfig& config, ais::AcousticWorld& world,
                                     ais::BoxAcousticEnvironment* ambientEnvironment, ais::EntityId sourceIdBase)
    : config_(config), world_(world), env_(ambientEnvironment), sourceIdBase_(sourceIdBase) {}

ais::StimulusCategory AisAcousticBridge::MapCategory(SoundKind kind) {
    using C = ais::StimulusCategory;
    switch (kind) {
        case SoundKind::GeneratorStart:
        case SoundKind::GeneratorRunning: return C::Generator;
        case SoundKind::GeneratorFailure: return C::MetalImpact;
        case SoundKind::MachineryRunning:
        case SoundKind::CommBurst: return C::Machinery;
        case SoundKind::DoorMotor: return C::Door;
        case SoundKind::DoorForced: return C::MetalImpact;
        case SoundKind::BreakerTrip: return C::BreakObject;
        case SoundKind::AlarmPulse:
        case SoundKind::ExtractionBeacon: return C::Alarm;
    }
    return C::Environmental;
}

void AisAcousticBridge::OnSound(const EnvSoundEvent& e) {
    const ais::StimulusCategory cat = MapCategory(e.kind);
    // Facility sound sources get their own id range so AIS dedup and rate
    // limiting treat each machine as one emitter.
    ais::AcousticEvent ev = ais::MakeEvent(config_, cat, ais::Vec3{e.position.x, e.position.y, e.position.z},
                                           world_.CurrentTick(), ais::SurfaceType::None, sourceIdBase_ + e.node);
    ev.loudnessDb = e.loudnessDb;
    ev.durationTicks = std::clamp<ais::Tick>(e.durationTicks, 1, config_.bus.maxDurationTicks);
    ev.artificiality = 0.0;  // real infrastructure, not a decoy
    ++submitted_;
    if (ais::IsAccepted(world_.SubmitEvent(ev, ais::EventOrigin::ServerAuthoritative))) ++accepted_;
}

void AisAcousticBridge::OnContinuousSound(NodeIndex node, const Vec3& position, double loudnessDb, bool active) {
    ambient_.erase(std::remove_if(ambient_.begin(), ambient_.end(), [node](const Entry& x) { return x.node == node; }),
                   ambient_.end());
    if (active) {
        ais::AmbientSource src;
        src.position = ais::Vec3{position.x, position.y, position.z};
        src.loudnessDb = loudnessDb;
        src.spectrumDb = config_.categories[ais::Index(ais::StimulusCategory::Generator)].spectrumDb;
        ambient_.push_back(Entry{node, src});
        std::sort(ambient_.begin(), ambient_.end(), [](const Entry& a, const Entry& b) { return a.node < b.node; });
    }
    RebuildAmbient();
}

void AisAcousticBridge::AddStaticAmbient(const ais::AmbientSource& source) {
    static_.push_back(source);
    RebuildAmbient();
}

void AisAcousticBridge::RebuildAmbient() {
    if (env_ == nullptr) return;
    env_->ClearAmbient();
    for (const auto& s : static_) env_->AddAmbient(s);
    for (const auto& e : ambient_) env_->AddAmbient(e.source);
}

}  // namespace llenv
