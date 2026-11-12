// LAST LIGHT - World Simulation & Scavenging: portable-core CPU benchmark.
//
// Measures the C++ core only (no Unreal, no rendering, no navigation, no AIS).
// Results are CPU time of this process on the machine that runs it; they are
// not frame-rate predictions for the game.
//
//   make bench
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "WsTestHelpers.h"

using namespace wstest;
using Clock = std::chrono::steady_clock;

namespace {

// N entities in clusters of 10 (one "site" each), spread over a ~10 km map.
std::string SyntheticWorld(int n) {
    const int sites = std::max(1, n / 10);
    std::string t = "[content]\nversion = 1\n[simulation]\nqueue_capacity = " + std::to_string(4 * n + 4 * sites + 64) +
                    "\nmax_updates_per_step = 512\nevent_log_capacity = 256\n";
    const int side = 1;
    (void)side;
    int grid = 1;
    while (grid * grid < sites) ++grid;
    for (int s = 0; s < sites; ++s) {
        const std::string id = std::to_string(s);
        const double x = (s % grid) * 1000000.0 / grid;  // up to 10 km
        const double y = (s / grid) * 1000000.0 / grid;
        const std::string pos = std::to_string(x) + ", " + std::to_string(y) + ", 0";
        const std::string next = std::to_string((s + 1) % sites);
        t += "[area.A" + id + "]\ncenter = " + pos + "\nradius_cm = 1500\n";
        t += "[entity.GEN" + id + "]\nkind = Generator\narea = A" + id + "\nfuel_seconds = " + std::to_string(120 + (s * 37) % 900) +
             "\nlow_fuel_seconds = 30\n";
        t += "[entity.BAK" + id + "]\nkind = Generator\narea = A" + id + "\nbackup_for = GEN" + id + "\nfuel_seconds = 600\n";
        t += "[entity.NET" + id + "]\nkind = PowerNetwork\narea = A" + id + "\ndepends_on = GEN" + id + ", BAK" + id + "\n";
        t += "[entity.SEC" + id + "]\nkind = SecuritySystem\narea = A" + id + "\ndepends_on = NET" + id + "\n";
        t += "[entity.COM" + id + "]\nkind = CommsStation\narea = A" + id + "\ndepends_on = NET" + id + "\n";
        t += "[entity.HAZ" + id + "]\nkind = HazardZone\narea = A" + id + "\nactive_ticks = " + std::to_string(600 + (s * 13) % 600) +
             "\ndormant_ticks = 2400\n";
        t += "[entity.FAC" + id + "]\nkind = Facility\narea = A" + id + "\ndepends_on = NET" + id + ", SEC" + id + ", HAZ" + id + "\n";
        t += "[entity.STO" + id + "]\nkind = Storage\narea = A" + id + "\ndepends_on = NET" + id + ", FAC" + id + "\n";
        t += "[entity.MAC" + id + "]\nkind = Machinery\narea = A" + id + "\ndepends_on = NET" + id + "\n";
        t += "[entity.PAT" + id + "]\nkind = PatrolGroup\narea = A" + id + "\nroute = A" + id + ", A" + next +
             "\ndwell_ticks = 600\ntravel_ticks = 900\ncomms = COM" + id + "\n";
        t += "[rule.LOCK" + id + "]\non = state:SEC" + id + "=Alarm\ndo = set:FAC" + id + "=Lockdown, alert:FAC" + id + "\nonce = true\n";
        if (s % 10 == 0) t += "[schedule.ALARM" + id + "]\nat_ticks = " + std::to_string(3000 + s * 7) + "\ndo = signal:AlarmRaised@SEC" + id + "\n";
    }
    return t;
}

struct Result {
    int n = 0;
    double totalMs = 0.0;
    double meanUs = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
    llsim::SimStats stats;
    std::size_t entities = 0;
};

Result RunSim(int n, llworld::Tick ticks) {
    Content c;
    std::vector<std::string> errors;
    llcontent::LoadContentText(SyntheticWorld(n), "synthetic", c.loot, c.sim, errors);
    for (const auto& e : llsim::ValidateSim(c.sim)) errors.push_back(e);
    for (const auto& e : errors) std::printf("  config: %s\n", e.c_str());
    llsim::WorldSimulation w(c.sim);
    w.BeginRaid(1);
    std::vector<double> stepUs;
    stepUs.reserve(ticks);
    const auto start = Clock::now();
    for (llworld::Tick t = 0; t < ticks; ++t) {
        // Four players walking across the map; the game reports them every 15 ticks.
        if (t % 15 == 0) {
            const double d = static_cast<double>(t) * 30.0;  // 3 m/s
            w.SetPlayers({{d, 0, 0}, {d, 400, 0}, {0, d, 0}, {d, d, 0}});
        }
        const auto s0 = Clock::now();
        w.Step();
        stepUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
    }
    Result r;
    r.n = n;
    r.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::sort(stepUs.begin(), stepUs.end());
    double sum = 0.0;
    for (double v : stepUs) sum += v;
    r.meanUs = sum / static_cast<double>(stepUs.size());
    r.p99Us = stepUs[stepUs.size() * 99 / 100];
    r.maxUs = stepUs.back();
    r.stats = w.Stats();
    r.entities = w.EntityCount();
    return r;
}

}  // namespace

