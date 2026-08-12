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
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "Bot/Economy/PlayerbotEconomyRuntime.h"
#include "Bot/Economy/PlayerbotEconomyTrace.h"
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
