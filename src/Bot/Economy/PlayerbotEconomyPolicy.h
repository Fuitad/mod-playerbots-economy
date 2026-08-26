/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H
#define PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H

#include <string_view>
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
    // No work on this pass. Retry at the ordinary interval without accumulating failure backoff.
    Idle,
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
    bool necessaryPurchase = false;
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
    /*
     * An equipped item sitting at zero durability.
     *
     * The economy yields the whole cycle for this, and deliberately not as a transient pause: a bot
     * that cannot fight cannot finish an errand, and while the economy holds a forced travel target
     * the maintenance repair action cannot get the bot to a repairer. Live on 2026-08-25 a level 28
     * hunter with a zero durability bow was walked toward an auctioneer while its repair attempts
     * failed, dying repeatedly with 1075 gold it could not spend. Repair comes first, so the cycle
     * releases the target and the suspended idle strategies until the gear works again.
     */
    bool brokenEquipment = false;
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
    // Reagents this listing breaks into: dust and essence for a green the bot could disenchant, pigments
    // for a herb stack the bot could mill. Filled only when the bot itself could do the breaking.
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
    bool unusable = false;
};

struct EconomySnapshot
{
    uint64 guidCounter = 0;
    uint32 botAccountId = 0;
    uint64 freeMoneyForTradeskill = 0;
    bool careerEligible = true;
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
    // A restricted consumer-only cycle may list only an item the bot cannot use. Revalidate that
    // property immediately before the auction packet is sent.
    bool requiresUnusableItem = false;
    std::vector<AuctionPurchase> purchases;
    enum class Blocker : uint8
    {
        None,
        PriceCorridor
    } blocker = Blocker::None;
};

using EconomyDecisionBlocker = EconomyDecision::Blocker;

/*
 * Item spell categories that mark sustenance. 11 is food, 59 is drink; the same pair the usage
 * value and the consumable describer key on.
 */
inline constexpr std::uint32_t SUSTENANCE_FOOD_SPELL_CATEGORY = 11u;
inline constexpr std::uint32_t SUSTENANCE_DRINK_SPELL_CATEGORY = 59u;
/*
 * How far below the bot a food tier must sit before it is worth handing to a vendor. Tiers are
 * about ten levels apart, so this sells the tier the bot has outgrown and keeps the current one.
 */
inline constexpr std::uint32_t SUSTENANCE_OUTGROWN_LEVEL_MARGIN = 10u;

class PlayerbotEconomyPolicy
{
public:
    static EconomyDecision Decide(EconomySnapshot const& snapshot);
    static EconomyWorkBlocker EvaluateWork(EconomyWorkPolicyInput const& input);
    static char const* WorkBlockerName(EconomyWorkBlocker blocker);
    static bool IsLifecycleSafe(EconomyEligibility const& eligibility);
    static bool HasCareerCapability(EconomyEligibility const& eligibility);
    // Unsafe only because of combat or a teleport in progress: the cycle waits, it does not
    // release its trip and claims. A hunting trip fights by design; resetting it at the first pull
    // released every hunted claim as capability_lost.
    static bool IsTransientlyUnsafe(EconomyEligibility const& eligibility);
    static bool IsProfessionRecipeSpell(uint32 effect, uint32 craftedItemId, int32 firstReagentCount,
                                        uint32 schoolMask);
    static bool IsUnlimitedGoldVendorOffer(uint32 maximumCount, uint32 extendedCost);
    static bool IsApplicableUnlimitedGoldVendorOffer(VendorOfferPolicyInput const& input);
    static bool AllowsAutonomousListing(AutonomousListingPolicyInput const& input);
    // What an economy bot may hand to a vendor when the rpg layer sells: only what the usage value
    // already marked as vendor trash. Anything marked for the auction house is market supply.
    [[nodiscard]] static bool VendorSellAllowed(ItemUsage usage);
    /*
     * Sustenance a bot can never put to use, handed over on a vendor visit it was already making.
     * The usage value routes white food and drink to the auction house because it has a sell price
     * and ordinary quality, so a warrior's looted water and a level 27 bot's level 1 boar meat sat
     * in the bags forever: too cheap to be worth an auction, never classed as vendor trash. This
     * only widens what may be sold when the bot is standing at a vendor; it never routes a trip.
     */
    [[nodiscard]] static bool IsUnusableSustenance(uint32 spellCategory, uint32 requiredLevel, bool botHasMana,
                                                   uint32 botLevel);
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
    // transientNoCandidate marks a NoCandidate whose cause is where the bot happens to stand (a material
    // source with no population in reach); it waits one doubled interval instead of compounding.
    static uint64 NextEligibleTime(uint64 now, uint32 intervalSeconds, EconomyAttemptOutcome outcome,
                                   uint8 consecutiveFailures, bool transientNoCandidate = false);
    [[nodiscard]] static char const* IdleBlocker(bool careerCapable);
    [[nodiscard]] static bool IsTransientNoCandidate(std::string_view blocker);
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

// Candidate stand off distances for a spell focus whose template lists focusRange yards, nearest first,
// one yard apart, from SPELL_FOCUS_STAND_OFF_DISTANCE up to one yard past half the listed range. The
// core accepts half the range plus both object sizes (a player's 1.5 yard reach and the object's 0.39),
// so one yard past still leaves 0.9 yards of slack. The Ironforge forges are the lava pools themselves
// (a 30 yard focus whose magma reaches 14 yards out, with dry floor from 15), so the runtime walks this
// ladder outward until it finds a point that is neither in a damaging liquid nor off the platform.
std::vector<float> SpellFocusStandOffDistances(uint32 focusRange);

// A stand point whose ground is further than this from the focus object's height is a pit or a balcony
// (the Ironforge lava floor sits 19 yards under the forge), not a place to craft from.
constexpr float SPELL_FOCUS_STAND_POINT_MAX_DROP = 3.0f;
}  // namespace PlayerbotEconomy

#endif  // PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H
