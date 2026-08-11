/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <memory>

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

    snapshot.inventory.front().count = 1u;
    decision = PlayerbotEconomyPolicy::Decide(snapshot);
    EXPECT_EQ(decision.phase, EconomyPhase::None);

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

    std::vector<NextAction> travelActions;
    for (TriggerNode* trigger : triggers)
    {
        if (trigger->getName() == "far from travel target")
            travelActions = trigger->getHandlers();
    }

    for (TriggerNode* trigger : triggers)
        delete trigger;

    auto const moveAction = std::find_if(travelActions.begin(), travelActions.end(), [](NextAction& action)
                                         { return action.getName() == "move to travel target"; });

    ASSERT_NE(moveAction, travelActions.end());
    EXPECT_GE(moveAction->getRelevance(), 100.0f);
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
