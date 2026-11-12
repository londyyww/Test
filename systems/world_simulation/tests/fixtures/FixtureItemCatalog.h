// LAST LIGHT - TEST / EXAMPLE FIXTURE ONLY.
//
// A stand-in for the authoritative item catalog so the portable core can be
// tested without the inventory service. It is NOT an item database: it holds
// only what IItemCatalog exposes (category, stack limit, footprint) for the IDs
// used by the example content. Production builds bind IItemCatalog to the real
// catalog (see docs/UNREAL_INTEGRATION_CONTRACT.md).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "llloot/Loot.h"
#include "llworld/Ini.h"

namespace llfixture {

class FixtureItemCatalog final : public llloot::IItemCatalog {
public:
    void Add(std::string id, std::string category, std::uint32_t maxStack, std::uint32_t slotSize) {
        llloot::ItemDescriptor d;
        d.categoryId = std::move(category);
        d.maxStack = maxStack;
        d.slotSize = slotSize;
        items_.emplace_back(std::move(id), std::move(d));
    }
    bool Describe(std::string_view id, llloot::ItemDescriptor& out) const override {
        for (const auto& [k, d] : items_) {
            if (k == id) {
                out = d;
                return true;
            }
        }
        return false;
    }
    // [fixture_item.<id>] category / max_stack / slot_size
    bool Load(const std::string& path, std::vector<std::string>& errors) {
        std::string text;
        if (!llworld::ReadFile(path, text)) {
            errors.push_back(path + ": cannot read file");
            return false;
        }
        const std::size_t before = errors.size();
        const llworld::IniDocument doc = llworld::ParseIni(text, errors);
        for (const auto& s : doc.sections) {
            if (s.name.rfind("fixture_item.", 0) != 0) {
                errors.push_back("fixture catalog: unexpected section [" + s.name + "]");
                continue;
            }
            std::string category;
            std::uint32_t maxStack = 1;
            std::uint32_t slotSize = 1;
            llworld::IniBinder b;
            b.Str("category", category);
            b.U32("max_stack", maxStack);
            b.U32("slot_size", slotSize);
            b.Apply(s, errors);
            Add(s.name.substr(13), category, maxStack, slotSize);
        }
        return errors.size() == before;
    }
    std::size_t Size() const { return items_.size(); }

private:
    std::vector<std::pair<std::string, llloot::ItemDescriptor>> items_;
};

}  // namespace llfixture
