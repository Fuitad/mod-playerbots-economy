/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTAUTHORITY_H
#define PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTAUTHORITY_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace PlayerbotEconomy
{
enum class MaterialCapacityKind : std::uint8_t
{
    OwnedItem,
    AuctionListing,
    Money,
    GatheringCapacity,
    ProductionCapacity
};

enum class MaterialCapacityUnit : std::uint8_t
{
    ItemUnits,
    Copper,
    GatheringUnits,
    ProductionUnits
};

struct MaterialCapacityKey
{
    MaterialCapacityKind kind = MaterialCapacityKind::OwnedItem;
    std::string authorityIdentity;

    bool operator==(MaterialCapacityKey const&) const = default;
};

struct MaterialRequirement
{
    std::uint32_t itemId = 0u;
    std::uint32_t quantity = 0u;

    bool operator==(MaterialRequirement const&) const = default;
};

enum class MaterialCommitmentOwnerKind : std::uint8_t
{
    ProfessionProgression,
    StockMaintenance,
    SupplyRemediation,
    ActivityCritical,
    GroupCommitment
};

struct MaterialIntent
{
    std::string originIdentity;
    MaterialCommitmentOwnerKind ownerKind = MaterialCommitmentOwnerKind::ProfessionProgression;
    std::uint64_t ownerRevision = 0u;
    std::uint32_t marketId = 0u;
    std::uint32_t boundedQuantity = 0u;
    std::optional<std::uint64_t> neededBy;
    std::uint64_t firstObservedAt = 0u;
    std::uint64_t lastObservedAt = 0u;
    std::vector<MaterialRequirement> requirements;

    bool operator==(MaterialIntent const&) const = default;
};

struct MaterialCapacityObservation
{
    MaterialCapacityKey capacity;
    MaterialCapacityUnit unit = MaterialCapacityUnit::ItemUnits;
    std::uint32_t materialItemId = 0u;
    std::uint64_t authorityRevision = 0u;
    std::uint64_t availableQuantity = 0u;

    bool operator==(MaterialCapacityObservation const&) const = default;
};

struct MaterialReservationRequest
{
    std::uint32_t materialItemId = 0u;
    MaterialCapacityKey capacity;
    std::uint64_t authorityRevision = 0u;
    std::uint64_t backedMaterialQuantity = 0u;
    std::uint64_t capacityQuantity = 0u;

    bool operator==(MaterialReservationRequest const&) const = default;
};

struct MaterialAdmissionCandidate
{
    std::string originIdentity;
    std::uint64_t ownerRevision = 0u;
    std::vector<MaterialReservationRequest> reservations;

    bool operator==(MaterialAdmissionCandidate const&) const = default;
};

struct MaterialReservationSettlement
{
    MaterialCapacityKey capacity;
    std::uint64_t backedMaterialQuantity = 0u;
    std::uint64_t capacityQuantity = 0u;

    bool operator==(MaterialReservationSettlement const&) const = default;
};

struct MaterialFulfillment
{
    std::string commitmentIdentity;
    std::uint32_t quantity = 0u;
    std::vector<MaterialReservationSettlement> reservationSettlements;

    bool operator==(MaterialFulfillment const&) const = default;
};

enum class MaterialCommitmentState : std::uint8_t
{
    Admitted,
    PartiallyFulfilled,
    Completed,
    Released,
    Superseded
};

struct MaterialReservation
{
    std::uint32_t materialItemId = 0u;
    MaterialCapacityKey capacity;
    MaterialCapacityUnit unit = MaterialCapacityUnit::ItemUnits;
    std::uint64_t authorityRevision = 0u;
    std::uint64_t initialBackedMaterialQuantity = 0u;
    std::uint64_t remainingBackedMaterialQuantity = 0u;
    std::uint64_t initialCapacityQuantity = 0u;
    std::uint64_t remainingCapacityQuantity = 0u;

    bool operator==(MaterialReservation const&) const = default;
};

struct MaterialCommitment
{
    std::string identity;
    std::string originIdentity;
    MaterialCommitmentOwnerKind ownerKind = MaterialCommitmentOwnerKind::ProfessionProgression;
    std::uint64_t ownerRevision = 0u;
    std::uint32_t marketId = 0u;
    std::uint32_t materialItemId = 0u;
    std::uint32_t boundedQuantity = 0u;
    std::uint32_t remainingQuantity = 0u;
    std::uint64_t neededBy = 0u;
    MaterialCommitmentState state = MaterialCommitmentState::Admitted;
    std::vector<MaterialReservation> reservations;

    bool operator==(MaterialCommitment const&) const = default;
};

struct MaterialCommitmentOperation
{
    std::string identity;
    std::string fingerprint;
    std::uint64_t resultingBookRevision = 0u;
    std::vector<std::string> commitmentIdentities;

    bool operator==(MaterialCommitmentOperation const&) const = default;
};

struct MaterialCommitmentStartup
{
    bool sourceAvailable = false;
    std::uint64_t bookRevision = 0u;
    std::vector<MaterialIntent> intents;
    std::vector<MaterialCommitment> commitments;
    std::vector<MaterialCommitmentOperation> operations;

    bool operator==(MaterialCommitmentStartup const&) const = default;
};

enum class MaterialCommitmentCommandKind : std::uint8_t
{
    Observe,
    Admit,
    Fulfill,
    Release,
    Supersede
};

struct MaterialCommitmentCommand
{
    std::string operationIdentity;
    std::uint64_t expectedBookRevision = 0u;
    MaterialCommitmentCommandKind kind = MaterialCommitmentCommandKind::Observe;
    std::vector<MaterialIntent> intents;
    std::vector<MaterialAdmissionCandidate> candidates;
    std::vector<MaterialCapacityObservation> capacityObservations;
    std::vector<MaterialFulfillment> fulfillments;
    std::vector<std::string> commitmentIdentities;

    bool operator==(MaterialCommitmentCommand const&) const = default;
};

enum class MaterialCommitmentApplyStatus : std::uint8_t
{
    PendingPersistence,
    Idempotent,
    Busy,
    PersistenceUnavailable,
    InvalidCommand,
    StaleBookRevision,
    StaleOwnerRevision,
    StaleCapacityRevision,
    MissingHorizon,
    InsufficientCapacity,
    ExistingCommitment,
    UnknownIntent,
    UnknownCommitment,
    TerminalCommitment,
    OperationConflict,
    IdentityCollision
};

struct MaterialCommitmentApplyResult
{
    MaterialCommitmentApplyStatus status = MaterialCommitmentApplyStatus::InvalidCommand;
    std::uint64_t writeToken = 0u;
    std::vector<std::string> commitmentIdentities;
};

struct MaterialCommitmentWrite
{
    std::uint64_t expectedBookRevision = 0u;
    std::uint64_t newBookRevision = 0u;
    std::vector<std::string> changedOriginIdentities;
    std::vector<std::string> changedCommitmentIdentities;
    MaterialCommitmentOperation operation;
    MaterialCommitmentStartup replacement;
};

struct MaterialCommitmentSnapshot
{
    std::uint64_t bookRevision = 0u;
    bool persistenceHealthy = false;
    bool busy = false;
    std::vector<MaterialIntent> intents;
    std::vector<MaterialCommitment> commitments;
};

class PlayerbotMaterialCommitmentAuthority
{
public:
    using AsyncWriter = std::function<void(std::uint64_t, MaterialCommitmentWrite const&)>;

    explicit PlayerbotMaterialCommitmentAuthority(AsyncWriter writer);

    [[nodiscard]] bool Restore(MaterialCommitmentStartup startup);
    [[nodiscard]] MaterialCommitmentApplyResult Apply(MaterialCommitmentCommand command, std::uint64_t now);
    void CompleteWrite(std::uint64_t writeToken, bool success);
    [[nodiscard]] MaterialCommitmentSnapshot Snapshot() const;

private:
    struct PendingWrite
    {
        std::uint64_t token = 0u;
        MaterialCommitmentStartup replacement;
    };

    AsyncWriter writer;
    MaterialCommitmentStartup durable;
    std::optional<PendingWrite> pending;
    mutable std::mutex mutex;
    std::uint64_t nextWriteToken = 1u;
    bool persistenceHealthy = false;
};

PlayerbotMaterialCommitmentAuthority& GetPlayerbotMaterialCommitmentAuthority();
void LoadPlayerbotMaterialCommitmentsFromDatabase();
void UpdatePlayerbotMaterialCommitmentDatabaseCallbacks();
}  // namespace PlayerbotEconomy

#endif
