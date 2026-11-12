// Co-op & multiplayer authority, duplicates, disconnection, persistence, config, flooding, determinism.
#include <string>

#include "EnvTestHelpers.h"
#include "TestFramework.h"

using namespace llenv;
using envtest::Header;
using envtest::Node;
using envtest::Rig;
using A = InteractionAction;
using R = InteractionResult;

namespace {

FacilityConfig Loose() {
    FacilityConfig c;
    c.requestBurst = 1000.0;
    return c;
}

std::string Small() {
    return Header("small") +
           Node("G", "Generator", "A", "", "capacity_kw = 20\nstarts_running = true\nfuel_ticks = 5000\npersistence = World\n") +
           Node("BRK", "Breaker", "A", "G", "capacity_kw = 15\nfuse_installed = false\nfuse_item = Fuse\npersistence = World\n") +
           Node("L", "Light", "A", "BRK", "demand_kw = 2\n") +
           Node("D", "Door", "A", "G", "demand_kw = 1\nstarts_locked = true\nfail_mode = FailSecure\nforce_item = Bar\npersistence = WorldOnExtraction\n", "200, 0, 0") +
           Node("GATE", "SecurityGate", "A", "G", "demand_kw = 2\nmanual_participants = 2\nmanual_work = 100\n", "400, 0, 0");
}

}  // namespace

AIS_TEST(Coop_SimultaneousToggleUsesOptimisticVersioning) {
    Rig r(Small(), AuthorityRole::Server, Loose());
    r.Join(1, "BRK");
    r.Join(2, "BRK");
    r.inv.Give(1, "Fuse");
    r.Run(1);
    CHECK(r.Do(1, "BRK", A::InstallFuse) == R::Applied);
    const std::uint32_t v = r.S("BRK").version;
    // Both players saw version v and press "open" in the same tick.
    CHECK(r.Do(1, "BRK", A::Deactivate, v) == R::Applied);
    CHECK(r.Do(2, "BRK", A::Deactivate, v) == R::RejectedStaleVersion);
    CHECK(!r.S("BRK").closed);
    // Without a version the second request is judged on current state.
    CHECK(r.Do(2, "BRK", A::Deactivate) == R::RejectedInvalidState);
}

AIS_TEST(Coop_DuplicateRequestsAreIdempotent) {
    Rig r(Small(), AuthorityRole::Server, Loose());
    r.Join(1, "G");
    r.Run(1);
    InteractionRequest q;
    q.player = 1;
    q.sequence = 77;
    q.node = r.N("G");
    q.action = A::Deactivate;
    CHECK(r.f->Submit(q) == R::Applied);
    CHECK(r.f->Submit(q) == R::RejectedDuplicate);  // retransmit
    q.action = A::Activate;
    CHECK(r.f->Submit(q) == R::RejectedDuplicate);  // same seq, different payload: still a replay
    CHECK(r.S("G").gen == GeneratorState::Off);
    // Sequences far below the window are rejected; fresh ones work.
    for (std::uint32_t s = 100; s < 200; ++s) {
        InteractionRequest n = q;
        n.sequence = s;
        n.action = A::CancelChannel;
        r.f->Submit(n);
    }
    q.sequence = 78;  // older than the 64-entry window
    CHECK(r.f->Submit(q) == R::RejectedDuplicate);
    q.sequence = 500;
    CHECK(r.f->Submit(q) == R::Applied);
    CHECK_EQ(r.f->Stats().Count(R::RejectedDuplicate), std::uint64_t{3});
}

AIS_TEST(Coop_RangePermissionUnknownAndPlayerCap) {
    Rig r(Small(), AuthorityRole::Server, Loose());
    r.Run(1);
    CHECK(r.Do(9, "G", A::Deactivate) == R::RejectedUnknownPlayer);
    CHECK(r.f->ConnectPlayer(1, {5000, 0, 0}, 0));
    CHECK(r.Do(1, "G", A::Deactivate) == R::RejectedOutOfRange);
    CHECK(r.f->UpdatePlayerPosition(1, {280, 0, 0}));  // within 250 cm * 1.15 latency slack
    CHECK(r.Do(1, "G", A::Deactivate) == R::Applied);
    InteractionRequest bad;
    bad.player = 1;
    bad.sequence = 999;
    bad.node = 12345;
    CHECK(r.f->Submit(bad) == R::RejectedUnknownNode);
    CHECK(!r.f->ConnectPlayer(1, {0, 0, 0}, 0));  // already connected
    CHECK(r.f->ConnectPlayer(2, {0, 0, 0}, 0));
    CHECK(r.f->ConnectPlayer(3, {0, 0, 0}, 0));
    CHECK(r.f->ConnectPlayer(4, {0, 0, 0}, 0));
    CHECK(!r.f->ConnectPlayer(5, {0, 0, 0}, 0));  // 1-4 players
    CHECK(!r.f->ConnectPlayer(kNoPlayer, {0, 0, 0}, 0));
    CHECK(!r.f->UpdatePlayerPosition(2, {std::nan(""), 0, 0}));
}

