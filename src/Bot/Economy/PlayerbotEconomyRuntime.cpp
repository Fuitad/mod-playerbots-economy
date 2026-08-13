/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyRuntime.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "Ai/Base/Actions/ChooseTravelTargetAction.h"
#include "Ai/Base/Actions/EquipAction.h"
#include "Ai/Base/Actions/ListSpellsAction.h"
#include "Ai/Base/Actions/MailAction.h"
#include "Ai/Base/Actions/SellAction.h"
#include "Ai/Base/Actions/UseItemAction.h"
#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyConsumption.h"
#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "Bot/Economy/PlayerbotEconomyMail.h"
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "Bot/Economy/PlayerbotEconomyTrace.h"
#include "Bot/Economy/PlayerbotEconomyTravel.h"
#include "Bot/Economy/PlayerbotProfessionCapability.h"
#include "Bot/Personality/PlayerbotCareerAdapter.h"
#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "Bot/Personality/PlayerbotPersonalityMgr.h"
#include "BudgetValues.h"
#include "CharacterCache.h"
#include "GameTime.h"
#include "Item.h"
#include "LootObjectStack.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "World.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr char PROFESSION_WORK_ORDER_EVENT[] = "profession work order";
constexpr uint64 POSITION_ID_NAMESPACE = 0x6f4a7d19c3b258e1ULL;

std::string ReagentGroup(uint32 itemId) { return "reagent:" + std::to_string(itemId); }

std::string ItemGroup(uint32 itemId) { return "item:" + std::to_string(itemId); }

bool StartRecipeLearning(PlayerbotAI* botAI, Item* item, uint32 expectedSpellId)
{
    if (!botAI || !expectedSpellId)
        return false;

    Player* const bot = botAI->GetBot();
    if (bot->HasSpell(expectedSpellId) || !item)
        return false;

    PlayerbotRecipeCandidate const recipe = PlayerbotCareer::DescribeRecipe(item->GetTemplate(), bot, 0u);
    if (recipe.recipeSpellId != expectedSpellId || recipe.isKnown || !recipe.isUsable ||
        bot->CanUseItem(item) != EQUIP_ERR_OK)
    {
        return false;
    }

    SpellCastTargets targets;
    targets.SetUnitTarget(bot);
    bot->CastItemUseSpell(item, targets, 1u, 0u);
    return true;
}

EconomyRiskConfiguration MarketRiskConfiguration()
{
    return {
        .enabled = sPlayerbotEconomyConfig.marketMakingEnabled,
        .perGroupExposurePercent = sPlayerbotEconomyConfig.marketMakingPerGroupExposurePercent,
        .totalExposurePercent = sPlayerbotEconomyConfig.marketMakingTotalExposurePercent,
        .minimumEvidence = sPlayerbotEconomyConfig.marketMakingMinimumEvidence,
        .holdingHorizonSeconds = sPlayerbotEconomyConfig.marketMakingHoldingHorizonSeconds,
        .maximumRelistAttempts = sPlayerbotEconomyConfig.marketMakingMaximumRelistAttempts,
        .cooldownSeconds = sPlayerbotEconomyConfig.marketMakingCooldownSeconds,
    };
}

std::string PositionPublicId(uint32 traderGuid, uint64 itemGuid, uint64 now)
{
    uint64 const high = PlayerbotPersonality::SplitMix64(itemGuid ^ (static_cast<uint64>(traderGuid) << 32u) ^ now ^
                                                         POSITION_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ POSITION_ID_NAMESPACE);
    return Acore::StringFormat("{:016x}{:04x}", high, low & 0xffffu);
}

struct AuctionMailDetails
{
    uint32 itemId = 0;
    MailAuctionAnswers response = AUCTION_OUTBIDDED;
    uint32 auctionId = 0;
    uint32 quantity = 0;
    uint64 bid = 0;
    uint64 buyout = 0;
    uint64 deposit = 0;
    uint64 cut = 0;
    uint32 bidderGuid = 0;
};

std::optional<std::vector<uint64>> ParseUnsignedFields(std::string_view text, bool firstFieldHex)
{
    std::vector<uint64> fields;
    while (!text.empty())
    {
        std::size_t const separator = text.find(':');
        std::string_view const field = text.substr(0, separator);
        if (field.empty())
            return std::nullopt;

        uint64 value = 0u;
        int const base = firstFieldHex && fields.empty() ? 16 : 10;
        auto const parsed = std::from_chars(field.data(), field.data() + field.size(), value, base);
        if (parsed.ec != std::errc() || parsed.ptr != field.data() + field.size())
            return std::nullopt;
        fields.push_back(value);
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1u);
    }
    return fields;
}

std::optional<AuctionMailDetails> ParseAuctionMail(Mail const* mail)
{
    if (!mail || mail->messageType != MAIL_AUCTION)
        return std::nullopt;
    std::optional<std::vector<uint64>> const subject = ParseUnsignedFields(mail->subject, false);
    std::optional<std::vector<uint64>> const body = ParseUnsignedFields(mail->body, true);
    if (!subject || subject->size() != 5u || !body || body->size() < 5u || (*subject)[0] > UINT32_MAX ||
        (*subject)[2] > AUCTION_SALE_PENDING || (*subject)[3] > UINT32_MAX || (*subject)[4] > UINT32_MAX ||
        (*body)[1] > UINT32_MAX || (*body)[2] > UINT32_MAX || (*body)[3] > UINT32_MAX || (*body)[4] > UINT32_MAX)
    {
        return std::nullopt;
    }
    return AuctionMailDetails{
        .itemId = static_cast<uint32>((*subject)[0]),
        .response = static_cast<MailAuctionAnswers>((*subject)[2]),
        .auctionId = static_cast<uint32>((*subject)[3]),
        .quantity = static_cast<uint32>((*subject)[4]),
        .bid = (*body)[1],
        .buyout = (*body)[2],
        .deposit = (*body)[3],
        .cut = (*body)[4],
        .bidderGuid = (*body)[0] <= UINT32_MAX ? static_cast<uint32>((*body)[0]) : 0u,
    };
}

std::string TraceChainForActor(uint32 actorGuid, uint64 now)
{
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyActorChainObservation const observation = coordinator.ObserveActor(actorGuid, now);
    if (!observation.chainPublicId.empty())
        return observation.chainPublicId;

    if (!observation.available || !observation.marketId)
        return {};
    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(now);
    auto const chain =
        std::find_if(snapshot.chains.rbegin(), snapshot.chains.rend(), [&observation](EconomyChain const& candidate)
                     { return candidate.marketId == observation.marketId && candidate.group == observation.group; });
    return chain == snapshot.chains.rend() ? std::string{} : chain->publicId;
}

uint32 TraceActorIfKnown(uint32 actorGuid, uint64 now)
{
    if (!actorGuid)
        return 0u;
    EconomyCoordinatorSnapshot const snapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    auto const actor =
        std::find_if(snapshot.actors.begin(), snapshot.actors.end(),
                     [actorGuid](EconomyActorFacts const& candidate) { return candidate.characterGuid == actorGuid; });
    return actor == snapshot.actors.end() ? 0u : actorGuid;
}

std::optional<EconomyTraceEvent> TraceEventForAuction(uint32 actorGuid, uint32 auctionId, EconomyTraceKind kind)
{
    EconomyTraceSnapshot const snapshot = GetPlayerbotEconomyTrace().Snapshot();
    auto const event = std::find_if(snapshot.events.rbegin(), snapshot.events.rend(),
                                    [actorGuid, auctionId, kind](EconomyTraceEvent const& candidate)
                                    {
                                        return candidate.actorGuid == actorGuid &&
                                               candidate.correlationAuctionId == auctionId && candidate.kind == kind;
                                    });
    return event == snapshot.events.rend() ? std::nullopt : std::optional<EconomyTraceEvent>(*event);
}

EconomyFinalUseKind TraceFinalUse(FinishedGoodUse use)
{
    switch (use)
    {
        case FinishedGoodUse::Equip:
            return EconomyFinalUseKind::Equipped;
        case FinishedGoodUse::SetAmmunition:
            return EconomyFinalUseKind::AmmunitionSet;
        case FinishedGoodUse::Consume:
            return EconomyFinalUseKind::Consumed;
        case FinishedGoodUse::Apply:
            return EconomyFinalUseKind::Applied;
        case FinishedGoodUse::Recover:
            return EconomyFinalUseKind::Recovered;
    }
    return EconomyFinalUseKind::Lost;
}

bool IsFinishedGoodUsage(ItemUsage usage)
{
    return usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_AMMO ||
           usage == ITEM_USAGE_USE;
}

bool IsUsefulCraftOutput(ItemUsage usage)
{
    return usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_AMMO ||
           usage == ITEM_USAGE_QUEST || usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_USE;
}

uint32 CraftOutputPriority(ItemUsage usage)
{
    if (IsUsefulCraftOutput(usage))
        return 0;

    if (usage == ITEM_USAGE_KEEP)
        return 1;

    if (usage == ITEM_USAGE_AH)
        return 2;

    return 3;
}

uint32 AuctionMarketId(uint32 factionTemplateId)
{
    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
        return static_cast<uint32>(AuctionHouseId::Neutral);

    AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(factionTemplateId);
    return entry ? entry->houseId : 0u;
}

std::optional<GatheringProfession> GatheringProfessionForSkill(uint32 skillId)
{
    if (skillId == SKILL_HERBALISM)
        return GatheringProfession::Herbalism;
    if (skillId == SKILL_MINING)
        return GatheringProfession::Mining;
    if (skillId == SKILL_SKINNING)
        return GatheringProfession::Skinning;
    return std::nullopt;
}

std::optional<uint32> GatheringSkillForItem(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate || itemTemplate->Class != ITEM_CLASS_TRADE_GOODS)
        return std::nullopt;
    if (itemTemplate->SubClass == ITEM_SUBCLASS_HERB)
        return SKILL_HERBALISM;
    if (itemTemplate->SubClass == ITEM_SUBCLASS_METAL_STONE)
        return SKILL_MINING;
    if (itemTemplate->SubClass == ITEM_SUBCLASS_LEATHER)
        return SKILL_SKINNING;
    return std::nullopt;
}

uint32 PlannedInputCount(EconomySnapshot const& snapshot, uint32 itemId)
{
    auto const inventory = std::find_if(snapshot.inventory.begin(), snapshot.inventory.end(),
                                        [itemId](InventoryCount const& item) { return item.itemId == itemId; });
    if (inventory == snapshot.inventory.end())
        return 0u;
    return inventory->count + inventory->mailCount + inventory->purchasedCount + inventory->committedCount;
}

std::unordered_set<uint32> ApplicableUnlimitedGoldVendorItems(Player* bot)
{
    std::unordered_set<uint32> vendorItems;
    if (!bot)
        return vendorItems;

    WorldPosition botPosition(bot);
    for (TravelDestination* destination : TravelMgr::instance().getRpgTravelDestinations(bot, true, true, 200000.0f))
    {
        RpgTravelDestination* rpgDestination = dynamic_cast<RpgTravelDestination*>(destination);
        CreatureTemplate const* creatureTemplate = rpgDestination ? rpgDestination->GetCreatureTemplate() : nullptr;
        if (!creatureTemplate || !(creatureTemplate->npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;

        FactionTemplateEntry const* vendorFaction = sFactionTemplateStore.LookupEntry(creatureTemplate->faction);
        if (!vendorFaction)
            continue;
        ReputationRank const reaction = Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), vendorFaction);
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(creatureTemplate->Entry);
        if (!items)
            continue;

        bool const sameMap = destination->onMap(&botPosition);
        bool const routeAvailable = !destination->nextPoint(&botPosition, true).empty();
        for (VendorItem const* item : items->m_items)
        {
            ItemTemplate const* itemTemplate = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (!itemTemplate)
                continue;

            VendorOfferPolicyInput const input{
                .maximumCount = item->maxcount,
                .extendedCost = item->ExtendedCost,
                .factionAllowed = reaction >= REP_NEUTRAL,
                .levelAllowed =
                    bot->GetLevel() >= itemTemplate->RequiredLevel && bot->CanUseItem(itemTemplate) == EQUIP_ERR_OK,
                .reputationAllowed =
                    !itemTemplate->RequiredReputationFaction ||
                    static_cast<uint32>(bot->GetReputationRank(itemTemplate->RequiredReputationFaction)) >=
                        itemTemplate->RequiredReputationRank,
                .sameMap = sameMap,
                .routeAvailable = routeAvailable,
            };
            if (PlayerbotEconomyPolicy::IsApplicableUnlimitedGoldVendorOffer(input))
                vendorItems.insert(item->item);
        }
    }
    return vendorItems;
}

struct GatheringOpportunity
{
    GatheringProfession profession = GatheringProfession::Herbalism;
    uint32 skillId = 0;
    uint32 itemId = 0;
    uint32 quantity = 0;
    uint32 spellId = 0;
};

struct RecipeDeficit
{
    uint32 itemId = 0;
    uint32 quantity = 0;
    uint32 demandQuantity = 0;
    uint32 spellId = 0;
};

std::optional<RecipeDeficit> NextRecipeDeficit(EconomySnapshot const& snapshot)
{
    std::vector<RecipeCandidate const*> recipes;
    recipes.reserve(snapshot.recipes.size());
    for (RecipeCandidate const& recipe : snapshot.recipes)
        recipes.push_back(&recipe);
    std::stable_sort(recipes.begin(), recipes.end(),
                     [&snapshot](RecipeCandidate const* left, RecipeCandidate const* right)
                     {
                         return left->spellId == snapshot.preferredRecipeSpellId &&
                                right->spellId != snapshot.preferredRecipeSpellId;
                     });

    for (RecipeCandidate const* recipe : recipes)
    {
        for (ReagentRequirement const& reagent : recipe->reagents)
        {
            if (reagent.unlimitedGoldVendorSupply)
                continue;

            uint32 const planned = PlannedInputCount(snapshot, reagent.itemId);
            if (planned < reagent.count)
                return RecipeDeficit{reagent.itemId, reagent.count - planned, reagent.count, recipe->spellId};
        }
    }
    return std::nullopt;
}

std::optional<GatheringOpportunity> DeficitGatheringOpportunity(Player const* bot, EconomySnapshot const& snapshot,
                                                                EconomyDecision const& decision)
{
    auto const build = [bot](uint32 itemId, uint32 quantity, uint32 spellId) -> std::optional<GatheringOpportunity>
    {
        std::optional<uint32> const skillId = GatheringSkillForItem(sObjectMgr->GetItemTemplate(itemId));
        if (!skillId || !bot->HasSkill(*skillId))
            return std::nullopt;
        std::optional<GatheringProfession> const profession = GatheringProfessionForSkill(*skillId);
        if (!profession || !quantity)
            return std::nullopt;
        return GatheringOpportunity{*profession, *skillId, itemId, quantity, spellId};
    };

    if (decision.phase == EconomyPhase::BuyReagent)
    {
        if (PlayerbotEconomyPolicy::IsKnownRecipeOutput(snapshot, decision.itemId))
            return std::nullopt;
        return build(decision.itemId, decision.count, decision.spellId);
    }
    if (decision.phase != EconomyPhase::None)
        return std::nullopt;

    std::optional<RecipeDeficit> const deficit = NextRecipeDeficit(snapshot);
    if (deficit && PlayerbotEconomyPolicy::IsKnownRecipeOutput(snapshot, deficit->itemId))
        return std::nullopt;
    return deficit ? build(deficit->itemId, deficit->quantity, deficit->spellId) : std::nullopt;
}

std::optional<GatheringOpportunity> CoordinatorGatheringOpportunity(Player const* bot,
                                                                    EconomyCoordinatorSnapshot const& snapshot,
                                                                    EconomySnapshot const& economy, uint32 marketId)
{
    for (EconomyDemandGap const& gap : snapshot.gaps)
    {
        if (gap.marketId != marketId || gap.group.kind != EconomySubstitutionKind::ExactReagent ||
            !gap.group.exactItemId || !gap.remainingQuantity)
        {
            continue;
        }
        if (PlayerbotEconomyPolicy::IsKnownRecipeOutput(economy, gap.group.exactItemId))
            continue;
        std::optional<uint32> const skillId = GatheringSkillForItem(sObjectMgr->GetItemTemplate(gap.group.exactItemId));
        if (!skillId || !bot->HasSkill(*skillId))
            continue;
        std::optional<GatheringProfession> const profession = GatheringProfessionForSkill(*skillId);
        if (profession)
            return GatheringOpportunity{*profession, *skillId, gap.group.exactItemId, gap.remainingQuantity, 0u};
    }
    return std::nullopt;
}

uint8 EconomyAffinity(uint32 characterGuid)
{
    std::optional<PlayerbotPersonalityProfile> const personality = sPlayerbotPersonalityMgr.GetOrCreate(characterGuid);
    return personality ? personality->economyAffinity : 0u;
}

uint8 GatheringAffinity(uint32 characterGuid)
{
    std::optional<PlayerbotPersonalityProfile> const personality = sPlayerbotPersonalityMgr.GetOrCreate(characterGuid);
    return personality ? personality->gatheringAffinity : 0u;
}

