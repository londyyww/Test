// LAST LIGHT - Environmental & World Intelligence (WI)
// Minimal strict INI reader shared by all WI configuration loaders.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llworld/WorldTypes.h"

namespace llworld {

struct IniEntry {
    std::string key;
    std::string value;
    std::size_t line = 0;
};
struct IniSection {
    std::string name;  // e.g. "hazard.GAS_A"
    std::size_t line = 0;
    std::vector<IniEntry> entries;
};
struct IniDocument {
    std::vector<IniSection> sections;
};

// Syntax errors are appended to `errors`; parsing continues.
IniDocument ParseIni(std::string_view text, std::vector<std::string>& errors);
bool ReadFile(const std::string& path, std::string& out);

// Binds keys of one section to fields. Unknown keys and bad values are errors.
class IniBinder {
public:
    using Setter = std::function<bool(std::string_view)>;
    void Num(const char* key, double& f);
    void Flag(const char* key, bool& f);
    void Str(const char* key, std::string& f);
    void List(const char* key, std::vector<std::string>& f);
    void Point(const char* key, Vec3& f);
    void Points(const char* key, std::vector<Vec3>& f);  // "x,y,z | x,y,z" (";" starts a comment)
    void Ticks(const char* key, Tick& f);
    void U32(const char* key, std::uint32_t& f);
    void Custom(std::string key, Setter s) { entries_.emplace_back(std::move(key), std::move(s)); }
    void Apply(const IniSection& section, std::vector<std::string>& errors) const;

private:
    std::vector<std::pair<std::string, Setter>> entries_;
};

std::string_view TrimView(std::string_view s);
bool ParseNumber(std::string_view s, double& out);

}  // namespace llworld
