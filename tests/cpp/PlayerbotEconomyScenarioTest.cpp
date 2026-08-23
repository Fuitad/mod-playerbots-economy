/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "Bot/Economy/PlayerbotEconomyConsumption.h"
#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "Bot/Economy/PlayerbotEconomyRuntime.h"
#include "Bot/Economy/PlayerbotEconomyTrace.h"
#include "Bot/Personality/PlayerbotCareerProgression.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr uint64 NOW = 1'785'000'000u;
constexpr uint32 MARKET_ID = 7u;
constexpr uint32 GATHERER_GUID = 10u;
constexpr uint32 PRODUCER_GUID = 20u;
constexpr uint32 CONSUMER_GUID = 30u;
constexpr uint32 SPECULATOR_GUID = 40u;
constexpr uint32 GATHERER_ACCOUNT = 110u;
constexpr uint32 PRODUCER_ACCOUNT = 120u;
constexpr uint32 CONSUMER_ACCOUNT = 130u;
constexpr uint32 SPECULATOR_ACCOUNT = 140u;

struct ProfessionScenario
{
    char const* name;
    uint16 sourceProfession;
    uint16 producerProfession;
    uint16 secondaryInputProfession;
    uint32 inputItemId;
    uint32 outputItemId;
    uint32 recipeSpellId;
    EconomySubstitutionGroup outputGroup;
    FinishedGoodUse finalUse;
    EconomyFinalUseKind finalUseTrace;
};

struct ItemLedger
{
    uint32 ordinarySourceInflow = 0u;
    uint32 sourceInventory = 0u;
    uint32 producerInventory = 0u;
    uint32 consumerInventory = 0u;
    uint32 mail = 0u;
    uint32 activeAuctions = 0u;
    uint32 rawConsumed = 0u;
    uint32 outputProduced = 0u;
    uint32 finalUse = 0u;
    uint32 positionQuantity = 0u;
};

struct GoldLedger
{
    uint64 gatherer = 1'000u;
    uint64 producer = 1'000u;
    uint64 consumer = 1'000u;
    uint64 depositEscrow = 0u;
    uint64 auctionCuts = 0u;
    uint64 proceedsMail = 0u;

    [[nodiscard]] uint64 Total() const
    {
        return gatherer + producer + consumer + depositEscrow + auctionCuts + proceedsMail;
    }
};

void ListAuction(ItemLedger& items, GoldLedger& gold, bool raw, uint32 quantity, uint64 deposit)
{
    uint32& inventory = raw ? items.sourceInventory : items.producerInventory;
    ASSERT_GE(inventory, quantity);
    ASSERT_GE(raw ? gold.gatherer : gold.producer, deposit);
    inventory -= quantity;
    items.activeAuctions += quantity;
    if (raw)
        gold.gatherer -= deposit;
    else
        gold.producer -= deposit;
    gold.depositEscrow += deposit;
}

void BuyAuction(ItemLedger& items, GoldLedger& gold, bool raw, uint32 quantity, uint64 buyout, uint64 deposit,
                uint64 cut)
{
    uint64& buyerGold = raw ? gold.producer : gold.consumer;
    ASSERT_GE(items.activeAuctions, quantity);
    ASSERT_GE(buyerGold, buyout);
    ASSERT_GE(gold.depositEscrow, deposit);
    buyerGold -= buyout;
    items.activeAuctions -= quantity;
    items.mail += quantity;
    gold.depositEscrow -= deposit;
    gold.auctionCuts += cut;
    gold.proceedsMail += buyout + deposit - cut;
}

void ExpireAuction(ItemLedger& items, GoldLedger& gold, uint32 quantity, uint64 deposit)
{
    ASSERT_GE(items.activeAuctions, quantity);
    ASSERT_GE(gold.depositEscrow, deposit);
    items.activeAuctions -= quantity;
    items.mail += quantity;
    gold.depositEscrow -= deposit;
    gold.auctionCuts += deposit;
}

void CollectAuctionMail(ItemLedger& items, GoldLedger& gold, bool raw, uint32 quantity)
{
    ASSERT_GE(items.mail, quantity);
    items.mail -= quantity;
    if (raw)
    {
        items.producerInventory += quantity;
        gold.gatherer += gold.proceedsMail;
    }
    else
    {
        items.consumerInventory += quantity;
        gold.producer += gold.proceedsMail;
    }
    gold.proceedsMail = 0u;
}

void CollectExpiredAuctionMail(ItemLedger& items, uint32 quantity)
{
    ASSERT_GE(items.mail, quantity);
    items.mail -= quantity;
    items.producerInventory += quantity;
}

EconomyActorFacts Actor(uint32 guid, uint32 account, std::vector<uint16> professions = {})
{
    return {
        .characterGuid = guid,
        .accountId = account,
        .marketId = MARKET_ID,
        .online = true,
        .autonomous = true,
        .craftingAffinity = 100u,
        .gatheringAffinity = 100u,
        .economyAffinity = 100u,
        .professionSkillIds = std::move(professions),
    };
}

EconomyAssignmentRequest PurchaseRequest(uint32 guid, EconomySubstitutionGroup group, EconomyClaimPriority priority,
                                         uint32 sellerAccount)
{
    return {
        .characterGuid = guid,
        .marketId = MARKET_ID,
        .group = group,
        .quantity = 4u,
        .kind = EconomyClaimKind::Purchase,
        .priority = priority,
        .workKind =
            priority == EconomyClaimPriority::Speculation ? EconomyWorkKind::MarketMaking : EconomyWorkKind::Buy,
        .workIdentity = priority == EconomyClaimPriority::Speculation ? "position:raw" : "producer:raw",
        .sellerAccountId = sellerAccount,
        .expiresAt = NOW + 60u,
    };
}

EconomyTraceRecord TraceRecord(std::string key, std::string const& chainId, EconomyTraceKind kind, uint32 itemId,
                               uint32 quantity, uint64 occurredAt)
{
    return {
        .deduplicationKey = std::move(key),
        .chainPublicId = chainId,
        .actorGuid = PRODUCER_GUID,
        .counterpartyGuid = CONSUMER_GUID,
        .itemId = itemId,
        .quantity = quantity,
        .occurredAt = occurredAt,
        .kind = kind,
    };
}

