/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H
#define PLAYERBOTS_PLAYERBOTECONOMYPOLICY_H

#include <string_view>
#include <vector>

#include "Ai/Base/Actions/RandomBotMaintenancePolicy.h"
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

// The core mills five herbs of one kind per cast.
inline constexpr uint32 MILLING_HERBS_PER_CAST = 5u;

struct ReagentRequirement
{
    uint32 itemId = 0;
    uint32 count = 0;
    bool unlimitedGoldVendorSupply = false;
    // Herbs that mill into this reagent when it is a pigment; empty otherwise.
    std::vector<uint32> millingInputItemIds;
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
    // A bar, bolt, cloth, herb, ore, leather or dust: what other professions wait on.
    bool circulationMaterial = false;
};

struct RecipeCandidate
{
    uint32 spellId = 0;
    uint32 craftedItemId = 0;
    bool givesSkillUp = false;
    uint32 outputUsagePriority = 0;
    std::vector<ReagentRequirement> reagents;
    uint16 professionSkillId = 0;
    // False when the bags cannot store one batch of the product. The craft is not chosen then: the
    // cast would fail the bag pre-check five times and quarantine the bot at the forge.
    bool outputRoom = true;
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
    bool circulationMaterial = false;
    bool unusable = false;
    // Uncommon armor or weapon the bot could wear but does not want (item usage AH).
    bool unwantedEquipment = false;
    // A trade good none of the bot's professions can use (meat on a non-cook): listed, not hoarded.
    bool unwantedMaterial = false;
    uint32 itemClass = 0;
    uint32 quality = 0;
};

