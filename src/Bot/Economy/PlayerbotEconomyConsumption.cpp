/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyConsumption.h"

#include <algorithm>
#include <limits>
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

uint32 SaturatedUtility(uint64 value)
{
    return static_cast<uint32>(std::min<uint64>(value, std::numeric_limits<uint32>::max()));
}

uint32 EffectTicks(SpellInfo const* spellInfo, SpellEffectInfo const& effect)
{
    if (!spellInfo || !effect.Amplitude)
        return 1u;
    int32 const duration = spellInfo->GetMaxDuration();
    return duration > 0 ? std::max<uint32>(1u, static_cast<uint32>(duration) / effect.Amplitude) : 1u;
}

uint32 ConsumableUtility(Player const* bot, ItemTemplate const* itemTemplate, ConsumableCapability capability)
{
    uint64 utility = 0u;
    for (auto const& itemSpell : itemTemplate->Spells)
    {
        if (itemSpell.SpellId <= 0)
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
        if (!spellInfo)
            continue;
        for (SpellEffectInfo const& effect : spellInfo->Effects)
        {
            uint32 const amount = static_cast<uint32>(std::max(0, effect.CalcValue(bot)));
            bool const health = capability == ConsumableCapability::Food ||
                                capability == ConsumableCapability::HealthRestoration ||
                                capability == ConsumableCapability::Bandage;
            bool const mana =
                capability == ConsumableCapability::Drink || capability == ConsumableCapability::ManaRestoration;
            if (health && (effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_MECHANICAL))
                utility += amount;
            else if (health && effect.Effect == SPELL_EFFECT_HEAL_PCT)
                utility += bot->CountPctFromMaxHealth(amount);
            else if (health && effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                     (effect.ApplyAuraName == SPELL_AURA_OBS_MOD_HEALTH ||
                      effect.ApplyAuraName == SPELL_AURA_MOD_REGEN))
            {
                utility += static_cast<uint64>(amount) * EffectTicks(spellInfo, effect);
            }
            else if (mana && effect.Effect == SPELL_EFFECT_ENERGIZE && effect.MiscValue == POWER_MANA)
                utility += amount;
            else if (mana && effect.Effect == SPELL_EFFECT_ENERGIZE_PCT && effect.MiscValue == POWER_MANA)
                utility += static_cast<uint64>(bot->GetMaxPower(POWER_MANA)) * amount / 100u;
            else if (mana && effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.MiscValue == POWER_MANA &&
                     (effect.ApplyAuraName == SPELL_AURA_OBS_MOD_POWER ||
                      effect.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN))
            {
                utility += static_cast<uint64>(amount) * EffectTicks(spellInfo, effect);
            }
        }
    }
    return SaturatedUtility(utility);
}