std::vector<uint16> const& PrimaryCapabilitySkillIds()
{
    static std::vector<uint16> const skillIds = []
    {
        std::vector<uint16> result;
        for (ProfessionCapability const& capability : PlayerbotProfessionCapabilityCatalog::All())
        {
            if (capability.primaryProfession)
                result.push_back(capability.professionSkillId);
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }();
    return skillIds;
}

std::vector<uint16> LearnedPrimaryCapabilitySkillIds(Player const* bot)
{
    std::vector<uint16> learned;
    for (uint16 skillId : PrimaryCapabilitySkillIds())
        if (bot->HasSkill(skillId))
            learned.push_back(skillId);
    return learned;
}

std::vector<uint16> LearnedCareerSkillIds(Player const* bot, PlayerbotCareerPlan const& plan)
{
    std::vector<uint16> learned;
    auto const appendLearned = [bot, &learned](uint16 skillId)
    {
        if (bot->HasSkill(skillId) && std::find(learned.begin(), learned.end(), skillId) == learned.end())
            learned.push_back(skillId);
    };
    for (uint16 skillId : plan.primarySkills)
        appendLearned(skillId);
    for (uint16 skillId : plan.secondarySkills)
        appendLearned(skillId);
    if (plan.capabilityGoal)
        appendLearned(plan.capabilityGoal->professionSkillId);
    return learned;
}

std::vector<uint32> KnownCapabilityRecipeSpellIds(Player const* bot)
{
    std::vector<uint32> known;
    for (ProfessionCapability const& capability : PlayerbotProfessionCapabilityCatalog::All())
    {
        if (capability.recipeSpellId && bot->HasSpell(capability.recipeSpellId))
            known.push_back(capability.recipeSpellId);
    }
    std::sort(known.begin(), known.end());
    known.erase(std::unique(known.begin(), known.end()), known.end());
    return known;
}

bool ProductionOutputMatchesGroup(Player const* bot, uint32 itemId, EconomySubstitutionGroup const& group)
{
    if (group.kind == EconomySubstitutionKind::ExactReagent)
        return group.exactItemId == itemId;

    std::optional<FinishedGoodDescription> const output =
        PlayerbotEconomyConsumption::Describe(bot, sObjectMgr->GetItemTemplate(itemId));
    if (!output || output->group.kind != group.kind)
        return false;
    if (group.kind == EconomySubstitutionKind::Consumable)
    {
        ConsumptionNeed requirement;
        requirement.group = group;
        requirement.requiredUtility = group.valueBand;
        return PlayerbotEconomyConsumption::MatchesNeed(requirement, output->group, output->utility);
    }
    if (group.kind != EconomySubstitutionKind::Equipment)
        return output->group == group;
    return output->group.equipmentSlot == group.equipmentSlot && output->group.tier == group.tier;
}

std::vector<EconomyProductionRecipe> ProductionRecipes(Player const* bot, EconomySnapshot const& economy,
                                                       EconomyCoordinatorSnapshot const& coordinator, uint32 marketId)
{
    std::vector<EconomyProductionRecipe> recipes;
    for (EconomyDemandGap const& gap : coordinator.gaps)
    {
        if (gap.marketId != marketId || !gap.demandQuantity)
            continue;
        for (RecipeCandidate const& candidate : economy.recipes)
        {
            if (!candidate.spellId || !candidate.craftedItemId ||
                !ProductionOutputMatchesGroup(bot, candidate.craftedItemId, gap.group))
            {
                continue;
            }
            ItemTemplate const* const outputTemplate = sObjectMgr->GetItemTemplate(candidate.craftedItemId);
            uint32 const stackSize = outputTemplate ? std::max(1u, outputTemplate->GetMaxStackSize()) : 1u;
            uint32 const ceiling = static_cast<uint32>(
                std::min<uint64>(static_cast<uint64>(stackSize) * sPlayerbotEconomyConfig.productionMaxBatchStacks,
                                 std::numeric_limits<uint32>::max()));
            uint32 const batchQuantity = PlayerbotEconomyPolicy::ProductionBatchQuantity(candidate, economy, ceiling);
            if (!batchQuantity)
                continue;
            EconomyProductionRecipe const recipe{gap.group, candidate.spellId, candidate.craftedItemId, batchQuantity};
            if (std::find(recipes.begin(), recipes.end(), recipe) == recipes.end())
                recipes.push_back(recipe);
        }
    }
    return recipes;
}

uint64 ProductionLeaseExpiry(PlayerbotCareerPlan const& careerPlan, uint64 now)
{
    uint64 const interval = PlayerbotEconomyPolicy::CareerIntervalSeconds(sPlayerbotAIConfig.randomBotUpdateInterval,
                                                                          careerPlan.engagement);
    uint64 const duration =
        interval > std::numeric_limits<uint64>::max() / 2u ? std::numeric_limits<uint64>::max() : interval * 2u;
    return duration > std::numeric_limits<uint64>::max() - now ? std::numeric_limits<uint64>::max() : now + duration;
}

void RemoveClaimBackedProductionSupply(std::vector<EconomySupplyFact>& supplies,
                                       EconomyCoordinatorSnapshot const& coordinator, uint32 characterGuid)
{
    for (EconomyAssignment const& claim : coordinator.claims)
    {
        if (claim.characterGuid != characterGuid || claim.kind != EconomyClaimKind::Production ||
            claim.state != EconomyClaimState::Leased || !claim.committedQuantity)
        {
            continue;
        }

        uint32 remaining = claim.committedQuantity;
        for (EconomySupplyFact& supply : supplies)
        {
            if (!remaining || supply.source != EconomySupplySource::Inventory || supply.itemId != claim.outputItemId ||
                supply.group != claim.group)
            {
                continue;
            }
            uint32 const removed = std::min(supply.quantity, remaining);
            supply.quantity -= removed;
            remaining -= removed;
        }
        std::erase_if(supplies, [](EconomySupplyFact const& supply) { return !supply.quantity; });
    }
}

void RefreshCoordinator(PlayerbotAI* botAI, EconomySnapshot const& snapshot, ConsumptionSnapshot const& consumption,
                        uint32 marketId, uint64 now, uint32 excludedItemId, uint32 excludedQuantity)
{
    Player* const bot = botAI->GetBot();
    EconomyActorFacts actor;
    actor.characterGuid = bot->GetGUID().GetCounter();
    actor.accountId = bot->GetSession()->GetAccountId();
    actor.marketId = marketId;
    actor.online = bot->IsInWorld();
    actor.autonomous = !botAI->HasActivePlayerMaster();
    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(actor.characterGuid);
    if (personality)
    {
        actor.craftingAffinity = personality->craftingAffinity;
        actor.gatheringAffinity = personality->gatheringAffinity;
        actor.economyAffinity = personality->economyAffinity;
    }
    actor.freePrimaryProfessionSlots =
        static_cast<uint8>(std::min<uint32>(bot->GetFreePrimaryProfessionPoints(), std::numeric_limits<uint8>::max()));
    actor.professionSkillIds = LearnedPrimaryCapabilitySkillIds(bot);
    actor.recipeSpellIds = KnownCapabilityRecipeSpellIds(bot);

    std::optional<RecipeDeficit> const deficit = NextRecipeDeficit(snapshot);
    if (deficit)
        actor.demands.push_back({EconomySubstitutionGroup::ExactReagent(deficit->itemId), deficit->demandQuantity});
    std::vector<EconomyDemandFact> const consumerDemands = PlayerbotEconomyConsumption::DemandFacts(consumption);
    actor.demands.insert(actor.demands.end(), consumerDemands.begin(), consumerDemands.end());
    actor.supplies = PlayerbotEconomyConsumption::SupplyFacts(consumption);
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();

    std::map<uint32, uint32> inventorySupply;
    std::map<uint32, uint32> mailSupply;
    if (deficit)
    {
        auto const item =
            std::find_if(snapshot.inventory.begin(), snapshot.inventory.end(),
                         [&deficit](InventoryCount const& candidate) { return candidate.itemId == deficit->itemId; });
        if (item != snapshot.inventory.end())
        {
            inventorySupply[item->itemId] = item->count;
            mailSupply[item->itemId] = item->mailCount;
        }
    }
    if (excludedItemId && excludedQuantity)
    {
        uint32& quantity = inventorySupply[excludedItemId];
        quantity = quantity > excludedQuantity ? quantity - excludedQuantity : 0u;
    }
    for (auto const& [itemId, quantity] : inventorySupply)
    {
        if (quantity)
            actor.supplies.push_back(
                {EconomySubstitutionGroup::ExactReagent(itemId), quantity, EconomySupplySource::Inventory, itemId});
    }
    for (auto const& [itemId, quantity] : mailSupply)
    {
        if (quantity)
            actor.supplies.push_back(
                {EconomySubstitutionGroup::ExactReagent(itemId), quantity, EconomySupplySource::Mail, itemId});
    }

    RemoveClaimBackedProductionSupply(actor.supplies, coordinator.Snapshot(now), actor.characterGuid);
    coordinator.RefreshActor(std::move(actor), now);

    EconomyMarketFacts market;
    market.marketId = marketId;
    for (AuctionListingCandidate const& auction : snapshot.auctions)
    {
        if (auction.itemId && auction.count)
        {
            market.supplies.push_back({EconomySubstitutionGroup::ExactReagent(auction.itemId), auction.count,
                                       EconomySupplySource::ActiveAuction});
        }
    }
    for (ConsumptionOffer const& offer : consumption.offers)
    {
        if (offer.count)
            market.supplies.push_back({offer.group, offer.count, EconomySupplySource::ActiveAuction});
    }
    coordinator.RefreshMarket(std::move(market), now);
}

char const* AutonomousBlockerName(AutonomousGatheringBlocker blocker)
{
    switch (blocker)
    {
        case AutonomousGatheringBlocker::None:
            return "gathering_none";
        case AutonomousGatheringBlocker::DemandGone:
            return "gathering_demand_gone";
        case AutonomousGatheringBlocker::DestinationUnavailable:
            return "gathering_destination_unavailable";
        case AutonomousGatheringBlocker::DestinationExpired:
            return "gathering_destination_expired";
        case AutonomousGatheringBlocker::InventoryFull:
            return "gathering_inventory_full";
        case AutonomousGatheringBlocker::Unsafe:
            return "gathering_unsafe";
        case AutonomousGatheringBlocker::OneKillBoundReached:
            return "gathering_one_kill_bound";
    }
    return "gathering_unknown";
}

char const* GatheringDestinationBlockerName(GatheringDestinationBlocker blocker)
{
    switch (blocker)
    {
        case GatheringDestinationBlocker::None:
            return "gathering_destination_none";
        case GatheringDestinationBlocker::Empty:
            return "gathering_destination_empty";
        case GatheringDestinationBlocker::Full:
            return "gathering_destination_full";
        case GatheringDestinationBlocker::Expired:
            return "gathering_destination_expired";
        case GatheringDestinationBlocker::Cooldown:
            return "gathering_destination_cooldown";
        case GatheringDestinationBlocker::WrongMap:
            return "gathering_destination_wrong_map";
        case GatheringDestinationBlocker::WrongSkill:
            return "gathering_destination_wrong_skill";
        case GatheringDestinationBlocker::InsufficientSkill:
            return "gathering_destination_insufficient_skill";
        case GatheringDestinationBlocker::WrongLevel:
            return "gathering_destination_wrong_level";
        case GatheringDestinationBlocker::Inaccessible:
            return "gathering_destination_inaccessible";
    }
    return "gathering_destination_unknown";
}

uint64 LowestCompetingBuyoutPerItem(AuctionHouseObject* auctionHouse, uint32 itemId, uint32 botAccountId)
{
    if (!auctionHouse)
        return 0;

    uint64 lowest = 0;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        (void)auctionId;
        if (!auction || auction->item_template != itemId || !auction->itemCount || !auction->buyout)
            continue;

        uint32 const ownerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
        if (ownerAccountId == botAccountId)
            continue;

        uint64 const perItem = (static_cast<uint64>(auction->buyout) + auction->itemCount - 1u) / auction->itemCount;
        if (!lowest || perItem < lowest)
            lowest = perItem;
    }
    return lowest;
}

class EconomyTravelAction final : public ChooseTravelTargetAction
{
public:
    explicit EconomyTravelAction(PlayerbotAI* botAI) : ChooseTravelTargetAction(botAI, "economy travel") {}

    void Apply(TravelTarget* newTarget, TravelTarget* oldTarget) { setNewTarget(newTarget, oldTarget); }
    void Clear(TravelTarget* target) { SetNullTarget(target); }
};

class EconomyUseItemAction final : public UseItemAction
{
public:
    explicit EconomyUseItemAction(PlayerbotAI* botAI) : UseItemAction(botAI, "economy final use", true) {}

    bool Apply(Item* item) { return UseItemAuto(item); }
};

enum class ExecutionResult : uint8
{
    Failed,
    Scheduled,
    Operation,
    Recovery
};

