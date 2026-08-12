/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotEconomyPolicy.h"

#include <algorithm>
#include <limits>
#include <optional>

#include "Bot/Personality/PlayerbotPersonality.h"
#include "SharedDefines.h"

using namespace PlayerbotEconomy;

namespace
{
// These namespaces are independent of personality profile version 1. They only
// partition deterministic economy choices that consume the public SplitMix64 helper.
constexpr uint64 RECIPE_TIE_NAMESPACE = 0x45434F4E52454349ULL;
constexpr uint64 AUCTION_TIE_NAMESPACE = 0x45434F4E41554354ULL;
constexpr uint64 SALE_TIE_NAMESPACE = 0x45434F4E53414C45ULL;
constexpr uint64 CADENCE_NAMESPACE = 0x45434F4E43414445ULL;

InventoryCount const* GetInventory(EconomySnapshot const& snapshot, uint32 itemId)
{
    auto const inventory = std::find_if(snapshot.inventory.begin(), snapshot.inventory.end(),
                                        [itemId](InventoryCount const& item) { return item.itemId == itemId; });
    return inventory == snapshot.inventory.end() ? nullptr : &*inventory;
}

uint32 GetInventoryCount(EconomySnapshot const& snapshot, uint32 itemId)
{
    InventoryCount const* inventory = GetInventory(snapshot, itemId);
    return inventory ? inventory->count : 0u;
}

uint32 GetPlannedInputCount(EconomySnapshot const& snapshot, uint32 itemId)
{
    InventoryCount const* inventory = GetInventory(snapshot, itemId);
    if (!inventory)
        return 0u;

    uint64 const total = static_cast<uint64>(inventory->count) + inventory->mailCount + inventory->purchasedCount +
                         inventory->committedCount;
    return static_cast<uint32>(std::min<uint64>(total, std::numeric_limits<uint32>::max()));
}

bool IsCraftable(EconomySnapshot const& snapshot, RecipeCandidate const& recipe)
{
    return std::all_of(recipe.reagents.begin(), recipe.reagents.end(), [&snapshot](ReagentRequirement const& reagent)
                       { return GetInventoryCount(snapshot, reagent.itemId) >= reagent.count; });
}

uint64 RecipeTieBreak(uint64 guidCounter, uint32 spellId)
{
    return PlayerbotPersonality::SplitMix64(guidCounter ^ spellId ^ RECIPE_TIE_NAMESPACE);
}

bool IsPreferredRecipe(EconomySnapshot const& snapshot, RecipeCandidate const& candidate,
                       RecipeCandidate const& current)
{
    if (candidate.givesSkillUp != current.givesSkillUp)
        return candidate.givesSkillUp;

    if (candidate.outputUsagePriority != current.outputUsagePriority)
        return candidate.outputUsagePriority < current.outputUsagePriority;

    uint64 const candidateTie = RecipeTieBreak(snapshot.guidCounter, candidate.spellId);
    uint64 const currentTie = RecipeTieBreak(snapshot.guidCounter, current.spellId);
    return candidateTie != currentTie ? candidateTie < currentTie : candidate.spellId < current.spellId;
}

RecipeCandidate const* SelectCraftableRecipe(EconomySnapshot const& snapshot)
{
    if (snapshot.preferredRecipeSpellId)
    {
        auto const preferred = std::find_if(
            snapshot.recipes.begin(), snapshot.recipes.end(), [&snapshot](RecipeCandidate const& recipe)
            { return recipe.spellId == snapshot.preferredRecipeSpellId && IsCraftable(snapshot, recipe); });
        if (preferred != snapshot.recipes.end())
            return &*preferred;
    }

    RecipeCandidate const* selected = nullptr;
    for (RecipeCandidate const& recipe : snapshot.recipes)
    {
        if (!IsCraftable(snapshot, recipe))
            continue;

        if (!selected || IsPreferredRecipe(snapshot, recipe, *selected))
            selected = &recipe;
    }
    return selected;
}

RecipeCandidate const* SelectIncompleteRecipe(EconomySnapshot const& snapshot)
{
    if (snapshot.preferredRecipeSpellId)
    {
        auto const preferred = std::find_if(
            snapshot.recipes.begin(), snapshot.recipes.end(), [&snapshot](RecipeCandidate const& recipe)
            { return recipe.spellId == snapshot.preferredRecipeSpellId && !IsCraftable(snapshot, recipe); });
        if (preferred != snapshot.recipes.end())
            return &*preferred;
    }

    RecipeCandidate const* selected = nullptr;
    for (RecipeCandidate const& recipe : snapshot.recipes)
    {
        if (IsCraftable(snapshot, recipe))
            continue;

        if (!selected || IsPreferredRecipe(snapshot, recipe, *selected))
            selected = &recipe;
    }
    return selected;
}

struct ReagentDeficit
{
    uint32 itemId = 0;
    uint32 count = 0;
};

std::optional<ReagentDeficit> SelectNextDeficit(EconomySnapshot const& snapshot, RecipeCandidate const& recipe)
{
    auto const reagent = std::find_if(recipe.reagents.begin(), recipe.reagents.end(),
                                      [&snapshot](ReagentRequirement const& candidate) {
                                          return !candidate.unlimitedGoldVendorSupply &&
                                                 GetPlannedInputCount(snapshot, candidate.itemId) < candidate.count;
                                      });
    if (reagent == recipe.reagents.end())
        return std::nullopt;
    return ReagentDeficit{reagent->itemId, reagent->count - GetPlannedInputCount(snapshot, reagent->itemId)};
}

uint64 BuyerCeiling(AuctionListingCandidate const& auction)
{
    return auction.buyerCeilingPerItem ? auction.buyerCeilingPerItem : auction.templateBuyPrice;
}

bool IsWithinBuyerCeiling(AuctionListingCandidate const& auction)
{
    uint64 const ceiling = BuyerCeiling(auction);
    return ceiling != 0u &&
           static_cast<unsigned __int128>(auction.buyout) <= static_cast<unsigned __int128>(ceiling) * auction.count;
}

bool IsEligibleAuction(EconomySnapshot const& snapshot, ReagentDeficit const& deficit,
                       AuctionListingCandidate const& auction, uint32 committedQuantity, uint64 committedCost)
{
    if (auction.itemId != deficit.itemId || !auction.count || committedQuantity >= deficit.count)
        return false;

    if (!auction.accessible || auction.ownerAccountId == snapshot.botAccountId || !auction.buyout)
        return false;

    if (!auction.reserveCeiling ||
        static_cast<uint64>(GetPlannedInputCount(snapshot, deficit.itemId)) + committedQuantity + auction.count >
            auction.reserveCeiling)
        return false;

    if (committedCost > snapshot.freeMoneyForTradeskill ||
        auction.buyout > snapshot.freeMoneyForTradeskill - committedCost)
        return false;

    return IsWithinBuyerCeiling(auction);
}

uint64 AuctionTieBreak(uint64 guidCounter, uint32 auctionId)
{
    return PlayerbotPersonality::SplitMix64(guidCounter ^ auctionId ^ AUCTION_TIE_NAMESPACE);
}

bool IsPreferredAuction(EconomySnapshot const& snapshot, AuctionListingCandidate const& candidate,
                        AuctionListingCandidate const& current)
{
    uint64 const candidateScaledPrice = candidate.buyout * current.count;
    uint64 const currentScaledPrice = current.buyout * candidate.count;
    if (candidateScaledPrice != currentScaledPrice)
        return candidateScaledPrice < currentScaledPrice;

    if (candidate.count != current.count)
        return candidate.count < current.count;

    uint64 const candidateTie = AuctionTieBreak(snapshot.guidCounter, candidate.auctionId);
    uint64 const currentTie = AuctionTieBreak(snapshot.guidCounter, current.auctionId);
    return candidateTie != currentTie ? candidateTie < currentTie : candidate.auctionId < current.auctionId;
}

std::vector<AuctionListingCandidate const*> SelectAuctions(EconomySnapshot const& snapshot,
                                                           ReagentDeficit const& deficit)
{
    std::vector<AuctionListingCandidate const*> candidates;
    for (AuctionListingCandidate const& auction : snapshot.auctions)
    {
        if (auction.itemId == deficit.itemId && auction.count && auction.accessible &&
            auction.ownerAccountId != snapshot.botAccountId && auction.buyout)
            candidates.push_back(&auction);
    }
    std::sort(candidates.begin(), candidates.end(),
              [&snapshot](AuctionListingCandidate const* left, AuctionListingCandidate const* right)
              { return IsPreferredAuction(snapshot, *left, *right); });

    std::vector<AuctionListingCandidate const*> selected;
    uint32 quantity = 0u;
    uint64 cost = 0u;
    for (AuctionListingCandidate const* auction : candidates)
    {
        if (!IsEligibleAuction(snapshot, deficit, *auction, quantity, cost))
            continue;
        selected.push_back(auction);
        quantity += auction->count;
        cost += auction->buyout;
        if (quantity >= deficit.count)
            break;
    }
    return selected;
}

bool HasPriceBlockedAuction(EconomySnapshot const& snapshot, ReagentDeficit const& deficit)
{
    return std::any_of(snapshot.auctions.begin(), snapshot.auctions.end(),
                       [&snapshot, &deficit](AuctionListingCandidate const& auction)
                       {
                           return auction.itemId == deficit.itemId && auction.count && auction.accessible &&
                                  auction.ownerAccountId != snapshot.botAccountId && auction.buyout &&
                                  !IsWithinBuyerCeiling(auction);
                       });
}

AuctionListingCandidate const* SelectRecipeAuction(EconomySnapshot const& snapshot)
{
    AuctionListingCandidate const* selected = nullptr;
    for (AuctionListingCandidate const& auction : snapshot.auctions)
    {
        if (!auction.recipeEligible || !auction.count || !auction.accessible ||
            auction.ownerAccountId == snapshot.botAccountId || !auction.buyout ||
            auction.buyout > snapshot.freeMoneyForTradeskill || !IsWithinBuyerCeiling(auction))
        {
            continue;
        }

        if (!selected || IsPreferredAuction(snapshot, auction, *selected))
            selected = &auction;
    }

    return selected;
}

bool IsEligibleSale(SaleItemCandidate const& item)
{
    if (!item.professionRelated || !item.itemGuidCounter || !item.itemId || !item.count || item.usage != ITEM_USAGE_AH)
        return false;

    if (!item.canBeTraded || item.bound || (item.container && item.containerItemCount))
        return false;

    if (item.conjured || item.duration || item.alreadyAuctioned)
        return false;

    return true;
}

std::optional<SaleItemCandidate> PrepareSaleCandidate(EconomySnapshot const& snapshot, SaleItemCandidate const& item)
{
    if (!IsEligibleSale(item) || std::find(snapshot.controlledItemGuids.begin(), snapshot.controlledItemGuids.end(),
                                           item.itemGuidCounter) != snapshot.controlledItemGuids.end())
    {
        return std::nullopt;
    }

    uint32 const reserve = PlayerbotEconomyPolicy::EffectiveProfessionReserve(item);
    uint32 const surplus = item.inventoryCount > reserve ? item.inventoryCount - reserve : 0u;
    uint32 const count = std::min(item.count, surplus);
    if (!count)
        return std::nullopt;

    SaleItemCandidate candidate = item;
    candidate.count = count;
    unsigned __int128 const scaledDeposit =
        (static_cast<unsigned __int128>(item.deposit) * count + item.count - 1u) / item.count;
    candidate.deposit =
        static_cast<uint64>(std::min<unsigned __int128>(scaledDeposit, std::numeric_limits<uint64>::max()));
    return candidate;
}

struct SalePricing
{
    uint64 buyout = 0u;
    uint64 startBid = 0u;
};

std::optional<SalePricing> PriceSale(SaleItemCandidate const& item)
{
    uint64 const targetPerItem = item.lowestCompetingBuyoutPerItem
                                     ? item.lowestCompetingBuyoutPerItem
                                     : (item.buyerCeilingPerItem ? item.buyerCeilingPerItem : item.templateBuyPrice);
    if (targetPerItem == 0u || item.count == 0u)
        return std::nullopt;

    uint64 const target = targetPerItem * item.count;
    uint64 const ceiling = (item.buyerCeilingPerItem ? item.buyerCeilingPerItem : targetPerItem) * item.count;
    uint64 const floor = PlayerbotEconomyPolicy::SellerFloor(item);
    if (floor == 0u || floor > ceiling)
        return std::nullopt;

    uint64 const buyout = std::max(floor, std::min(target, ceiling));
    return SalePricing{buyout, floor};
}

uint64 SaleTieBreak(uint64 guidCounter, SaleItemCandidate const& item)
{
    return PlayerbotPersonality::SplitMix64(guidCounter ^ item.itemGuidCounter ^ item.itemId ^ SALE_TIE_NAMESPACE);
}

std::optional<SaleItemCandidate> SelectSale(EconomySnapshot const& snapshot)
{
    std::optional<SaleItemCandidate> selected;
    for (SaleItemCandidate const& item : snapshot.saleItems)
    {
        std::optional<SaleItemCandidate> const candidate = PrepareSaleCandidate(snapshot, item);
        if (!candidate || !PriceSale(*candidate).has_value())
            continue;

        if (!selected)
        {
            selected = *candidate;
            continue;
        }

        uint64 const candidateTie = SaleTieBreak(snapshot.guidCounter, *candidate);
        uint64 const currentTie = SaleTieBreak(snapshot.guidCounter, *selected);
        if (candidateTie < currentTie ||
            (candidateTie == currentTie && candidate->itemGuidCounter < selected->itemGuidCounter))
        {
            selected = *candidate;
        }
    }
    return selected;
}

bool HasPriceBlockedSale(EconomySnapshot const& snapshot)
{
    return std::any_of(snapshot.saleItems.begin(), snapshot.saleItems.end(),
                       [&snapshot](SaleItemCandidate const& item)
                       {
                           std::optional<SaleItemCandidate> const candidate = PrepareSaleCandidate(snapshot, item);
                           return candidate && !PriceSale(*candidate).has_value();
                       });
}
}  // namespace