std::optional<ConsumableCapability> DescribeConsumable(Player const* bot, ItemTemplate const* itemTemplate)
{
    if (itemTemplate->SubClass == ITEM_SUBCLASS_BANDAGE)
        return ConsumableCapability::Bandage;
    if (itemTemplate->Spells[0].SpellCategory == 11)
        return ConsumableCapability::Food;
    if (itemTemplate->Spells[0].SpellCategory == 59 && bot->GetMaxPower(POWER_MANA))
        return ConsumableCapability::Drink;

    for (auto const& itemSpell : itemTemplate->Spells)
    {
        SpellInfo const* spellInfo = itemSpell.SpellId > 0 ? sSpellMgr->GetSpellInfo(itemSpell.SpellId) : nullptr;
        if (!spellInfo)
            continue;
        for (SpellEffectInfo const& effect : spellInfo->Effects)
        {
            if ((effect.Effect == SPELL_EFFECT_ENERGIZE || effect.Effect == SPELL_EFFECT_ENERGIZE_PCT) &&
                effect.MiscValue == POWER_MANA && bot->GetMaxPower(POWER_MANA))
            {
                return ConsumableCapability::ManaRestoration;
            }
            if (effect.Effect == SPELL_EFFECT_HEAL || effect.Effect == SPELL_EFFECT_HEAL_MECHANICAL ||
                effect.Effect == SPELL_EFFECT_HEAL_PCT)
            {
                return ConsumableCapability::HealthRestoration;
            }
        }
    }
    return std::nullopt;
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

        bool const equipment = need.group.kind == EconomySubstitutionKind::Equipment;
        auto const eligibleOwned = [&need, equipment](ConsumptionOwnedItem const& item)
        {
            return item.compatible &&
                   (equipment ? item.group == need.group : MatchesNeed(need, item.group, item.utility));
        };
        auto const owned =
            std::max_element(snapshot.owned.begin(), snapshot.owned.end(),
                             [&eligibleOwned](ConsumptionOwnedItem const& left, ConsumptionOwnedItem const& right)
                             {
                                 uint32 const leftUtility = eligibleOwned(left) ? left.utility : 0u;
                                 uint32 const rightUtility = eligibleOwned(right) ? right.utility : 0u;
                                 return leftUtility < rightUtility;
                             });
        if (owned != snapshot.owned.end() && owned->compatible && owned->count &&
            MatchesNeed(need, owned->group, owned->utility))
        {
            return FinalUse(need, *owned);
        }

        uint64 const equivalentSupply = EquivalentSupply(need);
        uint64 const pendingSupply = static_cast<uint64>(need.mailQuantity) + need.activePurchaseQuantity +
                                     need.productionQuantity + need.committedPurchaseQuantity;
        bool const ownedEquipment = equipment && owned != snapshot.owned.end() && eligibleOwned(*owned) && owned->count;
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
            if (!offer.compatible || !offer.auctionId || !offer.count || offer.count > remaining ||
                !MatchesNeed(need, offer.group, offer.utility) ||
                (equipment && owned != snapshot.owned.end() && eligibleOwned(*owned) && owned->count &&
                 offer.utility <= owned->utility))
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

ConsumptionNeed PlayerbotEconomyConsumption::BuildNeed(ConsumptionNeedIntent const& intent)
{
    ConsumptionNeed need;
    need.group = EconomySubstitutionGroup::Consumable(intent.capability, intent.requiredUtility);
    need.use = FinishedGoodUse::Consume;
    need.quantity = intent.desiredStock;
    need.requiredUtility = intent.requiredUtility;
    need.protectedBudget = intent.protectedBudget;
    need.remainingUses = intent.desiredStock;
    need.compatibleActivity = intent.compatibleActivity;
    need.ordinaryVendorSupply = intent.ordinaryVendorSupply;
    need.sharedDemandEligible = true;
    return need;
}

std::vector<EconomyDemandFact> PlayerbotEconomyConsumption::DemandFacts(ConsumptionSnapshot const& snapshot)
{
    std::map<EconomySubstitutionGroup, uint32> quantities;
    for (ConsumptionNeed const& need : snapshot.needs)
        if (need.quantity && need.compatibleActivity && need.sharedDemandEligible && !need.ordinaryVendorSupply)
            quantities[need.group] += need.quantity;

    std::vector<EconomyDemandFact> demands;
    demands.reserve(quantities.size());
    for (auto const& [group, quantity] : quantities)
        demands.push_back({group, quantity});
    return demands;
}

bool PlayerbotEconomyConsumption::MatchesNeed(ConsumptionNeed const& need,
                                              EconomySubstitutionGroup const& candidateGroup, uint32 candidateUtility)
{
    if (candidateUtility < need.requiredUtility || candidateGroup.kind != need.group.kind)
        return false;
    if (need.group.kind == EconomySubstitutionKind::Consumable)
        return candidateGroup.effectFamily == need.group.effectFamily;
    return candidateGroup == need.group;
}

std::vector<EconomySupplyFact> PlayerbotEconomyConsumption::SupplyFacts(ConsumptionSnapshot const& snapshot)
{
    using SupplyIdentity = std::tuple<EconomySubstitutionGroup, uint32, EconomySupplySource>;
    std::map<SupplyIdentity, uint32> quantities;
    for (ConsumptionHeldItem const& held : snapshot.held)
    {
        if (!held.itemId || !held.count)
            continue;
        auto const need = std::find_if(snapshot.needs.begin(), snapshot.needs.end(),
                                       [&held](ConsumptionNeed const& item)
                                       {
                                           return item.quantity && item.compatibleActivity &&
                                                  item.sharedDemandEligible &&
                                                  MatchesNeed(item, held.group, held.utility);
                                       });
        if (need != snapshot.needs.end())
            quantities[{need->group, held.itemId, held.source}] += held.count;
    }

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
        std::optional<ConsumableCapability> const capability = DescribeConsumable(bot, itemTemplate);
        if (!capability)
            return std::nullopt;
        uint32 const utility = ConsumableUtility(bot, itemTemplate, *capability);
        return FinishedGoodDescription{EconomySubstitutionGroup::Consumable(*capability, utility),
                                       FinishedGoodUse::Consume, utility};
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
