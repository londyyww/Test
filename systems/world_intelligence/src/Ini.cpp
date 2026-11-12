// LAST LIGHT - Environmental & World Intelligence (WI)
#include "llworld/Ini.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "llworld/WorldEvents.h"

namespace llworld {

// ---------------------------------------------------------------- events

const char* ToString(WorldEventType t) {
    constexpr const char* names[kWorldEventTypeCount] = {
        "WeatherChanged", "WindGust",      "Thunder",          "HazardActivated", "HazardDeactivated", "HazardEscalated",
        "FireSpread",     "GasIgnition",   "StructuralCollapse", "RouteBlocked",  "RouteCleared",      "PlayerExposure",
        "AlarmRaised",    "PowerLost",     "PowerRestored",    "EmergencyLighting", "CommsLost",       "CommsRestored",
        "AssignmentChanged", "Gunfire",    "Explosion",
    };
    const auto i = static_cast<std::size_t>(t);
    return i < kWorldEventTypeCount ? names[i] : "Unknown";
}

void WorldEventBus::BeginTick(Tick now) {
    if (now == tick_) return;
    tick_ = now;
    previous_.swap(current_);
    current_.clear();
}

std::uint64_t WorldEventBus::Publish(WorldEvent e) {
    if (current_.size() >= cap_ || !e.position.IsFinite() || !std::isfinite(e.magnitude) || !std::isfinite(e.radiusCm) ||
        static_cast<std::size_t>(e.type) >= kWorldEventTypeCount) {
        ++dropped_;
        return 0;
    }
    e.id = nextId_++;
    e.tick = tick_;
    current_.push_back(e);
    ++published_;
    return e.id;
}

// ---------------------------------------------------------------- INI

std::string_view TrimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool ParseNumber(std::string_view s, double& out) {
    const std::string t(TrimView(s));
    if (t.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(t.c_str(), &end);
    if (errno != 0 || end != t.c_str() + t.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

namespace {

bool ParseU64(std::string_view s, std::uint64_t& out) {
    const std::string t(TrimView(s));
    if (t.empty() || t[0] == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 10);
    if (errno != 0 || end != t.c_str() + t.size()) return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    while (true) {
        const std::size_t c = s.find(sep);
        out.push_back(TrimView(c == std::string_view::npos ? s : s.substr(0, c)));
        if (c == std::string_view::npos) break;
        s.remove_prefix(c + 1);
    }
    return out;
}

bool ParsePoint(std::string_view s, Vec3& out) {
    const auto p = Split(s, ',');
    return p.size() == 3 && ParseNumber(p[0], out.x) && ParseNumber(p[1], out.y) && ParseNumber(p[2], out.z);
}

}  // namespace

IniDocument ParseIni(std::string_view text, std::vector<std::string>& errors) {
    IniDocument doc;
    std::size_t lineNo = 0;
    while (!text.empty()) {
        ++lineNo;
        const std::size_t nl = text.find('\n');
        std::string_view line = nl == std::string_view::npos ? text : text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const std::size_t c = line.find_first_of(";#");
        if (c != std::string_view::npos) line = line.substr(0, c);
        line = TrimView(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 3) {
                errors.push_back("line " + std::to_string(lineNo) + ": malformed section header");
                continue;
            }
            IniSection s;
            s.name = std::string(TrimView(line.substr(1, line.size() - 2)));
            s.line = lineNo;
            doc.sections.push_back(std::move(s));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            errors.push_back("line " + std::to_string(lineNo) + ": expected key = value");
            continue;
        }
        if (doc.sections.empty()) {
            errors.push_back("line " + std::to_string(lineNo) + ": key outside a section");
            continue;
        }
        doc.sections.back().entries.push_back(
            IniEntry{std::string(TrimView(line.substr(0, eq))), std::string(TrimView(line.substr(eq + 1))), lineNo});
    }
    return doc;
}

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void IniBinder::Num(const char* key, double& f) {
    Custom(key, [&f](std::string_view v) { return ParseNumber(v, f); });
}
void IniBinder::Flag(const char* key, bool& f) {
    Custom(key, [&f](std::string_view v) {
        if (v == "true" || v == "1" || v == "yes") return (f = true, true);
        if (v == "false" || v == "0" || v == "no") return (f = false, true);
        return false;
    });
}
void IniBinder::Str(const char* key, std::string& f) {
    Custom(key, [&f](std::string_view v) {
        f = std::string(v);
        return true;
    });
}
void IniBinder::List(const char* key, std::vector<std::string>& f) {
    Custom(key, [&f](std::string_view v) {
        f.clear();
        for (auto p : Split(v, ',')) {
            if (!p.empty()) f.emplace_back(p);
        }
        return true;
    });
}
void IniBinder::Point(const char* key, Vec3& f) {
    Custom(key, [&f](std::string_view v) { return ParsePoint(v, f); });
}
void IniBinder::Points(const char* key, std::vector<Vec3>& f) {
    Custom(key, [&f](std::string_view v) {
        std::vector<Vec3> tmp;
        for (auto p : Split(v, '|')) {
            if (p.empty()) continue;
            Vec3 q;
            if (!ParsePoint(p, q)) return false;
            tmp.push_back(q);
        }
        f = tmp;
        return true;
    });
}
void IniBinder::Ticks(const char* key, Tick& f) {
    Custom(key, [&f](std::string_view v) {
        std::uint64_t t = 0;
        if (!ParseU64(v, t)) return false;
        f = t;
        return true;
    });
}
void IniBinder::U32(const char* key, std::uint32_t& f) {
    Custom(key, [&f](std::string_view v) {
        std::uint64_t t = 0;
        if (!ParseU64(v, t) || t > 0xFFFFFFFFull) return false;
        f = static_cast<std::uint32_t>(t);
        return true;
    });
}

void IniBinder::Apply(const IniSection& section, std::vector<std::string>& errors) const {
    for (const IniEntry& e : section.entries) {
        bool found = false;
        for (const auto& [k, setter] : entries_) {
            if (k != e.key) continue;
            found = true;
            if (!setter(e.value)) {
                errors.push_back("line " + std::to_string(e.line) + ": invalid value '" + e.value + "' for '" + e.key + "' in [" +
                                 section.name + "]");
            }
            break;
        }
        if (!found) errors.push_back("line " + std::to_string(e.line) + ": unknown key '" + e.key + "' in [" + section.name + "]");
    }
}

}  // namespace llworld
