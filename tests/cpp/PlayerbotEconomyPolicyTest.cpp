/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "Bot/Economy/PlayerbotEconomyConsumption.h"
#include "Bot/Economy/PlayerbotEconomyMail.h"
#include "Bot/Economy/PlayerbotEconomyPolicy.h"
#include "Bot/Economy/PlayerbotEconomyRuntime.h"
#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "Bot/Extension/PlayerbotExtension.h"
#include "SharedDefines.h"
#include "Strategy.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

void AddPlayerbotsEconomyScripts();

TEST(PlayerbotEconomyRuntimeContractTest, CycleResultOwnsItsWorkIdentityAndBlocker)
{
    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {101u, 201u, 301u, 401u};
    result.blocker = "auctioneer_unreachable";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;

    PlayerbotEconomyCycleResult const copied = result;
    result.blocker = "changed";

    EXPECT_EQ(copied.outcome, PlayerbotEconomyCycleOutcome::Scheduled);
    EXPECT_EQ(copied.phase, EconomyPhase::BuyReagent);
    EXPECT_EQ(copied.workIdentity.spellId, 101u);
    EXPECT_EQ(copied.workIdentity.itemId, 201u);
    EXPECT_EQ(copied.workIdentity.auctionId, 301u);
    EXPECT_EQ(copied.workIdentity.itemGuidCounter, 401u);
    EXPECT_EQ(copied.blocker, "auctioneer_unreachable");
    EXPECT_EQ(copied.schedulingEffect, EconomyAttemptOutcome::Operation);
}

TEST(PlayerbotEconomyRuntimeContractTest, TimedOutProgressionClearsOnlyItsUnleasedWorkOrder)
{
    constexpr uint32 characterGuid = 42u;
    constexpr uint32 progressionRecipeSpellId = 101u;

    EXPECT_TRUE(
        CanClearTimedOutProgressionWorkOrder(progressionRecipeSpellId, progressionRecipeSpellId, characterGuid, {}));
    EXPECT_FALSE(CanClearTimedOutProgressionWorkOrder(202u, progressionRecipeSpellId, characterGuid, {}));

    EconomyAssignment production;
    production.characterGuid = characterGuid;
    production.kind = EconomyClaimKind::Production;
    production.state = EconomyClaimState::Leased;
    production.recipeSpellId = progressionRecipeSpellId;
    EXPECT_FALSE(CanClearTimedOutProgressionWorkOrder(progressionRecipeSpellId, progressionRecipeSpellId, characterGuid,
                                                      {production}));

    production.state = EconomyClaimState::Released;
    EXPECT_TRUE(CanClearTimedOutProgressionWorkOrder(progressionRecipeSpellId, progressionRecipeSpellId, characterGuid,
                                                     {production}));
}

namespace
{
std::unique_ptr<Strategy> EconomyStrategy()
{
    AddPlayerbotsEconomyScripts();
    SharedNamedObjectContextList<Strategy> contexts;
    GetPlayerbotExtensionRegistry().ForEach([&contexts](PlayerbotExtension& extension)
                                            { extension.AddStrategyContexts(contexts); });
    auto const creator = contexts.creators.find("playerbots economy");
    if (creator == contexts.creators.end())
        return nullptr;
    return std::unique_ptr<Strategy>(creator->second(nullptr));
}
}  // namespace

TEST(PlayerbotEconomyPolicyTest, AuctionMailHasFirstPrecedence)
{
    EconomySnapshot snapshot;
    snapshot.auctionMail.push_back({41u, true, 800u, 0u});

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);

    EXPECT_EQ(decision.phase, EconomyPhase::CollectAuctionMail);
    EXPECT_EQ(decision.mailId, 41u);

    snapshot.auctionMail.clear();
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
}

TEST(PlayerbotEconomyPolicyTest, CraftableSkillUpRecipePrecedesOtherWork)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.inventory = {{10u, 3u}, {11u, 1u}, {12u, 2u}};
    snapshot.recipes = {{100u, 200u, true, 4u, {{10u, 3u}, {11u, 1u}}}, {101u, 201u, false, 1u, {{12u, 2u}}}};

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);

    EXPECT_EQ(decision.phase, EconomyPhase::Craft);
    EXPECT_EQ(decision.spellId, 100u);
    EXPECT_EQ(decision.itemId, 200u);
}

TEST(PlayerbotEconomyPolicyTest, PersistedProfessionWorkOrderKeepsItsRecipeUntilCompletion)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.preferredRecipeSpellId = 101u;
    snapshot.inventory = {{10u, 3u}, {11u, 3u}};
    snapshot.recipes = {{100u, 200u, true, 1u, {{10u, 3u}}}, {101u, 201u, true, 1u, {{11u, 3u}}}};

    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).spellId, 101u);

    snapshot.preferredRecipeSpellId = 999u;
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).spellId, 100u);
}

TEST(PlayerbotEconomyPolicyTest, VendorReagentsAreBoughtAtAVendorOnceMarketReagentsAreInHand)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    // Minor Healing Potion: Peacebloom and Silverleaf come from the market, the Empty Vial from any vendor.
    snapshot.recipes = {{2330u, 118u, true, 1u, {{2447u, 1u, false}, {765u, 1u, false}, {3371u, 1u, true}}}};
    snapshot.inventory = {{2447u, 1u}, {765u, 1u}};

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);
    EXPECT_EQ(decision.phase, EconomyPhase::BuyReagent);
    EXPECT_TRUE(decision.vendorPurchase);
    EXPECT_EQ(decision.spellId, 2330u);
    EXPECT_EQ(decision.itemId, 3371u);
    EXPECT_EQ(decision.count, 1u);

    // A market reagent still missing, with nothing listed, keeps the vendor trip from running ahead of it.
    snapshot.inventory = {{2447u, 1u}};
    EconomyDecision const waiting = PlayerbotEconomyPolicy::Decide(snapshot);
    EXPECT_EQ(waiting.phase, EconomyPhase::None);
    EXPECT_FALSE(waiting.vendorPurchase);

    // Vials already in the bags: nothing to buy, craft.
    snapshot.inventory = {{2447u, 1u}, {765u, 1u}, {3371u, 1u}};
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::Craft);
}

