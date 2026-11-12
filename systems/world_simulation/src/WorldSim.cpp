// LAST LIGHT - Mission World Simulation implementation.
#include "llsim/WorldSim.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

namespace llsim {

using llworld::IniBinder;
using llworld::IniDocument;
using llworld::IniSection;

namespace {

constexpr const char* kKindNames[kEntityKindCount] = {"PatrolGroup",    "Facility",  "Generator",   "PowerNetwork", "SecuritySystem",
                                                      "Machinery",      "Storage",   "CommsStation", "HazardZone"};
constexpr const char* kStateNames[kEntityStateCount] = {"Nominal", "Active", "Standby", "Degraded", "Offline",
                                                        "Alarm",   "Lockdown", "Interrupted", "Responding"};
constexpr const char* kTierNames[kTierCount] = {"High", "Medium", "Low"};
constexpr const char* kSignalNames[kSignalKindCount] = {"PowerLost",       "PowerRestored", "AlarmRaised",   "AlarmCleared",
                                                        "CommsLost",       "CommsRestored", "HazardActivated", "HazardCleared",
                                                        "WeatherSevere",   "WeatherCleared", "Disturbance"};
constexpr const char* kEventNames[13] = {"StateChanged", "Directive", "InstructionQueued", "InstructionDelivered", "InstructionDropped",
                                         "RuleFired",    "ScheduleFired", "TierChanged",   "FuelLow",              "FuelEmpty",
                                         "BackupStarted", "OverrideApplied", "OverrideExpired"};

template <std::size_t N>
bool ParseName(std::string_view s, const char* const (&names)[N], std::size_t& out) {
    for (std::size_t i = 0; i < N; ++i) {
        if (s == names[i]) return (out = i, true);
    }
    return false;
}

using S = EntityState;
using K = EntityKind;

// Timer kinds (per entity, generation-checked).
enum TimerKind : std::uint16_t { kFuelLow = 0, kFuelEmpty, kBackupStart, kResponseEnd, kHazardCycle, kOverrideExpire, kEntityTimerCount };
// Global timers (target = rule / schedule index).
constexpr std::uint16_t kRuleFire = 100;
constexpr std::uint16_t kScheduleFire = 101;
constexpr std::uint16_t kUpdate = 200;

bool Up(S s) { return s == S::Active || s == S::Degraded; }

}  // namespace

const char* ToString(EntityKind k) { return static_cast<std::size_t>(k) < kEntityKindCount ? kKindNames[static_cast<std::size_t>(k)] : "?"; }
const char* ToString(EntityState s) { return static_cast<std::size_t>(s) < kEntityStateCount ? kStateNames[static_cast<std::size_t>(s)] : "?"; }
const char* ToString(Tier t) { return static_cast<std::size_t>(t) < kTierCount ? kTierNames[static_cast<std::size_t>(t)] : "?"; }
const char* ToString(SignalKind s) { return static_cast<std::size_t>(s) < kSignalKindCount ? kSignalNames[static_cast<std::size_t>(s)] : "?"; }
const char* ToString(SimEventType t) { return static_cast<std::size_t>(t) < 13 ? kEventNames[static_cast<std::size_t>(t)] : "?"; }
bool Parse(std::string_view s, EntityKind& out) {
    std::size_t i = 0;
    return ParseName(s, kKindNames, i) ? (out = static_cast<EntityKind>(i), true) : false;
}
bool Parse(std::string_view s, EntityState& out) {
    std::size_t i = 0;
    return ParseName(s, kStateNames, i) ? (out = static_cast<EntityState>(i), true) : false;
}
bool Parse(std::string_view s, SignalKind& out) {
    std::size_t i = 0;
    return ParseName(s, kSignalNames, i) ? (out = static_cast<SignalKind>(i), true) : false;
}

bool StateAllowed(EntityKind k, EntityState s) {
    switch (k) {
        case K::PatrolGroup: return s == S::Active || s == S::Responding || s == S::Interrupted || s == S::Offline;
        case K::Facility: return s == S::Nominal || s == S::Degraded || s == S::Offline || s == S::Alarm || s == S::Lockdown;
        case K::Generator: return s == S::Active || s == S::Standby || s == S::Degraded || s == S::Offline;
        case K::PowerNetwork: return s == S::Nominal || s == S::Degraded || s == S::Offline;
        case K::SecuritySystem: return s == S::Nominal || s == S::Alarm || s == S::Offline;
        case K::Machinery: return s == S::Active || s == S::Degraded || s == S::Offline;
        case K::Storage: return s == S::Nominal || s == S::Lockdown || s == S::Offline;
        case K::CommsStation: return s == S::Nominal || s == S::Interrupted || s == S::Offline;
        case K::HazardZone: return s == S::Nominal || s == S::Active;
    }
    return false;
}

// ---------------------------------------------------------------- loading

bool IsSimSection(std::string_view n) {
    return n == "simulation" || n.rfind("tier.", 0) == 0 || n.rfind("area.", 0) == 0 || n.rfind("entity.", 0) == 0 ||
           n.rfind("schedule.", 0) == 0 || n.rfind("rule.", 0) == 0;
}

void LoadSimSections(const IniDocument& doc, SimConfig& cfg, std::vector<std::string>& errors) {
    auto suffix = [](const std::string& n) { return n.substr(n.find('.') + 1); };
    auto size = [](std::size_t& f) {
        return [&f](std::string_view v) {
            double d = 0.0;
            if (!llworld::ParseNumber(v, d) || d < 0.0 || d > 1e9 || std::floor(d) != d) return false;
            f = static_cast<std::size_t>(d);
            return true;
        };
    };
    for (const IniSection& s : doc.sections) {
        const std::string& n = s.name;
        if (n == "simulation") {
            IniBinder b;
            b.Num("ticks_per_second", cfg.ticksPerSecond);
            b.Custom("max_updates_per_step", size(cfg.maxUpdatesPerStep));
            b.Custom("max_timers_per_step", size(cfg.maxTimersPerStep));
            b.Custom("queue_capacity", size(cfg.queueCapacity));
            b.Ticks("relevance_interval_ticks", cfg.relevanceIntervalTicks);
            b.Ticks("observed_window_ticks", cfg.observedWindowTicks);
            b.Custom("max_transitions_per_step", size(cfg.maxTransitionsPerStep));
            b.Custom("event_log_capacity", size(cfg.eventLogCapacity));
            b.Custom("max_pending_instructions", size(cfg.maxPendingInstructions));
            b.Apply(s, errors);
        } else if (n.rfind("tier.", 0) == 0) {
            std::size_t t = 0;
            if (!ParseName(suffix(n), kTierNames, t)) {
                errors.push_back("line " + std::to_string(s.line) + ": unknown tier [" + n + "] (High, Medium, Low)");
                continue;
            }
            IniBinder b;
            b.Ticks("interval_ticks", cfg.tiers[t].intervalTicks);
            b.Num("radius_cm", cfg.tiers[t].radiusCm);
            b.Apply(s, errors);
        } else if (n.rfind("area.", 0) == 0) {
            AreaDef a;
            a.id = suffix(n);
            IniBinder b;
            b.Point("center", a.center);
            b.Num("radius_cm", a.radiusCm);
            b.Flag("objective", a.objective);
            b.Apply(s, errors);
            cfg.areas.push_back(a);
        } else if (n.rfind("entity.", 0) == 0) {
            EntityDef e;
            e.id = suffix(n);
            bool kindSet = false;
            IniBinder b;
            b.Custom("kind", [&e, &kindSet](std::string_view v) { return (kindSet = true, Parse(v, e.kind)); });
            b.Str("area", e.area);
            b.Custom("position", [&e](std::string_view v) {
                IniBinder inner;
                e.hasPosition = true;
                std::vector<std::string> errs;
                IniSection tmp;
                tmp.entries.push_back({"p", std::string(v), 0});
                inner.Point("p", e.position);
                inner.Apply(tmp, errs);
                return errs.empty();
            });
            b.List("depends_on", e.dependsOn);
            b.Str("bind", e.bind);
            b.Str("state", e.initialState);
            b.Flag("persist", e.persist);
            b.Num("fuel_seconds", e.fuelSeconds);
            b.Num("burn_rate", e.burnRate);
            b.Num("low_fuel_seconds", e.lowFuelSeconds);
            b.Str("backup_for", e.backupFor);
            b.Ticks("start_delay_ticks", e.startDelayTicks);
            b.Flag("running", e.running);
            b.List("route", e.route);
            b.Ticks("dwell_ticks", e.dwellTicks);
            b.Ticks("travel_ticks", e.travelTicks);
            b.Num("respond_radius_cm", e.respondRadiusCm);
            b.Ticks("respond_hold_ticks", e.respondHoldTicks);
            b.Str("comms", e.comms);
            b.Ticks("active_ticks", e.activeTicks);
            b.Ticks("dormant_ticks", e.dormantTicks);
            b.Apply(s, errors);
            if (!kindSet) errors.push_back("line " + std::to_string(s.line) + ": [" + n + "] needs kind");
            cfg.entities.push_back(e);
        } else if (n.rfind("schedule.", 0) == 0) {
            ScheduleDef d;
            d.id = suffix(n);
            IniBinder b;
            b.Ticks("at_ticks", d.atTicks);
            b.Ticks("repeat_ticks", d.repeatTicks);
            b.List("do", d.actions);
            b.Apply(s, errors);
            cfg.schedules.push_back(d);
        } else if (n.rfind("rule.", 0) == 0) {
            RuleDef r;
            r.id = suffix(n);
            IniBinder b;
            b.Str("on", r.on);
            b.List("require", r.requires_);
            b.List("do", r.actions);
            b.Ticks("delay_ticks", r.delayTicks);
            b.Flag("once", r.once);
            b.Apply(s, errors);
            cfg.rules.push_back(r);
        }
    }
}

// ---------------------------------------------------------------- compilation

namespace {

enum class ActionType : std::uint8_t { Set, Alert, Retask, Start, Stop, Signal };
struct Action {
    ActionType type = ActionType::Set;
    std::uint32_t entity = kNone;
    EntityState state = S::Nominal;
    std::vector<std::uint32_t> route;
    SignalKind signal = SignalKind::Disturbance;
};
enum class TriggerType : std::uint8_t { State, Signal };
struct Trigger {
    TriggerType type = TriggerType::State;
    std::uint32_t entity = kNone;  // kNone on a signal trigger = any source
    EntityState state = S::Nominal;
    SignalKind signal = SignalKind::Disturbance;
};
struct Condition {
    std::uint32_t entity = kNone;
    EntityState state = S::Nominal;
    bool negate = false;
};
struct CRule {
    Trigger on;
    std::vector<Condition> require;
    std::vector<Action> actions;
    Tick delay = 0;
    bool once = false;
};
struct CSchedule {
    Tick at = 0;
    Tick repeat = 0;
    std::vector<Action> actions;
};
struct CEntity {
    std::uint32_t area = kNone;
    std::vector<std::uint32_t> deps;
    std::uint32_t backupFor = kNone;
    std::uint32_t comms = kNone;
    std::vector<std::uint32_t> route;
    bool bound = false;
    bool bindHazard = false;
    std::string bindName;
    EntityState initial = S::Nominal;
};
struct Compiled {
    std::vector<CEntity> entities;
    std::vector<CRule> rules;
    std::vector<CSchedule> schedules;
    std::vector<std::uint32_t> topo;  // dependency order
};

template <class T>
std::uint32_t FindById(const std::vector<T>& v, std::string_view id) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].id == id) return static_cast<std::uint32_t>(i);
    }
    return kNone;
}

