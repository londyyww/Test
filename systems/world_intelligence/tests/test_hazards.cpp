// Hazards: activation, expiry, fire/smoke, spread, electrical & power, structures,
// gas ignition, duplicates, interactions, exposure, replication, persistence, config.
#include <map>
#include <memory>
#include <string>

#include "TestFramework.h"
#include "llworld/Hazards.h"

using namespace llworld;

namespace {

// Fake infrastructure: named nodes with energized/running flags and health.
class FakeWorld final : public IHazardWorld {
public:
    struct Node {
        std::string id;
        bool energized = true;
        bool running = false;
        double health = 1.0;
        int alarms = 0;
    };
    std::vector<Node> nodes;
    struct Sound {
        HazardSound s;
        double db;
        std::uint32_t hazard;
    };
    std::vector<Sound> sounds;
    std::map<std::pair<PlayerId, std::string>, int> items;

    int Add(const std::string& id) {
        nodes.push_back(Node{id});
        return static_cast<int>(nodes.size() - 1);
    }
    int ResolveNode(std::string_view id) const override {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }
    bool NodeEnergized(int n) const override { return nodes[static_cast<std::size_t>(n)].energized; }
    bool NodeRunning(int n) const override { return nodes[static_cast<std::size_t>(n)].running; }
    void DamageNode(int n, double a) override { nodes[static_cast<std::size_t>(n)].health -= a; }
    void TriggerAlarm(int n) override { ++nodes[static_cast<std::size_t>(n)].alarms; }
    void EmitSound(HazardSound s, const Vec3&, double db, Tick, std::uint32_t h) override { sounds.push_back({s, db, h}); }
    bool PlayerHasItem(PlayerId p, std::string_view i) const override {
        auto it = items.find({p, std::string(i)});
        return it != items.end() && it->second > 0;
    }
    bool ConsumeItem(PlayerId p, std::string_view i) override {
        auto it = items.find({p, std::string(i)});
        if (it == items.end() || it->second <= 0) return false;
        --it->second;
        return true;
    }
    std::size_t CountSounds(HazardSound s) const {
        std::size_t n = 0;
        for (const auto& x : sounds) n += x.s == s ? 1 : 0;
        return n;
    }
};

HazardDef H(const char* id, HazardType t, Vec3 p, double r) {
    HazardDef h;
    h.id = id;
    h.type = t;
    h.position = p;
    h.radiusCm = r;
    return h;
}

struct Rig {
    WorldEventBus bus{512};
    FakeWorld world;
    HazardConfig cfg;
    std::unique_ptr<HazardSystem> sys;
    WeatherSnapshot weather;
    Tick t = 0;
    void Build() { sys = std::make_unique<HazardSystem>(cfg, &bus, &world); }
    std::uint32_t I(const char* id) const { return sys->Find(id); }
    const HazardRuntime& S(const char* id) const { return sys->State(I(id)); }
    void Run(int n) {
        for (int i = 0; i < n; ++i) {
            bus.BeginTick(++t);
            sys->Step(t, weather);
        }
    }
    std::size_t Events(WorldEventType type) const {
        std::size_t n = 0;
        for (const auto& e : bus.Previous()) n += e.type == type ? 1 : 0;
        for (const auto& e : bus.Current()) n += e.type == type ? 1 : 0;
        return n;
    }
};

}  // namespace

AIS_TEST(Hazard_ActivationExpiryAndDuplicateCauses) {
    Rig r;
    HazardDef gas = H("GAS", HazardType::GasLeak, {0, 0, 0}, 300);
    gas.durationTicks = 300;
    gas.growthPerSecond = 0.1;
    r.cfg.hazards = {gas};
    r.Build();
    CHECK(r.sys->IsValid());
    r.Run(1);
    CHECK(r.S("GAS").state == HazardState::Dormant);
    CHECK(r.sys->Activate(r.I("GAS"), 77));
    CHECK(!r.sys->Activate(r.I("GAS"), 77));  // same cause twice
    CHECK(!r.sys->Activate(r.I("GAS"), 78));  // already active
    CHECK(r.S("GAS").state == HazardState::Active);
    r.Run(299);
    CHECK(r.S("GAS").state == HazardState::Active);
    CHECK(r.S("GAS").intensity > 0.5);  // concentration grew
    r.Run(30);
    CHECK(r.S("GAS").state == HazardState::Spent);
    CHECK(!r.sys->Activate(kInvalidIndex, 1));
}

