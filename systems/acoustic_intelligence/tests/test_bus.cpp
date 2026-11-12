// Event bus: authority, validation, dedup, rate limiting, bounded queue, flooding.
#include <cmath>
#include <limits>

#include "TestFramework.h"
#include "TestHelpers.h"

using namespace ais;

namespace {

AcousticEvent Ev(const AisConfig& cfg, StimulusCategory c, const Vec3& p, Tick t, EntityId src) {
    return MakeEvent(cfg, c, p, t, SurfaceType::None, src);
}

}  // namespace

AIS_TEST(Bus_ClientClaimsAreNeverAccepted) {
    const AisConfig cfg = AisConfig::MakeDefault();
    AcousticEventBus bus(cfg);
    EventId id = 99;
    CHECK(bus.Submit(Ev(cfg, StimulusCategory::Gunfire, {0, 0, 0}, 0, 5), EventOrigin::ClientClaim, 0, &id) ==
          SubmitResult::RejectedNotAuthoritative);
    CHECK_EQ(id, EventId{0});
    CHECK_EQ(bus.QueueSize(), std::size_t{0});
    CHECK_EQ(bus.Stats().Count(SubmitResult::RejectedNotAuthoritative), std::uint64_t{1});
}

AIS_TEST(Bus_AssignsMonotonicIdsIgnoringCallerIds) {
    const AisConfig cfg = AisConfig::MakeDefault();
    AcousticEventBus bus(cfg);
    AcousticEvent e = Ev(cfg, StimulusCategory::Door, {0, 0, 0}, 0, 1);
    e.id = 123456;
    EventId a = 0, b = 0;
    CHECK(IsAccepted(bus.Submit(e, EventOrigin::ServerAuthoritative, 0, &a)));
    e.sourceId = 2;
    CHECK(IsAccepted(bus.Submit(e, EventOrigin::ServerAuthoritative, 0, &b)));
    CHECK_EQ(a, EventId{1});
    CHECK_EQ(b, EventId{2});
}

AIS_TEST(Bus_RejectsInvalidEvents) {
    const AisConfig cfg = AisConfig::MakeDefault();
    AcousticEventBus bus(cfg);
    const Tick now = 100;
    auto submit = [&](AcousticEvent e) { return bus.Submit(e, EventOrigin::ServerAuthoritative, now); };
    const AcousticEvent good = Ev(cfg, StimulusCategory::Footstep, {0, 0, 0}, now, 1);
    AcousticEvent e = good;
    e.position.x = std::numeric_limits<double>::quiet_NaN();
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.position.y = std::numeric_limits<double>::infinity();
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.position.x = cfg.bus.worldMax.x * 2.0;
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.loudnessDb = cfg.bus.maxLoudnessDb + 1.0;
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.loudnessDb = -5.0;
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.emitTick = now + 1;  // from the future
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.emitTick = now - cfg.bus.maxEventAgeTicks - 1;  // stale
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.durationTicks = 0;
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.artificiality = 1.5;
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.category = static_cast<StimulusCategory>(200);
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.surface = static_cast<SurfaceType>(77);
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    e = good;
    e.spectrumDb[1] = std::numeric_limits<double>::quiet_NaN();
    CHECK(submit(e) == SubmitResult::RejectedInvalid);
    // Boundaries that must still pass.
    e = good;
    e.emitTick = now - cfg.bus.maxEventAgeTicks;
    CHECK(IsAccepted(submit(e)));
    e = good;
    e.sourceId = 2;
    e.loudnessDb = cfg.bus.maxLoudnessDb;
    CHECK(IsAccepted(submit(e)));
    CHECK_EQ(bus.QueueSize(), std::size_t{2});
}

