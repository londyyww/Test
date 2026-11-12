// Propagation: distance, occlusion, materials, interior/exterior, weather, masking.
#include <cmath>

#include "TestFramework.h"
#include "TestHelpers.h"

using namespace ais;

namespace {

AcousticEvent Ev(const AisConfig& cfg, StimulusCategory c, const Vec3& p) { return MakeEvent(cfg, c, p, 0); }
constexpr std::size_t kLow = 0, kMid = 1, kHigh = 2;

}  // namespace

AIS_TEST(Propagation_DistanceAttenuationFollowsInverseSquare) {
    const AisConfig cfg = AisConfig::MakeDefault();
    BoxAcousticEnvironment env;
    AcousticPropagationModel model(cfg, env);
    const AcousticEvent e = Ev(cfg, StimulusCategory::Gunfire, {0, 0, 0});
    const auto r1 = model.Evaluate(e, {100, 0, 0});
    const auto r10 = model.Evaluate(e, {1000, 0, 0});
    const auto r100 = model.Evaluate(e, {10000, 0, 0});
    // 20 dB per decade plus small air absorption (mid: 3 dB / 100 m).
    CHECK_NEAR(r1.receivedDb[kMid], e.loudnessDb, 0.05);
    CHECK_NEAR(r1.receivedDb[kMid] - r10.receivedDb[kMid], 20.0 + 0.27, 0.05);
    CHECK_NEAR(r10.receivedDb[kMid] - r100.receivedDb[kMid], 20.0 + 2.7, 0.05);
    // Air absorbs highs more than lows.
    CHECK(r1.receivedDb[kHigh] - r100.receivedDb[kHigh] > r1.receivedDb[kLow] - r100.receivedDb[kLow]);
    CHECK_EQ(r100.occluderCount, std::size_t{0});
}

AIS_TEST(Propagation_ZeroDistanceAndInsideReferenceAreFinite) {
    const AisConfig cfg = AisConfig::MakeDefault();
    BoxAcousticEnvironment env;
    AcousticPropagationModel model(cfg, env);
    const AcousticEvent e = Ev(cfg, StimulusCategory::Footstep, {5, 5, 5});
    const auto r0 = model.Evaluate(e, {5, 5, 5});
    const auto rHalf = model.Evaluate(e, {55, 5, 5});
    for (std::size_t b = 0; b < kBandCount; ++b) {
        CHECK(std::isfinite(r0.receivedDb[b]));
        CHECK_NEAR(r0.receivedDb[b], rHalf.receivedDb[b], 0.05);  // spreading clamped; only air absorption differs
    }
}

AIS_TEST(Propagation_WallOccludesAndLowFrequencyLeaksThrough) {
    const AisConfig cfg = AisConfig::MakeDefault();
    BoxAcousticEnvironment open;
    BoxAcousticEnvironment walled;
    walled.AddWall({{500, -1000, -500}, {530, 1000, 500}}, static_cast<MaterialId>(cfg.FindMaterial("Concrete")));
    AcousticPropagationModel a(cfg, open);
    AcousticPropagationModel b(cfg, walled);
    const AcousticEvent e = Ev(cfg, StimulusCategory::Gunfire, {0, 0, 0});
    const auto ro = a.Evaluate(e, {2000, 0, 0});
    const auto rw = b.Evaluate(e, {2000, 0, 0});
    CHECK_EQ(rw.occluderCount, std::size_t{1});
    const MaterialDescriptor& concrete = cfg.materials[static_cast<std::size_t>(cfg.FindMaterial("Concrete"))];
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const double expected = concrete.lossDb[band] + concrete.lossDbPerMeter[band] * 0.30;
        CHECK_NEAR(ro.receivedDb[band] - rw.receivedDb[band], expected, 0.01);
    }
    CHECK(ro.receivedDb[kLow] - rw.receivedDb[kLow] < ro.receivedDb[kHigh] - rw.receivedDb[kHigh]);
    // A segment that does not cross the wall is unaffected.
    const auto side = b.Evaluate(e, {0, 2000, 0});
    CHECK_EQ(side.occluderCount, std::size_t{0});
}

