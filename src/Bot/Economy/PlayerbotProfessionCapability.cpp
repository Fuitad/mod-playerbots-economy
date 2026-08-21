/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotProfessionCapability.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "PlayerbotEconomyPolicy.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

using namespace PlayerbotEconomy;

namespace
{
bool CapabilityLess(ProfessionCapability const& left, ProfessionCapability const& right)
{
    if (left.professionSkillId != right.professionSkillId)
        return left.professionSkillId < right.professionSkillId;
    if (left.recipeSpellId != right.recipeSpellId)
        return left.recipeSpellId < right.recipeSpellId;
    if (left.outputItemId != right.outputItemId)
        return left.outputItemId < right.outputItemId;
    if (left.kind != right.kind)
        return left.kind < right.kind;
    return left.primaryProfession < right.primaryProfession;
}

bool IsValidCapability(ProfessionCapability const& capability)
{
    if (!capability.outputItemId || !capability.professionSkillId)
        return false;

    std::optional<ProfessionCapabilityKind> const kind =
        PlayerbotProfessionCapabilityCatalog::ClassifySkill(capability.professionSkillId);
    if (!kind || *kind != capability.kind)
        return false;
    bool const expectedPrimary =
        capability.professionSkillId != SKILL_COOKING && capability.professionSkillId != SKILL_FIRST_AID;
    if (capability.primaryProfession != expectedPrimary)
        return false;
    return capability.kind == ProfessionCapabilityKind::Gathering || capability.recipeSpellId != 0u;
}

std::vector<ProfessionCapability> BuildCatalog()
{
    std::vector<ProfessionCapability> capabilities;
    for (uint32 spellId = 1u; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !PlayerbotEconomyPolicy::IsProfessionRecipeSpell(
                              spellInfo->Effects[EFFECT_0].Effect, spellInfo->Effects[EFFECT_0].ItemType,
                              spellInfo->ReagentCount[EFFECT_0], spellInfo->SchoolMask))
        {
            continue;
        }

        uint32 const outputItemId = spellInfo->Effects[EFFECT_0].ItemType;
        SkillLineAbilityMapBounds const skillBounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto ability = skillBounds.first; ability != skillBounds.second; ++ability)
        {
            SkillLineAbilityEntry const* skill = ability->second;
            if (!skill || !skill->SkillLine || skill->SkillLine > std::numeric_limits<uint16>::max())
                continue;

            uint16 const skillId = static_cast<uint16>(skill->SkillLine);
            std::optional<ProfessionCapabilityKind> const kind =
                PlayerbotProfessionCapabilityCatalog::ClassifySkill(skillId);
            if (!kind)
                continue;

            capabilities.push_back({
                .outputItemId = outputItemId,
                .recipeSpellId = spellId,
                .professionSkillId = skillId,
                .kind = *kind,
                .primaryProfession = IsPrimaryProfessionSkill(skillId),
            });
        }
    }

    std::vector<ItemTemplate*> const* itemTemplates = sObjectMgr->GetItemTemplateStoreFast();
    if (itemTemplates)
    {
        for (ItemTemplate const* itemTemplate : *itemTemplates)
        {
            if (!itemTemplate)
                continue;
            std::optional<ProfessionCapability> const gathering =
                PlayerbotProfessionCapabilityCatalog::DescribeGatheringItem(itemTemplate->ItemId, itemTemplate->Class,
                                                                            itemTemplate->SubClass);
            if (gathering)
                capabilities.push_back(*gathering);
        }
    }

    std::sort(capabilities.begin(), capabilities.end(), CapabilityLess);
    capabilities.erase(std::unique(capabilities.begin(), capabilities.end()), capabilities.end());
    return capabilities;
}
}  // namespace

namespace
{
std::unordered_map<uint32, std::vector<ReagentUse>> BuildReagentUses()
{
    std::unordered_map<uint32, std::vector<ReagentUse>> uses;
    for (uint32 spellId = 1u; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !PlayerbotEconomyPolicy::IsProfessionRecipeSpell(
                              spellInfo->Effects[EFFECT_0].Effect, spellInfo->Effects[EFFECT_0].ItemType,
                              spellInfo->ReagentCount[EFFECT_0], spellInfo->SchoolMask))
        {
            continue;
        }
        SkillLineAbilityMapBounds const skillBounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto ability = skillBounds.first; ability != skillBounds.second; ++ability)
        {
            SkillLineAbilityEntry const* skill = ability->second;
            if (!skill || !skill->SkillLine || skill->SkillLine > std::numeric_limits<uint16>::max() ||
                !PlayerbotProfessionCapabilityCatalog::ClassifySkill(static_cast<uint16>(skill->SkillLine)))
            {
                continue;
            }
            for (uint8 index = 0u; index < MAX_SPELL_REAGENTS; ++index)
            {
                if (spellInfo->Reagent[index] <= 0 || spellInfo->ReagentCount[index] <= 0)
                    continue;
                uses[static_cast<uint32>(spellInfo->Reagent[index])].push_back(
                    {static_cast<uint16>(skill->SkillLine), skill->MinSkillLineRank,
                     static_cast<uint32>(spellInfo->Effects[EFFECT_0].ItemType)});
            }
        }
    }
    return uses;
}

