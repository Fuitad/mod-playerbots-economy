/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyConsumption.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

#include "ItemTemplate.h"
#include "Player.h"
#include "SpellMgr.h"

using namespace PlayerbotEconomy;

namespace
{
bool IsRecurring(ConsumptionNeed const& need)
{
    return need.group.kind == EconomySubstitutionKind::Ammunition ||
           need.group.kind == EconomySubstitutionKind::Consumable;
}

uint64 EquivalentSupply(ConsumptionNeed const& need)
{
    return static_cast<uint64>(need.inventoryQuantity) + need.mailQuantity + need.activePurchaseQuantity +
           need.productionQuantity + need.committedPurchaseQuantity;
}

ConsumptionDecision Recovery(ConsumptionNeed const& need)
{
    ConsumptionDecision decision;
    decision.action = ConsumptionAction::Recovery;
    decision.blocker = ConsumptionBlocker::None;
    decision.group = need.group;
    decision.use = FinishedGoodUse::Recover;
    decision.count = need.committedPurchaseQuantity;
    return decision;
}

ConsumptionDecision FinalUse(ConsumptionNeed const& need, ConsumptionOwnedItem const& item)
{
    ConsumptionDecision decision;
    decision.action = ConsumptionAction::FinalUse;
    decision.blocker = ConsumptionBlocker::None;
    decision.group = need.group;
    decision.use = need.use;
    decision.itemGuidCounter = item.itemGuidCounter;
    decision.itemId = item.itemId;
    decision.count = 1u;
    return decision;
}

ConsumptionDecision Purchase(ConsumptionNeed const& need, ConsumptionOffer const& offer)
{
    ConsumptionDecision decision;
    decision.action = ConsumptionAction::Purchase;
    decision.blocker = ConsumptionBlocker::None;
    decision.group = need.group;
    decision.use = need.use;
    decision.itemId = offer.itemId;
    decision.auctionId = offer.auctionId;
    decision.count = offer.count;
    decision.buyout = offer.buyout;
    return decision;
}
}  // namespace

ConsumptionDecision PlayerbotEconomyConsumption::Decide(ConsumptionSnapshot const& snapshot)
{
    ConsumptionBlocker blocker = ConsumptionBlocker::NoOffer;
    for (ConsumptionNeed const& need : snapshot.needs)
    {
        if (!need.quantity)
            continue;

        if (need.committedPurchaseQuantity && !need.committedPurchaseStillUseful)
            return Recovery(need);

        if (IsRecurring(need) && (!need.compatibleActivity || !need.remainingUses))
        {
            blocker = ConsumptionBlocker::ActivityStopped;
            continue;
        }

        auto const owned = std::max_element(
            snapshot.owned.begin(), snapshot.owned.end(),
            [&need](ConsumptionOwnedItem const& left, ConsumptionOwnedItem const& right)
            {
                uint32 const leftUtility = left.group == need.group && left.compatible ? left.utility : 0u;
                uint32 const rightUtility = right.group == need.group && right.compatible ? right.utility : 0u;
                return leftUtility < rightUtility;
            });
        if (owned != snapshot.owned.end() && owned->group == need.group && owned->compatible && owned->count &&
            owned->utility >= need.requiredUtility)
        {
            return FinalUse(need, *owned);
        }

        bool const equipment = need.group.kind == EconomySubstitutionKind::Equipment;
        uint64 const equivalentSupply = EquivalentSupply(need);
        uint64 const pendingSupply = static_cast<uint64>(need.mailQuantity) + need.activePurchaseQuantity +
                                     need.productionQuantity + need.committedPurchaseQuantity;
        bool const ownedEquipment = equipment && owned != snapshot.owned.end() && owned->group == need.group &&
                                    owned->compatible && owned->count;
        if (equivalentSupply >= need.quantity && (!equipment || pendingSupply >= need.quantity || !ownedEquipment))
        {
            blocker = ConsumptionBlocker::EquivalentSupply;
            continue;
        }
        uint32 const remaining = need.quantity - static_cast<uint32>(equivalentSupply);

        ConsumptionOffer const* best = nullptr;
        bool rejectedSameAccount = false;
        bool rejectedCorridor = false;
        for (ConsumptionOffer const& offer : snapshot.offers)
        {
            if (offer.group != need.group || !offer.compatible || !offer.auctionId || !offer.count ||
                offer.count > remaining || offer.utility < need.requiredUtility ||
                (equipment && owned != snapshot.owned.end() && owned->group == need.group && owned->compatible &&
                 owned->count && offer.utility <= owned->utility))
            {
                continue;
            }
            if (offer.ownerAccountId && offer.ownerAccountId == snapshot.botAccountId)
            {
                rejectedSameAccount = true;
                continue;
            }

            uint64 const unitPrice = (offer.buyout + offer.count - 1u) / offer.count;
            if (!offer.buyout || !need.buyerCeilingPerItem || unitPrice > need.buyerCeilingPerItem ||
                offer.buyout > need.protectedBudget)
            {
                rejectedCorridor = true;
                continue;
            }

            if (!best || offer.utility > best->utility ||
                (offer.utility == best->utility && offer.buyout < best->buyout) ||
                (offer.utility == best->utility && offer.buyout == best->buyout && offer.auctionId < best->auctionId))
            {
                best = &offer;
            }
        }

        if (best)
            return Purchase(need, *best);
        if (rejectedCorridor)
            blocker = ConsumptionBlocker::PriceCorridor;
        else if (rejectedSameAccount)
            blocker = ConsumptionBlocker::SameAccount;
    }

    ConsumptionDecision decision;
    decision.blocker = blocker;
    return decision;
}