std::array<ProfessionScenario, 9> Scenarios()
{
    return {
        ProfessionScenario{"Mining to Blacksmithing", 186u, 164u, 0u, 2770u, 2844u, 2661u,
                           EconomySubstitutionGroup::Equipment(16u, 1u, 1u), FinishedGoodUse::Equip,
                           EconomyFinalUseKind::Equipped},
        ProfessionScenario{"Mining to Engineering", 186u, 202u, 0u, 2835u, 4364u, 3925u,
                           EconomySubstitutionGroup::Ammunition(2u, 1u), FinishedGoodUse::SetAmmunition,
                           EconomyFinalUseKind::AmmunitionSet},
        ProfessionScenario{"Mining to Jewelcrafting", 186u, 755u, 0u, 2772u, 774u, 25255u,
                           EconomySubstitutionGroup::Enhancement(1u, 1u), FinishedGoodUse::Apply,
                           EconomyFinalUseKind::Applied},
        ProfessionScenario{"Herbalism to Alchemy", 182u, 171u, 0u, 2447u, 118u, 2330u,
                           EconomySubstitutionGroup::Consumable(1u, 1u), FinishedGoodUse::Consume,
                           EconomyFinalUseKind::Consumed},
        ProfessionScenario{"Herbalism to Inscription", 182u, 773u, 0u, 765u, 42912u, 48114u,
                           EconomySubstitutionGroup::Enhancement(2u, 1u), FinishedGoodUse::Apply,
                           EconomyFinalUseKind::Applied},
        ProfessionScenario{"Skinning to Leatherworking", 393u, 165u, 0u, 2318u, 2300u, 2160u,
                           EconomySubstitutionGroup::Equipment(5u, 1u, 1u), FinishedGoodUse::Equip,
                           EconomyFinalUseKind::Equipped},
        ProfessionScenario{"Ordinary cloth to Tailoring", 0u, 197u, 0u, 2589u, 4238u, 3755u,
                           EconomySubstitutionGroup::Bag(6u), FinishedGoodUse::Equip, EconomyFinalUseKind::Equipped},
        ProfessionScenario{"Inscription vellum to Enchanting", 773u, 333u, 0u, 38682u, 38837u, 7421u,
                           EconomySubstitutionGroup::Enhancement(3u, 1u), FinishedGoodUse::Apply,
                           EconomyFinalUseKind::Applied},
        ProfessionScenario{"Mining and Skinning to Engineering", 186u, 202u, 393u, 2836u, 4374u, 3945u,
                           EconomySubstitutionGroup::Equipment(12u, 1u, 1u), FinishedGoodUse::Equip,
                           EconomyFinalUseKind::Equipped},
    };
}
}  // namespace

