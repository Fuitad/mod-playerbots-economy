/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Ai/Base/Actions/EconomyGatheringAction.h"

#include <algorithm>
#include <map>
#include <optional>

#include "Bag.h"
#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "Bot/Economy/PlayerbotEconomyTrace.h"
#include "Bot/Economy/PlayerbotEconomyTravel.h"
#include "Bot/Economy/PlayerbotProfessionCapability.h"
#include "Bot/Personality/PlayerbotCareerAdapter.h"
#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "Bot/Personality/PlayerbotPersonalityMgr.h"
#include "Duration.h"
#include "GameTime.h"
#include "Item.h"
#include "LootObjectStack.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "TravelMgr.h"

namespace
{
constexpr uint32 NEARBY_GATHERING_LEASE_SECONDS = 30u;

std::optional<PlayerbotEconomy::GatheringProfession> GatheringProfessionForSkill(uint32 skillId)
{
    switch (skillId)
    {
        case SKILL_HERBALISM:
            return PlayerbotEconomy::GatheringProfession::Herbalism;
        case SKILL_MINING:
            return PlayerbotEconomy::GatheringProfession::Mining;
        case SKILL_SKINNING:
            return PlayerbotEconomy::GatheringProfession::Skinning;
        default:
            return std::nullopt;
    }
}

bool CareerContainsSkill(PlayerbotCareerPlan const& plan, uint32 skillId)
{
    return PlayerbotCareer::PlansSkill(plan, static_cast<uint16>(skillId));
}

std::map<uint32, uint32> InventoryCounts(Player* bot)
{
    std::map<uint32, uint32> counts;
    auto const add = [&counts](Item const* item)
    {
        if (item)
            counts[item->GetEntry()] += item->GetCount();
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* const bag = static_cast<Bag const*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            add(bag->GetItemByPos(slot));
    }
    return counts;
}

void ConfirmGatheredLoot(Player* bot, uint32 itemId)
{
    uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());
    std::optional<PlayerbotEconomy::GatheringObservedSuccess> const success =
        PlayerbotEconomy::GetPlayerbotEconomyGathering().ConfirmLoot(bot->GetGUID().GetCounter(), itemId,
                                                                     bot->GetItemCount(itemId), now);
    if (!success)
        return;

    [[maybe_unused]] bool const recorded =
        PlayerbotEconomy::PlayerbotEconomyTraceRuntime(PlayerbotEconomy::GetPlayerbotEconomyTrace())
            .Complete(true, {
                                .deduplicationKey =
                                    Acore::StringFormat("gather:{}:{}", success->characterGuid, success->leaseId),
                                .actorGuid = success->characterGuid,
                                .itemId = success->itemId,
                                .quantity = success->quantity,
                                .occurredAt = now,
                                .kind = PlayerbotEconomy::EconomyTraceKind::Gathered,
                            });
}
}  // namespace

