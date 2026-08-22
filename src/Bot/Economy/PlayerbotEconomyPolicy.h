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

// A gathering trip in flight is re-evaluated this often: fast enough to notice arrival at the node and
// to chain gathers, without the one-second cadence a craft in progress uses.
inline constexpr uint32 PLAYERBOT_ECONOMY_TRIP_POLL_SECONDS = 5;

enum class EconomyAttemptOutcome : uint8
{
    InProgress,
    // Work that is underway and being tracked (a gathering trip): poll on the trip cadence.
    Tracking,
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

struct VendorOfferPolicyInput
{
    uint32 maximumCount = 0;
    uint32 extendedCost = 0;
    bool factionAllowed = false;
    bool levelAllowed = false;
    bool reputationAllowed = false;
    bool sameMap = false;
    bool routeAvailable = false;
};

struct AutonomousListingPolicyInput
{
    bool ordinaryVendorSupply = false;
    bool trainingOutput = false;
    bool independentDemand = false;
};

struct RecipeCandidate
{
    uint32 spellId = 0;
    uint32 craftedItemId = 0;
    bool givesSkillUp = false;
    uint32 outputUsagePriority = 0;
    std::vector<ReagentRequirement> reagents;
    uint16 professionSkillId = 0;
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
    // Reagents this listing disenchants into, filled only for a green the bot could break itself.
    std::vector<uint32> disenchantYieldItemIds;
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
    bool ordinaryVendorSupply = false;
    bool trainingOutput = false;
    bool independentDemand = false;
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
    std::vector<uint32> applicableUnlimitedGoldVendorItemIds;
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
    // BuyReagent from an unlimited gold vendor (itemId, count) instead of the auction house.
    bool vendorPurchase = false;
    // BuyReagent of a green to disenchant into the reagent (itemId is the green). A one-off progression
    // purchase the coordinator has no demand gap for, so it is leased nowhere.
    bool disenchantSourcePurchase = false;
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
    static bool IsApplicableUnlimitedGoldVendorOffer(VendorOfferPolicyInput const& input);
    static bool AllowsAutonomousListing(AutonomousListingPolicyInput const& input);
    // What an economy bot may hand to a vendor when the rpg layer sells: only what the usage value
    // already marked as vendor trash. Anything marked for the auction house is market supply.
    [[nodiscard]] static bool VendorSellAllowed(ItemUsage usage);
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

struct EconomyApproachPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

// Where a bot should stand to use an object at (objectX, objectY): `distance` yards from it on the side the
// bot approaches from, fanned by up to 60 degrees from a stable per-bot seed so several bots do not stack on
// one spot. A bot already on the object gets a seed-chosen direction.
EconomyApproachPoint ApproachPoint(float objectX, float objectY, float botX, float botY, float distance, uint32 seed);

// Yards from a spell focus object (forge, anvil, cooking fire) that a crafting bot stands at. Most world
// campfires run the core's go_flames script, which burns any player inside the model's bounding box plus
// 0.3 yards every three seconds, and that damage interrupts every craft. The core accepts a focus within
// half its listed range, 5 yards for a forge or a campfire, so 3 yards clears the flames with room to spare.
constexpr float SPELL_FOCUS_STAND_OFF_DISTANCE = 3.0f;
}  // namespace PlayerbotEconomy

#endif  // PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H