TEST(PlayerbotEconomyScenarioTest, EveryProfessionFamilyReconcilesOrdinarySourceThroughAuctionFinalUse)
{
    std::unique_ptr<PlayerbotEconomyRuntime> economyRuntime = CreatePlayerbotEconomyRuntime();
    for (ProfessionScenario const& scenario : Scenarios())
    {
        SCOPED_TRACE(scenario.name);
        PlayerbotEconomyCoordinator coordinator;
        EconomySubstitutionGroup const raw = EconomySubstitutionGroup::ExactReagent(scenario.inputItemId);
        EconomyActorFacts gatherer = Actor(
            GATHERER_GUID, GATHERER_ACCOUNT,
            scenario.sourceProfession == 0u ? std::vector<uint16>{} : std::vector<uint16>{scenario.sourceProfession});
        EconomyActorFacts producer =
            Actor(PRODUCER_GUID, PRODUCER_ACCOUNT,
                  scenario.secondaryInputProfession == 0u
                      ? std::vector<uint16>{scenario.producerProfession}
                      : std::vector<uint16>{scenario.producerProfession, scenario.secondaryInputProfession});
        producer.recipeSpellIds.push_back(scenario.recipeSpellId);
        EconomyActorFacts consumer = Actor(CONSUMER_GUID, CONSUMER_ACCOUNT);
        consumer.demands.push_back({scenario.outputGroup, 1u});
        coordinator.RefreshActor(gatherer, NOW);
        coordinator.RefreshActor(producer, NOW);
        coordinator.RefreshActor(std::move(consumer), NOW);
        coordinator.RefreshActor(Actor(SPECULATOR_GUID, SPECULATOR_ACCOUNT), NOW);
        coordinator.RefreshMarket({.marketId = MARKET_ID, .supplies = {{raw, 4u, EconomySupplySource::ActiveAuction}}},
                                  NOW);

        EconomyAssignmentLease const production = economyRuntime->AssignProduction(
            coordinator,
            {.characterGuid = PRODUCER_GUID,
             .marketId = MARKET_ID,
             .recipes = {{scenario.outputGroup, scenario.recipeSpellId, scenario.outputItemId}},
             .expiresAt = NOW + 60u},
            NOW);
        ASSERT_TRUE(production.assignment.has_value());
        EconomyAssignmentLease const speculation = coordinator.Lease(
            PurchaseRequest(SPECULATOR_GUID, raw, EconomyClaimPriority::Speculation, GATHERER_ACCOUNT), NOW);
        ASSERT_TRUE(speculation.assignment.has_value());
        producer.demands.push_back({raw, 4u});
        coordinator.RefreshActor(producer, NOW + 1u);
        EconomyAssignmentLease const producerLease = coordinator.Lease(
            PurchaseRequest(PRODUCER_GUID, raw, EconomyClaimPriority::Producer, GATHERER_ACCOUNT), NOW + 1u);
        ASSERT_TRUE(producerLease.assignment.has_value());
        EXPECT_EQ(producerLease.assignment->quantity, 4u);
        EconomyAssignmentLease const retained = economyRuntime->AssignProduction(
            coordinator,
            {.characterGuid = PRODUCER_GUID,
             .marketId = MARKET_ID,
             .recipes = {{scenario.outputGroup, scenario.recipeSpellId, scenario.outputItemId}},
             .expiresAt = NOW + 90u},
            NOW + 1u);
        ASSERT_TRUE(retained.assignment.has_value());
        EXPECT_EQ(retained.assignment->leaseId, production.assignment->leaseId);
        EconomyAssignmentLease const selfPurchase = coordinator.Lease(
            PurchaseRequest(GATHERER_GUID, raw, EconomyClaimPriority::Producer, GATHERER_ACCOUNT), NOW + 1u);
        EXPECT_FALSE(selfPurchase.assignment.has_value());
        EXPECT_EQ(selfPurchase.blocker, EconomyWorkBlocker::SameAccountPurchase);

        EconomyCoordinatorSnapshot coordinatorSnapshot = coordinator.Snapshot(NOW + 1u);
        ASSERT_EQ(coordinatorSnapshot.claims.size(), 3u);
        EXPECT_EQ(coordinatorSnapshot.claims[0].state, EconomyClaimState::Leased);
        EXPECT_EQ(coordinatorSnapshot.claims[1].state, EconomyClaimState::Released);
        EXPECT_EQ(coordinatorSnapshot.claims[2].state, EconomyClaimState::Leased);
        ASSERT_TRUE(coordinator.RecordOutcome(producerLease.assignment->leaseId,
                                              EconomyAssignmentOutcome::InventoryReceived, 4u, NOW + 2u));

        ItemLedger items;
        GoldLedger gold;
        uint64 const startingGold = gold.Total();
        items.ordinarySourceInflow = 4u;
        items.sourceInventory = items.ordinarySourceInflow;

        ListAuction(items, gold, true, 4u, 10u);
        BuyAuction(items, gold, true, 4u, 100u, 10u, 5u);
        CollectAuctionMail(items, gold, true, 4u);
        ASSERT_EQ(items.producerInventory, 4u);
        EconomyProductionOutput const noOutput =
            economyRuntime->ReconcileProductionInventory(coordinator, production.assignment->leaseId, 0u, 0u, NOW + 4u);
        EXPECT_FALSE(noOutput.recorded);
        EconomyCoordinatorSnapshot const beforeCraft = coordinator.Snapshot(NOW + 4u);
        auto const productionBeforeCraft = std::find_if(beforeCraft.claims.begin(), beforeCraft.claims.end(),
                                                        [&production](EconomyAssignment const& claim)
                                                        { return claim.leaseId == production.assignment->leaseId; });
        ASSERT_NE(productionBeforeCraft, beforeCraft.claims.end());
        EXPECT_EQ(productionBeforeCraft->state, EconomyClaimState::Leased);
        items.producerInventory -= 4u;
        items.rawConsumed += 4u;
        items.producerInventory += 1u;
        items.outputProduced += 1u;
        EconomyProductionOutput const produced = economyRuntime->ReconcileProductionInventory(
            coordinator, production.assignment->leaseId, 0u, items.producerInventory, NOW + 5u);
        EXPECT_TRUE(produced.recorded);
        EXPECT_TRUE(produced.completed);
        ListAuction(items, gold, false, 1u, 20u);
        BuyAuction(items, gold, false, 1u, 500u, 20u, 25u);
        CollectAuctionMail(items, gold, false, 1u);

        ConsumptionSnapshot consumption;
        consumption.botAccountId = CONSUMER_ACCOUNT;
        consumption.needs.push_back({
            .group = scenario.outputGroup,
            .use = scenario.finalUse,
            .quantity = 1u,
            .requiredUtility = 10u,
            .buyerCeilingPerItem = 500u,
            .protectedBudget = 500u,
            .remainingUses = 1u,
            .compatibleActivity = true,
        });
        consumption.owned.push_back({scenario.outputGroup, 9'001u, scenario.outputItemId, 1u, 10u, true});
        ConsumptionDecision const finalUse = PlayerbotEconomyConsumption::Decide(consumption);
        ASSERT_EQ(finalUse.action, ConsumptionAction::FinalUse);
        ASSERT_EQ(finalUse.use, scenario.finalUse);
        items.consumerInventory -= 1u;
        items.finalUse += 1u;

        PlayerbotEconomyTrace trace;
        PlayerbotEconomyTraceRuntime runtime(trace);
        std::string const& chainId = production.assignment->chainPublicId;
        EXPECT_TRUE(runtime.Complete(true, TraceRecord("raw-purchased", chainId, EconomyTraceKind::Purchased,
                                                       scenario.inputItemId, 4u, NOW + 3u)));
        EXPECT_TRUE(runtime.Complete(true, TraceRecord("raw-delivered", chainId, EconomyTraceKind::Delivered,
                                                       scenario.inputItemId, 4u, NOW + 4u)));
        EXPECT_TRUE(runtime.Complete(
            true, TraceRecord("crafted", chainId, EconomyTraceKind::Crafted, scenario.outputItemId, 1u, NOW + 5u)));
        EXPECT_TRUE(runtime.Complete(
            true, TraceRecord("listed", chainId, EconomyTraceKind::Listed, scenario.outputItemId, 1u, NOW + 6u)));
        EXPECT_TRUE(runtime.Complete(
            true, TraceRecord("purchased", chainId, EconomyTraceKind::Purchased, scenario.outputItemId, 1u, NOW + 7u)));
        EXPECT_TRUE(runtime.Complete(
            true, TraceRecord("delivered", chainId, EconomyTraceKind::Delivered, scenario.outputItemId, 1u, NOW + 8u)));
        EconomyTraceRecord settlement =
            TraceRecord("settled", chainId, EconomyTraceKind::SaleSettled, scenario.outputItemId, 1u, NOW + 9u);
        settlement.unitPriceCopper = 500u;
        settlement.depositCopper = 20u;
        settlement.auctionCutCopper = 25u;
        settlement.proceedsCopper = 495u;
        EXPECT_TRUE(runtime.Complete(true, std::move(settlement)));
        EconomyTraceRecord used =
            TraceRecord("final-use", chainId, EconomyTraceKind::FinalUse, scenario.outputItemId, 1u, NOW + 10u);
        used.finalUse = scenario.finalUseTrace;
        EXPECT_TRUE(runtime.Complete(true, std::move(used)));

        EconomyTraceSnapshot const traceSnapshot = trace.Snapshot();
        ASSERT_EQ(traceSnapshot.events.size(), 8u);
        EXPECT_EQ(traceSnapshot.events.back().kind, EconomyTraceKind::FinalUse);
        EXPECT_EQ(traceSnapshot.events.back().finalUse, scenario.finalUseTrace);
        EXPECT_EQ(items.ordinarySourceInflow, items.rawConsumed);
        EXPECT_EQ(items.outputProduced, items.finalUse);
        EXPECT_EQ(items.sourceInventory, 0u);
        EXPECT_EQ(items.producerInventory, 0u);
        EXPECT_EQ(items.consumerInventory, 0u);
        EXPECT_EQ(items.mail, 0u);
        EXPECT_EQ(items.activeAuctions, 0u);
        EXPECT_EQ(items.positionQuantity, 0u);
        EXPECT_EQ(gold.depositEscrow, 0u);
        EXPECT_EQ(gold.proceedsMail, 0u);
        EXPECT_EQ(gold.auctionCuts, 30u);
        EXPECT_EQ(gold.Total(), startingGold);
    }
}

TEST(PlayerbotEconomyScenarioTest, GatheringThroughSaleAndExpirationReconcilesItemsAndGold)
{
    constexpr uint32 rawItemId = 2318u;
    constexpr uint32 finishedItemId = 2300u;
    ItemLedger items;
    GoldLedger gold;
    uint64 const startingGold = gold.Total();
    items.ordinarySourceInflow = 4u;
    items.sourceInventory = items.ordinarySourceInflow;

    PlayerbotEconomyTrace trace;
    PlayerbotEconomyTraceRuntime runtime(trace);
    std::string const chainId = "gather-to-expiration";
    EconomyTraceRecord gathered = TraceRecord("gathered", chainId, EconomyTraceKind::Gathered, rawItemId, 4u, NOW);
    gathered.actorGuid = GATHERER_GUID;
    EXPECT_TRUE(runtime.Complete(true, std::move(gathered)));

    ListAuction(items, gold, true, 4u, 10u);
    EXPECT_TRUE(
        runtime.Complete(true, TraceRecord("raw-listed", chainId, EconomyTraceKind::Listed, rawItemId, 4u, NOW + 1u)));
    BuyAuction(items, gold, true, 4u, 100u, 10u, 5u);
    EXPECT_TRUE(runtime.Complete(
        true, TraceRecord("raw-purchased", chainId, EconomyTraceKind::Purchased, rawItemId, 4u, NOW + 2u)));
    CollectAuctionMail(items, gold, true, 4u);
    EconomyTraceRecord rawSale =
        TraceRecord("raw-sale", chainId, EconomyTraceKind::SaleSettled, rawItemId, 4u, NOW + 3u);
    rawSale.unitPriceCopper = 25u;
    rawSale.depositCopper = 10u;
    rawSale.auctionCutCopper = 5u;
    rawSale.proceedsCopper = 105u;
    EXPECT_TRUE(runtime.Complete(true, std::move(rawSale)));

    items.producerInventory -= 4u;
    items.rawConsumed += 4u;
    items.producerInventory += 2u;
    items.outputProduced += 2u;
    EXPECT_TRUE(runtime.Complete(
        true, TraceRecord("crafted", chainId, EconomyTraceKind::Crafted, finishedItemId, 2u, NOW + 4u)));

    ListAuction(items, gold, false, 1u, 10u);
    EXPECT_TRUE(runtime.Complete(
        true, TraceRecord("output-listed", chainId, EconomyTraceKind::Listed, finishedItemId, 1u, NOW + 5u)));
    BuyAuction(items, gold, false, 1u, 500u, 10u, 25u);
    CollectAuctionMail(items, gold, false, 1u);
    EconomyTraceRecord outputSale =
        TraceRecord("output-sale", chainId, EconomyTraceKind::SaleSettled, finishedItemId, 1u, NOW + 6u);
    outputSale.unitPriceCopper = 500u;
    outputSale.depositCopper = 10u;
    outputSale.auctionCutCopper = 25u;
    outputSale.proceedsCopper = 485u;
    EXPECT_TRUE(runtime.Complete(true, std::move(outputSale)));
    items.consumerInventory -= 1u;
    items.finalUse += 1u;
    EconomyTraceRecord used =
        TraceRecord("final-use", chainId, EconomyTraceKind::FinalUse, finishedItemId, 1u, NOW + 7u);
    used.finalUse = EconomyFinalUseKind::Equipped;
    EXPECT_TRUE(runtime.Complete(true, std::move(used)));

    ListAuction(items, gold, false, 1u, 10u);
    ExpireAuction(items, gold, 1u, 10u);
    CollectExpiredAuctionMail(items, 1u);
    EconomyTraceRecord expired =
        TraceRecord("expired", chainId, EconomyTraceKind::Expired, finishedItemId, 1u, NOW + 8u);
    expired.depositCopper = 10u;
    EXPECT_TRUE(runtime.Complete(true, std::move(expired)));

    EconomyTraceSnapshot const snapshot = trace.Snapshot();
    ASSERT_EQ(snapshot.events.size(), 9u);
    EXPECT_EQ(snapshot.events.front().kind, EconomyTraceKind::Gathered);
    EXPECT_EQ(snapshot.events.back().kind, EconomyTraceKind::Expired);
    EXPECT_EQ(snapshot.events.back().unitPriceCopper, 0u);
    EXPECT_EQ(items.ordinarySourceInflow, items.rawConsumed);
    EXPECT_EQ(items.outputProduced, items.finalUse + items.producerInventory);
    EXPECT_EQ(items.sourceInventory, 0u);
    EXPECT_EQ(items.consumerInventory, 0u);
    EXPECT_EQ(items.mail, 0u);
    EXPECT_EQ(items.activeAuctions, 0u);
    EXPECT_EQ(gold.depositEscrow, 0u);
    EXPECT_EQ(gold.proceedsMail, 0u);
    EXPECT_EQ(gold.auctionCuts, 40u);
    EXPECT_EQ(gold.Total(), startingGold);
}

TEST(PlayerbotEconomyScenarioTest, IrreversibleCommitmentRemainsHonestAcrossDisableLogoutAndRestart)
{
    EconomySubstitutionGroup const raw = EconomySubstitutionGroup::ExactReagent(2770u);
    PlayerbotEconomyCoordinator coordinator;
    EconomyActorFacts consumer = Actor(CONSUMER_GUID, CONSUMER_ACCOUNT);
    consumer.demands.push_back({raw, 5u});
    coordinator.RefreshActor(consumer, NOW);
    coordinator.RefreshActor(Actor(PRODUCER_GUID, PRODUCER_ACCOUNT), NOW);
    coordinator.RefreshActor(Actor(SPECULATOR_GUID, SPECULATOR_ACCOUNT), NOW);
    coordinator.RefreshMarket({.marketId = MARKET_ID, .supplies = {{raw, 5u, EconomySupplySource::ActiveAuction}}},
                              NOW);

    EconomyAssignmentRequest committedRequest =
        PurchaseRequest(PRODUCER_GUID, raw, EconomyClaimPriority::Producer, GATHERER_ACCOUNT);
    committedRequest.quantity = 5u;
    EconomyAssignmentLease const committed = coordinator.Lease(committedRequest, NOW);
    ASSERT_TRUE(committed.assignment.has_value());
    ASSERT_TRUE(
        coordinator.RecordOutcome(committed.assignment->leaseId, EconomyAssignmentOutcome::Committed, 2u, NOW + 1u));
    coordinator.InvalidateActor(PRODUCER_GUID, EconomyAssignmentOutcome::LoggedOut, NOW + 2u);
    coordinator.RefreshMarket({.marketId = MARKET_ID, .supplies = {{raw, 3u, EconomySupplySource::ActiveAuction}}},
                              NOW + 2u);

    EconomyAssignmentRequest replacementRequest = committedRequest;
    replacementRequest.characterGuid = SPECULATOR_GUID;
    replacementRequest.quantity = 5u;
    EconomyAssignmentLease const replacement = coordinator.Lease(replacementRequest, NOW + 3u);
    ASSERT_TRUE(replacement.assignment.has_value());
    EXPECT_EQ(replacement.assignment->quantity, 3u);

    EconomyCoordinatorSnapshot const disabled = coordinator.Snapshot(NOW + 3u);
    ASSERT_EQ(disabled.claims.size(), 2u);
    EXPECT_EQ(disabled.claims[0].committedQuantity, 2u);
    EXPECT_EQ(disabled.claims[0].state, EconomyClaimState::Released);
    EXPECT_EQ(disabled.claims[1].quantity, 3u);

    EconomyPosition position{
        .publicId = "restart-backed-position",
        .traderGuid = SPECULATOR_GUID,
        .marketId = MARKET_ID,
        .itemId = 2770u,
        .substitutionGroup = "exact_reagent:2770",
        .initialQuantity = 2u,
        .remainingQuantity = 2u,
        .acquisitionCost = 200u,
        .state = EconomyPositionState::Open,
        .relistAttempts = 0u,
        .maximumRelistAttempts = 2u,
        .cooldownSeconds = 300u,
        .openedAt = NOW,
        .holdingDeadline = NOW + 3'600u,
        .updatedAt = NOW,
    };
    EconomyMarketStartup startup;
    startup.positions.push_back(position);
    startup.backing.push_back({position.publicId, 2u});
    startup.circulation.push_back({
        .positionPublicId = position.publicId,
        .itemGuid = 9'001u,
        .quantity = 2u,
        .provenance = EconomyCirculationProvenance::Speculative,
        .state = EconomyCirculationState::Acquired,
        .occurredAt = NOW,
    });
    PlayerbotEconomyMarket restarted;
    EXPECT_TRUE(restarted.Restore(std::move(startup), NOW + 1u).empty());
    EconomyMarketSnapshot const restored = restarted.Snapshot(NOW + 1u);
    ASSERT_EQ(restored.positions.size(), 1u);
    EXPECT_EQ(restored.positions.front(), position);
    ASSERT_EQ(restored.circulation.size(), 1u);
    EXPECT_EQ(restored.circulation.front().quantity, 2u);
}

TEST(PlayerbotEconomyScenarioTest, SharedMarketSignalsRequireConsumerResiduals)
{
    PlayerbotEconomyCoordinator coordinator;

    ConsumptionSnapshot explicitNeed;
    explicitNeed.needs.push_back(
        PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Food, 100u, 3u, true, 500u}));
    explicitNeed.held.push_back({EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 100u), 4540u, 1u,
                                 EconomySupplySource::Inventory, 100u});

    EconomyActorFacts actor;
    actor.characterGuid = 1u;
    actor.accountId = 1u;
    actor.marketId = MARKET_ID;
    actor.online = true;
    actor.autonomous = true;
    actor.demands = PlayerbotEconomyConsumption::DemandFacts(explicitNeed);
    actor.supplies = PlayerbotEconomyConsumption::SupplyFacts(explicitNeed);
    coordinator.RefreshActor(actor, NOW);

    EconomyCoordinatorSnapshot snapshot = coordinator.Snapshot(NOW);
    ASSERT_EQ(snapshot.gaps.size(), 1u);
    EXPECT_EQ(snapshot.gaps.front().demandQuantity, 3u);
    EXPECT_EQ(snapshot.gaps.front().supplyQuantity, 1u);
    EXPECT_EQ(snapshot.gaps.front().remainingQuantity, 2u);

    ConsumptionSnapshot discoveryOnly;
    discoveryOnly.held.push_back({EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 100u), 4540u, 20u,
                                  EconomySupplySource::Inventory, 100u});
    discoveryOnly.offers.push_back(
        {EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 100u), 9u, 2u, 4540u, 20u, 100u, 100u, true});
    actor.demands = PlayerbotEconomyConsumption::DemandFacts(discoveryOnly);
    actor.supplies = PlayerbotEconomyConsumption::SupplyFacts(discoveryOnly);
    coordinator.RefreshActor(actor, NOW + 1u);
    EXPECT_TRUE(coordinator.Snapshot(NOW + 1u).gaps.empty());
}

