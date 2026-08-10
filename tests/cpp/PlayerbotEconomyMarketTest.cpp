/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <vector>

#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr uint64 NOW = 1'000u;

EconomyPriceEvidence Evidence(EconomyEvidenceSource source, uint64 unitPrice, uint32 quantity = 1u)
{
    return {
        .marketId = 2u,
        .itemId = 100u,
        .substitutionGroup = "reagent:100",
        .source = source,
        .unitPrice = unitPrice,
        .quantity = quantity,
        .observedAt = NOW - 10u,
        .expiresAt = NOW + 100u,
    };
}

EconomyPosition MakePosition(std::string publicId, uint32 remainingQuantity, uint64 acquisitionCost)
{
    return {
        .publicId = std::move(publicId),
        .traderGuid = 10u,
        .marketId = 2u,
        .itemId = 100u,
        .substitutionGroup = "reagent:100",
        .initialQuantity = 5u,
        .remainingQuantity = remainingQuantity,
        .acquisitionCost = acquisitionCost,
        .state = EconomyPositionState::Open,
        .relistAttempts = 2u,
        .maximumRelistAttempts = 2u,
        .cooldownSeconds = 300u,
        .openedAt = NOW - 200u,
        .holdingDeadline = NOW + 1'000u,
        .updatedAt = NOW - 20u,
    };
}

EconomyRiskConfiguration RiskConfiguration()
{
    return {
        .enabled = true,
        .perGroupExposurePercent = 10u,
        .totalExposurePercent = 25u,
        .minimumEvidence = 3u,
        .holdingHorizonSeconds = 3'600u,
        .maximumRelistAttempts = 2u,
        .cooldownSeconds = 300u,
    };
}
}  // namespace

TEST(PlayerbotEconomyMarketTest, RestartPreservesBackedLotsAndClosesOnlyMissingQuantity)
{
    std::vector<EconomyMarketWrite> writes;
    PlayerbotEconomyMarket market([&writes](uint64, EconomyMarketWrite const& write) { writes.push_back(write); });
    EconomyMarketStartup startup;
    startup.positions.push_back(MakePosition("fully-backed", 5u, 500u));
    startup.positions.push_back(MakePosition("partly-backed", 5u, 500u));
    startup.positions.push_back(MakePosition("missing", 5u, 500u));
    startup.backing.push_back({"fully-backed", 5u});
    startup.backing.push_back({"partly-backed", 3u});
    startup.cooldowns.push_back({10u, 2u, "reagent:100", EconomyCooldownCause::Loss, NOW + 50u});
    startup.circulation.push_back({
        .positionPublicId = "fully-backed",
        .itemGuid = 1001u,
        .quantity = 5u,
        .provenance = EconomyCirculationProvenance::Speculative,
        .state = EconomyCirculationState::Listed,
        .occurredAt = NOW - 30u,
    });
    startup.circulation.push_back({
        .positionPublicId = "partly-backed",
        .itemGuid = 1002u,
        .quantity = 3u,
        .provenance = EconomyCirculationProvenance::Recovery,
        .state = EconomyCirculationState::Acquired,
        .occurredAt = NOW - 20u,
    });

    std::vector<EconomyPositionReconciliation> const changes = market.Restore(std::move(startup), NOW);
    EconomyMarketSnapshot const snapshot = market.Snapshot(NOW);

    ASSERT_EQ(snapshot.positions.size(), 2u);
    EXPECT_EQ(snapshot.positions[0], MakePosition("fully-backed", 5u, 500u));
    EconomyPosition partlyBacked = MakePosition("partly-backed", 3u, 300u);
    partlyBacked.realizedCost = 200u;
    EXPECT_EQ(snapshot.positions[1], partlyBacked);
    ASSERT_EQ(snapshot.cooldowns.size(), 1u);
    EXPECT_EQ(snapshot.cooldowns.front().nextEligibleAt, NOW + 50u);
    ASSERT_EQ(snapshot.circulation.size(), 2u);
    EXPECT_EQ(snapshot.circulation[0].provenance, EconomyCirculationProvenance::Speculative);
    EXPECT_EQ(snapshot.circulation[1].provenance, EconomyCirculationProvenance::Recovery);

    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].publicId, "partly-backed");
    EXPECT_EQ(changes[0].remainingQuantity, 3u);
    EXPECT_EQ(changes[0].acquisitionCost, 300u);
    EXPECT_EQ(changes[1].publicId, "missing");
    EXPECT_EQ(changes[1].state, EconomyPositionState::Lost);
    ASSERT_EQ(writes.size(), 2u);
    EXPECT_EQ(writes[0].kind, EconomyMarketWriteKind::UpdatePosition);
    EXPECT_EQ(writes[0].position.remainingQuantity, 3u);
    EXPECT_EQ(writes[0].position.realizedCost, 200u);
    EXPECT_EQ(writes[1].kind, EconomyMarketWriteKind::ClosePosition);
    EXPECT_EQ(writes[1].position.acquisitionCost, 0u);
    EXPECT_EQ(writes[1].position.realizedCost, 500u);
    EXPECT_EQ(writes[1].position.realizedOutcome, EconomyPositionOutcome::Loss);

    EconomyMarketStartup restarted;
    restarted.positions = snapshot.positions;
    restarted.cooldowns = snapshot.cooldowns;
    restarted.circulation = snapshot.circulation;
    restarted.backing.push_back({"fully-backed", 5u});
    restarted.backing.push_back({"partly-backed", 3u});
    writes.clear();
    EXPECT_TRUE(market.Restore(std::move(restarted), NOW).empty());
    EXPECT_TRUE(writes.empty());
}

