/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYMARKET_H
#define PLAYERBOTS_PLAYERBOTECONOMYMARKET_H

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Define.h"

namespace PlayerbotEconomy
{
enum class EconomyEvidenceSource : uint8
{
    Sale,
    Listing,
    Recovery,
    Speculation
};

enum class EconomyPositionState : uint8
{
    Pending,
    Open,
    Listed,
    Closed,
    Lost
};

enum class EconomyPositionOutcome : uint8
{
    None,
    Sale,
    Use,
    Transformation,
    Vendor,
    Loss
};

enum class EconomyCooldownCause : uint8
{
    Loss,
    FailedPurchase,
    FailedListing,
    Expired
};

enum class EconomyCirculationProvenance : uint8
{
    Ordinary,
    Speculative,
    Recovery
};

enum class EconomyCirculationState : uint8
{
    Pending,
    Acquired,
    Listed,
    Delivered,
    Merged,
    Consumed,
    Transformed,
    Vendored,
    Lost
};

enum class EconomyMarketBlocker : uint8
{
    None,
    PersistenceUnavailable
};

enum class EconomyClaimDisposition : uint8
{
    ReleaseUncommitted,
    RetainCommitted
};

enum class EconomyRiskBlocker : uint8
{
    None,
    Disabled,
    MissingExposure,
    InvalidPercentage,
    InvalidConcentration,
    InvalidEvidenceMinimum,
    InvalidHoldingHorizon,
    InvalidRelistAttempts,
    InvalidCooldown,
    AffinityTooLow,
    InsufficientEvidence,
    GroupExposure,
    TotalExposure,
    SameAccountPurchase,
    UnconfidentEvidence,
    NotUnderpriced,
    ExpectedLoss,
    Cooldown,
    ExistingPosition,
    AlreadySpeculated
};

struct EconomyRiskConfiguration
{
    bool enabled = false;
    uint32 perGroupExposurePercent = 0;
    uint32 totalExposurePercent = 0;
    uint32 minimumEvidence = 0;
    uint32 holdingHorizonSeconds = 0;
    uint32 maximumRelistAttempts = 0;
    uint32 cooldownSeconds = 0;
};

struct EconomyRiskFacts
{
    uint8 economyAffinity = 0;
    uint64 freeTradeskillMoney = 0;
    uint64 groupExposure = 0;
    uint64 totalExposure = 0;
    uint32 qualifiedEvidence = 0;
    uint64 proposedCost = 0;
};

struct EconomyRiskDecision
{
    EconomyRiskBlocker blocker = EconomyRiskBlocker::Disabled;
    uint64 perGroupExposureLimit = 0;
    uint64 totalExposureLimit = 0;
};

struct EconomyMarketEntryFacts
{
    EconomyRiskFacts risk;
    uint32 traderGuid = 0;
    uint32 buyerAccountId = 0;
    uint32 sellerAccountId = 0;
    uint32 marketId = 0;
    uint32 itemId = 0;
    std::string substitutionGroup;
    uint64 itemGuid = 0;
    uint32 quantity = 0;
    uint64 buyout = 0;
    uint64 referenceUnitPrice = 0;
    bool referenceConfident = false;
    uint64 depositPerListing = 0;
    uint32 auctionCutBasisPoints = 0;
    uint64 now = 0;
};

struct EconomyPriceEvidence
{
    uint32 marketId = 0;
    uint32 itemId = 0;
    std::string substitutionGroup;
    EconomyEvidenceSource source = EconomyEvidenceSource::Listing;
    uint32 auctionId = 0;
    uint64 unitPrice = 0;
    uint32 quantity = 0;
    uint64 observedAt = 0;
    uint64 expiresAt = 0;
    std::string positionPublicId;

    bool operator==(EconomyPriceEvidence const&) const = default;
};

struct EconomyPosition
{
    std::string publicId;
    uint32 traderGuid = 0;
    uint32 marketId = 0;
    uint32 itemId = 0;
    std::string substitutionGroup;
    uint32 initialQuantity = 0;
    uint32 remainingQuantity = 0;
    uint64 acquisitionCost = 0;
    uint64 realizedCost = 0;
    uint64 realizedProceeds = 0;
    uint64 realizedFees = 0;
    EconomyPositionState state = EconomyPositionState::Open;
    uint8 relistAttempts = 0;
    uint8 maximumRelistAttempts = 0;
    uint32 cooldownSeconds = 0;
    uint64 openedAt = 0;
    uint64 holdingDeadline = 0;
    uint64 updatedAt = 0;
    uint64 closedAt = 0;
    EconomyPositionOutcome realizedOutcome = EconomyPositionOutcome::None;