TEST(PlayerbotEconomyScenarioTest, ZeroAffinityProfessionlessActorCanClaimARequiredConsumptionPurchase)
{
    ConsumptionSnapshot consumption;
    ConsumptionNeed need = PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Food, 100u, 4u, true, 500u});
    need.buyerCeilingPerItem = 100u;
    consumption.botAccountId = CONSUMER_ACCOUNT;
    consumption.needs.push_back(need);
    consumption.offers.push_back({need.group, 50u, GATHERER_ACCOUNT, 4'540u, 4u, 100u, 100u, true});

    ConsumptionDecision const decision = PlayerbotEconomyConsumption::Decide(consumption);
    ASSERT_EQ(decision.action, ConsumptionAction::Purchase);

    EconomyActorFacts consumer;
    consumer.characterGuid = CONSUMER_GUID;
    consumer.accountId = CONSUMER_ACCOUNT;
    consumer.marketId = MARKET_ID;
    consumer.online = true;
    consumer.autonomous = true;
    consumer.demands = PlayerbotEconomyConsumption::DemandFacts(consumption);

    PlayerbotEconomyCoordinator coordinator;
    coordinator.RefreshActor(std::move(consumer), NOW);
    coordinator.RefreshMarket(
        {.marketId = MARKET_ID,
         .supplies = {{need.group, decision.count, EconomySupplySource::ActiveAuction, decision.itemId}}},
        NOW);

    EconomyAssignmentLease const lease = coordinator.Lease(
        PurchaseRequest(CONSUMER_GUID, need.group, EconomyClaimPriority::Consumer, GATHERER_ACCOUNT), NOW);
    ASSERT_TRUE(lease.assignment.has_value());
    EXPECT_EQ(lease.assignment->quantity, 4u);
    EXPECT_EQ(lease.assignment->priority, EconomyClaimPriority::Consumer);
}

TEST(PlayerbotEconomyScenarioTest, SharedMarketSignalOracleCoversDiscoveryVendorAndTrainingBoundaries)
{
    enum class ScenarioKind
    {
        ExplicitNeed,
        InventoryOnly,
        AuctionOnly,
        ApplicableVendor,
        TrainingOutput
    };
    struct Scenario
    {
        ScenarioKind kind;
        uint32 expectedDemand;
        uint32 expectedSupply;
        uint32 expectedGap;
        EconomyPhase expectedSalePhase;
    };
    std::array const scenarios{
        Scenario{ScenarioKind::ExplicitNeed, 3u, 1u, 2u, EconomyPhase::SellSurplus},
        Scenario{ScenarioKind::InventoryOnly, 0u, 0u, 0u, EconomyPhase::SellSurplus},
        Scenario{ScenarioKind::AuctionOnly, 0u, 0u, 0u, EconomyPhase::SellSurplus},
        Scenario{ScenarioKind::ApplicableVendor, 0u, 0u, 0u, EconomyPhase::None},
        Scenario{ScenarioKind::TrainingOutput, 0u, 0u, 0u, EconomyPhase::None},
    };

    for (Scenario const& scenario : scenarios)
    {
        EconomySubstitutionGroup const food = EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 100u);
        ConsumptionSnapshot consumption;
        if (scenario.kind == ScenarioKind::ExplicitNeed || scenario.kind == ScenarioKind::ApplicableVendor)
        {
            consumption.needs.push_back(PlayerbotEconomyConsumption::BuildNeed(
                {ConsumableCapability::Food, 100u, 3u, true, 500u, scenario.kind == ScenarioKind::ApplicableVendor}));
        }
        if (scenario.kind == ScenarioKind::ExplicitNeed || scenario.kind == ScenarioKind::InventoryOnly)
            consumption.held.push_back({food, 4540u, 1u, EconomySupplySource::Inventory, 100u});
        if (scenario.kind == ScenarioKind::AuctionOnly)
            consumption.offers.push_back({food, 9u, 2u, 4540u, 1u, 100u, 100u, true});

        PlayerbotEconomyCoordinator coordinator;
        EconomyActorFacts actor;
        actor.characterGuid = 1u;
        actor.accountId = 1u;
        actor.marketId = MARKET_ID;
        actor.online = true;
        actor.autonomous = true;
        actor.demands = PlayerbotEconomyConsumption::DemandFacts(consumption);
        actor.supplies = PlayerbotEconomyConsumption::SupplyFacts(consumption);
        coordinator.RefreshActor(actor, NOW);
        EconomyCoordinatorSnapshot const coordinatorSnapshot = coordinator.Snapshot(NOW);

        if (scenario.expectedGap)
        {
            ASSERT_EQ(coordinatorSnapshot.gaps.size(), 1u);
            EXPECT_EQ(coordinatorSnapshot.gaps.front().demandQuantity, scenario.expectedDemand);
            EXPECT_EQ(coordinatorSnapshot.gaps.front().supplyQuantity, scenario.expectedSupply);
            EXPECT_EQ(coordinatorSnapshot.gaps.front().remainingQuantity, scenario.expectedGap);
        }
        else
            EXPECT_TRUE(coordinatorSnapshot.gaps.empty());

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
        sale.ordinaryVendorSupply = scenario.kind == ScenarioKind::ApplicableVendor;
        sale.trainingOutput = scenario.kind == ScenarioKind::TrainingOutput;

        EconomySnapshot economy;
        economy.guidCounter = 42u;
        economy.saleItems.push_back(sale);
        EconomyDecision const decision = PlayerbotEconomyPolicy::Decide(economy);
        EXPECT_EQ(decision.phase, scenario.expectedSalePhase);
        EXPECT_EQ(decision.itemGuidCounter,
                  scenario.expectedSalePhase == EconomyPhase::SellSurplus ? sale.itemGuidCounter : 0u);
    }

    ConsumptionSnapshot satisfiedNeed;
    satisfiedNeed.needs.push_back(
        PlayerbotEconomyConsumption::BuildNeed({ConsumableCapability::Food, 100u, 1u, true, 500u}));
    satisfiedNeed.held.push_back({EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 100u), 4540u, 1u,
                                  EconomySupplySource::Inventory, 100u});

    PlayerbotEconomyCoordinator satisfiedCoordinator;
    EconomyActorFacts satisfiedActor;
    satisfiedActor.characterGuid = 1u;
    satisfiedActor.accountId = 1u;
    satisfiedActor.marketId = MARKET_ID;
    satisfiedActor.online = true;
    satisfiedActor.autonomous = true;
    satisfiedActor.demands = PlayerbotEconomyConsumption::DemandFacts(satisfiedNeed);
    satisfiedActor.supplies = PlayerbotEconomyConsumption::SupplyFacts(satisfiedNeed);
    satisfiedCoordinator.RefreshActor(satisfiedActor, NOW);

    EconomyCoordinatorSnapshot const satisfiedSnapshot = satisfiedCoordinator.Snapshot(NOW);
    ASSERT_EQ(satisfiedSnapshot.gaps.size(), 1u);
    EXPECT_EQ(satisfiedSnapshot.gaps.front().demandQuantity, 1u);
    EXPECT_EQ(satisfiedSnapshot.gaps.front().supplyQuantity, 1u);
    EXPECT_EQ(satisfiedSnapshot.gaps.front().remainingQuantity, 0u);
    EXPECT_FALSE(satisfiedSnapshot.gaps.front().HasResidualDemand());

    SaleItemCandidate satisfiedTrainingOutput;
    satisfiedTrainingOutput.itemGuidCounter = 20u;
    satisfiedTrainingOutput.itemId = 4540u;
    satisfiedTrainingOutput.count = 1u;
    satisfiedTrainingOutput.usage = ITEM_USAGE_AH;
    satisfiedTrainingOutput.canBeTraded = true;
    satisfiedTrainingOutput.templateBuyPrice = 10u;
    satisfiedTrainingOutput.templateSellPrice = 1u;
    satisfiedTrainingOutput.inventoryCount = 1u;
    satisfiedTrainingOutput.professionRelated = true;
    satisfiedTrainingOutput.buyerCeilingPerItem = 10u;
    satisfiedTrainingOutput.trainingOutput = true;
    satisfiedTrainingOutput.independentDemand = satisfiedSnapshot.gaps.front().HasResidualDemand();

    EconomySnapshot satisfiedEconomy;
    satisfiedEconomy.saleItems.push_back(satisfiedTrainingOutput);
    EXPECT_FALSE(satisfiedTrainingOutput.independentDemand);
    EXPECT_EQ(PlayerbotEconomyPolicy::Decide(satisfiedEconomy).phase, EconomyPhase::None);
}