template <class T>
void CheckDuplicates(const std::vector<T>& v, const char* kind, std::vector<std::string>& out) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].id.empty()) out.push_back(std::string(kind) + " with an empty id");
        for (std::size_t j = 0; j < i; ++j) {
            if (v[i].id == v[j].id) {
                out.push_back("duplicate " + std::string(kind) + " id '" + v[i].id + "'");
                break;
            }
        }
    }
}

EntityState DefaultInitial(const EntityDef& d) {
    switch (d.kind) {
        case K::PatrolGroup: return S::Active;
        case K::Generator: return !d.backupFor.empty() ? S::Standby : (d.running ? S::Active : S::Offline);
        case K::Machinery: return S::Active;
        default: return S::Nominal;
    }
}

bool ParseEntityState(const SimConfig& cfg, std::string_view text, char sep, std::uint32_t& entity, EntityState& state, bool& negate,
                      std::string& why) {
    negate = false;
    std::size_t p = text.find("!=");
    std::size_t len = 2;
    if (p != std::string_view::npos) {
        negate = true;
    } else {
        p = text.find(sep);
        len = 1;
    }
    if (p == std::string_view::npos) return (why = "expected ENTITY=STATE", false);
    const std::string_view e = llworld::TrimView(text.substr(0, p));
    const std::string_view s = llworld::TrimView(text.substr(p + len));
    entity = FindById(cfg.entities, e);
    if (entity == kNone) return (why = "unknown entity '" + std::string(e) + "'", false);
    if (!Parse(s, state)) return (why = "unknown state '" + std::string(s) + "'", false);
    if (!StateAllowed(cfg.entities[entity].kind, state))
        return (why = std::string("state ") + ToString(state) + " is not valid for " + ToString(cfg.entities[entity].kind) + " '" +
                      std::string(e) + "'",
                false);
    return true;
}

bool ParseAction(const SimConfig& cfg, const Compiled& c, std::string_view text, Action& a, std::string& why) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) return (why = "expected verb:args", false);
    const std::string_view verb = llworld::TrimView(text.substr(0, colon));
    const std::string_view arg = llworld::TrimView(text.substr(colon + 1));
    bool neg = false;
    if (verb == "set") {
        a.type = ActionType::Set;
        if (!ParseEntityState(cfg, arg, '=', a.entity, a.state, neg, why)) return false;
        if (neg) return (why = "set cannot use !=", false);
    } else if (verb == "alert") {
        a.type = ActionType::Alert;
        a.entity = FindById(cfg.entities, arg);
        if (a.entity == kNone) return (why = "unknown entity '" + std::string(arg) + "'", false);
    } else if (verb == "start" || verb == "stop") {
        a.type = verb == "start" ? ActionType::Start : ActionType::Stop;
        a.entity = FindById(cfg.entities, arg);
        if (a.entity == kNone) return (why = "unknown entity '" + std::string(arg) + "'", false);
        if (cfg.entities[a.entity].kind != K::Generator) return (why = std::string(verb) + " needs a Generator", false);
    } else if (verb == "retask") {
        a.type = ActionType::Retask;
        const std::size_t eq = arg.find('=');
        if (eq == std::string_view::npos) return (why = "expected retask:GROUP=AREA+AREA", false);
        a.entity = FindById(cfg.entities, llworld::TrimView(arg.substr(0, eq)));
        if (a.entity == kNone || cfg.entities[a.entity].kind != K::PatrolGroup) return (why = "retask needs a PatrolGroup", false);
        std::string_view rest = arg.substr(eq + 1);
        while (true) {
            const std::size_t plus = rest.find('+');
            const std::string_view area = llworld::TrimView(plus == std::string_view::npos ? rest : rest.substr(0, plus));
            const std::uint32_t ai = FindById(cfg.areas, area);
            if (ai == kNone) return (why = "unknown area '" + std::string(area) + "'", false);
            a.route.push_back(ai);
            if (plus == std::string_view::npos) break;
            rest.remove_prefix(plus + 1);
        }
    } else if (verb == "signal") {
        a.type = ActionType::Signal;
        const std::size_t at = arg.find('@');
        if (!Parse(llworld::TrimView(arg.substr(0, at)), a.signal)) return (why = "unknown signal '" + std::string(arg.substr(0, at)) + "'", false);
        if (at != std::string_view::npos) {
            a.entity = FindById(cfg.entities, llworld::TrimView(arg.substr(at + 1)));
            if (a.entity == kNone) return (why = "unknown entity in '" + std::string(arg) + "'", false);
        }
    } else {
        return (why = "unknown action '" + std::string(verb) + "'", false);
    }
    if ((a.type == ActionType::Set || a.type == ActionType::Start || a.type == ActionType::Stop) && a.entity < c.entities.size() &&
        c.entities[a.entity].bound) {
        return (why = "entity '" + cfg.entities[a.entity].id +
                      "' is bound to infrastructure; its state is owned by that system (use signal: or the facility)",
                false);
    }
    return true;
}

bool ParseTrigger(const SimConfig& cfg, std::string_view text, Trigger& t, std::string& why) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) return (why = "expected state:E=S or signal:KIND[@E]", false);
    const std::string_view verb = llworld::TrimView(text.substr(0, colon));
    const std::string_view arg = llworld::TrimView(text.substr(colon + 1));
    if (verb == "state") {
        t.type = TriggerType::State;
        bool neg = false;
        if (!ParseEntityState(cfg, arg, '=', t.entity, t.state, neg, why)) return false;
        if (neg) return (why = "a trigger cannot use !=", false);
        return true;
    }
    if (verb == "signal") {
        t.type = TriggerType::Signal;
        const std::size_t at = arg.find('@');
        if (!Parse(llworld::TrimView(arg.substr(0, at)), t.signal)) return (why = "unknown signal '" + std::string(arg.substr(0, at)) + "'", false);
        if (at != std::string_view::npos) {
            t.entity = FindById(cfg.entities, llworld::TrimView(arg.substr(at + 1)));
            if (t.entity == kNone) return (why = "unknown entity in '" + std::string(arg) + "'", false);
        }
        return true;
    }
    return (why = "unknown trigger '" + std::string(verb) + "'", false);
}

bool CanTrigger(const Action& a, const Trigger& t) {
    if (t.type == TriggerType::State) {
        if (a.entity != t.entity) return false;
        if (a.type == ActionType::Set) return a.state == t.state;
        if (a.type == ActionType::Start) return t.state == S::Active || t.state == S::Degraded;
        if (a.type == ActionType::Stop) return t.state == S::Offline || t.state == S::Standby;
        return false;
    }
    return a.type == ActionType::Signal && a.signal == t.signal && (t.entity == kNone || t.entity == a.entity);
}