bool DirectUse(uint32 itemId, std::span<PlannedProfessionRank const> professions, ReagentUsesFn const& lookup)
{
    for (ReagentUse const& use : lookup(itemId))
        for (PlannedProfessionRank const& profession : professions)
            if (profession.skillId == use.skillId && use.minSkillRank <= profession.rankCap)
                return true;
    return false;
}
}  // namespace

std::vector<ReagentUse> const& PlayerbotProfessionCapabilityCatalog::ReagentUses(uint32 itemId)
{
    static std::unordered_map<uint32, std::vector<ReagentUse>> const uses = BuildReagentUses();
    static std::vector<ReagentUse> const none;
    auto const found = uses.find(itemId);
    return found == uses.end() ? none : found->second;
}

bool PlayerbotProfessionCapabilityCatalog::CraftingUsesItem(uint32 itemId,
                                                            std::span<PlannedProfessionRank const> crafting,
                                                            std::span<PlannedProfessionRank const> gathering,
                                                            ReagentUsesFn const& lookup)
{
    if (!itemId || crafting.empty())
        return false;
    if (DirectUse(itemId, crafting, lookup))
        return true;
    for (ReagentUse const& hop : lookup(itemId))
    {
        bool const ownGatheringRecipe =
            std::any_of(gathering.begin(), gathering.end(), [&hop](PlannedProfessionRank const& profession)
                        { return profession.skillId == hop.skillId && hop.minSkillRank <= profession.rankCap; });
        if (ownGatheringRecipe && hop.outputItemId && hop.outputItemId != itemId &&
            DirectUse(hop.outputItemId, crafting, lookup))
            return true;
    }
    return false;
}

std::vector<ProfessionCapability> const& PlayerbotProfessionCapabilityCatalog::All()
{
    static std::vector<ProfessionCapability> const capabilities = BuildCatalog();
    return capabilities;
}

std::optional<ProfessionCapabilityKind> PlayerbotProfessionCapabilityCatalog::ClassifySkill(uint16 skillId)
{
    switch (skillId)
    {
        case SKILL_HERBALISM:
        case SKILL_MINING:
        case SKILL_SKINNING:
            return ProfessionCapabilityKind::Gathering;
        case SKILL_ALCHEMY:
        case SKILL_BLACKSMITHING:
        case SKILL_COOKING:
        case SKILL_ENCHANTING:
        case SKILL_ENGINEERING:
        case SKILL_FIRST_AID:
        case SKILL_INSCRIPTION:
        case SKILL_JEWELCRAFTING:
        case SKILL_LEATHERWORKING:
        case SKILL_TAILORING:
            return ProfessionCapabilityKind::Crafting;
        default:
            return std::nullopt;
    }
}

std::optional<ProfessionCapability> PlayerbotProfessionCapabilityCatalog::DescribeGatheringItem(uint32 itemId,
                                                                                                uint32 itemClass,
                                                                                                uint32 itemSubclass)
{
    if (!itemId || itemClass != ITEM_CLASS_TRADE_GOODS)
        return std::nullopt;

    uint16 skillId;
    switch (itemSubclass)
    {
        case ITEM_SUBCLASS_HERB:
            skillId = SKILL_HERBALISM;
            break;
        case ITEM_SUBCLASS_METAL_STONE:
            skillId = SKILL_MINING;
            break;
        case ITEM_SUBCLASS_LEATHER:
            skillId = SKILL_SKINNING;
            break;
        default:
            return std::nullopt;
    }

    return ProfessionCapability{
        .outputItemId = itemId,
        .professionSkillId = skillId,
        .kind = ProfessionCapabilityKind::Gathering,
        .primaryProfession = true,
    };
}

std::optional<ProfessionCapability> PlayerbotProfessionCapabilityCatalog::Select(
    std::vector<ProfessionCapability> candidates, std::vector<uint32> const& knownRecipeSpellIds,
    bool primaryProfessionOnly)
{
    if (candidates.empty() ||
        std::any_of(candidates.begin(), candidates.end(),
                    [](ProfessionCapability const& candidate) { return !IsValidCapability(candidate); }))
    {
        return std::nullopt;
    }

    if (primaryProfessionOnly)
    {
        std::erase_if(candidates, [](ProfessionCapability const& candidate) { return !candidate.primaryProfession; });
    }
    if (candidates.empty())
        return std::nullopt;

    std::sort(candidates.begin(), candidates.end(), CapabilityLess);
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    bool const hasKnownRecipe = std::any_of(candidates.begin(), candidates.end(),
                                            [&knownRecipeSpellIds](ProfessionCapability const& candidate)
                                            {
                                                return candidate.recipeSpellId &&
                                                       std::find(knownRecipeSpellIds.begin(), knownRecipeSpellIds.end(),
                                                                 candidate.recipeSpellId) != knownRecipeSpellIds.end();
                                            });
    if (hasKnownRecipe)
    {
        std::erase_if(candidates,
                      [&knownRecipeSpellIds](ProfessionCapability const& candidate)
                      {
                          return std::find(knownRecipeSpellIds.begin(), knownRecipeSpellIds.end(),
                                           candidate.recipeSpellId) == knownRecipeSpellIds.end();
                      });
    }

    return candidates.front();
}