    bool operator==(EconomyPosition const&) const = default;
};

struct EconomyCooldown
{
    uint32 traderGuid = 0;
    uint32 marketId = 0;
    std::string substitutionGroup;
    EconomyCooldownCause cause = EconomyCooldownCause::Loss;
    uint64 nextEligibleAt = 0;

    bool operator==(EconomyCooldown const&) const = default;
};

struct EconomyCirculation
{
    std::string positionPublicId;
    uint64 itemGuid = 0;
    uint32 quantity = 0;
    uint32 auctionId = 0;
    EconomyCirculationProvenance provenance = EconomyCirculationProvenance::Ordinary;
    EconomyCirculationState state = EconomyCirculationState::Acquired;
    uint64 occurredAt = 0;

    bool operator==(EconomyCirculation const&) const = default;
};

enum class EconomyPositionEventKind : uint8
{
    Split,
    Merge,
    Listed,
    Relisted,
    Expired,
    Sold,
    Used,
    Transformed,
    Vendored,
    Lost
};

struct EconomyPositionEvent
{
    EconomyPositionEventKind kind = EconomyPositionEventKind::Split;
    std::string positionPublicId;
    uint64 itemGuid = 0;
    uint64 replacementItemGuid = 0;
    uint32 quantity = 0;
    uint32 replacementQuantity = 0;
    uint32 auctionId = 0;
    uint64 proceeds = 0;
    uint64 fees = 0;
    uint64 occurredAt = 0;
    uint32 cooldownSeconds = 0;
};

struct EconomyPositionMutationResult
{
    bool accepted = false;
    uint64 releasedCost = 0;
    uint64 writeToken = 0;
};

struct EconomyPositionBacking
{
    std::string positionPublicId;
    uint32 quantity = 0;
};

struct EconomyPositionReconciliation
{
    std::string publicId;
    uint32 remainingQuantity = 0;
    uint64 acquisitionCost = 0;
    EconomyPositionState state = EconomyPositionState::Open;
};

struct EconomyReferencePrice
{
    uint64 unitPrice = 0;
    uint32 acceptedSales = 0;
    uint32 acceptedListings = 0;
    bool confident = false;

    bool operator==(EconomyReferencePrice const&) const = default;
};

struct EconomyMarketReference
{
    uint32 marketId = 0;
    std::string substitutionGroup;
    EconomyReferencePrice price;

    bool operator==(EconomyMarketReference const&) const = default;
};

enum class EconomyMarketWriteKind : uint8
{
    AppendEvidence,
    OpenPosition,
    UpdatePosition,
    ClosePosition,
    AppendCirculation,
    SaveCooldown,
    OpenPositionTransaction,
    UpdatePositionTransaction,
    ClosePositionTransaction,
    DeletePendingPosition
};

struct EconomyMarketWrite
{
    EconomyMarketWriteKind kind = EconomyMarketWriteKind::AppendEvidence;
    EconomyPriceEvidence evidence;
    EconomyPosition position;
    EconomyCirculation circulation;
    EconomyCooldown cooldown;
    std::vector<EconomyCirculation> circulationEvents;
    bool hasCooldown = false;
};

struct EconomyPersistenceFailure
{
    uint64 writeToken = 0;
    uint64 leaseId = 0;
    EconomyClaimDisposition claimDisposition = EconomyClaimDisposition::ReleaseUncommitted;
};

struct EconomyMarketStartup
{
    std::vector<EconomyPriceEvidence> evidence;
    std::vector<EconomyPosition> positions;
    std::vector<EconomyCooldown> cooldowns;
    std::vector<EconomyCirculation> circulation;
    std::vector<EconomyPositionBacking> backing;
};

struct EconomyMarketSnapshot
{
    uint64 generation = 0;
    std::vector<EconomyPriceEvidence> evidence;
    std::vector<EconomyMarketReference> references;
    std::vector<EconomyPosition> positions;
    std::vector<EconomyCooldown> cooldowns;
    std::vector<EconomyCirculation> circulation;
    std::vector<EconomyPersistenceFailure> persistenceFailures;
    EconomyMarketBlocker persistenceBlocker = EconomyMarketBlocker::None;
    bool persistenceHealthy = true;
};

class PlayerbotEconomyMarket
{
public:
    static constexpr uint32 MAX_EVIDENCE_PER_GROUP = 64u;
    using AsyncWriter = std::function<void(uint64, EconomyMarketWrite const&)>;

