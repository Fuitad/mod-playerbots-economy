/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYCONSUMPTION_H
#define PLAYERBOTS_PLAYERBOTECONOMYCONSUMPTION_H

#include <optional>
#include <string>
#include <vector>

#include "Ai/Base/Value/ItemUsageValue.h"
#include "Bot/Economy/PlayerbotEconomyCoordinator.h"

class Player;
struct ItemTemplate;

namespace PlayerbotEconomy
{
enum class FinishedGoodUse : uint8
{
    Equip,
    SetAmmunition,
    Consume,
    Apply,
    Retain,
    Recover
};

enum class ConsumptionAction : uint8
{
    None,
    Purchase,
    VendorPurchase,
    FinalUse,
    Recovery
};

enum class ConsumptionBlocker : uint8
{
    None,
    ActivityStopped,
    EquivalentSupply,
    SameAccount,
    PriceCorridor,
    NoOffer,
    WorkTripInFlight
};

struct FinishedGoodDescription
{
    EconomySubstitutionGroup group;
    FinishedGoodUse use = FinishedGoodUse::Equip;
    uint32 utility = 0;
    uint32 appliedEnchantmentId = 0;
};

struct EnhancementTargetCandidate
{
    uint8 equipmentSlot = 0;
    uint32 inventoryTypeMask = 0;
    uint32 existingUtility = 0;
    bool mainHand = false;
    bool fitsSpellRequirements = false;
};

struct EnhancementTargetSelection
{
    uint8 equipmentSlot = 0;
    uint32 existingUtility = 0;
};

struct GemSocketTargetCandidate
{
    uint8 equipmentSlot = 0;
    uint8 socketIndex = 0;
    uint32 socketColor = 0;
    bool occupied = false;
};

struct GemSocketTargetSelection
{
    uint8 equipmentSlot = 0;
    uint8 socketIndex = 0;
};

struct ConsumptionNeedIntent
{
    ConsumableCapability capability = ConsumableCapability::Food;
    uint32 requiredUtility = 0;
    uint32 desiredStock = 0;
    bool compatibleActivity = false;
    uint64 protectedBudget = 0;
    bool ordinaryVendorSupply = false;
};

/*
 * Food and drink are the only consumables a bot burns continuously and cannot replace once an
 * activity has started. A bot pulled into a dungeon through the group finder never gets a shopping
 * trip: it arrives with whatever it was already carrying, and there is no vendor or mailbox inside.
 * A standing single unit therefore reads as "supplied" while leaving the bot useless for the run.
 * Stock is instead held at two full stacks, reordered as it falls, so readiness is decided long
 * before the dungeon exists. Two stacks of each cost roughly 8 gold at mid level against the
 * thousand a bot carries, and occupy four bag slots, so the ceiling is set by usefulness rather
 * than by affordability.
 */
inline constexpr std::uint32_t CONSUMABLE_SUSTENANCE_EXPECTED_USES = 20u;
inline constexpr std::uint32_t CONSUMABLE_SUSTENANCE_SAFETY_RESERVE = 20u;
inline constexpr std::uint32_t CONSUMABLE_SUSTENANCE_CARRYING_BUDGET = 40u;
/*
 * Restock once a full stack has been eaten rather than on every missing item. One vendor trip per
 * twenty consumed, instead of one per one, and the bot is never seen below this level.
 */
inline constexpr std::uint32_t CONSUMABLE_SUSTENANCE_REORDER_POINT = 20u;

/*
 * Everything else a bot buys is used on a specific occasion rather than continuously, so it keeps
 * the single-unit target it has always had. Widening that is a separate decision about how much of
 * every bot's purse goes to bandages.
 */
inline constexpr std::uint32_t CONSUMABLE_OCCASIONAL_STOCK = 1u;

/*
 * Combat potions sit between the two. They are not burned continuously the way food is, but a fight
 * consumes several in a row and then none for an hour, and like food they cannot be replaced once a
 * dungeon has started. A full stack is one bag slot, so the ceiling is usefulness rather than cost.
 *
 * The reorder point matters more here than the target. Potions reach a bot only through crafting,
 * the auction house, or loot: every potion row in npc_vendor carries a maxcount of one to five with
 * an hours-long restock, so IsApplicableUnlimitedGoldVendorOffer never admits one and no bot is ever
 * shown a potion by a vendor. Demand therefore has to survive long enough for an alchemist to see
 * it and produce against it, which a target that reorders on every missing unit would not do.
 */
inline constexpr std::uint32_t CONSUMABLE_POTION_STOCK = 20u;
inline constexpr std::uint32_t CONSUMABLE_POTION_REORDER_POINT = 5u;

struct RecurringStockFacts
{
    uint32 expectedUses = 0;
    uint32 safetyReserve = 0;
    uint32 carryingBudget = 0;
    uint32 adequateCurrentAndPendingSupply = 0;
    uint32 usesBeforeDevelopmentalDelivery = 0;
    uint32 credibleDevelopmentalDeliveryQuantity = 0;
    bool developmentalPathViable = false;
    std::string developmentalRejectionReason;
};

struct RecurringStockReconciliation
{
    uint32 desiredStock = 0;
    uint32 bridgeQuantity = 0;
    uint32 developmentalReservationQuantity = 0;
    uint32 residualUncoveredQuantity = 0;
    std::string developmentalRejectionReason;
};

struct ClassReagentStock
{
    uint32 itemId = 0;
    uint32 desiredStock = 0;