TEST(PlayerbotEconomyPolicyTest, KnownRecipeOutputIsProductionRatherThanRawGathering)
{
    EconomySnapshot snapshot;
    snapshot.recipes = {{2657u, 2840u, false, 3u, {{2770u, 1u}}}};

    EXPECT_TRUE(PlayerbotEconomyPolicy::IsKnownRecipeOutput(snapshot, 2840u));
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsKnownRecipeOutput(snapshot, 2770u));
}

TEST(PlayerbotEconomyPolicyTest, SelectedRecipeDeficitsCloseInStableOrder)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.botAccountId = 7u;
    snapshot.freeMoneyForTradeskill = 100u;
    snapshot.inventory = {{10u, 1u}};
    snapshot.recipes = {{100u, 200u, true, 1u, {{10u, 3u}, {11u, 2u}}}};
    snapshot.auctions = {{1u, 8u, 12u, 2u, 10u, 20u, 20u},  {2u, 8u, 10u, 1u, 10u, 20u, 20u},
                         {3u, 7u, 10u, 2u, 20u, 20u, 20u},  {4u, 8u, 10u, 2u, 0u, 20u, 20u},
                         {5u, 8u, 10u, 20u, 20u, 20u, 20u}, {6u, 8u, 10u, 2u, 101u, 100u, 20u},
                         {7u, 8u, 10u, 2u, 50u, 20u, 20u},  {8u, 8u, 10u, 2u, 30u, 20u, 20u},
                         {9u, 8u, 10u, 4u, 40u, 20u, 20u},  {10u, 8u, 11u, 2u, 20u, 20u, 20u}};

    EconomyDecision decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::BuyReagent);
    EXPECT_EQ(decision.spellId, 100u);
    EXPECT_EQ(decision.itemId, 10u);
    EXPECT_EQ(decision.auctionId, 2u);
    ASSERT_EQ(decision.purchases.size(), 2u);
    EXPECT_EQ(decision.purchases[0].auctionId, 2u);
    EXPECT_EQ(decision.purchases[1].auctionId, 9u);
    EXPECT_EQ(decision.count, 5u);
    EXPECT_EQ(decision.buyout, 50u);

    snapshot.inventory = {{10u, 5u}};
    decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::BuyReagent);
    EXPECT_EQ(decision.itemId, 11u);
    EXPECT_EQ(decision.auctionId, 10u);

    snapshot.inventory.push_back({11u, 2u});
    decision = PlayerbotEconomyPolicy::Decide(snapshot);
    EXPECT_EQ(decision.phase, EconomyPhase::Craft);
    EXPECT_EQ(decision.spellId, 100u);
}

TEST(PlayerbotEconomyPolicyTest, UnlimitedGoldVendorReagentsNeverCreateAuctionDemand)
{
    EXPECT_TRUE(PlayerbotEconomyPolicy::IsUnlimitedGoldVendorOffer(0u, 0u));
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsUnlimitedGoldVendorOffer(1u, 0u));
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsUnlimitedGoldVendorOffer(0u, 1u));

    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.botAccountId = 7u;
    snapshot.freeMoneyForTradeskill = 100u;
    snapshot.inventory = {{11u, 0u}};
    snapshot.recipes = {{100u, 200u, true, 1u, {{10u, 2u, true}, {11u, 1u}}}};
    snapshot.auctions = {{1u, 8u, 10u, 2u, 10u, 20u, 20u}, {2u, 8u, 11u, 1u, 10u, 20u, 20u}};

    EconomyDecision decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::BuyReagent);
    EXPECT_EQ(decision.itemId, 11u);
    EXPECT_EQ(decision.auctionId, 2u);

    // With the market reagent in hand, the vendor reagent is bought at a vendor, never from the listing.
    snapshot.inventory.front().count = 1u;
    decision = PlayerbotEconomyPolicy::Decide(snapshot);
    EXPECT_EQ(decision.phase, EconomyPhase::BuyReagent);
    EXPECT_TRUE(decision.vendorPurchase);
    EXPECT_EQ(decision.itemId, 10u);
    EXPECT_EQ(decision.count, 2u);
    EXPECT_EQ(decision.auctionId, 0u);
    EXPECT_TRUE(decision.purchases.empty());

    snapshot.inventory.push_back({10u, 2u});
    decision = PlayerbotEconomyPolicy::Decide(snapshot);
    EXPECT_EQ(decision.phase, EconomyPhase::Craft);
    EXPECT_EQ(decision.spellId, 100u);
}

TEST(PlayerbotEconomyPolicyTest, ReagentDeficitAggregatesCheaperListingsWithoutSameAccountOrExcess)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.botAccountId = 7u;
    snapshot.freeMoneyForTradeskill = 100u;
    snapshot.inventory = {{10u, 1u}};
    snapshot.recipes = {{100u, 200u, true, 1u, {{10u, 6u}}}};

    AuctionListingCandidate sameAccount{1u, 7u, 10u, 5u, 5u, 0u, 20u};
    sameAccount.buyerCeilingPerItem = 20u;
    AuctionListingCandidate first{2u, 8u, 10u, 2u, 20u, 0u, 20u};
    first.buyerCeilingPerItem = 20u;
    AuctionListingCandidate second{3u, 9u, 10u, 3u, 36u, 0u, 20u};
    second.buyerCeilingPerItem = 20u;
    AuctionListingCandidate excess{4u, 10u, 10u, 6u, 6u, 0u, 20u};
    excess.buyerCeilingPerItem = 20u;
    excess.reserveCeiling = 6u;
    AuctionListingCandidate expensive{5u, 11u, 10u, 1u, 25u, 0u, 20u};
    expensive.buyerCeilingPerItem = 20u;
    snapshot.auctions = {sameAccount, first, second, excess, expensive};

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);

    ASSERT_EQ(decision.phase, EconomyPhase::BuyReagent);
    ASSERT_EQ(decision.purchases.size(), 2u);
    EXPECT_EQ(decision.purchases[0].auctionId, 2u);
    EXPECT_EQ(decision.purchases[1].auctionId, 3u);
    EXPECT_EQ(decision.count, 5u);
    EXPECT_EQ(decision.buyout, 56u);
}