TEST(PlayerbotEconomyScenarioTest, RecurringStockReconciliationPreservesPublicDemandAndCommittedSupplyFacts)
{
    RecurringStockReconciliation const reconciliation = PlayerbotEconomyConsumption::ReconcileRecurringStock({
        .expectedUses = 8u,
        .safetyReserve = 2u,
        .carryingBudget = 9u,
        .adequateCurrentAndPendingSupply = 2u,
        .usesBeforeDevelopmentalDelivery = 5u,
        .credibleDevelopmentalDeliveryQuantity = 3u,
        .developmentalPathViable = true,
    });

    EconomySubstitutionGroup const food = EconomySubstitutionGroup::Consumable(ConsumableCapability::Food, 100u);
    ConsumptionSnapshot consumption;
    consumption.needs.push_back(PlayerbotEconomyConsumption::BuildNeed(
        {ConsumableCapability::Food, 100u, reconciliation.desiredStock, true, 500u}));
    consumption.held.push_back({food, 4540u, 2u, EconomySupplySource::CommittedProduction, 100u});

    EconomyActorFacts actor = Actor(CONSUMER_GUID, CONSUMER_ACCOUNT);
    actor.demands = PlayerbotEconomyConsumption::DemandFacts(consumption);
    actor.supplies = PlayerbotEconomyConsumption::SupplyFacts(consumption);
    ASSERT_EQ(actor.demands.size(), 1u);
    ASSERT_EQ(actor.supplies.size(), 1u);
    EXPECT_EQ(actor.supplies.front().source, EconomySupplySource::CommittedProduction);
    EXPECT_EQ(actor.supplies.front().quantity, 2u);

    PlayerbotEconomyCoordinator coordinator;
    coordinator.RefreshActor(actor, NOW);
    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(NOW);

    ASSERT_EQ(snapshot.gaps.size(), 1u);
    EXPECT_EQ(snapshot.gaps.front().demandQuantity, reconciliation.desiredStock);
    EXPECT_EQ(snapshot.gaps.front().supplyQuantity, actor.supplies.front().quantity);
    EXPECT_EQ(snapshot.gaps.front().remainingQuantity, reconciliation.bridgeQuantity +
                                                           reconciliation.developmentalReservationQuantity +
                                                           reconciliation.residualUncoveredQuantity);
}