bool EconomyGatheringLootAction::AddLoot(ObjectGuid guid)
{
    if (!sPlayerbotEconomyConfig.lifecycleEnabled || !sRandomPlayerbotMgr.IsRandomBot(bot) ||
        IsRealPlayer(botAI->GetMaster()))
    {
        return AddGatheringLootAction::AddLoot(guid);
    }

    LootObject loot(bot, guid);
    WorldObject* const resourceObject = loot.GetWorldObject(bot);
    std::optional<PlayerbotEconomy::GatheringProfession> const profession = GatheringProfessionForSkill(loot.skillId);
    if (!resourceObject || !profession || !loot.IsLootPossible(bot))
        return false;

    PlayerbotCareerPlan careerPlan;
    bool const hasCareer =
        PlayerbotCareer::EnsurePersistentPlan(bot, careerPlan) && CareerContainsSkill(careerPlan, loot.skillId);
    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(bot->GetGUID().GetCounter());

    float const distance = bot->GetDistance(resourceObject);
    PlayerbotEconomy::GatheringResource resource;
    resource.resourceGuid = guid.GetRawValue();
    resource.profession = *profession;
    resource.mapId = resourceObject->GetMapId();
    resource.phaseMask = resourceObject->GetPhaseMask();
    resource.requiredSkill = loot.reqSkillValue;
    resource.spawned = resourceObject->IsInWorld();

    PlayerbotEconomy::GatheringCandidate candidate;
    candidate.characterGuid = bot->GetGUID().GetCounter();
    candidate.profession = *profession;
    candidate.skillValue = bot->GetSkillValue(loot.skillId);
    candidate.economyAffinity = personality ? personality->gatheringAffinity : 0u;
    candidate.botDistance = distance;
    candidate.formationDistance = distance;
    candidate.lootDistance = sPlayerbotAIConfig.lootDistance;
    candidate.discoveryDistance = sPlayerbotAIConfig.sightDistance;
    candidate.hasCareer = hasCareer;
    candidate.hasLearnedSkill = bot->HasSkill(loot.skillId);
    candidate.grouped = bot->GetGroup() != nullptr;
    candidate.sameMap = bot->GetMapId() == resourceObject->GetMapId();
    candidate.samePhase = (bot->GetPhaseMask() & resourceObject->GetPhaseMask()) != 0u;
    candidate.pathAvailable = candidate.sameMap && WorldPosition(bot).canPathTo(WorldPosition(resourceObject), bot);
    candidate.safe = bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() && !bot->GetTransport() &&
                     !bot->InBattleground() && !bot->IsBeingTeleported();

    // Passive pickup is about bag space: a skill-up, a reagent a planned crafting profession can use within
    // its current rank, or something the bot sells. A dedicated trip already justified the node.
    candidate.skillUpPossible = candidate.skillValue < loot.reqSkillValue + 100u;
    candidate.marketEligible = hasCareer && careerPlan.marketEligible;
    candidate.activeTrip =
        PlayerbotEconomy::GetPlayerbotEconomyGathering().ActiveTripSkill(candidate.characterGuid) == loot.skillId;
    if (hasCareer)
    {
        std::vector<PlayerbotEconomy::PlannedProfessionRank> crafting;
        std::vector<PlayerbotEconomy::PlannedProfessionRank> gathering;
        for (uint16 const skillId : PlayerbotCareer::PlannedSkills(careerPlan))
        {
            std::optional<PlayerbotEconomy::ProfessionCapabilityKind> const kind =
                PlayerbotEconomy::PlayerbotProfessionCapabilityCatalog::ClassifySkill(skillId);
            if (!kind)
                continue;
            PlayerbotEconomy::PlannedProfessionRank const rank{skillId, bot->GetMaxSkillValue(skillId)};
            (*kind == PlayerbotEconomy::ProfessionCapabilityKind::Crafting ? crafting : gathering).push_back(rank);
        }
        if (GatheringTravelDestination* const destination = sPlayerbotEconomyTravelCatalog.FindGatheringDestination(
                loot.skillId, resourceObject->GetEntry(), resourceObject->GetMapId()))
        {
            std::vector<uint32> const yields = destination->YieldItemIds();
            candidate.craftingUsesYield =
                std::any_of(yields.begin(), yields.end(),
                            [&crafting, &gathering](uint32 itemId)
                            {
                                return PlayerbotEconomy::PlayerbotProfessionCapabilityCatalog::CraftingUsesItem(
                                    itemId, crafting, gathering,
                                    &PlayerbotEconomy::PlayerbotProfessionCapabilityCatalog::ReagentUses);
                            });
            candidate.yieldsAtCeiling =
                !yields.empty() &&
                std::all_of(yields.begin(), yields.end(),
                            [this](uint32 itemId)
                            {
                                ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemId);
                                return itemTemplate &&
                                       bot->GetItemCount(itemId) >= 2u * itemTemplate->GetMaxStackSize();
                            });
        }
    }

    PlayerbotEconomy::GatheringClaimResult const claim = PlayerbotEconomy::GetPlayerbotEconomyGathering().ClaimNearby(
        resource, candidate, static_cast<uint64>(GameTime::GetGameTime().count()), NEARBY_GATHERING_LEASE_SECONDS);
    if (!claim.claim)
        return false;

    // The upstream action re-runs checks this function already passed, so a refusal here means the guid is
    // already on the loot stack: an unclaimed "add all loot" pass queued it first, or an earlier lease expired
    // while the entry lived on. The bot loots whatever is on the stack, so the claim must stay leased for the
    // gather to be confirmed and for other bots to stay blocked. Releasing it here made every such node churn
    // claim/release once per gather tick and left its yield unconfirmed.
    [[maybe_unused]] bool const queuedNow = AddGatheringLootAction::AddLoot(guid);
    if (!PlayerbotEconomy::GetPlayerbotEconomyGathering().Observe(*claim.claim, InventoryCounts(bot)))
    {
        [[maybe_unused]] bool const released = PlayerbotEconomy::GetPlayerbotEconomyGathering().Release(
            claim.claim->leaseId, PlayerbotEconomy::GatheringReleaseCause::Disabled);
    }
    return true;
}

void EconomyGatheringLootAction::HandleLoot(PlayerbotAI* botAI, uint32 itemId)
{
    if (!botAI || !botAI->GetBot() || !itemId)
        return;

    Player* const bot = botAI->GetBot();
    Milliseconds const confirmationDelay(std::max(1u, sPlayerbotAIConfig.lootDelay));
    bot->m_Events.AddEventAtOffset([bot, itemId]() { ConfirmGatheredLoot(bot, itemId); }, confirmationDelay);
}

void EconomyGatheringLootAction::Remove(PlayerbotAI* botAI)
{
    if (botAI && botAI->GetBot())
        PlayerbotEconomy::GetPlayerbotEconomyGathering().RemoveActor(botAI->GetBot()->GetGUID().GetCounter());
}