int main() {
    std::printf("LAST LIGHT world simulation - portable core CPU benchmark (single thread)\n");
    std::printf("10 simulated minutes @30 Hz (18000 steps), 4 moving players, tiers High 3 / Medium 30 / Low 600 ticks\n\n");
    std::printf("%8s %10s %10s %10s %10s %12s %12s %10s %12s\n", "entities", "total ms", "mean us", "p99 us", "max us", "updates", "per-tick eq.",
                "timers", "transitions");
    for (int n : {100, 500, 1000, 5000}) {
        const Result r = RunSim(n, 18000);
        const unsigned long long naive = 18000ull * r.entities;
        std::printf("%8zu %10.1f %10.2f %10.2f %10.1f %12llu %12llu %10llu %12llu\n", r.entities, r.totalMs, r.meanUs, r.p99Us, r.maxUs,
                    static_cast<unsigned long long>(r.stats.Updates()), naive, static_cast<unsigned long long>(r.stats.timersFired),
                    static_cast<unsigned long long>(r.stats.transitions));
        std::printf("%8s updates by tier H/M/L: %llu / %llu / %llu, deferred %llu, overflows %llu\n", "", static_cast<unsigned long long>(r.stats.updatesByTier[0]),
                    static_cast<unsigned long long>(r.stats.updatesByTier[1]), static_cast<unsigned long long>(r.stats.updatesByTier[2]),
                    static_cast<unsigned long long>(r.stats.deferredUpdates), static_cast<unsigned long long>(r.stats.queueOverflows));
    }

    // Loot generation: every container of the example map, many raids.
    const Content all = Load({kAllLocations[0], kAllLocations[1], kAllLocations[2], kAllLocations[3], kAllLocations[4]});
    const int raids = 20000;
    std::uint64_t items = 0;
    std::uint64_t sink = 0;
    const auto t0 = Clock::now();
    for (int raid = 0; raid < raids; ++raid) {
        for (const auto& d : all.loot.containers) {
            const llloot::LootResult r = llloot::GenerateLoot(all.loot, Catalog(), d, llloot::ContainerSeed(static_cast<std::uint64_t>(raid), d.id, 0));
            items += r.items.size();
            sink ^= r.seed;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const double gens = static_cast<double>(raids) * static_cast<double>(all.loot.containers.size());
    std::printf("\nloot generation: %.0f containers in %.1f ms = %.2f us/container (%.2f items avg) [%llx]\n", gens, ms, ms * 1000.0 / gens,
                static_cast<double>(items) / gens, static_cast<unsigned long long>(sink & 0xF));

    // ---------------------------------------------------------------- stress: container count
    for (int nContainers : {1000, 10000, 50000}) {
        const int locations = std::max(1, nContainers / 100);
        std::string t = "[content]\nversion = 1\n[table.T]\nentry = mech.bearing_set | weight=3 | qty=1-3\nentry = elec.copper_wire | weight=2\n"
                        "[profile.P]\npool = T | guaranteed | rolls=1-2\n";
        for (int l = 0; l < locations; ++l) t += "[location.L" + std::to_string(l) + "]\ntype = Industrial\nprofile.Toolbox = P\n";
        for (int i = 0; i < nContainers; ++i) {
            t += "[container.C" + std::to_string(i) + "]\nlocation = L" + std::to_string(i % locations) + "\ntype = TOOLBOX\nposition = " +
                 std::to_string((i % 100) * 500) + ", " + std::to_string((i / 100) * 500) + ", 0\n";
            if (i % 10 == 0) t += "access = power:N" + std::to_string(i / 10) + "\n";
        }
        Content sc;
        std::vector<std::string> errors;
        llcontent::LoadContentFile(Data("common.ini"), sc.loot, sc.sim, errors);
        const auto l0 = Clock::now();
        llcontent::LoadContentText(t, "stress", sc.loot, sc.sim, errors);
        const double loadMs = std::chrono::duration<double, std::milli>(Clock::now() - l0).count();
        const auto v0 = Clock::now();
        const auto problems = llloot::ValidateLoot(sc.loot, Catalog());
        const double validateMs = std::chrono::duration<double, std::milli>(Clock::now() - v0).count();
        Access access;
        MemInventory inv;
        const auto c0 = Clock::now();
        llloot::ContainerService svc(sc.loot, Catalog());
        svc.SetAccessPort(&access);
        svc.SetGrantPort(&inv);
        svc.BeginRaid(7);
        const double beginMs = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
        for (llloot::PlayerId p = 1; p <= 4; ++p) svc.ConnectPlayer(p, p, {0, 0, 0});
        // 4 players search 2,000 containers back to back.
        const auto s0 = Clock::now();
        llloot::Tick now = 0;
        std::uint32_t seq = 0;
        for (int k = 0; k < 2000; ++k) {
            const auto c = static_cast<std::uint32_t>((k * 7919) % nContainers);
            const llloot::PlayerId p = static_cast<llloot::PlayerId>(1 + k % 4);
            svc.UpdatePlayerPosition(p, svc.Def(c).position);
            svc.Submit({p, ++seq, c, llloot::LootAction::Search, 0});
            if (k % 4 == 3) {
                for (int s = 0; s < 46; ++s) svc.Step(++now);
            }
        }
        const double searchMs = std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
        const auto n0 = Clock::now();
        for (int k = 0; k < 1000; ++k) svc.NotifyInfrastructureChanged("N" + std::to_string(k % (nContainers / 10)));
        const double notifyUs = std::chrono::duration<double, std::micro>(Clock::now() - n0).count() / 1000.0;
        std::printf("containers %6d: load %.1f ms, validate %.1f ms (%zu issues), service+BeginRaid %.1f ms, 2000 searches %.1f ms "
                    "(%.1f us each, %llu completed), power notify %.1f us\n",
                    nContainers, loadMs, validateMs, problems.size(), beginMs, searchMs, searchMs * 1000.0 / 2000.0,
                    static_cast<unsigned long long>(svc.GetStats().searchesCompleted), notifyUs);
    }

    // ---------------------------------------------------------------- stress: simultaneous world events
    {
        Content c;
        std::vector<std::string> errors;
        llcontent::LoadContentText(SyntheticWorld(5000), "synthetic", c.loot, c.sim, errors);
        llsim::WorldSimulation w(c.sim);
        w.BeginRaid(1);
        w.AdvanceTo(100);
        const auto a0 = Clock::now();
        for (int s = 0; s < 500; ++s) w.NotifySignal(llsim::SignalKind::AlarmRaised, "SEC" + std::to_string(s));
        const double alarmMs = std::chrono::duration<double, std::milli>(Clock::now() - a0).count();
        const auto st0 = Clock::now();
        w.Step();
        const double stepUs = std::chrono::duration<double, std::micro>(Clock::now() - st0).count();
        std::printf("\n500 simultaneous facility alarms (5000 entities): signals %.2f ms total, next step %.1f us, transitions %llu, rules %llu, "
                    "deferred transitions %llu\n",
                    alarmMs, stepUs, static_cast<unsigned long long>(w.Stats().transitions), static_cast<unsigned long long>(w.Stats().rulesFired),
                    static_cast<unsigned long long>(w.Stats().deferredTransitions));
        const auto f0 = Clock::now();
        for (int k = 0; k < 100000; ++k) w.NotifySignal(k % 2 ? llsim::SignalKind::AlarmCleared : llsim::SignalKind::AlarmRaised, "SEC0");
        const double floodUs = std::chrono::duration<double, std::micro>(Clock::now() - f0).count() / 100000.0;
        const auto d0 = Clock::now();
        for (int k = 0; k < 10000; ++k) w.ReportDisturbance({static_cast<double>(k % 1000) * 1000.0, 0, 0}, 70.0);
        const double distUs = std::chrono::duration<double, std::micro>(Clock::now() - d0).count() / 10000.0;
        std::printf("signal flood: %.2f us per signal (100k); disturbance: %.2f us each (10k, scans %zu areas + patrol list)\n", floodUs, distUs,
                    c.sim.areas.size());
    }
    return 0;
}