bool Compile(const SimConfig& cfg, Compiled& c, std::vector<std::string>& out) {
    const std::size_t before = out.size();
    if (cfg.version != kSimConfigVersion)
        out.push_back("unsupported content version " + std::to_string(cfg.version) + " (this build reads version " +
                      std::to_string(kSimConfigVersion) + ")");
    if (!std::isfinite(cfg.ticksPerSecond) || cfg.ticksPerSecond <= 0.0) out.push_back("[simulation] ticks_per_second must be > 0");
    for (std::size_t t = 0; t < kTierCount; ++t) {
        if (cfg.tiers[t].intervalTicks == 0) out.push_back(std::string("[tier.") + kTierNames[t] + "] interval_ticks must be >= 1");
        if (!std::isfinite(cfg.tiers[t].radiusCm) || cfg.tiers[t].radiusCm < 0.0) out.push_back(std::string("[tier.") + kTierNames[t] + "] radius_cm must be >= 0");
    }
    if (cfg.tiers[0].intervalTicks > cfg.tiers[1].intervalTicks || cfg.tiers[1].intervalTicks > cfg.tiers[2].intervalTicks)
        out.push_back("tier intervals must satisfy High <= Medium <= Low (got " + std::to_string(cfg.tiers[0].intervalTicks) + ", " +
                      std::to_string(cfg.tiers[1].intervalTicks) + ", " + std::to_string(cfg.tiers[2].intervalTicks) + ")");
    if (cfg.tiers[0].radiusCm > cfg.tiers[1].radiusCm) out.push_back("tier radii must satisfy High <= Medium");
    if (cfg.relevanceIntervalTicks == 0) out.push_back("[simulation] relevance_interval_ticks must be >= 1");
    if (cfg.maxUpdatesPerStep == 0 || cfg.maxTimersPerStep == 0 || cfg.maxTransitionsPerStep == 0)
        out.push_back("[simulation] per-step budgets must be >= 1");
    if (cfg.eventLogCapacity == 0) out.push_back("[simulation] event_log_capacity must be >= 1");
    const std::size_t needed = 2 * cfg.entities.size() + cfg.schedules.size() + cfg.rules.size();
    if (cfg.queueCapacity < needed)
        out.push_back("[simulation] queue_capacity " + std::to_string(cfg.queueCapacity) + " is too small for " +
                      std::to_string(cfg.entities.size()) + " entities, " + std::to_string(cfg.schedules.size()) + " schedules and " +
                      std::to_string(cfg.rules.size()) + " rules (needs >= " + std::to_string(needed) + ")");
    CheckDuplicates(cfg.areas, "area", out);
    CheckDuplicates(cfg.entities, "entity", out);
    CheckDuplicates(cfg.schedules, "schedule", out);
    CheckDuplicates(cfg.rules, "rule", out);
    for (const AreaDef& a : cfg.areas) {
        if (!a.center.IsFinite() || !std::isfinite(a.radiusCm) || a.radiusCm <= 0.0) out.push_back("area '" + a.id + "': needs a finite center and radius_cm > 0");
    }

    c.entities.assign(cfg.entities.size(), CEntity{});
    for (std::size_t i = 0; i < cfg.entities.size(); ++i) {
        const EntityDef& d = cfg.entities[i];
        CEntity& e = c.entities[i];
        const std::string where = std::string(ToString(d.kind)) + " '" + d.id + "'";
        e.area = FindById(cfg.areas, d.area);
        if (d.area.empty()) out.push_back(where + ": needs an area");
        else if (e.area == kNone) out.push_back(where + ": unknown area '" + d.area + "'");
        if (d.hasPosition && !d.position.IsFinite()) out.push_back(where + ": position must be finite");
        for (const std::string& dep : d.dependsOn) {
            const std::uint32_t j = FindById(cfg.entities, dep);
            if (j == kNone) out.push_back(where + ": missing dependency '" + dep + "'");
            else if (j == i) out.push_back(where + ": depends on itself");
            else e.deps.push_back(j);
        }
        if (!d.bind.empty()) {
            const bool node = d.bind.rfind("node:", 0) == 0;
            const bool hazard = d.bind.rfind("hazard:", 0) == 0;
            e.bound = node || hazard;
            e.bindHazard = hazard;
            e.bindName = d.bind.substr(d.bind.find(':') + 1);
            if (!e.bound || e.bindName.empty()) out.push_back(where + ": bind must be node:<id> or hazard:<id>");
            else if (hazard && d.kind != K::HazardZone) out.push_back(where + ": only HazardZone entities can bind to a hazard");
            else if (node && (d.kind == K::HazardZone || d.kind == K::PatrolGroup || d.kind == K::Facility))
                out.push_back(where + ": " + ToString(d.kind) + " cannot bind to a power-grid node");
        }
        e.initial = DefaultInitial(d);
        if (!d.initialState.empty()) {
            if (!Parse(d.initialState, e.initial)) out.push_back(where + ": unknown state '" + d.initialState + "'");
            else if (!StateAllowed(d.kind, e.initial)) out.push_back(where + ": state " + d.initialState + " is not valid for " + ToString(d.kind));
        }
        if (d.kind == K::Generator) {
            if (!std::isfinite(d.fuelSeconds)) out.push_back(where + ": fuel_seconds must be finite (negative = unlimited)");
            if (!std::isfinite(d.burnRate) || d.burnRate <= 0.0) out.push_back(where + ": burn_rate must be > 0");
            if (!std::isfinite(d.lowFuelSeconds) || d.lowFuelSeconds < 0.0 || (d.fuelSeconds >= 0.0 && d.lowFuelSeconds > d.fuelSeconds))
                out.push_back(where + ": low_fuel_seconds must be within 0..fuel_seconds");
            if (!d.backupFor.empty()) {
                e.backupFor = FindById(cfg.entities, d.backupFor);
                if (e.backupFor == kNone) out.push_back(where + ": backup_for references missing entity '" + d.backupFor + "'");
                else if (e.backupFor == i || cfg.entities[e.backupFor].kind != K::Generator)
                    out.push_back(where + ": backup_for must name another Generator");
            }
        }
        if (d.kind == K::PatrolGroup) {
            if (d.route.empty()) out.push_back(where + ": needs a route of one or more areas");
            for (const std::string& a : d.route) {
                const std::uint32_t ai = FindById(cfg.areas, a);
                if (ai == kNone) out.push_back(where + ": route references unknown area '" + a + "'");
                else e.route.push_back(ai);
            }
            if (d.dwellTicks + d.travelTicks == 0) out.push_back(where + ": dwell_ticks + travel_ticks must be > 0");
            if (!std::isfinite(d.respondRadiusCm) || d.respondRadiusCm <= 0.0) out.push_back(where + ": respond_radius_cm must be > 0");
            if (!d.comms.empty()) {
                e.comms = FindById(cfg.entities, d.comms);
                if (e.comms == kNone) out.push_back(where + ": comms references missing entity '" + d.comms + "'");
                else if (cfg.entities[e.comms].kind != K::CommsStation) out.push_back(where + ": comms must be a CommsStation");
                else if (std::find(e.deps.begin(), e.deps.end(), e.comms) == e.deps.end()) e.deps.push_back(e.comms);
            }
        } else if (!d.route.empty() || !d.comms.empty()) {
            out.push_back(where + ": route/comms only apply to PatrolGroup");
        }
        if (d.kind == K::HazardZone && ((d.activeTicks == 0) != (d.dormantTicks == 0)))
            out.push_back(where + ": active_ticks and dormant_ticks must both be set for an autonomous cycle");
    }

    // Circular dependencies (iterative DFS; reports the cycle path).
    {
        const std::size_t n = c.entities.size();
        std::vector<std::uint8_t> color(n, 0);
        std::vector<std::uint32_t> parent(n, kNone);
        for (std::size_t root = 0; root < n; ++root) {
            if (color[root] != 0) continue;
            std::vector<std::pair<std::uint32_t, std::size_t>> stack{{static_cast<std::uint32_t>(root), 0}};
            color[root] = 1;
            while (!stack.empty()) {
                auto& [v, next] = stack.back();
                if (next < c.entities[v].deps.size()) {
                    const std::uint32_t w = c.entities[v].deps[next++];
                    if (color[w] == 0) {
                        color[w] = 1;
                        parent[w] = v;
                        stack.emplace_back(w, 0);
                    } else if (color[w] == 1) {
                        std::string path = cfg.entities[w].id;
                        for (std::uint32_t x = v; x != w && x != kNone; x = parent[x]) path = cfg.entities[x].id + " -> " + path;
                        out.push_back("circular dependency: " + cfg.entities[w].id + " -> " + path);
                    }
                } else {
                    color[v] = 2;
                    c.topo.push_back(v);  // post-order: dependencies first
                    stack.pop_back();
                }
            }
        }
    }

    for (const ScheduleDef& sd : cfg.schedules) {
        CSchedule cs;
        cs.at = sd.atTicks;
        cs.repeat = sd.repeatTicks;
        const std::string where = "schedule '" + sd.id + "'";
        if (sd.actions.empty()) out.push_back(where + ": needs at least one action (do = ...)");
        if (sd.repeatTicks != 0 && sd.repeatTicks < cfg.tiers[0].intervalTicks)
            out.push_back(where + ": repeat_ticks " + std::to_string(sd.repeatTicks) + " is shorter than the High tier interval");
        for (const std::string& t : sd.actions) {
            Action a;
            std::string why;
            if (!ParseAction(cfg, c, t, a, why)) {
                out.push_back(where + ": action '" + t + "': " + why);
                continue;
            }
            if (a.type == ActionType::Retask && sd.repeatTicks != 0) {
                const EntityDef& g = cfg.entities[a.entity];
                const Tick leg = g.dwellTicks + g.travelTicks;
                if (sd.repeatTicks < leg)
                    out.push_back(where + ": repeats every " + std::to_string(sd.repeatTicks) + " ticks but patrol '" + g.id + "' needs " +
                                  std::to_string(leg) + " ticks per leg; the group could never complete a leg");
            }
            cs.actions.push_back(a);
        }
        c.schedules.push_back(cs);
    }
    for (const RuleDef& rd : cfg.rules) {
        CRule r;
        r.delay = rd.delayTicks;
        r.once = rd.once;
        const std::string where = "rule '" + rd.id + "'";
        std::string why;
        if (rd.on.empty()) out.push_back(where + ": needs a trigger (on = ...)");
        else if (!ParseTrigger(cfg, rd.on, r.on, why)) out.push_back(where + ": trigger '" + rd.on + "': " + why);
        for (const std::string& q : rd.requires_) {
            Condition cond;
            if (!ParseEntityState(cfg, q.rfind("state:", 0) == 0 ? std::string_view(q).substr(6) : std::string_view(q), '=', cond.entity, cond.state,
                                  cond.negate, why))
                out.push_back(where + ": require '" + q + "': " + why);
            else r.require.push_back(cond);
        }
        if (rd.actions.empty()) out.push_back(where + ": needs at least one action (do = ...)");
        for (const std::string& t : rd.actions) {
            Action a;
            if (!ParseAction(cfg, c, t, a, why)) out.push_back(where + ": action '" + t + "': " + why);
            else r.actions.push_back(a);
        }
        c.rules.push_back(r);
    }
    // Rules that re-trigger each other forever (unless one of them fires once).
    {
        const std::size_t n = c.rules.size();
        std::vector<std::vector<std::uint32_t>> edges(n);
        for (std::size_t a = 0; a < n; ++a) {
            for (std::size_t b = 0; b < n; ++b) {
                for (const Action& act : c.rules[a].actions) {
                    if (CanTrigger(act, c.rules[b].on)) {
                        edges[a].push_back(static_cast<std::uint32_t>(b));
                        break;
                    }
                }
            }
        }
        std::vector<std::uint8_t> color(n, 0);
        std::vector<std::uint32_t> path;
        std::function<bool(std::uint32_t)> dfs = [&](std::uint32_t v) -> bool {
            color[v] = 1;
            path.push_back(v);
            for (std::uint32_t w : edges[v]) {
                if (color[w] == 1) {
                    auto it = std::find(path.begin(), path.end(), w);
                    bool anyOnce = false;
                    std::string text;
                    for (auto p = it; p != path.end(); ++p) {
                        anyOnce = anyOnce || c.rules[*p].once;
                        text += cfg.rules[*p].id + " -> ";
                    }
                    if (!anyOnce) {
                        out.push_back("circular rule chain: " + text + cfg.rules[w].id + " (mark one rule once = true or break the loop)");
                        return true;
                    }
                } else if (color[w] == 0 && dfs(w)) {
                    return true;
                }
            }
            color[v] = 2;
            path.pop_back();
            return false;
        };
        for (std::size_t v = 0; v < n; ++v) {
            if (color[v] == 0 && dfs(static_cast<std::uint32_t>(v))) break;
        }
    }
    return out.size() == before;
}

}  // namespace