AIS_TEST(Hazard_FireDrivesSmokeWhichDissipatesAfterBurnout) {
    Rig r;
    HazardDef fire = H("FIRE", HazardType::Fire, {0, 0, 0}, 400);
    fire.durationTicks = 300;
    fire.severity = 0.8;
    fire.smoke = "SMOKE";
    fire.damagePerSecond = 20;
    fire.thermalClutter = 0.7;
    fire.activationNoiseDb = 85;
    fire.exposed = false;
    HazardDef smoke = H("SMOKE", HazardType::Smoke, {0, 0, 0}, 1200);
    smoke.severity = 1.0;
    smoke.visibilityReduction = 0.9;
    r.cfg.hazards = {fire, smoke};
    r.cfg.smokeDissipateSeconds = 10;
    r.Build();
    r.Run(1);
    CHECK(r.sys->Activate(r.I("FIRE"), 0));
    CHECK(r.S("SMOKE").state == HazardState::Active);
    r.Run(30);
    const HazardExposure near = r.sys->ExposureAt({100, 0, 0});
    CHECK(near.visibilityReduction > 0.5);
    CHECK(near.damagePerSecond > 10);
    CHECK(near.thermalClutter > 0.5);
    const HazardExposure edge = r.sys->ExposureAt({1000, 0, 0});
    CHECK(edge.visibilityReduction > 0.0 && edge.damagePerSecond < 5);  // smoke only
    CHECK_EQ(r.world.CountSounds(HazardSound::FireIgnite), std::size_t{1});
    r.Run(300);
    CHECK(r.S("FIRE").state == HazardState::Spent && r.S("FIRE").burnedOut);
    CHECK(r.S("SMOKE").state == HazardState::Active);  // lingers
    r.Run(30 * 12);
    CHECK(r.S("SMOKE").state == HazardState::Dormant);
    CHECK_NEAR(r.sys->ExposureAt({100, 0, 0}).visibilityReduction, 0.0, 1e-12);
    CHECK(!r.sys->Activate(r.I("FIRE"), 0));  // burned-out site cannot burn again
}

AIS_TEST(Hazard_FireSpreadsOnlyToNearbyFlammableSitesAndWeatherMatters) {
    auto build = [](double fireSpreadMul, double rain) {
        auto r = std::make_unique<Rig>();
        HazardDef a = H("A", HazardType::Fire, {0, 0, 0}, 300);
        a.spreadRadiusCm = 1500;
        a.spreadIntervalTicks = 30;
        a.spreadChance = 0.5;
        a.durationTicks = 3000;
        a.exposed = true;
        HazardDef b = H("B", HazardType::Fire, {1000, 0, 0}, 300);
        b.flammable = true;
        b.durationTicks = 3000;
        HazardDef c = H("C", HazardType::Fire, {5000, 0, 0}, 300);  // too far
        c.flammable = true;
        HazardDef d = H("D", HazardType::Fire, {800, 0, 0}, 300);   // close but not flammable
        r->cfg.hazards = {a, b, c, d};
        r->weather.fireSpreadMul = fireSpreadMul;
        r->weather.rain = rain;
        r->weather.fireRainSuppression = 2.0;
        r->Build();
        r->Run(1);
        r->sys->Activate(r->I("A"), 0);
        return r;
    };
    auto dry = build(1.8, 0.0);
    dry->Run(600);
    CHECK(dry->S("B").state == HazardState::Active || dry->S("B").burnedOut);
    CHECK(dry->S("C").state == HazardState::Dormant);
    CHECK(dry->S("D").state == HazardState::Dormant);
    CHECK(dry->Events(WorldEventType::FireSpread) <= 1);

    auto wet = build(0.0, 1.0);  // downpour: no spread, exposed fuel burns 3x faster
    wet->Run(1101);
    CHECK(wet->S("B").state == HazardState::Dormant);
    CHECK(wet->S("A").state == HazardState::Spent);
}