class DefaultPlayerbotEconomyRuntime final : public PlayerbotEconomyRuntime
{
public:
    bool IsEligible(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan) const override;
    PlayerbotEconomyCycleResult ExecuteCycle(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan) override;
    void Reset(PlayerbotAI* botAI) override;

private:
    EconomySnapshot BuildSnapshot(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan);
    ConsumptionSnapshot BuildConsumptionSnapshot(PlayerbotAI* botAI, EconomySnapshot const& economy, uint32 marketId,
                                                 uint64 now);
    Creature* FindAuctioneer(PlayerbotAI* botAI);
    ExecutionResult ExecuteDecision(PlayerbotAI* botAI, EconomyDecision const& decision, Creature* auctioneer);
    ExecutionResult ExecuteConsumption(PlayerbotAI* botAI, ConsumptionDecision const& decision, Creature* auctioneer);
    ExecutionResult CollectAuctionMail(PlayerbotAI* botAI);
    ExecutionResult BuyReagent(PlayerbotAI* botAI, EconomyDecision const& decision, Creature* auctioneer,
                               EconomyClaimPriority priority = EconomyClaimPriority::Producer,
                               std::optional<EconomySubstitutionGroup> claimGroup = std::nullopt);
    ExecutionResult SellSurplus(PlayerbotAI* botAI, EconomyDecision const& decision, Creature* auctioneer);
    void ObserveMarketEvidence(PlayerbotAI* botAI, uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ReconcileMarketPositionMail(PlayerbotAI* botAI, uint32 marketId,
                                                                           uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ExecuteMarketMaking(PlayerbotAI* botAI, EconomySnapshot const& snapshot,
                                                                   Creature* auctioneer, uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ManageMarketPosition(PlayerbotAI* botAI, EconomyPosition const& position,
                                                                    EconomyMarketSnapshot const& market,
                                                                    Creature* auctioneer, bool ordinaryVendorSupply,
                                                                    uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ManagePendingMarketPosition(PlayerbotAI* botAI,
                                                                           EconomyPosition const& position,
                                                                           EconomyMarketSnapshot const& market,
                                                                           Creature* auctioneer, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> OpenMarketPosition(PlayerbotAI* botAI, EconomySnapshot const& snapshot,
                                                                  EconomyMarketSnapshot const& market,
                                                                  Creature* auctioneer, uint32 marketId, uint64 now);
    bool IsSafePositionItem(PlayerbotAI* botAI, Item const* item, EconomyPosition const& position) const;
    std::optional<PlayerbotEconomyCycleResult> ListMarketPosition(PlayerbotAI* botAI, EconomyPosition const& position,
                                                                  Item* item, Creature* auctioneer,
                                                                  EconomyReferencePrice const& reference, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> VendorMarketPosition(PlayerbotAI* botAI, EconomyPosition const& position,
                                                                    Item* item, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ExecuteAutonomousGathering(PlayerbotAI* botAI,
                                                                          PlayerbotCareerPlan const& careerPlan,
                                                                          EconomySnapshot const& snapshot,
                                                                          EconomyDecision const& productionDecision,
                                                                          uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> StartAutonomousGathering(PlayerbotAI* botAI,
                                                                        GatheringOpportunity const& opportunity,
                                                                        bool allowFailureResult, uint32 marketId,
                                                                        uint64 now);
    bool HasMatchingGatheringLoot(PlayerbotAI* botAI, uint32 skillId);
    bool StartOneSkinningKill(PlayerbotAI* botAI, GatheringTravelDestination* destination);
    bool TravelToGatheringPoint(PlayerbotAI* botAI, GatheringTravelDestination* destination, WorldPosition* point);
    bool TravelToAuctionHouse(PlayerbotAI* botAI);
    bool TravelToMailbox(PlayerbotAI* botAI);
    bool TravelToDestination(PlayerbotAI* botAI, TravelDestination* destination);
    bool IsInventoryBagItem(Item const* item) const;
    bool IsSafeSaleItem(PlayerbotAI* botAI, Item const* item, EconomyDecision const& decision);
    std::vector<ProfessionCapability> const& CapabilityCandidates(Player const* bot,
                                                                  EconomySubstitutionGroup const& group);
    void RevalidateCapabilities(PlayerbotAI* botAI, uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ReconcileCapabilityGoal(PlayerbotAI* botAI,
                                                                       PlayerbotCareerPlan const& careerPlan,
                                                                       uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ExecuteTrainerObjective(PlayerbotAI* botAI,
                                                                       PlayerbotCareerPlan const& careerPlan);
    std::optional<PlayerbotEconomyCycleResult> ReconcileRecipeLearning(PlayerbotAI* botAI, uint64 now);
    void ReconcileCraftTrace(Player* bot, uint64 now);

    struct CommittedFinishedGood
    {
        EconomySubstitutionGroup group;
        FinishedGoodUse use = FinishedGoodUse::Equip;
        uint32 itemId = 0;
        uint32 quantity = 0;
        std::string chainPublicId;
        uint32 counterpartyGuid = 0;
    };

    struct PendingCraftTrace
    {
        std::string chainPublicId;
        uint64 coordinatorLeaseId = 0;
        uint32 actorGuid = 0;
        uint32 itemId = 0;
        uint32 recipeSpellId = 0;
        uint32 startingQuantity = 0;
        uint64 startedAt = 0;
    };

    struct CommittedRecipe
    {
        uint32 itemId = 0;
        uint32 recipeSpellId = 0;
        std::string chainPublicId;
        uint32 counterpartyGuid = 0;
    };

    struct ActiveGatheringTrip
    {
        AutonomousGatheringPlan plan;
        uint32 skillId = 0;
        uint32 spellId = 0;
        uint64 coordinatorLeaseId = 0;
        uint32 committedQuantity = 0;
        bool coordinatorSettled = false;
        GatheringTravelDestination* destination = nullptr;
        std::vector<WorldPosition*> attemptedPoints;
        ObjectGuid killTarget;
    };

    TravelDestination* ownedTravelDestination = nullptr;
    bool ownsTravelStrategy = false;
    std::map<uint64, CommittedFinishedGood> committedFinishedGoods;
    std::map<uint64, CommittedRecipe> committedRecipes;
    std::map<uint32, uint32> pendingGatheredSupply;
    std::map<std::pair<uint8, EconomySubstitutionGroup>, std::vector<ProfessionCapability>> capabilityCandidates;
    std::unique_ptr<TravelDestination> activeGatheringPointDestination;
    std::optional<ActiveGatheringTrip> activeGathering;
    std::optional<PlayerbotTrainerTravelSelection> activeTrainer;
    std::optional<PlayerbotCareerTrainerObjective> activeTrainerObjective;
    std::optional<PendingCraftTrace> pendingCraftTrace;
};

void DefaultPlayerbotEconomyRuntime::ReconcileCraftTrace(Player* bot, uint64 now)
{
    if (!pendingCraftTrace)
        return;
    uint32 const currentQuantity = bot->GetItemCount(pendingCraftTrace->itemId);
    if (currentQuantity > pendingCraftTrace->startingQuantity)
    {
        uint32 const producedQuantity = currentQuantity - pendingCraftTrace->startingQuantity;
        EconomyProductionOutput productionOutput;
        if (pendingCraftTrace->coordinatorLeaseId)
        {
            productionOutput =
                ReconcileProductionInventory(GetPlayerbotEconomyCoordinator(), pendingCraftTrace->coordinatorLeaseId,
                                             pendingCraftTrace->startingQuantity, currentQuantity, now);
        }
        [[maybe_unused]] bool const recorded =
            PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                .Complete(true, {
                                    .deduplicationKey = Acore::StringFormat(
                                        "craft:{}:{}:{}", pendingCraftTrace->actorGuid,
                                        pendingCraftTrace->recipeSpellId, pendingCraftTrace->startedAt),
                                    .chainPublicId = pendingCraftTrace->chainPublicId,
                                    .actorGuid = pendingCraftTrace->actorGuid,
                                    .itemId = pendingCraftTrace->itemId,
                                    .recipeSpellId = pendingCraftTrace->recipeSpellId,
                                    .quantity = producedQuantity,
                                    .occurredAt = now,
                                    .kind = EconomyTraceKind::Crafted,
                                });
        if (!pendingCraftTrace->coordinatorLeaseId || productionOutput.completed)
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        pendingCraftTrace.reset();
        return;
    }
    if (now > pendingCraftTrace->startedAt + 60u)
    {
        if (!pendingCraftTrace->coordinatorLeaseId)
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        pendingCraftTrace.reset();
    }
}

bool DefaultPlayerbotEconomyRuntime::IsEligible(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan) const
{
    Player* const bot = botAI->GetBot();
    EconomyEligibility eligibility;
    eligibility.enabled = sPlayerbotEconomyConfig.lifecycleEnabled;
    eligibility.randomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
    eligibility.activePlayerMaster = botAI->HasActivePlayerMaster();
    eligibility.inCombat = bot->IsInCombat();
    eligibility.inBattleground = bot->InBattleground();
    eligibility.dead = bot->isDead();
    eligibility.teleporting = bot->IsBeingTeleported();
    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(bot->GetGUID().GetCounter());
    bool const capabilityCandidate =
        personality &&
        (personality->craftingAffinity >= PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM ||
         personality->gatheringAffinity >= PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM) &&
        (bot->GetFreePrimaryProfessionPoints() || !LearnedPrimaryCapabilitySkillIds(bot).empty());
    eligibility.careerMarketEligible = PlayerbotCareer::SchedulesProfessionWork(careerPlan) || capabilityCandidate;
    eligibility.hasActionableProfessionWork = !careerPlan.primarySkills.empty() ||
                                              !careerPlan.secondarySkills.empty() ||
                                              careerPlan.capabilityGoal.has_value() || capabilityCandidate;
    return PlayerbotEconomyPolicy::IsEligible(eligibility);
}

std::vector<ProfessionCapability> const& DefaultPlayerbotEconomyRuntime::CapabilityCandidates(
    Player const* bot, EconomySubstitutionGroup const& group)
{
    auto const key = std::make_pair(bot->getClass(), group);
    auto const existing = capabilityCandidates.find(key);
    if (existing != capabilityCandidates.end())
        return existing->second;

    std::vector<ProfessionCapability> candidates;
    for (ProfessionCapability const& capability : PlayerbotProfessionCapabilityCatalog::All())
    {
        if (!capability.primaryProfession)
            continue;

        bool const matches = ProductionOutputMatchesGroup(bot, capability.outputItemId, group);
        if (matches)
            candidates.push_back(capability);
    }

    return capabilityCandidates.emplace(key, std::move(candidates)).first->second;
}

void DefaultPlayerbotEconomyRuntime::RevalidateCapabilities(PlayerbotAI* botAI, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(now);
    std::vector<uint32> knownRecipes;
    for (EconomyActorFacts const& actor : snapshot.actors)
    {
        if (actor.online && actor.autonomous && actor.marketId == marketId)
            knownRecipes.insert(knownRecipes.end(), actor.recipeSpellIds.begin(), actor.recipeSpellIds.end());
    }
    std::sort(knownRecipes.begin(), knownRecipes.end());
    knownRecipes.erase(std::unique(knownRecipes.begin(), knownRecipes.end()), knownRecipes.end());

    for (EconomyDemandGap const& gap : snapshot.gaps)
    {
        if (gap.marketId != marketId || !gap.remainingQuantity)
            continue;

        std::vector<ProfessionCapability> candidates = CapabilityCandidates(bot, gap.group);
        std::vector<ProfessionCapability> available;
        for (ProfessionCapability const& capability : candidates)
        {
            bool const hasProvider =
                std::any_of(snapshot.actors.begin(), snapshot.actors.end(),
                            [marketId, &capability](EconomyActorFacts const& actor)
                            {
                                if (!actor.online || !actor.autonomous || actor.marketId != marketId ||
                                    std::find(actor.professionSkillIds.begin(), actor.professionSkillIds.end(),
                                              capability.professionSkillId) == actor.professionSkillIds.end())
                                {
                                    return false;
                                }
                                return capability.kind == ProfessionCapabilityKind::Gathering ||
                                       std::find(actor.recipeSpellIds.begin(), actor.recipeSpellIds.end(),
                                                 capability.recipeSpellId) != actor.recipeSpellIds.end();
                            });
            if (hasProvider)
                available.push_back(capability);
        }
        if (!available.empty())
            candidates = std::move(available);

        std::optional<ProfessionCapability> const selected =
            PlayerbotProfessionCapabilityCatalog::Select(std::move(candidates), knownRecipes, true);
        if (!selected)
            continue;

        coordinator.RevalidateCapability({{marketId, gap.group, *selected}, true}, now);
    }
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ReconcileCapabilityGoal(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    EconomyCoordinatorSnapshot const snapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    auto const matchesGoal = [&careerPlan, marketId](EconomyCapabilityBlocker const& blocker)
    {
        if (!careerPlan.capabilityGoal || blocker.requirement.marketId != marketId)
            return false;
        PlayerbotCareerCapabilityGoal const& goal = *careerPlan.capabilityGoal;
        ProfessionCapability const& capability = blocker.requirement.capability;
        return goal.professionSkillId == capability.professionSkillId &&
               goal.recipeSpellId == capability.recipeSpellId && goal.outputItemId == capability.outputItemId;
    };

    if (careerPlan.capabilityGoal)
    {
        PlayerbotCareerCapabilityGoal const& goal = *careerPlan.capabilityGoal;
        auto const matching =
            std::find_if(snapshot.capabilityBlockers.begin(), snapshot.capabilityBlockers.end(), matchesGoal);
        bool const satisfied = goal.kind == PlayerbotCareerCapabilityGoalKind::Trainer
                                   ? bot->HasSkill(goal.professionSkillId)
                                   : bot->HasSpell(goal.recipeSpellId);
        bool const reassigned = matching != snapshot.capabilityBlockers.end() &&
                                matching->state == EconomyCapabilityBlockerState::Persistent &&
                                matching->assignedActorGuid &&
                                matching->assignedActorGuid != bot->GetGUID().GetCounter();
        if (satisfied || matching == snapshot.capabilityBlockers.end() || reassigned)
        {
            PlayerbotCareerPlan updated = careerPlan;
            if (PlayerbotCareer::ClearCapabilityGoal(updated))
            {
                PlayerbotCareer::SavePersistentPlan(bot->GetGUID().GetCounter(), updated);
                PlayerbotEconomyCycleResult result;
                result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
                result.phase = goal.kind == PlayerbotCareerCapabilityGoalKind::Recipe ? EconomyPhase::BuyRecipe
                                                                                      : EconomyPhase::None;
                result.workIdentity = {goal.recipeSpellId, goal.outputItemId, 0u, 0u};
                result.blocker = "capability_goal_cleared";
                result.schedulingEffect = EconomyAttemptOutcome::Operation;
                return result;
            }
        }
        return std::nullopt;
    }

    auto const assigned = std::find_if(snapshot.capabilityBlockers.begin(), snapshot.capabilityBlockers.end(),
                                       [bot, marketId](EconomyCapabilityBlocker const& blocker)
                                       {
                                           return blocker.requirement.marketId == marketId &&
                                                  blocker.state == EconomyCapabilityBlockerState::Persistent &&
                                                  blocker.assignedActorGuid == bot->GetGUID().GetCounter() &&
                                                  blocker.assignedWorkKind.has_value();
                                       });
    if (assigned == snapshot.capabilityBlockers.end())
        return std::nullopt;

    PlayerbotCareerPlan updated = careerPlan;
    ProfessionCapability const& capability = assigned->requirement.capability;
    PlayerbotCareerCapabilityGoal const goal = {
        *assigned->assignedWorkKind == EconomyWorkKind::Recipe ? PlayerbotCareerCapabilityGoalKind::Recipe
                                                               : PlayerbotCareerCapabilityGoalKind::Trainer,
        capability.professionSkillId,
        capability.recipeSpellId,
        capability.outputItemId,
    };
    if (!PlayerbotCareer::TryAssignCapabilityGoal(updated, goal, PrimaryCapabilitySkillIds(),
                                                  LearnedPrimaryCapabilitySkillIds(bot)))
    {
        return std::nullopt;
    }

    PlayerbotCareer::SavePersistentPlan(bot->GetGUID().GetCounter(), updated);
    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.phase =
        goal.kind == PlayerbotCareerCapabilityGoalKind::Recipe ? EconomyPhase::BuyRecipe : EconomyPhase::None;
    result.workIdentity = {goal.recipeSpellId, goal.outputItemId, 0u, 0u};
    result.blocker = "capability_goal_assigned";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteTrainerObjective(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan)
{
    Player* const bot = botAI->GetBot();
    if (activeTrainerObjective && bot->HasSkill(activeTrainerObjective->professionSkillId))
    {
        PlayerbotCareerTrainerObjective const completed = *activeTrainerObjective;
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.blocker = completed.kind == PlayerbotCareerTrainerObjectiveKind::BaseCareer
                             ? "base_career_profession_learned"
                             : "capability_profession_learned";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }

    PlayerbotCareerAcquisition const selectedObjective = PlayerbotCareer::SelectTrainerObjective(
        careerPlan, LearnedCareerSkillIds(bot, careerPlan), PrimaryCapabilitySkillIds(),
        static_cast<uint8>(std::min<uint32>(bot->GetFreePrimaryProfessionPoints(), std::numeric_limits<uint8>::max())));
    if (!selectedObjective.objective)
    {
        if (activeTrainerObjective || activeTrainer)
            Reset(botAI);
        return std::nullopt;
    }
    if (selectedObjective.state == PlayerbotCareerAcquisitionState::Blocked)
    {
        if (activeTrainerObjective || activeTrainer)
            Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = PlayerbotCareer::AcquisitionBlockerCode(selectedObjective.blocker);
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!activeTrainerObjective || *activeTrainerObjective != *selectedObjective.objective)
    {
        if (activeTrainerObjective || activeTrainer)
            Reset(botAI);
        activeTrainerObjective = selectedObjective.objective;
    }
    PlayerbotCareerTrainerObjective const objective = *activeTrainerObjective;

    AiObjectContext* const context = botAI->GetAiObjectContext();
    uint32 const availableMoney = AI_VALUE2(uint32, "free money for", static_cast<uint32>(NeedMoneyFor::tradeskill));
    if (!activeTrainer)
    {
        PlayerbotTrainerTravelSelection const selected =
            sPlayerbotEconomyTravelCatalog.SelectTrainer(bot, objective, availableMoney);
        if (!selected.destination)
        {
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = PlayerbotCareer::AcquisitionBlockerCode(selected.blocker);
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            return result;
        }
        activeTrainer = selected;
    }

    if (!TravelToDestination(botAI, activeTrainer->destination))
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
        result.blocker = "trainer_travel_deferred";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }

    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (target->getStatus() != TRAVEL_STATUS_WORK)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
        result.blocker = "trainer_travel";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }

    Creature* trainerCreature = bot->FindNearestCreature(activeTrainer->entry, INTERACTION_DISTANCE * 3.0f);
    Trainer::Trainer* trainer = trainerCreature ? sObjectMgr->GetTrainer(activeTrainer->entry) : nullptr;
    if (!trainerCreature || !trainerCreature->IsAlive() || !trainer)
    {
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "trainer_unavailable";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    float const reputationDiscount = bot->GetReputationPriceDiscount(trainerCreature);
    if (!trainer->IsTrainerValidForPlayer(bot) ||
        !PlayerbotCareer::TrainerOffersCareerLesson(objective, bot, trainer, reputationDiscount,
                                                    std::numeric_limits<uint32>::max()))
    {
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "trainer_ineligible";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!PlayerbotCareer::TrainerOffersCareerLesson(objective, bot, trainer, reputationDiscount, availableMoney))
    {
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "insufficient_protected_money";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    std::vector<PlayerbotTrainerLessonCandidate> lessons;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
        SpellInfo const* spellInfo = trainerSpell ? sSpellMgr->GetSpellInfo(trainerSpell->SpellId) : nullptr;
        if (!trainerSpell || !spellInfo || !trainer->CanTeachSpell(bot, trainerSpell))
            continue;
        uint32 const cost = static_cast<uint32>(std::floor(trainerSpell->MoneyCost * reputationDiscount));
        lessons.push_back(PlayerbotCareer::DescribeTrainerLesson(*trainerSpell, spellInfo, bot, cost));
    }

    std::vector<uint32> const selected = PlayerbotCareer::SelectTrainerLessons(objective, lessons);
    uint32 remainingMoney = availableMoney;
    bool attempted = false;
    PlayerbotCareerAcquisitionBlocker rejectedLesson = PlayerbotCareerAcquisitionBlocker::TrainerIneligible;
    for (uint32 spellId : selected)
    {
        auto const lesson =
            std::find_if(lessons.begin(), lessons.end(), [spellId](PlayerbotTrainerLessonCandidate const& candidate)
                         { return candidate.spellId == spellId; });
        if (lesson == lessons.end())
            continue;
        if (lesson->cost > remainingMoney || lesson->cost > bot->GetMoney())
        {
            rejectedLesson = PlayerbotCareerAcquisitionBlocker::InsufficientProtectedMoney;
            continue;
        }

        uint32 const moneyBefore = bot->GetMoney();
        trainer->TeachSpell(trainerCreature, bot, spellId);
        uint32 const spent = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0u;
        remainingMoney -= std::min(remainingMoney, spent);
        attempted = attempted || spent || bot->HasSpell(spellId);
        if (bot->HasSkill(objective.professionSkillId))
            break;
    }

    PlayerbotCareerAcquisition const acquisition = PlayerbotCareer::EvaluateTrainerObjective(
        objective, {
                       .professionLearned = bot->HasSkill(objective.professionSkillId),
                       .atTrainer = true,
                       .lessonAttempted = attempted,
                   });
    bool const learned = acquisition.state == PlayerbotCareerAcquisitionState::Complete;
    if (learned)
        Reset(botAI);
    PlayerbotEconomyCycleResult result;
    result.outcome =
        learned ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
    result.blocker =
        learned ? (objective.kind == PlayerbotCareerTrainerObjectiveKind::BaseCareer ? "base_career_profession_learned"
                                                                                     : "capability_profession_learned")
        : attempted ? PlayerbotCareer::AcquisitionBlockerCode(acquisition.blocker)
                    : PlayerbotCareer::AcquisitionBlockerCode(rejectedLesson);
    result.schedulingEffect = learned ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ReconcileRecipeLearning(PlayerbotAI* botAI,
                                                                                                   uint64 now)
{
    Player* const bot = botAI->GetBot();
    for (auto committed = committedRecipes.begin(); committed != committedRecipes.end(); ++committed)
    {
        uint64 const itemGuidCounter = committed->first;
        CommittedRecipe const recipe = committed->second;
        if (bot->HasSpell(recipe.recipeSpellId))
        {
            if (!recipe.chainPublicId.empty())
            {
                [[maybe_unused]] bool const recorded =
                    PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                        .Complete(true,
                                  {
                                      .deduplicationKey = Acore::StringFormat(
                                          "recipe-learned:{}:{}", bot->GetGUID().GetCounter(), recipe.recipeSpellId),
                                      .chainPublicId = recipe.chainPublicId,
                                      .actorGuid = bot->GetGUID().GetCounter(),
                                      .counterpartyGuid = recipe.counterpartyGuid,
                                      .itemId = recipe.itemId,
                                      .recipeSpellId = recipe.recipeSpellId,
                                      .quantity = 1u,
                                      .occurredAt = now,
                                      .kind = EconomyTraceKind::FinalUse,
                                      .finalUse = EconomyFinalUseKind::Learned,
                                  });
            }

            committedRecipes.erase(committed);
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::Operation;
            result.phase = EconomyPhase::BuyRecipe;
            result.workIdentity = {recipe.recipeSpellId, recipe.itemId, 0u, itemGuidCounter};
            result.blocker = "recipe_learned";
            result.schedulingEffect = EconomyAttemptOutcome::Operation;
            return result;
        }

        Item* const item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuidCounter));
        if (!item)
            continue;
        if (item->GetEntry() != recipe.itemId)
        {
            committedRecipes.erase(committed);
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
            result.phase = EconomyPhase::BuyRecipe;
            result.workIdentity = {recipe.recipeSpellId, recipe.itemId, 0u, itemGuidCounter};
            result.blocker = "recipe_item_mismatch";
            result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
            return result;
        }
        if (!StartRecipeLearning(botAI, item, recipe.recipeSpellId))
            continue;

        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.phase = EconomyPhase::BuyRecipe;
        result.workIdentity = {recipe.recipeSpellId, recipe.itemId, 0u, itemGuidCounter};
        result.blocker = "recipe_learning_started";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }
    return std::nullopt;
}

PlayerbotEconomyCycleResult DefaultPlayerbotEconomyRuntime::ExecuteCycle(PlayerbotAI* botAI,
                                                                         PlayerbotCareerPlan const& careerPlan)
{
    Player* const bot = botAI->GetBot();
    uint32 const marketId = AuctionMarketId(bot->GetFaction());
    uint64 const now = GameTime::GetGameTime().count();
    ReconcileCraftTrace(bot, now);
    if (std::optional<PlayerbotEconomyCycleResult> learned = ReconcileRecipeLearning(botAI, now))
        return *learned;
    if (std::optional<PlayerbotEconomyCycleResult> trainerResult = ExecuteTrainerObjective(botAI, careerPlan))
        return *trainerResult;
    ObserveMarketEvidence(botAI, marketId, now);
    if (std::optional<PlayerbotEconomyCycleResult> const reconciled = ReconcileMarketPositionMail(botAI, marketId, now))
    {
        return *reconciled;
    }

    Creature* auctioneer = FindAuctioneer(botAI);
    EconomySnapshot snapshot = BuildSnapshot(botAI, careerPlan);
    ConsumptionSnapshot const consumptionSnapshot = BuildConsumptionSnapshot(botAI, snapshot, marketId, now);
    uint32 excludedItemId = 0u;
    uint32 excludedQuantity = 0u;
    if (activeGathering && activeGathering->plan.itemId)
    {
        excludedItemId = activeGathering->plan.itemId;
        uint32 const current = bot->GetItemCount(excludedItemId);
        excludedQuantity =
            current > activeGathering->plan.startingItemCount
                ? std::min(current - activeGathering->plan.startingItemCount, activeGathering->plan.requestedQuantity)
                : 0u;
        if (activeGathering->coordinatorLeaseId && excludedQuantity > activeGathering->committedQuantity)
        {
            [[maybe_unused]] bool const committed = GetPlayerbotEconomyCoordinator().RecordOutcome(
                activeGathering->coordinatorLeaseId, EconomyAssignmentOutcome::Committed, excludedQuantity, now);
            activeGathering->committedQuantity = excludedQuantity;
        }
        if (activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled &&
            excludedQuantity >= activeGathering->plan.requestedQuantity)
        {
            [[maybe_unused]] bool const completed = GetPlayerbotEconomyCoordinator().RecordOutcome(
                activeGathering->coordinatorLeaseId, EconomyAssignmentOutcome::Completed,
                activeGathering->plan.requestedQuantity, now);
            activeGathering->coordinatorSettled = true;
        }
    }
    RefreshCoordinator(botAI, snapshot, consumptionSnapshot, marketId, now, excludedItemId, excludedQuantity);
    RevalidateCapabilities(botAI, marketId, now);
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyProductionRequest productionRequest{
        .characterGuid = bot->GetGUID().GetCounter(),
        .marketId = marketId,
        .recipes = ProductionRecipes(bot, snapshot, coordinator.Snapshot(now), marketId),
        .expiresAt = ProductionLeaseExpiry(careerPlan, now),
    };
    EconomyAssignmentLease const productionLease = AssignProduction(coordinator, std::move(productionRequest), now);
    std::optional<EconomyAssignment> const activeProduction = productionLease.assignment;
    if (activeProduction)
    {
        snapshot.preferredRecipeSpellId = activeProduction->recipeSpellId;
        sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, activeProduction->recipeSpellId);
    }
    EconomyDecision const productionDecision = PlayerbotEconomyPolicy::Decide(snapshot);
    if (std::optional<PlayerbotEconomyCycleResult> const capability =
            ReconcileCapabilityGoal(botAI, careerPlan, marketId, now))
    {
        return *capability;
    }
    EconomyMarketSnapshot const marketSnapshot = GetPlayerbotEconomyMarket().Snapshot(now);
    auto const pendingPosition = std::find_if(marketSnapshot.positions.begin(), marketSnapshot.positions.end(),
                                              [bot, marketId](EconomyPosition const& position)
                                              {
                                                  return position.traderGuid == bot->GetGUID().GetCounter() &&
                                                         position.marketId == marketId &&
                                                         position.state == EconomyPositionState::Pending;
                                              });
    if (pendingPosition != marketSnapshot.positions.end())
    {
        if (std::optional<PlayerbotEconomyCycleResult> const pending =
                ManagePendingMarketPosition(botAI, *pendingPosition, marketSnapshot, auctioneer, now))
        {
            return *pending;
        }
    }
    ConsumptionDecision consumptionDecision;
    if (productionDecision.phase != EconomyPhase::CollectAuctionMail)
        consumptionDecision = PlayerbotEconomyConsumption::Decide(consumptionSnapshot);
    std::optional<ExecutionResult> finalUseExecution;
    if (consumptionDecision.action == ConsumptionAction::FinalUse)
    {
        finalUseExecution = ExecuteConsumption(botAI, consumptionDecision, auctioneer);
        if (*finalUseExecution == ExecutionResult::Failed)
            consumptionDecision = {};
    }

    if (consumptionDecision.action != ConsumptionAction::None)
    {
        if (activeGathering)
        {
            if (activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    activeGathering->coordinatorLeaseId,
                    activeGathering->committedQuantity ? EconomyAssignmentOutcome::InventoryReceived
                                                       : EconomyAssignmentOutcome::NeedChanged,
                    activeGathering->committedQuantity, now);
                activeGathering->coordinatorSettled = true;
            }
            if (activeGathering->plan.itemId && activeGathering->committedQuantity)
                pendingGatheredSupply[activeGathering->plan.itemId] += activeGathering->committedQuantity;
            Reset(botAI);
        }
        PlayerbotEconomyCycleResult result;
        result.phase = consumptionDecision.action == ConsumptionAction::Purchase   ? EconomyPhase::BuyFinishedGood
                       : consumptionDecision.action == ConsumptionAction::FinalUse ? EconomyPhase::UseFinishedGood
                                                                                   : EconomyPhase::RecoverFinishedGood;
        result.workIdentity = {0u, consumptionDecision.itemId, consumptionDecision.auctionId,
                               consumptionDecision.itemGuidCounter};

        ExecutionResult const execution = finalUseExecution.has_value()
                                              ? *finalUseExecution
                                              : ExecuteConsumption(botAI, consumptionDecision, auctioneer);
        if (execution == ExecutionResult::Recovery)
        {
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = "obsolete_committed_purchase_recovery";
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            Reset(botAI);
            return result;
        }
        if (execution == ExecutionResult::Scheduled)
            result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
        else if (execution == ExecutionResult::Operation)
            result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        else
        {
            result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
            result.blocker = "finished_good_failed_precondition";
        }
        result.schedulingEffect = execution == ExecutionResult::Failed ? EconomyAttemptOutcome::FailedPrecondition
                                                                       : EconomyAttemptOutcome::Operation;
        return result;
    }

    EconomyDecision const& decision = productionDecision;

    if (decision.phase == EconomyPhase::CollectAuctionMail || decision.phase == EconomyPhase::Craft ||
        decision.phase == EconomyPhase::BuyRecipe || decision.phase == EconomyPhase::SellSurplus)
    {
        if (activeGathering)
        {
            if (activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    activeGathering->coordinatorLeaseId,
                    activeGathering->committedQuantity ? EconomyAssignmentOutcome::InventoryReceived
                                                       : EconomyAssignmentOutcome::NeedChanged,
                    activeGathering->committedQuantity, now);
                activeGathering->coordinatorSettled = true;
            }
            if (activeGathering->plan.itemId && activeGathering->committedQuantity)
                pendingGatheredSupply[activeGathering->plan.itemId] += activeGathering->committedQuantity;
            Reset(botAI);
        }
    }
    else if (std::optional<PlayerbotEconomyCycleResult> const gathering =
                 ExecuteAutonomousGathering(botAI, careerPlan, snapshot, decision, marketId, now))
    {
        return *gathering;
    }

    PlayerbotEconomyCycleResult result;
    result.phase = decision.phase;
    result.workIdentity = {snapshot.preferredRecipeSpellId, decision.itemId, decision.auctionId,
                           decision.itemGuidCounter};

    if (decision.phase == EconomyPhase::None)
    {
        if (std::optional<PlayerbotEconomyCycleResult> const marketMaking =
                ExecuteMarketMaking(botAI, snapshot, auctioneer, marketId, now))
        {
            return *marketMaking;
        }

        if (snapshot.preferredRecipeSpellId && !activeProduction)
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);

        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        if (decision.blocker == EconomyDecisionBlocker::PriceCorridor)
            result.blocker = "price_corridor";
        else if (consumptionDecision.blocker != ConsumptionBlocker::NoOffer)
            result.blocker = PlayerbotEconomyConsumption::BlockerName(consumptionDecision.blocker);
        else
            result.blocker = "no_candidate";
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        Reset(botAI);
        return result;
    }

    if (decision.phase == EconomyPhase::Craft || decision.phase == EconomyPhase::BuyReagent)
    {
        sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, decision.spellId);
        result.workIdentity.spellId = decision.spellId;
    }

    uint32 const craftStartingQuantity =
        decision.phase == EconomyPhase::Craft ? bot->GetItemCount(decision.itemId) : 0u;
    bool const productionCraft = decision.phase == EconomyPhase::Craft && activeProduction &&
                                 activeProduction->recipeSpellId == decision.spellId &&
                                 activeProduction->outputItemId == decision.itemId;
    std::string const craftChain = decision.phase != EconomyPhase::Craft ? std::string{}
                                   : productionCraft ? activeProduction->chainPublicId
                                                     : TraceChainForActor(bot->GetGUID().GetCounter(), now);
    ExecutionResult const execution = ExecuteDecision(botAI, decision, auctioneer);
    if (execution == ExecutionResult::Operation && decision.phase == EconomyPhase::Craft)
    {
        if (!craftChain.empty())
        {
            pendingCraftTrace = PendingCraftTrace{
                .chainPublicId = craftChain,
                .coordinatorLeaseId = productionCraft ? activeProduction->leaseId : 0u,
                .actorGuid = bot->GetGUID().GetCounter(),
                .itemId = decision.itemId,
                .recipeSpellId = decision.spellId,
                .startingQuantity = craftStartingQuantity,
                .startedAt = now,
            };
            ReconcileCraftTrace(bot, now);
        }
        else
        {
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        }
    }

    if (execution == ExecutionResult::Scheduled)
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    else if (execution == ExecutionResult::Operation)
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    else
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "failed_precondition";
    }

    result.schedulingEffect = execution == ExecutionResult::Failed ? EconomyAttemptOutcome::FailedPrecondition
                                                                   : EconomyAttemptOutcome::Operation;
    return result;
}

EconomySnapshot DefaultPlayerbotEconomyRuntime::BuildSnapshot(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    EconomySnapshot snapshot;
    snapshot.guidCounter = bot->GetGUID().GetCounter();
    snapshot.botAccountId = bot->GetSession()->GetAccountId();
    snapshot.freeMoneyForTradeskill =
        AI_VALUE2(uint32, "free money for", static_cast<uint32>(NeedMoneyFor::tradeskill));
    snapshot.preferredRecipeSpellId = sRandomPlayerbotMgr.GetValue(bot, PROFESSION_WORK_ORDER_EVENT);
    std::unordered_set<uint32> const applicableVendorItems = ApplicableUnlimitedGoldVendorItems(bot);
    snapshot.applicableUnlimitedGoldVendorItemIds.assign(applicableVendorItems.begin(), applicableVendorItems.end());
    std::sort(snapshot.applicableUnlimitedGoldVendorItemIds.begin(),
              snapshot.applicableUnlimitedGoldVendorItemIds.end());

    time_t const now = GameTime::GetGameTime().count();
    uint32 const marketId = AuctionMarketId(bot->GetFaction());
    EconomyCoordinatorSnapshot const coordinatorSnapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    auto const coordinatorDemandsOutput = [bot, marketId, &coordinatorSnapshot](uint32 itemId)
    {
        return std::any_of(coordinatorSnapshot.gaps.begin(), coordinatorSnapshot.gaps.end(),
                           [bot, marketId, itemId](EconomyDemandGap const& gap)
                           {
                               return gap.marketId == marketId && gap.HasResidualDemand() &&
                                      ProductionOutputMatchesGroup(bot, itemId, gap.group);
                           });
    };
    snapshot.controlledItemGuids = GetPlayerbotEconomyMarket().ControlledItemGuids(snapshot.guidCounter, marketId);
    std::unordered_set<uint64> const controlledItemGuids(snapshot.controlledItemGuids.begin(),
                                                         snapshot.controlledItemGuids.end());
    std::map<uint32, uint32> controlledInventory;
    for (uint64 itemGuid : snapshot.controlledItemGuids)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuid));
        if (item)
            controlledInventory[item->GetEntry()] += item->GetCount();
    }
    auto const availableInventory = [bot, &controlledInventory](uint32 itemId)
    {
        uint32 const total = bot->GetItemCount(itemId);
        uint32 const controlled = controlledInventory[itemId];
        return total > controlled ? total - controlled : 0u;
    };
    AuctionHouseEntry const* auctionHouseEntry =
        marketId ? AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId(marketId)) : nullptr;
    std::map<uint32, uint32> mailedInputs;
    for (Mail const* mail : bot->GetMails())
    {
        if (!mail || mail->messageType != MAIL_AUCTION || mail->state == MAIL_STATE_DELETED)
            continue;

        snapshot.auctionMail.push_back(
            {mail->messageID, mail->deliver_time <= now, mail->money, static_cast<uint32>(mail->items.size())});
        for (MailItemInfo const& mailedItem : mail->items)
        {
            Item const* item = bot->GetMItem(mailedItem.item_guid);
            if (item && !controlledItemGuids.contains(mailedItem.item_guid))
                mailedInputs[mailedItem.item_template] += item->GetCount();
        }
    }