AIS_TEST(Coop_DisconnectPausesSharedWorkAndReconnectResumes) {
    Rig r(Small(), AuthorityRole::Server, Loose());
    r.Join(1, "GATE");
    r.Join(2, "GATE");
    r.Run(1);
    CHECK(r.Do(1, "G", A::Deactivate) == R::RejectedOutOfRange);  // gate is 4 m from the generator
    r.MoveTo(1, "G");
    CHECK(r.Do(1, "G", A::Deactivate) == R::Applied);
    r.MoveTo(1, "GATE");
    CHECK(r.Do(1, "GATE", A::Crank) == R::ChannelStarted);
    CHECK(r.Do(2, "GATE", A::Crank) == R::ChannelJoined);
    r.Run(20);
    CHECK_NEAR(r.S("GATE").progress, 40.0, 1e-9);
    CHECK(r.f->DisconnectPlayer(2));
    CHECK(r.Do(2, "GATE", A::Crank) == R::RejectedDisconnected);
    r.Run(30);
    CHECK_NEAR(r.S("GATE").progress, 40.0, 1e-9);  // one player cannot hold it alone
    CHECK_EQ(r.f->Channels().size(), std::size_t{1});
    // Walking away also drops a participant.
    r.f->UpdatePlayerPosition(1, {5000, 0, 0});
    r.Run(1);
    CHECK(r.f->Channels().empty());
    CHECK_NEAR(r.S("GATE").progress, 40.0, 1e-9);  // progress stays on the gate
    // Reconnect: the dedup history survives, so an old request is still a replay.
    CHECK(r.f->ConnectPlayer(2, r.f->Def(r.N("GATE")).position, 0));
    InteractionRequest replay;
    replay.player = 2;
    replay.sequence = r.seq[2] - 1;  // the earlier Crank that was accepted
    replay.node = r.N("GATE");
    replay.action = A::Crank;
    CHECK(r.f->Submit(replay) == R::RejectedDuplicate);
    r.MoveTo(1, "GATE");
    CHECK(r.Do(1, "GATE", A::Crank) == R::ChannelStarted);
    CHECK(r.Do(2, "GATE", A::Crank) == R::ChannelJoined);
    r.Run(30);
    CHECK(r.S("GATE").door == DoorState::Open);
}

AIS_TEST(Authority_ClientsCannotChangePowerAndMirrorReplicatedState) {
    Rig server(Small(), AuthorityRole::Server, Loose());
    Rig client(Small(), AuthorityRole::Client, Loose());
    CHECK(!client.f->ConnectPlayer(1, {0, 0, 0}, 0));
    InteractionRequest q;
    q.player = 1;
    q.sequence = 1;
    q.node = client.N("G");
    q.action = A::Deactivate;
    CHECK(client.f->Submit(q) == R::RejectedNotAuthority);
    CHECK(!client.f->ServerApplyDamage(client.N("G"), 1.0));
    CHECK(!client.f->ServerTriggerAlarm(client.N("G")));
    client.Run(100);
    CHECK_EQ(client.f->CurrentTick(), Tick{0});

    server.Join(1, "G");
    server.inv.Give(1, "Fuse");
    server.Run(1);
    std::vector<NodeReplicationState> deltas;
    server.f->CollectReplication(deltas);
    CHECK_EQ(deltas.size(), server.f->NodeCount());  // first sync: everything
    CHECK_EQ(client.f->ApplyReplication(deltas), server.f->NodeCount());
    CHECK_EQ(server.f->ApplyReplication(deltas), std::size_t{0});  // servers never take mirrored state
    deltas.clear();
    server.f->CollectReplication(deltas);
    CHECK(deltas.empty());  // nothing changed
    server.MoveTo(1, "BRK");
    CHECK(server.Do(1, "BRK", A::InstallFuse) == R::Applied);  // closes the circuit to L
    server.f->CollectReplication(deltas);
    CHECK(deltas.size() >= 2 && deltas.size() <= 3);  // breaker, light (+ nothing else)
    CHECK(client.f->ApplyReplication(deltas) == deltas.size());
    CHECK(client.f->ApplyReplication(deltas) == 0);  // stale versions ignored
    for (NodeIndex i = 0; i < server.f->NodeCount(); ++i) {
        const auto a = server.f->MakeReplicationState(i);
        const auto b = client.f->MakeReplicationState(i);
        CHECK_EQ(a.flags, b.flags);
        CHECK_EQ(a.primaryState, b.primaryState);
    }
    CHECK(client.S("L").lit);
}