TEST(PlayerbotEconomyPolicyTest, ReagentDeficitsAcceptIndivisibleStacksInsideTheReserveCeiling)
{
    struct Case
    {
        uint32 deficit;
        uint32 stack;
    };
    for (Case const test : {Case{1u, 20u}, Case{2u, 20u}, Case{3u, 6u}})
    {
        EconomySnapshot snapshot;
        snapshot.guidCounter = 42u;
        snapshot.botAccountId = 7u;
        snapshot.freeMoneyForTradeskill = 1'000u;
        snapshot.inventory = {{2589u, 0u}};
        snapshot.recipes = {{100u, 200u, true, 1u, {{2589u, test.deficit}}}};

        AuctionListingCandidate listing{1u, 8u, 2589u, test.stack, test.stack * 5u, 10u, test.stack};
        listing.buyerCeilingPerItem = 10u;
        snapshot.auctions.push_back(listing);

        EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);
        ASSERT_EQ(decision.phase, EconomyPhase::BuyReagent);
        EXPECT_EQ(decision.count, test.stack);
        EXPECT_EQ(decision.buyout, test.stack * 5u);
    }
}

TEST(PlayerbotEconomyPolicyTest, IndivisibleReagentStacksStillRespectEveryPurchaseGuard)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.botAccountId = 7u;
    snapshot.freeMoneyForTradeskill = 100u;
    snapshot.inventory = {{2589u, 0u}};
    snapshot.recipes = {{100u, 200u, true, 1u, {{2589u, 1u}}}};

    AuctionListingCandidate listing{1u, 8u, 2589u, 20u, 100u, 10u, 20u};
    listing.buyerCeilingPerItem = 10u;
    snapshot.auctions = {listing};
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::BuyReagent);

    snapshot.auctions.front().ownerAccountId = 7u;
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
    snapshot.auctions.front() = listing;
    snapshot.auctions.front().accessible = false;
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
    snapshot.auctions.front() = listing;
    snapshot.auctions.front().reserveCeiling = 19u;
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
    snapshot.auctions.front() = listing;
    snapshot.freeMoneyForTradeskill = 99u;
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
    snapshot.freeMoneyForTradeskill = 100u;
    snapshot.auctions.front() = listing;
    snapshot.auctions.front().buyout = 201u;
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
}

TEST(PlayerbotEconomyPolicyTest, InboundAndCommittedInputsSuppressDemandButNeverMakeRecipeCraftable)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.botAccountId = 7u;
    snapshot.freeMoneyForTradeskill = 100u;
    snapshot.inventory = {{10u, 1u, 2u, 1u, 2u}};
    snapshot.recipes = {{100u, 200u, true, 1u, {{10u, 6u}}}};
    AuctionListingCandidate listing{2u, 8u, 10u, 1u, 10u, 0u, 20u};
    listing.buyerCeilingPerItem = 20u;
    snapshot.auctions.push_back(listing);

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);

    EXPECT_EQ(decision.phase, EconomyPhase::None);
    EXPECT_TRUE(decision.purchases.empty());
}

TEST(PlayerbotEconomyPolicyTest, ZeroTemplatePriceTradesOnlyWhenSellerFloorAndBuyerCeilingOverlap)
{
    EconomySnapshot purchase;
    purchase.guidCounter = 42u;
    purchase.botAccountId = 7u;
    purchase.freeMoneyForTradeskill = 100u;
    purchase.inventory = {{10u, 0u}};
    purchase.recipes = {{100u, 200u, true, 1u, {{10u, 2u}}}};
    AuctionListingCandidate listing{2u, 8u, 10u, 2u, 40u, 0u, 20u};
    listing.buyerCeilingPerItem = 25u;
    purchase.auctions.push_back(listing);

    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(purchase).auctionId, 2u);

    EconomySnapshot sale;
    sale.guidCounter = 42u;
    SaleItemCandidate item;
    item.itemGuidCounter = 20u;
    item.itemId = 10u;
    item.count = 2u;
    item.usage = ITEM_USAGE_AH;
    item.canBeTraded = true;
    item.inventoryCount = 2u;
    item.professionRelated = true;
    item.deposit = 5u;
    item.minimumTransactionBasis = 1u;
    item.lowestCompetingBuyoutPerItem = 2u;
    item.buyerCeilingPerItem = 10u;
    sale.saleItems.push_back(item);

    EconomyDecision decision = PlayerbotEconomyPolicy::Decide(sale);
    ASSERT_EQ(decision.phase, EconomyPhase::SellSurplus);
    EXPECT_EQ(decision.buyout, 6u);
    EXPECT_EQ(decision.startBid, 6u);

    sale.saleItems.front().buyerCeilingPerItem = 2u;
    decision = PlayerbotEconomyPolicy::Decide(sale);
    EXPECT_EQ(decision.phase, EconomyPhase::None);
    EXPECT_EQ(decision.blocker, EconomyDecisionBlocker::PriceCorridor);
}

