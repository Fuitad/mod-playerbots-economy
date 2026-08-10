/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTPROFESSIONCAPABILITY_H
#define PLAYERBOTS_PLAYERBOTPROFESSIONCAPABILITY_H

#include <optional>
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

class PlayerbotProfessionCapabilityCatalog
{
public:
    [[nodiscard]] static std::vector<ProfessionCapability> const& All();
    [[nodiscard]] static std::optional<ProfessionCapabilityKind> ClassifySkill(uint16 skillId);
    [[nodiscard]] static std::optional<ProfessionCapability> DescribeGatheringItem(uint32 itemId, uint32 itemClass,
                                                                                   uint32 itemSubclass);
    [[nodiscard]] static std::optional<ProfessionCapability> Select(std::vector<ProfessionCapability> candidates,
                                                                    std::vector<uint32> const& knownRecipeSpellIds,
                                                                    bool primaryProfessionOnly);
};
}  // namespace PlayerbotEconomy

#endif
