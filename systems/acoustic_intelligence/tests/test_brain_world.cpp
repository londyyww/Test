// Robot brains, investigation lifecycle, decoys, signal network, scaling, replay, config.
#include <algorithm>
#include <cmath>
#include <string>

#include "TestFramework.h"
#include "TestHelpers.h"

using namespace ais;
using aistest::Sim;

namespace {

PerceivedStimulus Heard(StimulusCategory c, const Vec3& p, Tick t, EventId id, double snr = 20.0, double unc = 200.0) {
    const AisConfig cfg = AisConfig::MakeDefault();
    PerceivedStimulus s;
    s.eventId = id;
    s.apparentCategory = c;
    s.estimatedPosition = p;
    s.uncertaintyCm = unc;
    s.effectiveSnrDb = snr;
    s.threat = cfg.categories[Index(c)].threat;
    s.hostileCategory = cfg.categories[Index(c)].hostile;
    s.regularityIsSuspicious = cfg.categories[Index(c)].regularityIsSuspicious;
    s.tick = t;
    return s;
}

RobotHostState At(const Vec3& p, double priority = 0.0) {
    RobotHostState h;
    h.position = p;
    h.objectivePriority = priority;
    return h;
}

void UpdateBrain(RobotAcousticBrain& b, Tick t, const RobotHostState& h) { b.Update(t, h, nullptr, nullptr); }

}  // namespace

// --- Sensory profiles ---------------------------------------------------------

AIS_TEST(Profiles_SentinelAndWraithHearDifferentBands) {
    // Pure low rumble tuned so raw low-band SNR is 0 dB: Sentinel (+10) hears it,
    // Wraith (+2) does not. Pure hiss at -6 dB: Wraith (+16) hears, Sentinel does not.
    for (int variant = 0; variant < 2; ++variant) {
        Sim s;
        s.AddRobot(1, "SENTINEL", {0, 0, 0});
        s.AddRobot(2, "WRAITH", {0, 0, 0});
        AcousticEvent e = MakeEvent(s.cfg, StimulusCategory::Machinery, {2000, 0, 0}, 0, SurfaceType::None, 9);
        e.spectrumDb = variant == 0 ? BandArray{0.0, -40.0, -40.0} : BandArray{-40.0, -40.0, 0.0};
        const std::size_t band = variant == 0 ? 0 : 2;
        const double targetSnr = variant == 0 ? 0.0 : -6.0;
        AcousticPropagationModel m(s.cfg, s.env);
        e.loudnessDb -= m.Evaluate(e, {0, 0, 0}).snrDb[band] - targetSnr;
        std::vector<PerceptionRecord> seen;
        s.world->SetPerceptionObserver(&seen);
        CHECK(IsAccepted(s.world->SubmitEvent(e, EventOrigin::ServerAuthoritative)));
        s.Run(1);
        CHECK_EQ(seen.size(), std::size_t{1});
        if (!seen.empty()) CHECK_EQ(seen[0].robotId, variant == 0 ? EntityId{1} : EntityId{2});
    }
}

AIS_TEST(Profiles_WraithLocalizesTighterThanSentinel) {
    Sim s;
    s.AddRobot(1, "SENTINEL", {0, 0, 0});
    s.AddRobot(2, "WRAITH", {0, 0, 0});
    std::vector<PerceptionRecord> seen;
    s.world->SetPerceptionObserver(&seen);
    s.Emit(StimulusCategory::Gunfire, {3000, 2000, 0});
    s.Run(1);
    CHECK_EQ(seen.size(), std::size_t{2});
    if (seen.size() == 2) {
        CHECK(seen[1].perceived.uncertaintyCm < seen[0].perceived.uncertaintyCm);
        // Same sound, different robots, different estimates.
        CHECK(Distance(seen[0].perceived.estimatedPosition, seen[1].perceived.estimatedPosition) > 1.0);
    }
}

AIS_TEST(Profiles_DataDrivenThirdArchetypeFromIni) {
    AisConfig cfg = AisConfig::MakeDefault();
    const auto r = LoadConfigFromIni(
        "[robot.WARDEN]\nbase = SENTINEL\nband_sensitivity_db = 14, 6, 0\nmax_investigation_range_cm = 2500\n"
        "objective_commitment = 2.0\ninterest.Explosion = 2.0\n",
        cfg);
    CHECK(r.ok);
    const RobotSensoryProfile* w = cfg.FindProfile("WARDEN");
    CHECK(w != nullptr);
    if (w != nullptr) {
        CHECK_NEAR(w->bandSensitivityDb[0], 14.0, 1e-12);
        CHECK_NEAR(w->repetitionBonus, MakeSentinelProfile().repetitionBonus, 1e-12);  // inherited
        CHECK_NEAR(w->categoryInterest[Index(StimulusCategory::Explosion)], 2.0, 1e-12);
    }
    Sim s(cfg);
    CHECK(s.AddRobot(5, "WARDEN", {0, 0, 0}));
    CHECK(!s.world->AddRobot(6, "NOT_A_ROBOT", {0, 0, 0}));
}

