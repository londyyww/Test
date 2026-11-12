// Weather: transitions, Markov determinism, per-condition effects, gusts, storms.
#include <cmath>

#include "TestFramework.h"
#include "llworld/Weather.h"

using namespace llworld;

namespace {
WeatherConfig Fixed(WeatherKind initial = WeatherKind::Clear) {
    WeatherConfig c = WeatherConfig::MakeDefault();
    c.initial = initial;
    c.autoAdvance = false;
    c.anchors = {{{100, 0, 0}, true}, {{-100, 0, 0}, false}};
    return c;
}
void Run(WeatherSystem& w, Tick& t, Tick n) {
    for (Tick i = 0; i < n; ++i) w.Step(++t);
}
std::size_t Count(const WorldEventBus& bus, WorldEventType type) {
    std::size_t n = 0;
    for (const auto& e : bus.Current()) n += e.type == type ? 1 : 0;
    return n;
}
}  // namespace

AIS_TEST(Weather_RequestedTransitionBlendsAndCompletes) {
    WorldEventBus bus;
    WeatherSystem w(Fixed(), &bus);
    Tick t = 0;
    bus.BeginTick(++t);
    w.Step(t);
    CHECK(w.Snapshot().from == WeatherKind::Clear);
    CHECK(w.RequestWeather(WeatherKind::HeavyRain, 300, t));
    CHECK(!w.RequestWeather(WeatherKind::HeavyRain, 300, t));  // duplicate request
    CHECK(w.InTransition());
    Run(w, t, 150);
    const double mid = w.Snapshot().rain;
    CHECK(mid > 0.3 && mid < 0.7);  // halfway between 0 and 0.9
    Run(w, t, 200);
    CHECK(!w.InTransition());
    CHECK(w.Snapshot().from == WeatherKind::HeavyRain);
    CHECK_NEAR(w.Snapshot().rain, 0.9, 1e-9);
    CHECK(!w.RequestWeather(WeatherKind::HeavyRain, 10, t));  // already there
    CHECK(!w.RequestWeather(static_cast<WeatherKind>(42), 10, t));
}

AIS_TEST(Weather_ConditionsHaveDistinctEffects) {
    const WeatherConfig c = WeatherConfig::MakeDefault();
    const auto& rain = c.profiles[static_cast<std::size_t>(WeatherKind::HeavyRain)];
    const auto& fog = c.profiles[static_cast<std::size_t>(WeatherKind::Fog)];
    const auto& wind = c.profiles[static_cast<std::size_t>(WeatherKind::StrongWind)];
    const auto& storm = c.profiles[static_cast<std::size_t>(WeatherKind::Storm)];
    // Fog: worst for eyes, irrelevant for ears, mild for thermal.
    CHECK(fog.visualRangeMul < rain.visualRangeMul);
    CHECK(fog.rain == 0.0 && fog.wind < 0.1);
    CHECK(fog.thermalRangeMul > rain.thermalRangeMul);
    // Heavy rain: noisy (masks), hurts thermal more than fog does.
    CHECK(rain.rain > 0.8);
    // Wind: loud low band and gusts, barely affects vision, spreads fire.
    CHECK(wind.wind > 0.8 && wind.visualRangeMul > 0.9 && wind.gustIntervalTicks > 0 && wind.fireSpreadMul > 1.0);
    // Storm: comms interference + infrastructure exposure + thunder.
    CHECK(storm.commsRangeMul < 1.0 && storm.electricalExposure > 0.0 && storm.thunderIntervalTicks > 0);
    // Visual and thermal are separate channels.
    CHECK(fog.visualRangeMul != fog.thermalRangeMul);
}

AIS_TEST(Weather_NeverMakesAnyoneInvisible) {
    WeatherConfig c = Fixed(WeatherKind::Fog);
    c.profiles[static_cast<std::size_t>(WeatherKind::Fog)].visualRangeMul = 0.01;
    WeatherSystem w(c, nullptr);
    w.Step(1);
    CHECK_NEAR(w.VisualRangeFactor(), c.minVisualRangeFactor, 1e-12);
    CHECK(w.ThermalRangeFactor() >= c.minThermalRangeFactor);
}

AIS_TEST(Weather_AcousticReportConfidenceFallsWithDistanceInRainAndWind) {
    WeatherSystem clear(Fixed(WeatherKind::Clear), nullptr);
    WeatherSystem storm(Fixed(WeatherKind::Storm), nullptr);
    clear.Step(1);
    storm.Step(1);
    CHECK_NEAR(storm.AcousticReportConfidenceScale(0.0), 1.0, 1e-12);       // close sounds are fine
    CHECK(storm.AcousticReportConfidenceScale(6000.0) < 0.6);               // distant ones are doubtful
    CHECK(clear.AcousticReportConfidenceScale(6000.0) > 0.9);
    CHECK(storm.AcousticReportConfidenceScale(1e9) >= 0.2);
}