AIS_TEST(Propagation_MaterialTransmissionOrderingThicknessAndCap) {
    AisConfig cfg = AisConfig::MakeDefault();
    const AcousticEvent e = Ev(cfg, StimulusCategory::Gunfire, {0, 0, 0});
    auto lossThrough = [&](const char* material, double thickness) {
        BoxAcousticEnvironment env;
        env.AddWall({{500, -1000, -500}, {500 + thickness, 1000, 500}}, static_cast<MaterialId>(cfg.FindMaterial(material)));
        BoxAcousticEnvironment open;
        AcousticPropagationModel m(cfg, env);
        AcousticPropagationModel o(cfg, open);
        return o.Evaluate(e, {2000, 0, 0}).receivedDb[kMid] - m.Evaluate(e, {2000, 0, 0}).receivedDb[kMid];
    };
    const double foliage = lossThrough("Foliage", 100);
    const double wood = lossThrough("Wood", 5);
    const double concrete = lossThrough("Concrete", 30);
    CHECK(foliage < wood);
    CHECK(wood < concrete);
    CHECK(lossThrough("Concrete", 60) > concrete);
    // Stacked walls are capped so that sound never becomes -infinity.
    BoxAcousticEnvironment bunker;
    for (int i = 0; i < 6; ++i) {
        const double x = 300.0 + 200.0 * i;
        bunker.AddWall({{x, -1000, -500}, {x + 50, 1000, 500}}, static_cast<MaterialId>(cfg.FindMaterial("Concrete")));
    }
    AcousticPropagationModel m(cfg, bunker);
    BoxAcousticEnvironment open;
    AcousticPropagationModel o(cfg, open);
    const auto rb = m.Evaluate(e, {2000, 0, 0});
    CHECK_EQ(rb.occluderCount, std::size_t{6});
    CHECK_NEAR(o.Evaluate(e, {2000, 0, 0}).receivedDb[kMid] - rb.receivedDb[kMid], cfg.propagation.maxOcclusionLossDb, 0.01);
    // Unknown material ids fall back to Default instead of reading out of range.
    BoxAcousticEnvironment bad;
    bad.AddWall({{500, -1000, -500}, {530, 1000, 500}}, MaterialId{999});
    AcousticPropagationModel mb(cfg, bad);
    CHECK(std::isfinite(mb.Evaluate(e, {2000, 0, 0}).receivedDb[kMid]));
}

AIS_TEST(Propagation_InteriorExteriorTransitionAndRoomSpreading) {
    const AisConfig cfg = AisConfig::MakeDefault();
    BoxAcousticEnvironment env;
    env.AddZone({{0, 0, -100}, {2000, 2000, 400}}, 7, true);
    AcousticPropagationModel m(cfg, env);
    const AcousticEvent outside = Ev(cfg, StimulusCategory::Door, {-1000, 1000, 0});
    const auto cross = m.Evaluate(outside, {0 + 1000, 1000, 0});
    CHECK(cross.crossesInteriorBoundary);
    CHECK(cross.listenerInterior);
    CHECK(!cross.sourceInterior);
    BoxAcousticEnvironment flat;
    AcousticPropagationModel f(cfg, flat);
    CHECK_NEAR(f.Evaluate(outside, {1000, 1000, 0}).receivedDb[kMid] - cross.receivedDb[kMid],
               cfg.propagation.interiorExteriorTransitionDb, 0.01);
    // Same room: lower spreading factor -> louder than in the open.
    const AcousticEvent inside = Ev(cfg, StimulusCategory::Door, {200, 200, 0});
    const auto room = m.Evaluate(inside, {1800, 1800, 0});
    CHECK(!room.crossesInteriorBoundary);
    CHECK(room.receivedDb[kMid] > f.Evaluate(inside, {1800, 1800, 0}).receivedDb[kMid]);
    // Interior floor is quieter than exterior floor.
    CHECK(room.noiseFloorDb[kMid] < f.Evaluate(inside, {1800, 1800, 0}).noiseFloorDb[kMid]);
}

AIS_TEST(Propagation_RainAndWindRaiseNoiseFloorAndShelterHelps) {
    const AisConfig cfg = AisConfig::MakeDefault();
    BoxAcousticEnvironment env;
    env.AddZone({{5000, -500, -100}, {6000, 500, 400}}, 3, true);
    AcousticPropagationModel m(cfg, env);
    const BandArray clear = m.NoiseFloorAt({0, 0, 0}, false);
    env.SetWeather({1.0, 0.0});
    const BandArray rain = m.NoiseFloorAt({0, 0, 0}, false);
    const BandArray rainIndoor = m.NoiseFloorAt({5500, 0, 0}, true);
    env.SetWeather({0.0, 1.0});
    const BandArray wind = m.NoiseFloorAt({0, 0, 0}, false);
    CHECK(rain[kHigh] > clear[kHigh] + 20.0);  // rain hiss masks highs
    CHECK(wind[kLow] > clear[kLow] + 15.0);    // wind rumble masks lows
    CHECK(rainIndoor[kHigh] < rain[kHigh]);
    // Rain also attenuates exposed long-range paths slightly.
    env.SetWeather({1.0, 0.0});
    const AcousticEvent e = Ev(cfg, StimulusCategory::Gunfire, {0, 0, 0});
    const double wet = m.Evaluate(e, {20000, 0, 0}).receivedDb[kHigh];
    env.SetWeather({0.0, 0.0});
    CHECK(m.Evaluate(e, {20000, 0, 0}).receivedDb[kHigh] > wet);
}

