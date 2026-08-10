/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyMarket.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include "StringFormat.h"

namespace PlayerbotEconomy
{
namespace
{
using EvidenceKey = std::pair<uint32, std::string>;
constexpr uint32 MAX_STARTUP_EVIDENCE = 4'096u;
constexpr uint32 MAX_STARTUP_POSITIONS = 1'024u;
constexpr uint32 MAX_STARTUP_CONTROLLED_AUCTIONS = 4'096u;
constexpr uint32 MAX_CIRCULATION_PER_POSITION = 256u;
constexpr uint32 MAX_STARTUP_COOLDOWNS = 2'048u;

AsyncCallbackProcessor<TransactionCallback>& EconomyTransactionProcessor()
{
    static AsyncCallbackProcessor<TransactionCallback> processor;
    return processor;
}

std::optional<EconomyEvidenceSource> ParseEvidenceSource(std::string const& value)
{
    if (value == "sale")
        return EconomyEvidenceSource::Sale;
    if (value == "listing")
        return EconomyEvidenceSource::Listing;
    if (value == "recovery")
        return EconomyEvidenceSource::Recovery;
    if (value == "speculation")
        return EconomyEvidenceSource::Speculation;
    return std::nullopt;
}

std::optional<EconomyPositionState> ParsePositionState(std::string const& value)
{
    if (value == "pending")
        return EconomyPositionState::Pending;
    if (value == "open")
        return EconomyPositionState::Open;
    if (value == "listed")
        return EconomyPositionState::Listed;
    if (value == "closed")
        return EconomyPositionState::Closed;
    if (value == "lost")
        return EconomyPositionState::Lost;
    return std::nullopt;
}

std::optional<EconomyCooldownCause> ParseCooldownCause(std::string const& value)
{
    if (value == "loss")
        return EconomyCooldownCause::Loss;
    if (value == "failed_purchase")
        return EconomyCooldownCause::FailedPurchase;
    if (value == "failed_listing")
        return EconomyCooldownCause::FailedListing;
    if (value == "expired")
        return EconomyCooldownCause::Expired;
    return std::nullopt;
}

std::optional<EconomyCirculationProvenance> ParseCirculationProvenance(std::string const& value)
{
    if (value == "ordinary")
        return EconomyCirculationProvenance::Ordinary;
    if (value == "speculative")
        return EconomyCirculationProvenance::Speculative;
    if (value == "recovery")
        return EconomyCirculationProvenance::Recovery;
    return std::nullopt;
}

std::optional<EconomyCirculationState> ParseCirculationState(std::string const& value)
{
    if (value == "pending")
        return EconomyCirculationState::Pending;
    if (value == "acquired")
        return EconomyCirculationState::Acquired;
    if (value == "listed")
        return EconomyCirculationState::Listed;
    if (value == "delivered")
        return EconomyCirculationState::Delivered;
    if (value == "merged")
        return EconomyCirculationState::Merged;
    if (value == "consumed")
        return EconomyCirculationState::Consumed;
    if (value == "transformed")
        return EconomyCirculationState::Transformed;
    if (value == "vendored")
        return EconomyCirculationState::Vendored;
    if (value == "lost")
        return EconomyCirculationState::Lost;
    return std::nullopt;
}

char const* EvidenceSourceName(EconomyEvidenceSource value)
{
    switch (value)
    {
        case EconomyEvidenceSource::Sale:
            return "sale";
        case EconomyEvidenceSource::Listing:
            return "listing";
        case EconomyEvidenceSource::Recovery:
            return "recovery";
        case EconomyEvidenceSource::Speculation:
            return "speculation";
    }
    return "recovery";
}

char const* PositionStateName(EconomyPositionState value)
{
    switch (value)
    {
        case EconomyPositionState::Pending:
            return "pending";
        case EconomyPositionState::Open:
            return "open";
        case EconomyPositionState::Listed:
            return "listed";
        case EconomyPositionState::Closed:
            return "closed";
        case EconomyPositionState::Lost:
            return "lost";
    }
    return "lost";
}

char const* PositionOutcomeName(EconomyPositionOutcome value)
{
    switch (value)
    {
        case EconomyPositionOutcome::Sale:
            return "sale";
        case EconomyPositionOutcome::Use:
            return "use";
        case EconomyPositionOutcome::Transformation:
            return "transformation";
        case EconomyPositionOutcome::Vendor:
            return "vendor";
        case EconomyPositionOutcome::Loss:
        case EconomyPositionOutcome::None:
            return "loss";
    }
    return "loss";
}

char const* CooldownCauseName(EconomyCooldownCause value)
{
    switch (value)
    {
        case EconomyCooldownCause::Loss:
            return "loss";
        case EconomyCooldownCause::FailedPurchase:
            return "failed_purchase";
        case EconomyCooldownCause::FailedListing:
            return "failed_listing";
        case EconomyCooldownCause::Expired:
            return "expired";
    }
    return "loss";
}

char const* CirculationProvenanceName(EconomyCirculationProvenance value)
{
    switch (value)
    {
        case EconomyCirculationProvenance::Ordinary:
            return "ordinary";
        case EconomyCirculationProvenance::Speculative:
            return "speculative";
        case EconomyCirculationProvenance::Recovery:
            return "recovery";
    }
    return "recovery";
}

char const* CirculationStateName(EconomyCirculationState value)
{
    switch (value)
    {
        case EconomyCirculationState::Pending:
            return "pending";
        case EconomyCirculationState::Acquired:
            return "acquired";
        case EconomyCirculationState::Listed:
            return "listed";
        case EconomyCirculationState::Delivered:
            return "delivered";
        case EconomyCirculationState::Merged:
            return "merged";
        case EconomyCirculationState::Consumed:
            return "consumed";
        case EconomyCirculationState::Transformed:
            return "transformed";
        case EconomyCirculationState::Vendored:
            return "vendored";
        case EconomyCirculationState::Lost:
            return "lost";
    }
    return "lost";
}

std::string SqlString(std::string value)
{
    PlayerbotsDatabase.EscapeString(value);
    return Acore::StringFormat("'{}'", value);
}

std::string CirculationInsertSql(EconomyCirculation const& event)
{
    return Acore::StringFormat(
        "INSERT INTO playerbot_economy_circulation "
        "(position_public_id, item_guid, quantity, auction_id, provenance, state, occurred_at) "
        "VALUES ({}, {}, {}, NULLIF({}, 0), {}, {}, FROM_UNIXTIME({}))",
        SqlString(event.positionPublicId), event.itemGuid, event.quantity, event.auctionId,
        SqlString(CirculationProvenanceName(event.provenance)), SqlString(CirculationStateName(event.state)),
        event.occurredAt);
}

std::string CooldownInsertSql(EconomyCooldown const& cooldown)
{
    return Acore::StringFormat(
        "INSERT INTO playerbot_economy_cooldown "
        "(trader_guid, market_id, substitution_group, cause, next_eligible_at) "
        "VALUES ({}, {}, {}, {}, FROM_UNIXTIME({})) ON DUPLICATE KEY UPDATE "
        "cause = VALUES(cause), next_eligible_at = VALUES(next_eligible_at)",
        cooldown.traderGuid, cooldown.marketId, SqlString(cooldown.substitutionGroup),
        SqlString(CooldownCauseName(cooldown.cause)), cooldown.nextEligibleAt);
}

std::string MarketWriteSql(EconomyMarketWrite const& write)
{
    switch (write.kind)
    {
        case EconomyMarketWriteKind::AppendEvidence:
            return Acore::StringFormat(
                "INSERT INTO playerbot_economy_price_evidence "
                "(market_id, item_id, substitution_group, source, auction_id, unit_price, quantity, "
                "observed_at, expires_at, position_public_id) VALUES ({}, {}, {}, {}, NULLIF({}, 0), {}, "
                "{}, FROM_UNIXTIME({}), FROM_UNIXTIME({}), NULLIF({}, '')) ON DUPLICATE KEY UPDATE "
                "item_id = VALUES(item_id), substitution_group = VALUES(substitution_group), "
                "unit_price = VALUES(unit_price), quantity = VALUES(quantity), "
                "observed_at = VALUES(observed_at), expires_at = VALUES(expires_at), "
                "position_public_id = VALUES(position_public_id)",
                write.evidence.marketId, write.evidence.itemId, SqlString(write.evidence.substitutionGroup),
                SqlString(EvidenceSourceName(write.evidence.source)), write.evidence.auctionId,
                write.evidence.unitPrice, write.evidence.quantity, write.evidence.observedAt, write.evidence.expiresAt,
                SqlString(write.evidence.positionPublicId));
        case EconomyMarketWriteKind::OpenPosition:
        case EconomyMarketWriteKind::OpenPositionTransaction:
            return Acore::StringFormat(
                "INSERT INTO playerbot_economy_position "
                "(public_id, trader_guid, market_id, item_id, substitution_group, initial_quantity, "
                "remaining_quantity, acquisition_cost, state, maximum_relist_attempts, cooldown_seconds, "
                "opened_at, holding_deadline, updated_at) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, "
                "{}, FROM_UNIXTIME({}), FROM_UNIXTIME({}), FROM_UNIXTIME({}))",
                SqlString(write.position.publicId), write.position.traderGuid, write.position.marketId,
                write.position.itemId, SqlString(write.position.substitutionGroup), write.position.initialQuantity,
                write.position.remainingQuantity, write.position.acquisitionCost,
                SqlString(PositionStateName(write.position.state)), write.position.maximumRelistAttempts,
                write.position.cooldownSeconds, write.position.openedAt, write.position.holdingDeadline,
                write.position.updatedAt);
        case EconomyMarketWriteKind::UpdatePosition:
        case EconomyMarketWriteKind::UpdatePositionTransaction:
            return Acore::StringFormat(
                "UPDATE playerbot_economy_position SET remaining_quantity = {}, acquisition_cost = {}, "
                "realized_cost = {}, realized_proceeds = {}, realized_fees = {}, state = {}, "
                "relist_attempts = {}, updated_at = FROM_UNIXTIME({}) WHERE public_id = {}",
                write.position.remainingQuantity, write.position.acquisitionCost, write.position.realizedCost,
                write.position.realizedProceeds, write.position.realizedFees,
                SqlString(PositionStateName(write.position.state)), write.position.relistAttempts,
                write.position.updatedAt, SqlString(write.position.publicId));
        case EconomyMarketWriteKind::ClosePosition:
        case EconomyMarketWriteKind::ClosePositionTransaction:
            return Acore::StringFormat(
                "UPDATE playerbot_economy_position SET remaining_quantity = 0, acquisition_cost = {}, "
                "realized_cost = {}, realized_proceeds = {}, realized_fees = {}, state = {}, "
                "realized_outcome = {}, updated_at = FROM_UNIXTIME({}), closed_at = FROM_UNIXTIME({}) "
                "WHERE public_id = {}",
                write.position.acquisitionCost, write.position.realizedCost, write.position.realizedProceeds,
                write.position.realizedFees, SqlString(PositionStateName(write.position.state)),
                SqlString(PositionOutcomeName(write.position.realizedOutcome)), write.position.updatedAt,
                write.position.closedAt, SqlString(write.position.publicId));
        case EconomyMarketWriteKind::AppendCirculation:
            return CirculationInsertSql(write.circulation);
        case EconomyMarketWriteKind::SaveCooldown:
            return CooldownInsertSql(write.cooldown);
        case EconomyMarketWriteKind::DeletePendingPosition:
            return Acore::StringFormat(
                "DELETE FROM playerbot_economy_position WHERE public_id = {} AND state = 'pending'",
                SqlString(write.position.publicId));
    }
    return {};
}

void QueueDatabaseWrite(uint64 token, EconomyMarketWrite const& write)
{
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    transaction->Append(MarketWriteSql(write));
    for (EconomyCirculation const& event : write.circulationEvents)
        transaction->Append(CirculationInsertSql(event));
    if (write.hasCooldown)
        transaction->Append(CooldownInsertSql(write.cooldown));
    EconomyTransactionProcessor()
        .AddCallback(PlayerbotsDatabase.AsyncCommitTransaction(transaction))
        .AfterComplete([token](bool success) { GetPlayerbotEconomyMarket().CompleteWrite(token, success); });
}

bool IsValid(EconomyPriceEvidence const& value)
{
    return value.marketId != 0u && value.itemId != 0u && !value.substitutionGroup.empty() && value.unitPrice != 0u &&
           value.quantity != 0u && value.expiresAt > value.observedAt;
}

bool IsOpen(EconomyPosition const& value)
{
    return value.state == EconomyPositionState::Pending || value.state == EconomyPositionState::Open ||
           value.state == EconomyPositionState::Listed;
}

bool IsValid(EconomyPosition const& value)
{
    return !value.publicId.empty() && value.traderGuid != 0u && value.marketId != 0u && value.itemId != 0u &&
           !value.substitutionGroup.empty() && value.initialQuantity != 0u && value.remainingQuantity != 0u &&
           value.remainingQuantity <= value.initialQuantity && value.acquisitionCost != 0u &&
           value.relistAttempts <= value.maximumRelistAttempts && value.maximumRelistAttempts != 0u &&
           value.cooldownSeconds != 0u && value.openedAt != 0u && value.updatedAt >= value.openedAt &&
           value.holdingDeadline > value.openedAt && IsOpen(value);
}

uint64 WeightedAverage(std::vector<EconomyPriceEvidence const*> const& values)
{
    uint64 totalQuantity = 0u;
    unsigned __int128 totalPrice = 0u;
    for (EconomyPriceEvidence const* value : values)
    {
        totalQuantity += value->quantity;
        totalPrice += static_cast<unsigned __int128>(value->unitPrice) * value->quantity;
    }

    return totalQuantity == 0u ? 0u : static_cast<uint64>(totalPrice / totalQuantity);
}

std::optional<EconomyReferencePrice> BuildReference(std::vector<EconomyPriceEvidence const*> const& candidates)
{
    if (candidates.empty())
        return std::nullopt;

    std::vector<uint64> prices;
    prices.reserve(candidates.size());
    for (EconomyPriceEvidence const* value : candidates)
        prices.push_back(value->unitPrice);
    std::sort(prices.begin(), prices.end());
    uint64 const median = prices[prices.size() / 2u];
    uint64 const floor = std::max<uint64>(1u, median / 4u);
    uint64 const ceiling = median > (UINT64_MAX / 4u) ? UINT64_MAX : median * 4u;

    std::vector<EconomyPriceEvidence const*> sales;
    std::vector<EconomyPriceEvidence const*> listings;
    for (EconomyPriceEvidence const* value : candidates)
    {
        if (value->unitPrice < floor || value->unitPrice > ceiling)
            continue;
        (value->source == EconomyEvidenceSource::Sale ? sales : listings).push_back(value);
    }
    if (sales.empty() && listings.empty())
        return std::nullopt;

    EconomyReferencePrice reference;
    reference.unitPrice = WeightedAverage(sales.empty() ? listings : sales);
    reference.acceptedSales = static_cast<uint32>(sales.size());
    reference.acceptedListings = static_cast<uint32>(listings.size());
    reference.confident = sales.size() >= 2u || (sales.size() == 1u && !listings.empty());
    return reference;
}
}  // namespace

EconomyRiskDecision PlayerbotEconomyMarket::EvaluateRisk(EconomyRiskConfiguration const& configuration,
                                                         EconomyRiskFacts const& facts)
{
    EconomyRiskDecision decision;
    if (!configuration.enabled)
    {
        decision.blocker = EconomyRiskBlocker::Disabled;
        return decision;
    }
    if (!configuration.perGroupExposurePercent || !configuration.totalExposurePercent)
    {
        decision.blocker = EconomyRiskBlocker::MissingExposure;
        return decision;
    }
    if (configuration.perGroupExposurePercent > 100u || configuration.totalExposurePercent > 100u)
    {
        decision.blocker = EconomyRiskBlocker::InvalidPercentage;
        return decision;
    }
    if (configuration.perGroupExposurePercent > configuration.totalExposurePercent)
    {
        decision.blocker = EconomyRiskBlocker::InvalidConcentration;
        return decision;
    }
    if (!configuration.minimumEvidence || configuration.minimumEvidence > MAX_EVIDENCE_PER_GROUP)
    {
        decision.blocker = EconomyRiskBlocker::InvalidEvidenceMinimum;
        return decision;
    }
    if (!configuration.holdingHorizonSeconds)
    {
        decision.blocker = EconomyRiskBlocker::InvalidHoldingHorizon;
        return decision;
    }
    if (!configuration.maximumRelistAttempts || configuration.maximumRelistAttempts > 255u)
    {
        decision.blocker = EconomyRiskBlocker::InvalidRelistAttempts;
        return decision;
    }
    if (!configuration.cooldownSeconds)
    {
        decision.blocker = EconomyRiskBlocker::InvalidCooldown;
        return decision;
    }
    if (facts.economyAffinity < 75u)
    {
        decision.blocker = EconomyRiskBlocker::AffinityTooLow;
        return decision;
    }
    if (facts.qualifiedEvidence < configuration.minimumEvidence)
    {
        decision.blocker = EconomyRiskBlocker::InsufficientEvidence;
        return decision;
    }

    decision.perGroupExposureLimit =
        facts.freeTradeskillMoney * configuration.perGroupExposurePercent * facts.economyAffinity / 10'000u;
    decision.totalExposureLimit =
        facts.freeTradeskillMoney * configuration.totalExposurePercent * facts.economyAffinity / 10'000u;
    if (facts.proposedCost > decision.perGroupExposureLimit ||
        facts.groupExposure > decision.perGroupExposureLimit - facts.proposedCost)
    {
        decision.blocker = EconomyRiskBlocker::GroupExposure;
        return decision;
    }
    if (facts.proposedCost > decision.totalExposureLimit ||
        facts.totalExposure > decision.totalExposureLimit - facts.proposedCost)
    {
        decision.blocker = EconomyRiskBlocker::TotalExposure;
        return decision;
    }

    decision.blocker = EconomyRiskBlocker::None;
    return decision;
}

EconomyRiskDecision PlayerbotEconomyMarket::EvaluateEntry(EconomyRiskConfiguration const& configuration,
                                                          EconomyMarketEntryFacts const& facts) const
{
    std::scoped_lock lock(mutex);
    EconomyRiskFacts risk = facts.risk;
    risk.proposedCost = facts.buyout;
    EconomyRiskDecision decision = EvaluateRisk(configuration, risk);
    if (decision.blocker != EconomyRiskBlocker::None)
        return decision;

    if (!facts.buyerAccountId || !facts.sellerAccountId || facts.buyerAccountId == facts.sellerAccountId)
    {
        decision.blocker = EconomyRiskBlocker::SameAccountPurchase;
        return decision;
    }
    if (!facts.referenceConfident || !facts.referenceUnitPrice || !facts.quantity)
    {
        decision.blocker = EconomyRiskBlocker::UnconfidentEvidence;
        return decision;
    }
    if (std::any_of(circulation.begin(), circulation.end(),
                    [&facts](EconomyCirculation const& event)
                    {
                        return event.itemGuid == facts.itemGuid &&
                               event.provenance == EconomyCirculationProvenance::Speculative;
                    }))
    {
        decision.blocker = EconomyRiskBlocker::AlreadySpeculated;
        return decision;
    }
    if (std::any_of(cooldowns.begin(), cooldowns.end(),
                    [&facts](EconomyCooldown const& value)
                    {
                        return value.traderGuid == facts.traderGuid && value.marketId == facts.marketId &&
                               value.substitutionGroup == facts.substitutionGroup && value.nextEligibleAt > facts.now;
                    }))
    {
        decision.blocker = EconomyRiskBlocker::Cooldown;
        return decision;
    }
    if (std::any_of(positions.begin(), positions.end(),
                    [&facts](EconomyPosition const& value)
                    {
                        return value.traderGuid == facts.traderGuid && value.marketId == facts.marketId &&
                               value.substitutionGroup == facts.substitutionGroup;
                    }))
    {
        decision.blocker = EconomyRiskBlocker::ExistingPosition;
        return decision;
    }

    unsigned __int128 const gross = static_cast<unsigned __int128>(facts.referenceUnitPrice) * facts.quantity;
    if (!facts.buyout || static_cast<unsigned __int128>(facts.buyout) >= gross)
    {
        decision.blocker = EconomyRiskBlocker::NotUnderpriced;
        return decision;
    }
    if (facts.auctionCutBasisPoints > 10'000u)
    {
        decision.blocker = EconomyRiskBlocker::ExpectedLoss;
        return decision;
    }

    unsigned __int128 const cut = gross * facts.auctionCutBasisPoints / 10'000u;
    unsigned __int128 const expiryRisk = static_cast<unsigned __int128>(facts.depositPerListing) *
                                         (static_cast<uint64>(configuration.maximumRelistAttempts) + 1u);
    unsigned __int128 const expectedCost = static_cast<unsigned __int128>(facts.buyout) + cut + expiryRisk;
    if (gross <= expectedCost)
        decision.blocker = EconomyRiskBlocker::ExpectedLoss;
    return decision;
}

std::vector<uint64> PlayerbotEconomyMarket::ControlledItemGuids(uint32 traderGuid, uint32 marketId) const
{
    std::scoped_lock lock(mutex);
    auto const existing = controlledItems.find({traderGuid, marketId});
    return existing == controlledItems.end() ? std::vector<uint64>{} : existing->second;
}

char const* PlayerbotEconomyMarket::RiskBlockerName(EconomyRiskBlocker blocker)
{
    switch (blocker)
    {
        case EconomyRiskBlocker::None:
            return "market_making_none";
        case EconomyRiskBlocker::Disabled:
            return "market_making_disabled";
        case EconomyRiskBlocker::MissingExposure:
            return "market_making_missing_exposure";
        case EconomyRiskBlocker::InvalidPercentage:
            return "market_making_invalid_percentage";
        case EconomyRiskBlocker::InvalidConcentration:
            return "market_making_invalid_concentration";
        case EconomyRiskBlocker::InvalidEvidenceMinimum:
            return "market_making_invalid_evidence_minimum";
        case EconomyRiskBlocker::InvalidHoldingHorizon:
            return "market_making_invalid_holding_horizon";
        case EconomyRiskBlocker::InvalidRelistAttempts:
            return "market_making_invalid_relist_attempts";
        case EconomyRiskBlocker::InvalidCooldown:
            return "market_making_invalid_cooldown";
        case EconomyRiskBlocker::AffinityTooLow:
            return "market_making_affinity_too_low";
        case EconomyRiskBlocker::InsufficientEvidence:
            return "market_making_insufficient_evidence";
        case EconomyRiskBlocker::GroupExposure:
            return "market_making_group_exposure";
        case EconomyRiskBlocker::TotalExposure:
            return "market_making_total_exposure";
        case EconomyRiskBlocker::SameAccountPurchase:
            return "market_making_same_account_purchase";
        case EconomyRiskBlocker::UnconfidentEvidence:
            return "market_making_unconfident_evidence";
        case EconomyRiskBlocker::NotUnderpriced:
            return "market_making_not_underpriced";
        case EconomyRiskBlocker::ExpectedLoss:
            return "market_making_expected_loss";
        case EconomyRiskBlocker::Cooldown:
            return "market_making_cooldown";
        case EconomyRiskBlocker::ExistingPosition:
            return "market_making_existing_position";
        case EconomyRiskBlocker::AlreadySpeculated:
            return "market_making_already_speculated";
    }
    return "market_making_unknown";
}

PlayerbotEconomyMarket::PlayerbotEconomyMarket(AsyncWriter writer) : writer(std::move(writer)) {}

EconomyPositionMutationResult PlayerbotEconomyMarket::OpenPosition(EconomyPosition value, uint64 itemGuid,
                                                                   uint64 leaseId, uint64 now)
{
    std::scoped_lock lock(mutex);
    if (!persistenceHealthy || value.publicId.empty() || !value.traderGuid || !value.marketId || !value.itemId ||
        value.substitutionGroup.empty() || !value.initialQuantity || value.remainingQuantity != value.initialQuantity ||
        !value.acquisitionCost || !itemGuid || !now || !value.maximumRelistAttempts || !value.cooldownSeconds ||
        value.holdingDeadline <= now || value.state != EconomyPositionState::Open || value.realizedCost ||
        value.realizedProceeds || value.realizedFees || value.closedAt ||
        value.realizedOutcome != EconomyPositionOutcome::None)
    {
        return {};
    }
    if (std::any_of(positions.begin(), positions.end(),
                    [&value](EconomyPosition const& existing)
                    {
                        return existing.publicId == value.publicId ||
                               (existing.traderGuid == value.traderGuid && existing.marketId == value.marketId &&
                                existing.substitutionGroup == value.substitutionGroup);
                    }) ||
        HasPendingWriteLocked(value.publicId) ||
        std::any_of(
            circulation.begin(), circulation.end(), [itemGuid](EconomyCirculation const& event)
            { return event.itemGuid == itemGuid && event.provenance == EconomyCirculationProvenance::Speculative; }))
    {
        return {};
    }

    value.openedAt = now;
    value.updatedAt = now;
    value.relistAttempts = 0u;
    EconomyCirculation acquired{
        .positionPublicId = value.publicId,
        .itemGuid = itemGuid,
        .quantity = value.remainingQuantity,
        .provenance = EconomyCirculationProvenance::Speculative,
        .state = EconomyCirculationState::Acquired,
        .occurredAt = now,
    };
    positions.push_back(value);
    circulation.push_back(acquired);
    RebuildControlledItemIndex();

    EconomyMarketWrite write;
    write.kind = EconomyMarketWriteKind::OpenPositionTransaction;
    write.position = std::move(value);
    write.circulationEvents.push_back(std::move(acquired));
    uint64 const token = QueueWrite(std::move(write), leaseId, true);
    return {true, 0u, token};
}

EconomyPositionMutationResult PlayerbotEconomyMarket::StagePosition(EconomyPosition value, uint64 itemGuid,
                                                                    uint32 auctionId, uint64 leaseId, uint64 now)
{
    std::scoped_lock lock(mutex);
    if (!persistenceHealthy || value.publicId.empty() || !value.traderGuid || !value.marketId || !value.itemId ||
        value.substitutionGroup.empty() || !value.initialQuantity || value.remainingQuantity != value.initialQuantity ||
        !value.acquisitionCost || !itemGuid || !auctionId || !now || !value.maximumRelistAttempts ||
        !value.cooldownSeconds || value.holdingDeadline <= now || value.state != EconomyPositionState::Pending ||
        value.realizedCost || value.realizedProceeds || value.realizedFees || value.closedAt ||
        value.realizedOutcome != EconomyPositionOutcome::None)
    {
        return {};
    }
    if (std::any_of(positions.begin(), positions.end(),
                    [&value](EconomyPosition const& existing)
                    {
                        return existing.publicId == value.publicId ||
                               (existing.traderGuid == value.traderGuid && existing.marketId == value.marketId);
                    }) ||
        HasPendingWriteLocked(value.publicId) ||
        std::any_of(
            circulation.begin(), circulation.end(), [itemGuid](EconomyCirculation const& event)
            { return event.itemGuid == itemGuid && event.provenance == EconomyCirculationProvenance::Speculative; }))
    {
        return {};
    }

    value.openedAt = now;
    value.updatedAt = now;
    value.relistAttempts = 0u;
    EconomyCirculation pending{
        .positionPublicId = value.publicId,
        .itemGuid = itemGuid,
        .quantity = value.remainingQuantity,
        .auctionId = auctionId,
        .provenance = EconomyCirculationProvenance::Speculative,
        .state = EconomyCirculationState::Pending,
        .occurredAt = now,
    };
    positions.push_back(value);
    circulation.push_back(pending);
    RebuildControlledItemIndex();

    EconomyMarketWrite write;
    write.kind = EconomyMarketWriteKind::OpenPositionTransaction;
    write.position = std::move(value);
    write.circulationEvents.push_back(std::move(pending));
    uint64 const token = QueueWrite(std::move(write), leaseId, false);
    return {true, 0u, token};
}

EconomyPositionMutationResult PlayerbotEconomyMarket::ActivatePendingPosition(std::string const& positionPublicId,
                                                                              uint64 leaseId, uint64 now)
{
    std::scoped_lock lock(mutex);
    auto const existing =
        std::find_if(positions.begin(), positions.end(),
                     [&positionPublicId](EconomyPosition const& value) { return value.publicId == positionPublicId; });
    if (!persistenceHealthy || existing == positions.end() || existing->state != EconomyPositionState::Pending ||
        !now || HasPendingWriteLocked(positionPublicId))
    {
        return {};
    }
    auto const pending = std::find_if(
        circulation.rbegin(), circulation.rend(), [&positionPublicId](EconomyCirculation const& event)
        { return event.positionPublicId == positionPublicId && event.state == EconomyCirculationState::Pending; });
    if (pending == circulation.rend())
        return {};

    EconomyPosition updated = *existing;
    updated.state = EconomyPositionState::Open;
    updated.updatedAt = updated.updatedAt == UINT64_MAX ? UINT64_MAX : std::max(now, updated.updatedAt + 1u);
    EconomyCirculation acquired = *pending;
    acquired.state = EconomyCirculationState::Acquired;
    acquired.occurredAt = updated.updatedAt;
    circulation.push_back(acquired);
    *existing = updated;
    RebuildControlledItemIndex();

    EconomyMarketWrite write;
    write.kind = EconomyMarketWriteKind::UpdatePositionTransaction;
    write.position = std::move(updated);
    write.circulationEvents.push_back(std::move(acquired));
    uint64 const token = QueueWrite(std::move(write), leaseId, true);
    return {true, 0u, token};
}

uint64 PlayerbotEconomyMarket::CancelPendingPosition(std::string const& positionPublicId, uint64 leaseId)
{
    std::scoped_lock lock(mutex);
    auto const existing =
        std::find_if(positions.begin(), positions.end(),
                     [&positionPublicId](EconomyPosition const& value) { return value.publicId == positionPublicId; });
    if (!persistenceHealthy || existing == positions.end() || existing->state != EconomyPositionState::Pending ||
        HasPendingWriteLocked(positionPublicId))
    {
        return 0u;
    }

    EconomyMarketWrite write;
    write.kind = EconomyMarketWriteKind::DeletePendingPosition;
    write.position = *existing;
    positions.erase(existing);
    std::erase_if(circulation, [&positionPublicId](EconomyCirculation const& event)
                  { return event.positionPublicId == positionPublicId; });
    RebuildControlledItemIndex();
    return QueueWrite(std::move(write), leaseId, false);
}

bool PlayerbotEconomyMarket::HasPendingWrite(std::string const& positionPublicId) const
{
    std::scoped_lock lock(mutex);
    return HasPendingWriteLocked(positionPublicId);
}

bool PlayerbotEconomyMarket::HasPendingWriteLocked(std::string const& positionPublicId) const
{
    return std::any_of(pendingWrites.begin(), pendingWrites.end(), [&positionPublicId](PendingWrite const& write)
                       { return write.positionPublicId == positionPublicId; });
}

EconomyPositionMutationResult PlayerbotEconomyMarket::ApplyPositionEvent(EconomyPositionEvent const& event,
                                                                         uint64 leaseId, bool irreversibleCommitted)
{
    std::scoped_lock lock(mutex);
    auto const existing = std::find_if(positions.begin(), positions.end(), [&event](EconomyPosition const& value)
                                       { return value.publicId == event.positionPublicId; });
    if (!persistenceHealthy || existing == positions.end() || !event.itemGuid || !event.quantity || !event.occurredAt ||
        existing->state == EconomyPositionState::Pending || event.quantity > existing->remainingQuantity ||
        HasPendingWriteLocked(event.positionPublicId))
    {
        return {};
    }

    EconomyPosition updated = *existing;
    uint64 const effectiveTime =
        updated.updatedAt == UINT64_MAX ? UINT64_MAX : std::max(event.occurredAt, updated.updatedAt + 1u);
    std::vector<EconomyCirculation> events;
    auto const append = [&](uint64 itemGuid, uint32 quantity, uint32 auctionId, EconomyCirculationState state)
    {
        events.push_back({
            .positionPublicId = updated.publicId,
            .itemGuid = itemGuid,
            .quantity = quantity,
            .auctionId = auctionId,
            .provenance = EconomyCirculationProvenance::Speculative,
            .state = state,
            .occurredAt = effectiveTime,
        });
    };

    bool terminal = false;
    EconomyPositionOutcome outcome = EconomyPositionOutcome::None;
    EconomyCooldownCause cooldownCause = EconomyCooldownCause::Loss;
    switch (event.kind)
    {
        case EconomyPositionEventKind::Split:
            if (!event.replacementItemGuid || !event.replacementQuantity ||
                static_cast<uint64>(event.quantity) + event.replacementQuantity > updated.remainingQuantity)
            {
                return {};
            }
            append(event.itemGuid, event.quantity, 0u, EconomyCirculationState::Acquired);
            append(event.replacementItemGuid, event.replacementQuantity, 0u, EconomyCirculationState::Acquired);
            break;
        case EconomyPositionEventKind::Merge:
            if (!event.replacementItemGuid || !event.replacementQuantity ||
                event.replacementQuantity > updated.remainingQuantity)
            {
                return {};
            }
            append(event.itemGuid, event.quantity, 0u, EconomyCirculationState::Merged);
            append(event.replacementItemGuid, event.replacementQuantity, 0u, EconomyCirculationState::Acquired);
            break;
        case EconomyPositionEventKind::Listed:
            updated.state = EconomyPositionState::Listed;
            append(event.itemGuid, event.quantity, event.auctionId, EconomyCirculationState::Listed);
            break;
        case EconomyPositionEventKind::Relisted:
            if (updated.relistAttempts >= updated.maximumRelistAttempts)
                return {};
            ++updated.relistAttempts;
            updated.state = EconomyPositionState::Listed;
            append(event.itemGuid, event.quantity, event.auctionId, EconomyCirculationState::Listed);
            break;
        case EconomyPositionEventKind::Expired:
            if (UINT64_MAX - updated.realizedFees < event.fees)
                return {};
            updated.realizedFees += event.fees;
            updated.state = EconomyPositionState::Open;
            cooldownCause = EconomyCooldownCause::Expired;
            append(event.itemGuid, event.quantity, event.auctionId, EconomyCirculationState::Delivered);
            break;
        case EconomyPositionEventKind::Sold:
            terminal = true;
            outcome = EconomyPositionOutcome::Sale;
            append(event.itemGuid, event.quantity, event.auctionId, EconomyCirculationState::Delivered);
            break;
        case EconomyPositionEventKind::Used:
            terminal = true;
            outcome = EconomyPositionOutcome::Use;
            append(event.itemGuid, event.quantity, 0u, EconomyCirculationState::Consumed);
            break;
        case EconomyPositionEventKind::Transformed:
            terminal = true;
            outcome = EconomyPositionOutcome::Transformation;
            append(event.itemGuid, event.quantity, 0u, EconomyCirculationState::Transformed);
            break;
        case EconomyPositionEventKind::Vendored:
            terminal = true;
            outcome = EconomyPositionOutcome::Vendor;
            append(event.itemGuid, event.quantity, 0u, EconomyCirculationState::Vendored);
            break;
        case EconomyPositionEventKind::Lost:
            terminal = true;
            outcome = EconomyPositionOutcome::Loss;
            cooldownCause = EconomyCooldownCause::Loss;
            append(event.itemGuid, event.quantity, 0u, EconomyCirculationState::Lost);
            break;
    }

    uint64 releasedCost = 0u;
    if (terminal)
    {
        if (UINT64_MAX - updated.realizedProceeds < event.proceeds || UINT64_MAX - updated.realizedFees < event.fees)
        {
            return {};
        }
        releasedCost = static_cast<uint64>(static_cast<unsigned __int128>(updated.acquisitionCost) * event.quantity /
                                           updated.remainingQuantity);
        updated.remainingQuantity -= event.quantity;
        updated.acquisitionCost -= releasedCost;
        updated.realizedCost += releasedCost;
        updated.realizedProceeds += event.proceeds;
        updated.realizedFees += event.fees;
        if (!updated.remainingQuantity)
        {
            updated.state =
                outcome == EconomyPositionOutcome::Loss ? EconomyPositionState::Lost : EconomyPositionState::Closed;
            updated.closedAt = effectiveTime;
            updated.realizedOutcome = outcome;
        }
        else
            updated.state = EconomyPositionState::Open;
    }
    updated.updatedAt = effectiveTime;

    EconomyMarketWrite write;
    write.kind = updated.remainingQuantity ? EconomyMarketWriteKind::UpdatePositionTransaction
                                           : EconomyMarketWriteKind::ClosePositionTransaction;
    write.position = updated;
    write.circulationEvents = events;
    if (updated.cooldownSeconds &&
        (event.kind == EconomyPositionEventKind::Expired || event.kind == EconomyPositionEventKind::Lost))
    {
        write.hasCooldown = true;
        write.cooldown = {
            .traderGuid = updated.traderGuid,
            .marketId = updated.marketId,
            .substitutionGroup = updated.substitutionGroup,
            .cause = cooldownCause,
            .nextEligibleAt = effectiveTime > UINT64_MAX - updated.cooldownSeconds
                                  ? UINT64_MAX
                                  : effectiveTime + updated.cooldownSeconds,
        };
        auto const current = std::find_if(cooldowns.begin(), cooldowns.end(),
                                          [&write](EconomyCooldown const& value)
                                          {
                                              return value.traderGuid == write.cooldown.traderGuid &&
                                                     value.marketId == write.cooldown.marketId &&
                                                     value.substitutionGroup == write.cooldown.substitutionGroup;
                                          });
        if (current == cooldowns.end())
            cooldowns.push_back(write.cooldown);
        else
            *current = write.cooldown;
    }

    circulation.insert(circulation.end(), events.begin(), events.end());
    if (updated.remainingQuantity)
        *existing = updated;
    else
        positions.erase(existing);
    RebuildControlledItemIndex();
    uint64 const token = QueueWrite(std::move(write), leaseId, irreversibleCommitted);
    return {true, releasedCost, token};
}

std::vector<EconomyPositionReconciliation> PlayerbotEconomyMarket::Restore(EconomyMarketStartup startup, uint64 now)
{
    std::scoped_lock lock(mutex);
    ++generation;
    evidence.clear();
    positions.clear();
    cooldowns.clear();
    circulation.clear();
    pendingWrites.clear();
    persistenceFailures.clear();
    persistenceHealthy = true;

    for (EconomyPriceEvidence& value : startup.evidence)
    {
        if (IsValid(value) && value.expiresAt > now)
            evidence.push_back(std::move(value));
    }
    PruneEvidence(now);

    std::map<std::string, uint64> backedQuantity;
    for (EconomyPositionBacking const& value : startup.backing)
        backedQuantity[value.positionPublicId] += value.quantity;

    std::vector<EconomyPositionReconciliation> reconciliations;
    for (EconomyPosition& value : startup.positions)
    {
        if (!IsValid(value))
            continue;
        if (value.state == EconomyPositionState::Pending)
        {
            positions.push_back(std::move(value));
            continue;
        }

        uint32 const backed =
            static_cast<uint32>(std::min<uint64>(backedQuantity[value.publicId], value.remainingQuantity));
        if (backed == value.remainingQuantity)
        {
            positions.push_back(std::move(value));
            continue;
        }

        EconomyPositionReconciliation reconciliation;
        reconciliation.publicId = value.publicId;
        reconciliation.remainingQuantity = backed;
        reconciliation.acquisitionCost =
            value.remainingQuantity == 0u ? 0u
                                          : static_cast<uint64>(static_cast<unsigned __int128>(value.acquisitionCost) *
                                                                backed / value.remainingQuantity);
        reconciliation.state = backed == 0u ? EconomyPositionState::Lost : value.state;
        reconciliations.push_back(reconciliation);
        uint64 const releasedCost = value.acquisitionCost - reconciliation.acquisitionCost;

        EconomyMarketWrite write;
        write.kind = backed == 0u ? EconomyMarketWriteKind::ClosePosition : EconomyMarketWriteKind::UpdatePosition;
        write.position = value;
        write.position.remainingQuantity = backed;
        write.position.acquisitionCost = reconciliation.acquisitionCost;
        write.position.realizedCost += releasedCost;
        write.position.state = reconciliation.state;
        write.position.updatedAt = now;
        if (backed == 0u)
        {
            write.position.closedAt = now;
            write.position.realizedOutcome = EconomyPositionOutcome::Loss;
        }
        static_cast<void>(QueueWrite(std::move(write), 0u, true));

        if (backed == 0u)
            continue;

        value.remainingQuantity = backed;
        value.acquisitionCost = reconciliation.acquisitionCost;
        value.realizedCost += releasedCost;
        positions.push_back(std::move(value));
    }

    for (EconomyCooldown& value : startup.cooldowns)
    {
        if (value.traderGuid != 0u && value.marketId != 0u && !value.substitutionGroup.empty() &&
            value.nextEligibleAt > now)
            cooldowns.push_back(std::move(value));
    }

    for (EconomyCirculation& value : startup.circulation)
    {
        if (!value.positionPublicId.empty() && value.itemGuid != 0u && value.quantity != 0u && value.occurredAt != 0u)
            circulation.push_back(std::move(value));
    }
    RebuildControlledItemIndex();

    return reconciliations;
}

std::optional<EconomyReferencePrice> PlayerbotEconomyMarket::ReferencePrice(uint32 marketId,
                                                                            std::string const& substitutionGroup,
                                                                            uint64 now) const
{
    std::scoped_lock lock(mutex);
    std::vector<EconomyPriceEvidence const*> candidates;
    for (EconomyPriceEvidence const& value : evidence)
    {
        if (value.marketId == marketId && value.substitutionGroup == substitutionGroup && value.expiresAt > now &&
            (value.source == EconomyEvidenceSource::Sale || value.source == EconomyEvidenceSource::Listing))
            candidates.push_back(&value);
    }
    return BuildReference(candidates);
}

EconomyMarketSnapshot PlayerbotEconomyMarket::Snapshot(uint64 now) const
{
    std::scoped_lock lock(mutex);
    EconomyMarketSnapshot snapshot;
    snapshot.generation = generation;
    std::copy_if(evidence.begin(), evidence.end(), std::back_inserter(snapshot.evidence),
                 [now](EconomyPriceEvidence const& value) { return value.expiresAt > now; });
    std::map<EvidenceKey, std::vector<EconomyPriceEvidence const*>> groupedEvidence;
    for (EconomyPriceEvidence const& value : snapshot.evidence)
    {
        if (value.source == EconomyEvidenceSource::Sale || value.source == EconomyEvidenceSource::Listing)
            groupedEvidence[{value.marketId, value.substitutionGroup}].push_back(&value);
    }
    for (auto const& [key, values] : groupedEvidence)
    {
        if (std::optional<EconomyReferencePrice> const reference = BuildReference(values))
            snapshot.references.push_back({key.first, key.second, *reference});
    }
    snapshot.positions = positions;
    std::copy_if(cooldowns.begin(), cooldowns.end(), std::back_inserter(snapshot.cooldowns),
                 [now](EconomyCooldown const& value) { return value.nextEligibleAt > now; });
    snapshot.circulation = circulation;
    snapshot.persistenceFailures = persistenceFailures;
    snapshot.persistenceHealthy = persistenceHealthy;
    snapshot.persistenceBlocker =
        persistenceHealthy ? EconomyMarketBlocker::None : EconomyMarketBlocker::PersistenceUnavailable;
    return snapshot;
}

uint64 PlayerbotEconomyMarket::AppendEvidence(EconomyPriceEvidence value, uint64 leaseId, bool irreversibleCommitted)
{
    std::scoped_lock lock(mutex);
    if (!persistenceHealthy || !IsValid(value))
        return 0u;

    auto const existing = value.auctionId ? std::find_if(evidence.begin(), evidence.end(),
                                                         [&value](EconomyPriceEvidence const& candidate)
                                                         {
                                                             return candidate.marketId == value.marketId &&
                                                                    candidate.auctionId == value.auctionId &&
                                                                    candidate.source == value.source;
                                                         })
                                          : evidence.end();
    if (existing == evidence.end())
        evidence.push_back(value);
    else
    {
        if (existing->itemId == value.itemId && existing->substitutionGroup == value.substitutionGroup &&
            existing->unitPrice == value.unitPrice && existing->quantity == value.quantity &&
            existing->positionPublicId == value.positionPublicId)
        {
            return 0u;
        }
        *existing = value;
    }
    PruneEvidence(value.observedAt);
    EconomyMarketWrite write;
    write.kind = EconomyMarketWriteKind::AppendEvidence;
    write.evidence = std::move(value);
    return QueueWrite(std::move(write), leaseId, irreversibleCommitted);
}

uint64 PlayerbotEconomyMarket::SavePosition(EconomyPosition value, uint64 leaseId, bool irreversibleCommitted)
{
    std::scoped_lock lock(mutex);
    if (!persistenceHealthy || value.publicId.empty())
        return 0u;

    auto const existing = std::find_if(positions.begin(), positions.end(), [&value](EconomyPosition const& candidate)
                                       { return candidate.publicId == value.publicId; });
    EconomyMarketWrite write;
    write.kind = existing == positions.end()
                     ? EconomyMarketWriteKind::OpenPosition
                     : (IsOpen(value) ? EconomyMarketWriteKind::UpdatePosition : EconomyMarketWriteKind::ClosePosition);
    write.position = value;
    if (existing == positions.end())
        positions.push_back(std::move(value));
    else if (IsOpen(value))
        *existing = std::move(value);
    else
        positions.erase(existing);
    RebuildControlledItemIndex();
    return QueueWrite(std::move(write), leaseId, irreversibleCommitted);
}

uint64 PlayerbotEconomyMarket::SaveCooldown(EconomyCooldown value, uint64 leaseId, bool irreversibleCommitted)
{
    std::scoped_lock lock(mutex);
    if (!persistenceHealthy)
        return 0u;

    auto const existing = std::find_if(cooldowns.begin(), cooldowns.end(),
                                       [&value](EconomyCooldown const& candidate)
                                       {
                                           return candidate.traderGuid == value.traderGuid &&
                                                  candidate.marketId == value.marketId &&
                                                  candidate.substitutionGroup == value.substitutionGroup;
                                       });
    EconomyMarketWrite write;
    write.kind = EconomyMarketWriteKind::SaveCooldown;
    write.cooldown = value;
    if (existing == cooldowns.end())
        cooldowns.push_back(std::move(value));
    else
        *existing = std::move(value);
    return QueueWrite(std::move(write), leaseId, irreversibleCommitted);
}

uint64 PlayerbotEconomyMarket::AppendCirculation(EconomyCirculation value, uint64 leaseId, bool irreversibleCommitted)
{
    std::scoped_lock lock(mutex);
    if (!persistenceHealthy || value.positionPublicId.empty() || value.itemGuid == 0u || value.quantity == 0u ||
        value.occurredAt == 0u)
        return 0u;

    circulation.push_back(value);
    RebuildControlledItemIndex();
    EconomyMarketWrite write;
    write.kind = EconomyMarketWriteKind::AppendCirculation;
    write.circulation = std::move(value);
    return QueueWrite(std::move(write), leaseId, irreversibleCommitted);
}

void PlayerbotEconomyMarket::CompleteWrite(uint64 writeToken, bool success)
{
    std::scoped_lock lock(mutex);
    auto const pending = std::find_if(pendingWrites.begin(), pendingWrites.end(),
                                      [writeToken](PendingWrite const& value) { return value.token == writeToken; });
    if (pending == pendingWrites.end())
        return;

    ++generation;

    if (!success)
    {
        persistenceHealthy = false;
        persistenceFailures.push_back({
            .writeToken = pending->token,
            .leaseId = pending->leaseId,
            .claimDisposition = pending->irreversibleCommitted ? EconomyClaimDisposition::RetainCommitted
                                                               : EconomyClaimDisposition::ReleaseUncommitted,
        });
        if (!pending->position.publicId.empty())
        {
            auto const existing =
                std::find_if(positions.begin(), positions.end(), [&pending](EconomyPosition const& value)
                             { return value.publicId == pending->position.publicId; });
            if (existing == positions.end())
                positions.push_back(pending->position);
            else
                *existing = pending->position;
            RebuildControlledItemIndex();
        }
    }
    pendingWrites.erase(pending);
}

uint64 PlayerbotEconomyMarket::QueueWrite(EconomyMarketWrite write, uint64 leaseId, bool irreversibleCommitted)
{
    ++generation;
    uint64 const token = nextWriteToken++;
    pendingWrites.push_back({token, leaseId, irreversibleCommitted, write.position.publicId, write.position});
    if (writer)
        writer(token, write);
    return token;
}

void PlayerbotEconomyMarket::PruneEvidence(uint64 now)
{
    std::erase_if(evidence, [now](EconomyPriceEvidence const& value) { return value.expiresAt <= now; });
    std::map<EvidenceKey, std::vector<std::size_t>> indexes;
    for (std::size_t i = 0; i < evidence.size(); ++i)
        indexes[{evidence[i].marketId, evidence[i].substitutionGroup}].push_back(i);

    std::vector<bool> keep(evidence.size(), true);
    for (auto& [key, groupIndexes] : indexes)
    {
        static_cast<void>(key);
        std::sort(groupIndexes.begin(), groupIndexes.end(), [this](std::size_t left, std::size_t right)
                  { return evidence[left].observedAt > evidence[right].observedAt; });
        for (std::size_t i = MAX_EVIDENCE_PER_GROUP; i < groupIndexes.size(); ++i)
            keep[groupIndexes[i]] = false;
    }

    std::vector<EconomyPriceEvidence> bounded;
    bounded.reserve(evidence.size());
    for (std::size_t i = 0; i < evidence.size(); ++i)
    {
        if (keep[i])
            bounded.push_back(std::move(evidence[i]));
    }
    evidence = std::move(bounded);
}

void PlayerbotEconomyMarket::RebuildControlledItemIndex()
{
    controlledItems.clear();
    std::unordered_map<std::string, std::pair<uint32, uint32>> positionOwners;
    for (EconomyPosition const& position : positions)
        positionOwners[position.publicId] = {position.traderGuid, position.marketId};

    for (EconomyCirculation const& event : circulation)
    {
        auto const owner = positionOwners.find(event.positionPublicId);
        if (owner == positionOwners.end() || !event.itemGuid ||
            event.provenance != EconomyCirculationProvenance::Speculative)
        {
            continue;
        }
        controlledItems[owner->second].push_back(event.itemGuid);
    }

    for (auto& [owner, itemGuids] : controlledItems)
    {
        (void)owner;
        std::ranges::sort(itemGuids);
        itemGuids.erase(std::unique(itemGuids.begin(), itemGuids.end()), itemGuids.end());
    }
}

PlayerbotEconomyMarket& GetPlayerbotEconomyMarket()
{
    static PlayerbotEconomyMarket market(QueueDatabaseWrite);
    return market;
}

void LoadPlayerbotEconomyMarketFromDatabase(uint64 now)
{
    EconomyMarketStartup startup;

    if (QueryResult result = PlayerbotsDatabase.Query(Acore::StringFormat(
            "SELECT market_id, item_id, substitution_group, source, COALESCE(auction_id, 0), unit_price, "
            "quantity, UNIX_TIMESTAMP(observed_at), UNIX_TIMESTAMP(expires_at), "
            "COALESCE(position_public_id, '') FROM playerbot_economy_price_evidence "
            "WHERE expires_at > NOW() ORDER BY observed_at DESC LIMIT {}",
            MAX_STARTUP_EVIDENCE)))
    {
        do
        {
            Field* fields = result->Fetch();
            std::optional<EconomyEvidenceSource> const source = ParseEvidenceSource(fields[3].Get<std::string>());
            if (!source.has_value())
                continue;
            startup.evidence.push_back({
                .marketId = fields[0].Get<uint32>(),
                .itemId = fields[1].Get<uint32>(),
                .substitutionGroup = fields[2].Get<std::string>(),
                .source = *source,
                .auctionId = fields[4].Get<uint32>(),
                .unitPrice = fields[5].Get<uint64>(),
                .quantity = fields[6].Get<uint32>(),
                .observedAt = fields[7].Get<uint64>(),
                .expiresAt = fields[8].Get<uint64>(),
                .positionPublicId = fields[9].Get<std::string>(),
            });
        } while (result->NextRow());
    }

    if (QueryResult result = PlayerbotsDatabase.Query(Acore::StringFormat(
            "SELECT public_id, trader_guid, market_id, item_id, substitution_group, initial_quantity, "
            "remaining_quantity, acquisition_cost, realized_cost, realized_proceeds, realized_fees, state, "
            "relist_attempts, maximum_relist_attempts, cooldown_seconds, UNIX_TIMESTAMP(opened_at), "
            "UNIX_TIMESTAMP(holding_deadline), UNIX_TIMESTAMP(updated_at) FROM playerbot_economy_position "
            "WHERE state IN ('pending', 'open', 'listed') ORDER BY opened_at LIMIT {}",
            MAX_STARTUP_POSITIONS)))
    {
        do
        {
            Field* fields = result->Fetch();
            std::optional<EconomyPositionState> const state = ParsePositionState(fields[11].Get<std::string>());
            if (!state.has_value())
                continue;
            startup.positions.push_back({
                .publicId = fields[0].Get<std::string>(),
                .traderGuid = fields[1].Get<uint32>(),
                .marketId = fields[2].Get<uint32>(),
                .itemId = fields[3].Get<uint32>(),
                .substitutionGroup = fields[4].Get<std::string>(),
                .initialQuantity = fields[5].Get<uint32>(),
                .remainingQuantity = fields[6].Get<uint32>(),
                .acquisitionCost = fields[7].Get<uint64>(),
                .realizedCost = fields[8].Get<uint64>(),
                .realizedProceeds = fields[9].Get<uint64>(),
                .realizedFees = fields[10].Get<uint64>(),
                .state = *state,
                .relistAttempts = fields[12].Get<uint8>(),
                .maximumRelistAttempts = fields[13].Get<uint8>(),
                .cooldownSeconds = fields[14].Get<uint32>(),
                .openedAt = fields[15].Get<uint64>(),
                .holdingDeadline = fields[16].Get<uint64>(),
                .updatedAt = fields[17].Get<uint64>(),
            });
        } while (result->NextRow());
    }

    if (QueryResult result = PlayerbotsDatabase.Query(Acore::StringFormat(
            "SELECT trader_guid, market_id, substitution_group, cause, UNIX_TIMESTAMP(next_eligible_at) "
            "FROM playerbot_economy_cooldown WHERE next_eligible_at > NOW() "
            "ORDER BY next_eligible_at LIMIT {}",
            MAX_STARTUP_COOLDOWNS)))
    {
        do
        {
            Field* fields = result->Fetch();
            std::optional<EconomyCooldownCause> const cause = ParseCooldownCause(fields[3].Get<std::string>());
            if (!cause.has_value())
                continue;
            startup.cooldowns.push_back({
                .traderGuid = fields[0].Get<uint32>(),
                .marketId = fields[1].Get<uint32>(),
                .substitutionGroup = fields[2].Get<std::string>(),
                .cause = *cause,
                .nextEligibleAt = fields[4].Get<uint64>(),
            });
        } while (result->NextRow());
    }

    if (QueryResult result = PlayerbotsDatabase.Query(Acore::StringFormat(
            "SELECT c.position_public_id, c.item_guid, c.quantity, c.auction_id, c.provenance, c.state, "
            "UNIX_TIMESTAMP(c.occurred_at) FROM playerbot_economy_circulation c "
            "INNER JOIN playerbot_economy_position p ON p.public_id = c.position_public_id "
            "WHERE c.auction_id IS NOT NULL AND c.provenance <> 'ordinary' "
            "AND p.state IN ('closed', 'lost') ORDER BY c.occurred_at DESC, c.id DESC LIMIT {}",
            MAX_STARTUP_CONTROLLED_AUCTIONS)))
    {
        do
        {
            Field* fields = result->Fetch();
            std::optional<EconomyCirculationProvenance> const provenance =
                ParseCirculationProvenance(fields[4].Get<std::string>());
            std::optional<EconomyCirculationState> const state = ParseCirculationState(fields[5].Get<std::string>());
            if (!provenance.has_value() || !state.has_value())
                continue;
            startup.circulation.push_back({
                .positionPublicId = fields[0].Get<std::string>(),
                .itemGuid = fields[1].Get<uint64>(),
                .quantity = fields[2].Get<uint32>(),
                .auctionId = fields[3].Get<uint32>(),
                .provenance = *provenance,
                .state = *state,
                .occurredAt = fields[6].Get<uint64>(),
            });
        } while (result->NextRow());
    }

    std::unordered_map<uint64, uint32> remainingPhysicalQuantity;
    for (EconomyPosition const& position : startup.positions)
    {
        QueryResult circulation = PlayerbotsDatabase.Query(Acore::StringFormat(
            "SELECT item_guid, quantity, auction_id, provenance, state, UNIX_TIMESTAMP(occurred_at) "
            "FROM playerbot_economy_circulation WHERE position_public_id = {} "
            "ORDER BY occurred_at DESC, id DESC LIMIT {}",
            SqlString(position.publicId), MAX_CIRCULATION_PER_POSITION));
        if (!circulation)
            continue;

        std::unordered_map<uint64, uint32> latestPhysicalLots;
        do
        {
            Field* fields = circulation->Fetch();
            uint64 const itemGuid = fields[0].Get<uint64>();
            std::optional<EconomyCirculationProvenance> const provenance =
                ParseCirculationProvenance(fields[3].Get<std::string>());
            std::optional<EconomyCirculationState> const state = ParseCirculationState(fields[4].Get<std::string>());
            if (!provenance.has_value() || !state.has_value())
                continue;

            startup.circulation.push_back({
                .positionPublicId = position.publicId,
                .itemGuid = itemGuid,
                .quantity = fields[1].Get<uint32>(),
                .auctionId = fields[2].Get<uint32>(),
                .provenance = *provenance,
                .state = *state,
                .occurredAt = fields[5].Get<uint64>(),
            });
            if (latestPhysicalLots.contains(itemGuid))
                continue;
            latestPhysicalLots[itemGuid] = *state == EconomyCirculationState::Acquired ||
                                                   *state == EconomyCirculationState::Listed ||
                                                   *state == EconomyCirculationState::Delivered
                                               ? fields[1].Get<uint32>()
                                               : 0u;
        } while (circulation->NextRow());

        for (auto const& [itemGuid, recordedQuantity] : latestPhysicalLots)
        {
            if (recordedQuantity == 0u || itemGuid > UINT32_MAX)
                continue;

            auto const known = remainingPhysicalQuantity.find(itemGuid);
            if (known == remainingPhysicalQuantity.end())
            {
                QueryResult backing = CharacterDatabase.Query(Acore::StringFormat(
                    "SELECT ii.count FROM item_instance ii WHERE ii.guid = {} AND ii.itemEntry = {} AND ("
                    "EXISTS (SELECT 1 FROM character_inventory ci WHERE ci.item = ii.guid AND ci.guid = {}) OR "
                    "EXISTS (SELECT 1 FROM mail_items mi INNER JOIN mail m ON m.id = mi.mail_id "
                    "WHERE mi.item_guid = ii.guid AND m.receiver = {}) OR "
                    "EXISTS (SELECT 1 FROM auctionhouse ah WHERE ah.itemguid = ii.guid AND ah.itemowner = {})) "
                    "LIMIT 1",
                    static_cast<uint32>(itemGuid), position.itemId, position.traderGuid, position.traderGuid,
                    position.traderGuid));
                remainingPhysicalQuantity[itemGuid] = backing ? backing->Fetch()[0].Get<uint32>() : 0u;
            }

            uint32& available = remainingPhysicalQuantity[itemGuid];
            uint32 const assigned = std::min(recordedQuantity, available);
            if (assigned == 0u)
                continue;
            startup.backing.push_back({position.publicId, assigned});
            available -= assigned;
        }
    }

    std::size_t const restoredEvidence = startup.evidence.size();
    std::size_t const restoredPositions = startup.positions.size();
    std::vector<EconomyPositionReconciliation> const reconciliations =
        GetPlayerbotEconomyMarket().Restore(std::move(startup), now);
    LOG_INFO("playerbots.economy", "Loaded {} market observations and {} open positions; reconciled {} positions.",
             restoredEvidence, restoredPositions, reconciliations.size());
}

void UpdatePlayerbotEconomyMarketDatabaseCallbacks() { EconomyTransactionProcessor().ProcessReadyCallbacks(); }
}  // namespace PlayerbotEconomy
