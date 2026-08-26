/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyConsumption.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>

#include "DBCStores.h"
#include "Item.h"
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

bool IsFactoryClassReagent(uint32 itemId)
{
    static constexpr std::array<uint32, 25> reagentIds = {
        5'175u,  5'176u,  5'177u,  5'178u,  6'265u,  17'020u, 17'021u, 17'026u, 17'028u,
        17'029u, 17'030u, 17'031u, 17'032u, 17'034u, 17'035u, 17'036u, 17'037u, 17'038u,
        21'177u, 22'147u, 22'148u, 37'201u, 44'605u, 44'614u, 44'615u,
    };
    return std::find(reagentIds.begin(), reagentIds.end(), itemId) != reagentIds.end();
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

ConsumptionDecision VendorPurchase(ConsumptionNeed const& need, ConsumptionVendorOffer const& offer, uint32 bundleCount)
{
    ConsumptionDecision decision;
    decision.action = ConsumptionAction::VendorPurchase;
    decision.blocker = ConsumptionBlocker::None;
    decision.group = need.group;
    decision.use = need.use;
    decision.itemId = offer.itemId;
    decision.count = offer.bundleSize * bundleCount;
    decision.vendorBundleCount = bundleCount;
    decision.buyout = offer.bundlePrice * bundleCount;
    decision.protectedBudget = need.protectedBudget;
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
            // A drink carries its restoration in a SECOND, periodic dummy effect. Its
            // MOD_POWER_REGEN effect above holds base points of -1, which clamps to zero, so
            // scoring only that effect gave every drink in the game a utility of 0 and
            // MatchesNeed rejects any candidate below the need's required utility. No bot could
            // buy any drink: live on 2026-08-25, 144 of 156 online mana users held none at all,
            // while food, whose MOD_REGEN effect carries a real value, was bought normally. The
            // base points rank correctly by tier (Refreshing Spring Water 41 through Morning
            // Glory Dew 488), which is what the offer decision needs.
            else if (capability == ConsumableCapability::Drink && effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                     effect.ApplyAuraName == SPELL_AURA_PERIODIC_DUMMY)
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

bool InventoryTypeMasksMatch(uint32 requiredMask, uint32 candidateMask)
{
    return !requiredMask || !candidateMask || (requiredMask & candidateMask) != 0u;
}

bool GemColorsMatch(uint32 socketColor, uint32 gemColor)
{
    if (!socketColor || !gemColor)
        return false;
    bool const metaSocket = socketColor == SOCKET_COLOR_META;
    bool const metaGem = gemColor == SOCKET_COLOR_META;
    if (metaSocket || metaGem)
        return metaSocket && metaGem;
    return (socketColor & gemColor) != 0u;
}
}  // namespace

ConsumptionDecision PlayerbotEconomyConsumption::Decide(ConsumptionSnapshot const& snapshot)
{
    ConsumptionBlocker blocker = ConsumptionBlocker::None;
    ConsumptionDecision pendingFinalUse;
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
        auto const eligibleOwned = [&need, equipment](ConsumptionOwnedItem const& item) {
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
        if (need.finalUseNeeded && owned != snapshot.owned.end() && owned->compatible && owned->count &&
            MatchesNeed(need, owned->group, owned->utility))
        {
            // A final use no longer ends the scan. An owned item can qualify for its final use every
            // cycle (an already equipped ring kept "equipping" itself live, 2026-08-23), and returning
            // here starved every later need's purchase forever. Capture the first final use, keep
            // looking for a purchase, and fall back to the final use when nothing is purchasable.
            if (pendingFinalUse.action == ConsumptionAction::None)
                pendingFinalUse = FinalUse(need, *owned);
        }

        uint64 const equivalentSupply = EquivalentSupply(need);
        uint64 const pendingSupply = static_cast<uint64>(need.mailQuantity) + need.activePurchaseQuantity +
                                     need.productionQuantity + need.committedPurchaseQuantity;
        bool const ownedEquipment = equipment && owned != snapshot.owned.end() && eligibleOwned(*owned) && owned->count;
        // Restock when the bot drops BELOW its reorder point, then refill all the way to `quantity`
        // in one trip. Clamped so a reorder point above the target can never invert the shortfall.
        uint32 const restockThreshold = need.reorderPoint ? std::min(need.reorderPoint, need.quantity) : need.quantity;
        if (equivalentSupply >= restockThreshold && (!equipment || pendingSupply >= need.quantity || !ownedEquipment))
        {
            blocker = ConsumptionBlocker::EquivalentSupply;
            continue;
        }
        uint32 const remaining = need.quantity - static_cast<uint32>(equivalentSupply);
        if (snapshot.workTripInFlight)
        {
            blocker = ConsumptionBlocker::WorkTripInFlight;
            continue;
        }

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

        ConsumptionVendorOffer const* bestVendor = nullptr;
        uint32 bestVendorBundles = 0u;
        uint64 bestVendorPrice = 0u;
        for (ConsumptionVendorOffer const& offer : snapshot.vendorOffers)
        {
            if (!offer.compatible || !offer.itemId || !offer.bundleSize ||
                !MatchesNeed(need, offer.group, offer.utility))
            {
                continue;
            }

            uint64 const wantedBundles = (static_cast<uint64>(remaining) + offer.bundleSize - 1u) / offer.bundleSize;
            uint32 const boundedBundles = static_cast<uint32>(
                std::min<uint64>(wantedBundles, static_cast<uint64>(std::numeric_limits<uint8>::max())));
            uint32 const affordableBundles =
                offer.bundlePrice
                    ? static_cast<uint32>(std::min<uint64>(boundedBundles, need.protectedBudget / offer.bundlePrice))
                    : boundedBundles;
            if (!affordableBundles)
            {
                rejectedCorridor = true;
                continue;
            }

            uint64 const price = offer.bundlePrice * affordableBundles;
            if (!bestVendor || offer.utility > bestVendor->utility ||
                (offer.utility == bestVendor->utility && price < bestVendorPrice) ||
                (offer.utility == bestVendor->utility && price == bestVendorPrice && offer.itemId < bestVendor->itemId))
            {
                bestVendor = &offer;
                bestVendorBundles = affordableBundles;
                bestVendorPrice = price;
            }
        }
        if (bestVendor)
            return VendorPurchase(need, *bestVendor, bestVendorBundles);
        if (rejectedCorridor)
            blocker = ConsumptionBlocker::PriceCorridor;
        else if (rejectedSameAccount)
            blocker = ConsumptionBlocker::SameAccount;
        else if (blocker == ConsumptionBlocker::None)
            blocker = ConsumptionBlocker::NoOffer;
    }

    if (pendingFinalUse.action == ConsumptionAction::FinalUse)
        return pendingFinalUse;

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

std::optional<ConsumptionNeed> PlayerbotEconomyConsumption::BuildBagNeed(BagNeedFacts const& facts)
{
    if (facts.affordableCapacities.empty())
        return std::nullopt;

    uint16 const targetCapacity =
        *std::max_element(facts.affordableCapacities.begin(), facts.affordableCapacities.end());
    if (!targetCapacity)
        return std::nullopt;

    uint32 quantity = facts.emptyBagSlots;
    quantity += static_cast<uint32>(std::count_if(facts.equippedCapacities.begin(), facts.equippedCapacities.end(),
                                                  [targetCapacity](uint16 capacity)
                                                  { return static_cast<uint32>(capacity) + 4u <= targetCapacity; }));
    if (!quantity)
        return std::nullopt;

    ConsumptionNeed need;
    need.group = EconomySubstitutionGroup::Bag(targetCapacity);
    need.use = FinishedGoodUse::Equip;
    need.quantity = quantity;
    need.requiredUtility = targetCapacity;
    need.compatibleActivity = true;
    need.remainingUses = quantity;
    need.protectedBudget = facts.protectedBudget;
    return need;
}

std::vector<ClassReagentStock> PlayerbotEconomyConsumption::ClassReagentNeeds(uint8 playerClass, uint8 level,
                                                                              bool hasShamanRelic)
{
    std::vector<ClassReagentStock> items;
    switch (playerClass)
    {
        case CLASS_DEATH_KNIGHT:
            if (level >= 56u)
                items.push_back({37'201u, 40u});
            break;
        case CLASS_DRUID:
            if (level >= 20u && level < 30u)
                items.push_back({17'034u, 20u});
            else if (level >= 30u && level < 40u)
                items.push_back({17'035u, 20u});
            else if (level >= 40u && level < 50u)
                items.push_back({17'036u, 20u});
            else if (level >= 50u && level < 60u)
                items = {{17'037u, 20u}, {17'021u, 20u}};
            else if (level >= 60u && level < 69u)
                items = {{17'038u, 20u}, {17'026u, 20u}};
            else if (level == 69u)
                items = {{22'147u, 20u}, {17'026u, 20u}};
            else if (level >= 70u && level < 79u)
                items = {{22'147u, 20u}, {22'148u, 20u}};
            else if (level == 79u)
                items = {{44'614u, 20u}, {22'148u, 20u}};
            else if (level >= 80u)
                items = {{44'614u, 20u}, {44'605u, 20u}};
            break;
        case CLASS_MAGE:
            if (level >= 20u)
                items.push_back({17'031u, 20u});
            if (level >= 40u)
                items.push_back({17'032u, 20u});
            if (level >= 56u)
                items.push_back({17'020u, 20u});
            break;
        case CLASS_PALADIN:
            if (level >= 52u)
                items.push_back({21'177u, 100u});
            break;
        case CLASS_PRIEST:
            if (level >= 48u && level < 56u)
                items.push_back({17'028u, 40u});
            else if (level >= 56u && level < 60u)
                items = {{17'028u, 20u}, {17'029u, 20u}};
            else if (level >= 60u && level < 77u)
                items.push_back({17'029u, 40u});
            else if (level >= 77u && level < 80u)
                items = {{17'029u, 20u}, {44'615u, 20u}};
            else if (level >= 80u)
                items.push_back({44'615u, 40u});
            break;
        case CLASS_SHAMAN:
            if (!hasShamanRelic)
            {
                if (level >= 4u)
                    items.push_back({5'175u, 1u});
                if (level >= 10u)
                    items.push_back({5'176u, 1u});
                if (level >= 20u)
                    items.push_back({5'177u, 1u});
                if (level >= 30u)
                    items.push_back({5'178u, 1u});
            }
            if (level >= 30u)
                items.push_back({17'030u, 20u});
            break;
        case CLASS_WARLOCK:
            items.push_back({6'265u, 5u});
            break;
        default:
            break;
    }
    return items;
}

RecurringStockReconciliation PlayerbotEconomyConsumption::ReconcileRecurringStock(RecurringStockFacts const& facts)
{
    RecurringStockReconciliation result;
    uint64 const uncappedDesiredStock = static_cast<uint64>(facts.expectedUses) + facts.safetyReserve;
    result.desiredStock = static_cast<uint32>(std::min<uint64>(uncappedDesiredStock, facts.carryingBudget));

    uint32 const adequateSupply = std::min(facts.adequateCurrentAndPendingSupply, result.desiredStock);
    uint32 const preDeliveryUses = std::min(facts.usesBeforeDevelopmentalDelivery, result.desiredStock);
    result.bridgeQuantity = preDeliveryUses > adequateSupply ? preDeliveryUses - adequateSupply : 0u;

    uint32 const uncoveredAfterBridge = result.desiredStock - adequateSupply - result.bridgeQuantity;
    if (facts.developmentalPathViable)
    {
        result.developmentalReservationQuantity =
            std::min(facts.credibleDevelopmentalDeliveryQuantity, uncoveredAfterBridge);
    }
    else
        result.developmentalRejectionReason = facts.developmentalRejectionReason;

    result.residualUncoveredQuantity = uncoveredAfterBridge - result.developmentalReservationQuantity;
    return result;
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
    switch (need.group.kind)
    {
        case EconomySubstitutionKind::Consumable:
            return candidateGroup.effectFamily == need.group.effectFamily;
        case EconomySubstitutionKind::Bag:
            return candidateGroup.bagCapacity >= need.group.bagCapacity;
        case EconomySubstitutionKind::Enhancement:
            return candidateGroup.enhancementSlot == need.group.enhancementSlot &&
                   InventoryTypeMasksMatch(need.group.enhancementTarget, candidateGroup.enhancementTarget);
        case EconomySubstitutionKind::Glyph:
            return candidateGroup.glyphSpellId == need.group.glyphSpellId &&
                   candidateGroup.glyphSlotType == need.group.glyphSlotType;
        case EconomySubstitutionKind::Gem:
            return GemColorsMatch(need.group.gemColor, candidateGroup.gemColor);
        default:
            return candidateGroup == need.group;
    }
}

bool PlayerbotEconomyConsumption::BelowRestorationThreshold(uint32 current, uint32 maximum, uint32 thresholdPercent)
{
    if (!maximum)
        return false;
    uint32 const boundedThreshold = std::min(thresholdPercent, 100u);
    return static_cast<uint64>(current) * 100u < static_cast<uint64>(maximum) * boundedThreshold;
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
    if (!itemTemplate)
        return std::nullopt;

    if (IsFactoryClassReagent(itemTemplate->ItemId))
    {
        return FinishedGoodDescription{EconomySubstitutionGroup::ExactReagent(itemTemplate->ItemId),
                                       FinishedGoodUse::Retain, 0u};
    }
    if (!bot)
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

    if (itemTemplate->Class == ITEM_CLASS_GLYPH)
    {
        for (auto const& itemSpell : itemTemplate->Spells)
        {
            SpellInfo const* spellInfo = itemSpell.SpellId > 0 ? sSpellMgr->GetSpellInfo(itemSpell.SpellId) : nullptr;
            if (!spellInfo)
                continue;
            for (SpellEffectInfo const& effect : spellInfo->Effects)
            {
                if (effect.Effect != SPELL_EFFECT_APPLY_GLYPH || effect.MiscValue <= 0)
                    continue;
                GlyphPropertiesEntry const* glyph =
                    sGlyphPropertiesStore.LookupEntry(static_cast<uint32>(effect.MiscValue));
                if (glyph)
                    return DescribeGlyph(glyph->SpellId, glyph->TypeFlags);
            }
        }
        return std::nullopt;
    }

    if (itemTemplate->Class == ITEM_CLASS_GEM)
    {
        GemPropertiesEntry const* gem = sGemPropertiesStore.LookupEntry(itemTemplate->GemProperties);
        return gem ? DescribeGem(gem->color, gem->spellitemenchantement, 1u) : std::nullopt;
    }

    for (auto const& itemSpell : itemTemplate->Spells)
    {
        if (itemSpell.SpellId <= 0)
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
        if (!spellInfo || !(spellInfo->Targets & TARGET_FLAG_ITEM))
            continue;
        for (SpellEffectInfo const& effect : spellInfo->Effects)
        {
            bool const permanent = effect.Effect == SPELL_EFFECT_ENCHANT_ITEM;
            bool temporary = effect.Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY;
            if ((!permanent && !temporary) || effect.MiscValue <= 0)
                continue;
            uint32 const enchantmentId = static_cast<uint32>(effect.MiscValue);
            SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(enchantmentId);
            if (!enchantment ||
                (enchantment->slot != PERM_ENCHANTMENT_SLOT && enchantment->slot != TEMP_ENCHANTMENT_SLOT))
                continue;
            temporary = temporary || enchantment->slot == TEMP_ENCHANTMENT_SLOT;
            uint32 const targetMask = temporary
                                          ? 1u << INVTYPE_WEAPONMAINHAND
                                          : static_cast<uint32>(std::max(0, spellInfo->EquippedItemInventoryTypeMask));
            uint8 const enchantmentSlot = static_cast<uint8>(enchantment->slot);
            return DescribeEnhancement(targetMask, enchantmentSlot, enchantmentId,
                                       std::max<uint32>(1u, itemTemplate->ItemLevel));
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

std::optional<FinishedGoodDescription> PlayerbotEconomyConsumption::DescribeEnhancement(uint32 targetInventoryTypeMask,
                                                                                        uint8 enchantmentSlot,
                                                                                        uint32 enchantmentId,
                                                                                        uint32 utility)
{
    if (!enchantmentId)
        return std::nullopt;
    return FinishedGoodDescription{EconomySubstitutionGroup::Enhancement(targetInventoryTypeMask, enchantmentSlot, 0u),
                                   FinishedGoodUse::Apply, utility, enchantmentId};
}

std::optional<FinishedGoodDescription> PlayerbotEconomyConsumption::DescribeGlyph(uint32 glyphSpellId,
                                                                                  uint32 glyphSlotType)
{
    if (!glyphSpellId || !glyphSlotType)
        return std::nullopt;
    return FinishedGoodDescription{EconomySubstitutionGroup::Glyph(glyphSpellId, glyphSlotType), FinishedGoodUse::Apply,
                                   1u};
}

std::optional<FinishedGoodDescription> PlayerbotEconomyConsumption::DescribeGem(uint32 gemColor, uint32 enchantmentId,
                                                                                uint32 utility)
{
    if (!gemColor || !enchantmentId)
        return std::nullopt;
    return FinishedGoodDescription{EconomySubstitutionGroup::Gem(gemColor), FinishedGoodUse::Apply, utility,
                                   enchantmentId};
}

std::vector<uint8> PlayerbotEconomyConsumption::UnlockedGlyphSlots(uint32 level)
{
    std::vector<uint8> slots;
    if (level >= 15u)
    {
        slots.push_back(0u);
        slots.push_back(1u);
    }
    if (level >= 30u)
        slots.push_back(3u);
    if (level >= 50u)
        slots.push_back(2u);
    if (level >= 70u)
        slots.push_back(4u);
    if (level >= 80u)
        slots.push_back(5u);
    return slots;
}

ConsumptionNeed PlayerbotEconomyConsumption::BuildGlyphNeed(uint32 glyphSpellId, uint32 glyphSlotType,
                                                            uint64 protectedBudget)
{
    ConsumptionNeed need;
    need.group = EconomySubstitutionGroup::Glyph(glyphSpellId, glyphSlotType);
    need.use = FinishedGoodUse::Apply;
    need.quantity = 1u;
    need.requiredUtility = 1u;
    need.protectedBudget = protectedBudget;
    need.remainingUses = 1u;
    need.compatibleActivity = true;
    need.sharedDemandEligible = true;
    return need;
}

std::vector<ConsumptionNeed> PlayerbotEconomyConsumption::BuildGemNeeds(std::vector<uint32> const& emptySocketColors,
                                                                        uint64 protectedBudget)
{
    std::map<uint32, uint32> quantities;
    for (uint32 color : emptySocketColors)
        if (color)
            ++quantities[color];

    std::vector<ConsumptionNeed> needs;
    needs.reserve(quantities.size());
    for (auto const& [color, quantity] : quantities)
    {
        ConsumptionNeed need;
        need.group = EconomySubstitutionGroup::Gem(color);
        need.use = FinishedGoodUse::Apply;
        need.quantity = quantity;
        need.requiredUtility = 1u;
        need.protectedBudget = protectedBudget;
        need.remainingUses = quantity;
        need.compatibleActivity = true;
        need.sharedDemandEligible = true;
        needs.push_back(std::move(need));
    }
    return needs;
}

std::optional<EnhancementTargetSelection> PlayerbotEconomyConsumption::SelectEnhancementTarget(
    bool mainHandOnly, uint32 targetInventoryTypeMask, uint32 candidateUtility,
    std::vector<EnhancementTargetCandidate> const& candidates)
{
    EnhancementTargetCandidate const* selected = nullptr;
    for (EnhancementTargetCandidate const& candidate : candidates)
    {
        if (!candidate.fitsSpellRequirements || (mainHandOnly && !candidate.mainHand) ||
            !InventoryTypeMasksMatch(targetInventoryTypeMask, candidate.inventoryTypeMask) ||
            candidate.existingUtility >= candidateUtility)
        {
            continue;
        }
        if (!selected || candidate.existingUtility < selected->existingUtility ||
            (candidate.existingUtility == selected->existingUtility &&
             candidate.equipmentSlot < selected->equipmentSlot))
        {
            selected = &candidate;
        }
    }
    if (!selected)
        return std::nullopt;
    return EnhancementTargetSelection{selected->equipmentSlot, selected->existingUtility};
}

std::optional<GemSocketTargetSelection> PlayerbotEconomyConsumption::SelectGemTarget(
    uint32 requiredSocketColor, uint32 gemColor, std::vector<GemSocketTargetCandidate> const& candidates)
{
    if (!GemColorsMatch(requiredSocketColor, gemColor))
        return std::nullopt;
    auto const selected = std::find_if(candidates.begin(), candidates.end(),
                                       [requiredSocketColor, gemColor](GemSocketTargetCandidate const& item) {
                                           return !item.occupied && item.socketColor == requiredSocketColor &&
                                                  GemColorsMatch(item.socketColor, gemColor);
                                       });
    if (selected == candidates.end())
        return std::nullopt;
    return GemSocketTargetSelection{selected->equipmentSlot, selected->socketIndex};
}

bool PlayerbotEconomyConsumption::IsMarketEquipment(uint32 itemClass, uint32 quality, ItemUsage usage)
{
    return (itemClass == ITEM_CLASS_ARMOR || itemClass == ITEM_CLASS_WEAPON) && quality >= ITEM_QUALITY_UNCOMMON &&
           (usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP);
}

bool PlayerbotEconomyConsumption::IsStuckBlocker(ConsumptionBlocker blocker)
{
    return blocker != ConsumptionBlocker::None;
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
        case ConsumptionBlocker::WorkTripInFlight:
            return "work_trip_in_flight";
    }
    return "no_finished_good_offer";
}

std::string PlayerbotEconomyConsumption::GroupKey(EconomySubstitutionGroup const& group)
{
    std::ostringstream key;
    key << "finished:" << static_cast<uint32>(group.kind) << ':' << group.exactItemId << ':'
        << static_cast<uint32>(group.equipmentSlot) << ':' << group.roleMask << ':' << group.bagCapacity << ':'
        << group.ammunitionType << ':' << static_cast<uint32>(group.tier) << ':' << group.effectFamily << ':'
        << group.enhancementTarget << ':' << static_cast<uint32>(group.enhancementSlot) << ':' << group.glyphSpellId
        << ':' << group.glyphSlotType << ':' << group.gemColor << ':' << group.valueBand;
    return key.str();
}
