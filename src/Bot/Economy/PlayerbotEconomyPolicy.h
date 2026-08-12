/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H
#define PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H

#include <vector>

#include "Ai/Base/Value/ItemUsageValue.h"
#include "Define.h"

namespace PlayerbotEconomy
{
enum class EconomyPhase : uint8
{
    None,
    CollectAuctionMail,
    Craft,
    BuyReagent,
    BuyRecipe,
    BuyFinishedGood,
    UseFinishedGood,
    RecoverFinishedGood,
    SellSurplus
};

enum class EconomyAttemptOutcome : uint8
{
    Operation,
    NoCandidate,
    FailedPrecondition
};

enum class EconomyWorkKind : uint8
{
    Craft,
    Gather,
    Buy,
    Sell,
    Recipe,
    Trainer,
    MarketMaking
};

enum class EconomyWorkBlocker : uint8
{
    None,
    UnknownActor,
    Offline,
    NotAutonomous,
    WrongMarket,
    NoDemand,
    AffinityTooLow,
    AutonomousOnly,
    Illegal,
    Budget,
    AccountIdentityUnavailable,
    SameAccountPurchase,
    MissingLiveObject,
    MissingPath,
    MissingSkill,
    WrongPhase,
    UnsafeTransaction,
    Capacity
};

struct EconomyWorkPolicyInput
{
    EconomyWorkKind kind = EconomyWorkKind::Craft;
    uint8 economyAffinity = 0;
    bool affinityRelaxed = false;
    bool directCommand = false;
    bool legal = true;
    bool withinBudget = true;
    bool sameAccountPurchase = false;
    bool liveObject = true;
    bool pathAvailable = true;
    bool hasSkill = true;
    bool phaseAllowed = true;
    bool transactionSafe = true;
};

struct EconomyEligibility
{
    bool enabled = true;
    bool randomBot = true;
    bool activePlayerMaster = false;
    bool inCombat = false;
    bool inBattleground = false;
    bool dead = false;
    bool teleporting = false;
    bool careerMarketEligible = true;
    bool hasActionableProfessionWork = true;
};

struct AuctionMailCandidate
{
    uint32 mailId = 0;
    bool delivered = false;
    uint32 money = 0;
    uint32 attachmentCount = 0;
};

struct InventoryCount
{
    uint32 itemId = 0;
    uint32 count = 0;
    uint32 mailCount = 0;
    uint32 purchasedCount = 0;
    uint32 committedCount = 0;
};

struct ReagentRequirement
{
    uint32 itemId = 0;
    uint32 count = 0;
    bool unlimitedGoldVendorSupply = false;
};

struct RecipeCandidate
{
    uint32 spellId = 0;
    uint32 craftedItemId = 0;
    bool givesSkillUp = false;
    uint32 outputUsagePriority = 0;
    std::vector<ReagentRequirement> reagents;
};

struct AuctionListingCandidate
{
    uint32 auctionId = 0;
    uint32 ownerAccountId = 0;
    uint32 itemId = 0;
    uint32 count = 0;
    uint64 buyout = 0;
    uint32 templateBuyPrice = 0;
    uint32 reserveCeiling = 0;
    bool recipeEligible = false;
    uint64 buyerCeilingPerItem = 0;
    bool accessible = true;
    uint32 recipeSpellId = 0;
};

struct SaleItemCandidate
{
    uint64 itemGuidCounter = 0;
    uint32 itemId = 0;
    uint32 count = 0;
    ItemUsage usage = ITEM_USAGE_NONE;
    bool canBeTraded = false;
    bool bound = false;
    bool container = false;
    uint32 containerItemCount = 0;
    bool conjured = false;
    uint32 duration = 0;
    bool alreadyAuctioned = false;
    uint32 templateBuyPrice = 0;
    uint32 templateSellPrice = 0;
    uint64 lowestCompetingBuyoutPerItem = 0;
    uint32 inventoryCount = 0;
    uint32 professionReserveFloor = 0;
    bool professionRelated = false;
    uint64 allocatedInputCost = 0;
    uint64 deposit = 0;
    uint32 auctionCutBasisPoints = 0;
    uint64 minimumTransactionBasis = 1u;
    uint64 buyerCeilingPerItem = 0;
    bool pureGatheringMaterial = false;
};

struct EconomySnapshot
{
    uint64 guidCounter = 0;
    uint32 botAccountId = 0;
    uint64 freeMoneyForTradeskill = 0;
    uint32 preferredRecipeSpellId = 0;
    std::vector<AuctionMailCandidate> auctionMail;
    std::vector<InventoryCount> inventory;
    std::vector<RecipeCandidate> recipes;
    std::vector<AuctionListingCandidate> auctions;
    std::vector<SaleItemCandidate> saleItems;
    std::vector<uint64> controlledItemGuids;
};

struct EconomyDecision
{
    struct AuctionPurchase
    {
        uint32 auctionId = 0;
        uint32 itemId = 0;
        uint32 count = 0;
        uint64 buyout = 0;
    };

    EconomyPhase phase = EconomyPhase::None;
    uint32 mailId = 0;
    uint32 spellId = 0;
    uint32 itemId = 0;
    uint32 auctionId = 0;
    uint32 count = 0;
    uint64 buyout = 0;
    uint64 itemGuidCounter = 0;
    uint64 startBid = 0;
    uint64 deposit = 0;
    uint64 buyerCeilingPerItem = 0;
    uint64 lowestCompetingBuyoutPerItem = 0;
    uint32 auctionCutBasisPoints = 0;
    uint32 professionReserveFloor = 0;
    uint32 recipeSpellId = 0;
    std::vector<AuctionPurchase> purchases;
    enum class Blocker : uint8
    {
        None,
        PriceCorridor
    } blocker = Blocker::None;
};

using EconomyDecisionBlocker = EconomyDecision::Blocker;

class PlayerbotEconomyPolicy
{
public:
    static EconomyDecision Decide(EconomySnapshot const& snapshot);
    static EconomyWorkBlocker EvaluateWork(EconomyWorkPolicyInput const& input);
    static char const* WorkBlockerName(EconomyWorkBlocker blocker);
    static bool IsEligible(EconomyEligibility const& eligibility);
    static bool IsProfessionRecipeSpell(uint32 effect, uint32 craftedItemId, int32 firstReagentCount,
                                        uint32 schoolMask);
    static bool IsUnlimitedGoldVendorOffer(uint32 maximumCount, uint32 extendedCost);
    static bool IsKnownRecipeOutput(EconomySnapshot const& snapshot, uint32 itemId);
    static bool PreservesProfessionReserve(uint32 inventoryCount, uint32 saleCount, uint32 reserveFloor);
    static uint32 EffectiveProfessionReserve(SaleItemCandidate const& item);
    static bool IsCirculationMaterial(uint32 itemClass, uint32 itemSubclass);
    static uint64 SellerFloor(SaleItemCandidate const& item);
    static uint32 ProductionReserve(EconomySnapshot const& snapshot, uint32 itemId, uint32 configuredReserve = 0u);
    static uint32 ProductionBatchQuantity(RecipeCandidate const& recipe, EconomySnapshot const& snapshot,
                                          uint32 ceiling);
    static uint32 CareerIntervalSeconds(uint32 intervalSeconds, uint8 engagement);
    static uint64 InitialEligibleTime(uint64 now, uint64 guidCounter, uint32 intervalSeconds);
    static uint64 NextEligibleTime(uint64 now, uint32 intervalSeconds, EconomyAttemptOutcome outcome,
                                   uint8 consecutiveFailures);
};
}  // namespace PlayerbotEconomy

#endif  // PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H
