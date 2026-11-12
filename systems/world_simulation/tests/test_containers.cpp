// Container interaction framework: server authority, atomic claims, duplicate
// rejection, shared vs per-player loot, access requirements, recovery.
#include "WsTestHelpers.h"

using namespace wstest;
using namespace llloot;

namespace {

struct Rig {
    Content content;
    Access access;
    MemInventory inv;
    Recorder rec;
    std::unique_ptr<ContainerService> svc;
    std::map<PlayerId, std::uint32_t> seq;
    Tick now = 0;

    explicit Rig(std::uint64_t raid = 1234, ContainerServiceConfig sc = {}) {
        content = Load({kAllLocations[0], kAllLocations[1], kAllLocations[2], kAllLocations[3], kAllLocations[4]});
        svc = std::make_unique<ContainerService>(content.loot, Catalog(), sc);
        svc->SetAccessPort(&access);
        svc->SetGrantPort(&inv);
        svc->SetObserver(&rec);
        svc->SetSoundSink(&rec);
        svc->BeginRaid(raid);
    }
    std::uint32_t C(const char* id) const { return svc->Find(id); }
    Vec3 At(const char* id) const { return svc->Def(C(id)).position + Vec3{50, 0, 0}; }
    void Join(PlayerId p, const char* near) { CHECK(svc->ConnectPlayer(p, 100 + p, At(near))); }
    LootResultCode Do(PlayerId p, LootAction a, const char* c, std::uint32_t slot = 0) {
        return svc->Submit(LootRequest{p, ++seq[p], C(c), a, slot});
    }
    LootResultCode Raw(PlayerId p, std::uint32_t sequence, LootAction a, const char* c, std::uint32_t slot = 0) {
        return svc->Submit(LootRequest{p, sequence, C(c), a, slot});
    }
    void Run(Tick ticks) {
        for (Tick i = 0; i < ticks; ++i) svc->Step(++now);
    }
    // Search to completion; returns the result code of the request.
    LootResultCode SearchAll(PlayerId p, const char* c) {
        const LootResultCode r = Do(p, LootAction::Search, c);
        Run(400);
        return r;
    }
};

}  // namespace