    explicit PlayerbotEconomyMarket(AsyncWriter writer = {});

    [[nodiscard]] std::vector<EconomyPositionReconciliation> Restore(EconomyMarketStartup startup, uint64 now);
    [[nodiscard]] std::optional<EconomyReferencePrice> ReferencePrice(uint32 marketId,
                                                                      std::string const& substitutionGroup,
                                                                      uint64 now) const;
    [[nodiscard]] EconomyMarketSnapshot Snapshot(uint64 now) const;
    [[nodiscard]] std::vector<uint64> ControlledItemGuids(uint32 traderGuid, uint32 marketId) const;
    [[nodiscard]] static EconomyRiskDecision EvaluateRisk(EconomyRiskConfiguration const& configuration,
                                                          EconomyRiskFacts const& facts);
    [[nodiscard]] EconomyRiskDecision EvaluateEntry(EconomyRiskConfiguration const& configuration,
                                                    EconomyMarketEntryFacts const& facts) const;
    [[nodiscard]] static char const* RiskBlockerName(EconomyRiskBlocker blocker);
    [[nodiscard]] EconomyPositionMutationResult OpenPosition(EconomyPosition position, uint64 itemGuid, uint64 leaseId,
                                                             uint64 now);
    [[nodiscard]] EconomyPositionMutationResult StagePosition(EconomyPosition position, uint64 itemGuid,
                                                              uint32 auctionId, uint64 leaseId, uint64 now);
    [[nodiscard]] EconomyPositionMutationResult ActivatePendingPosition(std::string const& positionPublicId,
                                                                        uint64 leaseId, uint64 now);
    [[nodiscard]] uint64 CancelPendingPosition(std::string const& positionPublicId, uint64 leaseId);
    [[nodiscard]] bool HasPendingWrite(std::string const& positionPublicId) const;
    [[nodiscard]] EconomyPositionMutationResult ApplyPositionEvent(EconomyPositionEvent const& event, uint64 leaseId,
                                                                   bool irreversibleCommitted);

    [[nodiscard]] uint64 AppendEvidence(EconomyPriceEvidence evidence, uint64 leaseId, bool irreversibleCommitted);
    [[nodiscard]] uint64 SavePosition(EconomyPosition position, uint64 leaseId, bool irreversibleCommitted);
    [[nodiscard]] uint64 SaveCooldown(EconomyCooldown cooldown, uint64 leaseId, bool irreversibleCommitted);
    [[nodiscard]] uint64 AppendCirculation(EconomyCirculation value, uint64 leaseId, bool irreversibleCommitted);
    void CompleteWrite(uint64 writeToken, bool success);

private:
    struct PendingWrite
    {
        uint64 token = 0;
        uint64 leaseId = 0;
        bool irreversibleCommitted = false;
        std::string positionPublicId;
        EconomyPosition position;
    };

    [[nodiscard]] uint64 QueueWrite(EconomyMarketWrite write, uint64 leaseId, bool irreversibleCommitted);
    [[nodiscard]] bool HasPendingWriteLocked(std::string const& positionPublicId) const;
    void PruneEvidence(uint64 now);
    void RebuildControlledItemIndex();

    AsyncWriter writer;
    std::vector<EconomyPriceEvidence> evidence;
    std::vector<EconomyPosition> positions;
    std::vector<EconomyCooldown> cooldowns;
    std::vector<EconomyCirculation> circulation;
    std::vector<PendingWrite> pendingWrites;
    std::vector<EconomyPersistenceFailure> persistenceFailures;
    std::map<std::pair<uint32, uint32>, std::vector<uint64>> controlledItems;
    mutable std::mutex mutex;
    uint64 nextWriteToken = 1u;
    uint64 generation = 0u;
    bool persistenceHealthy = true;
};

PlayerbotEconomyMarket& GetPlayerbotEconomyMarket();
void LoadPlayerbotEconomyMarketFromDatabase(uint64 now);
void UpdatePlayerbotEconomyMarketDatabaseCallbacks();
}  // namespace PlayerbotEconomy

#endif
