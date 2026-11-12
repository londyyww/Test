// Power grid: generators, distribution, circuits, overloads, damage, backup, batteries.
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

// G(10 kW) -> BUS -> BRK(8 kW) -> L1, L2 (3 kW) ; BUS -> D (door 1 kW)
std::string BasicGrid(const std::string& genExtra = "", const std::string& brkExtra = "") {
    return Header() + Node("G", "Generator", "A", "", "capacity_kw = 10\nstartup_ticks = 30\nnoise_db = 70\n" + genExtra) +
           Node("BUS", "Junction", "A", "G") + Node("BRK", "Breaker", "A", "BUS", "capacity_kw = 8\ntrip_delay_ticks = 20\n" + brkExtra) +
           Node("L1", "Light", "A", "BRK", "demand_kw = 3\n", "100, 0, 0") + Node("L2", "Light", "A", "BRK", "demand_kw = 3\n", "-100, 0, 0") +
           Node("D", "Door", "A", "BUS", "demand_kw = 1\n");
}

}  // namespace

AIS_TEST(Generator_ActivationStartupAndShutdown) {
    Rig r(BasicGrid(), AuthorityRole::Server, Loose());
    CHECK(r.load.ok);
    r.Join(1, "G");
    r.Run(1);
    CHECK(!r.S("L1").powered);
    CHECK(r.Do(1, "G", A::Activate) == R::Applied);
    CHECK(r.S("G").gen == GeneratorState::Starting);
    CHECK(r.Do(1, "G", A::Activate) == R::RejectedInvalidState);  // already starting
    CHECK_EQ(r.rec.CountSounds(SoundKind::GeneratorStart), std::size_t{1});
    r.Run(29);
    CHECK(!r.S("L1").powered);  // still spinning up
    r.Run(1);
    CHECK(r.S("G").gen == GeneratorState::Running);
    CHECK(r.S("L1").powered && r.S("L1").lit);
    CHECK(r.S("D").powered);
    CHECK(!r.rec.continuous.empty() && r.rec.continuous.back().second);
    CHECK(r.Do(1, "G", A::Deactivate) == R::Applied);
    CHECK(r.S("G").gen == GeneratorState::Off);
    CHECK(!r.S("L1").powered && !r.S("L1").lit && !r.S("D").powered);
    CHECK(!r.rec.continuous.back().second);
    CHECK(r.Do(1, "G", A::Deactivate) == R::RejectedInvalidState);
}

AIS_TEST(Generator_FuelRunsOutAndRefuelConsumesPlayerItem) {
    Rig r(BasicGrid("fuel_ticks = 100\nfuel_item = Diesel\nrefuel_amount_ticks = 500\nstarts_running = true\n"), AuthorityRole::Server, Loose());
    r.Join(1, "G");
    r.Run(1);
    CHECK(r.S("G").gen == GeneratorState::Running);
    r.Run(99);
    CHECK(r.S("G").gen == GeneratorState::Running);
    r.Run(2);
    CHECK(r.S("G").gen == GeneratorState::Off);
    CHECK(r.Do(1, "G", A::Activate) == R::RejectedInvalidState);  // dry
    CHECK(r.Do(1, "G", A::Refuel) == R::RejectedMissingItem);
    r.inv.Give(1, "Diesel");
    CHECK(r.Do(1, "G", A::Refuel) == R::Applied);
    CHECK_EQ(r.inv.Count(1, "Diesel"), 0);
    CHECK(r.Do(1, "G", A::Activate) == R::Applied);
    r.Run(31);
    CHECK(r.S("G").gen == GeneratorState::Running);
}

AIS_TEST(Distribution_BreakerIsolatesOnlyItsBranch) {
    Rig r(BasicGrid("starts_running = true\n"), AuthorityRole::Server, Loose());
    r.Join(1, "BRK");
    r.Run(1);
    CHECK(r.S("L1").powered && r.S("D").powered);
    CHECK(r.Do(1, "BRK", A::Deactivate) == R::Applied);
    CHECK(!r.S("L1").powered && !r.S("L2").powered);
    CHECK(r.S("D").powered);  // fed straight from the bus
    CHECK(!r.S("BRK").energized && r.S("BUS").energized);
    CHECK(r.Do(1, "BRK", A::Activate) == R::Applied);
    CHECK(r.S("L1").powered);
    CHECK(r.rec.CountEvents(FacilityEventType::BreakerState) == 2);
}