AIS_TEST(Persistence_WorldStateKeepsConditionNotSwitches) {
    Rig r(Small(), AuthorityRole::Server, Loose());
    r.Join(1, "BRK");
    r.inv.Give(1, "Fuse");
    r.inv.Give(1, "Bar");
    r.Run(1);
    CHECK(r.Do(1, "BRK", A::InstallFuse) == R::Applied);
    CHECK(r.Do(1, "BRK", A::Deactivate) == R::Applied);  // a switch position
    CHECK(r.Do(1, "L", A::Deactivate) == R::Applied);    // another one
    r.Run(100);  // burn some fuel first
    r.MoveTo(1, "G");
    CHECK(r.Do(1, "G", A::Deactivate) == R::Applied);
    r.MoveTo(1, "D");
    CHECK(r.Do(1, "D", A::Force) == R::Applied);
    r.Run(100);
    CHECK(r.f->ServerApplyDamage(r.N("G"), 0.6));
    const int consumes = r.inv.consumeCalls;

    const WorldStateRecord notExtracted = r.f->ExportWorldState(false);
    const WorldStateRecord extracted = r.f->ExportWorldState(true);
    CHECK_EQ(notExtracted.nodes.size(), std::size_t{2});  // G, BRK
    CHECK_EQ(extracted.nodes.size(), std::size_t{3});     // + D (WorldOnExtraction)
    CHECK_EQ(r.inv.consumeCalls, consumes);               // exporting never touches inventory
    const std::string text = extracted.Serialize();
    CHECK(text.find("|0|") == std::string::npos || text.find("BRK|1|1|") != std::string::npos);
    WorldStateRecord parsed;
    std::string err;
    CHECK(WorldStateRecord::Parse(text, parsed, &err));
    CHECK_EQ(parsed.nodes.size(), std::size_t{3});

    // Next mission: fresh switches, persisted condition.
    Rig next(Small(), AuthorityRole::Server, Loose());
    const ImportReport rep = next.f->ImportWorldState(parsed);
    CHECK(rep.ok);
    CHECK_EQ(rep.applied, std::size_t{3});
    next.Join(1, "BRK");
    next.Run(1);
    CHECK(next.S("BRK").fuseInstalled);
    CHECK(next.S("BRK").closed);     // switch position reset to the definition
    CHECK(next.S("L").switchedOn);   // mission-scoped
    CHECK_NEAR(next.S("G").health, 0.4, 1e-9);
    CHECK(next.S("G").gen == GeneratorState::Running);  // starts_running from the definition
    CHECK(next.S("D").lockBroken);
    CHECK(next.S("L").powered);
    CHECK(next.S("G").fuel < 5000.0 - 99.0);  // fuel burnt last mission stays burnt

    // Mission rules are enforced.
    CHECK(!next.f->ImportWorldState(parsed).ok);  // mission already started
    Rig other(Header("other_site") + Node("G", "Generator", "A", "", "capacity_kw = 1\n"), AuthorityRole::Server, Loose());
    CHECK(!other.f->ImportWorldState(parsed).ok);  // wrong facility
    WorldStateRecord junk = parsed;
    junk.nodes.push_back({"NOPE", 1.0, true, -1.0, false});
    junk.nodes.push_back({"L", 0.0, true, -1.0, false});  // Mission policy: ignored
    Rig fresh(Small(), AuthorityRole::Server, Loose());
    const ImportReport rep2 = fresh.f->ImportWorldState(junk);
    CHECK(rep2.ok && rep2.ignored.size() == 2);
    CHECK(!WorldStateRecord::Parse("format=1\nnode=G|abc|1|0|0\n", parsed, &err));
    CHECK(!WorldStateRecord::Parse("garbage", parsed, &err));
}

