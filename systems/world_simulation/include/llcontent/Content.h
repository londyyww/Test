// LAST LIGHT - World content loader.
//
// One configuration convention for both subsystems: the llworld INI format
// already used by the power grid and world-intelligence data. Every file must
// declare its schema version; sections no subsystem claims are rejected.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "llloot/Loot.h"
#include "llsim/WorldSim.h"

namespace llcontent {

inline constexpr std::uint32_t kContentVersion = 1;

// Parses one file's text and appends its definitions. `source` prefixes messages.
bool LoadContentText(std::string_view text, const std::string& source, llloot::LootConfig& loot, llsim::SimConfig& sim,
                     std::vector<std::string>& errors);
bool LoadContentFile(const std::string& path, llloot::LootConfig& loot, llsim::SimConfig& sim, std::vector<std::string>& errors);

// Loads several files then runs both validators; returns true when clean.
bool LoadAndValidate(const std::vector<std::string>& paths, const llloot::IItemCatalog& catalog, llloot::LootConfig& loot, llsim::SimConfig& sim,
                     std::vector<std::string>& errors);

}  // namespace llcontent
