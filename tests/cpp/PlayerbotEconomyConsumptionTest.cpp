/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>

#include "Bot/Economy/PlayerbotEconomyConsumption.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
ConsumptionNeed Need(EconomySubstitutionGroup group, FinishedGoodUse use)
{
    ConsumptionNeed need;
    need.group = group;
    need.use = use;
    need.quantity = 1u;
    need.requiredUtility = 10u;
    need.compatibleActivity = true;
    need.remainingUses = 1u;
    need.buyerCeilingPerItem = 200u;
    need.protectedBudget = 200u;
    return need;
}
}  // namespace

TEST(PlayerbotEconomyConsumptionTest, OwnedFinishedGoodsReachTheirLegitimateFinalUse)
{
    std::array<std::pair<EconomySubstitutionGroup, FinishedGoodUse>, 5> const cases = {
        std::pair{EconomySubstitutionGroup::Equipment(4u, 1u, 2u), FinishedGoodUse::Equip},
        std::pair{EconomySubstitutionGroup::Bag(16u), FinishedGoodUse::Equip},
        std::pair{EconomySubstitutionGroup::Ammunition(2u, 3u), FinishedGoodUse::SetAmmunition},
        std::pair{EconomySubstitutionGroup::Consumable(7u, 2u), FinishedGoodUse::Consume},
        std::pair{EconomySubstitutionGroup::Enhancement(5u, 100u), FinishedGoodUse::Apply}};

    uint64 itemGuid = 100u;
    for (auto const& [group, use] : cases)
    {
        ConsumptionSnapshot snapshot;
        snapshot.needs.push_back(Need(group, use));
        snapshot.owned.push_back({group, itemGuid++, 500u, 1u, 10u, true});

        ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
        EXPECT_EQ(decision.action, ConsumptionAction::FinalUse);
        EXPECT_EQ(decision.use, use);
        EXPECT_NE(decision.itemGuidCounter, 0u);
    }
}

TEST(PlayerbotEconomyConsumptionTest, FinalUseReportsOneActuallyUsedItemFromALargerStack)
{
    ConsumptionSnapshot snapshot;
    ConsumptionNeed need = Need(EconomySubstitutionGroup::Consumable(7u, 2u), FinishedGoodUse::Consume);
    need.quantity = 6u;
    snapshot.needs.push_back(need);
    snapshot.owned.push_back({need.group, 100u, 4596u, 10u, 10u, true});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);

    ASSERT_EQ(decision.action, ConsumptionAction::FinalUse);
    EXPECT_EQ(decision.count, 1u);
}

TEST(PlayerbotEconomyConsumptionTest, HeldSuppliesRetainConcreteItemIdentityInsideBroadGroups)
{
    ConsumptionSnapshot snapshot;
    EconomySubstitutionGroup const group = EconomySubstitutionGroup::Consumable(7u, 2u);
    snapshot.needs.push_back(Need(group, FinishedGoodUse::Consume));
    snapshot.held.push_back({group, 4596u, 2u, EconomySupplySource::Inventory});
    snapshot.held.push_back({group, 4596u, 1u, EconomySupplySource::Inventory});
    snapshot.held.push_back({group, 4596u, 1u, EconomySupplySource::Mail});

    std::vector<EconomySupplyFact> const supplies = PlayerbotEconomyConsumption::SupplyFacts(snapshot);

    ASSERT_EQ(supplies.size(), 2u);
    EXPECT_EQ(supplies[0].group, group);
    EXPECT_EQ(supplies[0].itemId, 4596u);
    EXPECT_EQ(supplies[0].quantity, 3u);
    EXPECT_EQ(supplies[0].source, EconomySupplySource::Inventory);
    EXPECT_EQ(supplies[1].group, group);
    EXPECT_EQ(supplies[1].itemId, 4596u);
    EXPECT_EQ(supplies[1].quantity, 1u);
    EXPECT_EQ(supplies[1].source, EconomySupplySource::Mail);
}