EconomyDecision PlayerbotEconomyPolicy::Decide(EconomySnapshot const& snapshot)
{
    for (AuctionMailCandidate const& mail : snapshot.auctionMail)
    {
        if (!mail.delivered || (!mail.money && !mail.attachmentCount))
            continue;

        EconomyDecision decision;
        decision.phase = EconomyPhase::CollectAuctionMail;
        decision.mailId = mail.mailId;
        return decision;
    }

    if (RecipeCandidate const* recipe = SelectCraftableRecipe(snapshot))
    {
        EconomyDecision decision;
        decision.phase = EconomyPhase::Craft;
        decision.spellId = recipe->spellId;
        decision.itemId = recipe->craftedItemId;
        return decision;
    }

    if (RecipeCandidate const* recipe = SelectIncompleteRecipe(snapshot))
    {
        if (std::optional<ReagentDeficit> const deficit = SelectNextDeficit(snapshot, *recipe))
        {
            std::vector<AuctionListingCandidate const*> const auctions = SelectAuctions(snapshot, *deficit);
            if (!auctions.empty())
            {
                EconomyDecision decision;
                decision.phase = EconomyPhase::BuyReagent;
                decision.spellId = recipe->spellId;
                decision.itemId = deficit->itemId;
                for (AuctionListingCandidate const* auction : auctions)
                {
                    decision.purchases.push_back(
                        {auction->auctionId, auction->itemId, auction->count, auction->buyout});
                    decision.count += auction->count;
                    decision.buyout += auction->buyout;
                }
                decision.auctionId = decision.purchases.front().auctionId;
                return decision;
            }
            if (HasPriceBlockedAuction(snapshot, *deficit))
            {
                EconomyDecision decision;
                decision.blocker = EconomyDecisionBlocker::PriceCorridor;
                return decision;
            }
        }
    }

    if (AuctionListingCandidate const* auction = SelectRecipeAuction(snapshot))
    {
        EconomyDecision decision;
        decision.phase = EconomyPhase::BuyRecipe;
        decision.itemId = auction->itemId;
        decision.auctionId = auction->auctionId;
        decision.count = auction->count;
        decision.buyout = auction->buyout;
        decision.recipeSpellId = auction->recipeSpellId;
        return decision;
    }

    if (std::optional<SaleItemCandidate> const item = SelectSale(snapshot))
    {
        SalePricing const pricing = *PriceSale(*item);

        EconomyDecision decision;
        decision.phase = EconomyPhase::SellSurplus;
        decision.itemId = item->itemId;
        decision.count = item->count;
        decision.buyout = pricing.buyout;
        decision.itemGuidCounter = item->itemGuidCounter;
        decision.startBid = pricing.startBid;
        decision.deposit = item->deposit;
        decision.buyerCeilingPerItem = item->buyerCeilingPerItem;
        decision.lowestCompetingBuyoutPerItem = item->lowestCompetingBuyoutPerItem;
        decision.auctionCutBasisPoints = item->auctionCutBasisPoints;
        decision.professionReserveFloor = EffectiveProfessionReserve(*item);
        return decision;
    }

    if (HasPriceBlockedSale(snapshot))
    {
        EconomyDecision decision;
        decision.blocker = EconomyDecisionBlocker::PriceCorridor;
        return decision;
    }

    return {};
}

