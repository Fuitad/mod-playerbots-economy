/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotEconomyPolicy.h"

#include <algorithm>
#include <cmath>
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

// The first vendor-supplied reagent the bags are short of. Vendor reagents never form a market deficit,
// so without this step a recipe whose only missing input is an Empty Vial or a Coarse Thread stalls.
std::optional<ReagentDeficit> SelectVendorDeficit(EconomySnapshot const& snapshot, RecipeCandidate const& recipe)
{
    auto const reagent = std::find_if(recipe.reagents.begin(), recipe.reagents.end(),
                                      [&snapshot](ReagentRequirement const& candidate) {
                                          return candidate.unlimitedGoldVendorSupply &&
                                                 GetInventoryCount(snapshot, candidate.itemId) < candidate.count;
                                      });
    if (reagent == recipe.reagents.end())
        return std::nullopt;
    return ReagentDeficit{reagent->itemId, reagent->count - GetInventoryCount(snapshot, reagent->itemId)};
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
                       AuctionListingCandidate const& auction, uint32 committedQuantity, uint64 committedCost,
                       uint64 budgetOverride = 0u)
{
    uint64 const budget = std::max(budgetOverride, snapshot.freeMoneyForTradeskill);
    if (auction.itemId != deficit.itemId || !auction.count || committedQuantity >= deficit.count)
        return false;

    if (!auction.accessible || auction.ownerAccountId == snapshot.botAccountId || !auction.buyout)
        return false;

    if (!auction.reserveCeiling ||
        static_cast<uint64>(GetPlannedInputCount(snapshot, deficit.itemId)) + committedQuantity + auction.count >
            auction.reserveCeiling)
        return false;

    if (committedCost > budget || auction.buyout > budget - committedCost)
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

// A green the bot can break into the missing reagent, for when nobody lists the reagent itself. One
// listing at a time: a disenchant yields a handful of dust, and the next cycle looks again.
AuctionListingCandidate const* SelectDisenchantSourceAuction(EconomySnapshot const& snapshot,
                                                             ReagentDeficit const& deficit)
{
    AuctionListingCandidate const* selected = nullptr;
    for (AuctionListingCandidate const& auction : snapshot.auctions)
    {
        if (std::find(auction.disenchantYieldItemIds.begin(), auction.disenchantYieldItemIds.end(), deficit.itemId) ==
            auction.disenchantYieldItemIds.end())
        {
            continue;
        }
        ReagentDeficit const source{auction.itemId, auction.count};
        if (!IsEligibleAuction(snapshot, source, auction, 0u, 0u, snapshot.disenchantFodderMoney))
            continue;
        if (!selected || IsPreferredAuction(snapshot, auction, *selected))
            selected = &auction;
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

bool IsEligibleSale(EconomySnapshot const& snapshot, SaleItemCandidate const& item)
{
    // A skill-flagged material is still a sale past its reserve: the reserve floor, not the usage
    // flag, says how much of it the bot's own recipes need.
    bool const eligibleCategory = item.unusable || item.unwantedEquipment || item.unwantedMaterial ||
                                  (snapshot.careerEligible && item.professionRelated);
    if (!eligibleCategory || !item.itemGuidCounter || !item.itemId || !item.count ||
        (item.usage != ITEM_USAGE_AH && item.usage != ITEM_USAGE_SKILL))
    {
        return false;
    }

    if (!item.canBeTraded || item.bound || (item.container && item.containerItemCount))
        return false;

    if (item.conjured || item.duration || item.alreadyAuctioned)
        return false;

    // The deposit is paid up front; a listing the bot cannot pay for is refused by the auction house
    // and the retry quarantines the bot.
    if (item.deposit > snapshot.money)
        return false;

    if (!PlayerbotEconomyPolicy::AllowsAutonomousListing(
            {item.ordinaryVendorSupply, item.trainingOutput, item.independentDemand, item.circulationMaterial}))
        return false;

    // Looted junk gear stays off the market; the bot's own profession goods keep their listing.
    if (!item.professionRelated && PlayerbotEconomyPolicy::IsUnmarketableEquipment(item.itemClass, item.quality))
        return false;

    return true;
}

std::optional<SaleItemCandidate> PrepareSaleCandidate(EconomySnapshot const& snapshot, SaleItemCandidate const& item)
{
    if (!IsEligibleSale(snapshot, item) ||
        std::find(snapshot.controlledItemGuids.begin(), snapshot.controlledItemGuids.end(), item.itemGuidCounter) !=
            snapshot.controlledItemGuids.end())
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

// demandedOnly restricts the pick to surplus another bot is waiting on (coordinator demand).
std::optional<SaleItemCandidate> SelectSale(EconomySnapshot const& snapshot, bool demandedOnly = false)
{
    std::optional<SaleItemCandidate> selected;
    for (SaleItemCandidate const& item : snapshot.saleItems)
    {
        if (demandedOnly && !item.independentDemand)
            continue;
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

EconomyDecision SaleDecision(SaleItemCandidate const& item)
{
    SalePricing const pricing = *PriceSale(item);

    EconomyDecision decision;
    decision.phase = EconomyPhase::SellSurplus;
    decision.itemId = item.itemId;
    decision.count = item.count;
    decision.buyout = pricing.buyout;
    decision.itemGuidCounter = item.itemGuidCounter;
    decision.startBid = pricing.startBid;
    decision.deposit = item.deposit;
    decision.buyerCeilingPerItem = item.buyerCeilingPerItem;
    decision.lowestCompetingBuyoutPerItem = item.lowestCompetingBuyoutPerItem;
    decision.auctionCutBasisPoints = item.auctionCutBasisPoints;
    decision.professionReserveFloor = PlayerbotEconomyPolicy::EffectiveProfessionReserve(item);
    decision.requiresUnusableItem = item.unusable;
    return decision;
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

    RecipeCandidate const* const craftable = snapshot.careerEligible ? SelectCraftableRecipe(snapshot) : nullptr;
    auto const craft = [craftable]
    {
        EconomyDecision decision;
        decision.phase = EconomyPhase::Craft;
        decision.spellId = craftable->spellId;
        decision.itemId = craftable->craftedItemId;
        return decision;
    };
    if (craftable && craftable->givesSkillUp)
        return craft();

    // A maxed smelter or tailor can craft forever; surplus another bot is waiting on goes to the
    // auction house before the next batch, or the supply chain never sees it.
    if (std::optional<SaleItemCandidate> const demanded = SelectSale(snapshot, true))
        return SaleDecision(*demanded);

    if (craftable)
        return craft();

    bool purchaseBlockedByPrice = false;
    if (snapshot.careerEligible)
    {
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
                if (AuctionListingCandidate const* source = SelectDisenchantSourceAuction(snapshot, *deficit))
                {
                    EconomyDecision decision;
                    decision.phase = EconomyPhase::BuyReagent;
                    decision.spellId = recipe->spellId;
                    decision.itemId = source->itemId;
                    decision.disenchantSourcePurchase = true;
                    decision.purchases.push_back({source->auctionId, source->itemId, source->count, source->buyout});
                    decision.auctionId = source->auctionId;
                    decision.count = source->count;
                    decision.buyout = source->buyout;
                    return decision;
                }
                // Listings over the buyer ceiling are reported, but they must not stop the bot from
                // listing its own surplus below: that surplus is what other bots are waiting on.
                purchaseBlockedByPrice = HasPriceBlockedAuction(snapshot, *deficit);
            }
            else if (std::optional<ReagentDeficit> const vendorDeficit = SelectVendorDeficit(snapshot, *recipe))
            {
                // Every market reagent is in hand or on its way; the cheap vendor inputs come last so a
                // recipe that never gets its herbs does not leave a bag full of vials behind.
                EconomyDecision decision;
                decision.phase = EconomyPhase::BuyReagent;
                decision.vendorPurchase = true;
                decision.spellId = recipe->spellId;
                decision.itemId = vendorDeficit->itemId;
                decision.count = vendorDeficit->count;
                return decision;
            }
        }
    }

    if (snapshot.careerEligible)
    {
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
    }

    if (std::optional<SaleItemCandidate> const item = SelectSale(snapshot))
        return SaleDecision(*item);

    if (purchaseBlockedByPrice || HasPriceBlockedSale(snapshot))
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
    if (input.kind == EconomyWorkKind::Buy && input.necessaryPurchase)
        return EconomyWorkBlocker::None;
    if (!input.directCommand && input.economyAffinity < 25u && !input.affinityRelaxed)
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

bool PlayerbotEconomyPolicy::IsLifecycleSafe(EconomyEligibility const& eligibility)
{
    return eligibility.enabled && eligibility.randomBot && !eligibility.activePlayerMaster && !eligibility.inCombat &&
           !eligibility.inBattleground && !eligibility.dead && !eligibility.teleporting && !eligibility.brokenEquipment;
}

bool PlayerbotEconomyPolicy::HasCareerCapability(EconomyEligibility const& eligibility)
{
    return eligibility.careerMarketEligible && eligibility.hasActionableProfessionWork;
}

bool PlayerbotEconomyPolicy::IsTransientlyUnsafe(EconomyEligibility const& eligibility)
{
    if (IsLifecycleSafe(eligibility))
        return false;
    EconomyEligibility settled = eligibility;
    settled.inCombat = false;
    settled.teleporting = false;
    // brokenEquipment is deliberately not settled here. Combat and a teleport pass on their own;
    // broken gear does not, and the repair that clears it needs the travel target this cycle holds.
    return IsLifecycleSafe(settled);
}

bool PlayerbotEconomyPolicy::IsProfessionRecipeSpell(uint32 effect, uint32 craftedItemId, int32 firstReagentCount,
                                                     uint32 schoolMask)
{
    // An enchant names the scroll it writes on a vellum as its item type, so it fits the same shape.
    return (effect == SPELL_EFFECT_CREATE_ITEM || effect == SPELL_EFFECT_ENCHANT_ITEM) && craftedItemId != 0 &&
           firstReagentCount > 0 && schoolMask == SPELL_SCHOOL_MASK_NORMAL;
}

bool PlayerbotEconomyPolicy::IsUnlimitedGoldVendorOffer(uint32 maximumCount, uint32 extendedCost)
{
    return maximumCount == 0u && extendedCost == 0u;
}

bool PlayerbotEconomyPolicy::IsApplicableUnlimitedGoldVendorOffer(VendorOfferPolicyInput const& input)
{
    return IsUnlimitedGoldVendorOffer(input.maximumCount, input.extendedCost) && input.factionAllowed &&
           input.levelAllowed && input.reputationAllowed && input.sameMap && input.routeAvailable;
}

bool PlayerbotEconomyPolicy::VendorSellAllowed(ItemUsage usage, bool purseEmergency)
{
    return usage == ITEM_USAGE_VENDOR || (purseEmergency && usage == ITEM_USAGE_AH);
}

bool PlayerbotEconomyPolicy::BagPressure(uint8 bagSpacePercent) { return bagSpacePercent > BAG_PRESSURE_PERCENT; }

uint64 PlayerbotEconomyPolicy::BagPurchaseBudget(uint64 money, uint64 repairReserve)
{
    return money > repairReserve ? money - repairReserve : 0u;
}

uint64 PlayerbotEconomyPolicy::ConsumablePurchaseBudget(uint64 money, uint64 repairReserve)
{
    return BagPurchaseBudget(money, repairReserve) / 10u;
}

bool PlayerbotEconomyPolicy::IsBagPressureVendorSale(uint32 quality, uint32 itemClass, ItemUsage usage, bool unusable)
{
    if (quality == ITEM_QUALITY_POOR)
        return itemClass != ITEM_CLASS_QUEST;
    if (quality != ITEM_QUALITY_NORMAL)
        return false;
    // A special bag the bot cannot put to use (a herb bag without Herbalism) is dead weight too.
    if (itemClass == ITEM_CLASS_CONTAINER)
        return unusable;
    if (itemClass != ITEM_CLASS_ARMOR && itemClass != ITEM_CLASS_WEAPON && itemClass != ITEM_CLASS_CONSUMABLE)
        return false;
    if (unusable)
        return true;
    switch (usage)
    {
        case ITEM_USAGE_EQUIP:
        case ITEM_USAGE_REPLACE:
        case ITEM_USAGE_QUEST:
        case ITEM_USAGE_SKILL:
        case ITEM_USAGE_USE:
        case ITEM_USAGE_GUILD_TASK:
        case ITEM_USAGE_DISENCHANT:
        case ITEM_USAGE_KEEP:
        case ITEM_USAGE_AMMO:
            return false;
        default:
            return true;
    }
}

bool PlayerbotEconomyPolicy::IsUnusableSustenance(uint32 spellCategory, uint32 requiredLevel, bool botHasMana,
                                                  uint32 botLevel)
{
    // A drink is dead weight to a bot with no mana pool: it can never be drunk, at any level.
    if (spellCategory == SUSTENANCE_DRINK_SPELL_CATEGORY)
        return !botHasMana;

    // Food tiers sit roughly ten levels apart (1, 5, 15, 25, 35, ...). Selling anything a full tier
    // below the bot keeps the current tier and the one just behind it, so a bot is never stripped of
    // the only food it can actually buy at its level.
    if (spellCategory == SUSTENANCE_FOOD_SPELL_CATEGORY)
        return static_cast<uint64>(requiredLevel) + SUSTENANCE_OUTGROWN_LEVEL_MARGIN <= botLevel;

    return false;
}

bool PlayerbotEconomyPolicy::IsUnmarketableEquipment(uint32 itemClass, uint32 quality)
{
    return (itemClass == ITEM_CLASS_ARMOR || itemClass == ITEM_CLASS_WEAPON) && quality < ITEM_QUALITY_UNCOMMON;
}

bool PlayerbotEconomyPolicy::AllowsAutonomousListing(AutonomousListingPolicyInput const& input)
{
    // Skill-up gear and potions wait for a chain to ask; a skill-up bar or bolt is itself an input
    // other professions wait on, so it lists beyond the reserve like any surplus.
    return !input.ordinaryVendorSupply &&
           (!input.trainingOutput || input.independentDemand || input.circulationMaterial);
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
           itemSubclass == ITEM_SUBCLASS_METAL_STONE || itemSubclass == ITEM_SUBCLASS_LEATHER ||
           itemSubclass == ITEM_SUBCLASS_ENCHANTING || itemSubclass == ITEM_SUBCLASS_ARMOR_ENCHANTMENT ||
           itemSubclass == ITEM_SUBCLASS_WEAPON_ENCHANTMENT;
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
    // The configured stacks are a buffer for a recipe the bot still levels with. A maxed smelter or
    // tailor crafts whatever it holds, so keeping two stacks of its input off the market only starves
    // the bots waiting on that input: past the skill-up, one batch is the whole reserve.
    uint32 immediateUse = 0u;
    bool skillUpInput = false;
    for (RecipeCandidate const& recipe : snapshot.recipes)
    {
        for (ReagentRequirement const& reagent : recipe.reagents)
        {
            // A herb that mills into a pigment reagent is an input of that recipe too: one casting
            // of it makes the pigment. Without this a scribe listed the very herbs its ink waited on.
            bool const millsIntoReagent =
                std::find(reagent.millingInputItemIds.begin(), reagent.millingInputItemIds.end(), itemId) !=
                reagent.millingInputItemIds.end();
            if (reagent.itemId != itemId && !millsIntoReagent)
                continue;
            immediateUse = std::max(immediateUse, millsIntoReagent ? MILLING_HERBS_PER_CAST : reagent.count);
            skillUpInput = skillUpInput || recipe.givesSkillUp;
        }
    }
    uint64 const reserve = static_cast<uint64>(immediateUse) + (skillUpInput ? configuredReserve : 0u);
    return static_cast<uint32>(std::min<uint64>(reserve, std::numeric_limits<uint32>::max()));
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

bool PlayerbotEconomyPolicy::IsTransientNoCandidate(std::string_view blocker)
{
    return blocker == "profession_material_intent_latent";
}

RidingRankNeed PlayerbotEconomyPolicy::EvaluateRidingRank(
    uint32 level, uint32 ridingSkill, playerbots::maintenance::MountLevelThresholds const& thresholds)
{
    uint32 const targetSkill =
        playerbots::maintenance::RequiredRidingSkill(playerbots::maintenance::RequiredMountTier(level, thresholds));
    return {targetSkill != 0u && ridingSkill < targetSkill, targetSkill};
}

uint32 PlayerbotEconomyPolicy::RidingBudget(uint32 freeMoney, uint32 professionNeed, uint32 consumableNeed)
{
    uint64 const reserved = static_cast<uint64>(professionNeed) + consumableNeed;
    if (reserved >= freeMoney)
        return 0u;
    return static_cast<uint32>(freeMoney - reserved);
}

// 10s: covers apprentice and journeyman lessons in full, and is small enough that a poor bot
// spending all of it cannot meaningfully dent the travel or consumable lanes it bypasses.
constexpr uint32 PROFESSION_TRAINING_FLOOR_COPPER = 1000u;

uint32 PlayerbotEconomyPolicy::ProfessionTrainingBudget(uint32 freeTradeskillMoney, uint32 money, uint32 repairNeed)
{
    uint32 const floorBudget = money > repairNeed ? std::min(money - repairNeed, PROFESSION_TRAINING_FLOOR_COPPER) : 0u;
    return std::max(freeTradeskillMoney, floorBudget);
}

uint64 PlayerbotEconomyPolicy::DisenchantFodderBudget(uint64 freeTradeskillMoney, uint64 money, uint64 repairReserve)
{
    uint64 const spendable = BagPurchaseBudget(money, repairReserve);
    uint64 const aboveTrainingFloor =
        spendable > PROFESSION_TRAINING_FLOOR_COPPER ? spendable - PROFESSION_TRAINING_FLOOR_COPPER : 0u;
    return std::max(freeTradeskillMoney, aboveTrainingFloor);
}

// Level 6: Apprentice teach spells require level 5, and the stay home rule confines a level 5 bot
// to its own zone, so 6 is the first level at which a trainer trip can generally succeed.
constexpr uint8 PROFESSION_PIPELINE_MINIMUM_LEVEL = 6u;

bool PlayerbotEconomyPolicy::ProfessionPipelineOpen(uint8 botLevel)
{
    return botLevel >= PROFESSION_PIPELINE_MINIMUM_LEVEL;
}

bool PlayerbotEconomyPolicy::TrainerTripInFlight(bool trainerSelected, bool ownsTravelTarget)
{
    return trainerSelected && ownsTravelTarget;
}

TrainerStageObjective PlayerbotEconomyPolicy::ChooseTrainerStageObjective(TrainerStageFacts const& facts)
{
    // A progression objective is driven by the progression producer, not re-selected here.
    if (facts.activeObjective && facts.activeIsProgression)
        return TrainerStageObjective::KeepActive;
    // Riding takes the stage when nothing is travelling, and keeps it when the trip in flight is its own.
    if (facts.ridingWanted && (!facts.tripInFlight || facts.activeIsRiding))
        return TrainerStageObjective::Riding;
    // Riding is wanted and something else is already travelling. That trip finishes first, and it is
    // kept rather than re-selected: re-selection is what would cancel it. A career goal that changed
    // mid trip is therefore served one trip late, which is the price of the bot arriving at all.
    if (facts.ridingWanted && facts.activeObjective && facts.tripInFlight)
        return TrainerStageObjective::KeepActive;
    if (facts.careerPhasesAllowed && facts.professionEligible)
        return TrainerStageObjective::SelectProfession;
    return TrainerStageObjective::None;
}

char const* PlayerbotEconomyPolicy::IdleBlocker(bool careerCapable)
{
    return careerCapable ? "consumption_idle" : "career_ineligible";
}

uint64 PlayerbotEconomyPolicy::NextEligibleTime(uint64 now, uint32 intervalSeconds, EconomyAttemptOutcome outcome,
                                                uint8 consecutiveFailures, bool transientNoCandidate)
{
    if (outcome == EconomyAttemptOutcome::InProgress)
        return now + 1u;
    if (outcome == EconomyAttemptOutcome::Tracking)
        return now + PLAYERBOT_ECONOMY_TRIP_POLL_SECONDS;

    uint64 const interval = std::max(1u, intervalSeconds);
    if (outcome == EconomyAttemptOutcome::Idle || outcome == EconomyAttemptOutcome::Operation)
        return now + interval;

    if (transientNoCandidate && outcome == EconomyAttemptOutcome::NoCandidate)
        return now + interval * 2u;
    uint8 exponent = std::max<uint8>(1u, consecutiveFailures);
    if (outcome == EconomyAttemptOutcome::NoCandidate)
        ++exponent;
    exponent = std::min<uint8>(5u, exponent);
    return now + interval * (uint64(1) << exponent);
}

EconomyApproachPoint PlayerbotEconomy::ApproachPoint(float objectX, float objectY, float botX, float botY,
                                                     float distance, uint32 seed)
{
    constexpr float pi = 3.14159265358979f;
    float const dx = botX - objectX;
    float const dy = botY - objectY;
    // Fan of +-60 degrees around the approach direction, fixed per seed so a bot commits to one spot.
    float const fan = (static_cast<float>(seed % 1000u) / 999.0f - 0.5f) * (2.0f * pi / 3.0f);
    float const base =
        (dx * dx + dy * dy) > 0.01f ? std::atan2(dy, dx) : 2.0f * pi * static_cast<float>(seed % 360u) / 360.0f;
    float const angle = base + fan;
    return {objectX + std::cos(angle) * distance, objectY + std::sin(angle) * distance};
}

std::vector<float> PlayerbotEconomy::SpellFocusStandOffDistances(uint32 focusRange)
{
    std::vector<float> distances = {SPELL_FOCUS_STAND_OFF_DISTANCE};
    float const farthest = static_cast<float>(focusRange) / 2.0f + 1.0f;
    for (float distance = SPELL_FOCUS_STAND_OFF_DISTANCE + 1.0f; distance <= farthest; distance += 1.0f)
        distances.push_back(distance);
    return distances;
}