AIS_TEST(Bus_DeduplicatesSameSoundReportedTwice) {
    const AisConfig cfg = AisConfig::MakeDefault();
    AcousticEventBus bus(cfg);
    AcousticEvent e = Ev(cfg, StimulusCategory::Footstep, {0, 0, 0}, 10, 7);
    CHECK(IsAccepted(bus.Submit(e, EventOrigin::ServerAuthoritative, 10)));
    AcousticEvent louder = e;
    louder.position = {30, 0, 0};
    louder.loudnessDb += 6.0;
    CHECK(bus.Submit(louder, EventOrigin::ServerAuthoritative, 10) == SubmitResult::MergedDuplicate);
    CHECK_EQ(bus.QueueSize(), std::size_t{1});
    // Different source, different category, far away, or later: not duplicates.
    AcousticEvent other = e;
    other.sourceId = 8;
    CHECK(IsAccepted(bus.Submit(other, EventOrigin::ServerAuthoritative, 10)));
    AcousticEvent cat = e;
    cat.category = cat.apparentCategory = StimulusCategory::JumpLanding;
    CHECK(IsAccepted(bus.Submit(cat, EventOrigin::ServerAuthoritative, 10)));
    AcousticEvent far = e;
    far.position = {500, 0, 0};
    CHECK(IsAccepted(bus.Submit(far, EventOrigin::ServerAuthoritative, 10)));
    AcousticEvent later = e;
    later.emitTick = 10 + cfg.bus.dedupWindowTicks + 1;
    CHECK(IsAccepted(bus.Submit(later, EventOrigin::ServerAuthoritative, later.emitTick)));
    // Merged loudness is kept on the queued event.
    std::vector<AcousticEvent> out;
    bus.Drain(later.emitTick, out);
    bool foundLouder = false;
    for (const auto& q : out) foundLouder = foundLouder || (q.id == 1 && std::fabs(q.loudnessDb - louder.loudnessDb) < 1e-9);
    CHECK(foundLouder);
    // Anonymous sources dedupe by place and category too.
    AcousticEvent anon = Ev(cfg, StimulusCategory::BreakObject, {0, 0, 0}, 20, kNoEntity);
    CHECK(IsAccepted(bus.Submit(anon, EventOrigin::ServerAuthoritative, 20)));
    CHECK(bus.Submit(anon, EventOrigin::ServerAuthoritative, 20) == SubmitResult::MergedDuplicate);
}

AIS_TEST(Bus_BoundedQueueEvictsLowestPriority) {
    AisConfig cfg = AisConfig::MakeDefault();
    cfg.bus.queueCapacity = 8;
    cfg.bus.maxDrainPerTick = 3;
    AcousticEventBus bus(cfg);
    for (EntityId i = 1; i <= 8; ++i) {
        CHECK(IsAccepted(bus.Submit(Ev(cfg, StimulusCategory::Footstep, {static_cast<double>(i) * 1000, 0, 0}, 0, i),
                                    EventOrigin::ServerAuthoritative, 0)));
    }
    CHECK(bus.Submit(Ev(cfg, StimulusCategory::Footstep, {0, 9000, 0}, 0, 50), EventOrigin::ServerAuthoritative, 0) ==
          SubmitResult::RejectedQueueFull);
    CHECK(bus.Submit(Ev(cfg, StimulusCategory::Gunfire, {0, 0, 0}, 0, 51), EventOrigin::ServerAuthoritative, 0) ==
          SubmitResult::AcceptedEvictedLower);
    CHECK_EQ(bus.QueueSize(), std::size_t{8});
    CHECK_EQ(bus.Stats().evicted, std::uint64_t{1});
    std::vector<AcousticEvent> out;
    CHECK_EQ(bus.Drain(0, out), std::size_t{3});
    CHECK(out[0].category == StimulusCategory::Gunfire);  // highest priority first
    CHECK(out[1].id < out[2].id);                          // equal priority: FIFO by id
    CHECK_EQ(bus.QueueSize(), std::size_t{5});
}

