/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <limits>

#include "Bot/Economy/PlayerbotEconomyConsumption.h"
#include "ItemTemplate.h"
#include "SharedDefines.h"
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
    need.sharedDemandEligible = true;
    return need;
}
}  // namespace

TEST(PlayerbotEconomyConsumptionTest, OwnedFinishedGoodsReachTheirLegitimateFinalUse)
{
    std::array<std::pair<EconomySubstitutionGroup, FinishedGoodUse>, 7> const cases = {
        std::pair{EconomySubstitutionGroup::Equipment(4u, 1u, 2u), FinishedGoodUse::Equip},
        std::pair{EconomySubstitutionGroup::Bag(16u), FinishedGoodUse::Equip},
        std::pair{EconomySubstitutionGroup::Ammunition(2u, 3u), FinishedGoodUse::SetAmmunition},
        std::pair{EconomySubstitutionGroup::Consumable(7u, 2u), FinishedGoodUse::Consume},
        std::pair{EconomySubstitutionGroup::Enhancement(5u, 100u), FinishedGoodUse::Apply},
        std::pair{EconomySubstitutionGroup::Glyph(6u, 1u), FinishedGoodUse::Apply},
        std::pair{EconomySubstitutionGroup::Gem(2u), FinishedGoodUse::Apply}};

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

TEST(PlayerbotEconomyConsumptionTest, SpecialDescriptionsKeepApplicationIdentitySeparate)
{
    std::optional<FinishedGoodDescription> const enhancement =
        PlayerbotEconomyConsumption::DescribeEnhancement(1u << 13u, 1u, 777u, 25u);
    ASSERT_TRUE(enhancement.has_value());
    EXPECT_EQ(enhancement->group, EconomySubstitutionGroup::Enhancement(1u << 13u, 1u, 0u));
    EXPECT_EQ(enhancement->use, FinishedGoodUse::Apply);
    EXPECT_EQ(enhancement->utility, 25u);
    EXPECT_EQ(enhancement->appliedEnchantmentId, 777u);

    std::optional<FinishedGoodDescription> const glyph = PlayerbotEconomyConsumption::DescribeGlyph(1234u, 2u);
    ASSERT_TRUE(glyph.has_value());
    EXPECT_EQ(glyph->group, EconomySubstitutionGroup::Glyph(1234u, 2u));
    EXPECT_EQ(glyph->use, FinishedGoodUse::Apply);

    std::optional<FinishedGoodDescription> const gem = PlayerbotEconomyConsumption::DescribeGem(6u, 888u, 50u);
    ASSERT_TRUE(gem.has_value());
    EXPECT_EQ(gem->group, EconomySubstitutionGroup::Gem(6u));
    EXPECT_EQ(gem->use, FinishedGoodUse::Apply);
    EXPECT_EQ(gem->utility, 50u);
    EXPECT_EQ(gem->appliedEnchantmentId, 888u);

    EXPECT_FALSE(PlayerbotEconomyConsumption::DescribeEnhancement(1u, 0u, 0u, 1u).has_value());
    EXPECT_FALSE(PlayerbotEconomyConsumption::DescribeGlyph(0u, 1u).has_value());
    EXPECT_FALSE(PlayerbotEconomyConsumption::DescribeGem(0u, 1u, 1u).has_value());
}

TEST(PlayerbotEconomyConsumptionTest, EnhancementTargetResolutionUsesMainHandAndOnlyUpgrades)
{
    std::vector<EnhancementTargetCandidate> const candidates = {
        {15u, 1u << 13u, 40u, true, true},
        {16u, 1u << 13u, 0u, false, true},
        {4u, 1u << 5u, 10u, false, true},
    };

    std::optional<EnhancementTargetSelection> const oil =
        PlayerbotEconomyConsumption::SelectEnhancementTarget(true, 1u << 13u, 50u, candidates);
    ASSERT_TRUE(oil.has_value());
    EXPECT_EQ(oil->equipmentSlot, 15u);
    EXPECT_EQ(oil->existingUtility, 40u);

    EXPECT_FALSE(PlayerbotEconomyConsumption::SelectEnhancementTarget(true, 1u << 13u, 40u, candidates).has_value());

    std::optional<EnhancementTargetSelection> const scroll =
        PlayerbotEconomyConsumption::SelectEnhancementTarget(false, (1u << 5u) | (1u << 13u), 50u, candidates);
    ASSERT_TRUE(scroll.has_value());
    EXPECT_EQ(scroll->equipmentSlot, 16u);
    EXPECT_EQ(scroll->existingUtility, 0u);
}

TEST(PlayerbotEconomyConsumptionTest, GlyphAndGemNeedsFollowUnlockedEmptySockets)
{
    EXPECT_TRUE(PlayerbotEconomyConsumption::UnlockedGlyphSlots(14u).empty());
    EXPECT_EQ(PlayerbotEconomyConsumption::UnlockedGlyphSlots(15u), (std::vector<uint8>{0u, 1u}));
    EXPECT_EQ(PlayerbotEconomyConsumption::UnlockedGlyphSlots(30u), (std::vector<uint8>{0u, 1u, 3u}));
    EXPECT_EQ(PlayerbotEconomyConsumption::UnlockedGlyphSlots(80u), (std::vector<uint8>{0u, 1u, 3u, 2u, 4u, 5u}));

    ConsumptionNeed const glyph = PlayerbotEconomyConsumption::BuildGlyphNeed(1234u, 1u, 500u);
    EXPECT_EQ(glyph.group, EconomySubstitutionGroup::Glyph(1234u, 1u));
    EXPECT_EQ(glyph.quantity, 1u);
    EXPECT_TRUE(glyph.sharedDemandEligible);

    std::vector<ConsumptionNeed> const gems = PlayerbotEconomyConsumption::BuildGemNeeds({2u, 4u, 2u}, 600u);
    ASSERT_EQ(gems.size(), 2u);
    EXPECT_EQ(gems[0].group, EconomySubstitutionGroup::Gem(2u));
    EXPECT_EQ(gems[0].quantity, 2u);
    EXPECT_EQ(gems[1].group, EconomySubstitutionGroup::Gem(4u));
    EXPECT_EQ(gems[1].quantity, 1u);
    EXPECT_TRUE(gems[0].sharedDemandEligible);
}

TEST(PlayerbotEconomyConsumptionTest, GemTargetResolutionRequiresMatchingEmptySocket)
{
    std::vector<GemSocketTargetCandidate> const candidates = {
        {0u, 0u, 1u, false},
        {4u, 0u, 2u, true},
        {4u, 1u, 4u, false},
        {5u, 0u, 2u, false},
    };

    std::optional<GemSocketTargetSelection> const red =
        PlayerbotEconomyConsumption::SelectGemTarget(2u, 6u, candidates);
    ASSERT_TRUE(red.has_value());
    EXPECT_EQ(red->equipmentSlot, 5u);
    EXPECT_EQ(red->socketIndex, 0u);

    EXPECT_FALSE(PlayerbotEconomyConsumption::SelectGemTarget(1u, 2u, candidates).has_value());
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

TEST(PlayerbotEconomyConsumptionTest, RecurringConsumablesWaitUntilTheirRestorationThreshold)
{
    ConsumptionSnapshot snapshot;
    ConsumptionNeed need =
        Need(EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 10u), FinishedGoodUse::Consume);
    need.inventoryQuantity = 1u;
    need.finalUseNeeded = PlayerbotEconomyConsumption::BelowRestorationThreshold(80u, 100u, 80u);
    snapshot.needs.push_back(need);
    snapshot.owned.push_back({need.group, 100u, 4'540u, 1u, 10u, true});

    ConsumptionDecision const stocked = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(stocked.action, ConsumptionAction::None);
    EXPECT_EQ(stocked.blocker, ConsumptionBlocker::EquivalentSupply);

    snapshot.needs.front().finalUseNeeded = PlayerbotEconomyConsumption::BelowRestorationThreshold(79u, 100u, 80u);
    ConsumptionDecision const hungry = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(hungry.action, ConsumptionAction::FinalUse);
    EXPECT_EQ(hungry.itemId, 4'540u);
}

TEST(PlayerbotEconomyConsumptionTest, DescribesFactoryClassReagentsAsExactRetainedStock)
{
    ItemTemplate reagent{};
    reagent.ItemId = 17'034u;
    reagent.Class = ITEM_CLASS_MISC;
    reagent.SubClass = ITEM_SUBCLASS_REAGENT;

    std::optional<FinishedGoodDescription> const description = PlayerbotEconomyConsumption::Describe(nullptr, &reagent);

    ASSERT_TRUE(description.has_value());
    EXPECT_EQ(description->group, EconomySubstitutionGroup::ExactReagent(17'034u));
    EXPECT_EQ(description->use, FinishedGoodUse::Retain);

    reagent.ItemId = 1u;
    EXPECT_FALSE(PlayerbotEconomyConsumption::Describe(nullptr, &reagent).has_value());
}

TEST(PlayerbotEconomyConsumptionTest, ClassReagentNeedsFollowTheFactoryLevelBands)
{
    EXPECT_TRUE(PlayerbotEconomyConsumption::ClassReagentNeeds(CLASS_DRUID, 19u).empty());
    EXPECT_EQ(PlayerbotEconomyConsumption::ClassReagentNeeds(CLASS_DRUID, 69u),
              (std::vector<ClassReagentStock>{{22'147u, 20u}, {17'026u, 20u}}));
    EXPECT_EQ(PlayerbotEconomyConsumption::ClassReagentNeeds(CLASS_PRIEST, 77u),
              (std::vector<ClassReagentStock>{{17'029u, 20u}, {44'615u, 20u}}));
    EXPECT_EQ(PlayerbotEconomyConsumption::ClassReagentNeeds(CLASS_SHAMAN, 30u, false),
              (std::vector<ClassReagentStock>{{5'175u, 1u}, {5'176u, 1u}, {5'177u, 1u}, {5'178u, 1u}, {17'030u, 20u}}));
    EXPECT_EQ(PlayerbotEconomyConsumption::ClassReagentNeeds(CLASS_SHAMAN, 30u, true),
              (std::vector<ClassReagentStock>{{17'030u, 20u}}));
}

TEST(PlayerbotEconomyConsumptionTest, AffordableAuctionPrecedesVendorAndVendorFillsTheFallback)
{
    ConsumptionSnapshot snapshot;
    ConsumptionNeed need =
        Need(EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 10u), FinishedGoodUse::Consume);
    need.protectedBudget = 100u;
    snapshot.needs.push_back(need);
    snapshot.offers.push_back({need.group, 50u, 12u, 4'540u, 1u, 200u, 10u, true});
    snapshot.vendorOffers.push_back({need.group, 4'540u, 1u, 50u, 10u, true});

    ConsumptionDecision const vendor = PlayerbotEconomyConsumption::Decide(snapshot);
    ASSERT_EQ(vendor.action, ConsumptionAction::VendorPurchase);
    EXPECT_EQ(vendor.itemId, 4'540u);
    EXPECT_EQ(vendor.vendorBundleCount, 1u);
    EXPECT_EQ(vendor.buyout, 50u);

    snapshot.offers.front().buyout = 40u;
    ConsumptionDecision const auction = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(auction.action, ConsumptionAction::Purchase);
    EXPECT_EQ(auction.auctionId, 50u);
}

TEST(PlayerbotEconomyConsumptionTest, BagNeedCoversEmptySlotsAndFourSlotUpgrades)
{
    std::optional<ConsumptionNeed> const need = PlayerbotEconomyConsumption::BuildBagNeed({
        .emptyBagSlots = 1u,
        .equippedCapacities = {6u, 12u, 14u},
        .affordableCapacities = {10u, 16u},
        .protectedBudget = 500u,
    });

    ASSERT_TRUE(need.has_value());
    EXPECT_EQ(need->group, EconomySubstitutionGroup::Bag(16u));
    EXPECT_EQ(need->quantity, 3u);
    EXPECT_EQ(need->requiredUtility, 16u);
    EXPECT_EQ(need->protectedBudget, 500u);
    EXPECT_TRUE(need->finalUseNeeded);
}

TEST(PlayerbotEconomyConsumptionTest, LargerBagsSatisfySmallerBagNeeds)
{
    ConsumptionNeed need = Need(EconomySubstitutionGroup::Bag(14u), FinishedGoodUse::Equip);
    need.requiredUtility = 14u;

    EXPECT_FALSE(PlayerbotEconomyConsumption::MatchesNeed(need, EconomySubstitutionGroup::Bag(12u), 12u));
    EXPECT_TRUE(PlayerbotEconomyConsumption::MatchesNeed(need, EconomySubstitutionGroup::Bag(14u), 14u));
    EXPECT_TRUE(PlayerbotEconomyConsumption::MatchesNeed(need, EconomySubstitutionGroup::Bag(16u), 16u));
}

TEST(PlayerbotEconomyConsumptionTest, HeldSuppliesRetainConcreteItemIdentityInsideBroadGroups)
{
    ConsumptionSnapshot snapshot;
    EconomySubstitutionGroup const group = EconomySubstitutionGroup::Consumable(7u, 2u);
    snapshot.needs.push_back(Need(group, FinishedGoodUse::Consume));
    snapshot.held.push_back({group, 4596u, 2u, EconomySupplySource::Inventory, 10u});
    snapshot.held.push_back({group, 4596u, 1u, EconomySupplySource::Inventory, 10u});
    snapshot.held.push_back({group, 4596u, 1u, EconomySupplySource::Mail, 10u});

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

TEST(PlayerbotEconomyConsumptionTest, WorkTripInFlightDefersPurchasesButNotFinalUse)
{
    ConsumptionSnapshot snapshot;
    snapshot.workTripInFlight = true;
    ConsumptionNeed need = Need(EconomySubstitutionGroup::Consumable(7u, 2u), FinishedGoodUse::Consume);
    snapshot.needs.push_back(need);
    snapshot.offers.push_back({need.group, 50u, 99u, 500u, 1u, 100u, 10u, true});

    ConsumptionDecision const deferred = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(deferred.action, ConsumptionAction::None);
    EXPECT_EQ(deferred.blocker, ConsumptionBlocker::WorkTripInFlight);

    snapshot.owned.push_back({need.group, 7u, 1u, 1u, 10u, true});
    ConsumptionDecision const used = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(used.action, ConsumptionAction::FinalUse);

    snapshot.workTripInFlight = false;
    snapshot.owned.clear();
    ConsumptionDecision const bought = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(bought.action, ConsumptionAction::Purchase);
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

TEST(PlayerbotEconomyConsumptionTest, SameTierEquipmentSupplyDoesNotSuppressARealUpgrade)
{
    ConsumptionSnapshot snapshot;
    EconomySubstitutionGroup const group = EconomySubstitutionGroup::Equipment(4u, 1u, 2u);
    ConsumptionNeed need = Need(group, FinishedGoodUse::Equip);
    need.requiredUtility = 11u;
    need.inventoryQuantity = 2u;
    snapshot.needs.push_back(need);
    snapshot.owned.push_back({group, 100u, 500u, 1u, 10u, true});
    snapshot.owned.push_back({group, 101u, 501u, 1u, 10u, true});
    snapshot.offers.push_back({group, 50u, 12u, 502u, 1u, 150u, 20u, true});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
    ASSERT_EQ(decision.action, ConsumptionAction::Purchase);
    EXPECT_EQ(decision.auctionId, 50u);
}

TEST(PlayerbotEconomyConsumptionTest, EquipmentDemandRejectsNonupgradesAndUnsafeOffers)
{
    ConsumptionSnapshot snapshot;
    snapshot.botAccountId = 11u;
    EconomySubstitutionGroup const group = EconomySubstitutionGroup::Equipment(4u, 1u, 2u);
    ConsumptionNeed need = Need(group, FinishedGoodUse::Equip);
    need.requiredUtility = 11u;
    snapshot.needs.push_back(need);
    snapshot.owned.push_back({group, 100u, 500u, 1u, 10u, true});
    snapshot.offers.push_back({group, 50u, 12u, 501u, 1u, 100u, 10u, true});
    snapshot.offers.push_back({group, 51u, 11u, 502u, 1u, 100u, 20u, true});
    snapshot.offers.push_back({group, 52u, 12u, 503u, 1u, 201u, 20u, true});
    snapshot.offers.push_back({group, 53u, 12u, 504u, 1u, 100u, 20u, false});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(snapshot);
    EXPECT_EQ(decision.action, ConsumptionAction::None);
}

TEST(PlayerbotEconomyConsumptionTest, MarketEquipmentMustBeUsefulAndAtLeastUncommon)
{
    EXPECT_TRUE(
        PlayerbotEconomyConsumption::IsMarketEquipment(ITEM_CLASS_ARMOR, ITEM_QUALITY_UNCOMMON, ITEM_USAGE_REPLACE));
    EXPECT_TRUE(PlayerbotEconomyConsumption::IsMarketEquipment(ITEM_CLASS_WEAPON, ITEM_QUALITY_RARE, ITEM_USAGE_EQUIP));
    EXPECT_FALSE(
        PlayerbotEconomyConsumption::IsMarketEquipment(ITEM_CLASS_ARMOR, ITEM_QUALITY_NORMAL, ITEM_USAGE_REPLACE));
    EXPECT_FALSE(
        PlayerbotEconomyConsumption::IsMarketEquipment(ITEM_CLASS_ARMOR, ITEM_QUALITY_UNCOMMON, ITEM_USAGE_KEEP));
    EXPECT_FALSE(PlayerbotEconomyConsumption::IsMarketEquipment(ITEM_CLASS_CONTAINER, ITEM_QUALITY_UNCOMMON,
                                                                ITEM_USAGE_REPLACE));
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

TEST(PlayerbotEconomyConsumptionTest, ExplicitSemanticNeedsOwnDemandWhileDiscoveredItemsDoNot)
{
    ConsumptionSnapshot discoveredOnly;
    discoveredOnly.held.push_back({EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 120u), 4540u, 20u,
                                   EconomySupplySource::Inventory, 120u});
    discoveredOnly.offers.push_back({EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 120u), 7u, 12u,
                                     4540u, 20u, 100u, 120u, true});

    EXPECT_TRUE(PlayerbotEconomyConsumption::DemandFacts(discoveredOnly).empty());

    ConsumptionNeed discoveredEquipment = Need(EconomySubstitutionGroup::Equipment(4u, 1u, 2u), FinishedGoodUse::Equip);
    discoveredEquipment.sharedDemandEligible = false;
    discoveredOnly.needs.push_back(discoveredEquipment);
    EXPECT_TRUE(PlayerbotEconomyConsumption::DemandFacts(discoveredOnly).empty());

    ConsumptionSnapshot explicitNeed;
    explicitNeed.needs.push_back(
        PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Food, 120u, 3u, true, 500u}));
    explicitNeed.held.push_back({EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 120u), 4540u, 1u,
                                 EconomySupplySource::Inventory, 120u});

    std::vector<EconomyDemandFact> const demands = PlayerbotEconomyConsumption::DemandFacts(explicitNeed);
    ASSERT_EQ(demands.size(), 1u);
    EXPECT_EQ(demands.front().quantity, 3u);

    std::vector<EconomySupplyFact> const supplies = PlayerbotEconomyConsumption::SupplyFacts(explicitNeed);
    ASSERT_EQ(supplies.size(), 1u);
    EXPECT_EQ(supplies.front().group, demands.front().group);
    EXPECT_EQ(supplies.front().quantity, 1u);
}

TEST(PlayerbotEconomyConsumptionTest, SemanticCapabilitiesAndMinimumUtilityDoNotCollapse)
{
    std::array capabilities{
        ConsumableCapability::Food,
        ConsumableCapability::Drink,
        ConsumableCapability::HealthRestoration,
        ConsumableCapability::ManaRestoration,
        ConsumableCapability::Bandage,
    };

    for (ConsumableCapability const capability : capabilities)
    {
        ConsumptionNeed const need = PlayerbotEconomyConsumption::BuildNeed({capability, 100u, 1u, true, 500u});
        EXPECT_TRUE(PlayerbotEconomyConsumption::MatchesNeed(
            need, EconomySubstitutionGroup::Consumable(capability, 100u), 100u));
        EXPECT_FALSE(
            PlayerbotEconomyConsumption::MatchesNeed(need, EconomySubstitutionGroup::Consumable(capability, 99u), 99u));

        for (ConsumableCapability const other : capabilities)
        {
            if (other == capability)
                continue;
            EXPECT_FALSE(PlayerbotEconomyConsumption::MatchesNeed(
                need, EconomySubstitutionGroup::Consumable(other, 100u), 100u));
        }
    }
}

TEST(PlayerbotEconomyConsumptionTest, ActivityAndApplicableVendorFactsSuppressSharedDemand)
{
    ConsumptionSnapshot snapshot;
    snapshot.needs.push_back(
        PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Food, 100u, 2u, false, 500u}));
    snapshot.needs.push_back(
        PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Drink, 100u, 2u, true, 500u, true}));

    EXPECT_TRUE(PlayerbotEconomyConsumption::DemandFacts(snapshot).empty());
}

TEST(PlayerbotEconomyConsumptionTest, RecurringStockSaturatesCapsAndPermitsZero)
{
    RecurringStockReconciliation const zero = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 8u,
        .safetyReserve = 4u,
        .carryingBudget = 0u,
        .usesBeforeDevelopmentalDelivery = 8u,
        .credibleDevelopmentalDeliveryQuantity = 4u,
        .developmentalPathViable = true,
    });
    EXPECT_EQ(zero.desiredStock, 0u);
    EXPECT_EQ(zero.bridgeQuantity, 0u);
    EXPECT_EQ(zero.developmentalReservationQuantity, 0u);
    EXPECT_EQ(zero.residualUncoveredQuantity, 0u);

    RecurringStockReconciliation const capped = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 8u,
        .safetyReserve = 4u,
        .carryingBudget = 10u,
    });
    EXPECT_EQ(capped.desiredStock, 10u);
    EXPECT_EQ(capped.residualUncoveredQuantity, 10u);

    RecurringStockReconciliation const saturated = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = std::numeric_limits<uint32>::max(),
        .safetyReserve = 1u,
        .carryingBudget = std::numeric_limits<uint32>::max(),
    });
    EXPECT_EQ(saturated.desiredStock, std::numeric_limits<uint32>::max());
}

TEST(PlayerbotEconomyConsumptionTest, RecurringStockPartitionsBridgeDevelopmentAndResidualWithoutOverlap)
{
    RecurringStockReconciliation const result = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 8u,
        .safetyReserve = 2u,
        .carryingBudget = 9u,
        .adequateCurrentAndPendingSupply = 2u,
        .usesBeforeDevelopmentalDelivery = 5u,
        .credibleDevelopmentalDeliveryQuantity = 3u,
        .developmentalPathViable = true,
        .developmentalRejectionReason = "stale_reason",
    });

    EXPECT_EQ(result.desiredStock, 9u);
    EXPECT_EQ(result.bridgeQuantity, 3u);
    EXPECT_EQ(result.developmentalReservationQuantity, 3u);
    EXPECT_EQ(result.residualUncoveredQuantity, 1u);
    EXPECT_TRUE(result.developmentalRejectionReason.empty());
    EXPECT_EQ(2u + result.bridgeQuantity + result.developmentalReservationQuantity + result.residualUncoveredQuantity,
              result.desiredStock);
}

TEST(PlayerbotEconomyConsumptionTest, ExistingSupplyCoversPreDeliveryUsesBeforeDevelopment)
{
    RecurringStockReconciliation const result = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 7u,
        .safetyReserve = 2u,
        .carryingBudget = 9u,
        .adequateCurrentAndPendingSupply = 7u,
        .usesBeforeDevelopmentalDelivery = 4u,
        .credibleDevelopmentalDeliveryQuantity = 5u,
        .developmentalPathViable = true,
    });

    EXPECT_EQ(result.bridgeQuantity, 0u);
    EXPECT_EQ(result.developmentalReservationQuantity, 2u);
    EXPECT_EQ(result.residualUncoveredQuantity, 0u);

    RecurringStockReconciliation const fullySupplied = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 2u,
        .safetyReserve = 1u,
        .carryingBudget = 3u,
        .adequateCurrentAndPendingSupply = 50u,
        .usesBeforeDevelopmentalDelivery = 8u,
        .credibleDevelopmentalDeliveryQuantity = 8u,
        .developmentalPathViable = true,
    });
    EXPECT_EQ(fullySupplied.bridgeQuantity, 0u);
    EXPECT_EQ(fullySupplied.developmentalReservationQuantity, 0u);
    EXPECT_EQ(fullySupplied.residualUncoveredQuantity, 0u);

    RecurringStockReconciliation const bridgeLimitedToTarget = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 2u,
        .safetyReserve = 1u,
        .carryingBudget = 3u,
        .usesBeforeDevelopmentalDelivery = 10u,
        .credibleDevelopmentalDeliveryQuantity = 5u,
        .developmentalPathViable = true,
    });
    EXPECT_EQ(bridgeLimitedToTarget.bridgeQuantity, 3u);
    EXPECT_EQ(bridgeLimitedToTarget.developmentalReservationQuantity, 0u);
    EXPECT_EQ(bridgeLimitedToTarget.residualUncoveredQuantity, 0u);
}