    std::map<uint32, uint32> inventory;
    std::unordered_set<uint32> craftedOutputs;
    std::unordered_set<uint32> trainingOutputs;
    auto const hasCareerSkill = [bot, &careerPlan](uint16 skillId)
    {
        return (IsPrimaryProfessionSkill(skillId) && bot->HasSkill(skillId)) ||
               std::find(careerPlan.primarySkills.begin(), careerPlan.primarySkills.end(), skillId) !=
                   careerPlan.primarySkills.end() ||
               std::find(careerPlan.secondarySkills.begin(), careerPlan.secondarySkills.end(), skillId) !=
                   careerPlan.secondarySkills.end();
    };
    ListSpellsAction listSpells(botAI);
    for (auto const& [spellId, spellName] : listSpells.GetSpellList())
    {
        (void)spellName;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !bot->HasSpell(spellId) ||
            !PlayerbotEconomyPolicy::IsProfessionRecipeSpell(spellInfo->Effects[EFFECT_0].Effect,
                                                             spellInfo->Effects[EFFECT_0].ItemType,
                                                             spellInfo->ReagentCount[EFFECT_0], spellInfo->SchoolMask))
        {
            continue;
        }

        SkillLineAbilityMapBounds const skillBounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        bool careerRecipe = false;
        for (auto ability = skillBounds.first; ability != skillBounds.second; ++ability)
        {
            SkillLineAbilityEntry const* skill = ability->second;
            careerRecipe |= skill && hasCareerSkill(static_cast<uint16>(skill->SkillLine));
        }
        if (!careerRecipe)
            continue;

        RecipeCandidate recipe;
        recipe.spellId = spellId;
        recipe.craftedItemId = spellInfo->Effects[EFFECT_0].ItemType;
        craftedOutputs.insert(recipe.craftedItemId);
        recipe.givesSkillUp = ItemUsageValue::SpellGivesSkillUp(spellId, bot);
        if (recipe.givesSkillUp)
            trainingOutputs.insert(recipe.craftedItemId);
        ItemUsage const outputUsage = AI_VALUE2(ItemUsage, "item usage", recipe.craftedItemId);
        recipe.outputUsagePriority = CraftOutputPriority(outputUsage);

        bool hasAllReagents = true;
        for (uint8 index = 0; index < MAX_SPELL_REAGENTS; ++index)
        {
            if (spellInfo->Reagent[index] <= 0 || spellInfo->ReagentCount[index] <= 0)
                continue;

            uint32 const itemId = static_cast<uint32>(spellInfo->Reagent[index]);
            uint32 const count = static_cast<uint32>(spellInfo->ReagentCount[index]);
            recipe.reagents.push_back({itemId, count, applicableVendorItems.contains(itemId)});
            uint32 const inventoryCount = availableInventory(itemId);
            inventory[itemId] = inventoryCount;
            hasAllReagents = hasAllReagents && inventoryCount >= count;
        }

        if (recipe.reagents.empty() || (!recipe.givesSkillUp && !IsUsefulCraftOutput(outputUsage) &&
                                        !coordinatorDemandsOutput(recipe.craftedItemId)))
            continue;

        if (hasAllReagents && !botAI->CanCastSpell(spellId, bot, true))
            continue;

        snapshot.recipes.push_back(std::move(recipe));
    }

    for (auto const& [itemId, count] : inventory)
        snapshot.inventory.push_back({itemId, count, mailedInputs[itemId]});

    bool const capabilityMarketEligible =
        careerPlan.capabilityGoal.has_value() || !LearnedPrimaryCapabilitySkillIds(bot).empty();
    AuctionHouseObject* auctionHouse = careerPlan.marketEligible || capabilityMarketEligible
                                           ? sAuctionMgr->GetAuctionsMap(bot->GetFaction())
                                           : nullptr;
    if (auctionHouse)
    {
        for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
        {
            if (!auction || !sAuctionMgr->GetAItem(auction->item_guid))
                continue;

            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(auction->item_template);
            if (!itemTemplate)
                continue;

            PlayerbotRecipeCandidate const recipe = PlayerbotCareer::DescribeRecipe(itemTemplate, bot, 0u);

            AuctionListingCandidate listing{
                auctionId,
                sCharacterCache->GetCharacterAccountIdByGuid(auction->owner),
                auction->item_template,
                auction->itemCount,
                auction->buyout,
                static_cast<uint32>(std::max(0, itemTemplate->BuyPrice)),
                itemTemplate->GetMaxStackSize() * 2u,
                PlayerbotCareer::IsRecipeAcquisitionAllowed(careerPlan, recipe, PlayerbotRecipeSource::AuctionHouse)};
            std::optional<EconomyReferencePrice> const reference =
                GetPlayerbotEconomyMarket().ReferencePrice(marketId, ReagentGroup(auction->item_template), now);
            listing.buyerCeilingPerItem = reference.has_value() ? reference->unitPrice : listing.templateBuyPrice;
            listing.recipeSpellId = recipe.recipeSpellId;
            snapshot.auctions.push_back(std::move(listing));
        }
    }

    std::vector<Item*> const saleItems =
        AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(ITEM_USAGE_AH));
    auto const isGatheringSkill = [](uint16 skillId)
    { return skillId == SKILL_HERBALISM || skillId == SKILL_MINING || skillId == SKILL_SKINNING; };
    std::vector<uint16> const learnedPrimarySkills = LearnedPrimaryCapabilitySkillIds(bot);
    bool const pureGatheringCareer =
        !learnedPrimarySkills.empty() &&
        std::all_of(learnedPrimarySkills.begin(), learnedPrimarySkills.end(), isGatheringSkill);
    for (Item* item : saleItems)
    {
        if (!item || controlledItemGuids.contains(item->GetGUID().GetCounter()))
            continue;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        bool const circulationMaterial =
            PlayerbotEconomyPolicy::IsCirculationMaterial(itemTemplate->Class, itemTemplate->SubClass);
        bool const gatheringMaterial =
            itemTemplate->Class == ITEM_CLASS_TRADE_GOODS &&
            ((itemTemplate->SubClass == ITEM_SUBCLASS_HERB && hasCareerSkill(SKILL_HERBALISM)) ||
             (itemTemplate->SubClass == ITEM_SUBCLASS_METAL_STONE && hasCareerSkill(SKILL_MINING)) ||
             (itemTemplate->SubClass == ITEM_SUBCLASS_LEATHER && hasCareerSkill(SKILL_SKINNING)));
        bool const professionReagent = inventory.find(item->GetEntry()) != inventory.end();
        bool const professionOutput = craftedOutputs.contains(item->GetEntry());
        bool const professionRelated = circulationMaterial || professionReagent || professionOutput;
        if (!professionRelated)
            continue;

        uint64 const marketBuyout = LowestCompetingBuyoutPerItem(auctionHouse, item->GetEntry(), snapshot.botAccountId);
        uint64 const perItemBuyout =
            marketBuyout ? marketBuyout : static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        if (perItemBuyout * item->GetCount() > MAX_MONEY_AMOUNT)
            continue;

        uint64 allocatedInputCost = 0u;
        for (RecipeCandidate const& recipe : snapshot.recipes)
        {
            if (recipe.craftedItemId != item->GetEntry())
                continue;
            for (ReagentRequirement const& reagent : recipe.reagents)
            {
                std::optional<EconomyReferencePrice> const inputReference =
                    GetPlayerbotEconomyMarket().ReferencePrice(marketId, ReagentGroup(reagent.itemId), now);
                ItemTemplate const* inputTemplate = sObjectMgr->GetItemTemplate(reagent.itemId);
                uint64 const unitCost =
                    inputReference.has_value()
                        ? inputReference->unitPrice
                        : (inputTemplate ? static_cast<uint32>(std::max(0, inputTemplate->BuyPrice)) : 0u);
                allocatedInputCost += unitCost * reagent.count;
            }
            break;
        }

        std::string const group =
            gatheringMaterial || professionReagent ? ReagentGroup(item->GetEntry()) : ItemGroup(item->GetEntry());
        std::optional<EconomyReferencePrice> const outputReference =
            GetPlayerbotEconomyMarket().ReferencePrice(marketId, group, now);

        SaleItemCandidate sale;
        sale.itemGuidCounter = item->GetGUID().GetCounter();
        sale.itemId = item->GetEntry();
        sale.count = item->GetCount();
        sale.usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
        sale.canBeTraded = item->CanBeTraded();
        sale.bound = item->IsSoulBound();
        sale.container = item->IsBag();
        sale.containerItemCount = item->IsNotEmptyBag() ? 1u : 0u;
        sale.conjured = itemTemplate->HasFlag(ITEM_FLAG_CONJURED);
        sale.duration = item->GetUInt32Value(ITEM_FIELD_DURATION);
        sale.alreadyAuctioned = sAuctionMgr->GetAItem(item->GetGUID()) != nullptr;
        sale.templateBuyPrice = static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        sale.templateSellPrice = itemTemplate->SellPrice;
        sale.lowestCompetingBuyoutPerItem = marketBuyout;
        sale.inventoryCount = bot->GetItemCount(item->GetEntry());
        uint64 const configuredReserve =
            static_cast<uint64>(itemTemplate->GetMaxStackSize()) * sPlayerbotEconomyConfig.professionReserveStacks;
        sale.professionReserveFloor =
            professionReagent
                ? PlayerbotEconomyPolicy::ProductionReserve(
                      snapshot, item->GetEntry(),
                      static_cast<uint32>(std::min<uint64>(configuredReserve, std::numeric_limits<uint32>::max())))
                : 0u;
        sale.professionRelated = professionRelated;
        sale.allocatedInputCost = allocatedInputCost;
        sale.deposit = auctionHouseEntry ? AuctionHouseMgr::GetAuctionDeposit(auctionHouseEntry, MIN_AUCTION_TIME, item,
                                                                              item->GetCount())
                                         : 0u;
        sale.auctionCutBasisPoints =
            auctionHouseEntry
                ? static_cast<uint32>(auctionHouseEntry->cutPercent * sWorld->getRate(RATE_AUCTION_CUT) * 100.0f)
                : 0u;
        sale.buyerCeilingPerItem = outputReference.has_value() ? outputReference->unitPrice
                                                               : (marketBuyout ? marketBuyout : sale.templateBuyPrice);
        sale.pureGatheringMaterial = gatheringMaterial && pureGatheringCareer;
        sale.ordinaryVendorSupply = applicableVendorItems.contains(item->GetEntry());
        sale.trainingOutput = trainingOutputs.contains(item->GetEntry());
        sale.independentDemand = coordinatorDemandsOutput(item->GetEntry());
        snapshot.saleItems.push_back(std::move(sale));
    }

    return snapshot;
}