AIS_TEST(Hazard_ElectricalFollowsPowerAndWetGroundExtendsIt) {
    Rig r;
    r.world.Add("CABLE");
    HazardDef arc = H("ARC", HazardType::Electrical, {0, 0, 0}, 200);
    arc.powerNode = "CABLE";
    arc.startsActive = true;
    arc.damagePerSecond = 18;
    arc.noiseDb = 60;
    arc.noiseIntervalTicks = 30;
    r.cfg.hazards = {arc};
    r.Build();
    CHECK(r.sys->IsValid());
    r.Run(16);
    CHECK(r.S("ARC").state == HazardState::Active);
    CHECK(r.sys->ExposureAt({150, 0, 0}).damagePerSecond > 0);
    CHECK(r.sys->ExposureAt({300, 0, 0}).damagePerSecond == 0.0);
    r.weather.wetness = 1.0;
    ++r.weather.version;
    r.Run(16);
    CHECK(r.sys->ExposureAt({300, 0, 0}).damagePerSecond > 0);  // puddles carry current
    // Power shutdown kills the hazard; restoring power brings it back.
    r.world.nodes[0].energized = false;
    r.sys->NotifyInfrastructureChanged();
    r.Run(1);
    CHECK(r.S("ARC").state == HazardState::Suppressed);
    CHECK(r.sys->ExposureAt({0, 0, 0}).damagePerSecond == 0.0);
    const auto arcs = r.world.CountSounds(HazardSound::ElectricalArc);
    r.Run(120);
    CHECK_EQ(r.world.CountSounds(HazardSound::ElectricalArc), arcs);
    r.world.nodes[0].energized = true;
    r.sys->NotifyInfrastructureChanged();
    r.Run(1);
    CHECK(r.S("ARC").state == HazardState::Active);
}

AIS_TEST(Hazard_MachineryHazardFollowsRunningState) {
    Rig r;
    const int n = r.world.Add("CRUSHER");
    HazardDef m = H("ZONE", HazardType::Machinery, {0, 0, 0}, 200);
    m.powerNode = "CRUSHER";
    m.damagePerSecond = 60;
    r.cfg.hazards = {m};
    r.Build();
    r.Run(2);
    CHECK(r.S("ZONE").state != HazardState::Active);
    r.world.nodes[static_cast<std::size_t>(n)].running = true;
    r.sys->NotifyInfrastructureChanged();
    r.Run(1);
    CHECK(r.S("ZONE").state == HazardState::Active);
}

AIS_TEST(Hazard_GasIgnitesFromArcAndBecomesFireOnce) {
    Rig r;
    r.world.Add("CABLE");
    HazardDef arc = H("ARC", HazardType::Electrical, {0, 0, 0}, 200);
    arc.powerNode = "CABLE";
    arc.startsActive = true;
    HazardDef gas = H("GAS", HazardType::GasLeak, {300, 0, 0}, 400);
    gas.growthPerSecond = 0.1;
    gas.ignitionThreshold = 0.5;
    gas.ignites = "FIRE";
    gas.burstDamage = 40;
    gas.exposed = false;
    HazardDef fire = H("FIRE", HazardType::Fire, {300, 0, 0}, 300);
    fire.flammable = true;
    fire.durationTicks = 600;
    r.cfg.hazards = {arc, gas, fire};
    r.Build();
    r.Run(1);
    CHECK(r.sys->Activate(r.I("GAS"), 0));
    r.Run(30 * 4);
    CHECK(r.S("GAS").state == HazardState::Active);  // below threshold yet
    r.Run(30 * 2);
    CHECK(r.S("GAS").state == HazardState::Spent);
    CHECK(r.S("FIRE").state == HazardState::Active);
    CHECK_EQ(r.world.CountSounds(HazardSound::GasIgnition), std::size_t{1});
    // The burst is applied once to players in range.
    r.sys->SamplePlayers({{1, {300, 0, 0}}, {2, {5000, 0, 0}}}, r.t);
    CHECK(r.sys->DoseOf(1) >= 40.0);
    CHECK_NEAR(r.sys->DoseOf(2), 0.0, 1e-12);
}

