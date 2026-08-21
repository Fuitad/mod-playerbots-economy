/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTPROFESSIONCAPABILITY_H
#define PLAYERBOTS_PLAYERBOTPROFESSIONCAPABILITY_H

#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "Define.h"

namespace PlayerbotEconomy
{
enum class ProfessionCapabilityKind : uint8
{
    Crafting,
    Gathering
};

struct ProfessionCapability
{
    uint32 outputItemId = 0;
    uint32 recipeSpellId = 0;
    uint16 professionSkillId = 0;
    ProfessionCapabilityKind kind = ProfessionCapabilityKind::Crafting;
    bool primaryProfession = false;

    bool operator==(ProfessionCapability const&) const = default;
};

// One recipe that consumes an item: the profession that owns it, the skill needed to learn it, what it makes.
struct ReagentUse
{
    uint16 skillId = 0;
    uint32 minSkillRank = 0;
    uint32 outputItemId = 0;
};

struct PlannedProfessionRank
{
    uint16 skillId = 0;
    uint32 rankCap = 0;
};

using ReagentUsesFn = std::function<std::vector<ReagentUse> const&(uint32 itemId)>;

class PlayerbotProfessionCapabilityCatalog
{
public:
    [[nodiscard]] static std::vector<ProfessionCapability> const& All();
    // Every recipe of every profession skill line that consumes itemId, from SkillLineAbility and spell
    // reagents. Built once.
    [[nodiscard]] static std::vector<ReagentUse> const& ReagentUses(uint32 itemId);
    // True when a planned crafting profession has a recipe at or below its rank cap consuming itemId, either
    // directly or through one recipe of a planned gathering skill line (smelting) whose output it consumes.
    [[nodiscard]] static bool CraftingUsesItem(uint32 itemId, std::span<PlannedProfessionRank const> crafting,
                                               std::span<PlannedProfessionRank const> gathering,
                                               ReagentUsesFn const& lookup);
    [[nodiscard]] static std::optional<ProfessionCapabilityKind> ClassifySkill(uint16 skillId);
    [[nodiscard]] static std::optional<ProfessionCapability> DescribeGatheringItem(uint32 itemId, uint32 itemClass,
                                                                                   uint32 itemSubclass);
    [[nodiscard]] static std::optional<ProfessionCapability> Select(std::vector<ProfessionCapability> candidates,
                                                                    std::vector<uint32> const& knownRecipeSpellIds,
                                                                    bool primaryProfessionOnly);
};
}  // namespace PlayerbotEconomy

#endif