std::vector<std::string> ValidateSim(const SimConfig& cfg) {
    Compiled c;
    std::vector<std::string> out;
    Compile(cfg, c, out);
    return out;
}

// ---------------------------------------------------------------- runtime

struct WorldSimulation::Impl {
    struct Instr {
        bool retask = false;
        std::vector<std::uint32_t> route;
        std::uint32_t area = kNone;
        Vec3 target;
    };
    struct ERt {
        S base = S::Nominal;
        S eff = S::Nominal;
        Tier tier = Tier::Low;
        Tick lastUpdate = 0;
        std::uint32_t area = kNone;
        std::uint32_t updates = 0;
        std::uint32_t updateGen = 0;
        std::array<std::uint32_t, kEntityTimerCount> timerGen{};
        std::array<bool, kEntityTimerCount> timerPending{};
        double fuel = -1.0;
        Tick fuelStamp = 0;
        bool running = false;
        std::vector<std::uint32_t> route;
        Tick epoch = 0;
        std::uint32_t leg = kNone;
        bool travelling = false;
        bool connected = true;
        std::vector<Instr> pending;
        bool hasDirective = false;
        PatrolDirective last;
        Vec3 respondTarget;
        std::uint32_t respondArea = kNone;
        Tick observedUntil = 0;
        bool boosted = false;
        bool ovr = false;
        S ovrState = S::Nominal;
        bool dirty = false;
    };

    WorldSimulation& w;
    Compiled c;
    std::vector<ERt> e;
    std::vector<std::vector<std::uint32_t>> dependents;
    std::vector<std::vector<std::uint32_t>> backups;
    std::vector<std::vector<std::uint32_t>> areaMembers;
    std::vector<Tier> areaTier;
    std::vector<std::vector<std::uint32_t>> stateRules;  // per entity
    std::array<std::vector<std::uint32_t>, kSignalKindCount> signalRules;
    std::vector<std::pair<std::string, std::uint32_t>> bindIndex;  // sorted by name
    std::vector<std::uint32_t> patrols;
    std::vector<std::uint8_t> ruleFired;
    BoundedScheduler updates;
    BoundedScheduler timers;
    std::vector<std::uint32_t> dirtyQueue;
    std::size_t dirtyHead = 0;
    std::vector<std::uint32_t> boosted;
    std::vector<Vec3> players;
    std::uint64_t seed = 0;
    Tick nextRelevance = 0;

    Impl(WorldSimulation& owner, Compiled compiled)
        : w(owner), c(std::move(compiled)), updates(owner.cfg_.queueCapacity), timers(owner.cfg_.queueCapacity) {
        const SimConfig& cfg = w.cfg_;
        const std::size_t n = cfg.entities.size();
        dependents.assign(n, {});
        backups.assign(n, {});
        stateRules.assign(n, {});
        for (std::size_t i = 0; i < n; ++i) {
            for (std::uint32_t d : c.entities[i].deps) dependents[d].push_back(static_cast<std::uint32_t>(i));
            if (c.entities[i].backupFor != kNone) backups[c.entities[i].backupFor].push_back(static_cast<std::uint32_t>(i));
            if (cfg.entities[i].kind == K::PatrolGroup) patrols.push_back(static_cast<std::uint32_t>(i));
            if (c.entities[i].bound) bindIndex.emplace_back(c.entities[i].bindName, static_cast<std::uint32_t>(i));
        }
        std::sort(bindIndex.begin(), bindIndex.end());
        for (std::size_t r = 0; r < c.rules.size(); ++r) {
            const Trigger& t = c.rules[r].on;
            if (t.type == TriggerType::State && t.entity < n) stateRules[t.entity].push_back(static_cast<std::uint32_t>(r));
            if (t.type == TriggerType::Signal) signalRules[static_cast<std::size_t>(t.signal)].push_back(static_cast<std::uint32_t>(r));
        }
        Reset();
    }

    const EntityDef& D(std::uint32_t i) const { return w.cfg_.entities[i]; }
    double Tps() const { return w.cfg_.ticksPerSecond; }

    void Reset() {
        const SimConfig& cfg = w.cfg_;
        e.assign(cfg.entities.size(), ERt{});
        areaMembers.assign(cfg.areas.size(), {});
        areaTier.assign(cfg.areas.size(), Tier::Low);
        ruleFired.assign(c.rules.size(), 0);
        updates.Clear();
        timers.Clear();
        dirtyQueue.clear();
        dirtyHead = 0;
        boosted.clear();
        for (std::size_t i = 0; i < e.size(); ++i) {
            ERt& r = e[i];
            const EntityDef& d = D(static_cast<std::uint32_t>(i));
            r.base = c.entities[i].initial;
            r.area = c.entities[i].area;
            r.fuel = d.fuelSeconds;
            r.running = d.kind == K::Generator && (r.base == S::Active || r.base == S::Degraded);
            r.route = c.entities[i].route;
            if (r.area != kNone) areaMembers[r.area].push_back(static_cast<std::uint32_t>(i));
        }
        // Initial effective states in dependency order; no events for the authored start.
        for (std::uint32_t i : c.topo) e[i].eff = Derive(i);
    }

    // ------------------------------------------------------------ helpers
    void Log(SimEventType type, std::uint32_t entity, std::uint8_t from = 0, std::uint8_t to = 0, std::uint32_t aux = 0,
             std::uint32_t area = kNone) {
        SimEvent ev{w.now_, type, entity, from, to, aux, area};
        if (w.log_.size() < w.cfg_.eventLogCapacity) {
            w.log_.push_back(ev);
        } else {
            w.log_[w.logHead_] = ev;
            w.logHead_ = (w.logHead_ + 1) % w.log_.size();
        }
        ++w.logged_;
        if (w.observer_ != nullptr) w.observer_->OnSimEvent(ev);
    }

    Vec3 PositionOf(std::uint32_t i) const {
        const ERt& r = e[i];
        if (D(i).kind == K::PatrolGroup && r.eff == S::Responding) return r.respondTarget;
        if (D(i).kind != K::PatrolGroup && D(i).hasPosition) return D(i).position;
        return r.area != kNone ? w.cfg_.areas[r.area].center : Vec3{};
    }

