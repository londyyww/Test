// LAST LIGHT - Acoustic Intelligence System: example simulation.
//
// A player crosses a compound guarded by two SENTINELs and a WRAITH, makes
// noise, gets heard, hides, then throws a RepeatingMetallic decoy. All robot
// movement here is a straight-line stand-in for Unreal pathfinding.
//
// Build: make demo   (prints a timeline of state changes)
#include <cstdio>
#include <string>
#include <vector>

#include "ais/AcousticConfig.h"
#include "ais/AcousticPropagation.h"
#include "ais/AcousticWorld.h"

using namespace ais;

namespace {

struct Bot {
    EntityId id;
    const char* label;
    double objectivePriority;
};

void MoveToward(AcousticWorld& world, EntityId id, double speed) {
    const AcousticDirective* d = world.Directive(id);
    RobotHostState h = *world.HostState(id);
    if (d->hasMoveTarget) {
        const Vec3 delta = d->moveTarget - h.position;
        const double len = delta.Length();
        h.position = len <= speed ? d->moveTarget : h.position + delta * (speed / len);
    }
    world.SetHostState(id, h);
}

}  // namespace

int main(int argc, char** argv) {
    AisConfig cfg = AisConfig::MakeDefault();
    if (argc > 1) {
        const ConfigLoadResult r = LoadConfigFromFile(argv[1], cfg);
        for (const auto& e : r.errors) std::fprintf(stderr, "config: %s\n", e.c_str());
        if (!r.ok) return 1;
    }

    // Compound: a concrete pump house (interior zone) east of an open yard.
    BoxAcousticEnvironment env;
    const auto concrete = static_cast<MaterialId>(cfg.FindMaterial("Concrete"));
    env.AddWall({{3000, -1500, -100}, {3030, 1500, 500}}, concrete);   // west wall
    env.AddWall({{6000, -1500, -100}, {6030, 1500, 500}}, concrete);   // east wall
    env.AddZone({{3030, -1500, -100}, {6000, 1500, 500}}, 1, true);
    env.AddAmbient({{4500, 0, 0}, 72.0, {6.0, 0.0, -8.0}});           // pump generator inside

    AcousticWorld world(cfg, env);
    const std::vector<Bot> bots{{1, "SENTINEL-A (patrol)", 0.0}, {2, "SENTINEL-B (guards gate)", 60.0}, {3, "WRAITH", 0.0}};
    world.AddRobot(1, "SENTINEL", {0, 0, 0});
    world.AddRobot(2, "SENTINEL", {-2500, 1500, 0});
    world.AddRobot(3, "WRAITH", {4500, 800, 0});  // inside the pump house
    const Vec3 starts[] = {{0, 0, 0}, {-2500, 1500, 0}, {4500, 800, 0}};
    for (std::size_t i = 0; i < bots.size(); ++i) {
        RobotHostState h;
        h.position = starts[i];
        h.objectivePriority = bots[i].objectivePriority;
        world.SetHostState(bots[i].id, h);
    }
    world.SetClientListeners({{1, {-1500, -2500, 0}}});

    const EntityId player = 1001;
    Vec3 playerPos{-1500, -2500, 0};
    std::vector<AwarenessState> last(bots.size(), AwarenessState::Unaware);
    std::size_t cosmetic = 0;

    auto note = [&](const char* what) { std::printf("t=%5.1fs  ** %s\n", static_cast<double>(world.CurrentTick()) / 30.0, what); };
    auto emit = [&](StimulusCategory c, const Vec3& p, SurfaceType s = SurfaceType::None, double offset = 0.0) {
        AcousticEvent e = MakeEvent(cfg, c, p, world.CurrentTick(), s, player);
        e.loudnessDb += offset;
        world.SubmitEvent(e, EventOrigin::ServerAuthoritative);
    };

    for (int t = 0; t < 30 * 75; ++t) {
        // --- scripted player -----------------------------------------------
        if (t == 0) note("player crouch-walks along the south fence (quiet, grass)");
        if (t < 300 && t % 18 == 0) {
            playerPos = playerPos + Vec3{40, 0, 0};
            emit(StimulusCategory::Footstep, playerPos, SurfaceType::Grass, -8.0);
        }
        if (t == 300) note("player sprints across sheet-metal ramp (mistake)");
        if (t >= 300 && t < 390 && t % 9 == 0) {
            playerPos = playerPos + Vec3{60, 40, 0};
            emit(StimulusCategory::Sprint, playerPos, SurfaceType::Metal);
        }
        if (t == 390) note("player kicks open a door and ducks behind a container, silent");
        if (t == 390) emit(StimulusCategory::Door, playerPos);
        if (t == 900) {
            note("player throws RepeatingMetallic decoy far west of the hiding spot");
            world.Decoys().Deploy("RepeatingMetallic", {-4000, -500, 0}, player, world.CurrentTick());
        }
        if (t == 1500) {
            note("rain starts; player fires one suppressed shot at a lock 35 m from SENTINEL-B");
            env.SetWeather({0.9, 0.4});
            emit(StimulusCategory::Gunfire, {-2500, -2000, 0}, SurfaceType::None, -25.0);
        }

        world.Step();
        cosmetic += world.CosmeticCuesThisTick().size();
        for (std::size_t i = 0; i < bots.size(); ++i) MoveToward(world, bots[i].id, 14.0);

        for (std::size_t i = 0; i < bots.size(); ++i) {
            const RobotAcousticBrain* b = world.Brain(bots[i].id);
            if (b->State() == last[i]) continue;
            last[i] = b->State();
            const AcousticDirective& d = b->Directive();
            std::printf("t=%5.1fs  %-26s -> %-17s suspicion=%5.1f reason=%-18s", static_cast<double>(world.CurrentTick()) / 30.0,
                        bots[i].label, ToString(b->State()), b->Suspicion(), ToString(d.reason));
            if (d.hasMoveTarget) std::printf(" goto=(%.0f,%.0f) +/-%.0fcm", d.moveTarget.x, d.moveTarget.y, d.targetUncertaintyCm);
            std::printf("\n");
        }
    }

    std::printf("\nFinal memory:\n");
    for (const Bot& bot : bots) {
        for (const AcousticTrace& tr : world.Brain(bot.id)->Traces()) {
            std::printf("  %-26s trace#%u %-12s est=(%.0f,%.0f) +/-%.0fcm conf=%.2f hits=%u%s%s\n", bot.label, tr.traceId,
                        ToString(tr.category), tr.estimate.x, tr.estimate.y, tr.uncertaintyCm, tr.confidence, tr.hitCount,
                        tr.directHits == 0 ? " (second-hand)" : "", tr.suspectedArtificial ? " [judged artificial]" : "");
        }
    }
    const WorldStats& st = world.Stats();
    const BusStats& bs = world.Bus().Stats();
    std::printf("\nStats: events=%llu perception_checks=%llu perceptions=%llu reports=%llu/%llu brain_updates=%llu "
                "cosmetic_cues=%zu accepted=%llu merged=%llu rate_limited=%llu\n",
                static_cast<unsigned long long>(st.eventsProcessed), static_cast<unsigned long long>(st.perceptionChecks),
                static_cast<unsigned long long>(st.perceptionsDelivered), static_cast<unsigned long long>(st.reportsPublished),
                static_cast<unsigned long long>(st.reportsDelivered), static_cast<unsigned long long>(st.brainUpdates), cosmetic,
                static_cast<unsigned long long>(bs.Count(SubmitResult::Accepted)),
                static_cast<unsigned long long>(bs.Count(SubmitResult::MergedDuplicate)),
                static_cast<unsigned long long>(bs.Count(SubmitResult::RejectedRateLimited)));
    std::printf("State hash: %016llx\n", static_cast<unsigned long long>(world.StateHash()));
    return 0;
}