AIS_TEST(Persistence_MissionSnapshotRestoresExactly) {
    auto drive = [](Rig& r, int from, int to) {
        for (int t = from; t < to; ++t) {
            if (t == 5) r.Do(1, "G", A::Deactivate);
            if (t == 40) r.Do(1, "G", A::Activate);
            if (t == 60) r.Do(1, "GATE", A::Crank);
            r.f->Step();
        }
    };
    Rig a(Small(), AuthorityRole::Server, Loose());
    a.Join(1, "G");
    a.Run(1);
    drive(a, 0, 30);
    const MissionSnapshot snap = a.f->Capture();
    const auto seqAtSnap = a.seq;
    drive(a, 30, 200);
    const std::uint64_t expected = a.f->StateHash();

    a.f->Restore(snap);
    a.seq = seqAtSnap;
    CHECK(a.f->CurrentTick() == snap.tick);
    drive(a, 30, 200);
    CHECK_EQ(a.f->StateHash(), expected);
    Rig c(Small(), AuthorityRole::Client, Loose());
    CHECK(!c.f->Restore(snap));
}

AIS_TEST(Config_InvalidDefinitionsAreRejected) {
    auto fails = [](const std::string& ini) {
        FacilityDef d;
        return !LoadFacilityIni(ini, d).ok;
    };
    const std::string g = Node("G", "Generator", "A", "", "capacity_kw = 5\n");
    CHECK(!fails(Header() + g + Node("L", "Light", "A", "G", "demand_kw = 1\n")));
    CHECK(fails(Header() + g + Node("L", "Light", "A", "NOPE")));                      // unknown fed_by
    CHECK(fails(Header() + g + Node("L", "Light", "B", "G")));                         // cross-network
    CHECK(fails(Header() + g + Node("J1", "Junction", "A", "G, J2") + Node("J2", "Junction", "A", "J1")));  // cycle
    CHECK(fails(Header() + g + Node("L", "Light", "A", "G") + Node("L2", "Light", "A", "L")));             // consumer feeds
    CHECK(fails(Header() + g + Node("G2", "Generator", "A", "G", "capacity_kw = 5\n")));                  // fed source
    CHECK(fails(Header() + g + g));                                                                        // duplicate id
    CHECK(fails(Header() + Node("G", "Generator", "A", "", "capacity_kw = 0\n")));
    CHECK(fails(Header() + g + Node("L", "Light", "A", "")));                         // consumer without feed
    CHECK(fails(Header() + g + Node("B", "Breaker", "A", "G")));                      // breaker without rating
    CHECK(fails(Header() + g + Node("D", "Door", "A", "G", "alarm = G\n")));          // alarm must be an Alarm
    CHECK(fails(Header() + g + Node("D", "Door", "A", "G", "manual_participants = 9\n")));
    CHECK(fails(Header() + g + Node("L", "Light", "A", "G", "priority = 101\n")));
    CHECK(fails(Header() + g + Node("L", "Light", "A", "G", "health = 1.5\n")));
    CHECK(fails(Header() + g + Node("L", "Light", "A", "G", "brightness = 3\n")));    // typo
    CHECK(fails(Header() + g + Node("L", "Lamp", "A", "G")));                          // unknown type
    CHECK(fails(Header() + g + "[node.X]\nnetwork = A\nfed_by = G\n"));                // missing type
    CHECK(fails(Header() + g + "[nodes.Y]\n"));                                         // unknown section
    CHECK(fails(Header() + Node("G", "Generator", "A", "", "capacity_kw = 5\nauto_start = true\n")));
    CHECK(fails(Header() + g + Node("T", "Light", "A", "G", "controls = G\n")));
    CHECK(fails(Header() + g + Node("L", "Light", "A", "G", "demand_kw = abc\n")));
    CHECK(fails(Header() + g + Node("L", "Light", "A", "G", "position = 1, 2\n")));
    CHECK(fails(""));
    // An invalid definition yields an inert facility rather than undefined behaviour.
    FacilityDef bad;
    bad.nodes.push_back(NodeDef{});
    Facility f(bad);
    CHECK(!f.IsValid());
    f.Step();
    CHECK_EQ(f.CurrentTick(), Tick{0});
}

AIS_TEST(Config_ShippedFacilityLoads) {
    FacilityDef d;
    const LoadResult r = LoadFacilityFile(ENV_DATA_DIR "/relay_outpost.ini", d);
    for (const auto& e : r.errors) std::printf("    %s\n", e.c_str());
    CHECK(r.ok);
    CHECK(d.nodes.size() >= 20);
    CHECK(!LoadFacilityFile("/nonexistent.ini", d).ok);
}