    bool Push(BoundedScheduler& q, Tick due, std::uint32_t target, std::uint16_t kind, std::uint32_t gen) {
        if (q.Push(due, target, kind, gen)) return true;
        q.Compact([this](const SchedEntry& s) { return Live(s); });
        if (q.Push(due, target, kind, gen)) return true;
        ++w.stats_.queueOverflows;
        return false;
    }
    bool Live(const SchedEntry& s) const {
        if (s.kind == kUpdate) return s.target < e.size() && e[s.target].updateGen == s.gen;
        if (s.kind < kEntityTimerCount) return s.target < e.size() && e[s.target].timerGen[s.kind] == s.gen && e[s.target].timerPending[s.kind];
        return true;
    }
    void Schedule(std::uint32_t i, TimerKind k, Tick due) {
        ERt& r = e[i];
        ++r.timerGen[k];
        r.timerPending[k] = true;
        if (!Push(timers, std::max(due, w.now_), i, k, r.timerGen[k])) r.timerPending[k] = false;
    }
    void Cancel(std::uint32_t i, TimerKind k) {
        ++e[i].timerGen[k];
        e[i].timerPending[k] = false;
    }
    void ScheduleUpdate(std::uint32_t i, Tick due) {
        ERt& r = e[i];
        ++r.updateGen;
        Push(updates, std::max(due, w.now_), i, kUpdate, r.updateGen);
    }
    Tick Interval(std::uint32_t i) const { return w.cfg_.tiers[static_cast<std::size_t>(e[i].tier)].intervalTicks; }
    Tick SecondsToTicks(double s) const { return static_cast<Tick>(std::ceil(std::max(0.0, s) * Tps())); }

    void MarkDirty(std::uint32_t i) {
        if (e[i].dirty) return;
        e[i].dirty = true;
        dirtyQueue.push_back(i);
    }
    void SetBase(std::uint32_t i, S s) {
        if (e[i].base == s) return;
        e[i].base = s;
        MarkDirty(i);
    }

    // ------------------------------------------------------------ derivation
    S Derive(std::uint32_t i) const {
        const ERt& r = e[i];
        if (r.ovr) return r.ovrState;
        const EntityDef& d = D(i);
        bool netOffline = false;
        bool netDegraded = false;
        bool allNetsOffline = true;
        bool anyNet = false;
        bool facilityLockdown = false;
        bool alarm = false;
        bool primaryUp = false;
        bool backupUp = false;
        bool anyGen = false;
        for (std::uint32_t dep : c.entities[i].deps) {
            const S s = e[dep].eff;
            switch (D(dep).kind) {
                case K::PowerNetwork:
                    anyNet = true;
                    netOffline = netOffline || s == S::Offline;
                    netDegraded = netDegraded || s == S::Degraded;
                    allNetsOffline = allNetsOffline && s == S::Offline;
                    break;
                case K::Generator:
                    anyGen = true;
                    if (Up(s)) (c.entities[dep].backupFor != kNone ? backupUp : primaryUp) = true;
                    break;
                case K::Facility: facilityLockdown = facilityLockdown || s == S::Lockdown; break;
                case K::SecuritySystem: alarm = alarm || s == S::Alarm; break;
                case K::HazardZone: alarm = alarm || s == S::Active; break;
                default: break;
            }
        }
        switch (d.kind) {
            case K::Generator:
            case K::HazardZone: return r.base;
            case K::PowerNetwork: {
                if (c.entities[i].bound) return r.base;
                S s = r.base;
                if (anyGen) s = primaryUp ? S::Nominal : (backupUp ? S::Degraded : S::Offline);
                if (netOffline) return S::Offline;  // fed from an upstream network
                if (netDegraded && s == S::Nominal) return S::Degraded;
                return s;
            }
            case K::SecuritySystem:
            case K::CommsStation:
            case K::Machinery: return netOffline ? S::Offline : r.base;
            case K::Storage:
                if (netOffline) return S::Offline;
                return facilityLockdown ? S::Lockdown : r.base;
            case K::Facility:
                if (r.base == S::Lockdown) return S::Lockdown;
                if (alarm || r.base == S::Alarm) return S::Alarm;
                if (anyNet && allNetsOffline) return S::Offline;
                if (anyNet && (netOffline || netDegraded)) return S::Degraded;
                return r.base;
            case K::PatrolGroup: {
                if (r.base == S::Offline || r.base == S::Responding) return r.base;
                const std::uint32_t comms = c.entities[i].comms;
                if (comms != kNone && e[comms].eff != S::Nominal) return S::Interrupted;
                return S::Active;
            }
        }
        return r.base;
    }

    void ProcessTransitions() {
        std::size_t budget = w.cfg_.maxTransitionsPerStep;
        while (dirtyHead < dirtyQueue.size()) {
            if (budget == 0) {
                w.stats_.deferredTransitions += dirtyQueue.size() - dirtyHead;
                break;
            }
            const std::uint32_t i = dirtyQueue[dirtyHead++];
            e[i].dirty = false;
            const S next = Derive(i);
            if (next == e[i].eff) continue;
            --budget;
            const S prev = e[i].eff;
            e[i].eff = next;
            ++w.stats_.transitions;
            Log(SimEventType::StateChanged, i, static_cast<std::uint8_t>(prev), static_cast<std::uint8_t>(next), 0, e[i].area);
            OnChanged(i, prev, next);
        }
        if (dirtyHead >= dirtyQueue.size()) {
            dirtyQueue.clear();
            dirtyHead = 0;
        } else if (dirtyHead > 1024) {
            dirtyQueue.erase(dirtyQueue.begin(), dirtyQueue.begin() + static_cast<std::ptrdiff_t>(dirtyHead));
            dirtyHead = 0;
        }
    }

    void OnChanged(std::uint32_t i, S prev, S next) {
        for (std::uint32_t d : dependents[i]) MarkDirty(d);
        const EntityDef& d = D(i);
        if (d.kind == K::Generator) {
            if (Up(prev) && !Up(next)) {
                for (std::uint32_t b : backups[i]) {
                    if (!e[b].running) Schedule(b, kBackupStart, w.now_ + D(b).startDelayTicks);
                }
            } else if (!Up(prev) && Up(next)) {
                for (std::uint32_t b : backups[i]) {
                    Cancel(b, kBackupStart);
                    if (e[b].running) StopGen(b, S::Standby);
                }
            }
        }
        if (d.kind == K::PatrolGroup) RefreshLink(i);
        if (d.kind == K::CommsStation) {
            for (std::uint32_t g : dependents[i]) {
                if (D(g).kind == K::PatrolGroup) RefreshLink(g);
            }
        }
        for (std::uint32_t r : stateRules[i]) {
            if (c.rules[r].on.state == next) FireRule(r);
        }
        RecomputeTier(i);
    }

    // ------------------------------------------------------------ rules & actions
    bool Conditions(std::uint32_t r) const {
        for (const Condition& q : c.rules[r].require) {
            const bool eq = e[q.entity].eff == q.state;
            if (eq == q.negate) return false;
        }
        return true;
    }
    void FireRule(std::uint32_t r) {
        if (c.rules[r].once && ruleFired[r]) return;
        if (!Conditions(r)) return;
        if (c.rules[r].delay > 0) {
            Push(timers, w.now_ + c.rules[r].delay, r, kRuleFire, 0);
            return;
        }
        ExecuteRule(r);
    }
    void ExecuteRule(std::uint32_t r) {
        if (c.rules[r].once && ruleFired[r]) return;
        ruleFired[r] = 1;
        ++w.stats_.rulesFired;
        Log(SimEventType::RuleFired, kNone, 0, 0, r);
        for (const Action& a : c.rules[r].actions) Execute(a);
    }
    void Execute(const Action& a) {
        switch (a.type) {
            case ActionType::Set:
                if (D(a.entity).kind == K::Generator) {
                    if (Up(a.state)) StartGen(a.entity);
                    else StopGen(a.entity, a.state);
                } else if (D(a.entity).kind == K::PatrolGroup && a.state == S::Active && e[a.entity].base == S::Responding) {
                    EndResponse(a.entity);
                } else {
                    SetBase(a.entity, a.state);
                }
                break;
            case ActionType::Alert: Dispatch(PositionOf(a.entity), e[a.entity].area, true); break;
            case ActionType::Retask: {
                Instr in;
                in.retask = true;
                in.route = a.route;
                DeliverOrQueue(a.entity, in);
                break;
            }
            case ActionType::Start: StartGen(a.entity); break;
            case ActionType::Stop: StopGen(a.entity, S::Offline); break;
            case ActionType::Signal: ApplySignal(a.signal, a.entity, a.entity != kNone ? PositionOf(a.entity) : Vec3{}); break;
        }
    }