ConsumptionSnapshot DefaultPlayerbotEconomyRuntime::BuildConsumptionSnapshot(PlayerbotAI* botAI,
                                                                             EconomySnapshot const& economy,
                                                                             uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    ConsumptionSnapshot snapshot;
    snapshot.botAccountId = bot->GetSession()->GetAccountId();
    std::unordered_set<uint64> const controlledItemGuids(economy.controlledItemGuids.begin(),
                                                         economy.controlledItemGuids.end());

    std::map<EconomySubstitutionGroup, ConsumptionNeed> needs;
    std::map<EconomySubstitutionGroup, uint32> inventorySupply;
    std::map<EconomySubstitutionGroup, uint32> mailSupply;

    auto const budgetFor = [context](EconomySubstitutionKind kind)
    {
        NeedMoneyFor lane = NeedMoneyFor::gear;
        if (kind == EconomySubstitutionKind::Ammunition)
            lane = NeedMoneyFor::ammo;
        else if (kind == EconomySubstitutionKind::Consumable || kind == EconomySubstitutionKind::Enhancement)
        {
            lane = NeedMoneyFor::consumables;
        }
        return static_cast<uint64>(AI_VALUE2(uint32, "free money for", static_cast<uint32>(lane)));
    };

    auto const restorationUtility = [](uint32 maximum, uint32 triggerPercent)
    {
        uint32 const boundedPercent = std::min(triggerPercent, 100u);
        return static_cast<uint32>(static_cast<uint64>(maximum) * (100u - boundedPercent) / 100u);
    };
    auto const addConsumableNeed = [&](ConsumableCapability capability, uint32 requiredUtility)
    {
        ConsumptionNeed need = PlayerbotEconomyConsumption::BuildNeed(
            {capability, requiredUtility, 1u, true, budgetFor(EconomySubstitutionKind::Consumable)});
        needs.emplace(need.group, std::move(need));
    };
    if (botAI->HasStrategy("food", BOT_STATE_NON_COMBAT))
    {
        addConsumableNeed(ConsumableCapability::Food,
                          restorationUtility(bot->GetMaxHealth(), sPlayerbotAIConfig.lowHealth));
        if (uint32 const maximumMana = bot->GetMaxPower(POWER_MANA))
        {
            addConsumableNeed(ConsumableCapability::Drink, restorationUtility(maximumMana, sPlayerbotAIConfig.lowMana));
        }
    }
    if (botAI->HasStrategy("potions", BOT_STATE_COMBAT))
    {
        addConsumableNeed(ConsumableCapability::HealthRestoration,
                          restorationUtility(bot->GetMaxHealth(), sPlayerbotAIConfig.criticalHealth));
        if (uint32 const maximumMana = bot->GetMaxPower(POWER_MANA))
        {
            addConsumableNeed(ConsumableCapability::ManaRestoration,
                              restorationUtility(maximumMana, sPlayerbotAIConfig.mediumMana));
        }
    }

    auto const ensureNeed = [&](FinishedGoodDescription const& description,
                                ItemTemplate const* itemTemplate) -> ConsumptionNeed&
    {
        ConsumptionNeed& need = needs[description.group];
        if (!need.quantity)
        {
            need.group = description.group;
            need.use = description.use;
            need.quantity = description.group.kind == EconomySubstitutionKind::Ammunition ||
                                    description.group.kind == EconomySubstitutionKind::Consumable
                                ? std::max<uint32>(1u, itemTemplate->GetMaxStackSize())
                                : 1u;
            need.requiredUtility = description.utility;
            need.compatibleActivity = true;
            need.remainingUses = need.quantity;
            need.protectedBudget = budgetFor(description.group.kind);
        }
        else
            need.requiredUtility = std::min(need.requiredUtility, description.utility);

        std::optional<EconomyReferencePrice> const reference = GetPlayerbotEconomyMarket().ReferencePrice(
            marketId, PlayerbotEconomyConsumption::GroupKey(description.group), now);
        uint64 const templateCeiling = static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        need.buyerCeilingPerItem =
            std::max(need.buyerCeilingPerItem, reference.has_value() ? reference->unitPrice : templateCeiling);
        return need;
    };

    auto const findNeed = [&needs](FinishedGoodDescription const& description) -> ConsumptionNeed*
    {
        auto const found = std::find_if(
            needs.begin(), needs.end(), [&description](auto const& entry)
            { return PlayerbotEconomyConsumption::MatchesNeed(entry.second, description.group, description.utility); });
        return found == needs.end() ? nullptr : &found->second;
    };
    auto const updatePrice = [marketId, now](ConsumptionNeed& need, ItemTemplate const* itemTemplate)
    {
        std::optional<EconomyReferencePrice> const reference = GetPlayerbotEconomyMarket().ReferencePrice(
            marketId, PlayerbotEconomyConsumption::GroupKey(need.group), now);
        uint64 const templateCeiling = static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        need.buyerCeilingPerItem =
            std::max(need.buyerCeilingPerItem, reference.has_value() ? reference->unitPrice : templateCeiling);
    };

    std::unordered_set<uint64> visitedItems;
    auto const inspectInventoryItem = [&](Item* item)
    {
        if (!item || controlledItemGuids.contains(item->GetGUID().GetCounter()) ||
            !visitedItems.insert(item->GetGUID().GetCounter()).second)
            return;

        std::optional<FinishedGoodDescription> const description =
            PlayerbotEconomyConsumption::Describe(bot, item->GetTemplate());
        if (!description.has_value())
            return;

        snapshot.held.push_back({description->group, item->GetEntry(), item->GetCount(), EconomySupplySource::Inventory,
                                 description->utility});
        std::string const qualifier =
            std::to_string(item->GetEntry()) + "," + std::to_string(item->GetItemRandomPropertyId());
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", qualifier);
        bool const compatible = IsFinishedGoodUsage(usage) && bot->CanUseItem(item) == EQUIP_ERR_OK;
        ConsumptionNeed* need = nullptr;
        if (description->group.kind == EconomySubstitutionKind::Consumable)
            need = findNeed(*description);
        else if (compatible)
            need = &ensureNeed(*description, item->GetTemplate());
        if (!compatible || !need)
            return;

        inventorySupply[need->group] += item->GetCount();
        snapshot.owned.push_back({description->group, item->GetGUID().GetCounter(), item->GetEntry(), item->GetCount(),
                                  description->utility, true});
    };

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        inspectInventoryItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = static_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            inspectInventoryItem(bag->GetItemByPos(slot));
    }

    for (Mail const* mail : bot->GetMails())
    {
        if (!mail || mail->messageType != MAIL_AUCTION || mail->state == MAIL_STATE_DELETED)
            continue;
        for (MailItemInfo const& mailedItem : mail->items)
        {
            Item const* item = bot->GetMItem(mailedItem.item_guid);
            if (!item || controlledItemGuids.contains(mailedItem.item_guid))
                continue;
            std::optional<FinishedGoodDescription> const description =
                PlayerbotEconomyConsumption::Describe(bot, item->GetTemplate());
            if (description.has_value())
            {
                snapshot.held.push_back({description->group, item->GetEntry(), item->GetCount(),
                                         EconomySupplySource::Mail, description->utility});
                ConsumptionNeed* need = findNeed(*description);
                if (need && bot->CanUseItem(item->GetTemplate()) == EQUIP_ERR_OK)
                    mailSupply[need->group] += item->GetCount();
            }
        }
    }

    for (AuctionListingCandidate const& listing : economy.auctions)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(listing.itemId);
        std::optional<FinishedGoodDescription> const description =
            PlayerbotEconomyConsumption::Describe(bot, itemTemplate);
        if (!description.has_value() || !itemTemplate)
            continue;

        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", listing.itemId);
        bool const equipment = itemTemplate->Class == ITEM_CLASS_ARMOR || itemTemplate->Class == ITEM_CLASS_WEAPON;
        if ((equipment &&
             !PlayerbotEconomyConsumption::IsMarketEquipment(itemTemplate->Class, itemTemplate->Quality, usage)) ||
            (!equipment && !IsFinishedGoodUsage(usage)))
            continue;

        ConsumptionNeed* need = description->group.kind == EconomySubstitutionKind::Consumable
                                    ? findNeed(*description)
                                    : &ensureNeed(*description, itemTemplate);
        if (!need)
            continue;
        if (description->group.kind == EconomySubstitutionKind::Consumable)
            updatePrice(*need, itemTemplate);
        snapshot.offers.push_back({description->group, listing.auctionId, listing.ownerAccountId, listing.itemId,
                                   listing.count, listing.buyout, description->utility,
                                   bot->CanUseItem(itemTemplate) == EQUIP_ERR_OK});
    }

    for (auto const& [itemGuid, committed] : committedFinishedGoods)
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Item>(itemGuid);
        Item* item = bot->GetItemByGuid(guid);
        Item const* physicalItem = item ? item : bot->GetMItem(static_cast<ObjectGuid::LowType>(itemGuid));
        if (!physicalItem)
            continue;

        ConsumptionNeed& need = needs[committed.group];
        if (!need.quantity)
        {
            need.group = committed.group;
            need.use = committed.use;
            need.quantity = committed.quantity;
            need.compatibleActivity = true;
            need.remainingUses = committed.quantity;
            need.protectedBudget = budgetFor(committed.group.kind);
        }
        need.committedPurchaseQuantity += committed.quantity;
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", committed.itemId);
        need.committedPurchaseStillUseful = IsFinishedGoodUsage(usage);
    }

    for (auto& [group, need] : needs)
    {
        if (group.kind != EconomySubstitutionKind::Consumable)
            continue;
        need.ordinaryVendorSupply = std::any_of(
            economy.applicableUnlimitedGoldVendorItemIds.begin(), economy.applicableUnlimitedGoldVendorItemIds.end(),
            [bot, &need](uint32 itemId)
            {
                std::optional<FinishedGoodDescription> const description =
                    PlayerbotEconomyConsumption::Describe(bot, sObjectMgr->GetItemTemplate(itemId));
                return description &&
                       PlayerbotEconomyConsumption::MatchesNeed(need, description->group, description->utility);
            });
    }

    for (auto& [group, need] : needs)
    {
        need.inventoryQuantity = inventorySupply[group];
        need.mailQuantity = mailSupply[group];
        snapshot.needs.push_back(std::move(need));
    }
    return snapshot;
}

Creature* DefaultPlayerbotEconomyRuntime::FindAuctioneer(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    GuidVector const npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        if (Creature* auctioneer = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))
            return auctioneer;
    }

    return nullptr;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::ExecuteDecision(PlayerbotAI* botAI, EconomyDecision const& decision,
                                                                Creature* auctioneer)
{
    switch (decision.phase)
    {
        case EconomyPhase::CollectAuctionMail:
            return CollectAuctionMail(botAI);
        case EconomyPhase::Craft:
        {
            Player* const bot = botAI->GetBot();
            bool const cast =
                botAI->CanCastSpell(decision.spellId, bot, true) && botAI->CastSpell(decision.spellId, bot);
            if (cast)
                Reset(botAI);
            return cast ? ExecutionResult::Operation : ExecutionResult::Failed;
        }
        case EconomyPhase::BuyReagent:
        case EconomyPhase::BuyRecipe:
            return BuyReagent(botAI, decision, auctioneer);
        case EconomyPhase::BuyFinishedGood:
        case EconomyPhase::UseFinishedGood:
        case EconomyPhase::RecoverFinishedGood:
            return ExecutionResult::Failed;
        case EconomyPhase::SellSurplus:
            return SellSurplus(botAI, decision, auctioneer);
        case EconomyPhase::None:
            return ExecutionResult::Failed;
    }

    return ExecutionResult::Failed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::ExecuteConsumption(PlayerbotAI* botAI,
                                                                   ConsumptionDecision const& decision,
                                                                   Creature* auctioneer)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    if (decision.action == ConsumptionAction::Recovery)
    {
        auto const committed =
            std::find_if(committedFinishedGoods.begin(), committedFinishedGoods.end(),
                         [&decision](auto const& entry) { return entry.second.group == decision.group; });
        if (committed != committedFinishedGoods.end())
            committedFinishedGoods.erase(committed);
        return ExecutionResult::Recovery;
    }

    if (decision.action == ConsumptionAction::Purchase)
    {
        if (!auctioneer)
            return TravelToAuctionHouse(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

        AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
        AuctionEntry* auction = auctionHouse ? auctionHouse->GetAuction(decision.auctionId) : nullptr;
        if (!auction || auction->item_template != decision.itemId || auction->itemCount != decision.count ||
            auction->buyout != decision.buyout)
        {
            return ExecutionResult::Failed;
        }

        uint64 const itemGuid = auction->item_guid.GetCounter();
        EconomyDecision transaction;
        transaction.phase = EconomyPhase::BuyFinishedGood;
        transaction.itemId = decision.itemId;
        transaction.auctionId = decision.auctionId;
        transaction.count = decision.count;
        transaction.buyout = decision.buyout;
        transaction.purchases.push_back({decision.auctionId, decision.itemId, decision.count, decision.buyout});
        ExecutionResult const result =
            BuyReagent(botAI, transaction, auctioneer, EconomyClaimPriority::Consumer, decision.group);
        if (result == ExecutionResult::Operation)
        {
            std::optional<EconomyTraceEvent> const purchased =
                TraceEventForAuction(bot->GetGUID().GetCounter(), decision.auctionId, EconomyTraceKind::Purchased);
            committedFinishedGoods[itemGuid] = {
                decision.group,
                decision.use,
                decision.itemId,
                decision.count,
                purchased ? purchased->chainPublicId
                          : TraceChainForActor(bot->GetGUID().GetCounter(), GameTime::GetGameTime().count()),
                purchased ? purchased->counterpartyGuid : 0u,
            };
        }
        return result;
    }

    if (decision.action != ConsumptionAction::FinalUse || !decision.itemGuidCounter)
        return ExecutionResult::Failed;

    ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(decision.itemGuidCounter);
    Item* item = bot->GetItemByGuid(itemGuid);
    if (!item || item->GetEntry() != decision.itemId || item->GetCount() < decision.count ||
        bot->CanUseItem(item) != EQUIP_ERR_OK)
    {
        return ExecutionResult::Failed;
    }

    std::optional<FinishedGoodDescription> const description =
        PlayerbotEconomyConsumption::Describe(bot, item->GetTemplate());
    std::string const qualifier =
        std::to_string(item->GetEntry()) + "," + std::to_string(item->GetItemRandomPropertyId());
    if (!description.has_value() || description->group != decision.group || description->use != decision.use ||
        !IsFinishedGoodUsage(AI_VALUE2(ItemUsage, "item usage", qualifier)))
    {
        return ExecutionResult::Failed;
    }

    bool used = false;
    if (decision.use == FinishedGoodUse::Equip || decision.use == FinishedGoodUse::SetAmmunition)
    {
        EquipAction(botAI, "economy equip").EquipItems({decision.itemId});
        if (decision.use == FinishedGoodUse::SetAmmunition)
            used = bot->GetUInt32Value(PLAYER_AMMO_ID) == decision.itemId;
        else
        {
            Item* equipped = bot->GetItemByGuid(itemGuid);
            used = equipped && equipped->GetBagSlot() == INVENTORY_SLOT_BAG_0 &&
                   equipped->GetSlot() < INVENTORY_SLOT_ITEM_START;
        }
    }
    else if (decision.use == FinishedGoodUse::Consume || decision.use == FinishedGoodUse::Apply)
        used = EconomyUseItemAction(botAI).Apply(item);

    if (!used)
        return ExecutionResult::Failed;

    auto const committed = committedFinishedGoods.find(decision.itemGuidCounter);
    std::string const chainPublicId =
        committed != committedFinishedGoods.end()
            ? committed->second.chainPublicId
            : TraceChainForActor(bot->GetGUID().GetCounter(), GameTime::GetGameTime().count());
    if (!chainPublicId.empty())
    {
        [[maybe_unused]] bool const recorded =
            PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                .Complete(true,
                          {
                              .deduplicationKey = Acore::StringFormat("final-use:{}:{}", decision.itemGuidCounter,
                                                                      static_cast<uint32>(decision.use)),
                              .chainPublicId = chainPublicId,
                              .actorGuid = bot->GetGUID().GetCounter(),
                              .counterpartyGuid =
                                  committed != committedFinishedGoods.end() ? committed->second.counterpartyGuid : 0u,
                              .itemId = decision.itemId,
                              .quantity = decision.count,
                              .occurredAt = GameTime::GetGameTime().count(),
                              .kind = EconomyTraceKind::FinalUse,
                              .finalUse = TraceFinalUse(decision.use),
                          });
    }
    committedFinishedGoods.erase(decision.itemGuidCounter);
    return ExecutionResult::Operation;
}

bool HasAuctionMailBagSpace(Player* bot)
{
    uint32 used = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++used;

    uint32 free = 16u - used;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag const* container = static_cast<Bag const*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag));
        if (!container)
            continue;
        ItemTemplate const* bagTemplate = container->GetTemplate();
        if (bagTemplate->Class == ITEM_CLASS_CONTAINER && bagTemplate->SubClass == ITEM_SUBCLASS_CONTAINER)
            free += container->GetFreeSlots();
    }
    return free >= 2u;
}

void DeleteCollectedMail(Player* bot, ObjectGuid mailbox, uint32 mailId)
{
    WorldPacket packet;
    packet << mailbox;
    packet << mailId;
    packet << uint32(0);
    bot->GetSession()->HandleMailDelete(packet);
}

bool CollectOneAuctionMail(Player* bot, ObjectGuid mailbox, uint32 mailId)
{
    Mail* mail = bot->GetMail(mailId);
    if (!mail || mail->messageType != MAIL_AUCTION)
        return false;

    bool processed = false;
    if (mail->money)
    {
        WorldPacket packet;
        packet << mailbox;
        packet << mailId;
        bot->GetSession()->HandleMailTakeMoney(packet);
        mail = bot->GetMail(mailId);
        processed = mail && mail->money == 0;
    }

    mail = bot->GetMail(mailId);
    if (mail && !mail->items.empty())
    {
        if (!HasAuctionMailBagSpace(bot))
            return processed;

        std::vector<uint32> itemGuids;
        for (MailItemInfo const& item : mail->items)
            if (sObjectMgr->GetItemTemplate(item.item_template))
                itemGuids.push_back(item.item_guid);

        for (uint32 itemGuid : itemGuids)
        {
            mail = bot->GetMail(mailId);
            if (!mail)
                break;
            auto attachment = std::find_if(mail->items.begin(), mail->items.end(),
                                           [itemGuid](MailItemInfo const& item) { return item.item_guid == itemGuid; });
            if (attachment == mail->items.end() || !bot->GetMItem(itemGuid))
                continue;

            WorldPacket packet;
            packet << mailbox;
            packet << mailId;
            packet << itemGuid;
            bot->GetSession()->HandleMailTakeItem(packet);

            mail = bot->GetMail(mailId);
            processed |=
                mail && std::none_of(mail->items.begin(), mail->items.end(),
                                     [itemGuid](MailItemInfo const& item) { return item.item_guid == itemGuid; });
        }
    }

    mail = bot->GetMail(mailId);
    if (mail && PlayerbotEconomyMailIsFullyCollected(mail->money, mail->items.size()))
        DeleteCollectedMail(bot, mailbox, mailId);
    return processed;
}