TEST(PlayerbotEconomyPolicyTest, PureGatheringMaterialHasNoUncommittedReserve)
{
    SaleItemCandidate item;
    item.pureGatheringMaterial = true;
    item.professionReserveFloor = 40u;

    EXPECT_EQ(PlayerbotEconomyPolicy::EffectiveProfessionReserve(item), 0u);

    item.pureGatheringMaterial = false;
    EXPECT_EQ(PlayerbotEconomyPolicy::EffectiveProfessionReserve(item), 40u);
}

TEST(PlayerbotEconomyPolicyTest, OnlySafeAuctionUsageInstancesBecomeListings)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.saleItems = {
        {11u, 500u, 4u, ITEM_USAGE_AH, true, true, false, 0u, false, 0u, false, 30u, 10u, 25u, 4u, 0u, true},
        {12u, 500u, 4u, ITEM_USAGE_AH, true, false, false, 0u, false, 0u, false, 30u, 10u, 25u, 4u, 0u, true},
        {13u, 501u, 4u, ITEM_USAGE_SKILL, true, false, false, 0u, false, 0u, false, 30u, 10u, 25u, 4u, 0u, true},
        {14u, 502u, 4u, ITEM_USAGE_KEEP, true, false, false, 0u, false, 0u, false, 30u, 10u, 25u, 4u, 0u, true},
        {15u, 503u, 1u, ITEM_USAGE_AH, true, false, true, 1u, false, 0u, false, 30u, 10u, 25u, 1u, 0u, true},
        {16u, 504u, 1u, ITEM_USAGE_AH, true, false, false, 0u, true, 0u, false, 30u, 10u, 25u, 1u, 0u, true},
        {17u, 505u, 1u, ITEM_USAGE_AH, true, false, false, 0u, false, 30u, false, 30u, 10u, 25u, 1u, 0u, true},
        {18u, 506u, 1u, ITEM_USAGE_AH, true, false, false, 0u, false, 0u, true, 30u, 10u, 25u, 1u, 0u, true}};

    EconomyDecision decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::SellSurplus);
    EXPECT_EQ(decision.itemGuidCounter, 12u);
    EXPECT_EQ(decision.itemId, 500u);
    EXPECT_EQ(decision.count, 4u);
    EXPECT_EQ(decision.startBid, 40u);
    EXPECT_EQ(decision.buyout, 100u);

    snapshot.saleItems = {
        {19u, 507u, 2u, ITEM_USAGE_AH, true, false, false, 0u, false, 0u, false, 30u, 0u, 0u, 2u, 0u, true}};
    decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::SellSurplus);
    EXPECT_EQ(decision.itemGuidCounter, 19u);
    EXPECT_EQ(decision.startBid, 1u);
    EXPECT_EQ(decision.buyout, 60u);

    snapshot.saleItems.front().templateBuyPrice = 0u;
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
}

TEST(PlayerbotEconomyPolicyTest, ProfessionSalesListOnlyThePostReserveSurplus)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;

    SaleItemCandidate fullStack;
    fullStack.itemGuidCounter = 20u;
    fullStack.itemId = 508u;
    fullStack.count = 20u;
    fullStack.usage = ITEM_USAGE_AH;
    fullStack.canBeTraded = true;
    fullStack.templateBuyPrice = 30u;
    fullStack.templateSellPrice = 10u;
    fullStack.inventoryCount = 41u;
    fullStack.professionReserveFloor = 40u;
    fullStack.professionRelated = true;
    snapshot.saleItems.push_back(fullStack);

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::SellSurplus);
    EXPECT_EQ(decision.itemGuidCounter, 20u);
    EXPECT_EQ(decision.count, 1u);
    EXPECT_EQ(decision.professionReserveFloor, 40u);
}

TEST(PlayerbotEconomyPolicyTest, ProfessionSaleSplitsAStackAtThePostReserveSurplusBoundary)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;

    SaleItemCandidate material;
    material.itemGuidCounter = 30u;
    material.itemId = 2589u;
    material.count = 20u;
    material.usage = ITEM_USAGE_AH;
    material.canBeTraded = true;
    material.templateBuyPrice = 20u;
    material.templateSellPrice = 5u;
    material.inventoryCount = 45u;
    material.professionReserveFloor = 44u;
    material.professionRelated = true;
    material.deposit = 7u;
    snapshot.saleItems.push_back(material);

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::SellSurplus);
    EXPECT_EQ(decision.count, 1u);
    EXPECT_EQ(decision.deposit, 1u);
    EXPECT_EQ(decision.professionReserveFloor, 44u);

    snapshot.controlledItemGuids.push_back(material.itemGuidCounter);
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
}

TEST(PlayerbotEconomyPolicyTest, ProductionReserveIncludesImmediateUseAndConfiguredStacks)
{
    EconomySnapshot snapshot;
    snapshot.recipes = {
        {.spellId = 1u, .craftedItemId = 100u, .reagents = {{2589u, 2u, false}}},
        {.spellId = 2u, .craftedItemId = 101u, .reagents = {{2589u, 4u, false}}},
    };

    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionReserve(snapshot, 2589u, 40u), 44u);
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionReserve(snapshot, 2592u, 40u), 40u);
}

TEST(PlayerbotEconomyPolicyTest, ClothHerbsOreAndLeatherAreCirculationMaterials)
{
    EXPECT_TRUE(PlayerbotEconomyPolicy::IsCirculationMaterial(ITEM_CLASS_TRADE_GOODS, ITEM_SUBCLASS_CLOTH));
    EXPECT_TRUE(PlayerbotEconomyPolicy::IsCirculationMaterial(ITEM_CLASS_TRADE_GOODS, ITEM_SUBCLASS_HERB));
    EXPECT_TRUE(PlayerbotEconomyPolicy::IsCirculationMaterial(ITEM_CLASS_TRADE_GOODS, ITEM_SUBCLASS_METAL_STONE));
    EXPECT_TRUE(PlayerbotEconomyPolicy::IsCirculationMaterial(ITEM_CLASS_TRADE_GOODS, ITEM_SUBCLASS_LEATHER));
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsCirculationMaterial(ITEM_CLASS_ARMOR, ITEM_SUBCLASS_CLOTH));
}