std::vector<EconomySupplyFact> PlayerbotEconomyConsumption::SupplyFacts(ConsumptionSnapshot const& snapshot)
{
    std::map<EconomySubstitutionGroup, bool> eligibleGroups;
    for (ConsumptionNeed const& need : snapshot.needs)
        if (need.quantity && need.compatibleActivity)
            eligibleGroups[need.group] = true;

    using SupplyIdentity = std::tuple<EconomySubstitutionGroup, uint32, EconomySupplySource>;
    std::map<SupplyIdentity, uint32> quantities;
    for (ConsumptionHeldItem const& held : snapshot.held)
        if (held.itemId && held.count && eligibleGroups.contains(held.group))
            quantities[{held.group, held.itemId, held.source}] += held.count;

    std::vector<EconomySupplyFact> supplies;
    supplies.reserve(quantities.size());
    for (auto const& [identity, quantity] : quantities)
    {
        auto const& [group, itemId, source] = identity;
        supplies.push_back({group, quantity, source, itemId});
    }
    return supplies;
}

std::optional<FinishedGoodDescription> PlayerbotEconomyConsumption::Describe(Player const* bot,
                                                                             ItemTemplate const* itemTemplate)
{
    if (!bot || !itemTemplate)
        return std::nullopt;

    uint8 const tier = static_cast<uint8>(std::min<uint32>(itemTemplate->RequiredLevel / 10u, 255u));
    if (itemTemplate->Class == ITEM_CLASS_CONTAINER)
    {
        return FinishedGoodDescription{EconomySubstitutionGroup::Bag(itemTemplate->ContainerSlots),
                                       FinishedGoodUse::Equip, itemTemplate->ContainerSlots};
    }
    if (itemTemplate->Class == ITEM_CLASS_PROJECTILE)
    {
        uint32 const utility = static_cast<uint32>(
            (itemTemplate->Damage[0].DamageMin + itemTemplate->Damage[0].DamageMax) * 1000.0f / 2.0f);
        return FinishedGoodDescription{EconomySubstitutionGroup::Ammunition(itemTemplate->SubClass, tier),
                                       FinishedGoodUse::SetAmmunition, utility};
    }
    if (itemTemplate->Class == ITEM_CLASS_ARMOR || itemTemplate->Class == ITEM_CLASS_WEAPON)
    {
        uint32 const roleMask = bot->getClass() ? 1u << (bot->getClass() - 1u) : 0u;
        return FinishedGoodDescription{EconomySubstitutionGroup::Equipment(itemTemplate->InventoryType, roleMask, tier),
                                       FinishedGoodUse::Equip, itemTemplate->ItemLevel};
    }

    for (auto const& itemSpell : itemTemplate->Spells)
    {
        if (itemSpell.SpellId <= 0)
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
        if (spellInfo && (spellInfo->Targets & TARGET_FLAG_ITEM))
        {
            return FinishedGoodDescription{EconomySubstitutionGroup::Enhancement(itemSpell.SpellId, tier),
                                           FinishedGoodUse::Apply, itemTemplate->ItemLevel};
        }
    }

    if (itemTemplate->Class == ITEM_CLASS_CONSUMABLE)
    {
        uint32 const effectFamily = itemTemplate->Spells[0].SpellCategory > 0
                                        ? static_cast<uint32>(itemTemplate->Spells[0].SpellCategory)
                                        : itemTemplate->SubClass;
        return FinishedGoodDescription{EconomySubstitutionGroup::Consumable(effectFamily, tier),
                                       FinishedGoodUse::Consume,
                                       std::max<uint32>(itemTemplate->ItemLevel, itemTemplate->RequiredLevel)};
    }
    return std::nullopt;
}

bool PlayerbotEconomyConsumption::IsMarketEquipment(uint32 itemClass, uint32 quality, ItemUsage usage)
{
    return (itemClass == ITEM_CLASS_ARMOR || itemClass == ITEM_CLASS_WEAPON) && quality >= ITEM_QUALITY_UNCOMMON &&
           (usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP);
}

char const* PlayerbotEconomyConsumption::BlockerName(ConsumptionBlocker blocker)
{
    switch (blocker)
    {
        case ConsumptionBlocker::None:
            return "none";
        case ConsumptionBlocker::ActivityStopped:
            return "activity_stopped";
        case ConsumptionBlocker::EquivalentSupply:
            return "equivalent_supply";
        case ConsumptionBlocker::SameAccount:
            return "same_account_purchase";
        case ConsumptionBlocker::PriceCorridor:
            return "price_corridor";
        case ConsumptionBlocker::NoOffer:
            return "no_finished_good_offer";
    }
    return "no_finished_good_offer";
}

std::string PlayerbotEconomyConsumption::GroupKey(EconomySubstitutionGroup const& group)
{
    std::ostringstream key;
    key << "finished:" << static_cast<uint32>(group.kind) << ':' << group.exactItemId << ':'
        << static_cast<uint32>(group.equipmentSlot) << ':' << group.roleMask << ':' << group.bagCapacity << ':'
        << group.ammunitionType << ':' << static_cast<uint32>(group.tier) << ':' << group.effectFamily << ':'
        << group.enhancementTarget << ':' << group.valueBand;
    return key.str();
}