    bool operator==(ClassReagentStock const&) const = default;
};

struct BagNeedFacts
{
    uint32 emptyBagSlots = 0;
    std::vector<uint16> equippedCapacities;
    std::vector<uint16> affordableCapacities;
    uint64 protectedBudget = 0;
    uint8 level = 0;
};

struct ConsumptionNeed
{
    EconomySubstitutionGroup group;
    FinishedGoodUse use = FinishedGoodUse::Equip;
    uint32 quantity = 0;
    /*
     * The stock level at which a restock trip becomes worth making. Zero means "the moment the bot
     * is short", which is right for a one-off purchase and wrong for something consumed constantly:
     * refilling food at 39 of 40 would send the bot to a vendor for a single item. A reorder point
     * holds the bot between this level and `quantity`, so one trip buys the whole shortfall and the
     * next is a full stack's worth of consumption away.
     */
    uint32 reorderPoint = 0;
    uint32 inventoryQuantity = 0;
    uint32 mailQuantity = 0;
    uint32 activePurchaseQuantity = 0;
    uint32 productionQuantity = 0;
    uint32 committedPurchaseQuantity = 0;
    uint32 requiredUtility = 0;
    uint64 buyerCeilingPerItem = 0;
    uint64 protectedBudget = 0;
    uint32 remainingUses = 0;
    bool compatibleActivity = false;
    bool finalUseNeeded = true;
    bool committedPurchaseStillUseful = true;
    bool ordinaryVendorSupply = false;
    bool sharedDemandEligible = false;
};

struct ConsumptionOwnedItem
{
    EconomySubstitutionGroup group;
    uint64 itemGuidCounter = 0;
    uint32 itemId = 0;
    uint32 count = 0;
    uint32 utility = 0;
    bool compatible = false;
};

struct ConsumptionHeldItem
{
    EconomySubstitutionGroup group;
    uint32 itemId = 0;
    uint32 count = 0;
    EconomySupplySource source = EconomySupplySource::Inventory;
    uint32 utility = 0;
};

struct ConsumptionOffer
{
    EconomySubstitutionGroup group;
    uint32 auctionId = 0;
    uint32 ownerAccountId = 0;
    uint32 itemId = 0;
    uint32 count = 0;
    uint64 buyout = 0;
    uint32 utility = 0;
    bool compatible = false;
};

struct ConsumptionVendorOffer
{
    EconomySubstitutionGroup group;
    uint32 itemId = 0;
    uint32 bundleSize = 1;
    uint64 bundlePrice = 0;
    uint32 utility = 0;
    bool compatible = false;
};

struct ConsumptionSnapshot
{
    uint32 botAccountId = 0;
    std::vector<ConsumptionNeed> needs;
    std::vector<ConsumptionOwnedItem> owned;
    std::vector<ConsumptionHeldItem> held;
    std::vector<ConsumptionOffer> offers;
    std::vector<ConsumptionVendorOffer> vendorOffers;
    // A gathering trip or a walk to a forge the bot is still on. Buying off the auction house would
    // cancel it, so purchases wait until the trip ends; using something already in the bags does not.
    bool workTripInFlight = false;
};

struct ConsumptionDecision
{
    ConsumptionAction action = ConsumptionAction::None;
    ConsumptionBlocker blocker = ConsumptionBlocker::None;
    EconomySubstitutionGroup group;
    FinishedGoodUse use = FinishedGoodUse::Equip;
    uint64 itemGuidCounter = 0;
    uint32 itemId = 0;
    uint32 auctionId = 0;
    uint32 count = 0;
    uint32 vendorBundleCount = 0;
    uint64 buyout = 0;
    uint64 protectedBudget = 0;
};

class PlayerbotEconomyConsumption
{
public:
    static ConsumptionNeed BuildNeed(ConsumptionNeedIntent const& intent);
    static std::optional<ConsumptionNeed> BuildBagNeed(BagNeedFacts const& facts);
    // The bag a tailor of the bot's own level band can make, so demand exists before supply does.
    static uint16 ExpectedBagCapacity(uint8 level);
    static std::vector<ClassReagentStock> ClassReagentNeeds(uint8 playerClass, uint8 level,
                                                            bool hasShamanRelic = false);
    static RecurringStockReconciliation ReconcileRecurringStock(RecurringStockFacts const& facts);
    static std::vector<EconomyDemandFact> DemandFacts(ConsumptionSnapshot const& snapshot);
    static ConsumptionDecision Decide(ConsumptionSnapshot const& snapshot);
    static std::vector<EconomySupplyFact> SupplyFacts(ConsumptionSnapshot const& snapshot);
    static bool MatchesNeed(ConsumptionNeed const& need, EconomySubstitutionGroup const& candidateGroup,
                            uint32 candidateUtility);
    static bool BelowRestorationThreshold(uint32 current, uint32 maximum, uint32 thresholdPercent);
    static std::optional<FinishedGoodDescription> Describe(Player const* bot, ItemTemplate const* itemTemplate);
    static std::optional<FinishedGoodDescription> DescribeEnhancement(uint32 targetInventoryTypeMask,
                                                                      uint8 enchantmentSlot, uint32 enchantmentId,
                                                                      uint32 utility);
    static std::optional<FinishedGoodDescription> DescribeGlyph(uint32 glyphSpellId, uint32 glyphSlotType,
                                                                uint32 glyphItemId);
    static std::optional<FinishedGoodDescription> DescribeGem(uint32 gemColor, uint32 enchantmentId, uint32 utility);
    static std::vector<uint8> UnlockedGlyphSlots(uint32 level);
    static ConsumptionNeed BuildGlyphNeed(uint32 glyphSpellId, uint32 glyphSlotType, uint32 glyphItemId,
                                          uint64 protectedBudget);
    static std::vector<ConsumptionNeed> BuildGemNeeds(std::vector<uint32> const& emptySocketColors,
                                                      uint64 protectedBudget);
    static std::optional<EnhancementTargetSelection> SelectEnhancementTarget(
        bool mainHandOnly, uint32 targetInventoryTypeMask, uint32 candidateUtility,
        std::vector<EnhancementTargetCandidate> const& candidates);
    static std::optional<GemSocketTargetSelection> SelectGemTarget(
        uint32 requiredSocketColor, uint32 gemColor, std::vector<GemSocketTargetCandidate> const& candidates);
    static bool IsMarketEquipment(uint32 itemClass, uint32 quality, ItemUsage usage);
    static char const* BlockerName(ConsumptionBlocker blocker);
    // True when an active consumption need could not progress. None means there was no active need.
    [[nodiscard]] static bool IsStuckBlocker(ConsumptionBlocker blocker);
    static std::string GroupKey(EconomySubstitutionGroup const& group);
};
}  // namespace PlayerbotEconomy

#endif