TEST(PlayerbotEconomyMarketTest, ReferencePricePrefersSalesAndRejectsStaleOrExtremeListings)
{
    PlayerbotEconomyMarket market;
    EconomyMarketStartup startup;
    startup.evidence = {
        Evidence(EconomyEvidenceSource::Sale, 100u, 2u),
        Evidence(EconomyEvidenceSource::Sale, 110u, 2u),
        Evidence(EconomyEvidenceSource::Listing, 80u, 20u),
        Evidence(EconomyEvidenceSource::Listing, 10'000u, 20u),
        Evidence(EconomyEvidenceSource::Recovery, 1'000u, 100u),
        Evidence(EconomyEvidenceSource::Speculation, 2'000u, 100u),
    };
    EconomyPriceEvidence stale = Evidence(EconomyEvidenceSource::Sale, 1u, 100u);
    stale.expiresAt = NOW;
    startup.evidence.push_back(stale);
    static_cast<void>(market.Restore(std::move(startup), NOW));

    std::optional<EconomyReferencePrice> const reference = market.ReferencePrice(2u, "reagent:100", NOW);

    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(reference->unitPrice, 105u);
    EXPECT_EQ(reference->acceptedSales, 2u);
    EXPECT_EQ(reference->acceptedListings, 1u);
    EXPECT_TRUE(reference->confident);
}

TEST(PlayerbotEconomyMarketTest, EvidenceHistoryIsBoundedAndExpiresInMemory)
{
    PlayerbotEconomyMarket market;
    EconomyMarketStartup startup;
    for (uint32 i = 0; i < PlayerbotEconomyMarket::MAX_EVIDENCE_PER_GROUP + 5u; ++i)
    {
        EconomyPriceEvidence evidence = Evidence(EconomyEvidenceSource::Sale, 100u + i);
        evidence.observedAt += i;
        startup.evidence.push_back(evidence);
    }

    static_cast<void>(market.Restore(std::move(startup), NOW));
    EXPECT_EQ(market.Snapshot(NOW).evidence.size(), PlayerbotEconomyMarket::MAX_EVIDENCE_PER_GROUP);
    EXPECT_TRUE(market.Snapshot(NOW + 101u).evidence.empty());
}

TEST(PlayerbotEconomyMarketTest, RepeatedAuctionEvidenceDoesNotInflateConfidence)
{
    std::vector<EconomyMarketWrite> writes;
    PlayerbotEconomyMarket market([&writes](uint64, EconomyMarketWrite const& write) { writes.push_back(write); });
    EconomyPriceEvidence sale = Evidence(EconomyEvidenceSource::Sale, 100u, 5u);
    sale.auctionId = 77u;
    ASSERT_NE(market.AppendEvidence(sale, 0u, true), 0u);

    sale.observedAt += 10u;
    sale.expiresAt += 10u;
    EXPECT_EQ(market.AppendEvidence(sale, 0u, true), 0u);

    EconomyMarketSnapshot const snapshot = market.Snapshot(NOW - 5u);
    ASSERT_EQ(snapshot.evidence.size(), 1u);
    EXPECT_EQ(snapshot.evidence.front().observedAt, NOW - 10u);
    EXPECT_EQ(writes.size(), 1u);
    std::optional<EconomyReferencePrice> const reference = market.ReferencePrice(2u, "reagent:100", NOW);
    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(reference->acceptedSales, 1u);
    EXPECT_FALSE(reference->confident);
}

TEST(PlayerbotEconomyMarketTest, AsyncFailureRetainsCommittedAndReleasesUncommittedClaims)
{
    std::vector<uint64> queuedTokens;
    PlayerbotEconomyMarket market([&queuedTokens](uint64 token, EconomyMarketWrite const&)
                                  { queuedTokens.push_back(token); });

    uint64 const uncommitted = market.AppendEvidence(Evidence(EconomyEvidenceSource::Listing, 90u), 41u, false);
    uint64 const committed = market.AppendEvidence(Evidence(EconomyEvidenceSource::Sale, 100u), 42u, true);
    ASSERT_EQ(queuedTokens, (std::vector<uint64>{uncommitted, committed}));

    market.CompleteWrite(uncommitted, false);
    market.CompleteWrite(committed, false);
    EconomyMarketSnapshot const snapshot = market.Snapshot(NOW);

    EXPECT_FALSE(snapshot.persistenceHealthy);
    EXPECT_EQ(snapshot.persistenceBlocker, EconomyMarketBlocker::PersistenceUnavailable);
    ASSERT_EQ(snapshot.persistenceFailures.size(), 2u);
    EXPECT_EQ(snapshot.persistenceFailures[0].claimDisposition, EconomyClaimDisposition::ReleaseUncommitted);
    EXPECT_EQ(snapshot.persistenceFailures[1].claimDisposition, EconomyClaimDisposition::RetainCommitted);
    EXPECT_EQ(snapshot.persistenceFailures[0].leaseId, 41u);
    EXPECT_EQ(snapshot.persistenceFailures[1].leaseId, 42u);
    EXPECT_EQ(market.AppendEvidence(Evidence(EconomyEvidenceSource::Listing, 110u), 43u, false), 0u);
    EXPECT_EQ(market.Snapshot(NOW).evidence.size(), 2u);
}

TEST(PlayerbotEconomyMarketTest, FailedPositionCloseRemainsVisibleWithPersistenceBlocker)
{
    PlayerbotEconomyMarket market([](uint64, EconomyMarketWrite const&) {});
    EconomyPosition position = MakePosition("failed-close", 5u, 500u);
    position.initialQuantity = 5u;

    EconomyPositionMutationResult const opened = market.OpenPosition(position, 1'001u, 41u, NOW);
    ASSERT_TRUE(opened.accepted);
    market.CompleteWrite(opened.writeToken, true);

    EconomyPositionMutationResult const closed = market.ApplyPositionEvent(
        {
            .kind = EconomyPositionEventKind::Used,
            .positionPublicId = position.publicId,
            .itemGuid = 1'001u,
            .quantity = 5u,
            .occurredAt = NOW + 1u,
        },
        41u, true);
    ASSERT_TRUE(closed.accepted);
    EXPECT_TRUE(market.Snapshot(NOW + 1u).positions.empty());

    market.CompleteWrite(closed.writeToken, false);
    EconomyMarketSnapshot const snapshot = market.Snapshot(NOW + 1u);
    EXPECT_FALSE(snapshot.persistenceHealthy);
    EXPECT_EQ(snapshot.persistenceBlocker, EconomyMarketBlocker::PersistenceUnavailable);
    ASSERT_EQ(snapshot.positions.size(), 1u);
    EXPECT_EQ(snapshot.positions.front().publicId, position.publicId);
    EXPECT_EQ(snapshot.positions.front().state, EconomyPositionState::Closed);
    EXPECT_EQ(snapshot.positions.front().remainingQuantity, 0u);
    EXPECT_EQ(snapshot.positions.front().realizedCost, 500u);
    EXPECT_EQ(snapshot.positions.front().realizedOutcome, EconomyPositionOutcome::Use);
}

TEST(PlayerbotEconomyMarketTest, EconomyDecisionsUseTheLoadedViewWithoutInvokingTheWriter)
{
    uint32 writes = 0;
    PlayerbotEconomyMarket market([&writes](uint64, EconomyMarketWrite const&) { ++writes; });
    EconomyMarketStartup startup;
    startup.evidence.push_back(Evidence(EconomyEvidenceSource::Sale, 100u));
    static_cast<void>(market.Restore(std::move(startup), NOW));

    EXPECT_TRUE(market.ReferencePrice(2u, "reagent:100", NOW).has_value());
    EXPECT_EQ(market.Snapshot(NOW).evidence.size(), 1u);
    EXPECT_EQ(writes, 0u);
}

TEST(PlayerbotEconomyMarketTest, ConfigurationDefaultsAndInvalidCombinationsFailClosed)
{
    EconomyRiskConfiguration defaults;
    EconomyRiskFacts facts;
    facts.economyAffinity = 100u;
    facts.freeTradeskillMoney = 10'000u;
    facts.qualifiedEvidence = 10u;
    facts.proposedCost = 1u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(defaults, facts).blocker, EconomyRiskBlocker::Disabled);
    EXPECT_STREQ(PlayerbotEconomyMarket::RiskBlockerName(EconomyRiskBlocker::Disabled), "market_making_disabled");

    EconomyRiskConfiguration invalid;
    invalid.enabled = true;
    invalid.perGroupExposurePercent = 10u;
    invalid.totalExposurePercent = 5u;
    invalid.minimumEvidence = 3u;
    invalid.holdingHorizonSeconds = 3'600u;
    invalid.maximumRelistAttempts = 2u;
    invalid.cooldownSeconds = 60u;
    invalid.perGroupExposurePercent = 0u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(invalid, facts).blocker, EconomyRiskBlocker::MissingExposure);

    invalid.perGroupExposurePercent = 101u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(invalid, facts).blocker, EconomyRiskBlocker::InvalidPercentage);

    invalid.perGroupExposurePercent = 10u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(invalid, facts).blocker, EconomyRiskBlocker::InvalidConcentration);

    invalid.totalExposurePercent = 20u;
    invalid.minimumEvidence = PlayerbotEconomyMarket::MAX_EVIDENCE_PER_GROUP + 1u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(invalid, facts).blocker, EconomyRiskBlocker::InvalidEvidenceMinimum);

    invalid.minimumEvidence = 3u;
    invalid.holdingHorizonSeconds = 0u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(invalid, facts).blocker, EconomyRiskBlocker::InvalidHoldingHorizon);

    invalid.holdingHorizonSeconds = 3'600u;
    invalid.maximumRelistAttempts = 256u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(invalid, facts).blocker, EconomyRiskBlocker::InvalidRelistAttempts);

    invalid.maximumRelistAttempts = 2u;
    invalid.cooldownSeconds = 0u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(invalid, facts).blocker, EconomyRiskBlocker::InvalidCooldown);
}