// --- Suspicion, repetition, combat gating -------------------------------------

AIS_TEST(Brain_SingleDistantSoundNeverTriggersCombat) {
    for (double dist : {1500.0, 3000.0, 20000.0}) {
        Sim s;
        s.AddRobot(1, "SENTINEL", {0, 0, 0});
        s.AddRobot(2, "WRAITH", {0, 500, 0});
        s.Emit(StimulusCategory::Gunfire, {dist, 0, 0});
        for (int t = 0; t < 300; ++t) {
            s.world->Step();
            CHECK(s.State(1) != AwarenessState::Combat);
            CHECK(s.State(2) != AwarenessState::Combat);
        }
    }
    // Even a point-blank explosion: one event gain is capped below the combat gap.
    RobotAcousticBrain b(1, MakeSentinelProfile(), 1);
    b.Enqueue(Heard(StimulusCategory::Explosion, {100, 0, 0}, 1, 1, 80.0, 60.0));
    UpdateBrain(b, 1, At({0, 0, 0}));
    CHECK(b.Suspicion() <= MakeSentinelProfile().maxSingleEventGain + 1e-9);
    CHECK(b.State() != AwarenessState::Combat);
}

AIS_TEST(Brain_RepeatedSoundsEscalateAndSentinelSpotsRepetitionFaster) {
    RobotSensoryProfile flat = MakeSentinelProfile();
    flat.repetitionBonus = 0.0;
    RobotAcousticBrain sentinel(1, MakeSentinelProfile(), 1);
    RobotAcousticBrain plain(2, flat, 1);
    const RobotHostState h = At({0, 0, 0});
    sentinel.Enqueue(Heard(StimulusCategory::Footstep, {800, 0, 0}, 1, 1, 12.0));
    plain.Enqueue(Heard(StimulusCategory::Footstep, {800, 0, 0}, 1, 1, 12.0));
    UpdateBrain(sentinel, 1, h);
    UpdateBrain(plain, 1, h);
    const double first = sentinel.Suspicion();
    CHECK(sentinel.State() != AwarenessState::Investigating);  // one footstep is not enough
    for (Tick t = 2; t <= 6; ++t) {
        sentinel.Enqueue(Heard(StimulusCategory::Footstep, {800, 20.0 * static_cast<double>(t), 0}, t * 15, t, 12.0));
        plain.Enqueue(Heard(StimulusCategory::Footstep, {800, 20.0 * static_cast<double>(t), 0}, t * 15, t, 12.0));
        UpdateBrain(sentinel, t * 15, h);
        UpdateBrain(plain, t * 15, h);
    }
    CHECK(sentinel.Suspicion() > first * 3.0);
    CHECK(sentinel.Suspicion() > plain.Suspicion());
    CHECK(sentinel.State() == AwarenessState::Investigating);
    CHECK_EQ(sentinel.Traces().size(), std::size_t{1});  // fused into one disturbance
    CHECK(sentinel.Traces()[0].hitCount == 6u);
    CHECK(sentinel.Traces()[0].uncertaintyCm < 200.0);    // fusion tightened the estimate
}

AIS_TEST(Brain_SustainedNearbyGunfireLeadsToCombat) {
    Sim s;
    s.AddRobot(1, "SENTINEL", {0, 0, 0});
    bool combat = false;
    for (int t = 0; t < 60 && !combat; ++t) {
        if (t % 4 == 0) s.Emit(StimulusCategory::Gunfire, {1500, 0, 0});
        s.world->Step();
        combat = s.State(1) == AwarenessState::Combat;
    }
    CHECK(combat);
    CHECK(s.world->Directive(1)->reason == DirectiveReason::AcousticCombat);
}

AIS_TEST(Brain_VisualContactForcesCombatAndLosingItSearches) {
    RobotAcousticBrain b(1, MakeSentinelProfile(), 1);
    RobotHostState h = At({0, 0, 0});
    h.hasVisualContact = true;
    h.visualContactPosition = {900, 0, 0};
    UpdateBrain(b, 1, h);
    CHECK(b.State() == AwarenessState::Combat);
    CHECK(b.Directive().reason == DirectiveReason::VisualCombat);
    h.hasVisualContact = false;
    const Tick lose = MakeSentinelProfile().combatLoseTicks;
    UpdateBrain(b, 1 + lose, h);
    CHECK(b.State() == AwarenessState::Combat);
    UpdateBrain(b, 2 + lose, h);
    CHECK(b.State() == AwarenessState::Searching);
    CHECK(b.Directive().reason == DirectiveReason::CombatContactLost);
}