AIS_TEST(Hazard_GunfireIgnitesConcentratedGasButNotThinGas) {
    Rig r;
    HazardDef gas = H("GAS", HazardType::GasLeak, {0, 0, 0}, 400);
    gas.growthPerSecond = 0.05;
    gas.ignitionThreshold = 0.5;
    gas.exposed = false;
    r.cfg.hazards = {gas};
    r.Build();
    r.Run(1);
    r.sys->Activate(0, 0);
    r.Run(30);  // ~0.05 concentration
    WorldEvent shot;
    shot.type = WorldEventType::Gunfire;
    shot.position = {100, 0, 0};
    r.bus.Publish(shot);
    r.Run(2);
    CHECK(r.S("GAS").state == HazardState::Active);
    r.Run(30 * 10);
    r.bus.Publish(shot);
    r.Run(2);
    CHECK(r.S("GAS").state == HazardState::Spent);
}

AIS_TEST(Hazard_StructureStressesWarnsCollapsesAndBlocksRoute) {
    Rig r;
    const int cable = r.world.Add("CABLE");
    HazardDef truss = H("TRUSS", HazardType::Structural, {0, 0, 0}, 600);
    truss.routeId = 7;
    truss.damagesNodes = {"CABLE"};
    truss.burstNodeDamage = 1.0;
    truss.burstDamage = 50;
    truss.stressPerFireSecond = 0.05;
    truss.stressPerLoudEvent = 0.3;
    HazardDef fire = H("FIRE", HazardType::Fire, {200, 0, 0}, 300);
    fire.severity = 1.0;
    r.cfg.hazards = {truss, fire};
    r.Build();
    r.Run(1);
    WorldEvent boom;
    boom.type = WorldEventType::Explosion;
    boom.position = {500, 0, 0};
    r.bus.Publish(boom);
    r.Run(2);
    CHECK_NEAR(r.S("TRUSS").stress, 0.3, 1e-9);
    CHECK(r.S("TRUSS").state == HazardState::Dormant);
    CHECK(!r.sys->Activate(r.I("TRUSS"), 0));  // structures fail by stress only
    r.sys->Activate(r.I("FIRE"), 0);
    r.Run(30 * 5);
    CHECK(r.S("TRUSS").escalated);
    CHECK(r.S("TRUSS").state == HazardState::Active);
    CHECK(!r.sys->IsRouteBlocked(7));
    r.Run(30 * 10);
    CHECK(r.S("TRUSS").collapsed);
    CHECK(r.sys->IsRouteBlocked(7));
    CHECK(r.world.nodes[static_cast<std::size_t>(cable)].health <= 0.0);
    CHECK_EQ(r.world.CountSounds(HazardSound::StructureCollapse), std::size_t{1});
    r.Run(300);
    CHECK_EQ(r.world.CountSounds(HazardSound::StructureCollapse), std::size_t{1});  // only once
}

AIS_TEST(Hazard_ChemicalSpreadsFasterWhenWet) {
    auto grow = [](double wetness) {
        Rig r;
        HazardDef c = H("ACID", HazardType::Chemical, {0, 0, 0}, 200);
        c.maxRadiusCm = 700;
        c.growthPerSecond = 10;
        c.startsActive = true;
        r.cfg.hazards = {c};
        r.weather.wetness = wetness;
        r.Build();
        r.Run(30 * 30);
        return r.S("ACID").radius;
    };
    const double dry = grow(0.0);
    const double wet = grow(1.0);
    CHECK(dry > 200.0 && wet > dry);
    CHECK(wet <= 700.0 + 1e-9);
}