TEST(PlayerbotEconomyPolicyTest, GenericAuctionLootIsNotAProfessionSale)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.saleItems = {
        {22u, 600u, 1u, ITEM_USAGE_AH, true, false, false, 0u, false, 0u, false, 30u, 10u, 25u, 1u, 0u, false}};

    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);
}

TEST(PlayerbotEconomyPolicyTest, RecipeAuctionRequiresAConcreteSafeMarketPricedListing)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.botAccountId = 7u;
    snapshot.freeMoneyForTradeskill = 100u;
    AuctionListingCandidate sameAccount{1u, 7u, 700u, 1u, 20u, 0u, 0u, true};
    sameAccount.buyerCeilingPerItem = 50u;
    AuctionListingCandidate overBudget{2u, 8u, 701u, 1u, 101u, 0u, 0u, true};
    overBudget.buyerCeilingPerItem = 200u;
    AuctionListingCandidate irrelevant{3u, 8u, 702u, 1u, 20u, 0u, 0u, false};
    irrelevant.buyerCeilingPerItem = 50u;
    AuctionListingCandidate overPrice{4u, 8u, 703u, 1u, 51u, 0u, 0u, true};
    overPrice.buyerCeilingPerItem = 50u;
    AuctionListingCandidate inaccessible{5u, 8u, 704u, 1u, 20u, 0u, 0u, true};
    inaccessible.buyerCeilingPerItem = 50u;
    inaccessible.accessible = false;
    AuctionListingCandidate usable{6u, 8u, 705u, 1u, 20u, 0u, 0u, true};
    usable.buyerCeilingPerItem = 50u;
    usable.recipeSpellId = 9001u;
    snapshot.auctions = {sameAccount, overBudget, irrelevant, overPrice, inaccessible, usable};

    EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(snapshot);
    ASSERT_EQ(decision.phase, EconomyPhase::BuyRecipe);
    EXPECT_EQ(decision.auctionId, 6u);
    EXPECT_EQ(decision.itemId, 705u);
    EXPECT_EQ(decision.buyout, 20u);
    EXPECT_EQ(decision.recipeSpellId, 9001u);
}

TEST(PlayerbotEconomyCycleActionTest, RuntimeReserveGuardRejectsAFullStackAndAllowsOnlySurplus)
{
    EXPECT_FALSE(PlayerbotEconomyPolicy::PreservesProfessionReserve(41u, 20u, 40u));
    EXPECT_TRUE(PlayerbotEconomyPolicy::PreservesProfessionReserve(41u, 1u, 40u));
    EXPECT_TRUE(PlayerbotEconomyPolicy::PreservesProfessionReserve(1u, 1u, 0u));
}

TEST(PlayerbotEconomyCycleActionTest, NormalSchoolCreateItemSpellsAreProfessionRecipes)
{
    EXPECT_TRUE(
        PlayerbotEconomyPolicy::IsProfessionRecipeSpell(SPELL_EFFECT_CREATE_ITEM, 2851u, 6, SPELL_SCHOOL_MASK_NORMAL));
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsProfessionRecipeSpell(SPELL_EFFECT_CREATE_ITEM, 2851u, 6, 0u));
    EXPECT_FALSE(
        PlayerbotEconomyPolicy::IsProfessionRecipeSpell(SPELL_EFFECT_CREATE_ITEM, 2851u, 6, SPELL_SCHOOL_MASK_FIRE));
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsProfessionRecipeSpell(SPELL_EFFECT_SCHOOL_DAMAGE, 2851u, 6,
                                                                 SPELL_SCHOOL_MASK_NORMAL));
    EXPECT_FALSE(
        PlayerbotEconomyPolicy::IsProfessionRecipeSpell(SPELL_EFFECT_CREATE_ITEM, 0u, 6, SPELL_SCHOOL_MASK_NORMAL));
    EXPECT_FALSE(
        PlayerbotEconomyPolicy::IsProfessionRecipeSpell(SPELL_EFFECT_CREATE_ITEM, 2851u, 0, SPELL_SCHOOL_MASK_NORMAL));
}

TEST(PlayerbotEconomyCycleActionTest, BackgroundBotsCanRunTheEconomyCycle)
{
    std::unique_ptr<Strategy> strategy = EconomyStrategy();
    ASSERT_NE(strategy, nullptr);
    std::vector<TriggerNode*> triggers;
    strategy->InitTriggers(triggers);

    std::vector<NextAction> timerActions;
    for (TriggerNode* trigger : triggers)
    {
        if (trigger->getName() == "timer")
            timerActions = trigger->getHandlers();
    }

    for (TriggerNode* trigger : triggers)
        delete trigger;

    auto const economyAction = std::find_if(timerActions.begin(), timerActions.end(),
                                            [](NextAction& action) { return action.getName() == "economy cycle"; });

    ASSERT_NE(economyAction, timerActions.end());
    EXPECT_GE(economyAction->getRelevance(), 100.0f);
}