// --- Memory -------------------------------------------------------------------

AIS_TEST(Brain_ConfidenceAndSuspicionDecayAndOldTracesAreForgotten) {
    const RobotSensoryProfile p = MakeSentinelProfile();
    RobotAcousticBrain b(1, p, 1);
    b.Enqueue(Heard(StimulusCategory::Door, {600, 0, 0}, 1, 1));
    UpdateBrain(b, 1, At({0, 0, 0}, 100.0));
    CHECK_EQ(b.Traces().size(), std::size_t{1});
    const double c0 = b.Traces()[0].confidence;
    const double s0 = b.Suspicion();
    const double u0 = b.Traces()[0].uncertaintyCm;
    const Tick half = static_cast<Tick>(p.confidenceHalfLifeTicks);
    UpdateBrain(b, 1 + half, At({0, 0, 0}, 100.0));
    CHECK_NEAR(b.Traces()[0].confidence, c0 * 0.5, 1e-9);
    CHECK_NEAR(b.Suspicion(), s0 * std::pow(0.5, p.confidenceHalfLifeTicks / p.suspicionHalfLifeTicks), 1e-9);
    CHECK_NEAR(b.Traces()[0].uncertaintyCm, u0 + p.uncertaintyGrowthCmPerTick * static_cast<double>(half), 1e-9);
    UpdateBrain(b, 1 + half * 10, At({0, 0, 0}, 100.0));
    CHECK(b.Traces().empty());
    CHECK(b.State() == AwarenessState::Unaware);
}

AIS_TEST(Brain_MemoryAndInboxAreBounded) {
    RobotSensoryProfile p = MakeSentinelProfile();
    p.memoryCapacity = 3;
    p.inboxCapacity = 4;
    RobotAcousticBrain b(1, p, 1);
    for (EventId i = 1; i <= 10; ++i) {
        b.Enqueue(Heard(StimulusCategory::Door, {static_cast<double>(i) * 5000, 0, 0}, 1, i, static_cast<double>(i) + 6.0));
    }
    CHECK_EQ(b.InboxSize(), std::size_t{4});
    CHECK_EQ(b.DroppedPerceptions(), std::size_t{6});
    UpdateBrain(b, 1, At({0, 0, 0}));
    CHECK(b.Traces().size() <= 3);
    // The loudest perceptions survived the flood.
    for (const auto& t : b.Traces()) CHECK(t.estimate.x >= 7.0 * 5000 - 1e-9);
}

AIS_TEST(Brain_SimultaneousSoundsKeepSeparateTracesAndPickTheMostCompelling) {
    RobotAcousticBrain b(1, MakeSentinelProfile(), 1);
    b.Enqueue(Heard(StimulusCategory::Footstep, {500, 0, 0}, 1, 1, 10.0));
    b.Enqueue(Heard(StimulusCategory::Gunfire, {0, 2500, 0}, 1, 2, 30.0));
    b.Enqueue(Heard(StimulusCategory::Environmental, {-900, 0, 0}, 1, 3, 30.0));
    UpdateBrain(b, 1, At({0, 0, 0}));
    CHECK_EQ(b.Traces().size(), std::size_t{3});
    CHECK(b.Directive().hasLookAt);
    CHECK(Distance(b.Directive().lookAt, {0, 2500, 0}) < 1.0);
}

// --- Investigation lifecycle -----------------------------------------------------

AIS_TEST(Investigation_TargetUpdatesWhenStrongerEvidenceAppears) {
    RobotAcousticBrain b(1, MakeSentinelProfile(), 1);
    const RobotHostState h = At({0, 0, 0});
    for (Tick t = 1; t <= 4; ++t) b.Enqueue(Heard(StimulusCategory::Footstep, {1000, 0, 0}, t, t, 16.0));
    UpdateBrain(b, 4, h);
    CHECK(b.State() == AwarenessState::Investigating);
    const std::uint32_t first = b.Directive().traceId;
    CHECK(Distance(b.Directive().moveTarget, {1000, 0, 0}) < 1.0);
    // A weak sound elsewhere does not steal the target (hysteresis)...
    b.Enqueue(Heard(StimulusCategory::Environmental, {-1000, 0, 0}, 5, 5, 8.0));
    UpdateBrain(b, 5, h);
    CHECK_EQ(b.Directive().traceId, first);
    // ...but gunfire does.
    for (Tick t = 6; t <= 8; ++t) b.Enqueue(Heard(StimulusCategory::Gunfire, {0, -2000, 0}, t, t, 30.0));
    UpdateBrain(b, 8, h);
    CHECK(b.Directive().traceId != first);
    CHECK(b.Directive().reason == DirectiveReason::TargetUpdated || b.State() == AwarenessState::Combat);
    CHECK(Distance(b.Directive().moveTarget, {0, -2000, 0}) < 300.0);
    // Moving source: estimate follows new evidence.
    RobotAcousticBrain m(2, MakeSentinelProfile(), 1);
    for (Tick t = 1; t <= 4; ++t) m.Enqueue(Heard(StimulusCategory::Footstep, {1000, 0, 0}, t, t, 16.0));
    UpdateBrain(m, 4, h);
    const Vec3 before = m.Directive().moveTarget;
    for (Tick t = 5; t <= 8; ++t) m.Enqueue(Heard(StimulusCategory::Footstep, {1000, 400, 0}, t, t, 16.0));
    UpdateBrain(m, 8, h);
    CHECK(m.Directive().moveTarget.y > before.y + 100.0);
}

