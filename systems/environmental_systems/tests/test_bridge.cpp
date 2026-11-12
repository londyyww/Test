// Integration with the Acoustic Intelligence System through AisAcousticBridge.
#include "AisAcousticBridge.h"
#include "EnvTestHelpers.h"
#include "TestFramework.h"
#include "ais/AcousticWorld.h"

using namespace llenv;
using envtest::Header;
using envtest::Node;
using A = InteractionAction;
using R = InteractionResult;

namespace {

struct Stack {
    ais::AisConfig aisCfg = ais::AisConfig::MakeDefault();
    ais::BoxAcousticEnvironment env;
    std::unique_ptr<ais::AcousticWorld> world;
    std::unique_ptr<AisAcousticBridge> bridge;
    FacilityDef def;
    std::unique_ptr<Facility> f;
    envtest::FakeInventory inv;
    std::uint32_t seq = 0;

    explicit Stack(const std::string& ini) {
        world = std::make_unique<ais::AcousticWorld>(aisCfg, env);
        bridge = std::make_unique<AisAcousticBridge>(aisCfg, *world, &env);
        LoadFacilityIni(ini, def);
        FacilityConfig c;
        c.requestBurst = 1000.0;
        f = std::make_unique<Facility>(def, AuthorityRole::Server, c);
        f->SetAcousticSink(bridge.get());
        f->SetInventory(&inv);
    }
    void AddRobot(ais::EntityId id, const char* profile, const ais::Vec3& p) {
        world->AddRobot(id, profile, p);
        ais::RobotHostState h;
        h.position = p;
        world->SetHostState(id, h);
    }
    InteractionResult Do(PlayerId p, const char* node, A a) {
        InteractionRequest q;
        q.player = p;
        q.sequence = ++seq;
        q.node = f->Find(node);
        q.action = a;
        return f->Submit(q);
    }
    void Run(int n) {
        for (int i = 0; i < n; ++i) {
            f->Step();
            world->Step();
        }
    }
};

}  // namespace

AIS_TEST(Bridge_SoundKindsMapToAcousticCategories) {
    CHECK(AisAcousticBridge::MapCategory(SoundKind::AlarmPulse) == ais::StimulusCategory::Alarm);
    CHECK(AisAcousticBridge::MapCategory(SoundKind::GeneratorStart) == ais::StimulusCategory::Generator);
    CHECK(AisAcousticBridge::MapCategory(SoundKind::DoorForced) == ais::StimulusCategory::MetalImpact);
    CHECK(AisAcousticBridge::MapCategory(SoundKind::DoorMotor) == ais::StimulusCategory::Door);
}

AIS_TEST(Bridge_GeneratorSignatureAttractsAWraithAndMasksFootsteps) {
    const std::string ini = Header() +
                            Node("G", "Generator", "A", "", "capacity_kw = 10\nstartup_ticks = 30\nnoise_db = 78\nnoise_pulse_ticks = 60\n", "0, 0, 0") +
                            Node("L", "Light", "A", "G", "demand_kw = 1\n", "0, 0, 0");
    Stack s(ini);
    s.AddRobot(1, "WRAITH", {2500, 0, 0});
    s.AddRobot(2, "SENTINEL", {300, 200, 0});
    s.f->ConnectPlayer(1, {0, 0, 0}, 0);
    s.Run(1);
    // Before: a footstep near the Sentinel is heard.
    std::vector<ais::PerceptionRecord> seen;
    s.world->SetPerceptionObserver(&seen);
    ais::AcousticEvent step = ais::MakeEvent(s.aisCfg, ais::StimulusCategory::Footstep, {700, 200, 0}, s.world->CurrentTick(), ais::SurfaceType::None, 500);
    s.world->SubmitEvent(step, ais::EventOrigin::ServerAuthoritative);
    s.Run(1);
    bool sentinelHeardBefore = false;
    for (const auto& p : seen) sentinelHeardBefore = sentinelHeardBefore || p.robotId == 2;
    CHECK(sentinelHeardBefore);
    seen.clear();

    CHECK(s.Do(1, "G", A::Activate) == R::Applied);
    s.Run(400);
    CHECK(s.bridge->Accepted() >= 5);  // start + running pulses
    CHECK_EQ(s.bridge->ActiveAmbientCount(), std::size_t{1});
    const ais::RobotAcousticBrain* wraith = s.world->Brain(1);
    bool heardGenerator = false;
    for (const auto& t : wraith->Traces()) heardGenerator = heardGenerator || t.category == ais::StimulusCategory::Generator;
    CHECK(heardGenerator);
    CHECK(wraith->State() != ais::AwarenessState::Unaware);

    // Running generator masks the same footstep for the nearby Sentinel.
    seen.clear();
    step.emitTick = s.world->CurrentTick();
    step.sourceId = 501;
    s.world->SubmitEvent(step, ais::EventOrigin::ServerAuthoritative);
    s.world->Step();
    bool sentinelHeardStep = false;
    for (const auto& p : seen) sentinelHeardStep = sentinelHeardStep || (p.robotId == 2 && p.eventId != 0 &&
                                                                         p.perceived.apparentCategory == ais::StimulusCategory::Footstep);
    CHECK(!sentinelHeardStep);
    s.world->SetPerceptionObserver(nullptr);

    CHECK(s.Do(1, "G", A::Deactivate) == R::Applied);
    CHECK_EQ(s.bridge->ActiveAmbientCount(), std::size_t{0});
}

AIS_TEST(Bridge_AlarmPullsRobotsIntoInvestigation) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 10\nstarts_running = true\n") +
                            Node("ALARM", "Alarm", "A", "G", "demand_kw = 1\nalarm_interval_ticks = 45\nnoise_db = 98\n", "0, 0, 400");
    Stack s(ini);
    s.AddRobot(1, "SENTINEL", {3000, 0, 0});
    s.AddRobot(2, "SENTINEL", {-2500, 1000, 0});
    s.Run(1);
    CHECK(s.f->ServerTriggerAlarm(s.f->Find("ALARM")));
    bool investigating1 = false, investigating2 = false;
    for (int t = 0; t < 300; ++t) {
        s.Run(1);
        investigating1 = investigating1 || s.world->Brain(1)->State() == ais::AwarenessState::Investigating;
        investigating2 = investigating2 || s.world->Brain(2)->State() == ais::AwarenessState::Investigating;
        CHECK(s.world->Brain(1)->State() != ais::AwarenessState::Combat);  // an alarm is not a confirmed enemy
    }
    CHECK(investigating1);
    CHECK(investigating2);
}