TEST(PlayerbotEconomyCycleActionTest, BackgroundBotsCanAdvanceForcedTravelTargets)
{
    std::unique_ptr<Strategy> strategy = EconomyStrategy();
    ASSERT_NE(strategy, nullptr);
    std::vector<TriggerNode*> triggers;
    strategy->InitTriggers(triggers);

    std::vector<NextAction> cycleActions;
    std::vector<NextAction> travelActions;
    for (TriggerNode* trigger : triggers)
    {
        if (trigger->getName() == "timer")
            cycleActions = trigger->getHandlers();
        if (trigger->getName() == "far from travel target")
            travelActions = trigger->getHandlers();
    }

    for (TriggerNode* trigger : triggers)
        delete trigger;

    auto const cycleAction = std::find_if(cycleActions.begin(), cycleActions.end(),
                                          [](NextAction& action) { return action.getName() == "economy cycle"; });
    auto const moveAction = std::find_if(travelActions.begin(), travelActions.end(), [](NextAction& action)
                                         { return action.getName() == "move to travel target"; });

    ASSERT_NE(cycleAction, cycleActions.end());
    ASSERT_NE(moveAction, travelActions.end());
    EXPECT_GT(moveAction->getRelevance(), cycleAction->getRelevance());
}

TEST(PlayerbotEconomyPolicyTest, DeterministicTieBreakAndCadenceMatchLiteralContract)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;
    snapshot.recipes = {{101u, 201u, true, 1u, {}}, {100u, 200u, true, 1u, {}}};

    EconomyDecision const first = PlayerbotEconomyPolicy::Decide(snapshot);
    EconomyDecision const second = PlayerbotEconomyPolicy::Decide(snapshot);
    EXPECT_EQ(first.spellId, 100u);
    EXPECT_EQ(second.spellId, 100u);

    EXPECT_EQ(PlayerbotEconomyPolicy::InitialEligibleTime(1000u, 42u, 20u), 1003u);
    EXPECT_EQ(PlayerbotEconomyPolicy::InitialEligibleTime(1000u, 43u, 20u), 1015u);
    EXPECT_EQ(PlayerbotEconomyPolicy::CareerIntervalSeconds(20u, 25u), 80u);
    EXPECT_EQ(PlayerbotEconomyPolicy::CareerIntervalSeconds(20u, 50u), 60u);
    EXPECT_EQ(PlayerbotEconomyPolicy::CareerIntervalSeconds(20u, 75u), 40u);
    EXPECT_EQ(PlayerbotEconomyPolicy::CareerIntervalSeconds(20u, 100u), 20u);
    EXPECT_EQ(PlayerbotEconomyPolicy::NextEligibleTime(1000u, 20u, EconomyAttemptOutcome::InProgress, 4u), 1001u);
    // A trip in flight polls on its own short cadence whatever the engagement interval or failure count.
    EXPECT_EQ(PlayerbotEconomyPolicy::NextEligibleTime(1000u, 80u, EconomyAttemptOutcome::Tracking, 4u), 1005u);
    EXPECT_EQ(PlayerbotEconomyPolicy::NextEligibleTime(1000u, 20u, EconomyAttemptOutcome::Operation, 4u), 1020u);
    EXPECT_EQ(PlayerbotEconomyPolicy::NextEligibleTime(1000u, 20u, EconomyAttemptOutcome::FailedPrecondition, 1u),
              1040u);
    EXPECT_EQ(PlayerbotEconomyPolicy::NextEligibleTime(1000u, 20u, EconomyAttemptOutcome::NoCandidate, 1u), 1080u);
    EXPECT_EQ(PlayerbotEconomyPolicy::NextEligibleTime(1000u, 20u, EconomyAttemptOutcome::NoCandidate, 8u), 1640u);
    EXPECT_EQ(PlayerbotEconomyPolicy::NextEligibleTime(1000u, 0u, EconomyAttemptOutcome::NoCandidate, 1u), 1004u);
}

TEST(PlayerbotEconomyPolicyTest, EconomyTelemetryPreservesProductionOutcomeAndBackoff)
{
    PlayerbotEconomyTelemetry telemetry;
    EXPECT_FALSE(telemetry.Find(42u).has_value());

    telemetry.Publish(42u, {.outcome = PlayerbotEconomyOutcome::Scheduled,
                            .phase = PlayerbotEconomyTelemetryPhase::BuyReagent,
                            .workOrderSpellId = 1001u,
                            .nextEligibleTime = 1020u});
    PlayerbotEconomyObservation observation = *telemetry.Find(42u);
    EXPECT_EQ(observation.sequence, 1u);
    EXPECT_EQ(observation.outcome, PlayerbotEconomyOutcome::Scheduled);
    EXPECT_EQ(observation.phase, PlayerbotEconomyTelemetryPhase::BuyReagent);
    EXPECT_EQ(observation.workOrderSpellId, 1001u);
    EXPECT_EQ(observation.consecutiveFailures, 0u);
    EXPECT_EQ(observation.nextEligibleTime, 1020u);

    telemetry.Publish(42u, {.outcome = PlayerbotEconomyOutcome::Operation,
                            .phase = PlayerbotEconomyTelemetryPhase::Craft,
                            .nextEligibleTime = 1040u});
    telemetry.Publish(42u, {.outcome = PlayerbotEconomyOutcome::NoCandidate,
                            .phase = PlayerbotEconomyTelemetryPhase::None,
                            .consecutiveFailures = 1u,
                            .nextEligibleTime = 1120u});
    telemetry.Publish(42u, {.outcome = PlayerbotEconomyOutcome::FailedPrecondition,
                            .phase = PlayerbotEconomyTelemetryPhase::SellSurplus,
                            .consecutiveFailures = 2u,
                            .nextEligibleTime = 1200u});

    observation = *telemetry.Find(42u);
    EXPECT_EQ(observation.sequence, 4u);
    EXPECT_EQ(observation.outcome, PlayerbotEconomyOutcome::FailedPrecondition);
    EXPECT_EQ(observation.phase, PlayerbotEconomyTelemetryPhase::SellSurplus);
    EXPECT_EQ(observation.consecutiveFailures, 2u);
    EXPECT_EQ(observation.nextEligibleTime, 1200u);
}