EconomyWorkBlocker PlayerbotEconomyPolicy::EvaluateWork(EconomyWorkPolicyInput const& input)
{
    if (!input.legal)
        return EconomyWorkBlocker::Illegal;
    if (!input.withinBudget)
        return EconomyWorkBlocker::Budget;
    if (input.sameAccountPurchase)
        return EconomyWorkBlocker::SameAccountPurchase;
    if (!input.liveObject)
        return EconomyWorkBlocker::MissingLiveObject;
    if (!input.pathAvailable)
        return EconomyWorkBlocker::MissingPath;
    if (!input.hasSkill)
        return EconomyWorkBlocker::MissingSkill;
    if (!input.phaseAllowed)
        return EconomyWorkBlocker::WrongPhase;
    if (!input.transactionSafe)
        return EconomyWorkBlocker::UnsafeTransaction;
    if (input.kind == EconomyWorkKind::MarketMaking)
    {
        if (input.directCommand)
            return EconomyWorkBlocker::AutonomousOnly;
        return input.economyAffinity >= 75u ? EconomyWorkBlocker::None : EconomyWorkBlocker::AffinityTooLow;
    }
    if (!input.directCommand && input.economyAffinity < 25u)
        return EconomyWorkBlocker::AffinityTooLow;
    return EconomyWorkBlocker::None;
}