AIS_TEST(Containers_SearchRevealsDeterministicContents) {
    Rig a(99);
    Rig b(99);
    a.Join(1, "DF_CP_NORTH_LOCKER");
    b.Join(1, "DF_CP_NORTH_LOCKER");
    CHECK(a.SearchAll(1, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    b.SearchAll(1, "DF_CP_NORTH_LOCKER");
    const LootResult* ra = a.svc->Contents(a.C("DF_CP_NORTH_LOCKER"), 1);
    const LootResult* rb = b.svc->Contents(b.C("DF_CP_NORTH_LOCKER"), 1);
    CHECK(ra != nullptr && rb != nullptr);
    if (ra == nullptr || rb == nullptr) return;
    CHECK_EQ(ra->Hash(), rb->Hash());
    // Same as the pure generator: the service adds no hidden randomness.
    const ContainerDef& d = a.svc->Def(a.C("DF_CP_NORTH_LOCKER"));
    CHECK_EQ(ra->Hash(), GenerateLoot(a.content.loot, Catalog(), d, ContainerSeed(99, d.id, 0)).Hash());
    CHECK_EQ(a.svc->StateHash(), b.svc->StateHash());
}

AIS_TEST(Containers_DuplicateRequestsRejected) {
    Rig r;
    r.Join(1, "DF_CP_SOUTH_TRUCK");
    CHECK(r.Raw(1, 7, LootAction::Search, "DF_CP_SOUTH_TRUCK") == LootResultCode::Started);
    // The same packet delivered twice (network retry) does nothing the second time.
    CHECK(r.Raw(1, 7, LootAction::Search, "DF_CP_SOUTH_TRUCK") == LootResultCode::RejectedDuplicate);
    CHECK(r.Raw(1, 0, LootAction::Search, "DF_CP_SOUTH_TRUCK") == LootResultCode::RejectedDuplicate);
    r.Run(100);
    CHECK_EQ(r.svc->GetStats().searchesStarted, 1u);
    CHECK_EQ(r.svc->GetStats().generated, 1u);
    // A second, fresh search of a searched shared container is refused.
    CHECK(r.Raw(1, 8, LootAction::Search, "DF_CP_SOUTH_TRUCK") == LootResultCode::RejectedAlreadySearched);
    const LootResult* res = r.svc->Contents(r.C("DF_CP_SOUTH_TRUCK"), 1);
    CHECK(res != nullptr);
    if (res == nullptr || res->items.empty()) return;
    // Duplicate take requests: one grant, ever.
    CHECK(r.Raw(1, 20, LootAction::TakeItem, "DF_CP_SOUTH_TRUCK", 0) == LootResultCode::Accepted);
    CHECK(r.Raw(1, 20, LootAction::TakeItem, "DF_CP_SOUTH_TRUCK", 0) == LootResultCode::RejectedDuplicate);
    CHECK(r.Raw(1, 21, LootAction::TakeItem, "DF_CP_SOUTH_TRUCK", 0) == LootResultCode::RejectedSlotTaken);
    CHECK_EQ(r.inv.received[1].size(), 1u);
    CHECK_EQ(r.inv.calls, 1);
}

AIS_TEST(Containers_SimultaneousSearchesHaveOneWinner) {
    Rig r;
    for (PlayerId p = 1; p <= 4; ++p) r.Join(p, "DF_CP_NORTH_LOCKER");
    // Four requests in the same server tick: exactly one claim.
    int started = 0;
    int busy = 0;
    for (PlayerId p = 1; p <= 4; ++p) {
        const LootResultCode c = r.Do(p, LootAction::Search, "DF_CP_NORTH_LOCKER");
        started += c == LootResultCode::Started ? 1 : 0;
        busy += c == LootResultCode::RejectedBusy ? 1 : 0;
    }
    CHECK_EQ(started, 1);
    CHECK_EQ(busy, 3);
    CHECK_EQ(r.svc->Claimant(r.C("DF_CP_NORTH_LOCKER"), 2), 1u);
    r.Run(100);
    // Everyone sees the same shared contents once searched.
    const LootResult* first = r.svc->Contents(r.C("DF_CP_NORTH_LOCKER"), 1);
    const LootResult* other = r.svc->Contents(r.C("DF_CP_NORTH_LOCKER"), 3);
    CHECK(first != nullptr && first == other);
    if (first == nullptr || first->items.empty()) return;
    // Two players grab the same item in the same tick: one gets it.
    const LootResultCode a = r.Do(2, LootAction::TakeItem, "DF_CP_NORTH_LOCKER", 0);
    const LootResultCode b = r.Do(3, LootAction::TakeItem, "DF_CP_NORTH_LOCKER", 0);
    CHECK(a == LootResultCode::Accepted);
    CHECK(b == LootResultCode::RejectedSlotTaken);
    CHECK_EQ(r.inv.applied.size(), 1u);
    CHECK(r.svc->Slot(r.C("DF_CP_NORTH_LOCKER"), 3, 0).holder == 2u);
}

AIS_TEST(Containers_SharedVersusPerPlayerOwnership) {
    Rig r;
    r.Join(1, "RES_FLAT2_CUPBOARD");
    r.Join(2, "RES_FLAT2_CUPBOARD");
    const std::uint32_t c = r.C("RES_FLAT2_CUPBOARD");
    CHECK(r.content.loot.OwnershipFor(r.svc->Def(c)) == OwnershipPolicy::PerPlayer);
    // Both may search at once: separate instances, separate claims.
    CHECK(r.Do(1, LootAction::Search, "RES_FLAT2_CUPBOARD") == LootResultCode::Started);
    CHECK(r.Do(2, LootAction::Search, "RES_FLAT2_CUPBOARD") == LootResultCode::Started);
    r.Run(200);
    const LootResult* a = r.svc->Contents(c, 1);
    const LootResult* b = r.svc->Contents(c, 2);
    CHECK(a != nullptr && b != nullptr && a != b);
    if (a == nullptr || b == nullptr) return;
    CHECK(a->seed != b->seed);
    // Player 3 has not searched: sees nothing; player 1 cannot take from player 2's instance.
    CHECK(r.svc->Contents(c, 3) == nullptr);
    CHECK(r.Do(1, LootAction::Search, "RES_FLAT2_CUPBOARD") == LootResultCode::RejectedAlreadySearched);
    // Per-player contents depend on the stable recipient key, not the connection order.
    Rig r2;
    r2.Join(2, "RES_FLAT2_CUPBOARD");
    r2.Join(1, "RES_FLAT2_CUPBOARD");
    r2.Do(1, LootAction::Search, "RES_FLAT2_CUPBOARD");
    r2.Run(200);
    const LootResult* a2 = r2.svc->Contents(r2.C("RES_FLAT2_CUPBOARD"), 1);
    CHECK(a2 != nullptr && a2->Hash() == a->Hash());
    // The caretaker locker overrides the location policy: one shared box.
    const std::uint32_t care = r.C("RES_CARETAKER_LOCKER");
    CHECK(r.content.loot.OwnershipFor(r.svc->Def(care)) == OwnershipPolicy::Shared);
    r.svc->UpdatePlayerPosition(1, r.At("RES_CARETAKER_LOCKER"));
    r.svc->UpdatePlayerPosition(2, r.At("RES_CARETAKER_LOCKER"));
    CHECK(r.Do(1, LootAction::Search, "RES_CARETAKER_LOCKER") == LootResultCode::Started);
    CHECK(r.Do(2, LootAction::Search, "RES_CARETAKER_LOCKER") == LootResultCode::RejectedBusy);
}

AIS_TEST(Containers_PowerToolClearanceAndSealRequirements) {
    Rig r;
    r.Join(1, "DF_ARMORY_CRATE");
    CHECK(r.Do(1, LootAction::Search, "DF_ARMORY_CRATE") == LootResultCode::RejectedNoPower);
    r.access.powered.insert("ARMORY_LOCK");
    r.svc->NotifyInfrastructureChanged("ARMORY_LOCK");
    CHECK_EQ(r.rec.Count(LootEventType::AccessChanged), 1u);
    CHECK(r.Do(1, LootAction::Search, "DF_ARMORY_CRATE") == LootResultCode::Started);
    // The lock released once; losing power later does not re-lock an opened crate.
    r.access.powered.clear();
    r.Run(200);
    CHECK(r.svc->Contents(r.C("DF_ARMORY_CRATE"), 1) != nullptr);

    r.svc->UpdatePlayerPosition(1, r.At("DF_OFFICER_LOCKER"));
    CHECK(r.Do(1, LootAction::Search, "DF_OFFICER_LOCKER") == LootResultCode::RejectedNoClearance);
    r.access.items.insert({1, "keycard.depot_red"});
    CHECK(r.Do(1, LootAction::Search, "DF_OFFICER_LOCKER") == LootResultCode::Started);
    r.Run(200);

    r.svc->UpdatePlayerPosition(1, r.At("DF_SIGNALS_CASE"));
    CHECK(r.Do(1, LootAction::Search, "DF_SIGNALS_CASE") == LootResultCode::RejectedMissingTool);
    r.access.items.insert({1, "tool.bolt_cutter"});
    const std::size_t soundsBefore = r.rec.sounds.size();
    CHECK(r.Do(1, LootAction::Search, "DF_SIGNALS_CASE") == LootResultCode::Started);
    // Forcing the damaged lock is loud and slower.
    CHECK(r.rec.sounds.size() == soundsBefore + 2);
    CHECK(r.rec.sounds[soundsBefore].kind == LootSoundKind::ForceLock);
    CHECK(r.rec.sounds[soundsBefore].loudnessDb >= 75.0);
    r.Run(60);
    CHECK(r.svc->Contents(r.C("DF_SIGNALS_CASE"), 1) == nullptr);
    r.Run(200);
    CHECK(r.svc->Contents(r.C("DF_SIGNALS_CASE"), 1) != nullptr);

    r.svc->UpdatePlayerPosition(1, r.At("DF_STORE_CRATE"));
    r.access.sealed.insert("DF_STORAGE");
    CHECK(r.Do(1, LootAction::Search, "DF_STORE_CRATE") == LootResultCode::RejectedSealed);
    r.access.sealed.clear();
    CHECK(r.Do(1, LootAction::Search, "DF_STORE_CRATE") == LootResultCode::Started);
}

AIS_TEST(Containers_HiddenAndObstructedNeedExtraActions) {
    Rig r;
    r.Join(1, "MED_HIDDEN_CACHE");
    CHECK(r.Do(1, LootAction::Search, "MED_HIDDEN_CACHE") == LootResultCode::RejectedHidden);
    CHECK(r.Do(1, LootAction::Discover, "MED_HIDDEN_CACHE") == LootResultCode::Accepted);
    CHECK(r.svc->IsDiscovered(r.C("MED_HIDDEN_CACHE")));
    CHECK(r.Do(1, LootAction::Discover, "MED_TREATMENT_CABINET") == LootResultCode::RejectedNotApplicable);
    CHECK(r.Do(1, LootAction::Search, "MED_HIDDEN_CACHE") == LootResultCode::Started);
    r.Run(100);
    CHECK(r.svc->Contents(r.C("MED_HIDDEN_CACHE"), 1) != nullptr);

    r.svc->UpdatePlayerPosition(1, r.At("MED_WARD_FRIDGE"));
    CHECK(r.Do(1, LootAction::Search, "MED_WARD_FRIDGE") == LootResultCode::RejectedObstructed);
    CHECK(r.Do(1, LootAction::ClearObstruction, "MED_WARD_FRIDGE") == LootResultCode::Started);
    r.Run(30);
    CHECK(!r.svc->IsObstructionCleared(r.C("MED_WARD_FRIDGE")));
    r.Run(40);
    CHECK(r.svc->IsObstructionCleared(r.C("MED_WARD_FRIDGE")));
    CHECK(r.Do(1, LootAction::Search, "MED_WARD_FRIDGE") == LootResultCode::Started);

    r.svc->UpdatePlayerPosition(1, r.At("MED_XRAY_CABINET"));
    r.Run(100);
    CHECK(r.Do(1, LootAction::Search, "MED_XRAY_CABINET") == LootResultCode::RejectedDestroyed);
}

AIS_TEST(Containers_InterruptedSearchRecoversDeterministically) {
    Rig r(555);
    r.Join(1, "DF_CP_NORTH_LOCKER");
    r.Join(2, "DF_CP_NORTH_LOCKER");
    const std::uint32_t c = r.C("DF_CP_NORTH_LOCKER");
    CHECK(r.Do(1, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    r.Run(20);
    // Player 1 drops mid-search: claim released, nothing revealed.
    r.svc->DisconnectPlayer(1);
    CHECK(r.svc->StateFor(c, 2) == InstanceState::Unsearched);
    CHECK(r.svc->Contents(c, 2) == nullptr);
    CHECK_EQ(r.svc->GetStats().searchesInterrupted, 1u);
    // Walking away also interrupts.
    CHECK(r.Do(2, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    r.svc->UpdatePlayerPosition(2, r.At("DF_CP_NORTH_LOCKER") + Vec3{2000, 0, 0});
    r.Run(5);
    CHECK(r.svc->StateFor(c, 2) == InstanceState::Unsearched);
    r.svc->UpdatePlayerPosition(2, r.At("DF_CP_NORTH_LOCKER"));
    CHECK(r.Do(2, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    r.Run(100);
    const LootResult* res = r.svc->Contents(c, 2);
    CHECK(res != nullptr);
    if (res == nullptr) return;
    // The eventual contents equal what an uninterrupted search would have produced.
    const ContainerDef& d = r.svc->Def(c);
    CHECK_EQ(res->Hash(), GenerateLoot(r.content.loot, Catalog(), d, ContainerSeed(555, d.id, 0)).Hash());
    // Cancel is a request too.
    r.svc->UpdatePlayerPosition(2, r.At("DF_CP_SOUTH_TRUCK"));
    CHECK(r.Do(2, LootAction::Search, "DF_CP_SOUTH_TRUCK") == LootResultCode::Started);
    CHECK(r.Do(2, LootAction::CancelSearch, "DF_CP_SOUTH_TRUCK") == LootResultCode::Accepted);
    CHECK(r.svc->StateFor(r.C("DF_CP_SOUTH_TRUCK"), 2) == InstanceState::Unsearched);
}

AIS_TEST(Containers_GrantRetryIsIdempotentAfterLostResponse) {
    Rig r;
    r.Join(1, "MED_TREATMENT_CABINET");
    r.SearchAll(1, "MED_TREATMENT_CABINET");
    const std::uint32_t c = r.C("MED_TREATMENT_CABINET");
    const LootResult* res = r.svc->Contents(c, 1);
    CHECK(res != nullptr && !res->items.empty());
    if (res == nullptr || res->items.empty()) return;
    // The inventory applies the grant but the response is lost (twice).
    r.inv.failNext = 2;
    r.inv.applyBeforeFailing = true;
    CHECK(r.Do(1, LootAction::TakeItem, "MED_TREATMENT_CABINET", 0) == LootResultCode::Started);
    CHECK(r.svc->Slot(c, 1, 0).state == SlotState::Reserved);
    CHECK_EQ(r.svc->PendingGrants(), 1u);
    // While pending, nobody else can take it.
    r.Join(2, "MED_TREATMENT_CABINET");
    CHECK(r.Do(2, LootAction::TakeItem, "MED_TREATMENT_CABINET", 0) == LootResultCode::RejectedSlotTaken);
    r.Run(3);
    CHECK_EQ(r.svc->PendingGrants(), 0u);
    CHECK(r.svc->Slot(c, 1, 0).state == SlotState::Taken);
    // Every retry carried the same grant id; the item exists exactly once.
    CHECK_EQ(r.inv.applied.size(), 1u);
    CHECK_EQ(r.inv.received[1].size(), 0u);  // applied during the "lost" calls, acknowledged as AlreadyApplied
    // Inventory full: item stays in the container for someone else.
    r.inv.full.insert(2);
    if (res->items.size() > 1) {
        CHECK(r.Do(2, LootAction::TakeItem, "MED_TREATMENT_CABINET", 1) == LootResultCode::RejectedInventory);
        CHECK(r.svc->Slot(c, 1, 1).state == SlotState::Available);
        CHECK(r.Do(1, LootAction::TakeItem, "MED_TREATMENT_CABINET", 1) == LootResultCode::Accepted);
    }
    CHECK(r.Do(1, LootAction::TakeItem, "MED_TREATMENT_CABINET", 99) == LootResultCode::RejectedInvalidSlot);
}

AIS_TEST(Containers_ServerAuthorityRangeAndLifecycle) {
    ContainerServiceConfig client;
    client.serverAuthority = false;
    Rig c(1, client);
    c.Join(1, "DF_CP_NORTH_LOCKER");
    CHECK(c.Do(1, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::RejectedNotAuthority);

    Rig r;
    r.Join(1, "DF_CP_NORTH_LOCKER");
    CHECK(r.svc->Submit(LootRequest{9, 1, r.C("DF_CP_NORTH_LOCKER"), LootAction::Search, 0}) == LootResultCode::RejectedUnknownPlayer);
    CHECK(r.svc->Submit(LootRequest{1, 1001, 9999, LootAction::Search, 0}) == LootResultCode::RejectedUnknownContainer);
    CHECK(r.Do(1, LootAction::Search, "DF_CP_SOUTH_TRUCK") == LootResultCode::RejectedOutOfRange);
    CHECK(r.Do(1, LootAction::TakeItem, "DF_CP_NORTH_LOCKER", 0) == LootResultCode::RejectedNotSearched);
    // 1-4 players; the fifth is refused, recipient keys must be unique and stable.
    for (PlayerId p = 2; p <= 4; ++p) r.Join(p, "DF_CP_NORTH_LOCKER");
    CHECK(!r.svc->ConnectPlayer(5, 105, r.At("DF_CP_NORTH_LOCKER")));
    CHECK(!r.svc->ConnectPlayer(2, 999, r.At("DF_CP_NORTH_LOCKER")));
    r.svc->EndRaid();
    CHECK(r.Do(1, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::RejectedRaidInactive);
    // A new raid starts clean.
    r.svc->BeginRaid(777);
    CHECK(r.Do(1, LootAction::Search, "DF_CP_NORTH_LOCKER") == LootResultCode::Started);
    CHECK(r.svc->GetStats().results[static_cast<std::size_t>(LootResultCode::RejectedRaidInactive)] == 1u);
}