AIS_TEST(Hazard_PlayerInteractionsDedupRangeItemsAndRate) {
    Rig r;
    HazardDef fire = H("FIRE", HazardType::Fire, {0, 0, 0}, 300);
    fire.extinguishItem = "Extinguisher";
    HazardDef gas = H("GAS", HazardType::GasLeak, {2000, 0, 0}, 300);
    gas.playerTriggerable = true;
    r.cfg.hazards = {fire, gas};
    r.Build();
    r.Run(1);
    r.sys->Activate(r.I("FIRE"), 0);
    HazardRequest q;
    q.player = 1;
    q.sequence = 1;
    q.hazard = r.I("FIRE");
    q.action = HazardAction::Extinguish;
    q.playerPosition = {100, 0, 0};
    CHECK(r.sys->Interact(q, r.t) == HazardRequestResult::RejectedMissingItem);
    q.sequence = 2;
    r.world.items[{1, "Extinguisher"}] = 1;
    CHECK(r.sys->Interact(q, r.t) == HazardRequestResult::Applied);
    CHECK(r.sys->Interact(q, r.t) == HazardRequestResult::RejectedDuplicate);
    CHECK(r.S("FIRE").state == HazardState::Spent && !r.S("FIRE").burnedOut);
    q.sequence = 3;
    q.action = HazardAction::CloseValve;
    CHECK(r.sys->Interact(q, r.t) == HazardRequestResult::RejectedUnsupported);
    q.sequence = 4;
    q.hazard = r.I("GAS");
    q.action = HazardAction::Trigger;
    CHECK(r.sys->Interact(q, r.t) == HazardRequestResult::RejectedOutOfRange);
    q.playerPosition = {2000, 100, 0};
    q.sequence = 5;
    r.Run(10);  // refill request tokens (4 used above)
    CHECK(r.sys->Interact(q, r.t) == HazardRequestResult::Applied);
    // Simultaneous players: second trigger of the same valve is a no-op.
    HazardRequest q2 = q;
    q2.player = 2;
    q2.sequence = 1;
    const auto first = r.S("GAS").state;
    CHECK(first == HazardState::Active);
    CHECK(r.sys->Interact(q2, r.t) == HazardRequestResult::RejectedInvalidState);
    // Flood: one player spamming is rate limited.
    int ok = 0;
    for (std::uint32_t s = 100; s < 200; ++s) {
        HazardRequest f = q2;
        f.sequence = s;
        f.action = HazardAction::CloseValve;
        const auto res = r.sys->Interact(f, r.t);
        ok += res == HazardRequestResult::RejectedRateLimited ? 0 : 1;
    }
    CHECK(ok <= 4);
    // At most 4 players are tracked.
    for (PlayerId p = 3; p <= 5; ++p) {
        HazardRequest f = q;
        f.player = p;
        f.sequence = 1;
        const auto res = r.sys->Interact(f, r.t);
        CHECK((p <= 4) == (res != HazardRequestResult::RejectedTooManyPlayers));
    }
}