AIS_TEST(Investigation_LostSourceSearchThenReturnToPatrol) {
    Sim s;
    const Vec3 home{0, 0, 0};
    s.AddRobot(1, "SENTINEL", home);
    for (int i = 0; i < 6; ++i) {
        s.Emit(StimulusCategory::Footstep, {1000, 300, 0});
        s.Run(10, 15.0);
    }
    CHECK(s.State(1) == AwarenessState::Investigating || s.State(1) == AwarenessState::Searching);
    // The player goes quiet. Robot must search, then go home, then calm down.
    bool searched = false, returned = false;
    std::vector<Vec3> points;
    for (int t = 0; t < 3000; ++t) {
        s.Run(1, 15.0);
        const AcousticDirective* d = s.world->Directive(1);
        if (d->state == AwarenessState::Searching) {
            searched = true;
            if (d->hasMoveTarget && (points.empty() || Distance(points.back(), d->moveTarget) > 1.0)) points.push_back(d->moveTarget);
        }
        if (searched && d->state == AwarenessState::ReturningToPatrol) returned = true;
        if (returned && d->state == AwarenessState::Unaware) break;
    }
    CHECK(searched);
    CHECK(returned);
    CHECK(s.State(1) == AwarenessState::Unaware);
    CHECK(Distance(s.world->HostState(1)->position, home) <= MakeSentinelProfile().arrivalRadiusCm + 1e-6);
    CHECK(points.size() >= 2);
    for (const Vec3& p : points) CHECK(Distance(p, {1000, 300, 0}) <= MakeSentinelProfile().searchRadiusMaxCm + 600.0);
}

AIS_TEST(Investigation_SourceThatFadesTriggersSourceLostSearch) {
    RobotAcousticBrain b(1, MakeSentinelProfile(), 1);
    const RobotHostState h = At({0, 0, 0});
    for (Tick t = 1; t <= 4; ++t) b.Enqueue(Heard(StimulusCategory::Door, {2500, 0, 0}, t, t, 14.0));
    UpdateBrain(b, 4, h);
    CHECK(b.State() == AwarenessState::Investigating);
    // Robot is stuck (never arrives); information goes stale.
    Tick t = 5;
    while (b.State() == AwarenessState::Investigating && t < 5000) UpdateBrain(b, t += 30, h);
    CHECK(b.State() == AwarenessState::Searching || b.State() == AwarenessState::ReturningToPatrol);
}

AIS_TEST(Investigation_ObjectivePriorityResistsDistraction) {
    RobotAcousticBrain guard(1, MakeSentinelProfile(), 1);
    RobotAcousticBrain idle(2, MakeSentinelProfile(), 1);
    for (Tick t = 1; t <= 4; ++t) {
        guard.Enqueue(Heard(StimulusCategory::Door, {1500, 0, 0}, t, t, 14.0));
        idle.Enqueue(Heard(StimulusCategory::Door, {1500, 0, 0}, t, t, 14.0));
    }
    UpdateBrain(guard, 4, At({0, 0, 0}, 95.0));
    UpdateBrain(idle, 4, At({0, 0, 0}, 0.0));
    CHECK(idle.State() == AwarenessState::Investigating);
    CHECK(guard.State() == AwarenessState::Suspicious);
    CHECK(guard.Directive().reason == DirectiveReason::ObjectiveHeld);
    CHECK(guard.Directive().hasLookAt && !guard.Directive().hasMoveTarget);
}

// --- Decoys -------------------------------------------------------------------

