// LAST LIGHT - World content loader implementation.
#include "llcontent/Content.h"

namespace llcontent {

bool LoadContentText(std::string_view text, const std::string& source, llloot::LootConfig& loot, llsim::SimConfig& sim,
                     std::vector<std::string>& errors) {
    std::vector<std::string> local;
    const llworld::IniDocument doc = llworld::ParseIni(text, local);
    bool versionSeen = false;
    for (const llworld::IniSection& s : doc.sections) {
        if (s.name == "content") {
            std::uint32_t version = 0;
            llworld::IniBinder b;
            b.U32("version", version);
            b.Apply(s, local);
            versionSeen = true;
            if (version != kContentVersion) {
                local.push_back("line " + std::to_string(s.line) + ": unsupported content version " + std::to_string(version) +
                                " (this build reads version " + std::to_string(kContentVersion) + ")");
            } else {
                loot.version = version;
                sim.version = version;
            }
        } else if (!llloot::IsLootSection(s.name) && !llsim::IsSimSection(s.name)) {
            local.push_back("line " + std::to_string(s.line) + ": unknown section [" + s.name + "]");
        }
    }
    if (!versionSeen) local.push_back("missing [content] version = " + std::to_string(kContentVersion));
    llloot::LoadLootSections(doc, loot, local);
    llsim::LoadSimSections(doc, sim, local);
    for (std::string& m : local) errors.push_back(source + ": " + m);
    return local.empty();
}

bool LoadContentFile(const std::string& path, llloot::LootConfig& loot, llsim::SimConfig& sim, std::vector<std::string>& errors) {
    std::string text;
    if (!llworld::ReadFile(path, text)) {
        errors.push_back(path + ": cannot read file");
        return false;
    }
    const std::size_t slash = path.find_last_of("/\\");
    return LoadContentText(text, slash == std::string::npos ? path : path.substr(slash + 1), loot, sim, errors);
}

bool LoadAndValidate(const std::vector<std::string>& paths, const llloot::IItemCatalog& catalog, llloot::LootConfig& loot, llsim::SimConfig& sim,
                     std::vector<std::string>& errors) {
    const std::size_t before = errors.size();
    for (const std::string& p : paths) LoadContentFile(p, loot, sim, errors);
    if (errors.size() != before) return false;
    for (const std::string& m : llloot::ValidateLoot(loot, catalog)) errors.push_back("loot: " + m);
    for (const std::string& m : llsim::ValidateSim(sim)) errors.push_back("simulation: " + m);
    return errors.size() == before;
}

}  // namespace llcontent