TEST(PlayerbotEconomyScenarioTest, AggregateGatheringBacklogBecomesIndependentBoundedSlices)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(2770u);

    EconomyActorFacts gathererOne = Actor(GATHERER_GUID, GATHERER_ACCOUNT, {186u});
    EconomyActorFacts gathererTwo = Actor(PRODUCER_GUID, PRODUCER_ACCOUNT, {186u});
    EconomyActorFacts consumer = Actor(CONSUMER_GUID, CONSUMER_ACCOUNT, {164u});
    consumer.demands.push_back({copper, 398u});
    coordinator.RefreshActor(gathererOne, NOW);
    coordinator.RefreshActor(gathererTwo, NOW);
    coordinator.RefreshActor(consumer, NOW);

    EconomyCoordinatorSnapshot snapshot = coordinator.Snapshot(NOW);
    ASSERT_EQ(snapshot.gaps.size(), 1u);
    EXPECT_EQ(snapshot.gaps.front().remainingQuantity, 398u);

    std::array<DedicatedGatheringCandidate, 2> const candidates = {
        DedicatedGatheringCandidate{.characterGuid = GATHERER_GUID, .capacity = 7u},
        DedicatedGatheringCandidate{.characterGuid = PRODUCER_GUID, .capacity = 5u},
    };
    DedicatedGatheringPlanRequest const request{
        .tripIdentity = "trip-copper-shared",
        .observedAt = NOW,
        .batchQuantity = snapshot.gaps.front().remainingQuantity,
        .origins =
            {
                {.originIdentity = "consumer-copper-demand",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = snapshot.gaps.front().remainingQuantity,
                 .expiresAt = NOW + 300u},
                {.originIdentity = "future-copper-training",
                 .state = DedicatedGatheringOriginState::Latent,
                 .quantity = 20u,
                 .expiresAt = NOW + 600u},
            },
    };
    std::optional<DedicatedGatheringProvenancePlan> const planned =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(planned.has_value());
    DedicatedGatheringProvenancePlan const& plan = *planned;
    ASSERT_EQ(plan.workOrders.size(), 2u);
    EXPECT_EQ(plan.assignedQuantity, 12u);
    EXPECT_EQ(plan.unassignedBatchQuantity, 386u);
    EXPECT_EQ(plan.deferredActiveQuantity, 0u);
    EXPECT_EQ(plan.latentQuantity, 20u);

    std::optional<DedicatedGatheringTripProvenance> const firstProvenance =
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(plan, plan.workOrders[0]);
    std::optional<DedicatedGatheringTripProvenance> const secondProvenance =
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(plan, plan.workOrders[1]);
    ASSERT_TRUE(firstProvenance.has_value());
    ASSERT_TRUE(secondProvenance.has_value());
    EXPECT_EQ(firstProvenance->tripIdentity, secondProvenance->tripIdentity);
    ASSERT_EQ(firstProvenance->origins.size(), 2u);
    ASSERT_EQ(secondProvenance->origins.size(), 2u);
    EXPECT_EQ(firstProvenance->origins[0].allocatedQuantity, 7u);
    EXPECT_EQ(secondProvenance->origins[0].allocatedQuantity, 5u);
    EXPECT_EQ(firstProvenance->origins[1].allocatedQuantity, 0u);
    EXPECT_EQ(secondProvenance->origins[1].allocatedQuantity, 0u);

    auto leaseSlice = [&coordinator, &copper](DedicatedGatheringWorkOrder const& order)
    {
        EconomyAssignmentRequest request;
        request.characterGuid = order.characterGuid;
        request.marketId = MARKET_ID;
        request.group = copper;
        request.quantity = order.quantity;
        request.kind = EconomyClaimKind::Resource;
        request.priority = EconomyClaimPriority::Producer;
        request.workKind = EconomyWorkKind::Gather;
        request.workIdentity = "gather:2770:186";
        request.expiresAt = NOW + 300u;
        return coordinator.Lease(std::move(request), NOW);
    };

    EconomyAssignmentLease const first = leaseSlice(plan.workOrders[0]);
    EconomyAssignmentLease const second = leaseSlice(plan.workOrders[1]);
    ASSERT_TRUE(first.assignment);
    ASSERT_TRUE(second.assignment);
    EXPECT_EQ(first.assignment->quantity, 7u);
    EXPECT_EQ(second.assignment->quantity, 5u);

    snapshot = coordinator.Snapshot(NOW);
    ASSERT_EQ(snapshot.gaps.size(), 1u);
    EXPECT_EQ(snapshot.gaps.front().claimedQuantity, 12u);
    EXPECT_EQ(snapshot.gaps.front().remainingQuantity, 386u);

    ASSERT_TRUE(coordinator.RecordOutcome(first.assignment->leaseId, EconomyAssignmentOutcome::InventoryReceived, 4u,
                                          NOW + 1u));
    gathererOne.supplies.push_back({copper, 4u, EconomySupplySource::Inventory, 2770u});
    coordinator.RefreshActor(gathererOne, NOW + 1u);

    snapshot = coordinator.Snapshot(NOW + 1u);
    ASSERT_EQ(snapshot.gaps.size(), 1u);
    EXPECT_EQ(snapshot.gaps.front().supplyQuantity, 4u);
    EXPECT_EQ(snapshot.gaps.front().claimedQuantity, 5u);
    EXPECT_EQ(snapshot.gaps.front().remainingQuantity, 389u);
    auto const retainedSecond =
        std::find_if(snapshot.claims.begin(), snapshot.claims.end(),
                     [&second](EconomyAssignment const& claim) { return claim.leaseId == second.assignment->leaseId; });
    ASSERT_NE(retainedSecond, snapshot.claims.end());
    EXPECT_EQ(retainedSecond->state, EconomyClaimState::Leased);
    EXPECT_EQ(retainedSecond->quantity, 5u);
}