AIS_TEST(Bus_ExpiredEventsAreDroppedOnDrain) {
    const AisConfig cfg = AisConfig::MakeDefault();
    AcousticEventBus bus(cfg);
    CHECK(IsAccepted(bus.Submit(Ev(cfg, StimulusCategory::Door, {0, 0, 0}, 0, 1), EventOrigin::ServerAuthoritative, 0)));
    std::vector<AcousticEvent> out;
    CHECK_EQ(bus.Drain(cfg.bus.maxEventAgeTicks + 1, out), std::size_t{0});
    CHECK_EQ(bus.Stats().expired, std::uint64_t{1});
}

AIS_TEST(Bus_TokenBucketLimitsOneSourceAndRefills) {
    const AisConfig cfg = AisConfig::MakeDefault();
    AcousticEventBus bus(cfg);
    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        // Spread positions so dedup does not hide the rate limiter.
        const auto r = bus.Submit(Ev(cfg, StimulusCategory::Door, {static_cast<double>(i) * 200, 0, 0}, 0, 42),
                                  EventOrigin::ServerAuthoritative, 0);
        accepted += IsAccepted(r) ? 1 : 0;
    }
    CHECK_EQ(accepted, static_cast<int>(cfg.bus.sourceBurst));
    CHECK(bus.Stats().Count(SubmitResult::RejectedRateLimited) == 100 - static_cast<std::uint64_t>(cfg.bus.sourceBurst));
    // After 2 ticks at 0.5 tokens/tick exactly one more is allowed.
    std::vector<AcousticEvent> sink;
    bus.Drain(2, sink);
    CHECK(IsAccepted(bus.Submit(Ev(cfg, StimulusCategory::Door, {0, 5000, 0}, 2, 42), EventOrigin::ServerAuthoritative, 2)));
    CHECK(bus.Submit(Ev(cfg, StimulusCategory::Door, {0, 6000, 0}, 2, 42), EventOrigin::ServerAuthoritative, 2) ==
          SubmitResult::RejectedRateLimited);
}

AIS_TEST(Bus_AdversarialFloodStaysBoundedAndGunfireStillGetsThrough) {
    AisConfig cfg = AisConfig::MakeDefault();
    cfg.bus.maxTrackedSources = 64;
    AcousticEventBus bus(cfg);
    // 20k events from 20k distinct sources in one tick.
    std::size_t accepted = 0;
    for (EntityId i = 1; i <= 20000; ++i) {
        const auto r = bus.Submit(Ev(cfg, StimulusCategory::Environmental,
                                     {static_cast<double>(i % 1000) * 100, static_cast<double>(i / 1000) * 100, 0}, 5, i),
                                  EventOrigin::ServerAuthoritative, 5);
        accepted += IsAccepted(r) ? 1u : 0u;
    }
    CHECK(accepted <= cfg.bus.maxAcceptedPerTick);
    CHECK(bus.QueueSize() <= cfg.bus.queueCapacity);
    // Next tick: flood the queue to capacity with junk, then a real gunshot.
    for (EntityId i = 30000; i < 30400; ++i) {
        bus.Submit(Ev(cfg, StimulusCategory::Environmental, {static_cast<double>(i) * 10, 0, 0}, 6, i),
                   EventOrigin::ServerAuthoritative, 6);
    }
    CHECK(bus.QueueSize() <= cfg.bus.queueCapacity);
    for (Tick t = 7; t < 12; ++t) {
        for (EntityId i = 0; i < 200; ++i) {
            bus.Submit(Ev(cfg, StimulusCategory::Environmental, {static_cast<double>(i) * 37, 99, 0}, t, 40000 + i),
                       EventOrigin::ServerAuthoritative, t);
        }
    }
    CHECK_EQ(bus.QueueSize(), cfg.bus.queueCapacity);
    const auto shot = bus.Submit(Ev(cfg, StimulusCategory::Gunfire, {0, 0, 0}, 12, 999999), EventOrigin::ServerAuthoritative, 12);
    CHECK(IsAccepted(shot));
    std::vector<AcousticEvent> out;
    bus.Drain(12, out);
    CHECK(!out.empty() && out[0].category == StimulusCategory::Gunfire);
}