AIS_TEST(Decoy_EmissionPatternsAndExpiry) {
    Sim s;
    DecoySystem& d = s.world->Decoys();
    const EntityId timed = d.Deploy("TimedNoiseEmitter", {0, 0, 0}, 1, 0);
    const EntityId metal = d.Deploy("RepeatingMetallic", {100, 0, 0}, 1, 0);
    CHECK(timed != kNoEntity && metal != kNoEntity && timed != metal);
    CHECK(timed >= kDecoyIdBase);
    const DecoyDefinition* md = s.cfg.FindDecoy("RepeatingMetallic");
    const DecoyDefinition* td = s.cfg.FindDecoy("TimedNoiseEmitter");
    s.Run(static_cast<int>(md->lifetimeTicks) + 5);
    CHECK(!d.IsActive(timed));
    CHECK(!d.IsActive(metal));
    CHECK_EQ(d.ActiveCount(), std::size_t{0});
    const std::uint64_t expectedMetal = (md->lifetimeTicks - md->armDelayTicks - 1) / md->intervalTicks + 1;
    CHECK_EQ(d.TotalEmissions(), expectedMetal + 1);  // + one timed burst
    (void)td;
    const std::uint64_t after = d.TotalEmissions();
    s.Run(200);
    CHECK_EQ(d.TotalEmissions(), after);  // nothing after expiry
}

AIS_TEST(Decoy_DestroyStopsEmissionAndDeployRejectsBadInput) {
    Sim s;
    DecoySystem& d = s.world->Decoys();
    const EntityId id = d.Deploy("EmergencyRadio", {0, 0, 0}, 1, 0);
    s.Run(40);
    const std::uint64_t n = d.TotalEmissions();
    CHECK(n >= 1);
    CHECK(d.Destroy(id));
    CHECK(!d.Destroy(id));
    s.Run(300);
    CHECK_EQ(d.TotalEmissions(), n);
    DecoyDefinition bad;
    bad.intervalTicks = 0;
    CHECK(d.Deploy(bad, {0, 0, 0}, 1, 0) == kNoEntity);
    bad = *s.cfg.FindDecoy("RecordedMechanical");
    bad.jitterTicks = bad.intervalTicks;
    CHECK(d.Deploy(bad, {0, 0, 0}, 1, 0) == kNoEntity);
    CHECK(d.Deploy("RepeatingMetallic", {std::nan(""), 0, 0}, 1, 0) == kNoEntity);
    CHECK(d.Deploy("NoSuchDecoy", {0, 0, 0}, 1, 0) == kNoEntity);
    DecoySystem capped(s.cfg, 2);
    CHECK(capped.Deploy("EmergencyRadio", {0, 0, 0}, 1, 0) != kNoEntity);
    CHECK(capped.Deploy("EmergencyRadio", {0, 0, 0}, 1, 0) != kNoEntity);
    CHECK(capped.Deploy("EmergencyRadio", {0, 0, 0}, 1, 0) == kNoEntity);
}

AIS_TEST(Decoy_RobotsHearApparentSignatureAndDetectArtificiality) {
    Sim s;
    s.AddRobot(1, "SENTINEL", {0, 0, 0});
    s.AddRobot(2, "WRAITH", {0, 400, 0});
    std::vector<PerceptionRecord> seen;
    s.world->SetPerceptionObserver(&seen);
    s.world->Decoys().Deploy("RepeatingMetallic", {2000, 0, 0}, 77, 0);
    Tick wraithFlag = 0, sentinelFlag = 0;
    for (int t = 0; t < 400; ++t) {
        s.world->Step();
        for (EntityId id : {EntityId{1}, EntityId{2}}) {
            for (const auto& tr : s.world->Brain(id)->Traces()) {
                Tick& flag = id == 1 ? sentinelFlag : wraithFlag;
                if (tr.suspectedArtificial && flag == 0) flag = s.world->CurrentTick();
            }
        }
    }
    CHECK(!seen.empty());
    for (const auto& p : seen) CHECK(p.perceived.apparentCategory == StimulusCategory::MetalImpact);
    CHECK(wraithFlag != 0);
    CHECK(sentinelFlag != 0);
    CHECK(wraithFlag < sentinelFlag);  // better discrimination (and it shares the verdict)
}

AIS_TEST(Decoy_DoesNotGuaranteeEveryRobotAbandonsItsObjective) {
    Sim s;
    s.AddRobot(1, "SENTINEL", {0, 0, 0}, 0.0);    // idle
    s.AddRobot(3, "SENTINEL", {0, -300, 0}, 60.0);  // guarding extraction
    s.AddRobot(2, "WRAITH", {0, 300, 0}, 0.0);
    const Vec3 decoy{2000, 0, 0};
    s.world->Decoys().Deploy("RepeatingMetallic", decoy, 77, 0);
    double minIdle = 1e9, minGuard = 1e9, minWraith = 1e9;
    int wraithInvestigations = 0;
    AwarenessState prevW = AwarenessState::Unaware;
    for (int t = 0; t < 1500; ++t) {
        s.Run(1, 15.0);
        minIdle = std::min(minIdle, Distance(s.world->HostState(1)->position, decoy));
        minGuard = std::min(minGuard, Distance(s.world->HostState(3)->position, decoy));
        minWraith = std::min(minWraith, Distance(s.world->HostState(2)->position, decoy));
        const AwarenessState w = s.State(2);
        if (w == AwarenessState::Investigating && prevW != AwarenessState::Investigating) ++wraithInvestigations;
        prevW = w;
    }
    CHECK(minIdle < 800.0);    // idle Sentinel checked it out
    CHECK(minGuard > 1500.0);  // guard held its post
    CHECK(minWraith < 800.0);  // Wraith hunts artificial signals
    CHECK(wraithInvestigations >= 2);
    // After expiry everyone eventually stands down.
    s.Run(3000, 15.0);
    for (EntityId id : {EntityId{1}, EntityId{2}, EntityId{3}}) {
        CHECK(s.State(id) == AwarenessState::Unaware || s.State(id) == AwarenessState::Suspicious);
    }
}