bool CollectAvailableAuctionMail(PlayerbotAI* botAI, ObjectGuid mailbox)
{
    Player* bot = botAI->GetBot();
    WorldPacket packet;
    packet << mailbox;
    bot->GetSession()->HandleGetMailList(packet);

    time_t const now = GameTime::GetGameTime().count();
    std::vector<uint32> mailIds;
    for (Mail const* mail : bot->GetMails())
        if (mail && mail->messageType == MAIL_AUCTION && mail->state != MAIL_STATE_DELETED && mail->deliver_time <= now)
            mailIds.push_back(mail->messageID);

    bool processed = false;
    for (uint32 mailId : mailIds)
        processed = CollectOneAuctionMail(bot, mailbox, mailId) || processed;
    return processed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::CollectAuctionMail(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    ObjectGuid const mailbox = MailProcessor::FindMailbox(botAI);
    if (!mailbox)
        return TravelToMailbox(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    if (!botAI->GetGameObject(mailbox))
        return TravelToMailbox(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    if (!bot->GetGameObjectIfCanInteractWith(mailbox, GAMEOBJECT_TYPE_MAILBOX))
        return TravelToMailbox(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    struct TraceableMail
    {
        uint32 mailId = 0;
        uint64 money = 0;
        AuctionMailDetails details;
    };
    uint64 const now = GameTime::GetGameTime().count();
    std::vector<TraceableMail> traceable;
    for (Mail const* mail : bot->GetMails())
    {
        std::optional<AuctionMailDetails> const details = ParseAuctionMail(mail);
        if (!mail || !details || mail->state == MAIL_STATE_DELETED || mail->deliver_time > static_cast<time_t>(now))
        {
            continue;
        }
        if (details->response == AUCTION_WON || details->response == AUCTION_SUCCESSFUL ||
            details->response == AUCTION_EXPIRED)
            traceable.push_back({mail->messageID, mail->money, *details});
    }
    std::vector<EconomyTraceRecord> traceRecords;
    traceRecords.reserve(traceable.size());
    for (TraceableMail const& mail : traceable)
    {
        EconomyTraceKind const priorKind =
            mail.details.response == AUCTION_WON ? EconomyTraceKind::Purchased : EconomyTraceKind::Listed;
        std::optional<EconomyTraceEvent> const prior =
            TraceEventForAuction(bot->GetGUID().GetCounter(), mail.details.auctionId, priorKind);
        if (!prior)
            continue;
        EconomyTraceRecord record;
        std::string_view const outcome = mail.details.response == AUCTION_WON          ? "delivered"
                                         : mail.details.response == AUCTION_SUCCESSFUL ? "settled"
                                                                                       : "expired";
        record.deduplicationKey = Acore::StringFormat("mail:{}:{}", mail.mailId, outcome);
        record.chainPublicId = prior->chainPublicId;
        record.actorGuid = bot->GetGUID().GetCounter();
        record.counterpartyGuid = mail.details.response == AUCTION_SUCCESSFUL
                                      ? TraceActorIfKnown(mail.details.bidderGuid, now)
                                      : prior->counterpartyGuid;
        record.itemId = mail.details.itemId;
        record.quantity = mail.details.quantity;
        record.unitPriceCopper = mail.details.response != AUCTION_EXPIRED && mail.details.quantity
                                     ? (mail.details.bid + mail.details.quantity - 1u) / mail.details.quantity
                                     : 0u;
        record.depositCopper = mail.details.deposit;
        record.auctionCutCopper = mail.details.cut;
        record.proceedsCopper = mail.money;
        record.referenceUnitPriceCopper = prior->referenceUnitPriceCopper;
        record.competingUnitPriceCopper = prior->competingUnitPriceCopper;
        record.occurredAt = now;
        record.correlationAuctionId = mail.details.auctionId;
        record.correlationMailId = mail.mailId;
        record.kind = mail.details.response == AUCTION_WON          ? EconomyTraceKind::Delivered
                      : mail.details.response == AUCTION_SUCCESSFUL ? EconomyTraceKind::SaleSettled
                                                                    : EconomyTraceKind::Expired;
        traceRecords.push_back(std::move(record));
    }
    bool const collected = CollectAvailableAuctionMail(botAI, mailbox);
    [[maybe_unused]] std::size_t const recorded =
        PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace()).CompleteMailScan(collected, std::move(traceRecords));
    if (collected)
    {
        Reset(botAI);
    }
    return collected ? ExecutionResult::Operation : ExecutionResult::Failed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::BuyReagent(PlayerbotAI* botAI, EconomyDecision const& decision,
                                                           Creature* auctioneer, EconomyClaimPriority priority,
                                                           std::optional<EconomySubstitutionGroup> claimGroup)
{
    Player* const bot = botAI->GetBot();
    if (!auctioneer)
        return TravelToAuctionHouse(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse)
        return ExecutionResult::Failed;

    std::vector<EconomyDecision::AuctionPurchase> purchases = decision.purchases;
    if (purchases.empty())
        purchases.push_back({decision.auctionId, decision.itemId, decision.count, decision.buyout});

    bool boughtAny = false;
    for (EconomyDecision::AuctionPurchase const& purchase : purchases)
    {
        AuctionEntry* auction = auctionHouse->GetAuction(purchase.auctionId);
        if (!auction || auction->item_template != purchase.itemId || auction->itemCount != purchase.count ||
            auction->buyout != purchase.buyout || auction->buyout == 0 ||
            sCharacterCache->GetCharacterAccountIdByGuid(auction->owner) == bot->GetSession()->GetAccountId())
        {
            return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
        }
        uint32 const sellerGuid = auction->owner.GetCounter();
        uint64 const itemGuidCounter = auction->item_guid.GetCounter();

        std::optional<EconomyAssignment> assignment;
        if (decision.phase == EconomyPhase::BuyReagent || decision.phase == EconomyPhase::BuyFinishedGood)
        {
            EconomyAssignmentRequest request;
            request.characterGuid = bot->GetGUID().GetCounter();
            request.marketId = AuctionMarketId(bot->GetFaction());
            request.group = claimGroup.value_or(EconomySubstitutionGroup::ExactReagent(purchase.itemId));
            request.quantity = purchase.count;
            request.kind = EconomyClaimKind::Purchase;
            request.priority = priority;
            request.workKind = EconomyWorkKind::Buy;
            request.workIdentity = "auction:" + std::to_string(purchase.auctionId);
            request.sellerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
            request.expiresAt = GameTime::GetGameTime().count() + 1u;
            EconomyAssignmentLease const lease =
                GetPlayerbotEconomyCoordinator().Lease(std::move(request), GameTime::GetGameTime().count());
            if (!lease.assignment)
                return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
            assignment = lease.assignment;
        }

        WorldPacket packet;
        packet << auctioneer->GetGUID();
        packet << purchase.auctionId;
        packet << static_cast<uint32>(purchase.buyout);
        bot->GetSession()->HandleAuctionPlaceBid(packet);

        if (auctionHouse->GetAuction(purchase.auctionId))
        {
            if (assignment)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    assignment->leaseId, EconomyAssignmentOutcome::FailedPurchase, 0u, GameTime::GetGameTime().count());
            }
            return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
        }
        if (assignment)
        {
            [[maybe_unused]] bool const completed =
                GetPlayerbotEconomyCoordinator().RecordOutcome(assignment->leaseId, EconomyAssignmentOutcome::Completed,
                                                               purchase.count, GameTime::GetGameTime().count());
        }
        uint64 const now = GameTime::GetGameTime().count();
        std::string const chainPublicId =
            assignment ? assignment->chainPublicId : TraceChainForActor(bot->GetGUID().GetCounter(), now);
        if (decision.phase == EconomyPhase::BuyRecipe && decision.recipeSpellId)
        {
            committedRecipes[itemGuidCounter] = {
                purchase.itemId,
                decision.recipeSpellId,
                chainPublicId,
                TraceActorIfKnown(sellerGuid, now),
            };
        }
        if (!chainPublicId.empty())
        {
            std::optional<EconomyReferencePrice> const reference = GetPlayerbotEconomyMarket().ReferencePrice(
                AuctionMarketId(bot->GetFaction()), ReagentGroup(purchase.itemId), now);
            [[maybe_unused]] bool const recorded =
                PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                    .Complete(true,
                              {
                                  .deduplicationKey = Acore::StringFormat(
                                      "purchase:{}:{}", AuctionMarketId(bot->GetFaction()), purchase.auctionId),
                                  .chainPublicId = chainPublicId,
                                  .actorGuid = bot->GetGUID().GetCounter(),
                                  .counterpartyGuid = TraceActorIfKnown(sellerGuid, now),
                                  .itemId = purchase.itemId,
                                  .quantity = purchase.count,
                                  .unitPriceCopper = (purchase.buyout + purchase.count - 1u) / purchase.count,
                                  .referenceUnitPriceCopper = reference ? reference->unitPrice : 0u,
                                  .competingUnitPriceCopper = (purchase.buyout + purchase.count - 1u) / purchase.count,
                                  .occurredAt = now,
                                  .correlationAuctionId = purchase.auctionId,
                                  .kind = EconomyTraceKind::Purchased,
                              });
        }
        boughtAny = true;
    }

    if (boughtAny)
        Reset(botAI);
    return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::SellSurplus(PlayerbotAI* botAI, EconomyDecision const& decision,
                                                            Creature* auctioneer)
{
    Player* const bot = botAI->GetBot();
    if (!auctioneer)
        return TravelToAuctionHouse(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(decision.itemGuidCounter);
    Item* item = bot->GetItemByGuid(itemGuid);
    EconomyDecision sale = decision;
    auto const pending = pendingGatheredSupply.find(decision.itemId);
    if (pending != pendingGatheredSupply.end())
    {
        uint32 remainingDeficit = 0u;
        EconomyCoordinatorSnapshot const coordinator =
            GetPlayerbotEconomyCoordinator().Snapshot(GameTime::GetGameTime().count());
        auto const gap =
            std::find_if(coordinator.gaps.begin(), coordinator.gaps.end(),
                         [&decision, bot](EconomyDemandGap const& candidate)
                         {
                             return candidate.marketId == AuctionMarketId(bot->GetFaction()) &&
                                    candidate.group == EconomySubstitutionGroup::ExactReagent(decision.itemId);
                         });
        if (gap != coordinator.gaps.end())
            remainingDeficit = gap->remainingQuantity;

        AutonomousSupplierListing const bounded = PlayerbotEconomyGathering::BoundSupplierListing(
            sale.count, pending->second, remainingDeficit, sale.startBid, sale.buyout);
        sale.count = bounded.count;
        sale.startBid = bounded.startBid;
        sale.buyout = bounded.buyout;
        if (!sale.count)
        {
            pendingGatheredSupply.erase(pending);
            return ExecutionResult::Failed;
        }
    }
    if (!sale.count || !IsSafeSaleItem(botAI, item, sale))
        return ExecutionResult::Failed;

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse)
        return ExecutionResult::Failed;
    std::unordered_set<uint32> existingAuctions;
    existingAuctions.reserve(auctionHouse->GetAuctions().size());
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        (void)auction;
        existingAuctions.insert(auctionId);
    }

    WorldPacket packet;
    packet << auctioneer->GetGUID();
    packet << uint32(1);
    packet << itemGuid;
    packet << sale.count;
    packet << static_cast<uint32>(sale.startBid);
    packet << static_cast<uint32>(sale.buyout);
    packet << static_cast<uint32>(MIN_AUCTION_TIME / MINUTE);
    bot->GetSession()->HandleAuctionSellItem(packet);

    AuctionEntry const* listed = nullptr;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        if (!existingAuctions.contains(auctionId) && auction && auction->owner == bot->GetGUID() &&
            auction->item_template == sale.itemId && auction->itemCount == sale.count &&
            auction->startbid == sale.startBid && auction->buyout == sale.buyout)
        {
            listed = auction;
            break;
        }
    }
    if (listed && pending != pendingGatheredSupply.end())
    {
        pending->second -= sale.count;
        if (!pending->second)
            pendingGatheredSupply.erase(pending);
    }
    if (listed)
    {
        uint64 const now = GameTime::GetGameTime().count();
        std::string const chainPublicId = TraceChainForActor(bot->GetGUID().GetCounter(), now);
        if (!chainPublicId.empty())
        {
            [[maybe_unused]] bool const recorded =
                PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                    .Complete(true, {
                                        .deduplicationKey = Acore::StringFormat(
                                            "listing:{}:{}", AuctionMarketId(bot->GetFaction()), listed->Id),
                                        .chainPublicId = chainPublicId,
                                        .actorGuid = bot->GetGUID().GetCounter(),
                                        .itemId = sale.itemId,
                                        .quantity = sale.count,
                                        .unitPriceCopper = (sale.buyout + sale.count - 1u) / sale.count,
                                        .depositCopper = sale.deposit,
                                        .referenceUnitPriceCopper = sale.buyerCeilingPerItem,
                                        .competingUnitPriceCopper = sale.lowestCompetingBuyoutPerItem,
                                        .occurredAt = now,
                                        .correlationAuctionId = listed->Id,
                                        .kind = EconomyTraceKind::Listed,
                                    });
        }
        Reset(botAI);
    }
    return listed ? ExecutionResult::Operation : ExecutionResult::Failed;
}

void DefaultPlayerbotEconomyRuntime::ObserveMarketEvidence(PlayerbotAI* botAI, uint32 marketId, uint64 now)
{
    if (!marketId)
        return;

    Player* const bot = botAI->GetBot();
    PlayerbotEconomyMarket& market = GetPlayerbotEconomyMarket();
    EconomyMarketSnapshot const snapshot = market.Snapshot(now);
    std::unordered_set<uint32> controlledAuctions;
    for (EconomyCirculation const& event : snapshot.circulation)
    {
        if (event.auctionId && event.provenance != EconomyCirculationProvenance::Ordinary)
            controlledAuctions.insert(event.auctionId);
    }

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(marketId));
    if (auctionHouse)
    {
        for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
        {
            if (!auction || controlledAuctions.contains(auctionId) || !auction->itemCount || !auction->buyout ||
                auction->expire_time <= static_cast<time_t>(now))
            {
                continue;
            }
            [[maybe_unused]] uint64 const evidenceToken = market.AppendEvidence(
                {
                    .marketId = marketId,
                    .itemId = auction->item_template,
                    .substitutionGroup = ReagentGroup(auction->item_template),
                    .source = EconomyEvidenceSource::Listing,
                    .auctionId = auctionId,
                    .unitPrice = (static_cast<uint64>(auction->buyout) + auction->itemCount - 1u) / auction->itemCount,
                    .quantity = auction->itemCount,
                    .observedAt = now,
                    .expiresAt = static_cast<uint64>(auction->expire_time),
                },
                0u, true);
        }
    }

    for (Mail const* mail : bot->GetMails())
    {
        std::optional<AuctionMailDetails> const details = ParseAuctionMail(mail);
        if (!details || details->response != AUCTION_SUCCESSFUL || !details->bid || !details->quantity ||
            mail->deliver_time > static_cast<time_t>(now) || controlledAuctions.contains(details->auctionId))
        {
            continue;
        }
        uint64 const observedAt = mail->deliver_time;
        [[maybe_unused]] uint64 const evidenceToken = market.AppendEvidence(
            {
                .marketId = marketId,
                .itemId = details->itemId,
                .substitutionGroup = ReagentGroup(details->itemId),
                .source = EconomyEvidenceSource::Sale,
                .auctionId = details->auctionId,
                .unitPrice = (details->bid + details->quantity - 1u) / details->quantity,
                .quantity = details->quantity,
                .observedAt = observedAt,
                .expiresAt = observedAt + MIN_AUCTION_TIME,
            },
            0u, true);
    }
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ReconcileMarketPositionMail(
    PlayerbotAI* botAI, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyMarket& market = GetPlayerbotEconomyMarket();
    EconomyMarketSnapshot const snapshot = market.Snapshot(now);
    if (!snapshot.persistenceHealthy)
        return std::nullopt;
    AuctionHouseObject* const auctionHouse =
        marketId ? sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(marketId)) : nullptr;

    for (EconomyPosition const& position : snapshot.positions)
    {
        if (position.traderGuid != bot->GetGUID().GetCounter() || position.marketId != marketId)
            continue;
        if (market.HasPendingWrite(position.publicId))
            continue;

        std::map<uint64, EconomyCirculation const*> latest;
        for (EconomyCirculation const& event : snapshot.circulation)
        {
            if (event.positionPublicId != position.publicId)
                continue;
            auto const current = latest.find(event.itemGuid);
            if (current == latest.end() || current->second->occurredAt < event.occurredAt)
                latest[event.itemGuid] = &event;
        }

        for (auto const& [itemGuid, event] : latest)
        {
            if (event->state != EconomyCirculationState::Listed || !event->auctionId ||
                (auctionHouse && auctionHouse->GetAuction(event->auctionId)))
            {
                continue;
            }

            for (Mail const* mail : bot->GetMails())
            {
                std::optional<AuctionMailDetails> const details = ParseAuctionMail(mail);
                if (!details || details->auctionId != event->auctionId || details->itemId != position.itemId ||
                    details->quantity != event->quantity)
                {
                    continue;
                }

                EconomyPositionEvent outcome;
                outcome.positionPublicId = position.publicId;
                outcome.itemGuid = itemGuid;
                outcome.quantity = details->quantity;
                outcome.auctionId = details->auctionId;
                outcome.occurredAt = now;
                if (details->response == AUCTION_SUCCESSFUL)
                {
                    outcome.kind = EconomyPositionEventKind::Sold;
                    outcome.proceeds = details->bid;
                    outcome.fees = details->cut;
                }
                else if (details->response == AUCTION_EXPIRED)
                {
                    outcome.kind = EconomyPositionEventKind::Expired;
                    outcome.fees = details->deposit;
                }
                else
                    continue;

                if (!market.ApplyPositionEvent(outcome, 0u, true).accepted)
                    return std::nullopt;

                PlayerbotEconomyCycleResult result;
                result.outcome = PlayerbotEconomyCycleOutcome::Operation;
                result.phase = EconomyPhase::SellSurplus;
                result.workIdentity = {0u, position.itemId, details->auctionId, itemGuid};
                result.blocker =
                    details->response == AUCTION_SUCCESSFUL ? "market_position_sold" : "market_position_expired";
                result.schedulingEffect = EconomyAttemptOutcome::Operation;
                return result;
            }
        }
    }
    return std::nullopt;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteMarketMaking(
    PlayerbotAI* botAI, EconomySnapshot const& snapshot, Creature* auctioneer, uint32 marketId, uint64 now)
{
    EconomyMarketSnapshot const market = GetPlayerbotEconomyMarket().Snapshot(now);
    uint32 const traderGuid = botAI->GetBot()->GetGUID().GetCounter();
    auto const position = std::find_if(market.positions.begin(), market.positions.end(),
                                       [traderGuid, marketId](EconomyPosition const& value)
                                       { return value.traderGuid == traderGuid && value.marketId == marketId; });
    if (position != market.positions.end())
    {
        bool const ordinaryVendorSupply =
            std::binary_search(snapshot.applicableUnlimitedGoldVendorItemIds.begin(),
                               snapshot.applicableUnlimitedGoldVendorItemIds.end(), position->itemId);
        return ManageMarketPosition(botAI, *position, market, auctioneer, ordinaryVendorSupply, now);
    }
    return OpenMarketPosition(botAI, snapshot, market, auctioneer, marketId, now);
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ManageMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, EconomyMarketSnapshot const& market, Creature* auctioneer,
    bool ordinaryVendorSupply, uint64 now)
{
    Player* const bot = botAI->GetBot();
    if (position.state == EconomyPositionState::Pending)
        return ManagePendingMarketPosition(botAI, position, market, auctioneer, now);
    if (!market.persistenceHealthy)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, 0u};
        result.blocker = "market_position_persistence_unavailable";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (GetPlayerbotEconomyMarket().HasPendingWrite(position.publicId))
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, 0u};
        result.blocker = "market_position_persistence_pending";
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }

    std::map<uint64, EconomyCirculation const*> latest;
    for (EconomyCirculation const& event : market.circulation)
    {
        if (event.positionPublicId != position.publicId)
            continue;
        auto const current = latest.find(event.itemGuid);
        if (current == latest.end() || current->second->occurredAt < event.occurredAt)
            latest[event.itemGuid] = &event;
    }

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(position.marketId));
    for (auto const& [itemGuid, event] : latest)
    {
        (void)itemGuid;
        if (event->state == EconomyCirculationState::Listed && event->auctionId && auctionHouse &&
            auctionHouse->GetAuction(event->auctionId))
        {
            return std::nullopt;
        }
    }

    Item* item = nullptr;
    uint64 missingItemGuid = 0u;
    bool mailBacked = false;
    for (auto const& [itemGuid, event] : latest)
    {
        if (event->state != EconomyCirculationState::Acquired && event->state != EconomyCirculationState::Delivered)
        {
            continue;
        }
        missingItemGuid = itemGuid;
        Item* candidate = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuid));
        if (candidate && candidate->GetEntry() == position.itemId && candidate->GetCount() >= event->quantity)
        {
            item = candidate;
            break;
        }
        Item const* mailed = bot->GetMItem(static_cast<ObjectGuid::LowType>(itemGuid));
        mailBacked =
            mailBacked || (mailed && mailed->GetEntry() == position.itemId && mailed->GetCount() >= event->quantity);
    }
    if (!item)
    {
        if (mailBacked || !missingItemGuid)
            return std::nullopt;

        EconomyPositionEvent lost{
            .kind = EconomyPositionEventKind::Lost,
            .positionPublicId = position.publicId,
            .itemGuid = missingItemGuid,
            .quantity = position.remainingQuantity,
            .occurredAt = now,
        };
        if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(lost, 0u, true).accepted)
            return std::nullopt;

        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, missingItemGuid};
        result.blocker = "market_position_lost";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        Reset(botAI);
        return result;
    }

    if (!PlayerbotEconomyPolicy::AllowsAutonomousListing({ordinaryVendorSupply, false, false}))
        return VendorMarketPosition(botAI, position, item, now);

    if (now >= position.holdingDeadline || position.relistAttempts >= position.maximumRelistAttempts)
        return VendorMarketPosition(botAI, position, item, now);

    bool const coolingDown = std::any_of(market.cooldowns.begin(), market.cooldowns.end(),
                                         [&position, now](EconomyCooldown const& cooldown)
                                         {
                                             return cooldown.traderGuid == position.traderGuid &&
                                                    cooldown.marketId == position.marketId &&
                                                    cooldown.substitutionGroup == position.substitutionGroup &&
                                                    cooldown.nextEligibleAt > now;
                                         });
    if (coolingDown)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, item->GetGUID().GetCounter()};
        result.blocker = "market_position_cooldown";
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }

    EconomyCoordinatorSnapshot const coordinator = GetPlayerbotEconomyCoordinator().Snapshot(now);
    bool const hasDemand =
        std::any_of(coordinator.gaps.begin(), coordinator.gaps.end(),
                    [&position](EconomyDemandGap const& gap)
                    {
                        return gap.marketId == position.marketId &&
                               PlayerbotEconomyConsumption::GroupKey(gap.group) == position.substitutionGroup &&
                               gap.remainingQuantity != 0u;
                    });
    std::optional<EconomyReferencePrice> const reference =
        GetPlayerbotEconomyMarket().ReferencePrice(position.marketId, position.substitutionGroup, now);
    if (!hasDemand || !reference || !reference->confident)
        return std::nullopt;
    return ListMarketPosition(botAI, position, item, auctioneer, *reference, now);
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ManagePendingMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, EconomyMarketSnapshot const& market, Creature* auctioneer,
    uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyMarket& positionMarket = GetPlayerbotEconomyMarket();
    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {0u, position.itemId, 0u, 0u};
    result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
    result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;

    auto const pending = std::find_if(
        market.circulation.rbegin(), market.circulation.rend(), [&position](EconomyCirculation const& event)
        { return event.positionPublicId == position.publicId && event.state == EconomyCirculationState::Pending; });
    if (pending == market.circulation.rend())
    {
        result.blocker = "market_position_pending_provenance";
        return result;
    }
    result.workIdentity.auctionId = pending->auctionId;
    result.workIdentity.itemGuidCounter = pending->itemGuid;

    if (positionMarket.HasPendingWrite(position.publicId))
    {
        result.blocker = "market_position_persistence_pending";
        return result;
    }
    if (!market.persistenceHealthy)
    {
        result.blocker = "market_position_persistence_unavailable";
        return result;
    }

    ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(pending->itemGuid);
    Item* item = bot->GetItemByGuid(itemGuid);
    if (!item)
        item = bot->GetMItem(static_cast<ObjectGuid::LowType>(pending->itemGuid));
    if (item && item->GetEntry() == position.itemId && item->GetCount() >= position.remainingQuantity)
    {
        EconomyPositionMutationResult const activated =
            positionMarket.ActivatePendingPosition(position.publicId, 0u, now);
        result.outcome = activated.accepted ? PlayerbotEconomyCycleOutcome::Operation
                                            : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = activated.accepted ? "market_position_purchase_recovered" : "market_position_recovery_failed";
        result.schedulingEffect =
            activated.accepted ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    EconomyRiskFacts configurationFacts;
    configurationFacts.economyAffinity = 100u;
    configurationFacts.freeTradeskillMoney = MAX_MONEY_AMOUNT;
    configurationFacts.qualifiedEvidence = PlayerbotEconomyMarket::MAX_EVIDENCE_PER_GROUP;
    EconomyRiskDecision const configuration =
        PlayerbotEconomyMarket::EvaluateRisk(MarketRiskConfiguration(), configurationFacts);
    if (configuration.blocker != EconomyRiskBlocker::None)
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? PlayerbotEconomyMarket::RiskBlockerName(configuration.blocker)
                                   : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    if (now >= position.holdingDeadline)
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_stage_expired" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(position.marketId));
    AuctionEntry const* auction = auctionHouse ? auctionHouse->GetAuction(pending->auctionId) : nullptr;
    if (!auction)
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_candidate_gone" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    uint32 const sellerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
    if (auction->item_guid.GetCounter() != pending->itemGuid || auction->item_template != position.itemId ||
        auction->itemCount != position.remainingQuantity || auction->buyout != position.acquisitionCost ||
        !sellerAccountId || sellerAccountId == bot->GetSession()->GetAccountId())
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_candidate_changed" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    EconomyAssignmentRequest request;
    request.characterGuid = bot->GetGUID().GetCounter();
    request.marketId = position.marketId;
    request.group = EconomySubstitutionGroup::ExactReagent(position.itemId);
    request.quantity = position.remainingQuantity;
    request.kind = EconomyClaimKind::Purchase;
    request.priority = EconomyClaimPriority::Speculation;
    request.workKind = EconomyWorkKind::MarketMaking;
    request.workIdentity = "auction:" + std::to_string(pending->auctionId);
    request.sellerAccountId = sellerAccountId;
    request.expiresAt = now + 1u;
    EconomyAssignmentLease const lease = GetPlayerbotEconomyCoordinator().Lease(std::move(request), now);
    if (!lease.assignment || lease.assignment->quantity != position.remainingQuantity)
    {
        if (lease.assignment)
        {
            [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                lease.assignment->leaseId, EconomyAssignmentOutcome::NeedChanged, 0u, now);
        }
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_displaced" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    if (!auctioneer)
    {
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::FailedTravel, 0u, now);
        result.outcome = TravelToAuctionHouse(botAI) ? PlayerbotEconomyCycleOutcome::Scheduled
                                                     : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                             ? "market_position_travel"
                             : "market_position_missing_auctioneer";
        result.schedulingEffect = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                                      ? EconomyAttemptOutcome::Operation
                                      : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    WorldPacket packet;
    packet << auctioneer->GetGUID();
    packet << pending->auctionId;
    packet << static_cast<uint32>(position.acquisitionCost);
    bot->GetSession()->HandleAuctionPlaceBid(packet);
    if (auctionHouse->GetAuction(pending->auctionId))
    {
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::FailedPurchase, 0u, now);
        [[maybe_unused]] uint64 const cooldown = positionMarket.SaveCooldown(
            {
                .traderGuid = position.traderGuid,
                .marketId = position.marketId,
                .substitutionGroup = position.substitutionGroup,
                .cause = EconomyCooldownCause::FailedPurchase,
                .nextEligibleAt = now + position.cooldownSeconds,
            },
            lease.assignment->leaseId, false);
        [[maybe_unused]] uint64 const cancelled =
            positionMarket.CancelPendingPosition(position.publicId, lease.assignment->leaseId);
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "market_position_purchase_failed";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    [[maybe_unused]] bool const committed = GetPlayerbotEconomyCoordinator().RecordOutcome(
        lease.assignment->leaseId, EconomyAssignmentOutcome::Committed, position.remainingQuantity, now);
    EconomyPositionMutationResult const activated =
        positionMarket.ActivatePendingPosition(position.publicId, lease.assignment->leaseId, now);
    if (activated.accepted)
    {
        [[maybe_unused]] bool const completed = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::Completed, position.remainingQuantity, now);
    }
    result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    result.blocker = activated.accepted ? "market_position_opened" : "market_position_commit_pending";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    Reset(botAI);
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::OpenMarketPosition(
    PlayerbotAI* botAI, EconomySnapshot const& snapshot, EconomyMarketSnapshot const& market, Creature* auctioneer,
    uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    (void)auctioneer;
    EconomyRiskConfiguration const configuration = MarketRiskConfiguration();
    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(marketId));
    AuctionHouseEntry const* const houseEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId(marketId));
    if (!auctionHouse || !houseEntry)
        return std::nullopt;

    uint64 totalExposure = 0u;
    std::map<std::string, uint64> groupExposure;
    for (EconomyPosition const& position : market.positions)
    {
        if (position.traderGuid != bot->GetGUID().GetCounter() || position.marketId != marketId)
            continue;
        totalExposure += position.acquisitionCost;
        groupExposure[position.substitutionGroup] += position.acquisitionCost;
    }

    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        if (!auction || !auction->itemCount || !auction->buyout || bot->GetItemCount(auction->item_template) != 0u)
        {
            continue;
        }
        bool const ordinaryVendorSupply =
            std::binary_search(snapshot.applicableUnlimitedGoldVendorItemIds.begin(),
                               snapshot.applicableUnlimitedGoldVendorItemIds.end(), auction->item_template);
        if (!PlayerbotEconomyPolicy::AllowsAutonomousListing({ordinaryVendorSupply, false, false}))
            continue;
        bool alreadyMailed = false;
        for (Mail const* mail : bot->GetMails())
        {
            alreadyMailed =
                alreadyMailed || std::any_of(mail->items.begin(), mail->items.end(), [auction](MailItemInfo const& item)
                                             { return item.item_template == auction->item_template; });
        }
        if (alreadyMailed)
            continue;

        std::string const group = ReagentGroup(auction->item_template);
        std::optional<EconomyReferencePrice> const reference =
            GetPlayerbotEconomyMarket().ReferencePrice(marketId, group, now);
        Item* const auctionItem = sAuctionMgr->GetAItem(auction->item_guid);
        if (!reference || !auctionItem)
            continue;

        EconomyMarketEntryFacts facts;
        facts.risk.economyAffinity = EconomyAffinity(bot->GetGUID().GetCounter());
        facts.risk.freeTradeskillMoney = snapshot.freeMoneyForTradeskill;
        facts.risk.groupExposure = groupExposure[group];
        facts.risk.totalExposure = totalExposure;
        facts.risk.qualifiedEvidence = reference->acceptedSales + reference->acceptedListings;
        facts.traderGuid = bot->GetGUID().GetCounter();
        facts.buyerAccountId = bot->GetSession()->GetAccountId();
        facts.sellerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
        facts.marketId = marketId;
        facts.itemId = auction->item_template;
        facts.substitutionGroup = group;
        facts.itemGuid = auction->item_guid.GetCounter();
        facts.quantity = auction->itemCount;
        facts.buyout = auction->buyout;
        facts.referenceUnitPrice = reference->unitPrice;
        facts.referenceConfident = reference->confident;
        facts.depositPerListing =
            AuctionHouseMgr::GetAuctionDeposit(houseEntry, MIN_AUCTION_TIME, auctionItem, auction->itemCount);
        facts.auctionCutBasisPoints =
            static_cast<uint32>(houseEntry->cutPercent * sWorld->getRate(RATE_AUCTION_CUT) * 100.0f);
        facts.now = now;
        EconomyRiskDecision const risk = GetPlayerbotEconomyMarket().EvaluateEntry(configuration, facts);
        if (risk.blocker != EconomyRiskBlocker::None)
            continue;

        EconomyAssignmentRequest request;
        request.characterGuid = bot->GetGUID().GetCounter();
        request.marketId = marketId;
        request.group = EconomySubstitutionGroup::ExactReagent(auction->item_template);
        request.quantity = auction->itemCount;
        request.kind = EconomyClaimKind::Purchase;
        request.priority = EconomyClaimPriority::Speculation;
        request.workKind = EconomyWorkKind::MarketMaking;
        request.workIdentity = "auction:" + std::to_string(auctionId);
        request.sellerAccountId = facts.sellerAccountId;
        request.expiresAt = now + 1u;
        EconomyAssignmentLease const lease = GetPlayerbotEconomyCoordinator().Lease(std::move(request), now);
        if (!lease.assignment || lease.assignment->quantity != auction->itemCount)
        {
            if (lease.assignment)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    lease.assignment->leaseId, EconomyAssignmentOutcome::NeedChanged, 0u, now);
            }
            continue;
        }

        uint32 const purchasedItemId = auction->item_template;
        uint32 const purchasedQuantity = auction->itemCount;
        uint64 const purchasedItemGuid = auction->item_guid.GetCounter();
        uint64 const purchaseBuyout = auction->buyout;
        EconomyPosition position;
        position.publicId = PositionPublicId(bot->GetGUID().GetCounter(), purchasedItemGuid, now);
        position.traderGuid = bot->GetGUID().GetCounter();
        position.marketId = marketId;
        position.itemId = purchasedItemId;
        position.substitutionGroup = group;
        position.initialQuantity = purchasedQuantity;
        position.remainingQuantity = purchasedQuantity;
        position.acquisitionCost = purchaseBuyout;
        position.state = EconomyPositionState::Pending;
        position.maximumRelistAttempts = static_cast<uint8>(configuration.maximumRelistAttempts);
        position.cooldownSeconds = configuration.cooldownSeconds;
        position.holdingDeadline = now + configuration.holdingHorizonSeconds;
        EconomyPositionMutationResult const staged = GetPlayerbotEconomyMarket().StagePosition(
            std::move(position), purchasedItemGuid, auctionId, lease.assignment->leaseId, now);
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::NeedChanged, 0u, now);
        if (!staged.accepted)
            continue;

        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.phase = EconomyPhase::BuyReagent;
        result.workIdentity = {0u, purchasedItemId, auctionId, purchasedItemGuid};
        result.blocker = "market_position_staged";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        Reset(botAI);
        return result;
    }
    return std::nullopt;
}