AIS_TEST(Bus_OneSourceSpammingCannotStarveOthers) {
    const AisConfig cfg = AisConfig::MakeDefault();
    AcousticEventBus bus(cfg);
    for (int i = 0; i < 5000; ++i) {
        bus.Submit(Ev(cfg, StimulusCategory::Door, {static_cast<double>(i) * 100, 0, 0}, 0, 13), EventOrigin::ServerAuthoritative, 0);
    }
    CHECK(IsAccepted(bus.Submit(Ev(cfg, StimulusCategory::Footstep, {0, 0, 0}, 0, 14), EventOrigin::ServerAuthoritative, 0)));
    CHECK(bus.QueueSize() <= static_cast<std::size_t>(cfg.bus.sourceBurst) + 1);
}

AIS_TEST(Authority_ClientWorldIsInert) {
    aistest::Sim client(AisConfig::MakeDefault(), AuthorityRole::Client);
    CHECK(!client.world->AddRobot(1, "SENTINEL", {0, 0, 0}));
    CHECK(client.Emit(StimulusCategory::Gunfire, {0, 0, 0}) == SubmitResult::RejectedNotAuthoritative);
    client.world->Step();
    CHECK_EQ(client.world->CurrentTick(), Tick{0});
    CHECK(client.world->Decoys().ActiveCount() == 0);
}

AIS_TEST(Authority_ServerWorldRejectsClientOriginEvents) {
    aistest::Sim s;
    s.AddRobot(1, "SENTINEL", {0, 0, 0});
    AcousticEvent e = MakeEvent(s.cfg, StimulusCategory::Gunfire, {500, 0, 0}, 0, SurfaceType::None, 9);
    CHECK(s.world->SubmitEvent(e, EventOrigin::ClientClaim) == SubmitResult::RejectedNotAuthoritative);
    s.Run(10);
    CHECK(s.State(1) == AwarenessState::Unaware);
    CHECK(s.world->Brain(1)->Traces().empty());
}

AIS_TEST(Authority_CosmeticCuesAreRelevancyFilteredAndHideDecoyTruth) {
    aistest::Sim s;
    s.world->SetClientListeners({{2, {30000, 0, 0}}, {1, {500, 0, 0}}});
    s.Emit(StimulusCategory::Footstep, {0, 0, 0});
    s.world->Step();
    const auto& cues = s.world->CosmeticCuesThisTick();
    CHECK_EQ(cues.size(), std::size_t{1});
    CHECK(!cues.empty() && cues[0].clientId == 1);

    // Gunfire reaches the distant client too.
    s.Emit(StimulusCategory::Gunfire, {0, 0, 0}, 55);
    s.world->Step();
    CHECK_EQ(s.world->CosmeticCuesThisTick().size(), std::size_t{2});

    // Decoys replicate their apparent signature, never "Decoy".
    s.world->Decoys().Deploy("RepeatingMetallic", {0, 0, 0}, 1, s.world->CurrentTick());
    bool sawMetal = false;
    for (int i = 0; i < 40; ++i) {
        s.world->Step();
        for (const auto& c : s.world->CosmeticCuesThisTick()) {
            CHECK(c.apparentCategory != StimulusCategory::Decoy);
            sawMetal = sawMetal || c.apparentCategory == StimulusCategory::MetalImpact;
        }
    }
    CHECK(sawMetal);
}

AIS_TEST(Authority_CosmeticCuesCappedPerClient) {
    AisConfig cfg = AisConfig::MakeDefault();
    cfg.world.maxCosmeticCuesPerClientPerTick = 4;
    aistest::Sim s(cfg);
    s.world->SetClientListeners({{1, {0, 0, 0}}});
    for (EntityId i = 1; i <= 20; ++i) s.Emit(StimulusCategory::Door, {static_cast<double>(i) * 100, 0, 0}, i);
    s.world->Step();
    CHECK_EQ(s.world->CosmeticCuesThisTick().size(), std::size_t{4});
}