// --- Multiple robots & signal network ----------------------------------------------

AIS_TEST(Network_MultipleRobotsOneEventIndependentEstimates) {
    Sim s;
    s.AddRobot(1, "SENTINEL", {0, 0, 0});
    s.AddRobot(2, "SENTINEL", {2000, 0, 0});
    s.AddRobot(3, "SENTINEL", {1000, 1500, 0});
    s.AddRobot(4, "SENTINEL", {90000, 0, 0});  // far away: culled by broadphase
    std::vector<PerceptionRecord> seen;
    s.world->SetPerceptionObserver(&seen);
    s.Emit(StimulusCategory::BreakObject, {1000, 500, 0});
    s.Run(1);
    CHECK_EQ(seen.size(), std::size_t{3});
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i].robotId != 4);
        for (std::size_t j = i + 1; j < seen.size(); ++j) {
            CHECK(Distance(seen[i].perceived.estimatedPosition, seen[j].perceived.estimatedPosition) > 1.0);
        }
    }
    CHECK(s.world->Stats().perceptionChecks == 3);
}

AIS_TEST(Network_SupportRobotsTakeDistinctFlankingPositions) {
    const RobotHostState h = At({0, 0, 0});
    RobotAcousticBrain a(5, MakeSentinelProfile(), 1);
    RobotAcousticBrain b(6, MakeSentinelProfile(), 1);
    for (RobotAcousticBrain* r : {&a, &b}) {
        for (Tick t = 1; t <= 4; ++t) r->Enqueue(Heard(StimulusCategory::Door, {2000, 0, 0}, t, t, 14.0));
        r->NotifyAllyInvestigating(2, {2000, 0, 0}, 4);
        UpdateBrain(*r, 4, h);
        CHECK(r->Directive().reason == DirectiveReason::SupportingAlly);
    }
    CHECK(Distance(a.Directive().moveTarget, b.Directive().moveTarget) > 50.0);
    // Leader (lower id) ignores claims from higher ids.
    RobotAcousticBrain leader(1, MakeSentinelProfile(), 1);
    for (Tick t = 1; t <= 4; ++t) leader.Enqueue(Heard(StimulusCategory::Door, {2000, 0, 0}, t, t, 14.0));
    leader.NotifyAllyInvestigating(9, {2000, 0, 0}, 4);
    UpdateBrain(leader, 4, h);
    CHECK(leader.Directive().reason != DirectiveReason::SupportingAlly);
}

AIS_TEST(Network_WraithReportsReachSentinelOutOfEarshotWithLatency) {
    Sim s;
    s.AddRobot(1, "WRAITH", {0, 0, 0});
    s.AddRobot(2, "SENTINEL", {9000, 0, 0});  // too far to hear footsteps
    Tick firstShared = 0;
    for (int t = 0; t < 120; ++t) {
        if (t % 10 == 0) s.Emit(StimulusCategory::Sprint, {900, 0, 0});
        s.world->Step();
        if (firstShared == 0 && !s.world->Brain(2)->Traces().empty()) firstShared = s.world->CurrentTick();
    }
    CHECK(s.world->Stats().reportsPublished >= 1);
    CHECK(firstShared > s.cfg.world.reportLatencyTicks);
    const auto& traces = s.world->Brain(2)->Traces();
    CHECK(!traces.empty());
    if (!traces.empty()) {
        CHECK_EQ(traces[0].directHits, 0u);
        CHECK(traces[0].uncertaintyCm > s.world->Brain(1)->Traces().front().uncertaintyCm);  // second-hand is vaguer
    }
    CHECK(s.State(2) != AwarenessState::Combat);
}