TEST(PlayerbotEconomyConsumptionTest, UnviableDevelopmentLeavesResidualAndStableReason)
{
    RecurringStockReconciliation const result = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 6u,
        .safetyReserve = 2u,
        .carryingBudget = 8u,
        .adequateCurrentAndPendingSupply = 1u,
        .usesBeforeDevelopmentalDelivery = 3u,
        .credibleDevelopmentalDeliveryQuantity = 5u,
        .developmentalPathViable = false,
        .developmentalRejectionReason = "affinity_too_low",
    });

    EXPECT_EQ(result.bridgeQuantity, 2u);
    EXPECT_EQ(result.developmentalReservationQuantity, 0u);
    EXPECT_EQ(result.residualUncoveredQuantity, 5u);
    EXPECT_EQ(result.developmentalRejectionReason, "affinity_too_low");

    RecurringStockReconciliation const arbitraryReason = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 1u,
        .carryingBudget = 1u,
        .developmentalPathViable = false,
        .developmentalRejectionReason = "caller_reason",
    });
    EXPECT_EQ(arbitraryReason.developmentalRejectionReason, "caller_reason");
}

TEST(PlayerbotEconomyConsumptionTest, NoOfferExistsOnlyForAnActiveNeed)
{
    ConsumptionSnapshot snapshot;
    EXPECT_EQ(PlayerbotEconomyConsumption::Decide(snapshot).blocker, ConsumptionBlocker::None);

    snapshot.needs.push_back(
        PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Food, 100u, 1u, true, 500u}));
    EXPECT_EQ(PlayerbotEconomyConsumption::Decide(snapshot).blocker, ConsumptionBlocker::NoOffer);

    EXPECT_FALSE(PlayerbotEconomyConsumption::IsStuckBlocker(ConsumptionBlocker::None));
    EXPECT_TRUE(PlayerbotEconomyConsumption::IsStuckBlocker(ConsumptionBlocker::NoOffer));
    EXPECT_TRUE(PlayerbotEconomyConsumption::IsStuckBlocker(ConsumptionBlocker::PriceCorridor));
    EXPECT_TRUE(PlayerbotEconomyConsumption::IsStuckBlocker(ConsumptionBlocker::EquivalentSupply));
    EXPECT_TRUE(PlayerbotEconomyConsumption::IsStuckBlocker(ConsumptionBlocker::WorkTripInFlight));
}