    // ------------------------------------------------------------ signals
    void ApplySignal(SignalKind k, std::uint32_t i, const Vec3& pos) {
        if (i != kNone) {
            const K kind = D(i).kind;
            switch (k) {
                case SignalKind::PowerLost:
                    if (kind == K::Generator) {
                        if (c.entities[i].bound) SetBase(i, S::Offline);
                        else StopGen(i, S::Offline);
                    } else if (kind == K::Machinery || kind == K::PowerNetwork || kind == K::CommsStation || kind == K::Storage ||
                               kind == K::SecuritySystem) {
                        SetBase(i, S::Offline);
                    }
                    break;
                case SignalKind::PowerRestored:
                    if (kind == K::Generator) {
                        if (c.entities[i].bound) SetBase(i, S::Active);
                        else StartGen(i);
                    } else if (kind == K::Machinery) {
                        SetBase(i, S::Active);
                    } else if (kind == K::PowerNetwork || kind == K::CommsStation || kind == K::Storage || kind == K::SecuritySystem) {
                        SetBase(i, S::Nominal);
                    }
                    break;
                case SignalKind::AlarmRaised:
                    if (kind == K::SecuritySystem || kind == K::Facility) SetBase(i, S::Alarm);
                    break;
                case SignalKind::AlarmCleared:
                    if ((kind == K::SecuritySystem || kind == K::Facility) && e[i].base == S::Alarm) SetBase(i, S::Nominal);
                    break;
                case SignalKind::CommsLost:
                    if (kind == K::CommsStation) SetBase(i, S::Interrupted);
                    break;
                case SignalKind::CommsRestored:
                    if (kind == K::CommsStation && e[i].base == S::Interrupted) SetBase(i, S::Nominal);
                    break;
                case SignalKind::HazardActivated:
                    if (kind == K::HazardZone) SetBase(i, S::Active);
                    break;
                case SignalKind::HazardCleared:
                    if (kind == K::HazardZone) SetBase(i, S::Nominal);
                    break;
                default: break;
            }
        }
        // Process the direct effect first so rule conditions see the new state.
        ProcessTransitions();
        for (std::uint32_t r : signalRules[static_cast<std::size_t>(k)]) {
            if (c.rules[r].on.entity == kNone || c.rules[r].on.entity == i) FireRule(r);
        }
        (void)pos;
    }

    // ------------------------------------------------------------ generators
    double FuelAt(std::uint32_t i) const {
        const ERt& r = e[i];
        if (r.fuel < 0.0) return -1.0;
        if (!r.running) return r.fuel;
        const double used = static_cast<double>(w.now_ - r.fuelStamp) / Tps() * D(i).burnRate;
        return std::max(0.0, r.fuel - used);
    }
    void StartGen(std::uint32_t i) {
        ERt& r = e[i];
        if (r.running) return;
        if (r.fuel == 0.0) {
            SetBase(i, S::Offline);
            return;
        }
        r.running = true;
        r.fuelStamp = w.now_;
        SetBase(i, (r.fuel >= 0.0 && r.fuel <= D(i).lowFuelSeconds) ? S::Degraded : S::Active);
        ScheduleFuel(i);
    }
    void StopGen(std::uint32_t i, S next) {
        ERt& r = e[i];
        r.fuel = FuelAt(i);
        r.fuelStamp = w.now_;
        r.running = false;
        Cancel(i, kFuelLow);
        Cancel(i, kFuelEmpty);
        SetBase(i, next == S::Standby ? S::Standby : S::Offline);
    }
    void ScheduleFuel(std::uint32_t i) {
        const ERt& r = e[i];
        if (r.fuel < 0.0 || !r.running) return;
        const double burn = D(i).burnRate;
        Schedule(i, kFuelEmpty, w.now_ + SecondsToTicks(r.fuel / burn));
        if (r.fuel > D(i).lowFuelSeconds) Schedule(i, kFuelLow, w.now_ + SecondsToTicks((r.fuel - D(i).lowFuelSeconds) / burn));
    }

    // ------------------------------------------------------------ patrols
    void Emit(std::uint32_t g, DirectiveKind k, std::uint32_t area, const Vec3& target, bool central) {
        PatrolDirective d;
        d.group = g;
        d.kind = k;
        d.area = area;
        d.target = target;
        d.central = central;
        d.tick = w.now_;
        e[g].last = d;
        e[g].hasDirective = true;
        ++w.stats_.directives;
        Log(SimEventType::Directive, g, 0, 0, static_cast<std::uint32_t>(k), area);
        if (w.observer_ != nullptr) w.observer_->OnPatrolDirective(d);
    }
    void MoveToArea(std::uint32_t i, std::uint32_t area) {
        ERt& r = e[i];
        if (r.area == area) return;
        if (r.area != kNone) {
            auto& m = areaMembers[r.area];
            m.erase(std::remove(m.begin(), m.end(), i), m.end());
        }
        r.area = area;
        if (area != kNone) areaMembers[area].push_back(i);
        RecomputeTier(i);
    }
    // Analytic route position: identical results whether updated every tick or every 10 s.
    void AdvancePatrol(std::uint32_t g) {
        ERt& r = e[g];
        if (r.base != S::Active || r.route.empty()) return;
        const EntityDef& d = D(g);
        const Tick legLen = d.dwellTicks + d.travelTicks;
        const Tick cycle = legLen * r.route.size();
        const Tick t = (w.now_ - r.epoch) % cycle;
        const std::uint32_t leg = static_cast<std::uint32_t>(t / legLen);
        const bool travelling = (t % legLen) < d.travelTicks;
        if (leg == r.leg && travelling == r.travelling) return;
        r.leg = leg;
        r.travelling = travelling;
        const std::uint32_t area = r.route[leg];
        MoveToArea(g, area);
        Emit(g, travelling ? DirectiveKind::MoveTo : DirectiveKind::Hold, area, w.cfg_.areas[area].center, false);
    }
    Tick NextPhaseChange(std::uint32_t g) const {
        const ERt& r = e[g];
        const EntityDef& d = D(g);
        if (r.base != S::Active || r.route.empty()) return w.now_ + Interval(g);
        const Tick legLen = d.dwellTicks + d.travelTicks;
        const Tick inLeg = ((w.now_ - r.epoch) % (legLen * r.route.size())) % legLen;
        return w.now_ + (inLeg < d.travelTicks ? d.travelTicks - inLeg : legLen - inLeg);
    }
    void Respond(std::uint32_t g, const Vec3& target, std::uint32_t area, bool central) {
        ERt& r = e[g];
        r.respondTarget = target;
        r.respondArea = area;
        r.leg = kNone;
        SetBase(g, S::Responding);
        Schedule(g, kResponseEnd, w.now_ + D(g).travelTicks + D(g).respondHoldTicks);
        if (area != kNone) MoveToArea(g, area);
        Emit(g, DirectiveKind::Respond, area, target, central);
    }
    void EndResponse(std::uint32_t g) {
        ERt& r = e[g];
        Cancel(g, kResponseEnd);
        r.epoch = w.now_;
        r.leg = kNone;
        SetBase(g, S::Active);
        Emit(g, DirectiveKind::ResumeRoute, r.route.empty() ? kNone : r.route[0], r.route.empty() ? Vec3{} : w.cfg_.areas[r.route[0]].center,
             false);
    }
    bool CommsOk(std::uint32_t g) const {
        const std::uint32_t comms = c.entities[g].comms;
        return comms == kNone || e[comms].eff == S::Nominal;
    }
    // Central instructions need a working link; held orders are delivered on reconnect.
    void RefreshLink(std::uint32_t g) {
        const bool was = e[g].connected;
        e[g].connected = CommsOk(g);
        if (!was && e[g].connected) FlushInstructions(g);
    }
    void DeliverOrQueue(std::uint32_t g, const Instr& in) {
        ERt& r = e[g];
        if (r.base == S::Offline) return;
        if (r.connected) {
            ApplyInstr(g, in);
            return;
        }
        if (r.pending.size() >= w.cfg_.maxPendingInstructions) {
            r.pending.erase(r.pending.begin());
            Log(SimEventType::InstructionDropped, g);
        }
        r.pending.push_back(in);
        Log(SimEventType::InstructionQueued, g);
    }
    void ApplyInstr(std::uint32_t g, const Instr& in) {
        ERt& r = e[g];
        if (in.retask) {
            r.route = in.route;
            if (r.base == S::Active) {
                r.epoch = w.now_;
                r.leg = kNone;
                AdvancePatrol(g);
            }
        } else {
            Respond(g, in.target, in.area, true);
        }
    }
    void FlushInstructions(std::uint32_t g) {
        std::vector<Instr> list;
        list.swap(e[g].pending);
        for (const Instr& in : list) {
            Log(SimEventType::InstructionDelivered, g);
            ApplyInstr(g, in);
        }
    }
    // Nearest group that can hear it responds on its own; otherwise central command
    // sends the nearest group, which needs a working comms link.
    void Dispatch(const Vec3& pos, std::uint32_t area, bool centralAllowed, double hearingScale = 1.0) {
        std::uint32_t local = kNone;
        std::uint32_t remote = kNone;
        double bestLocal = 0.0;
        double bestRemote = 0.0;
        for (std::uint32_t g : patrols) {
            if (e[g].base == S::Offline || e[g].base == S::Responding) continue;
            const double dist = llworld::Distance(PositionOf(g), pos);
            if (dist <= D(g).respondRadiusCm * hearingScale && (local == kNone || dist < bestLocal)) {
                local = g;
                bestLocal = dist;
            }
            if (remote == kNone || dist < bestRemote) {
                remote = g;
                bestRemote = dist;
            }
        }
        if (local != kNone) {
            Respond(local, pos, area, false);
            return;
        }
        if (!centralAllowed || remote == kNone) return;
        Instr in;
        in.area = area;
        in.target = pos;
        DeliverOrQueue(remote, in);
    }