TEST(PlayerbotEconomyScenarioTest, AcceptedExternalGatheringSliceOutranksLaterSelfProgressionUntilAuthoritativeRelease)
{
    EconomySubstitutionGroup const copperOre = EconomySubstitutionGroup::ExactReagent(2770u);
    PlayerbotEconomyCoordinator coordinator;
    EconomyActorFacts blacksmith = Actor(PRODUCER_GUID, PRODUCER_ACCOUNT, {164u});
    blacksmith.demands.push_back({copperOre, 5u});
    coordinator.RefreshActor(blacksmith, NOW);
    coordinator.RefreshActor(Actor(GATHERER_GUID, GATHERER_ACCOUNT, {186u}), NOW);

    EconomyAssignmentRequest request;
    request.characterGuid = GATHERER_GUID;
    request.marketId = MARKET_ID;
    request.group = copperOre;
    request.quantity = 5u;
    request.kind = EconomyClaimKind::Resource;
    request.priority = EconomyClaimPriority::Producer;
    request.workKind = EconomyWorkKind::Gather;
    request.workIdentity = "gather:2770:186";
    request.expiresAt = NOW + 300u;
    EconomyAssignmentLease const lease = coordinator.Lease(std::move(request), NOW);
    ASSERT_TRUE(lease.assignment);
    ASSERT_TRUE(
        coordinator.RecordOutcome(lease.assignment->leaseId, EconomyAssignmentOutcome::Committed, 5u, NOW + 1u));

    PlayerbotCareer::ProfessionProgressionMilestone const smeltCopper = {
        .professionSkillId = 164u,
        .targetSkill = 75u,
        .recipeSpellId = 2657u,
        .outputItemId = 2840u,
    };
    auto admission = [&smeltCopper](uint32 ownedCopperOre)
    {
        PlayerbotCareer::ProfessionProgressionRecipe const recipe = {
            .professionSkillId = 164u,
            .spellId = 2657u,
            .outputItemId = 2840u,
            .known = true,
            .advancesSkill = true,
            .reagents = {{.itemId = 2770u, .count = 1u, .ownedCount = ownedCopperOre}},
        };
        return PlayerbotCareer::AdmitProgressionBatch(smeltCopper, recipe, 5u);
    };
    auto retained = [&coordinator, &lease](uint64 now)
    {
        EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(now);
        auto const claim = std::find_if(snapshot.claims.begin(), snapshot.claims.end(), [&lease](auto const& candidate)
                                        { return candidate.leaseId == lease.assignment->leaseId; });
        return claim != snapshot.claims.end() && claim->state == EconomyClaimState::Leased;
    };

    AcceptedExternalGatheringSlice const exactSlice = PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice({
        .currentInventoryQuantity = 5u,
        .preTripInventoryQuantity = 0u,
        .acceptedQuantity = lease.assignment->quantity,
        .retained = retained(NOW + 1u),
    });
    EXPECT_EQ(exactSlice.protectedQuantity, 5u);
    EXPECT_EQ(exactSlice.progressionAvailableQuantity, 0u);
    PlayerbotCareer::ProfessionProgressionAdmission const blocked = admission(exactSlice.progressionAvailableQuantity);
    EXPECT_EQ(blocked.state, PlayerbotCareer::ProfessionProgressionAdmissionState::Waiting);
    EXPECT_EQ(blocked.blocker, PlayerbotCareer::ProfessionProgressionBlocker::MaterialSourceUnavailable);
    EXPECT_EQ(blocked.missingItemId, 2770u);

    AcceptedExternalGatheringSlice const excess = PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice({
        .currentInventoryQuantity = 7u,
        .preTripInventoryQuantity = 0u,
        .acceptedQuantity = lease.assignment->quantity,
        .retained = retained(NOW + 1u),
    });
    EXPECT_EQ(excess.protectedQuantity, 5u);
    EXPECT_EQ(excess.progressionAvailableQuantity, 2u);
    PlayerbotCareer::ProfessionProgressionAdmission const bounded = admission(excess.progressionAvailableQuantity);
    EXPECT_EQ(bounded.state, PlayerbotCareer::ProfessionProgressionAdmissionState::Ready);
    EXPECT_EQ(bounded.batchQuantity, 2u);

    ASSERT_TRUE(
        coordinator.RecordOutcome(lease.assignment->leaseId, EconomyAssignmentOutcome::NeedChanged, 5u, NOW + 2u));
    AcceptedExternalGatheringSlice const released = PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice({
        .currentInventoryQuantity = 7u,
        .preTripInventoryQuantity = 0u,
        .acceptedQuantity = lease.assignment->quantity,
        .retained = retained(NOW + 2u),
    });
    EXPECT_EQ(released.protectedQuantity, 0u);
    EXPECT_EQ(released.progressionAvailableQuantity, 7u);
    PlayerbotCareer::ProfessionProgressionAdmission const unblocked = admission(released.progressionAvailableQuantity);
    EXPECT_EQ(unblocked.state, PlayerbotCareer::ProfessionProgressionAdmissionState::Ready);
    EXPECT_EQ(unblocked.batchQuantity, 5u);
}