AIS_TEST(Propagation_SurfaceModifiersAndAudibleRadius) {
    const AisConfig cfg = AisConfig::MakeDefault();
    BoxAcousticEnvironment env;
    AcousticPropagationModel m(cfg, env);
    AcousticEvent metal = MakeEvent(cfg, StimulusCategory::Footstep, {0, 0, 0}, 0, SurfaceType::Metal);
    AcousticEvent grass = MakeEvent(cfg, StimulusCategory::Footstep, {0, 0, 0}, 0, SurfaceType::Grass);
    CHECK(m.SourceBandLevels(metal)[kHigh] > m.SourceBandLevels(grass)[kHigh] + 10.0);
    const double footR = m.MaxAudibleRadiusCm(grass, BandArray{0.0, 0.0, 0.0});
    const double shotR = m.MaxAudibleRadiusCm(MakeEvent(cfg, StimulusCategory::Gunfire, {0, 0, 0}, 0), BandArray{0.0, 0.0, 0.0});
    CHECK(footR < shotR);
    CHECK(shotR <= cfg.propagation.maxAudibleRadiusCm);
    // Broadphase must never cull a listener that could actually hear the event.
    const double r = m.MaxAudibleRadiusCm(grass, BandArray{10.0, 10.0, 10.0});
    const auto atEdge = m.Evaluate(grass, {r * 1.01, 0, 0});
    for (std::size_t b = 0; b < kBandCount; ++b) CHECK(atEdge.receivedDb[b] < 10.0 + 1e-9);
}

AIS_TEST(Propagation_AmbientMachineryMasksFootsteps) {
    aistest::Sim quiet;
    aistest::Sim noisy;
    noisy.env.AddAmbient({{0, 200, 0}, 85.0, {6.0, 2.0, -6.0}});
    for (aistest::Sim* s : {&quiet, &noisy}) {
        s->AddRobot(1, "SENTINEL", {0, 0, 0});
        std::vector<PerceptionRecord> seen;
        s->world->SetPerceptionObserver(&seen);
        s->Emit(StimulusCategory::Footstep, {600, 0, 0});
        s->Run(3);
        CHECK_EQ(seen.size(), s == &quiet ? std::size_t{1} : std::size_t{0});
        s->world->SetPerceptionObserver(nullptr);
    }
}

AIS_TEST(Propagation_HeavyRainHidesFootstepsButNotGunfire) {
    aistest::Sim s;
    s.env.SetWeather({1.0, 0.3});
    s.AddRobot(1, "SENTINEL", {0, 0, 0});
    std::vector<PerceptionRecord> seen;
    s.world->SetPerceptionObserver(&seen);
    s.Emit(StimulusCategory::Footstep, {800, 0, 0});
    s.Run(2);
    CHECK(seen.empty());
    s.Emit(StimulusCategory::Gunfire, {8000, 0, 0}, 102);
    s.Run(2);
    CHECK_EQ(seen.size(), std::size_t{1});
}

AIS_TEST(Localization_EstimatesAreNoisyBoundedAndWorseWithDistanceAndWalls) {
    aistest::Sim s;
    s.AddRobot(1, "SENTINEL", {0, 0, 0});
    std::vector<PerceptionRecord> seen;
    s.world->SetPerceptionObserver(&seen);
    double nearErr = 0.0, farErr = 0.0;
    for (int i = 0; i < 20; ++i) {
        s.Emit(StimulusCategory::Gunfire, {1000, 0, 0}, 200u + static_cast<EntityId>(i));
        s.Emit(StimulusCategory::Gunfire, {6000, 3000, 0}, 300u + static_cast<EntityId>(i));
        s.Run(1);
    }
    CHECK_EQ(seen.size(), std::size_t{40});
    for (const auto& p : seen) {
        const double err = Distance(p.perceived.estimatedPosition, p.truePosition);
        CHECK(err > 0.0);  // never exact
        CHECK(err <= p.perceived.uncertaintyCm + 1e-6);
        (p.truePosition.x < 2000 ? nearErr : farErr) += p.perceived.uncertaintyCm;
    }
    CHECK(farErr > nearErr * 3.0);

    // Same geometry with a wall: larger uncertainty.
    aistest::Sim w;
    w.env.AddWall({{500, -1000, -500}, {520, 1000, 500}}, static_cast<MaterialId>(w.cfg.FindMaterial("Wood")));
    w.AddRobot(1, "SENTINEL", {0, 0, 0});
    std::vector<PerceptionRecord> seenW;
    w.world->SetPerceptionObserver(&seenW);
    w.Emit(StimulusCategory::Gunfire, {1000, 0, 0});
    w.Run(1);
    CHECK_EQ(seenW.size(), std::size_t{1});
    CHECK(!seenW.empty() && seenW[0].perceived.uncertaintyCm > seen[0].perceived.uncertaintyCm * 1.4);
}