TEST(PlayerbotEconomyConsumptionTest, EquivalentSupplySuppressesReplacementDemand)
{
    for (uint8 source = 0u; source < 4u; ++source)
    {
        ConsumptionSnapshot snapshot;
        ConsumptionNeed need = Need(EconomySubstitutionGroup::Equipment(4u, 1u, 2u), FinishedGoodUse::Equip);
        if (source == 0u)
            need.inventoryQuantity = 1u;
        else if (source == 1u)
            need.mailQuantity = 1u;
        else if (source == 2u)
            need.activePurchaseQuantity = 1u;
        else
            need.productionQuantity = 1u;
        snapshot.needs.push_back(need);
        snapshot.offers.push_back({need.group, 50u, 99u, 500u, 1u, 100u, 10u, true});

        ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
        EXPECT_EQ(decision.action, ConsumptionAction::None);
        EXPECT_EQ(decision.blocker, ConsumptionBlocker::EquivalentSupply);
    }
}

TEST(PlayerbotEconomyConsumptionTest, RecurringDemandEndsWithCompatibleActivityOrRemainingUse)
{
    ConsumptionSnapshot snapshot;
    ConsumptionNeed ammunition = Need(EconomySubstitutionGroup::Ammunition(2u, 3u), FinishedGoodUse::SetAmmunition);
    ammunition.compatibleActivity = false;
    snapshot.needs.push_back(ammunition);
    ConsumptionNeed consumable = Need(EconomySubstitutionGroup::Consumable(7u, 2u), FinishedGoodUse::Consume);
    consumable.remainingUses = 0u;
    snapshot.needs.push_back(consumable);
    snapshot.offers.push_back({ammunition.group, 50u, 99u, 500u, 1u, 100u, 10u, true});
    snapshot.offers.push_back({consumable.group, 51u, 99u, 501u, 1u, 100u, 10u, true});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(decision.action, ConsumptionAction::None);
    EXPECT_EQ(decision.blocker, ConsumptionBlocker::ActivityStopped);
}

TEST(PlayerbotEconomyConsumptionTest, ComparableUtilityPrefersCheaperValidOffer)
{
    ConsumptionSnapshot snapshot;
    snapshot.botAccountId = 11u;
    ConsumptionNeed need = Need(EconomySubstitutionGroup::Equipment(4u, 1u, 2u), FinishedGoodUse::Equip);
    snapshot.needs.push_back(need);
    snapshot.offers.push_back({need.group, 50u, 11u, 500u, 1u, 50u, 10u, true});
    snapshot.offers.push_back({need.group, 51u, 12u, 501u, 1u, 150u, 10u, true});
    snapshot.offers.push_back({need.group, 52u, 13u, 502u, 1u, 100u, 10u, true});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(decision.action, ConsumptionAction::Purchase);
    EXPECT_EQ(decision.auctionId, 52u);
    EXPECT_EQ(decision.buyout, 100u);
}

TEST(PlayerbotEconomyConsumptionTest, HigherUtilityMayCostMoreOnlyInsideTheCorridor)
{
    ConsumptionSnapshot snapshot;
    ConsumptionNeed need = Need(EconomySubstitutionGroup::Equipment(4u, 1u, 2u), FinishedGoodUse::Equip);
    snapshot.needs.push_back(need);
    snapshot.offers.push_back({need.group, 50u, 12u, 500u, 1u, 100u, 10u, true});
    snapshot.offers.push_back({need.group, 51u, 13u, 501u, 1u, 180u, 20u, true});
    snapshot.offers.push_back({need.group, 52u, 14u, 502u, 1u, 201u, 30u, true});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(decision.action, ConsumptionAction::Purchase);
    EXPECT_EQ(decision.auctionId, 51u);
    EXPECT_EQ(decision.buyout, 180u);
}

TEST(PlayerbotEconomyConsumptionTest, ObsoleteCommittedPurchaseBecomesRecoveryWithoutReplacementDemand)
{
    ConsumptionSnapshot snapshot;
    ConsumptionNeed need = Need(EconomySubstitutionGroup::Enhancement(5u, 100u), FinishedGoodUse::Apply);
    need.committedPurchaseQuantity = 1u;
    need.committedPurchaseStillUseful = false;
    snapshot.needs.push_back(need);
    snapshot.offers.push_back({need.group, 50u, 12u, 500u, 1u, 100u, 10u, true});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(decision.action, ConsumptionAction::Recovery);
    EXPECT_EQ(decision.use, FinishedGoodUse::Recover);
    EXPECT_EQ(decision.count, 1u);
    EXPECT_EQ(decision.auctionId, 0u);
}