char const* PlayerbotEconomyPolicy::WorkBlockerName(EconomyWorkBlocker blocker)
{
    switch (blocker)
    {
        case EconomyWorkBlocker::None:
            return "none";
        case EconomyWorkBlocker::UnknownActor:
            return "unknown_actor";
        case EconomyWorkBlocker::Offline:
            return "offline";
        case EconomyWorkBlocker::NotAutonomous:
            return "not_autonomous";
        case EconomyWorkBlocker::WrongMarket:
            return "wrong_market";
        case EconomyWorkBlocker::NoDemand:
            return "no_demand";
        case EconomyWorkBlocker::AffinityTooLow:
            return "affinity_too_low";
        case EconomyWorkBlocker::AutonomousOnly:
            return "autonomous_only";
        case EconomyWorkBlocker::Illegal:
            return "illegal";
        case EconomyWorkBlocker::Budget:
            return "budget";
        case EconomyWorkBlocker::AccountIdentityUnavailable:
            return "account_identity_unavailable";
        case EconomyWorkBlocker::SameAccountPurchase:
            return "same_account_purchase";
        case EconomyWorkBlocker::MissingLiveObject:
            return "missing_live_object";
        case EconomyWorkBlocker::MissingPath:
            return "missing_path";
        case EconomyWorkBlocker::MissingSkill:
            return "missing_skill";
        case EconomyWorkBlocker::WrongPhase:
            return "wrong_phase";
        case EconomyWorkBlocker::UnsafeTransaction:
            return "unsafe_transaction";
        case EconomyWorkBlocker::Capacity:
            return "capacity";
    }

    return "unknown";
}