bool DefaultPlayerbotEconomyRuntime::IsSafePositionItem(PlayerbotAI* botAI, Item const* item,
                                                        EconomyPosition const& position) const
{
    return item && item->GetEntry() == position.itemId && item->GetCount() == position.remainingQuantity &&
           IsInventoryBagItem(item) && item->CanBeTraded() && !item->IsSoulBound() && !item->IsNotEmptyBag() &&
           !item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) && item->GetUInt32Value(ITEM_FIELD_DURATION) == 0u &&
           !sAuctionMgr->GetAItem(item->GetGUID()) &&
           botAI->GetBot()->GetItemCount(position.itemId) >= position.remainingQuantity;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ListMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, Item* item, Creature* auctioneer,
    EconomyReferencePrice const& reference, uint64 now)
{
    Player* const bot = botAI->GetBot();
    if (!auctioneer)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = TravelToAuctionHouse(botAI) ? PlayerbotEconomyCycleOutcome::Scheduled
                                                     : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, item->GetGUID().GetCounter()};
        result.blocker = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                             ? "market_position_travel"
                             : "market_position_missing_auctioneer";
        result.schedulingEffect = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                                      ? EconomyAttemptOutcome::Operation
                                      : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!IsSafePositionItem(botAI, item, position))
        return std::nullopt;

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    AuctionHouseEntry const* const houseEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId(position.marketId));
    if (!auctionHouse || !houseEntry)
        return std::nullopt;

    uint32 const quantity = position.remainingQuantity;
    uint64 const ceiling = static_cast<uint64>(reference.unitPrice) * quantity;
    if (!ceiling || ceiling > MAX_MONEY_AMOUNT)
        return std::nullopt;
    SaleItemCandidate sale;
    sale.count = quantity;
    sale.templateSellPrice = item->GetTemplate()->SellPrice;
    sale.deposit = AuctionHouseMgr::GetAuctionDeposit(houseEntry, MIN_AUCTION_TIME, item, quantity);
    sale.auctionCutBasisPoints =
        static_cast<uint32>(houseEntry->cutPercent * sWorld->getRate(RATE_AUCTION_CUT) * 100.0f);
    sale.buyerCeilingPerItem = reference.unitPrice;
    sale.minimumTransactionBasis = 1u;
    uint64 const floor = PlayerbotEconomyPolicy::SellerFloor(sale);
    if (!floor || floor > ceiling)
        return std::nullopt;

    std::unordered_set<uint32> existingAuctions;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        (void)auction;
        existingAuctions.insert(auctionId);
    }
    ObjectGuid const originalGuid = item->GetGUID();
    WorldPacket packet;
    packet << auctioneer->GetGUID();
    packet << uint32(1);
    packet << originalGuid;
    packet << quantity;
    packet << static_cast<uint32>(floor);
    packet << static_cast<uint32>(ceiling);
    packet << static_cast<uint32>(MIN_AUCTION_TIME / MINUTE);
    bot->GetSession()->HandleAuctionSellItem(packet);

    AuctionEntry const* listed = nullptr;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        if (!existingAuctions.contains(auctionId) && auction && auction->owner == bot->GetGUID() &&
            auction->item_template == position.itemId && auction->itemCount == quantity && auction->startbid == floor &&
            auction->buyout == ceiling)
        {
            listed = auction;
            break;
        }
    }
    if (!listed)
        return std::nullopt;

    if (listed->item_guid != originalGuid)
    {
        Item* const remaining = bot->GetItemByGuid(originalGuid);
        if (!remaining)
            return std::nullopt;
        EconomyPositionEvent split{
            .kind = EconomyPositionEventKind::Split,
            .positionPublicId = position.publicId,
            .itemGuid = originalGuid.GetCounter(),
            .replacementItemGuid = listed->item_guid.GetCounter(),
            .quantity = remaining->GetCount(),
            .replacementQuantity = quantity,
            .occurredAt = now,
        };
        if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(split, 0u, true).accepted)
            return std::nullopt;
    }

    EconomyMarketSnapshot const market = GetPlayerbotEconomyMarket().Snapshot(now);
    bool const previouslyListed = std::any_of(
        market.circulation.begin(), market.circulation.end(), [&position](EconomyCirculation const& event)
        { return event.positionPublicId == position.publicId && event.state == EconomyCirculationState::Listed; });
    EconomyPositionEvent event{
        .kind = previouslyListed ? EconomyPositionEventKind::Relisted : EconomyPositionEventKind::Listed,
        .positionPublicId = position.publicId,
        .itemGuid = listed->item_guid.GetCounter(),
        .quantity = quantity,
        .auctionId = listed->Id,
        .occurredAt = now,
    };
    if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(event, 0u, true).accepted)
        return std::nullopt;

    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    result.phase = EconomyPhase::SellSurplus;
    result.workIdentity = {0u, position.itemId, listed->Id, listed->item_guid.GetCounter()};
    result.blocker = previouslyListed ? "market_position_relisted" : "market_position_listed";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    Reset(botAI);
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::VendorMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, Item* item, uint64 now)
{
    Player* const bot = botAI->GetBot();
    if (!item || item->GetCount() != position.remainingQuantity || item->GetTemplate()->SellPrice == 0u)
    {
        return std::nullopt;
    }
    ObjectGuid const itemGuid = item->GetGUID();
    uint32 const quantity = item->GetCount();
    uint32 const moneyBefore = bot->GetMoney();
    SellAction(botAI, "economy position vendor").Sell(item);
    if (bot->GetItemByGuid(itemGuid))
        return std::nullopt;

    EconomyPositionEvent event{
        .kind = EconomyPositionEventKind::Vendored,
        .positionPublicId = position.publicId,
        .itemGuid = itemGuid.GetCounter(),
        .quantity = quantity,
        .proceeds = bot->GetMoney() >= moneyBefore ? bot->GetMoney() - moneyBefore : 0u,
        .occurredAt = now,
    };
    if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(event, 0u, true).accepted)
        return std::nullopt;

    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    result.phase = EconomyPhase::SellSurplus;
    result.workIdentity = {0u, position.itemId, 0u, itemGuid.GetCounter()};
    result.blocker = "market_position_vendored";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    Reset(botAI);
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteAutonomousGathering(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan, EconomySnapshot const& snapshot,
    EconomyDecision const& productionDecision, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    if (GatheringAffinity(bot->GetGUID().GetCounter()) < PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM)
        return std::nullopt;

    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyCoordinatorSnapshot const coordinatorSnapshot = coordinator.Snapshot(now);
    std::optional<GatheringOpportunity> opportunity =
        CoordinatorGatheringOpportunity(bot, coordinatorSnapshot, snapshot, marketId);
    if (!opportunity)
        opportunity = DeficitGatheringOpportunity(bot, snapshot, productionDecision);
    if (!activeGathering)
    {
        if (opportunity)
            return StartAutonomousGathering(botAI, *opportunity, productionDecision.phase == EconomyPhase::None,
                                            marketId, now);

        if (productionDecision.phase != EconomyPhase::None)
            return std::nullopt;
        for (uint16 skillId : LearnedPrimaryCapabilitySkillIds(bot))
        {
            std::optional<GatheringProfession> const profession = GatheringProfessionForSkill(skillId);
            if (!profession || !bot->HasSkill(skillId) || bot->GetSkillValue(skillId) >= bot->GetMaxSkillValue(skillId))
                continue;
            return StartAutonomousGathering(botAI, GatheringOpportunity{*profession, skillId, 0u, 0u, 0u}, false,
                                            marketId, now);
        }
        return std::nullopt;
    }

    ActiveGatheringTrip& trip = *activeGathering;
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    bool const targetOwned = activeGatheringPointDestination &&
                             ownedTravelDestination == activeGatheringPointDestination.get() && target->isForced() &&
                             target->getDestination() == ownedTravelDestination;

    AutonomousGatheringFacts facts;
    facts.now = now;
    facts.currentItemCount = trip.plan.itemId ? bot->GetItemCount(trip.plan.itemId) : 0u;
    facts.currentSkillValue = bot->GetSkillValue(trip.skillId);
    auto const activeClaim =
        std::find_if(coordinatorSnapshot.claims.begin(), coordinatorSnapshot.claims.end(),
                     [&trip](EconomyAssignment const& claim) { return claim.leaseId == trip.coordinatorLeaseId; });
    facts.demandStillExists =
        !trip.coordinatorLeaseId || (!trip.coordinatorSettled && activeClaim != coordinatorSnapshot.claims.end() &&
                                     activeClaim->state == EconomyClaimState::Leased);
    facts.destinationAvailable = targetOwned && target->isActive() && target->getStatus() != TRAVEL_STATUS_COOLDOWN &&
                                 target->getStatus() != TRAVEL_STATUS_EXPIRED;
    facts.atDestination = facts.destinationAvailable && target->getStatus() == TRAVEL_STATUS_WORK;
    facts.safe = bot->IsInWorld() && bot->IsAlive() && !bot->GetTransport() && !bot->InBattleground() &&
                 !bot->IsBeingTeleported() && !bot->HasUnitState(UNIT_STATE_IN_FLIGHT) &&
                 (!bot->IsInCombat() || trip.killTarget);
    if (trip.plan.itemId)
    {
        ItemPosCountVec destinations;
        facts.inventoryCapacity =
            bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, trip.plan.itemId, 1u) == EQUIP_ERR_OK;
    }
    else
        facts.inventoryCapacity = AI_VALUE(uint8, "bag space") <= 80u;

    if (facts.atDestination)
    {
        facts.resourceAvailable = HasMatchingGatheringLoot(botAI, trip.skillId);
        facts.existingSkinningCorpse = facts.resourceAvailable;
    }
    facts.creatureKillStarted = static_cast<bool>(trip.killTarget);
    if (trip.killTarget)
    {
        Unit* const targetUnit = botAI->GetUnit(trip.killTarget);
        facts.creatureKillActive = targetUnit && targetUnit->IsAlive();
        facts.existingSkinningCorpse =
            facts.existingSkinningCorpse || (targetUnit && !targetUnit->IsAlive() && targetUnit->IsInWorld());
    }

    AutonomousGatheringDecision const decision = PlayerbotEconomyGathering::DecideAutonomous(trip.plan, facts);
    if (trip.coordinatorLeaseId && !trip.coordinatorSettled && decision.gatheredQuantity > trip.committedQuantity)
    {
        trip.committedQuantity = std::min(decision.gatheredQuantity, trip.plan.requestedQuantity);
        [[maybe_unused]] bool const committed = coordinator.RecordOutcome(
            trip.coordinatorLeaseId, EconomyAssignmentOutcome::Committed, trip.committedQuantity, now);
    }
    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {trip.spellId, trip.plan.itemId, 0u, 0u};

    if (decision.action == AutonomousGatheringAction::Complete)
    {
        if (trip.coordinatorLeaseId && !trip.coordinatorSettled)
        {
            [[maybe_unused]] bool const completed = coordinator.RecordOutcome(
                trip.coordinatorLeaseId, EconomyAssignmentOutcome::Completed, trip.plan.requestedQuantity, now);
            trip.coordinatorSettled = true;
        }
        if (trip.plan.itemId && decision.gatheredQuantity)
            pendingGatheredSupply[trip.plan.itemId] += decision.gatheredQuantity;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.blocker = "gathering_complete";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        Reset(botAI);
        return result;
    }
    if (decision.action == AutonomousGatheringAction::Release)
    {
        if (trip.coordinatorLeaseId && !trip.coordinatorSettled)
        {
            EconomyAssignmentOutcome const outcome =
                trip.committedQuantity ? EconomyAssignmentOutcome::InventoryReceived
                : decision.blocker == AutonomousGatheringBlocker::DemandGone ? EconomyAssignmentOutcome::NeedChanged
                                                                             : EconomyAssignmentOutcome::FailedTravel;
            [[maybe_unused]] bool const released =
                coordinator.RecordOutcome(trip.coordinatorLeaseId, outcome, trip.committedQuantity, now);
            trip.coordinatorSettled = true;
        }
        if (trip.plan.itemId && decision.gatheredQuantity)
            pendingGatheredSupply[trip.plan.itemId] += decision.gatheredQuantity;
        // A trip that captured loot before it ended did real work; only an empty-handed
        // release counts as a failure for backoff and quarantine purposes.
        bool const progress = PlayerbotEconomyGathering::ReleaseCountsAsProgress(decision);
        result.outcome = progress ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::NoCandidate;
        result.blocker = AutonomousBlockerName(decision.blocker);
        result.schedulingEffect = progress ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::NoCandidate;
        Reset(botAI);
        return result;
    }
    if (decision.action == AutonomousGatheringAction::Gather)
    {
        result.blocker = "gathering_resource";
    }
    else if (decision.action == AutonomousGatheringAction::GrindOneCreature)
    {
        if (!StartOneSkinningKill(botAI, trip.destination))
        {
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = "gathering_skinning_creature_missing";
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            Reset(botAI);
            return result;
        }
        result.blocker = "gathering_skinning_grind";
    }
    else if (decision.action == AutonomousGatheringAction::Travel && facts.atDestination)
    {
        WorldPosition botPosition(bot);
        WorldPosition* const nextPoint =
            trip.destination->NextUnvisitedPoint(botPosition, bot->GetMapId(), trip.attemptedPoints);
        if (!nextPoint)
        {
            if (trip.coordinatorLeaseId && !trip.coordinatorSettled)
            {
                [[maybe_unused]] bool const released = PlayerbotEconomyGathering::SettleUnavailableDestination(
                    coordinator, trip.coordinatorLeaseId, trip.committedQuantity, now);
                trip.coordinatorSettled = true;
            }
            if (trip.plan.itemId && decision.gatheredQuantity)
                pendingGatheredSupply[trip.plan.itemId] += decision.gatheredQuantity;
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = "gathering_resource_unavailable";
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            Reset(botAI);
            return result;
        }

        if (!TravelToGatheringPoint(botAI, trip.destination, nextPoint))
        {
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = "gathering_destination_unavailable";
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            Reset(botAI);
            return result;
        }
        trip.attemptedPoints.push_back(nextPoint);
        result.blocker = "gathering_search";
    }
    else
        result.blocker = decision.action == AutonomousGatheringAction::Travel ? "gathering_travel" : "gathering_wait";

    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::StartAutonomousGathering(
    PlayerbotAI* botAI, GatheringOpportunity const& opportunity, bool allowFailureResult, uint32 marketId, uint64 now)
{
    GatheringDestinationBlocker blocker = GatheringDestinationBlocker::Empty;
    std::vector<GatheringTravelDestination*> const destinations =
        sPlayerbotEconomyTravelCatalog.GatheringDestinations(botAI->GetBot(), opportunity.skillId, &blocker);
    if (destinations.empty())
    {
        if (!allowFailureResult)
            return std::nullopt;
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.phase = EconomyPhase::BuyReagent;
        result.workIdentity = {opportunity.spellId, opportunity.itemId, 0u, 0u};
        result.blocker = GatheringDestinationBlockerName(blocker);
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }

    Player* const bot = botAI->GetBot();
    WorldPosition botPosition(bot);
    GatheringTravelDestination* const destination =
        *std::min_element(destinations.begin(), destinations.end(),
                          [&botPosition](GatheringTravelDestination* left, GatheringTravelDestination* right)
                          { return left->distanceTo(&botPosition) < right->distanceTo(&botPosition); });
    WorldPosition* const initialPoint = destination->nearestPoint(&botPosition);
    if (!TravelToGatheringPoint(botAI, destination, initialPoint))
        return std::nullopt;

    std::optional<EconomyAssignment> assignment;
    if (opportunity.itemId)
    {
        EconomyAssignmentRequest request;
        request.characterGuid = botAI->GetBot()->GetGUID().GetCounter();
        request.marketId = marketId;
        request.group = EconomySubstitutionGroup::ExactReagent(opportunity.itemId);
        request.quantity = opportunity.quantity;
        request.kind = EconomyClaimKind::Resource;
        request.priority = EconomyClaimPriority::Producer;
        request.workKind = EconomyWorkKind::Gather;
        request.workIdentity =
            "gather:" + std::to_string(opportunity.itemId) + ":" + std::to_string(opportunity.skillId);
        request.expiresAt = now + destination->getExpireDelay() / 1000u;
        EconomyAssignmentLease const lease = GetPlayerbotEconomyCoordinator().Lease(std::move(request), now);
        if (!lease.assignment)
        {
            Reset(botAI);
            return std::nullopt;
        }
        assignment = lease.assignment;
    }

    ActiveGatheringTrip trip;
    trip.plan.profession = opportunity.profession;
    trip.plan.itemId = opportunity.itemId;
    trip.plan.requestedQuantity = assignment ? assignment->quantity : opportunity.quantity;
    trip.plan.startingItemCount = opportunity.itemId ? bot->GetItemCount(opportunity.itemId) : 0u;
    trip.plan.startingSkillValue = bot->GetSkillValue(opportunity.skillId);
    trip.plan.expiresAt = now + destination->getExpireDelay() / 1000u;
    trip.skillId = opportunity.skillId;
    trip.spellId = opportunity.spellId;
    trip.coordinatorLeaseId = assignment ? assignment->leaseId : 0u;
    trip.destination = destination;
    TravelTarget* const target = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (target->getPosition())
        trip.attemptedPoints.push_back(target->getPosition());
    activeGathering = std::move(trip);

    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {opportunity.spellId, opportunity.itemId, 0u, 0u};
    result.blocker = "gathering_travel";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    return result;
}