TEST(PlayerbotEconomyMarketTest, ConfigurationExplicitLimitsScaleWithAffinityAndCurrentExposure)
{
    EconomyRiskConfiguration const configuration = RiskConfiguration();

    EconomyRiskFacts facts;
    facts.economyAffinity = 75u;
    facts.freeTradeskillMoney = 10'000u;
    facts.qualifiedEvidence = 3u;
    facts.proposedCost = 750u;
    EconomyRiskDecision decision = PlayerbotEconomyMarket::EvaluateRisk(configuration, facts);
    EXPECT_EQ(decision.blocker, EconomyRiskBlocker::None);
    EXPECT_EQ(decision.perGroupExposureLimit, 750u);
    EXPECT_EQ(decision.totalExposureLimit, 1'875u);

    facts.groupExposure = 1u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(configuration, facts).blocker, EconomyRiskBlocker::GroupExposure);

    facts.groupExposure = 0u;
    facts.totalExposure = 1'126u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(configuration, facts).blocker, EconomyRiskBlocker::TotalExposure);

    facts.totalExposure = 0u;
    facts.qualifiedEvidence = 2u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(configuration, facts).blocker,
              EconomyRiskBlocker::InsufficientEvidence);

    facts.qualifiedEvidence = 3u;
    facts.economyAffinity = 74u;
    EXPECT_EQ(PlayerbotEconomyMarket::EvaluateRisk(configuration, facts).blocker, EconomyRiskBlocker::AffinityTooLow);
}