bool PlayerbotEconomyPolicy::IsEligible(EconomyEligibility const& eligibility)
{
    return eligibility.enabled && eligibility.randomBot && !eligibility.activePlayerMaster && !eligibility.inCombat &&
           !eligibility.inBattleground && !eligibility.dead && !eligibility.teleporting &&
           eligibility.careerMarketEligible && eligibility.hasActionableProfessionWork;
}

bool PlayerbotEconomyPolicy::IsProfessionRecipeSpell(uint32 effect, uint32 craftedItemId, int32 firstReagentCount,
                                                     uint32 schoolMask)
{
    return effect == SPELL_EFFECT_CREATE_ITEM && craftedItemId != 0 && firstReagentCount > 0 &&
           schoolMask == SPELL_SCHOOL_MASK_NORMAL;
}

bool PlayerbotEconomyPolicy::IsUnlimitedGoldVendorOffer(uint32 maximumCount, uint32 extendedCost)
{
    return maximumCount == 0u && extendedCost == 0u;
}

bool PlayerbotEconomyPolicy::IsKnownRecipeOutput(EconomySnapshot const& snapshot, uint32 itemId)
{
    return itemId && std::any_of(snapshot.recipes.begin(), snapshot.recipes.end(),
                                 [itemId](RecipeCandidate const& recipe) { return recipe.craftedItemId == itemId; });
}

