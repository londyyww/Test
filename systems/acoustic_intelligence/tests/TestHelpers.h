// Shared scenario helpers for AIS tests and the example simulation.
#pragma once

#include <memory>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/AcousticPropagation.h"
#include "ais/AcousticWorld.h"

namespace aistest {

using namespace ais;

// Owns config + environment + world with stable addresses.
struct Sim {
    AisConfig cfg;
    BoxAcousticEnvironment env;
    std::unique_ptr<AcousticWorld> world;

    explicit Sim(AisConfig config = AisConfig::MakeDefault(), AuthorityRole role = AuthorityRole::Server)
        : cfg(std::move(config)) {
        world = std::make_unique<AcousticWorld>(cfg, env, role);
    }
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    SubmitResult Emit(StimulusCategory c, const Vec3& p, EntityId source = 101,
                      SurfaceType surface = SurfaceType::None, double loudnessOffsetDb = 0.0) {
        AcousticEvent e = MakeEvent(cfg, c, p, world->CurrentTick(), surface, source);
        e.loudnessDb += loudnessOffsetDb;
        return world->SubmitEvent(e, EventOrigin::ServerAuthoritative);
    }

    // Kinematic stand-in for Unreal pathfinding: walk straight to the target.
    void MoveRobots(double speedCmPerTick) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const AcousticDirective* d = world->Directive(ids[i]);
            RobotHostState h = *world->HostState(ids[i]);
            if (d->hasMoveTarget) {
                const Vec3 delta = d->moveTarget - h.position;
                const double len = delta.Length();
                h.position = len <= speedCmPerTick ? d->moveTarget : h.position + delta * (speedCmPerTick / len);
            }
            world->SetHostState(ids[i], h);
        }
    }

    void Run(int ticks, double speed = 0.0) {
        for (int i = 0; i < ticks; ++i) {
            world->Step();
            if (speed > 0.0) MoveRobots(speed);
        }
    }

    bool AddRobot(EntityId id, const char* profile, const Vec3& p, double objectivePriority = 0.0) {
        if (!world->AddRobot(id, profile, p)) return false;
        RobotHostState h;
        h.position = p;
        h.objectivePriority = objectivePriority;
        world->SetHostState(id, h);
        ids.push_back(id);
        return true;
    }

    AwarenessState State(EntityId id) const { return world->Brain(id)->State(); }

    std::vector<EntityId> ids;
};

}  // namespace aistest