AIS_TEST(Network_SharedReportsAloneNeverCauseCombat) {
    RobotAcousticBrain b(2, MakeSentinelProfile(), 1);
    for (Tick t = 1; t <= 40; ++t) {
        PerceivedStimulus s = Heard(StimulusCategory::Gunfire, {800, 0, 0}, t, 0, 40.0, 100.0);
        s.fromSharedReport = true;
        b.Enqueue(s);
        UpdateBrain(b, t, At({0, 0, 0}));
        CHECK(b.State() != AwarenessState::Combat);
    }
    CHECK(b.Suspicion() > MakeSentinelProfile().combatThreshold);  // alarmed, but unconfirmed
}

AIS_TEST(Network_RelayEnforcesRangeRateAndCapacity) {
    WorldConfig w;
    w.reportLatencyTicks = 2;
    w.reportRangeCm = 1000;
    w.reportMinIntervalTicks = 10;
    w.maxReportsInFlight = 2;
    LocalSignalRelay relay(w);
    AcousticReport r;
    r.senderId = 1;
    r.sentTick = 0;
    CHECK(relay.Publish(r));
    r.sentTick = 5;
    CHECK(!relay.Publish(r));  // rate limited
    r.senderId = 2;
    CHECK(relay.Publish(r));
    r.senderId = 3;
    CHECK(!relay.Publish(r));  // capacity
    std::vector<RelayReceiver> rx{{1, {0, 0, 0}, true}, {2, {500, 0, 0}, true}, {3, {5000, 0, 0}, true}, {4, {0, 10, 0}, false}};
    std::vector<DeliveredReport> out;
    relay.Deliver(1, rx, out);
    CHECK(out.empty());  // latency
    relay.Deliver(2, rx, out);
    CHECK_EQ(out.size(), std::size_t{1});  // sender1 -> robot 2 only (3 out of range, 4 deaf, self skipped)
    if (!out.empty()) CHECK_EQ(out[0].first, EntityId{2});
    CHECK_EQ(relay.InFlight(), std::size_t{1});
}

// --- Scaling --------------------------------------------------------------------------

AIS_TEST(World_SensoryUpdateFrequencyIsHonouredAndStaggered) {
    AisConfig cfg = AisConfig::MakeDefault();
    RobotSensoryProfile slow = MakeSentinelProfile();
    slow.name = "SLOW";
    slow.sensoryUpdateIntervalTicks = 5;
    cfg.robotProfiles.push_back(slow);
    Sim s(cfg);
    for (EntityId i = 1; i <= 5; ++i) s.AddRobot(i, "SLOW", {static_cast<double>(i) * 100, 0, 0});
    s.Run(100);
    CHECK_EQ(s.world->Stats().brainUpdates, std::uint64_t{100});  // 5 robots * 100 ticks / 5
    std::vector<int> perTick(5, 0);
    for (Tick t = 1; t <= 5; ++t) {
        for (EntityId i = 1; i <= 5; ++i) perTick[t - 1] += s.world->Brain(i)->IsUpdateDue(t) ? 1 : 0;
    }
    for (int n : perTick) CHECK_EQ(n, 1);  // one robot per tick, not all at once
}

AIS_TEST(World_PerceptionBudgetDefersInsteadOfStalling) {
    AisConfig cfg = AisConfig::MakeDefault();
    cfg.world.maxPerceptionChecksPerTick = 10;
    Sim s(cfg);
    for (EntityId i = 1; i <= 8; ++i) s.AddRobot(i, "SENTINEL", {static_cast<double>(i) * 50, 0, 0});
    for (EntityId i = 1; i <= 6; ++i) s.Emit(StimulusCategory::Door, {0, static_cast<double>(i) * 300, 0}, 100 + i);
    s.world->Step();
    CHECK(s.world->Stats().perceptionChecks <= 10);
    CHECK(s.world->Stats().eventsDeferred > 0);
    s.Run(10);
    CHECK_EQ(s.world->Stats().eventsProcessed, std::uint64_t{6});
}

AIS_TEST(World_ManyRobotsManyEventsStayWithinBudget) {
    Sim s;
    for (EntityId i = 1; i <= 64; ++i) {
        const double x = static_cast<double>((i - 1) % 8) * 5000.0;
        const double y = static_cast<double>((i - 1) / 8) * 5000.0;
        s.AddRobot(i, (i % 4 == 0) ? "WRAITH" : "SENTINEL", {x, y, 0});
    }
    for (int t = 0; t < 300; ++t) {
        for (EntityId p = 0; p < 8; ++p) {
            s.Emit(StimulusCategory::Footstep, {static_cast<double>(p) * 4000 + t * 10.0, 17500, 0}, 500 + p);
        }
        if (t % 50 == 0) s.Emit(StimulusCategory::Gunfire, {17500, 17500, 0}, 600);
        s.world->Step();
        CHECK(s.world->Bus().QueueSize() <= s.cfg.bus.queueCapacity);
    }
    // Broadphase keeps footsteps local: far fewer checks than events * robots.
    const auto& st = s.world->Stats();
    CHECK(st.perceptionChecks < st.eventsProcessed * 64 / 4);
}