    // ------------------------------------------------------------ relevance / tiers
    Tier ComputeAreaTier(std::uint32_t a) const {
        const AreaDef& ad = w.cfg_.areas[a];
        double best = 1e300;
        for (const Vec3& p : players) best = std::min(best, std::max(0.0, llworld::Distance(p, ad.center) - ad.radiusCm));
        if (best <= w.cfg_.tiers[0].radiusCm) return Tier::High;
        if (best <= w.cfg_.tiers[1].radiusCm || ad.objective) return Tier::Medium;
        return Tier::Low;
    }
    Tier TierFor(std::uint32_t i) const {
        const ERt& r = e[i];
        Tier t = r.area != kNone ? areaTier[r.area] : Tier::Low;
        if (r.observedUntil > w.now_) return Tier::High;
        const bool threat = r.eff == S::Alarm || r.eff == S::Lockdown || r.eff == S::Responding || (D(i).kind == K::HazardZone && r.eff == S::Active);
        if (threat) t = t == Tier::Low ? Tier::Medium : Tier::High;
        return t;
    }
    void RecomputeTier(std::uint32_t i) {
        ERt& r = e[i];
        const Tier t = TierFor(i);
        if (t == r.tier) return;
        const Tier old = r.tier;
        r.tier = t;
        Log(SimEventType::TierChanged, i, static_cast<std::uint8_t>(old), static_cast<std::uint8_t>(t), 0, r.area);
        if (w.phase_ != RaidPhase::Active) return;
        // Promotion catches up immediately; demotion just stretches the next update.
        ScheduleUpdate(i, t < old ? w.now_ : std::max(w.now_ + 1, r.lastUpdate + Interval(i)));
    }
    void RelevancePass() {
        ++w.stats_.relevancePasses;
        for (std::uint32_t a = 0; a < areaTier.size(); ++a) {
            const Tier t = ComputeAreaTier(a);
            if (t == areaTier[a]) continue;
            areaTier[a] = t;
            const std::vector<std::uint32_t> members = areaMembers[a];  // RecomputeTier may move patrols
            for (std::uint32_t i : members) RecomputeTier(i);
        }
        for (std::size_t k = 0; k < boosted.size();) {
            const std::uint32_t i = boosted[k];
            if (e[i].observedUntil <= w.now_) {
                e[i].boosted = false;
                boosted[k] = boosted.back();
                boosted.pop_back();
                RecomputeTier(i);
            } else {
                ++k;
            }
        }
    }
    void Boost(std::uint32_t i) {
        ERt& r = e[i];
        r.observedUntil = w.now_ + w.cfg_.observedWindowTicks;
        if (!r.boosted) {
            r.boosted = true;
            boosted.push_back(i);
        }
        RecomputeTier(i);
    }

    // ------------------------------------------------------------ updates & timers
    void Resample(std::uint32_t i) {
        if (!c.entities[i].bound || w.infra_ == nullptr) return;
        const CEntity& ce = c.entities[i];
        if (ce.bindHazard) {
            SetBase(i, w.infra_->HazardActive(ce.bindName) ? S::Active : S::Nominal);
            return;
        }
        const InfraSample s = w.infra_->SampleNode(ce.bindName);
        if (!s.known) return;
        switch (D(i).kind) {
            case K::Generator: SetBase(i, s.running ? S::Active : S::Offline); break;
            case K::Machinery: SetBase(i, s.powered && s.running ? S::Active : S::Offline); break;
            case K::SecuritySystem: SetBase(i, !s.powered ? S::Offline : (s.alarm ? S::Alarm : S::Nominal)); break;
            case K::PowerNetwork:
            case K::CommsStation:
            case K::Storage: SetBase(i, s.powered ? S::Nominal : S::Offline); break;
            default: break;
        }
    }
    void Update(std::uint32_t i) {
        ERt& r = e[i];
        r.lastUpdate = w.now_;
        ++r.updates;
        ++w.stats_.updatesByTier[static_cast<std::size_t>(r.tier)];
        Resample(i);
        Tick next = w.now_ + Interval(i);
        if (D(i).kind == K::PatrolGroup) {
            AdvancePatrol(i);
            if (r.tier != Tier::Low) next = std::min(next, NextPhaseChange(i));
        }
        ScheduleUpdate(i, std::max(next, w.now_ + 1));
    }
    void FireTimer(const SchedEntry& s) {
        ++w.stats_.timersFired;
        if (s.kind == kRuleFire) {
            if (Conditions(s.target)) ExecuteRule(s.target);
            return;
        }
        if (s.kind == kScheduleFire) {
            const CSchedule& cs = c.schedules[s.target];
            Log(SimEventType::ScheduleFired, kNone, 0, 0, s.target);
            for (const Action& a : cs.actions) Execute(a);
            if (cs.repeat > 0) Push(timers, s.due + cs.repeat, s.target, kScheduleFire, 0);
            return;
        }
        const std::uint32_t i = s.target;
        ERt& r = e[i];
        r.timerPending[s.kind] = false;
        // Timers are evaluated at their due tick, so deferral never changes the outcome.
        const Tick saved = w.now_;
        switch (s.kind) {
            case kFuelLow:
                if (r.running) {
                    Log(SimEventType::FuelLow, i);
                    SetBase(i, S::Degraded);
                }
                break;
            case kFuelEmpty:
                if (r.running) {
                    w.now_ = s.due;
                    r.fuel = 0.0;
                    r.fuelStamp = s.due;
                    r.running = false;
                    Cancel(i, kFuelLow);
                    w.now_ = saved;
                    Log(SimEventType::FuelEmpty, i);
                    SetBase(i, S::Offline);
                }
                break;
            case kBackupStart: {
                const std::uint32_t primary = c.entities[i].backupFor;
                if (primary != kNone && !Up(e[primary].eff) && !r.running && r.fuel != 0.0) {
                    Log(SimEventType::BackupStarted, i);
                    StartGen(i);
                }
                break;
            }
            case kResponseEnd:
                if (r.base == S::Responding) EndResponse(i);
                break;
            case kHazardCycle: {
                const EntityDef& d = D(i);
                const bool activeNow = r.base == S::Active;
                SetBase(i, activeNow ? S::Nominal : S::Active);
                Schedule(i, kHazardCycle, s.due + (activeNow ? d.dormantTicks : d.activeTicks));
                break;
            }
            case kOverrideExpire:
                if (r.ovr) {
                    r.ovr = false;
                    Log(SimEventType::OverrideExpired, i);
                    MarkDirty(i);
                }
                break;
            default: break;
        }
    }

    void BeginRaid(std::uint64_t raidSeed, const std::vector<PersistentEntityRecord>& persisted) {
        Reset();
        seed = raidSeed;
        for (const PersistentEntityRecord& p : persisted) {
            const std::uint32_t i = w.Find(p.id);
            if (i == kNone || !D(i).persist || !StateAllowed(D(i).kind, p.state)) continue;
            e[i].base = p.state;
            if (D(i).kind == K::Generator) {
                e[i].fuel = p.fuelSeconds;
                e[i].running = Up(p.state) && p.fuelSeconds != 0.0;
                if (!e[i].running && Up(p.state)) e[i].base = S::Offline;
            }
        }
        for (std::uint32_t i : c.topo) e[i].eff = Derive(i);
        for (std::uint32_t a = 0; a < areaTier.size(); ++a) areaTier[a] = ComputeAreaTier(a);
        for (std::uint32_t i = 0; i < e.size(); ++i) {
            e[i].tier = TierFor(i);
            e[i].connected = D(i).kind != K::PatrolGroup || CommsOk(i);
            // Deterministic stagger so thousands of entities never update on the same tick.
            const Tick interval = Interval(i);
            ScheduleUpdate(i, 1 + llworld::Mix64(llworld::HashCombine(seed, i)) % interval);
            if (D(i).kind == K::Generator && e[i].running) {
                e[i].fuelStamp = 0;
                ScheduleFuel(i);
            }
            if (D(i).kind == K::HazardZone && D(i).activeTicks > 0 && !c.entities[i].bound)
                Schedule(i, kHazardCycle, e[i].base == S::Active ? D(i).activeTicks : D(i).dormantTicks);
        }
        for (std::uint32_t sIdx = 0; sIdx < c.schedules.size(); ++sIdx) Push(timers, std::max<Tick>(1, c.schedules[sIdx].at), sIdx, kScheduleFire, 0);
        for (std::uint32_t g : patrols) AdvancePatrol(g);
        nextRelevance = 0;
    }

    void Step() {
        const SimConfig& cfg = w.cfg_;
        if (w.now_ >= nextRelevance) {
            RelevancePass();
            nextRelevance = w.now_ + cfg.relevanceIntervalTicks;
        }
        std::size_t budget = cfg.maxTimersPerStep;
        while (timers.HasDue(w.now_)) {
            const SchedEntry s = timers.Top();
            if (!Live(s)) {
                timers.Pop();
                ++w.stats_.staleSkipped;
                continue;
            }
            if (budget == 0) {
                ++w.stats_.deferredTimers;
                break;
            }
            timers.Pop();
            --budget;
            FireTimer(s);
            ProcessTransitions();
        }
        budget = cfg.maxUpdatesPerStep;
        while (updates.HasDue(w.now_)) {
            const SchedEntry s = updates.Top();
            if (!Live(s)) {
                updates.Pop();
                ++w.stats_.staleSkipped;
                continue;
            }
            if (budget == 0) {
                ++w.stats_.deferredUpdates;
                break;
            }
            updates.Pop();
            --budget;
            Update(s.target);
        }
        ProcessTransitions();
    }
};