bool PlayerbotEconomyPolicy::PreservesProfessionReserve(uint32 inventoryCount, uint32 saleCount, uint32 reserveFloor)
{
    return reserveFloor == 0 || static_cast<uint64>(inventoryCount) >= static_cast<uint64>(saleCount) + reserveFloor;
}

uint32 PlayerbotEconomyPolicy::EffectiveProfessionReserve(SaleItemCandidate const& item)
{
    return item.pureGatheringMaterial ? 0u : item.professionReserveFloor;
}

bool PlayerbotEconomyPolicy::IsCirculationMaterial(uint32 itemClass, uint32 itemSubclass)
{
    if (itemClass != ITEM_CLASS_TRADE_GOODS)
        return false;
    return itemSubclass == ITEM_SUBCLASS_CLOTH || itemSubclass == ITEM_SUBCLASS_HERB ||
           itemSubclass == ITEM_SUBCLASS_METAL_STONE || itemSubclass == ITEM_SUBCLASS_LEATHER;
}

uint64 PlayerbotEconomyPolicy::SellerFloor(SaleItemCandidate const& item)
{
    unsigned __int128 const vendorValue = static_cast<unsigned __int128>(item.templateSellPrice) * item.count;
    unsigned __int128 basis = std::max<unsigned __int128>(
        vendorValue, std::max<unsigned __int128>(item.allocatedInputCost, item.minimumTransactionBasis));
    basis += item.deposit;

    uint32 const cutBasisPoints = std::min(item.auctionCutBasisPoints, 9'999u);
    unsigned __int128 const denominator = 10'000u - cutBasisPoints;
    unsigned __int128 const gross = (basis * 10'000u + denominator - 1u) / denominator;
    return static_cast<uint64>(std::min<unsigned __int128>(gross, std::numeric_limits<uint64>::max()));
}

uint32 PlayerbotEconomyPolicy::ProductionReserve(EconomySnapshot const& snapshot, uint32 itemId,
                                                 uint32 configuredReserve)
{
    uint32 immediateUse = 0u;
    for (RecipeCandidate const& recipe : snapshot.recipes)
    {
        auto const reagent = std::find_if(recipe.reagents.begin(), recipe.reagents.end(),
                                          [itemId](ReagentRequirement const& value) { return value.itemId == itemId; });
        if (reagent != recipe.reagents.end())
            immediateUse = std::max(immediateUse, reagent->count);
    }
    return static_cast<uint32>(
        std::min<uint64>(static_cast<uint64>(immediateUse) + configuredReserve, std::numeric_limits<uint32>::max()));
}

uint32 PlayerbotEconomyPolicy::ProductionBatchQuantity(RecipeCandidate const& recipe, EconomySnapshot const& snapshot,
                                                       uint32 ceiling)
{
    // Auction stock counts as obtainable only under the exact rules the purchase selector
    // applies (ownership, buyout, reserve ceiling, buyer ceiling), with one tradeskill
    // budget shared across every reagent of the batch.
    uint64 sharedCost = 0u;
    uint64 feasible = std::numeric_limits<uint32>::max();
    for (ReagentRequirement const& reagent : recipe.reagents)
    {
        if (!reagent.count)
            continue;
        if (reagent.unlimitedGoldVendorSupply)
            continue;

        uint64 available = 0u;
        auto const held =
            std::find_if(snapshot.inventory.begin(), snapshot.inventory.end(),
                         [&reagent](InventoryCount const& value) { return value.itemId == reagent.itemId; });
        if (held != snapshot.inventory.end())
            available += static_cast<uint64>(held->count) + held->mailCount;

        std::vector<AuctionListingCandidate const*> candidates;
        for (AuctionListingCandidate const& auction : snapshot.auctions)
        {
            if (auction.itemId == reagent.itemId && auction.count && auction.accessible &&
                auction.ownerAccountId != snapshot.botAccountId && auction.buyout)
            {
                candidates.push_back(&auction);
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [&snapshot](AuctionListingCandidate const* left, AuctionListingCandidate const* right)
                  { return IsPreferredAuction(snapshot, *left, *right); });

        ReagentDeficit const probe{reagent.itemId, std::numeric_limits<uint32>::max()};
        uint32 purchased = 0u;
        for (AuctionListingCandidate const* auction : candidates)
        {
            if (!IsEligibleAuction(snapshot, probe, *auction, purchased, sharedCost))
                continue;
            purchased += auction->count;
            sharedCost += auction->buyout;
            available += auction->count;
        }
        feasible = std::min(feasible, available / reagent.count);
    }

    if (ceiling)
        feasible = std::min<uint64>(feasible, ceiling);
    return static_cast<uint32>(feasible);
}

uint64 PlayerbotEconomyPolicy::InitialEligibleTime(uint64 now, uint64 guidCounter, uint32 intervalSeconds)
{
    uint32 const interval = std::max(1u, intervalSeconds);
    return now + PlayerbotPersonality::SplitMix64(guidCounter ^ CADENCE_NAMESPACE) % interval;
}

uint32 PlayerbotEconomyPolicy::CareerIntervalSeconds(uint32 intervalSeconds, uint8 engagement)
{
    uint32 const interval = std::max(1u, intervalSeconds);
    uint32 factor = 4u;
    if (engagement >= 100u)
        factor = 1u;
    else if (engagement >= 75u)
        factor = 2u;
    else if (engagement >= 50u)
        factor = 3u;

    return interval * factor;
}

uint64 PlayerbotEconomyPolicy::NextEligibleTime(uint64 now, uint32 intervalSeconds, EconomyAttemptOutcome outcome,
                                                uint8 consecutiveFailures)
{
    uint64 const interval = std::max(1u, intervalSeconds);
    if (outcome == EconomyAttemptOutcome::Operation)
        return now + interval;

    uint8 exponent = std::max<uint8>(1u, consecutiveFailures);
    if (outcome == EconomyAttemptOutcome::NoCandidate)
        ++exponent;
    exponent = std::min<uint8>(5u, exponent);
    return now + interval * (uint64(1) << exponent);
}