struct EconomySnapshot
{
    uint64 guidCounter = 0;
    uint32 botAccountId = 0;
    uint64 freeMoneyForTradeskill = 0;
    // Spendable on an input for the bot's own recipe (see OwnRecipeInputBudget).
    uint64 ownRecipeInputMoney = 0;
    // The whole purse: a listing's deposit is paid from it, and the auction house refuses one it cannot.
    uint64 money = 0;
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
        PriceCorridor,
        // The only craft had no bag room for its product and nothing was listable.
        CraftOutputRoom
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

/*
 * The riding rank a bot is currently shopping for.
 *
 * Riding is not a profession, but it is bought from a trainer for gold exactly like a profession
 * rank, so the economy runs it through the trainer objective machinery. What is different is the
 * completion test: a bot buying Journeyman riding already holds the riding skill, so only the skill
 * CAP rising proves the purchase landed.
 */
/*
 * Which objective the trainer stage should serve on this cycle.
 *
 * Riding outranks profession trainer work, but a priority that cancels a trip already under way is
 * not a priority, it is a treadmill: the trip restarts on every cycle and the bot never arrives.
 * Riding therefore yields to a trainer trip already in flight and takes the stage on the next cycle
 * that has none.
 */
enum class TrainerStageObjective : uint8
{
    // Nothing latched and nothing wanted. The trainer stage has no work.
    None,
    // Keep serving whatever is already latched.
    KeepActive,
    // Start or continue the riding rank.
    Riding,
    // Ask the career plan which profession objective to serve.
    SelectProfession
};

struct TrainerStageFacts
{
    bool activeObjective = false;
    bool activeIsProgression = false;
    bool activeIsRiding = false;
    // A trip this runtime owns is under way or has just arrived. See TrainerTripInFlight: this is
    // NOT the same fact as a trainer destination having been selected.
    bool tripInFlight = false;
    bool ridingWanted = false;
    bool careerPhasesAllowed = false;
    // The bot has reached the level where profession selection is worth attempting at all. See
    // ProfessionPipelineOpen. Defaults closed so a caller that forgets the fact spams nothing.
    bool professionEligible = false;
};

struct RidingRankNeed
{
    bool wanted = false;
    // Riding skill the bot's level entitles it to. 0 below the ground mount level.
    uint32 targetSkill = 0;
};

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
    // purseEmergency: the repair visit found the purse below the repair cost, so auction-usage goods
    // go to the vendor too; a bot that cannot fight has no market to wait for.
    [[nodiscard]] static bool VendorSellAllowed(ItemUsage usage, bool purseEmergency = false);
    // Bags past this share of their slots are under pressure: the next vendor visit sells what the
    // bot does not need, so the loot it came for has room. 161 of 200 bots sat above it on 2026-09-03.
    static constexpr uint8 BAG_PRESSURE_PERCENT = 80;
    [[nodiscard]] static bool BagPressure(uint8 bagSpacePercent);
    // Whether a bot standing at the auctioneer lists one more stack in the same visit, given how many
    // it has listed there already. Bounded so one visit cannot monopolise the cycle.
    [[nodiscard]] static bool ListsAnotherStack(uint32 listedThisVisit);
    // What a bag purchase may spend: the whole purse above the repair reserve. Bags are capacity,
    // bought once, and the gear lane a bag used to draw on never freed the price of a pouch.
    [[nodiscard]] static uint64 BagPurchaseBudget(uint64 money, uint64 repairReserve);
    // What one consumable purchase may spend: a tenth of the purse above the repair reserve. The
    // fork's consumables lane comes after gear savings and was empty for most bots.
    [[nodiscard]] static uint64 ConsumablePurchaseBudget(uint64 money, uint64 repairReserve);
    // What an input for the bot's own recipe may cost, a listed reagent or a green bought for its dust:
    // the tradeskill lane, or the purse above the repair reserve and the training floor when the lane
    // is empty. The lane saves level-cubed copper first and was zero for most bots.
    [[nodiscard]] static uint64 OwnRecipeInputBudget(uint64 freeTradeskillMoney, uint64 money, uint64 repairReserve);
    // Under bag pressure a bot may vendor gray items and the white weapons, armor and consumables it
    // cannot use or does not need. Trade goods, quest items, containers and anything above white stay.
    [[nodiscard]] static bool IsBagPressureVendorSale(uint32 quality, uint32 itemClass, ItemUsage usage, bool unusable);
    /*
     * Sustenance a bot can never put to use, handed over on a vendor visit it was already making.
     * The usage value routes white food and drink to the auction house because it has a sell price
     * and ordinary quality, so a warrior's looted water and a level 27 bot's level 1 boar meat sat
     * in the bags forever: too cheap to be worth an auction, never classed as vendor trash. This
     * only widens what may be sold when the bot is standing at a vendor; it never routes a trip.
     */
    [[nodiscard]] static bool IsUnusableSustenance(uint32 spellCategory, uint32 requiredLevel, bool botHasMana,
                                                   uint32 botLevel);
    /*
     * Equipment no bot will ever buy: the consumption side only shops for uncommon-or-better armor
     * and weapons (IsMarketEquipment), so a white or gray piece can only ever sell to a human and in
     * practice expires, burning its deposit every cycle. The usage value still routes it to the
     * auction house (ordinary quality, has a sell price), so the sale policy skips it and the vendor
     * visitor picks it up instead.
     */
    [[nodiscard]] static bool IsUnmarketableEquipment(uint32 itemClass, uint32 quality);
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
    /*
     * Whether the bot is short of the riding skill its level entitles it to.
     *
     * The tier rules are not restated here: RequiredMountTier and RequiredRidingSkill already own
     * which level buys which rank, and the mount actions in mod-playerbots read them too. A second
     * copy would drift.
     */
    [[nodiscard]] static RidingRankNeed EvaluateRidingRank(
        uint32 level, uint32 ridingSkill, playerbots::maintenance::MountLevelThresholds const& thresholds);
    /*
     * Gold a riding rank may spend.
     *
     * `freeMoney` is "free money for anything", which already holds back the guild, repair, ammo,
     * spell training and travel reserves. Nothing holds back the profession and consumable lanes,
     * so riding subtracts them itself: a rank is a one off durable purchase and must not eat the
     * reagents and food the bot needs on every later cycle. Saturates at zero.
     */
    [[nodiscard]] static uint32 RidingBudget(uint32 freeMoney, uint32 professionNeed, uint32 consumableNeed);
    /*
     * Gold a profession lesson may spend.
     *
     * `freeTradeskillMoney` is "free money for tradeskill", which holds back every standing reserve,
     * including a flat travel floor larger than a fresh bot's whole purse. Early lessons cost coppers,
     * so behind that lane alone a wiped population never trains anything: the reserves exceed the
     * purse and the lane stays at zero for every bot. A lesson is a one off durable purchase, so a
     * small floor may bypass the reserves: up to the floor cap of the purse, minus the repair need,
     * is always spendable on lessons. A bot whose ordinary lane already clears the floor keeps the
     * larger of the two. Saturates at zero.
     */
    [[nodiscard]] static uint32 ProfessionTrainingBudget(uint32 freeTradeskillMoney, uint32 money, uint32 repairNeed);
    /*
     * Is the profession pipeline worth running for a bot of this level?
     *
     * Below level 6 nothing can succeed: every Apprentice teach spell requires level 5, and the
     * stay home travel rule keeps a level 5 bot inside its own zone, so the stage would only scan
     * the realm and log a refusal every cycle. One overnight window produced 753 trainer_ineligible
     * and hundreds of unsafe_route lines from exactly this, all from bots that merely had to level.
     * Closed means ChooseTrainerStageObjective never selects a profession objective, so no scan, no
     * log line, and the cycle goes to work the bot can actually do.
     */
    [[nodiscard]] static bool ProfessionPipelineOpen(uint8 botLevel);
    [[nodiscard]] static TrainerStageObjective ChooseTrainerStageObjective(TrainerStageFacts const& facts);
    /*
     * Is a trainer trip actually in flight?
     *
     * A selected trainer destination is not a trip. Travel declines on any cycle where another system
     * holds the forced travel target, and the selection outlives that decline. Reading the selection
     * as liveness is what would pin a stale objective forever: the stage would keep serving it and
     * never re-select from the career plan. Liveness is the travel target this runtime owns, which
     * covers both walking there and having arrived.
     */
    [[nodiscard]] static bool TrainerTripInFlight(bool trainerSelected, bool ownsTravelTarget);
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