TEST(PlayerbotEconomyScenarioTest, StalledCareerStageReleasesTheCycleWhileWorkingStagesKeepIt)
{
    auto const stage = [](PlayerbotEconomyCycleOutcome outcome, char const* blocker)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = outcome;
        result.blocker = blocker;
        return result;
    };

    // A career stage that cannot progress must not consume the cycle, or the bot never reaches the
    // market stage to list and buy. Both blockers below are durable on a live realm.
    EXPECT_FALSE(
        CareerStageOwnsCycle(stage(PlayerbotEconomyCycleOutcome::FailedPrecondition, "primary_slots_occupied")));
    EXPECT_FALSE(
        CareerStageOwnsCycle(stage(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_intent_latent")));

    // A stage that is actively travelling or training still owns the cycle.
    EXPECT_TRUE(CareerStageOwnsCycle(stage(PlayerbotEconomyCycleOutcome::Scheduled, "trainer_travel")));
    EXPECT_TRUE(CareerStageOwnsCycle(stage(PlayerbotEconomyCycleOutcome::Operation, "base_career_profession_learned")));
}

TEST(PlayerbotEconomyScenarioTest, ReschedulingATrainerVisitDoesNotMaskTheTrainerStageThatJustFailed)
{
    auto const stage = [](PlayerbotEconomyCycleOutcome outcome, char const* blocker)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = outcome;
        result.blocker = blocker;
        return result;
    };

    PlayerbotEconomyCycleResult const rank =
        stage(PlayerbotEconomyCycleOutcome::Scheduled, "profession_trainer_rank_selected");
    PlayerbotEconomyCycleResult const recipe =
        stage(PlayerbotEconomyCycleOutcome::Scheduled, "profession_trainer_recipe_selected");

    // Nothing failed yet, so recording that a trainer visit is wanted is real progress.
    EXPECT_TRUE(ProgressionStageOwnsCycle(rank, false));
    EXPECT_TRUE(ProgressionStageOwnsCycle(recipe, false));

    // The trainer stage already tried and could not act on that very objective this tick. Recording the
    // same intent again reports success, hides the real blocker, and loops the bot forever.
    EXPECT_FALSE(ProgressionStageOwnsCycle(rank, true));
    EXPECT_FALSE(ProgressionStageOwnsCycle(recipe, true));

    // Progression work the trainer stage has no part in still owns the cycle.
    EXPECT_TRUE(
        ProgressionStageOwnsCycle(stage(PlayerbotEconomyCycleOutcome::Operation, "profession_batch_crafted"), true));

    // A progression stage that stalled never owned the cycle to begin with.
    EXPECT_FALSE(ProgressionStageOwnsCycle(
        stage(PlayerbotEconomyCycleOutcome::FailedPrecondition, "profession_material_intent_latent"), false));
}

TEST(PlayerbotEconomyScenarioTest, LosingAListingToAnotherBuyerReleasesTheCycleInsteadOfFailing)
{
    // Another buyer took the listing while this bot walked to the auctioneer, or the purchase could
    // not be completed at all. Either way the bot keeps its cycle for production and selling: being
    // unable to buy is no reason to stop earning.
    EXPECT_FALSE(ConsumptionStepOwnsCycle(EconomyExecutionResult::Superseded));
    EXPECT_FALSE(ConsumptionStepOwnsCycle(EconomyExecutionResult::Failed));

    // Buying, using, and travelling are real work and still decide the cycle.
    EXPECT_TRUE(ConsumptionStepOwnsCycle(EconomyExecutionResult::Scheduled));
    EXPECT_TRUE(ConsumptionStepOwnsCycle(EconomyExecutionResult::Operation));
    EXPECT_TRUE(ConsumptionStepOwnsCycle(EconomyExecutionResult::Recovery));
}