AIS_TEST(Distribution_MultipleNetworksAreIndependentAndSourcesPool) {
    const std::string ini = Header() + Node("G1", "Generator", "North", "", "capacity_kw = 4\nstarts_running = true\n") +
                            Node("G2", "Generator", "North", "", "capacity_kw = 4\nstarts_running = true\n") +
                            Node("BUS", "Junction", "North", "G1, G2") +
                            Node("M", "Machinery", "North", "BUS", "demand_kw = 7\nstarts_on = true\n") +
                            Node("B", "Battery", "South", "", "capacity_kw = 2\nbattery_capacity_kw_ticks = 1000\n") +
                            Node("SL", "Light", "South", "B", "demand_kw = 1\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    CHECK(r.load.ok);
    r.Join(1, "G1");
    r.Run(1);
    CHECK_EQ(r.f->Networks().size(), std::size_t{2});
    CHECK(r.S("M").powered);  // 7 kW needs both 4 kW generators pooled
    CHECK(r.S("SL").powered);
    CHECK(r.Do(1, "G2", A::Deactivate) == R::Applied);
    CHECK(!r.S("M").powered);  // 4 kW is not enough
    CHECK(r.S("SL").powered);  // other network untouched
    const auto& nets = r.f->Networks();
    CHECK(nets[0].name == "North" && nets[0].shedConsumers == 1);
}

AIS_TEST(Overload_BreakerTripsAfterDelayAndResetRetripsUnderLoad) {
    Rig r(BasicGrid("starts_running = true\n"), AuthorityRole::Server, Loose());
    r.Join(1, "BRK");
    // 3 kW lights; add a third light by raising L2's draw is not possible at
    // runtime, so overload the 8 kW breaker with a heavier grid instead.
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 50\nstarts_running = true\n") +
                            Node("BRK", "Breaker", "A", "G", "capacity_kw = 8\ntrip_delay_ticks = 20\n") +
                            Node("L1", "Light", "A", "BRK", "demand_kw = 3\n") + Node("L2", "Light", "A", "BRK", "demand_kw = 3\n") +
                            Node("L3", "Light", "A", "BRK", "demand_kw = 3\nstarts_on = false\n");
    Rig o(ini, AuthorityRole::Server, Loose());
    o.Join(1, "BRK");
    o.Run(1);
    CHECK(o.S("L1").powered && !o.S("L3").powered);
    CHECK(o.Do(1, "L3", A::Activate) == R::Applied);  // 9 kW > 8 kW rating
    CHECK(o.S("L3").powered);
    o.Run(19);
    CHECK(!o.S("BRK").tripped);
    o.Run(1);
    CHECK(o.S("BRK").tripped);
    CHECK(!o.S("L1").powered && !o.S("L3").powered);
    CHECK_EQ(o.rec.CountSounds(SoundKind::BreakerTrip), std::size_t{1});
    CHECK(o.Do(1, "BRK", A::Activate) == R::RejectedInvalidState);  // must reset
    CHECK(o.Do(1, "BRK", A::ResetBreaker) == R::Applied);
    o.Run(21);
    CHECK(o.S("BRK").tripped);  // load still there: trips again
    CHECK(o.Do(1, "L3", A::Deactivate) == R::Applied);
    CHECK(o.Do(1, "BRK", A::ResetBreaker) == R::Applied);
    o.Run(60);
    CHECK(!o.S("BRK").tripped && o.S("L1").powered);
    // A brief overload that is removed before the delay does not trip.
    CHECK(o.Do(1, "L3", A::Activate) == R::Applied);
    o.Run(10);
    CHECK(o.Do(1, "L3", A::Deactivate) == R::Applied);
    o.Run(30);
    CHECK(!o.S("BRK").tripped);
}

AIS_TEST(Overload_ShortageShedsLowPriorityAndGeneratorTripsWhenOverdriven) {
    const std::string ini = Header() +
                            Node("G", "Generator", "A", "", "capacity_kw = 10\nstarts_running = true\noverload_tolerance = 1.2\noverload_trip_ticks = 60\n") +
                            Node("CRIT", "Terminal", "A", "G", "demand_kw = 4\npriority = 90\n") +
                            Node("MID", "Light", "A", "G", "demand_kw = 4\npriority = 50\n") +
                            Node("LOW", "Machinery", "A", "G", "demand_kw = 4\npriority = 10\nstarts_on = false\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "G");
    r.Run(1);
    CHECK(r.S("CRIT").powered && r.S("MID").powered);
    CHECK(r.Do(1, "LOW", A::Activate) == R::Applied);  // 12 kW requested of 10 kW: within tolerance
    CHECK(r.S("CRIT").powered && r.S("MID").powered && !r.S("LOW").powered);
    CHECK_EQ(r.f->Networks()[0].shedConsumers, std::size_t{1});
    r.Run(200);
    CHECK(r.S("G").gen == GeneratorState::Running);  // 1.2 ratio is tolerated
    // Push past tolerance: generator trips after the warning period.
    const std::string heavy = Header() +
                              Node("G", "Generator", "A", "", "capacity_kw = 10\nstarts_running = true\noverload_tolerance = 1.2\noverload_trip_ticks = 60\n") +
                              Node("M1", "Machinery", "A", "G", "demand_kw = 8\nstarts_on = true\n") +
                              Node("M2", "Machinery", "A", "G", "demand_kw = 8\nstarts_on = false\n");
    Rig h(heavy, AuthorityRole::Server, Loose());
    h.Join(1, "G");
    h.Run(1);
    CHECK(h.Do(1, "M2", A::Activate) == R::Applied);
    CHECK(h.f->Networks()[0].overloaded);
    CHECK_EQ(h.rec.CountEvents(FacilityEventType::NetworkOverloaded), std::size_t{1});
    h.Run(59);
    CHECK(h.S("G").gen == GeneratorState::Running);
    h.Run(1);
    CHECK(h.S("G").gen == GeneratorState::Tripped);
    CHECK(!h.S("M1").powered);
    CHECK_EQ(h.rec.CountSounds(SoundKind::GeneratorFailure), std::size_t{1});
    CHECK(h.Do(1, "M2", A::Deactivate) == R::Applied);
    CHECK(h.Do(1, "G", A::Activate) == R::Applied);  // restart after trip
}

AIS_TEST(Damage_ReducesOutputCausesStartFailuresDestroysAndRepairs) {
    const std::string ini = Header() +
                            Node("G", "Generator", "A", "",
                                 "capacity_kw = 10\nhealth = 0.3\nstart_fail_chance_damaged = 0.5\nstartup_ticks = 5\nrepair_item = Parts\nrepair_ticks = 60\nnoise_db = 70\n") +
                            Node("M", "Machinery", "A", "G", "demand_kw = 6\nstarts_on = true\n") +
                            Node("T", "Terminal", "A", "G", "demand_kw = 2\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "G");
    r.Run(1);
    int failures = 0, attempts = 0;
    while (r.S("G").gen != GeneratorState::Starting && attempts < 50) {
        CHECK(r.Do(1, "G", A::Activate) == R::Applied);
        ++attempts;
        if (r.S("G").gen != GeneratorState::Starting) ++failures;
    }
    CHECK(attempts < 50);
    CHECK_EQ(static_cast<std::size_t>(failures), r.rec.CountSounds(SoundKind::GeneratorFailure));
    r.Run(6);
    CHECK(r.S("G").gen == GeneratorState::Running);
    CHECK(r.S("T").powered);   // 5 kW available (damaged) -> terminal only
    CHECK(!r.S("M").powered);
    CHECK(r.f->ServerApplyDamage(r.N("G"), 1.0));
    CHECK(r.S("G").gen == GeneratorState::Destroyed && r.S("G").destroyed);
    CHECK(!r.S("T").powered);
    CHECK(r.Do(1, "G", A::Activate) == R::RejectedDestroyed);
    CHECK(!r.f->ServerApplyDamage(r.N("G"), 1.0));
    CHECK(r.Do(1, "G", A::Repair) == R::RejectedMissingItem);
    r.inv.Give(1, "Parts");
    CHECK(r.Do(1, "G", A::Repair) == R::ChannelStarted);
    r.Run(59);
    CHECK(r.S("G").destroyed);
    r.Run(1);
    CHECK(!r.S("G").destroyed && r.S("G").gen == GeneratorState::Off);
    CHECK_EQ(r.inv.Count(1, "Parts"), 0);
    CHECK_EQ(r.rec.CountEvents(FacilityEventType::Repaired), std::size_t{1});
    CHECK(r.Do(1, "G", A::Activate) == R::Applied);  // healthy: no failure roll
    r.Run(6);
    CHECK(r.S("M").powered && r.S("T").powered);
}

AIS_TEST(Damage_StartFailuresAreDeterministic) {
    auto run = []() {
        const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 10\nhealth = 0.2\nstart_fail_chance_damaged = 0.6\nstartup_ticks = 1\n") +
                                Node("L", "Light", "A", "G", "demand_kw = 1\n");
        Rig r(ini, AuthorityRole::Server, Loose());
        r.Join(1, "G");
        r.Run(1);
        std::vector<int> outcome;
        for (int i = 0; i < 12; ++i) {
            r.Do(1, "G", A::Activate);
            outcome.push_back(r.S("G").gen == GeneratorState::Starting ? 1 : 0);
            r.Run(2);
            r.Do(1, "G", A::Deactivate);
        }
        return outcome;
    };
    CHECK(run() == run());
}

AIS_TEST(Backup_AutoStartsOnPrimaryLossAndStopsOnRestore) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 10\nstarts_running = true\nstartup_ticks = 10\n") +
                            Node("BK", "BackupGenerator", "A", "", "capacity_kw = 5\nauto_start = true\nauto_start_delay_ticks = 40\nstartup_ticks = 20\n") +
                            Node("BUS", "Junction", "A", "G, BK") + Node("T", "Terminal", "A", "BUS", "demand_kw = 2\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "G");
    r.Run(1);
    CHECK(r.S("T").powered && r.S("BK").gen == GeneratorState::Off);
    CHECK(r.Do(1, "G", A::Deactivate) == R::Applied);
    CHECK(!r.S("T").powered);
    r.Run(39);
    CHECK(r.S("BK").gen == GeneratorState::Off);
    r.Run(1);
    CHECK(r.S("BK").gen == GeneratorState::Starting && r.S("BK").autoStarted);
    r.Run(20);
    CHECK(r.S("BK").gen == GeneratorState::Running && r.S("T").powered);
    CHECK(r.Do(1, "G", A::Activate) == R::Applied);
    r.Run(10);
    CHECK(r.S("G").gen == GeneratorState::Running);
    CHECK(r.S("BK").gen == GeneratorState::Off);  // stood down
    CHECK(r.S("T").powered);
}

AIS_TEST(Backup_DoesNotStartForAFacilityThatWasAlreadyDead) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 10\n") +
                            Node("BK", "BackupGenerator", "A", "", "capacity_kw = 5\nauto_start = true\nauto_start_delay_ticks = 5\n") +
                            Node("BUS", "Junction", "A", "G, BK") + Node("T", "Terminal", "A", "BUS", "demand_kw = 2\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Run(300);
    CHECK(r.S("BK").gen == GeneratorState::Off);
    CHECK(!r.S("T").powered);
}

AIS_TEST(Battery_DischargesToEmptyAndRechargesFromSurplus) {
    const std::string ini = Header() +
                            Node("G", "Generator", "A", "", "capacity_kw = 10\nstartup_ticks = 1\n") +
                            Node("B", "Battery", "A", "", "capacity_kw = 2\nbattery_capacity_kw_ticks = 200\nbattery_charge_kw = 4\n") +
                            Node("BUS", "Junction", "A", "G, B") + Node("L", "Light", "A", "BUS", "demand_kw = 2\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "G");
    r.Run(1);
    CHECK(r.S("L").powered);
    CHECK_NEAR(r.S("B").batteryRateKw, 2.0, 1e-9);
    r.Run(99);
    CHECK(r.S("L").powered);
    r.Run(1);
    CHECK(!r.S("L").powered);  // 200 kW-ticks / 2 kW = 100 ticks
    CHECK_NEAR(r.S("B").charge, 0.0, 1e-9);
    CHECK(r.Do(1, "G", A::Activate) == R::Applied);
    r.Run(2);
    CHECK(r.S("L").powered);
    CHECK_NEAR(r.S("B").batteryRateKw, -4.0, 1e-9);  // surplus 8 kW, capped at 4 kW charging
    r.Run(51);
    CHECK_NEAR(r.S("B").charge, 200.0, 1e-9);
    CHECK_NEAR(r.S("B").batteryRateKw, 0.0, 1e-9);
}

AIS_TEST(Performance_EventDrivenIdleTicksDoNotResolve) {
    std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 1000\nstarts_running = true\nnoise_pulse_ticks = 300\n");
    for (int b = 0; b < 10; ++b) {
        const std::string brk = "BRK" + std::to_string(b);
        ini += Node(brk, "Breaker", "A", "G", "capacity_kw = 200\n");
        for (int k = 0; k < 30; ++k) {
            ini += Node("L" + std::to_string(b) + "_" + std::to_string(k), k % 3 == 0 ? "Door" : "Light", "A", brk, "demand_kw = 1\n");
        }
    }
    Rig r(ini, AuthorityRole::Server, Loose());
    CHECK(r.load.ok);
    CHECK_EQ(r.f->NodeCount(), std::size_t{311});
    r.Run(1);
    const auto solves = r.f->Stats().powerSolves;
    r.Run(3000);
    CHECK_EQ(r.f->Stats().powerSolves, solves);  // nothing changed, nothing solved
    CHECK(r.f->Stats().timersFired <= 3000 / 300 + 2);
}