TEST(PlayerbotAuctionMailTest, RemovalRequiresEmptyMoneyAndAttachments)
{
    EXPECT_FALSE(PlayerbotEconomyMailIsFullyCollected(1u, 0u));
    EXPECT_FALSE(PlayerbotEconomyMailIsFullyCollected(0u, 1u));
    EXPECT_FALSE(PlayerbotEconomyMailIsFullyCollected(1u, 1u));
    EXPECT_TRUE(PlayerbotEconomyMailIsFullyCollected(0u, 0u));
}

TEST(PlayerbotEconomyPolicyTest, EligibilityRequiresAnAutonomousSafeRandomBot)
{
    EconomyEligibility eligibility;
    EXPECT_TRUE(PlayerbotEconomyPolicy::IsEligible(eligibility));

    eligibility.enabled = false;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.enabled = true;

    eligibility.randomBot = false;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.randomBot = true;

    eligibility.activePlayerMaster = true;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.activePlayerMaster = false;

    eligibility.inCombat = true;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.inCombat = false;

    eligibility.inBattleground = true;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.inBattleground = false;

    eligibility.dead = true;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.dead = false;

    eligibility.teleporting = true;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.teleporting = false;

    eligibility.careerMarketEligible = false;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
    eligibility.careerMarketEligible = true;

    eligibility.hasActionableProfessionWork = false;
    EXPECT_FALSE(PlayerbotEconomyPolicy::IsEligible(eligibility));
}

namespace
{
AuctionListingCandidate ReagentListing(uint32 auctionId, uint32 itemId, uint32 count, uint64 buyout)
{
    AuctionListingCandidate listing;
    listing.auctionId = auctionId;
    listing.ownerAccountId = 99u;
    listing.itemId = itemId;
    listing.count = count;
    listing.buyout = buyout;
    listing.templateBuyPrice = 100u;
    listing.reserveCeiling = 1000u;
    return listing;
}
}  // namespace

TEST(PlayerbotEconomyPolicyTest, ProductionBatchQuantityBindsOnEachReagentSourceAndTheCeiling)
{
    RecipeCandidate recipe;
    recipe.spellId = 2964u;
    recipe.craftedItemId = 2997u;
    recipe.reagents = {{2589u, 2u, false}};

    EconomySnapshot snapshot;
    snapshot.botAccountId = 11u;
    snapshot.freeMoneyForTradeskill = 10'000u;

    // Nothing available anywhere: the batch is zero and the recipe is not claimable.
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 5u), 0u);

    // Inventory binds: 5 held with 2 needed per craft supports 2 crafts.
    snapshot.inventory = {{2589u, 5u, 0u, 0u, 0u}};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 10u), 2u);

    // Mail joins inventory: 5 held plus 3 inbound supports 4 crafts.
    snapshot.inventory = {{2589u, 5u, 3u, 0u, 0u}};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 10u), 4u);

    // Purchasable auctions join the pool; an inaccessible listing does not.
    snapshot.inventory.clear();
    AuctionListingCandidate inaccessible = ReagentListing(2u, 2589u, 40u, 40u);
    inaccessible.accessible = false;
    snapshot.auctions = {ReagentListing(1u, 2589u, 6u, 6u), inaccessible};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 10u), 3u);

    // The ceiling binds when reagents are abundant.
    snapshot.auctions.clear();
    snapshot.inventory = {{2589u, 200u, 0u, 0u, 0u}};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 5u), 5u);

    // A zero ceiling disables the hard cap but keeps the reagent bound.
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 0u), 100u);

    // Unlimited gold vendor supply is bounded only by the ceiling.
    recipe.reagents = {{2589u, 2u, true}};
    snapshot.inventory.clear();
    snapshot.auctions.clear();
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 7u), 7u);

    // The scarcest reagent binds a multi-reagent recipe.
    recipe.reagents = {{2589u, 1u, false}, {2320u, 2u, false}};
    snapshot.inventory = {{2589u, 4u, 0u, 0u, 0u}, {2320u, 4u, 0u, 0u, 0u}};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 10u), 2u);
}

TEST(PlayerbotEconomyPolicyTest, ProductionBatchQuantityCountsOnlyAuctionsTheBotCouldActuallyBuy)
{
    RecipeCandidate recipe;
    recipe.spellId = 2964u;
    recipe.craftedItemId = 2997u;
    recipe.reagents = {{2589u, 1u, false}};

    EconomySnapshot snapshot;
    snapshot.botAccountId = 11u;
    snapshot.freeMoneyForTradeskill = 100u;

    // Affordable control: an eligible listing within ceiling and budget counts.
    snapshot.auctions = {ReagentListing(1u, 2589u, 4u, 40u)};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 20u), 4u);

    // A same-account listing never counts.
    AuctionListingCandidate ownListing = ReagentListing(2u, 2589u, 8u, 8u);
    ownListing.ownerAccountId = snapshot.botAccountId;
    snapshot.auctions = {ownListing};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 20u), 0u);

    // A listing without a buyout never counts.
    snapshot.auctions = {ReagentListing(3u, 2589u, 8u, 0u)};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 20u), 0u);

    // A listing priced above the buyer ceiling never counts.
    AuctionListingCandidate overpriced = ReagentListing(4u, 2589u, 2u, 50'000u);
    snapshot.auctions = {overpriced};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 20u), 0u);

    // Two individually affordable listings stop counting once the shared budget runs out.
    snapshot.auctions = {ReagentListing(5u, 2589u, 3u, 60u), ReagentListing(6u, 2589u, 3u, 70u)};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 20u), 3u);

    // The budget is shared across reagents, not per reagent.
    recipe.reagents = {{2589u, 1u, false}, {2320u, 1u, false}};
    snapshot.auctions = {ReagentListing(7u, 2589u, 3u, 60u), ReagentListing(8u, 2320u, 3u, 60u)};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 20u), 0u);

    // A listing that would breach the item's hoard reserve ceiling never counts.
    recipe.reagents = {{2589u, 1u, false}};
    AuctionListingCandidate hoard = ReagentListing(9u, 2589u, 4u, 4u);
    hoard.reserveCeiling = 3u;
    snapshot.auctions = {hoard};
    EXPECT_EQ(PlayerbotEconomyPolicy::ProductionBatchQuantity(recipe, snapshot, 20u), 0u);
}

