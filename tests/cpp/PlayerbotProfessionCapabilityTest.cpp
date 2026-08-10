/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <optional>
#include <vector>

#include "Bot/Economy/PlayerbotProfessionCapability.h"
#include "ItemTemplate.h"
#include "SharedDefines.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
ProfessionCapability Capability(uint32 outputItemId, uint32 recipeSpellId, uint16 skillId,
                                ProfessionCapabilityKind kind, bool primaryProfession = true)
{
    return {
        .outputItemId = outputItemId,
        .recipeSpellId = recipeSpellId,
        .professionSkillId = skillId,
        .kind = kind,
        .primaryProfession = primaryProfession,
    };
}
}  // namespace

TEST(PlayerbotProfessionCapabilityTest, ProfessionKindUsesTheRelevantPersonalityAffinity)
{
    for (uint16 skillId : {SKILL_MINING, SKILL_HERBALISM, SKILL_SKINNING})
    {
        EXPECT_EQ(PlayerbotProfessionCapabilityCatalog::ClassifySkill(skillId), ProfessionCapabilityKind::Gathering);
    }

    for (uint16 skillId : {SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_INSCRIPTION,
                           SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_TAILORING})
    {
        EXPECT_EQ(PlayerbotProfessionCapabilityCatalog::ClassifySkill(skillId), ProfessionCapabilityKind::Crafting);
    }

    EXPECT_FALSE(PlayerbotProfessionCapabilityCatalog::ClassifySkill(SKILL_SWORDS));
}

TEST(PlayerbotProfessionCapabilityTest, GatheringItemsMapToAuthoritativeSkillsWithoutARecipeSpell)
{
    std::optional<ProfessionCapability> const herb =
        PlayerbotProfessionCapabilityCatalog::DescribeGatheringItem(100u, ITEM_CLASS_TRADE_GOODS, ITEM_SUBCLASS_HERB);
    ASSERT_TRUE(herb);
    EXPECT_EQ(herb->outputItemId, 100u);
    EXPECT_EQ(herb->recipeSpellId, 0u);
    EXPECT_EQ(herb->professionSkillId, SKILL_HERBALISM);
    EXPECT_EQ(herb->kind, ProfessionCapabilityKind::Gathering);
    EXPECT_TRUE(herb->primaryProfession);

    EXPECT_EQ(PlayerbotProfessionCapabilityCatalog::DescribeGatheringItem(101u, ITEM_CLASS_TRADE_GOODS,
                                                                          ITEM_SUBCLASS_METAL_STONE)
                  ->professionSkillId,
              SKILL_MINING);
    EXPECT_EQ(
        PlayerbotProfessionCapabilityCatalog::DescribeGatheringItem(102u, ITEM_CLASS_TRADE_GOODS, ITEM_SUBCLASS_LEATHER)
            ->professionSkillId,
        SKILL_SKINNING);
    EXPECT_FALSE(
        PlayerbotProfessionCapabilityCatalog::DescribeGatheringItem(103u, ITEM_CLASS_ARMOR, ITEM_SUBCLASS_LEATHER));
}

TEST(PlayerbotProfessionCapabilityTest, SelectionPrefersKnownRecipesAndIsStableAcrossInputOrder)
{
    ProfessionCapability const first = Capability(200u, 1002u, SKILL_BLACKSMITHING, ProfessionCapabilityKind::Crafting);
    ProfessionCapability const preferred = Capability(201u, 1001u, SKILL_TAILORING, ProfessionCapabilityKind::Crafting);
    std::vector<ProfessionCapability> candidates = {first, preferred, preferred};

    std::optional<ProfessionCapability> const selected =
        PlayerbotProfessionCapabilityCatalog::Select(candidates, {preferred.recipeSpellId}, true);
    ASSERT_TRUE(selected);
    EXPECT_EQ(*selected, preferred);

    std::reverse(candidates.begin(), candidates.end());
    EXPECT_EQ(PlayerbotProfessionCapabilityCatalog::Select(candidates, {preferred.recipeSpellId}, true), selected);
}

TEST(PlayerbotProfessionCapabilityTest, SelectionUsesTheStableLowestTupleAcrossProfessions)
{
    ProfessionCapability const later = Capability(200u, 1001u, SKILL_TAILORING, ProfessionCapabilityKind::Crafting);
    ProfessionCapability const earlier =
        Capability(300u, 1002u, SKILL_BLACKSMITHING, ProfessionCapabilityKind::Crafting);

    EXPECT_EQ(PlayerbotProfessionCapabilityCatalog::Select({later, earlier}, {}, true), earlier);
    EXPECT_EQ(PlayerbotProfessionCapabilityCatalog::Select({earlier, later}, {}, true), earlier);
}

TEST(PlayerbotProfessionCapabilityTest, SelectionFailsClosedForMalformedOrUnsupportedCapabilities)
{
    ProfessionCapability const blacksmithing =
        Capability(200u, 1001u, SKILL_BLACKSMITHING, ProfessionCapabilityKind::Crafting);

    ProfessionCapability malformed = blacksmithing;
    malformed.outputItemId = 0u;
    EXPECT_FALSE(PlayerbotProfessionCapabilityCatalog::Select({malformed}, {}, true));

    ProfessionCapability wrongKind = blacksmithing;
    wrongKind.kind = ProfessionCapabilityKind::Gathering;
    EXPECT_FALSE(PlayerbotProfessionCapabilityCatalog::Select({wrongKind}, {}, true));

    ProfessionCapability unsupported = blacksmithing;
    unsupported.professionSkillId = SKILL_SWORDS;
    EXPECT_FALSE(PlayerbotProfessionCapabilityCatalog::Select({unsupported}, {}, true));
}

TEST(PlayerbotProfessionCapabilityTest, PrimarySelectionNeverConsumesASlotForASecondaryProfession)
{
    ProfessionCapability const cooking =
        Capability(200u, 1001u, SKILL_COOKING, ProfessionCapabilityKind::Crafting, false);

    EXPECT_FALSE(PlayerbotProfessionCapabilityCatalog::Select({cooking}, {}, true));
    EXPECT_EQ(PlayerbotProfessionCapabilityCatalog::Select({cooking}, {}, false), cooking);
}