AIS_TEST(Flooding_RequestsEventsAndTimersStayBounded) {
    FacilityConfig cfg;  // default limits
    cfg.maxEventsPerTick = 20;
    Rig r(Small(), AuthorityRole::Server, cfg);
    r.Join(1, "L");
    r.Run(1);
    int applied = 0;
    for (int i = 0; i < 1000; ++i) {
        applied += Succeeded(r.Do(1, "L", (i % 2) == 0 ? A::Deactivate : A::Activate)) ? 1 : 0;
    }
    CHECK(applied <= static_cast<int>(cfg.requestBurst));
    CHECK(r.f->Stats().Count(R::RejectedRateLimited) >= 990u);
    // Sustained: refill 0.25/tick -> ~7.5/s.
    int sustained = 0;
    for (int t = 0; t < 120; ++t) {
        r.f->Step();
        sustained += Succeeded(r.Do(1, "L", (t % 2) == 0 ? A::Deactivate : A::Activate)) ? 1 : 0;
    }
    CHECK(sustained <= 32);
    // A second player is not starved by the first.
    r.Join(2, "L");
    CHECK(r.Do(2, "L", r.S("L").switchedOn ? A::Deactivate : A::Activate) == R::Applied);

    // Server-side storm: an explosion wrecks 60 lights in one tick.
    std::string many = Header() + Node("G", "Generator", "A", "", "capacity_kw = 500\nstarts_running = true\n");
    for (int i = 0; i < 60; ++i) many += Node("L" + std::to_string(i), "Light", "A", "G", "demand_kw = 1\n");
    Rig s(many, AuthorityRole::Server, cfg);
    s.Run(1);
    s.f->Step();  // fresh tick budget
    const auto before = s.rec.events.size();
    for (int i = 0; i < 60; ++i) s.f->ServerApplyDamage(s.N(("L" + std::to_string(i)).c_str()), 2.0);
    CHECK(s.rec.events.size() - before <= cfg.maxEventsPerTick);
    CHECK(s.f->Stats().eventsDropped > 0);
    CHECK(!s.S("L59").lit);  // state is still correct even when events are dropped
}

AIS_TEST(Flooding_DoorSpamCannotGrowTimerHeapUnbounded) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 10\nstarts_running = true\n") +
                            Node("D", "Door", "A", "G", "demand_kw = 1\nmotor_ticks = 1\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "D");
    r.Run(1);
    for (int i = 0; i < 5000; ++i) {
        r.Do(1, "D", (i % 2) == 0 ? A::Activate : A::Deactivate);
        r.f->Step();
    }
    const MissionSnapshot s = r.f->Capture();
    CHECK(s.timers.size() <= 8 * r.f->NodeCount() + 64);
}

namespace {

std::vector<std::uint64_t> Scripted() {
    FacilityDef d;
    LoadFacilityFile(ENV_DATA_DIR "/relay_outpost.ini", d);
    Facility f(d);
    envtest::FakeInventory inv;
    envtest::Recorder rec;
    f.SetInventory(&inv);
    f.SetObserver(&rec);
    f.SetAcousticSink(&rec);
    inv.Give(1, "Fuse");
    inv.Give(2, "SpareParts");
    f.ConnectPlayer(1, d.nodes[static_cast<std::size_t>(d.Find("GEN_MAIN"))].position, 0);
    f.ConnectPlayer(2, d.nodes[static_cast<std::size_t>(d.Find("GEN_BACKUP"))].position, 0);
    std::uint32_t seq = 0;
    auto act = [&](PlayerId p, const char* node, A a) {
        InteractionRequest q;
        q.player = p;
        q.sequence = ++seq;
        q.node = f.Find(node);
        q.action = a;
        f.Submit(q);
    };
    std::vector<std::uint64_t> hashes;
    for (int t = 0; t < 2400; ++t) {
        if (t == 2) act(1, "GEN_MAIN", A::Activate);
        if (t == 5) act(2, "GEN_BACKUP", A::Repair);
        if (t == 300) {
            f.UpdatePlayerPosition(1, d.nodes[static_cast<std::size_t>(d.Find("BRK_INDUSTRIAL"))].position);
            act(1, "BRK_INDUSTRIAL", A::InstallFuse);
        }
        if (t == 900) f.ServerApplyDamage(f.Find("GEN_MAIN"), 0.7);
        f.Step();
        hashes.push_back(f.StateHash());
    }
    return hashes;
}

}  // namespace

AIS_TEST(Determinism_FacilityScenarioReplaysIdentically) {
    const auto a = Scripted();
    const auto b = Scripted();
    CHECK(a == b);
    CHECK(a.front() != a.back());
}