// ---------------------------------------------------------------- public API

WorldSimulation::WorldSimulation(const SimConfig& config) : cfg_(config) {
    Compiled c;
    if (!Compile(cfg_, c, errors_)) return;
    impl_ = std::make_unique<Impl>(*this, std::move(c));
}

WorldSimulation::~WorldSimulation() = default;

bool WorldSimulation::BeginRaid(std::uint64_t raidSeed, const std::vector<PersistentEntityRecord>& persisted) {
    if (!impl_) return false;
    now_ = 0;
    log_.clear();
    logHead_ = 0;
    logged_ = 0;
    stats_ = SimStats{};
    phase_ = RaidPhase::Active;
    impl_->BeginRaid(raidSeed, persisted);
    return true;
}

void WorldSimulation::CompleteMission() {
    if (phase_ == RaidPhase::Active) phase_ = RaidPhase::Completed;
}

void WorldSimulation::ResetWorld() {
    phase_ = RaidPhase::Initial;
    now_ = 0;
    log_.clear();
    logHead_ = 0;
    logged_ = 0;
    stats_ = SimStats{};
    if (impl_) impl_->Reset();
}

std::vector<PersistentEntityRecord> WorldSimulation::ExportPersistentState() const {
    std::vector<PersistentEntityRecord> out;
    if (!impl_) return out;
    for (std::uint32_t i = 0; i < impl_->e.size(); ++i) {
        if (!cfg_.entities[i].persist) continue;
        PersistentEntityRecord r;
        r.id = cfg_.entities[i].id;
        // Mission overrides are temporary and never persist.
        r.state = impl_->e[i].base;
        r.fuelSeconds = impl_->FuelAt(i);
        out.push_back(r);
    }
    return out;
}

bool WorldSimulation::ApplyMissionOverride(std::uint32_t i, EntityState s, Tick duration) {
    if (!impl_ || phase_ != RaidPhase::Active || i >= impl_->e.size() || !StateAllowed(cfg_.entities[i].kind, s)) return false;
    auto& r = impl_->e[i];
    r.ovr = true;
    r.ovrState = s;
    impl_->Log(SimEventType::OverrideApplied, i, 0, static_cast<std::uint8_t>(s));
    if (duration > 0) impl_->Schedule(i, kOverrideExpire, now_ + duration);
    else impl_->Cancel(i, kOverrideExpire);
    impl_->MarkDirty(i);
    impl_->ProcessTransitions();
    return true;
}

void WorldSimulation::SetPlayers(const std::vector<Vec3>& positions) {
    if (!impl_) return;
    impl_->players.clear();
    for (const Vec3& p : positions) {
        if (impl_->players.size() >= 4) break;
        if (p.IsFinite()) impl_->players.push_back(p);
    }
}

void WorldSimulation::ReportObserved(std::uint32_t i) {
    if (impl_ && phase_ == RaidPhase::Active && i < impl_->e.size()) impl_->Boost(i);
}
void WorldSimulation::ReportInteraction(std::uint32_t i) { ReportObserved(i); }

void WorldSimulation::NotifySignal(SignalKind kind, std::string_view ref, const Vec3& position, double magnitude) {
    if (!impl_ || phase_ != RaidPhase::Active) return;
    (void)magnitude;
    const auto& idx = impl_->bindIndex;
    auto lo = std::lower_bound(idx.begin(), idx.end(), ref, [](const auto& a, std::string_view b) { return a.first < b; });
    bool any = false;
    for (auto it = lo; it != idx.end() && it->first == ref; ++it) {
        any = true;
        impl_->ApplySignal(kind, it->second, position);
    }
    if (!any) {
        const std::uint32_t byId = Find(ref);
        impl_->ApplySignal(kind, byId, position);
    }
    impl_->ProcessTransitions();
}

void WorldSimulation::NotifySignalForEntity(SignalKind kind, std::uint32_t i) {
    if (!impl_ || phase_ != RaidPhase::Active) return;
    impl_->ApplySignal(kind, i < impl_->e.size() ? i : kNone, i < impl_->e.size() ? impl_->PositionOf(i) : Vec3{});
    impl_->ProcessTransitions();
}

void WorldSimulation::NotifyBoundChanged(std::string_view ref) {
    if (!impl_ || phase_ != RaidPhase::Active) return;
    const auto& idx = impl_->bindIndex;
    auto lo = std::lower_bound(idx.begin(), idx.end(), ref, [](const auto& a, std::string_view b) { return a.first < b; });
    for (auto it = lo; it != idx.end() && it->first == ref; ++it) impl_->Resample(it->second);
    impl_->ProcessTransitions();
}

void WorldSimulation::ReportDisturbance(const Vec3& position, double loudnessDb) {
    if (!impl_ || phase_ != RaidPhase::Active || !position.IsFinite() || !std::isfinite(loudnessDb)) return;
    // Quiet sounds carry less far: 20 dB is inaudible, 80 dB reaches the full respond radius.
    const double scale = std::clamp((loudnessDb - 20.0) / 60.0, 0.0, 1.5);
    if (scale > 0.0) {
        std::uint32_t nearest = kNone;
        double best = 1e300;
        for (std::uint32_t a = 0; a < cfg_.areas.size(); ++a) {
            const double d = llworld::Distance(cfg_.areas[a].center, position);
            if (d < best) {
                best = d;
                nearest = a;
            }
        }
        impl_->Dispatch(position, nearest, false, scale);
    }
    impl_->ApplySignal(SignalKind::Disturbance, kNone, position);
    impl_->ProcessTransitions();
}

void WorldSimulation::Step() {
    if (!impl_ || phase_ != RaidPhase::Active) return;
    ++now_;
    ++stats_.steps;
    impl_->Step();
}

void WorldSimulation::AdvanceTo(Tick tick) {
    while (phase_ == RaidPhase::Active && now_ < tick) Step();
}

std::uint32_t WorldSimulation::Find(std::string_view id) const {
    for (std::size_t i = 0; i < cfg_.entities.size(); ++i) {
        if (cfg_.entities[i].id == id) return static_cast<std::uint32_t>(i);
    }
    return kNone;
}

std::uint32_t WorldSimulation::FindArea(std::string_view id) const {
    for (std::size_t i = 0; i < cfg_.areas.size(); ++i) {
        if (cfg_.areas[i].id == id) return static_cast<std::uint32_t>(i);
    }
    return kNone;
}

std::size_t WorldSimulation::EntityCount() const { return impl_ ? impl_->e.size() : 0; }

EntityView WorldSimulation::View(std::uint32_t i) const {
    EntityView v;
    if (!impl_ || i >= impl_->e.size()) return v;
    const auto& r = impl_->e[i];
    v.def = &cfg_.entities[i];
    v.state = r.eff;
    v.baseState = r.base;
    v.tier = r.tier;
    v.lastUpdate = r.lastUpdate;
    v.area = r.area;
    v.updates = r.updates;
    v.fuelSeconds = impl_->FuelAt(i);
    v.commsConnected = r.connected;
    v.pendingInstructions = r.pending.size();
    v.overridden = r.ovr;
    for (std::uint32_t k = 0; k < kEntityTimerCount; ++k) {
        if (r.timerPending[k]) v.activeTimers.push_back(k);
    }
    return v;
}

EntityState WorldSimulation::StateOf(std::uint32_t i) const { return impl_ && i < impl_->e.size() ? impl_->e[i].eff : EntityState::Nominal; }
Tier WorldSimulation::TierOf(std::uint32_t i) const { return impl_ && i < impl_->e.size() ? impl_->e[i].tier : Tier::Low; }
Tier WorldSimulation::AreaTier(std::uint32_t a) const { return impl_ && a < impl_->areaTier.size() ? impl_->areaTier[a] : Tier::Low; }

bool WorldSimulation::LastDirective(std::uint32_t g, PatrolDirective& out) const {
    if (!impl_ || g >= impl_->e.size() || !impl_->e[g].hasDirective) return false;
    out = impl_->e[g].last;
    return true;
}

std::vector<SimEvent> WorldSimulation::RecentEvents() const {
    std::vector<SimEvent> out;
    out.reserve(log_.size());
    for (std::size_t k = 0; k < log_.size(); ++k) out.push_back(log_[(logHead_ + k) % log_.size()]);
    return out;
}

std::size_t WorldSimulation::QueueSize() const { return impl_ ? impl_->updates.Size() + impl_->timers.Size() : 0; }

std::uint64_t WorldSimulation::StateHash() const {
    llworld::StateHasher h;
    h.Add(now_);
    h.Add(static_cast<std::uint64_t>(phase_));
    if (!impl_) return h.Value();
    for (std::uint32_t i = 0; i < impl_->e.size(); ++i) {
        const auto& r = impl_->e[i];
        h.Add(static_cast<std::uint64_t>(r.eff));
        h.Add(static_cast<std::uint64_t>(r.base));
        h.Add(r.area);
        h.Add(r.leg);
        h.Add(r.connected ? 1u : 0u);
        h.Add(r.pending.size());
        h.AddDouble(impl_->FuelAt(i));
    }
    return h.Value();
}

}  // namespace llsim