AIS_TEST(Weather_WetnessBuildsPuddlesAndDries) {
    WeatherSystem w(Fixed(WeatherKind::HeavyRain), nullptr);
    Tick t = 0;
    Run(w, t, 30);
    CHECK(!w.Surface().puddles);
    Run(w, t, 600);
    CHECK(w.Surface().puddles);
    CHECK(w.RequestWeather(WeatherKind::Clear, 30, t));
    Run(w, t, 30 * 120);
    CHECK(!w.Surface().puddles);
    CHECK(w.Surface().wetness < 0.4);
}

AIS_TEST(Weather_GustsOnlyInWindAndMetallicAnchorsFlagged) {
    WorldEventBus bus;
    WeatherSystem calm(Fixed(WeatherKind::Clear), &bus);
    std::size_t gusts = 0;
    for (Tick t = 1; t <= 3000; ++t) {
        bus.BeginTick(t);
        calm.Step(t);
        gusts += Count(bus, WorldEventType::WindGust);
    }
    CHECK_EQ(gusts, std::size_t{0});

    WorldEventBus bus2;
    WeatherSystem windy(Fixed(WeatherKind::StrongWind), &bus2);
    std::size_t metal = 0, plain = 0;
    for (Tick t = 1; t <= 3000; ++t) {
        bus2.BeginTick(t);
        windy.Step(t);
        for (const auto& e : bus2.Current()) {
            if (e.type != WorldEventType::WindGust) continue;
            (e.aux != 0 ? metal : plain) += 1;
            CHECK_NEAR(e.magnitude, 72.0, 1e-9);
        }
    }
    CHECK(metal + plain >= 6 && metal + plain <= 16);  // ~240 +- 120 ticks
    CHECK(metal > 0 && plain > 0);
}

AIS_TEST(Weather_StormHasThunderAndExposure) {
    WorldEventBus bus;
    WeatherSystem w(Fixed(WeatherKind::Storm), &bus);
    std::size_t thunder = 0;
    for (Tick t = 1; t <= 3000; ++t) {
        bus.BeginTick(t);
        w.Step(t);
        thunder += Count(bus, WorldEventType::Thunder);
    }
    CHECK(thunder >= 2);
    CHECK(w.Snapshot().electricalExposure > 0.0);
    CHECK(w.Snapshot().commsRangeMul < 1.0);
}

AIS_TEST(Weather_MarkovAdvanceIsDeterministicAndSeedDependent) {
    auto run = [](std::uint64_t seed) {
        WeatherConfig c = WeatherConfig::MakeDefault();
        c.seed = seed;
        c.anchors = {{{0, 0, 0}, true}};
        WorldEventBus bus;
        WeatherSystem w(c, &bus);
        std::vector<std::uint64_t> hashes;
        std::vector<int> kinds;
        for (Tick t = 1; t <= 30 * 60 * 20; ++t) {
            bus.BeginTick(t);
            w.Step(t);
            if (t % 300 == 0) {
                hashes.push_back(w.StateHash());
                kinds.push_back(static_cast<int>(w.Dominant()));
            }
        }
        return std::make_pair(hashes, kinds);
    };
    const auto a = run(7);
    const auto b = run(7);
    const auto c = run(8);
    CHECK(a.first == b.first);
    CHECK(a.first != c.first);
    bool changed = false;
    for (int k : a.second) changed = changed || k != a.second.front();
    CHECK(changed);  // 20 minutes of auto weather actually moves
}

AIS_TEST(Weather_UpdatesAtCoarseIntervalOnly) {
    WeatherSystem w(Fixed(WeatherKind::Clear), nullptr);
    Tick t = 0;
    Run(w, t, 3000);
    CHECK(w.Updates() <= 3000 / 30 + 1);
}

AIS_TEST(Weather_ConfigValidationAndLoading) {
    WeatherConfig ok = WeatherConfig::MakeDefault();
    CHECK(ValidateWeather(ok).empty());
    WeatherConfig bad = ok;
    bad.profiles[0].rain = 2.0;
    CHECK(!ValidateWeather(bad).empty());
    bad = ok;
    bad.profiles[1].nextWeights = {};
    CHECK(!ValidateWeather(bad).empty());
    bad = ok;
    bad.profiles[2].holdMinTicks = 10;
    bad.profiles[2].holdMaxTicks = 5;
    CHECK(!ValidateWeather(bad).empty());

    std::vector<std::string> errors;
    const IniDocument doc = ParseIni("[weather]\ninitial = Fog\n[weather.Fog]\nvisual_range_mul = 0.2\nnext.Clear = 1\n[gust.a]\nposition = 1,2,3\nmetallic = true\n", errors);
    WeatherConfig c = WeatherConfig::MakeDefault();
    LoadWeather(doc, c, errors);
    CHECK(errors.empty());
    CHECK(c.initial == WeatherKind::Fog);
    CHECK_NEAR(c.profiles[3].visualRangeMul, 0.2, 1e-12);
    CHECK(c.anchors.size() == 1 && c.anchors[0].metallic);
    std::vector<std::string> e2;
    LoadWeather(ParseIni("[weather.Hail]\nrain = 1\n[weather]\nnope = 1\n", e2), c, e2);
    CHECK(e2.size() == 2);
}