TEST(PlayerbotEconomyPolicyTest, ApplicableUnlimitedGoldVendorFactsUseEveryAccessBoundary)
{
    VendorOfferPolicyInput const accessible{
        .maximumCount = 0u,
        .extendedCost = 0u,
        .factionAllowed = true,
        .levelAllowed = true,
        .reputationAllowed = true,
        .sameMap = true,
        .routeAvailable = true,
    };

    std::array inaccessible{
        &VendorOfferPolicyInput::factionAllowed,    &VendorOfferPolicyInput::levelAllowed,
        &VendorOfferPolicyInput::reputationAllowed, &VendorOfferPolicyInput::sameMap,
        &VendorOfferPolicyInput::routeAvailable,
    };
    std::vector<VendorOfferPolicyInput> offers{accessible};
    for (bool VendorOfferPolicyInput::*field : inaccessible)
    {
        VendorOfferPolicyInput candidate = accessible;
        candidate.*field = false;
        offers.push_back(candidate);
    }
    VendorOfferPolicyInput finite = accessible;
    finite.maximumCount = 1u;
    offers.push_back(finite);
    VendorOfferPolicyInput specialCurrency = accessible;
    specialCurrency.extendedCost = 1u;
    offers.push_back(specialCurrency);

    for (std::size_t index = 0; index < offers.size(); ++index)
    {
        bool const applicable = PlayerbotEconomyPolicy::IsApplicableUnlimitedGoldVendorOffer(offers[index]);
        EXPECT_EQ(applicable, index == 0u);
        EXPECT_EQ(PlayerbotEconomyPolicy::AllowsAutonomousListing({applicable, false, false}), !applicable);

        ConsumptionSnapshot consumption;
        consumption.needs.push_back(
            PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Food, 100u, 1u, true, 500u, applicable}));
        EXPECT_EQ(PlayerbotEconomyConsumption::DemandFacts(consumption).empty(), applicable);

        EconomySnapshot economy;
        SaleItemCandidate sale;
        sale.itemGuidCounter = 20u;
        sale.itemId = 4540u;
        sale.count = 1u;
        sale.usage = ITEM_USAGE_AH;
        sale.canBeTraded = true;
        sale.templateBuyPrice = 10u;
        sale.templateSellPrice = 1u;
        sale.inventoryCount = 1u;
        sale.professionRelated = true;
        sale.buyerCeilingPerItem = 10u;
        sale.ordinaryVendorSupply = applicable;
        economy.saleItems.push_back(sale);
        EXPECT_EQ(PlayerbotEconomyPolicy::Decide(economy).phase,
                  applicable ? EconomyPhase::None : EconomyPhase::SellSurplus);
    }
}

TEST(PlayerbotEconomyPolicyTest, VendorAndUndemandedTrainingOutputsNeverList)
{
    EconomySnapshot snapshot;
    snapshot.guidCounter = 42u;

    SaleItemCandidate candidate;
    candidate.itemGuidCounter = 20u;
    candidate.itemId = 4540u;
    candidate.count = 1u;
    candidate.usage = ITEM_USAGE_AH;
    candidate.canBeTraded = true;
    candidate.templateBuyPrice = 10u;
    candidate.templateSellPrice = 1u;
    candidate.inventoryCount = 1u;
    candidate.professionRelated = true;
    candidate.buyerCeilingPerItem = 10u;

    candidate.ordinaryVendorSupply = true;
    snapshot.saleItems = {candidate};
    EXPECT_FALSE(PlayerbotEconomyPolicy::AllowsAutonomousListing({true, false, false}));
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);

    snapshot.saleItems.front().ordinaryVendorSupply = false;
    snapshot.saleItems.front().trainingOutput = true;
    EXPECT_FALSE(PlayerbotEconomyPolicy::AllowsAutonomousListing({false, true, false}));
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::None);

    snapshot.saleItems.front().independentDemand = true;
    EXPECT_TRUE(PlayerbotEconomyPolicy::AllowsAutonomousListing({false, true, true}));
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(snapshot).phase, EconomyPhase::SellSurplus);
}

TEST(PlayerbotEconomyPolicyTest, ApproachPointStandsOffTheObjectOnTheBotsSide)
{
    float const objectX = -8815.0f;
    float const objectY = 653.0f;
    float const botX = -8900.0f;
    float const botY = 700.0f;
    EconomyApproachPoint const point = ApproachPoint(objectX, objectY, botX, botY, 3.0f, 42u);

    float const offX = point.x - objectX;
    float const offY = point.y - objectY;
    EXPECT_NEAR(std::sqrt(offX * offX + offY * offY), 3.0f, 0.01f);
    // On the approach side: the offset points toward the bot, never behind the object.
    EXPECT_GT(offX * (botX - objectX) + offY * (botY - objectY), 0.0f);

    EconomyApproachPoint const other = ApproachPoint(objectX, objectY, botX, botY, 3.0f, 777u);
    EXPECT_GT(std::fabs(other.x - point.x) + std::fabs(other.y - point.y), 0.1f);

    EconomyApproachPoint const onTop = ApproachPoint(objectX, objectY, objectX, objectY, 3.0f, 42u);
    float const topX = onTop.x - objectX;
    float const topY = onTop.y - objectY;
    EXPECT_NEAR(std::sqrt(topX * topX + topY * topY), 3.0f, 0.01f);
}