AIS_TEST(Hazard_ReplicationDeltasAndFacilityPersistence) {
    Rig r;
    HazardDef truss = H("TRUSS", HazardType::Structural, {0, 0, 0}, 500);
    truss.routeId = 3;
    truss.persistence = HazardPersistence::Facility;
    HazardDef temp = H("GAS", HazardType::GasLeak, {0, 0, 0}, 300);  // mission scoped
    r.cfg.hazards = {truss, temp};
    r.Build();
    std::vector<HazardReplicationState> deltas;
    r.sys->CollectReplication(deltas);
    CHECK_EQ(deltas.size(), std::size_t{2});
    deltas.clear();
    r.Run(30);
    r.sys->CollectReplication(deltas);
    CHECK(deltas.empty());  // nothing changed
    for (int i = 0; i < 4; ++i) {
        WorldEvent boom;
        boom.type = WorldEventType::Explosion;
        r.bus.Publish(boom);
        r.Run(16);
    }
    r.sys->Activate(r.I("GAS"), 0);
    r.Run(16);
    CHECK(r.S("TRUSS").collapsed);
    r.sys->CollectReplication(deltas);
    CHECK(deltas.size() == 2);
    const auto rec = r.sys->ExportFacilityDamage();
    CHECK(rec.size() == 1 && rec[0].id == "TRUSS" && rec[0].collapsed);
    Rig next;
    next.cfg = r.cfg;
    next.Build();
    CHECK(next.sys->ImportFacilityDamage(rec));
    CHECK(next.S("TRUSS").collapsed && next.sys->IsRouteBlocked(3));
    CHECK(next.S("GAS").state == HazardState::Dormant);  // temporary hazards reset
    next.Run(1);
    CHECK(!next.sys->ImportFacilityDamage(rec));  // only at mission start
}

AIS_TEST(Hazard_InvalidConfigurationsAreRejected) {
    auto bad = [](std::vector<HazardDef> hs, IHazardWorld* w = nullptr) {
        HazardConfig c;
        c.hazards = std::move(hs);
        HazardSystem s(c, nullptr, w);
        return !s.IsValid();
    };
    HazardDef ok = H("A", HazardType::Fire, {0, 0, 0}, 100);
    CHECK(!bad({ok}));
    CHECK(bad({ok, ok}));  // duplicate id
    HazardDef h = ok;
    h.radiusCm = 0;
    CHECK(bad({h}));
    h = ok;
    h.severity = 2;
    CHECK(bad({h}));
    h = H("E", HazardType::Electrical, {0, 0, 0}, 100);
    CHECK(bad({h}));  // missing power node
    h.powerNode = "NOPE";
    FakeWorld fw;
    CHECK(bad({h}, &fw));  // unresolvable node
    h = H("G", HazardType::GasLeak, {0, 0, 0}, 100);
    h.ignites = "A";
    HazardDef notFire = H("A", HazardType::Smoke, {0, 0, 0}, 100);
    CHECK(bad({h, notFire}));
    h = H("S", HazardType::Structural, {0, 0, 0}, 100);
    h.stressWarn = 2.0;
    CHECK(bad({h}));
    h = H("X", HazardType::Smoke, {0, 0, 0}, 100);
    h.flammable = true;
    CHECK(bad({h}));
    h = ok;
    h.position = {std::nan(""), 0, 0};
    CHECK(bad({h}));
    // INI: unknown keys and missing type.
    std::vector<std::string> errors;
    HazardConfig c;
    LoadHazards(ParseIni("[hazard.Z]\nradius_cm = 5\nflavour = 1\n", errors), c, errors);
    CHECK(errors.size() == 2);
}

AIS_TEST(Hazard_EventFloodingIsBoundedByTheBus) {
    Rig r;
    r.bus = WorldEventBus(16);
    for (int i = 0; i < 100; ++i) {
        HazardDef f = H(("F" + std::to_string(i)).c_str(), HazardType::Fire, {static_cast<double>(i) * 100, 0, 0}, 50);
        r.cfg.hazards.push_back(f);
    }
    r.Build();
    r.Run(1);
    for (std::uint32_t i = 0; i < 100; ++i) r.sys->Activate(i, i + 1);
    CHECK(r.bus.Current().size() <= 16);
    CHECK(r.bus.Dropped() > 0);
    CHECK_EQ(r.sys->ActiveCount(), std::size_t{100});  // state is still correct
    WorldEvent badEvent;
    badEvent.position = {std::nan(""), 0, 0};
    CHECK_EQ(r.bus.Publish(badEvent), std::uint64_t{0});
}
