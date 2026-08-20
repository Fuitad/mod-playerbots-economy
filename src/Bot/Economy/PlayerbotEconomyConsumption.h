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
    Recover
};

enum class ConsumptionAction : uint8
{
    None,
    Purchase,
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
    GatheringTripInFlight
};

struct FinishedGoodDescription
{
    EconomySubstitutionGroup group;
    FinishedGoodUse use = FinishedGoodUse::Equip;
    uint32 utility = 0;
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

struct ConsumptionNeed
{
    EconomySubstitutionGroup group;
    FinishedGoodUse use = FinishedGoodUse::Equip;
    uint32 quantity = 0;
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

struct ConsumptionSnapshot
{
    uint32 botAccountId = 0;
    std::vector<ConsumptionNeed> needs;
    std::vector<ConsumptionOwnedItem> owned;
    std::vector<ConsumptionHeldItem> held;
    std::vector<ConsumptionOffer> offers;
    // A gathering trip the bot is still walking. Buying off the auction house would cancel it, so
    // purchases wait until the trip ends; using something already in the bags does not.
    bool gatheringTripInFlight = false;
};

struct ConsumptionDecision
{
    ConsumptionAction action = ConsumptionAction::None;
    ConsumptionBlocker blocker = ConsumptionBlocker::NoOffer;
    EconomySubstitutionGroup group;
    FinishedGoodUse use = FinishedGoodUse::Equip;
    uint64 itemGuidCounter = 0;
    uint32 itemId = 0;
    uint32 auctionId = 0;
    uint32 count = 0;
    uint64 buyout = 0;
};

class PlayerbotEconomyConsumption
{
public:
    static ConsumptionNeed BuildNeed(ConsumptionNeedIntent const& intent);
    static RecurringStockReconciliation ReconcileRecurringStock(RecurringStockFacts const& facts);
    static std::vector<EconomyDemandFact> DemandFacts(ConsumptionSnapshot const& snapshot);
    static ConsumptionDecision Decide(ConsumptionSnapshot const& snapshot);
    static std::vector<EconomySupplyFact> SupplyFacts(ConsumptionSnapshot const& snapshot);
    static bool MatchesNeed(ConsumptionNeed const& need, EconomySubstitutionGroup const& candidateGroup,
                            uint32 candidateUtility);
    static std::optional<FinishedGoodDescription> Describe(Player const* bot, ItemTemplate const* itemTemplate);
    static bool IsMarketEquipment(uint32 itemClass, uint32 quality, ItemUsage usage);
    static char const* BlockerName(ConsumptionBlocker blocker);
    static std::string GroupKey(EconomySubstitutionGroup const& group);
};
}  // namespace PlayerbotEconomy

#endif
