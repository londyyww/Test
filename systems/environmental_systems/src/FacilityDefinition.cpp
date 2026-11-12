// LAST LIGHT - Environmental Interaction & Power Grid (ENV)
#include "llenv/FacilityDefinition.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

#include "llenv/Facility.h"
#include "llenv/Ports.h"

namespace llenv {

namespace {
constexpr const char* kNodeTypeNames[kNodeTypeCount] = {
    "Generator", "BackupGenerator", "Battery",   "Junction",  "Breaker",         "Door",          "SecurityGate", "Light",
    "EmergencyLight", "Machinery",  "Terminal", "Alarm",     "CommArray", "ExtractionPoint", "SecuritySensor",
};
}  // namespace

const char* ToString(NodeType t) {
    const auto i = static_cast<std::size_t>(t);
    return i < kNodeTypeCount ? kNodeTypeNames[i] : "Invalid";
}

bool ParseNodeType(std::string_view s, NodeType& out) {
    for (std::size_t i = 0; i < kNodeTypeCount; ++i) {
        if (s == kNodeTypeNames[i]) {
            out = static_cast<NodeType>(i);
            return true;
        }
    }
    return false;
}

const char* ToString(SoundKind k) {
    switch (k) {
        case SoundKind::GeneratorStart: return "GeneratorStart";
        case SoundKind::GeneratorRunning: return "GeneratorRunning";
        case SoundKind::GeneratorFailure: return "GeneratorFailure";
        case SoundKind::MachineryRunning: return "MachineryRunning";
        case SoundKind::DoorMotor: return "DoorMotor";
        case SoundKind::DoorForced: return "DoorForced";
        case SoundKind::BreakerTrip: return "BreakerTrip";
        case SoundKind::AlarmPulse: return "AlarmPulse";
        case SoundKind::ExtractionBeacon: return "ExtractionBeacon";
        case SoundKind::CommBurst: return "CommBurst";
    }
    return "Unknown";
}

const char* ToString(FacilityEventType t) {
    switch (t) {
        case FacilityEventType::PowerChanged: return "PowerChanged";
        case FacilityEventType::GeneratorState: return "GeneratorState";
        case FacilityEventType::BreakerState: return "BreakerState";
        case FacilityEventType::BreakerTripped: return "BreakerTripped";
        case FacilityEventType::NetworkOverloaded: return "NetworkOverloaded";
        case FacilityEventType::DoorState: return "DoorState";
        case FacilityEventType::NavBlockChanged: return "NavBlockChanged";
        case FacilityEventType::LightChanged: return "LightChanged";
        case FacilityEventType::AlarmTriggered: return "AlarmTriggered";
        case FacilityEventType::AlarmSilenced: return "AlarmSilenced";
        case FacilityEventType::AlertLevelChanged: return "AlertLevelChanged";
        case FacilityEventType::SecurityArmed: return "SecurityArmed";
        case FacilityEventType::Damaged: return "Damaged";
        case FacilityEventType::Destroyed: return "Destroyed";
        case FacilityEventType::Repaired: return "Repaired";
        case FacilityEventType::ObjectiveSignal: return "ObjectiveSignal";
        case FacilityEventType::InteractionCompleted: return "InteractionCompleted";
    }
    return "Unknown";
}

const char* ToString(InteractionAction a) {
    switch (a) {
        case InteractionAction::Activate: return "Activate";
        case InteractionAction::Deactivate: return "Deactivate";
        case InteractionAction::ResetBreaker: return "ResetBreaker";
        case InteractionAction::Crank: return "Crank";
        case InteractionAction::Force: return "Force";
        case InteractionAction::Repair: return "Repair";
        case InteractionAction::Refuel: return "Refuel";
        case InteractionAction::InstallFuse: return "InstallFuse";
        case InteractionAction::Hack: return "Hack";
        case InteractionAction::Command: return "Command";
        case InteractionAction::TriggerAlarm: return "TriggerAlarm";
        case InteractionAction::SilenceAlarm: return "SilenceAlarm";
        case InteractionAction::CallExtraction: return "CallExtraction";
        case InteractionAction::CancelChannel: return "CancelChannel";
    }
    return "Unknown";
}

const char* ToString(InteractionResult r) {
    constexpr const char* names[kInteractionResultCount] = {
        "Applied",           "ChannelStarted",      "ChannelJoined",       "RejectedNotAuthority",
        "RejectedUnknownPlayer", "RejectedDisconnected", "RejectedRateLimited", "RejectedDuplicate",
        "RejectedUnknownNode",   "RejectedOutOfRange", "RejectedPermission",  "RejectedStaleVersion",
        "RejectedUnsupported",   "RejectedNoPower",    "RejectedLocked",      "RejectedMissingItem",
        "RejectedDestroyed",     "RejectedBusy",       "RejectedInvalidState",
    };
    const auto i = static_cast<std::size_t>(r);
    return i < kInteractionResultCount ? names[i] : "Unknown";
}

const char* ToString(GeneratorState s) {
    switch (s) {
        case GeneratorState::Off: return "Off";
        case GeneratorState::Starting: return "Starting";
        case GeneratorState::Running: return "Running";
        case GeneratorState::Tripped: return "Tripped";
        case GeneratorState::Destroyed: return "Destroyed";
    }
    return "Unknown";
}

const char* ToString(DoorState s) {
    switch (s) {
        case DoorState::Closed: return "Closed";
        case DoorState::Opening: return "Opening";
        case DoorState::Open: return "Open";
        case DoorState::Closing: return "Closing";
    }
    return "Unknown";
}

int FacilityDef::Find(std::string_view id) const {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

std::vector<std::string> ValidateFacility(const FacilityDef& def) {
    std::vector<std::string> e;
    auto require = [&e](bool ok, const std::string& msg) {
        if (!ok) e.push_back(msg);
    };
    require(!def.facilityId.empty(), "facility.id must not be empty");
    require(def.ticksPerSecond > 0.0, "facility.ticks_per_second must be > 0");
    require(!def.nodes.empty(), "facility has no nodes");
    require(def.nodes.size() < 0xFFFFFFF0u, "too many nodes");

    for (std::size_t i = 0; i < def.nodes.size(); ++i) {
        const NodeDef& n = def.nodes[i];
        const std::string p = "node." + n.id + ": ";
        require(!n.id.empty(), "node with empty id");
        for (std::size_t j = 0; j < i; ++j) require(def.nodes[j].id != n.id, p + "duplicate id");
        require(!n.network.empty(), p + "network is required");
        require(n.position.IsFinite(), p + "position must be finite");
        require(n.health >= 0.0 && n.health <= 1.0, p + "health must be in [0,1]");
        require(n.damagedThreshold > 0.0 && n.damagedThreshold < 1.0, p + "damaged_threshold must be in (0,1)");
        require(n.damagedOutputFactor >= 0.0 && n.damagedOutputFactor <= 1.0, p + "damaged_output_factor must be in [0,1]");
        require(n.priority >= 0 && n.priority <= 100, p + "priority must be in [0,100]");
        require(n.demandKw >= 0.0, p + "demand_kw must be >= 0");
        require(n.interactRangeCm > 0.0, p + "interact_range_cm must be > 0");
        require(n.repairTicks >= 1, p + "repair_ticks must be >= 1");

        if (IsSource(n.type)) {
            require(n.fedBy.empty(), p + "power sources cannot be fed by other nodes");
            require(n.capacityKw > 0.0, p + "sources need capacity_kw > 0");
            require(n.demandKw == 0.0, p + "sources cannot have demand_kw");
        }
        if (n.type == NodeType::Generator || n.type == NodeType::BackupGenerator) {
            require(n.startupTicks >= 1, p + "startup_ticks must be >= 1");
            require(n.overloadTolerance >= 1.0, p + "overload_tolerance must be >= 1");
            require(n.overloadTripTicks >= 1, p + "overload_trip_ticks must be >= 1");
            require(n.startFailChanceDamaged >= 0.0 && n.startFailChanceDamaged <= 1.0, p + "start_fail_chance_damaged must be in [0,1]");
            require(n.fuelTicks < 0.0 || n.fuelTicks >= 0.0, p + "fuel_ticks invalid");
            require(n.noisePulseTicks >= 1, p + "noise_pulse_ticks must be >= 1");
        }
        if (n.type == NodeType::Generator) require(!n.autoStart, p + "auto_start is only valid on BackupGenerator");
        if (n.type == NodeType::Battery) {
            require(n.batteryCapacityKwTicks > 0.0, p + "battery_capacity_kw_ticks must be > 0");
            require(n.batteryInitialFraction >= 0.0 && n.batteryInitialFraction <= 1.0, p + "battery_initial_fraction must be in [0,1]");
            require(n.batteryChargeKw >= 0.0, p + "battery_charge_kw must be >= 0");
        }
        if (n.type == NodeType::Breaker) {
            require(n.capacityKw > 0.0, p + "breakers need capacity_kw (rated load) > 0");
            require(n.tripDelayTicks >= 1, p + "trip_delay_ticks must be >= 1");
            require(n.fuseInstalled || !n.fuseItem.empty(), p + "a breaker without a fuse needs fuse_item");
        }
        if (IsDistribution(n.type) || IsConsumer(n.type)) {
            require(!n.fedBy.empty(), p + "must be fed by at least one node");
        }
        if (IsDoorLike(n.type)) {
            require(n.motorTicks >= 1, p + "motor_ticks must be >= 1");
            require(n.manualWork > 0.0, p + "manual_work must be > 0");
            require(n.manualParticipants >= 1 && n.manualParticipants <= 4, p + "manual_participants must be 1..4");
        }
        if (n.type == NodeType::ExtractionPoint) {
            require(n.callTicks >= 1, p + "call_ticks must be >= 1");
            require(n.manualParticipants >= 1 && n.manualParticipants <= 4, p + "manual_participants must be 1..4");
        }
        if (n.type == NodeType::Alarm) require(n.alarmIntervalTicks >= 1, p + "alarm_interval_ticks must be >= 1");
        if (n.type == NodeType::EmergencyLight) require(n.emergencyBatteryTicks >= 1, p + "emergency_battery_ticks must be >= 1");
        if (n.type == NodeType::Light || n.type == NodeType::EmergencyLight) {
            require(n.lightRadiusCm > 0.0 && n.lightIntensity >= 0.0, p + "light radius/intensity invalid");
        }
        if (n.type == NodeType::SecuritySensor) require(n.sensorRadiusCm > 0.0, p + "sensor_radius_cm must be > 0");
        if (n.autoStart) require(n.type == NodeType::BackupGenerator, p + "auto_start requires BackupGenerator");

        for (const auto& f : n.fedBy) {
            const int k = def.Find(f);
            if (k < 0) {
                e.push_back(p + "fed_by references unknown node '" + f + "'");
                continue;
            }
            const NodeDef& up = def.nodes[static_cast<std::size_t>(k)];
            require(up.network == n.network, p + "fed_by '" + f + "' is in another network (" + up.network + ")");
            require(!IsConsumer(up.type), p + "fed_by '" + f + "' is a consumer; consumers cannot feed power");
            require(f != n.id, p + "cannot feed itself");
        }
        if (!n.alarm.empty()) {
            const int k = def.Find(n.alarm);
            require(k >= 0 && def.nodes[static_cast<std::size_t>(k)].type == NodeType::Alarm, p + "alarm '" + n.alarm + "' is not an Alarm node");
        }
        for (const auto& c : n.controls) {
            require(n.type == NodeType::Terminal, p + "only terminals can have controls");
            require(def.Find(c) >= 0, p + "controls unknown node '" + c + "'");
        }
    }

    // Cycle detection on the fed_by graph (power must flow one way).
    const std::size_t n = def.nodes.size();
    std::vector<int> color(n, 0);
    std::function<bool(std::size_t)> visit = [&](std::size_t i) {
        color[i] = 1;
        for (const auto& f : def.nodes[i].fedBy) {
            const int k = def.Find(f);
            if (k < 0) continue;
            const auto u = static_cast<std::size_t>(k);
            if (color[u] == 1) return true;
            if (color[u] == 0 && visit(u)) return true;
        }
        color[i] = 2;
        return false;
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (color[i] == 0 && visit(i)) {
            e.push_back("power graph contains a cycle through node." + def.nodes[i].id);
            break;
        }
    }
    return e;
}

// ---------------------------------------------------------------------------
// INI loader
// ---------------------------------------------------------------------------
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool ParseDouble(std::string_view s, double& out) {
    const std::string t(Trim(s));
    if (t.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(t.c_str(), &end);
    if (errno != 0 || end != t.c_str() + t.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool ParseU64(std::string_view s, std::uint64_t& out) {
    const std::string t(Trim(s));
    if (t.empty() || t[0] == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 0);
    if (errno != 0 || end != t.c_str() + t.size()) return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

bool ParseBool(std::string_view s, bool& out) {
    const auto t = Trim(s);
    if (t == "true" || t == "1" || t == "yes") return (out = true, true);
    if (t == "false" || t == "0" || t == "no") return (out = false, true);
    return false;
}

std::vector<std::string> SplitList(std::string_view s) {
    std::vector<std::string> out;
    while (true) {
        const std::size_t c = s.find(',');
        const auto part = Trim(c == std::string_view::npos ? s : s.substr(0, c));
        if (!part.empty()) out.emplace_back(part);
        if (c == std::string_view::npos) break;
        s.remove_prefix(c + 1);
    }
    return out;
}

using Setter = std::function<bool(std::string_view)>;
using Bindings = std::vector<std::pair<std::string, Setter>>;

void Num(Bindings& b, const char* k, double& f) { b.emplace_back(k, [&f](std::string_view v) { return ParseDouble(v, f); }); }
void Flag(Bindings& b, const char* k, bool& f) { b.emplace_back(k, [&f](std::string_view v) { return ParseBool(v, f); }); }
void Str(Bindings& b, const char* k, std::string& f) {
    b.emplace_back(k, [&f](std::string_view v) {
        f = std::string(Trim(v));
        return true;
    });
}
template <class U>
void Uint(Bindings& b, const char* k, U& f) {
    b.emplace_back(k, [&f](std::string_view v) {
        std::uint64_t t = 0;
        if (!ParseU64(v, t) || t > static_cast<std::uint64_t>(static_cast<U>(~U{0}))) return false;
        f = static_cast<U>(t);
        return true;
    });
}

void BindNode(Bindings& b, NodeDef& n) {
    b.emplace_back("type", [&n](std::string_view v) { return ParseNodeType(Trim(v), n.type); });
    Str(b, "network", n.network);
    b.emplace_back("fed_by", [&n](std::string_view v) {
        n.fedBy = SplitList(v);
        return true;
    });
    b.emplace_back("position", [&n](std::string_view v) {
        const auto parts = SplitList(v);
        if (parts.size() != 3) return false;
        return ParseDouble(parts[0], n.position.x) && ParseDouble(parts[1], n.position.y) && ParseDouble(parts[2], n.position.z);
    });
    Num(b, "capacity_kw", n.capacityKw);
    Num(b, "demand_kw", n.demandKw);
    b.emplace_back("priority", [&n](std::string_view v) {
        double d = 0.0;
        if (!ParseDouble(v, d) || d != std::floor(d)) return false;
        n.priority = static_cast<int>(d);
        return true;
    });
    Num(b, "health", n.health);
    Num(b, "damaged_threshold", n.damagedThreshold);
    Num(b, "damaged_output_factor", n.damagedOutputFactor);
    Flag(b, "repairable", n.repairable);
    Uint(b, "repair_ticks", n.repairTicks);
    Str(b, "repair_item", n.repairItem);
    Flag(b, "starts_running", n.startsRunning);
    Uint(b, "startup_ticks", n.startupTicks);
    Num(b, "fuel_ticks", n.fuelTicks);
    Str(b, "fuel_item", n.fuelItem);
    Num(b, "refuel_amount_ticks", n.refuelAmountTicks);
    Num(b, "overload_tolerance", n.overloadTolerance);
    Uint(b, "overload_trip_ticks", n.overloadTripTicks);
    Flag(b, "auto_start", n.autoStart);
    Uint(b, "auto_start_delay_ticks", n.autoStartDelayTicks);
    Flag(b, "auto_stop_on_restore", n.autoStopOnRestore);
    Num(b, "start_fail_chance_damaged", n.startFailChanceDamaged);
    Num(b, "noise_db", n.noiseDb);
    Uint(b, "noise_pulse_ticks", n.noisePulseTicks);
    Num(b, "battery_capacity_kw_ticks", n.batteryCapacityKwTicks);
    Num(b, "battery_initial_fraction", n.batteryInitialFraction);
    Num(b, "battery_charge_kw", n.batteryChargeKw);
    Flag(b, "starts_closed", n.startsClosed);
    Uint(b, "trip_delay_ticks", n.tripDelayTicks);
    Flag(b, "fuse_installed", n.fuseInstalled);
    Str(b, "fuse_item", n.fuseItem);
    Flag(b, "starts_open", n.startsOpen);
    Flag(b, "starts_locked", n.startsLocked);
    b.emplace_back("fail_mode", [&n](std::string_view v) {
        const auto t = Trim(v);
        if (t == "FailSecure") n.failMode = LockFailMode::FailSecure;
        else if (t == "FailSafe") n.failMode = LockFailMode::FailSafe;
        else return false;
        return true;
    });
    Uint(b, "motor_ticks", n.motorTicks);
    Num(b, "manual_work", n.manualWork);
    Uint(b, "manual_participants", n.manualParticipants);
    Str(b, "key_item", n.keyItem);
    Str(b, "force_item", n.forceItem);
    Str(b, "alarm", n.alarm);
    Flag(b, "starts_on", n.startsOn);
    Num(b, "light_radius_cm", n.lightRadiusCm);
    Num(b, "light_intensity", n.lightIntensity);
    Uint(b, "emergency_battery_ticks", n.emergencyBatteryTicks);
    Num(b, "emergency_intensity", n.emergencyIntensity);
    b.emplace_back("controls", [&n](std::string_view v) {
        n.controls = SplitList(v);
        return true;
    });
    Uint(b, "hack_ticks", n.hackTicks);
    Uint(b, "alarm_interval_ticks", n.alarmIntervalTicks);
    Uint(b, "alarm_duration_ticks", n.alarmDurationTicks);
    Flag(b, "trigger_on_power_restore", n.triggerOnPowerRestore);
    Num(b, "sensor_radius_cm", n.sensorRadiusCm);
    Flag(b, "rearm_on_power_restore", n.rearmOnPowerRestore);
    Uint(b, "call_ticks", n.callTicks);
    Str(b, "objective_signal", n.objectiveSignal);
    Num(b, "interact_range_cm", n.interactRangeCm);
    Uint(b, "required_permissions", n.requiredPermissions);
    Flag(b, "blocks_nav_when_running", n.blocksNavWhenRunning);
    b.emplace_back("persistence", [&n](std::string_view v) {
        const auto t = Trim(v);
        if (t == "Mission") n.persistence = PersistencePolicy::Mission;
        else if (t == "World") n.persistence = PersistencePolicy::World;
        else if (t == "WorldOnExtraction") n.persistence = PersistencePolicy::WorldOnExtraction;
        else return false;
        return true;
    });
}

}  // namespace

LoadResult LoadFacilityIni(std::string_view text, FacilityDef& out) {
    LoadResult r;
    auto fail = [&r](std::size_t line, const std::string& m) {
        r.ok = false;
        r.errors.push_back("line " + std::to_string(line) + ": " + m);
    };
    Bindings bindings;
    bool valid = false;
    bool sawType = false;
    std::string section;
    std::size_t lineNo = 0;
    std::size_t sectionLine = 0;
    auto closeSection = [&]() {
        if (valid && section.rfind("node.", 0) == 0 && !sawType) fail(sectionLine, "[" + section + "] needs a type");
    };

    while (!text.empty()) {
        ++lineNo;
        const std::size_t nl = text.find('\n');
        std::string_view line = nl == std::string_view::npos ? text : text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const std::size_t c = line.find_first_of(";#");
        if (c != std::string_view::npos) line = line.substr(0, c);
        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            closeSection();
            bindings.clear();
            valid = false;
            sawType = false;
            sectionLine = lineNo;
            if (line.back() != ']') {
                fail(lineNo, "malformed section header");
                continue;
            }
            section = std::string(Trim(line.substr(1, line.size() - 2)));
            if (section == "facility") {
                Str(bindings, "id", out.facilityId);
                Num(bindings, "ticks_per_second", out.ticksPerSecond);
                Uint(bindings, "seed", out.seed);
                valid = true;
            } else if (section.rfind("node.", 0) == 0 && section.size() > 5) {
                const std::string id = section.substr(5);
                if (out.Find(id) >= 0) {
                    fail(lineNo, "duplicate node '" + id + "'");
                    continue;
                }
                NodeDef n;
                n.id = id;
                out.nodes.push_back(n);
                // Bindings point into the vector; no push_back happens until the next section.
                BindNode(bindings, out.nodes.back());
                valid = true;
            } else {
                fail(lineNo, "unknown section '" + section + "'");
            }
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            fail(lineNo, "expected key = value");
            continue;
        }
        if (!valid) {
            fail(lineNo, "key outside a valid section");
            continue;
        }
        const auto key = Trim(line.substr(0, eq));
        const auto value = Trim(line.substr(eq + 1));
        if (key == "type") sawType = true;
        bool found = false;
        for (const auto& [k, setter] : bindings) {
            if (k != key) continue;
            found = true;
            if (!setter(value)) fail(lineNo, "invalid value '" + std::string(value) + "' for '" + std::string(key) + "'");
            break;
        }
        if (!found) fail(lineNo, "unknown key '" + std::string(key) + "' in [" + section + "]");
    }
    closeSection();
    for (const auto& m : ValidateFacility(out)) fail(0, "validation: " + m);
    return r;
}

LoadResult LoadFacilityFile(const std::string& path, FacilityDef& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LoadResult r;
        r.ok = false;
        r.errors.push_back("cannot open '" + path + "'");
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return LoadFacilityIni(ss.str(), out);
}

}  // namespace llenv