bool DefaultPlayerbotEconomyRuntime::HasMatchingGatheringLoot(PlayerbotAI* botAI, uint32 skillId)
{
    AiObjectContext* const context = botAI->GetAiObjectContext();
    botAI->DoSpecificAction("add gathering loot", Event(), true);
    if (!AI_VALUE(bool, "has available loot"))
        return false;
    LootObject loot = AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.sightDistance);
    return !loot.IsEmpty() && loot.skillId == skillId;
}

bool DefaultPlayerbotEconomyRuntime::StartOneSkinningKill(PlayerbotAI* botAI, GatheringTravelDestination* destination)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Unit* const unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->GetEntry() != static_cast<uint32>(destination->getEntry()) ||
            !bot->IsHostileTo(unit) || unit->IsInCombat() || !WorldPosition(bot).canPathTo(WorldPosition(unit), bot))
        {
            continue;
        }
        activeGathering->killTarget = guid;
        SET_AI_VALUE(ObjectGuid, "pull target", guid);
        return true;
    }
    return false;
}

bool DefaultPlayerbotEconomyRuntime::TravelToGatheringPoint(PlayerbotAI* botAI, GatheringTravelDestination* destination,
                                                            WorldPosition* point)
{
    if (!destination || !point)
        return false;

    AiObjectContext* const context = botAI->GetAiObjectContext();
    TravelTarget* const currentTarget = AI_VALUE(TravelTarget*, "travel target");
    if (currentTarget->isForced() &&
        (!ownedTravelDestination || currentTarget->getDestination() != ownedTravelDestination))
    {
        return false;
    }

    std::unique_ptr<TravelDestination> pointDestination = destination->MakePointDestination(point);
    if (!pointDestination)
        return false;

    if (!botAI->HasStrategy("travel", BOT_STATE_NON_COMBAT))
    {
        botAI->ChangeStrategy("+travel", BOT_STATE_NON_COMBAT);
        ownsTravelStrategy = true;
    }

    TravelTarget newTarget(botAI);
    newTarget.setTarget(pointDestination.get(), point);
    newTarget.setRadius(INTERACTION_DISTANCE);
    newTarget.setForced(true);
    EconomyTravelAction(botAI).Apply(&newTarget, currentTarget);
    activeGatheringPointDestination = std::move(pointDestination);
    ownedTravelDestination = activeGatheringPointDestination.get();
    return true;
}

bool DefaultPlayerbotEconomyRuntime::TravelToAuctionHouse(PlayerbotAI* botAI)
{
    return TravelToDestination(botAI, sPlayerbotEconomyTravelCatalog.SelectAuctioneer(botAI->GetBot()));
}

bool DefaultPlayerbotEconomyRuntime::TravelToMailbox(PlayerbotAI* botAI)
{
    return TravelToDestination(botAI, sPlayerbotEconomyTravelCatalog.SelectMailbox(botAI->GetBot()));
}

bool DefaultPlayerbotEconomyRuntime::TravelToDestination(PlayerbotAI* botAI, TravelDestination* destination)
{
    if (!destination)
        return false;

    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    TravelTarget* currentTarget = AI_VALUE(TravelTarget*, "travel target");
    if (currentTarget->isForced() &&
        (!ownedTravelDestination || currentTarget->getDestination() != ownedTravelDestination))
    {
        return false;
    }

    if (currentTarget->isForced() && currentTarget->getDestination() == ownedTravelDestination)
    {
        if (currentTarget->getDestination() == destination && currentTarget->isActive())
            return true;

        Reset(botAI);
    }

    if (!botAI->HasStrategy("travel", BOT_STATE_NON_COMBAT))
    {
        botAI->ChangeStrategy("+travel", BOT_STATE_NON_COMBAT);
        ownsTravelStrategy = true;
    }

    WorldPosition botPosition(bot);
    TravelTarget newTarget(botAI);
    newTarget.setTarget(destination, destination->nearestPoint(&botPosition));
    newTarget.setRadius(INTERACTION_DISTANCE);
    newTarget.setForced(true);
    ownedTravelDestination = destination;
    EconomyTravelAction(botAI).Apply(&newTarget, currentTarget);
    return true;
}

bool DefaultPlayerbotEconomyRuntime::IsInventoryBagItem(Item const* item) const
{
    if (!item)
        return false;

    uint8 const bag = item->GetBagSlot();
    uint8 const slot = item->GetSlot();
    if (bag == INVENTORY_SLOT_BAG_0)
        return slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END;

    return bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END;
}

bool DefaultPlayerbotEconomyRuntime::IsSafeSaleItem(PlayerbotAI* botAI, Item const* item,
                                                    EconomyDecision const& decision)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    if (!item || item->GetGUID().GetCounter() != decision.itemGuidCounter || item->GetEntry() != decision.itemId ||
        item->GetCount() < decision.count || !IsInventoryBagItem(item) ||
        AI_VALUE2(ItemUsage, "item usage", item->GetEntry()) != ITEM_USAGE_AH)
    {
        return false;
    }

    if (!PlayerbotEconomyPolicy::PreservesProfessionReserve(bot->GetItemCount(item->GetEntry()), decision.count,
                                                            decision.professionReserveFloor))
    {
        return false;
    }

    return item->CanBeTraded() && !item->IsSoulBound() && !item->IsNotEmptyBag() &&
           !item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) && item->GetUInt32Value(ITEM_FIELD_DURATION) == 0 &&
           !sAuctionMgr->GetAItem(item->GetGUID()) && decision.startBid > 0 && decision.startBid <= decision.buyout &&
           decision.buyout <= MAX_MONEY_AMOUNT;
}

void DefaultPlayerbotEconomyRuntime::Reset(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    uint64 const now = GameTime::GetGameTime().count();
    if (activeGathering && activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled)
    {
        uint32 committedQuantity = activeGathering->committedQuantity;
        if (activeGathering->plan.itemId)
        {
            uint32 const current = bot->GetItemCount(activeGathering->plan.itemId);
            if (current > activeGathering->plan.startingItemCount)
            {
                committedQuantity = std::min(current - activeGathering->plan.startingItemCount,
                                             activeGathering->plan.requestedQuantity);
            }
        }
        EconomyAssignmentOutcome const outcome = committedQuantity ? EconomyAssignmentOutcome::InventoryReceived
                                                 : !sPlayerbotEconomyConfig.lifecycleEnabled
                                                     ? EconomyAssignmentOutcome::Disabled
                                                 : !bot->IsInWorld() ? EconomyAssignmentOutcome::LoggedOut
                                                                     : EconomyAssignmentOutcome::CapabilityLost;
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            activeGathering->coordinatorLeaseId, outcome, committedQuantity, now);
        activeGathering->coordinatorSettled = true;
    }

    if (activeGathering && activeGathering->killTarget &&
        AI_VALUE(ObjectGuid, "pull target") == activeGathering->killTarget)
    {
        SET_AI_VALUE(ObjectGuid, "pull target", ObjectGuid::Empty);
    }

    if (ownedTravelDestination)
    {
        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
        if (target->isForced() && target->getDestination() == ownedTravelDestination)
            EconomyTravelAction(botAI).Clear(target);

        ownedTravelDestination = nullptr;
    }
    activeGatheringPointDestination.reset();

    if (ownsTravelStrategy)
    {
        botAI->ChangeStrategy("-travel", BOT_STATE_NON_COMBAT);
        ownsTravelStrategy = false;
    }

    activeGathering.reset();
    activeTrainer.reset();
    activeTrainerObjective.reset();

    if (!sPlayerbotEconomyConfig.lifecycleEnabled || botAI->HasActivePlayerMaster() || !bot->IsInWorld())
    {
        EconomyAssignmentOutcome const outcome = !sPlayerbotEconomyConfig.lifecycleEnabled
                                                     ? EconomyAssignmentOutcome::Disabled
                                                 : !bot->IsInWorld() ? EconomyAssignmentOutcome::LoggedOut
                                                                     : EconomyAssignmentOutcome::CapabilityLost;
        GetPlayerbotEconomyCoordinator().InvalidateActor(bot->GetGUID().GetCounter(), outcome, now);
        pendingCraftTrace.reset();
        sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        EconomyActorFacts actor;
        actor.characterGuid = bot->GetGUID().GetCounter();
        actor.accountId = bot->GetSession() ? bot->GetSession()->GetAccountId() : 0u;
        actor.marketId = AuctionMarketId(bot->GetFaction());
        actor.online = bot->IsInWorld();
        actor.autonomous = false;
        GetPlayerbotEconomyCoordinator().RefreshActor(std::move(actor), now);
    }
}
}  // namespace

EconomyAssignmentLease PlayerbotEconomyRuntime::AssignProduction(PlayerbotEconomyCoordinator& coordinator,
                                                                 EconomyProductionRequest request, uint64 now)
{
    return coordinator.AssignProduction(std::move(request), now);
}

EconomyProductionOutput PlayerbotEconomyRuntime::ReconcileProductionInventory(PlayerbotEconomyCoordinator& coordinator,
                                                                              uint64 leaseId, uint32 startingQuantity,
                                                                              uint32 currentQuantity, uint64 now)
{
    return coordinator.RecordProductionInventory(leaseId, startingQuantity, currentQuantity, now);
}

std::unique_ptr<PlayerbotEconomyRuntime> CreatePlayerbotEconomyRuntime()
{
    return std::make_unique<DefaultPlayerbotEconomyRuntime>();
}