// --- Determinism ------------------------------------------------------------------------

namespace {

std::vector<std::uint64_t> RunScripted(std::uint64_t seed, bool reverseRegistration) {
    AisConfig cfg = AisConfig::MakeDefault();
    cfg.world.worldSeed = seed;
    Sim s(cfg);
    s.env.AddWall({{1500, -2000, -200}, {1530, 2000, 400}}, static_cast<MaterialId>(cfg.FindMaterial("Brick")));
    s.env.AddZone({{1530, -2000, -200}, {4000, 2000, 400}}, 1, true);
    const std::vector<std::pair<EntityId, const char*>> robots{{1, "SENTINEL"}, {2, "WRAITH"}, {3, "SENTINEL"}};
    const std::vector<Vec3> pos{{0, 0, 0}, {2500, 0, 0}, {-1500, 1500, 0}};
    for (std::size_t k = 0; k < robots.size(); ++k) {
        const std::size_t i = reverseRegistration ? robots.size() - 1 - k : k;
        s.AddRobot(robots[i].first, robots[i].second, pos[i]);
    }
    std::sort(s.ids.begin(), s.ids.end());
    std::vector<std::uint64_t> hashes;
    for (int t = 0; t < 900; ++t) {
        if (t % 15 == 0 && t < 300) s.Emit(StimulusCategory::Footstep, {800.0 + t, 200, 0}, 10, SurfaceType::Gravel);
        if (t == 320) s.world->Decoys().Deploy("RecordedMechanical", {-500, -800, 0}, 10, s.world->CurrentTick());
        if (t >= 500 && t < 520 && t % 4 == 0) s.Emit(StimulusCategory::Gunfire, {1200, 400, 0}, 10);
        s.world->Step();
        s.MoveRobots(12.0);
        hashes.push_back(s.world->StateHash());
    }
    return hashes;
}

}  // namespace

AIS_TEST(Determinism_ReplayIsBitIdentical) {
    const auto a = RunScripted(1234, false);
    const auto b = RunScripted(1234, false);
    CHECK(a == b);
    const auto c = RunScripted(1234, true);  // registration order must not matter
    CHECK(a == c);
    const auto d = RunScripted(9999, false);  // seed changes the noise
    CHECK(a != d);
}

// --- Config -------------------------------------------------------------------------------

AIS_TEST(Config_DefaultIsValidAndShippedIniLoads) {
    const AisConfig def = AisConfig::MakeDefault();
    CHECK(ValidateConfig(def).empty());
    AisConfig cfg = AisConfig::MakeDefault();
    const auto r = LoadConfigFromFile(AIS_DATA_DIR "/ais_default.ini", cfg);
    for (const auto& e : r.errors) std::printf("    %s\n", e.c_str());
    CHECK(r.ok);
    CHECK(cfg.FindProfile("WARDEN") != nullptr);
    CHECK(!LoadConfigFromFile("/nonexistent/ais.ini", cfg).ok);
}

AIS_TEST(Config_StrictLoaderRejectsTyposAndUnsafeValues) {
    AisConfig cfg = AisConfig::MakeDefault();
    CHECK(!LoadConfigFromIni("[robot.SENTINEL]\nhearing = 5\n", cfg).ok);         // unknown key
    cfg = AisConfig::MakeDefault();
    CHECK(!LoadConfigFromIni("[category.Gunfre]\nthreat = 1\n", cfg).ok);         // unknown category
    cfg = AisConfig::MakeDefault();
    CHECK(!LoadConfigFromIni("[bus]\nqueue_capacity = -3\n", cfg).ok);            // negative unsigned
    cfg = AisConfig::MakeDefault();
    CHECK(!LoadConfigFromIni("[propagation]\nrain_noise_db = 1,2\n", cfg).ok);    // short band list
    cfg = AisConfig::MakeDefault();
    CHECK(!LoadConfigFromIni("orphan = 1\n", cfg).ok);                            // no section
    cfg = AisConfig::MakeDefault();
    // A single sound could jump to combat: rejected by design validation.
    const auto r = LoadConfigFromIni("[robot.SENTINEL]\nmax_single_event_gain = 90\n", cfg);
    CHECK(!r.ok);
    cfg = AisConfig::MakeDefault();
    CHECK(!LoadConfigFromIni("[robot.SENTINEL]\ninvestigate_threshold = 5\n", cfg).ok);
    cfg = AisConfig::MakeDefault();
    const auto ok = LoadConfigFromIni("; comment\n[world]\nticks_per_second = 60 # inline\n[surface.Metal]\nloudness_offset_db = 9\n", cfg);
    CHECK(ok.ok);
    CHECK_NEAR(cfg.world.ticksPerSecond, 60.0, 1e-12);
}