TEST(PlayerbotEconomyMarketTest, PositionLifecyclePreservesQuantityCostAndGoldAcrossPhysicalLotChanges)
{
    std::vector<EconomyMarketWrite> writes;
    PlayerbotEconomyMarket market([&writes](uint64, EconomyMarketWrite const& write) { writes.push_back(write); });
    EconomyPosition position = MakePosition("position-lifecycle", 10u, 1'000u);
    position.initialQuantity = 10u;
    position.relistAttempts = 0u;

    EconomyPositionMutationResult opened = market.OpenPosition(position, 1'001u, 41u, NOW);
    ASSERT_TRUE(opened.accepted);
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes.back().kind, EconomyMarketWriteKind::OpenPositionTransaction);
    ASSERT_EQ(writes.back().circulationEvents.size(), 1u);
    EXPECT_EQ(writes.back().circulationEvents.front().quantity, 10u);
    EXPECT_EQ(market.ControlledItemGuids(position.traderGuid, position.marketId), std::vector<uint64>({1'001u}));
    EXPECT_FALSE(market
                     .ApplyPositionEvent(
                         {
                             .kind = EconomyPositionEventKind::Listed,
                             .positionPublicId = position.publicId,
                             .itemGuid = 1'001u,
                             .quantity = 10u,
                             .auctionId = 76u,
                             .occurredAt = NOW + 1u,
                         },
                         41u, true)
                     .accepted);
    market.CompleteWrite(opened.writeToken, true);

    EconomyPositionEvent split{
        .kind = EconomyPositionEventKind::Split,
        .positionPublicId = position.publicId,
        .itemGuid = 1'001u,
        .replacementItemGuid = 1'002u,
        .quantity = 4u,
        .replacementQuantity = 6u,
        .occurredAt = NOW + 1u,
    };
    EconomyPositionMutationResult mutation = market.ApplyPositionEvent(split, 41u, true);
    ASSERT_TRUE(mutation.accepted);
    market.CompleteWrite(mutation.writeToken, true);
    EXPECT_EQ(market.ControlledItemGuids(position.traderGuid, position.marketId),
              std::vector<uint64>({1'001u, 1'002u}));

    EconomyPositionEvent merge{
        .kind = EconomyPositionEventKind::Merge,
        .positionPublicId = position.publicId,
        .itemGuid = 1'001u,
        .replacementItemGuid = 1'002u,
        .quantity = 4u,
        .replacementQuantity = 10u,
        .occurredAt = NOW + 2u,
    };
    mutation = market.ApplyPositionEvent(merge, 41u, true);
    ASSERT_TRUE(mutation.accepted);
    market.CompleteWrite(mutation.writeToken, true);

    EconomyPositionEvent listed{
        .kind = EconomyPositionEventKind::Listed,
        .positionPublicId = position.publicId,
        .itemGuid = 1'002u,
        .quantity = 10u,
        .auctionId = 77u,
        .occurredAt = NOW + 3u,
    };
    mutation = market.ApplyPositionEvent(listed, 41u, true);
    ASSERT_TRUE(mutation.accepted);
    market.CompleteWrite(mutation.writeToken, true);

    EconomyPositionEvent expired = listed;
    expired.kind = EconomyPositionEventKind::Expired;
    expired.fees = 30u;
    expired.cooldownSeconds = 300u;
    expired.occurredAt = NOW + 4u;
    mutation = market.ApplyPositionEvent(expired, 41u, true);
    ASSERT_TRUE(mutation.accepted);
    market.CompleteWrite(mutation.writeToken, true);

    EconomyPositionEvent relisted = listed;
    relisted.kind = EconomyPositionEventKind::Relisted;
    relisted.auctionId = 78u;
    relisted.occurredAt = NOW + 5u;
    mutation = market.ApplyPositionEvent(relisted, 41u, true);
    ASSERT_TRUE(mutation.accepted);
    market.CompleteWrite(mutation.writeToken, true);

    EconomyPositionEvent sold = relisted;
    sold.kind = EconomyPositionEventKind::Sold;
    sold.quantity = 6u;
    sold.proceeds = 900u;
    sold.fees = 45u;
    sold.occurredAt = NOW + 6u;
    EconomyPositionMutationResult sale = market.ApplyPositionEvent(sold, 41u, true);
    ASSERT_TRUE(sale.accepted);
    EXPECT_EQ(sale.releasedCost, 600u);
    market.CompleteWrite(sale.writeToken, true);

    EconomyMarketSnapshot snapshot = market.Snapshot(NOW + 6u);
    ASSERT_EQ(snapshot.positions.size(), 1u);
    EXPECT_EQ(snapshot.positions.front().remainingQuantity, 4u);
    EXPECT_EQ(snapshot.positions.front().acquisitionCost, 400u);
    EXPECT_EQ(snapshot.positions.front().realizedCost, 600u);
    EXPECT_EQ(snapshot.positions.front().realizedProceeds, 900u);
    EXPECT_EQ(snapshot.positions.front().realizedFees, 75u);
    EXPECT_EQ(snapshot.positions.front().relistAttempts, 1u);
    ASSERT_EQ(snapshot.cooldowns.size(), 1u);
    EXPECT_EQ(snapshot.cooldowns.front().cause, EconomyCooldownCause::Expired);

    EconomyPositionEvent used{
        .kind = EconomyPositionEventKind::Used,
        .positionPublicId = position.publicId,
        .itemGuid = 1'002u,
        .quantity = 4u,
        .occurredAt = NOW + 7u,
    };
    mutation = market.ApplyPositionEvent(used, 41u, true);
    ASSERT_TRUE(mutation.accepted);
    snapshot = market.Snapshot(NOW + 7u);
    EXPECT_TRUE(snapshot.positions.empty());
    EXPECT_EQ(writes.back().kind, EconomyMarketWriteKind::ClosePositionTransaction);
    EXPECT_EQ(writes.back().position.realizedCost, 1'000u);
    EXPECT_EQ(writes.back().position.realizedProceeds, 900u);
    EXPECT_EQ(writes.back().position.realizedFees, 75u);
    EXPECT_EQ(writes.back().position.realizedOutcome, EconomyPositionOutcome::Use);
    EXPECT_TRUE(market.ControlledItemGuids(position.traderGuid, position.marketId).empty());
}

TEST(PlayerbotEconomyMarketTest, PositionStagedPurchaseClosesTheCrashWindowBeforeAcquisition)
{
    std::vector<uint64> tokens;
    PlayerbotEconomyMarket market([&tokens](uint64 token, EconomyMarketWrite const&) { tokens.push_back(token); });
    EconomyPosition staged = MakePosition("staged-position", 5u, 500u);
    staged.state = EconomyPositionState::Pending;

    EconomyPositionMutationResult const pending = market.StagePosition(staged, 9001u, 77u, 41u, NOW);
    ASSERT_TRUE(pending.accepted);
    EXPECT_TRUE(market.HasPendingWrite(staged.publicId));
    EXPECT_FALSE(market.ActivatePendingPosition(staged.publicId, 41u, NOW + 1u).accepted);

    market.CompleteWrite(pending.writeToken, true);
    EconomyPositionMutationResult const acquired = market.ActivatePendingPosition(staged.publicId, 41u, NOW + 1u);
    ASSERT_TRUE(acquired.accepted);
    EconomyMarketSnapshot active = market.Snapshot(NOW + 1u);
    ASSERT_EQ(active.positions.size(), 1u);
    EXPECT_EQ(active.positions.front().state, EconomyPositionState::Open);
    ASSERT_EQ(active.circulation.size(), 2u);
    EXPECT_EQ(active.circulation[0].state, EconomyCirculationState::Pending);
    EXPECT_EQ(active.circulation[1].state, EconomyCirculationState::Acquired);

    EconomyMarketStartup restart;
    staged.openedAt = NOW;
    staged.updatedAt = NOW;
    restart.positions.push_back(staged);
    restart.circulation.push_back(active.circulation.front());
    PlayerbotEconomyMarket recovered;
    EXPECT_TRUE(recovered.Restore(std::move(restart), NOW + 1u).empty());
    ASSERT_EQ(recovered.Snapshot(NOW + 1u).positions.size(), 1u);
    EXPECT_EQ(recovered.Snapshot(NOW + 1u).positions.front().state, EconomyPositionState::Pending);
    uint64 const cancellation = recovered.CancelPendingPosition(staged.publicId, 0u);
    EXPECT_NE(cancellation, 0u);
    EXPECT_TRUE(recovered.Snapshot(NOW + 1u).positions.empty());
    EXPECT_FALSE(recovered.StagePosition(staged, 9001u, 77u, 42u, NOW + 2u).accepted);
    recovered.CompleteWrite(cancellation, true);
    EXPECT_TRUE(recovered.StagePosition(staged, 9001u, 77u, 42u, NOW + 2u).accepted);
}

TEST(PlayerbotEconomyMarketTest, PositionRestartRetainsClosedControlledAuctionProvenance)
{
    EconomyMarketStartup startup;
    startup.circulation.push_back({
        .positionPublicId = "closed-position",
        .itemGuid = 9'001u,
        .quantity = 5u,
        .auctionId = 77u,
        .provenance = EconomyCirculationProvenance::Speculative,
        .state = EconomyCirculationState::Delivered,
        .occurredAt = NOW,
    });

    PlayerbotEconomyMarket market;
    EXPECT_TRUE(market.Restore(std::move(startup), NOW + 1u).empty());
    EconomyMarketSnapshot const snapshot = market.Snapshot(NOW + 1u);
    ASSERT_EQ(snapshot.circulation.size(), 1u);
    EXPECT_EQ(snapshot.circulation.front().auctionId, 77u);
    EXPECT_TRUE(market.ControlledItemGuids(10u, 2u).empty());
}

TEST(PlayerbotEconomyMarketTest, PositionTerminalOutcomesCloseExactlyOnceAndLossAppliesCooldown)
{
    for (EconomyPositionEventKind const kind :
         {EconomyPositionEventKind::Transformed, EconomyPositionEventKind::Vendored, EconomyPositionEventKind::Lost})
    {
        PlayerbotEconomyMarket market;
        EconomyPosition position = MakePosition("terminal-" + std::to_string(static_cast<uint8>(kind)), 5u, 505u);
        EconomyPositionMutationResult const opened =
            market.OpenPosition(position, 2'000u + static_cast<uint8>(kind), 0u, NOW);
        ASSERT_TRUE(opened.accepted);
        market.CompleteWrite(opened.writeToken, true);

        EconomyPositionEvent event{
            .kind = kind,
            .positionPublicId = position.publicId,
            .itemGuid = 2'000u + static_cast<uint8>(kind),
            .quantity = 5u,
            .proceeds = kind == EconomyPositionEventKind::Vendored ? 25u : 0u,
            .occurredAt = NOW + 1u,
            .cooldownSeconds = 300u,
        };
        EconomyPositionMutationResult result = market.ApplyPositionEvent(event, 0u, true);
        ASSERT_TRUE(result.accepted);
        EXPECT_EQ(result.releasedCost, 505u);
        EconomyMarketSnapshot const snapshot = market.Snapshot(NOW + 1u);
        EXPECT_TRUE(snapshot.positions.empty());
        EXPECT_EQ(snapshot.cooldowns.size(), kind == EconomyPositionEventKind::Lost ? 1u : 0u);
    }
}

TEST(PlayerbotEconomyMarketTest, PositionEntryRequiresLeftoverValueAndRejectsSameAccountOrSecondSpeculativeCycle)
{
    PlayerbotEconomyMarket market;
    EconomyMarketEntryFacts facts;
    facts.risk.economyAffinity = 100u;
    facts.risk.freeTradeskillMoney = 100'000u;
    facts.risk.qualifiedEvidence = 3u;
    facts.risk.proposedCost = 1'000u;
    facts.traderGuid = 10u;
    facts.buyerAccountId = 20u;
    facts.sellerAccountId = 30u;
    facts.marketId = 2u;
    facts.itemId = 100u;
    facts.substitutionGroup = "reagent:100";
    facts.itemGuid = 3'001u;
    facts.quantity = 10u;
    facts.buyout = 1'000u;
    facts.referenceUnitPrice = 150u;
    facts.referenceConfident = true;
    facts.depositPerListing = 25u;
    facts.auctionCutBasisPoints = 500u;
    facts.now = NOW;

    EXPECT_EQ(market.EvaluateEntry(RiskConfiguration(), facts).blocker, EconomyRiskBlocker::None);

    facts.sellerAccountId = facts.buyerAccountId;
    EXPECT_EQ(market.EvaluateEntry(RiskConfiguration(), facts).blocker, EconomyRiskBlocker::SameAccountPurchase);

    facts.sellerAccountId = 0u;
    EXPECT_EQ(market.EvaluateEntry(RiskConfiguration(), facts).blocker, EconomyRiskBlocker::SameAccountPurchase);

    facts.sellerAccountId = 30u;
    facts.depositPerListing = 200u;
    EXPECT_EQ(market.EvaluateEntry(RiskConfiguration(), facts).blocker, EconomyRiskBlocker::ExpectedLoss);

    facts.depositPerListing = 25u;
    EconomyPosition position = MakePosition("single-cycle", facts.quantity, facts.buyout);
    position.initialQuantity = facts.quantity;
    ASSERT_TRUE(market.OpenPosition(position, facts.itemGuid, 0u, NOW).accepted);
    EXPECT_EQ(market.EvaluateEntry(RiskConfiguration(), facts).blocker, EconomyRiskBlocker::AlreadySpeculated);
}

TEST(PlayerbotEconomyMarketTest, ConsumerPriorityDisplacesUncommittedSpeculationAndSameAccountStillFails)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const group = EconomySubstitutionGroup::ExactReagent(100u);
    coordinator.RefreshActor(
        {
            .characterGuid = 10u,
            .accountId = 20u,
            .marketId = 2u,
            .online = true,
            .autonomous = true,
            .economyAffinity = 100u,
        },
        NOW);
    coordinator.RefreshActor(
        {
            .characterGuid = 11u,
            .accountId = 21u,
            .marketId = 2u,
            .online = true,
            .autonomous = true,
            .economyAffinity = 100u,
            .demands = {{group, 5u}},
        },
        NOW);
    coordinator.RefreshMarket(
        {
            .marketId = 2u,
            .supplies = {{group, 10u, EconomySupplySource::ActiveAuction}},
        },
        NOW);

    EconomyAssignmentRequest speculation;
    speculation.characterGuid = 10u;
    speculation.marketId = 2u;
    speculation.group = group;
    speculation.quantity = 5u;
    speculation.kind = EconomyClaimKind::Purchase;
    speculation.priority = EconomyClaimPriority::Speculation;
    speculation.workKind = EconomyWorkKind::MarketMaking;
    speculation.workIdentity = "market:100";
    speculation.sellerAccountId = 30u;
    speculation.expiresAt = NOW + 60u;
    ASSERT_TRUE(coordinator.Lease(speculation, NOW).assignment.has_value());

    coordinator.RefreshActor(
        {
            .characterGuid = 12u,
            .accountId = 22u,
            .marketId = 2u,
            .online = true,
            .autonomous = true,
            .economyAffinity = 100u,
            .demands = {{group, 5u}},
        },
        NOW);
    EconomyAssignmentRequest consumer = speculation;
    consumer.characterGuid = 12u;
    consumer.priority = EconomyClaimPriority::Consumer;
    consumer.workKind = EconomyWorkKind::Buy;
    consumer.workIdentity = "consume:100";
    EconomyAssignmentLease consumerLease = coordinator.Lease(consumer, NOW);
    ASSERT_TRUE(consumerLease.assignment.has_value());

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(NOW);
    ASSERT_EQ(snapshot.claims.size(), 2u);
    EXPECT_EQ(snapshot.claims[0].state, EconomyClaimState::Released);
    EXPECT_EQ(snapshot.claims[1].state, EconomyClaimState::Leased);

    EconomyAssignmentRequest sameAccount = speculation;
    sameAccount.sellerAccountId = 20u;
    EXPECT_EQ(coordinator.Lease(sameAccount, NOW).blocker, EconomyWorkBlocker::SameAccountPurchase);
}
