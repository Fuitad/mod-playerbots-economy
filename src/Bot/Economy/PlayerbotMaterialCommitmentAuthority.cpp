/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "Bot/Economy/PlayerbotMaterialCommitmentEncoding.h"

namespace PlayerbotEconomy
{
namespace
{
using CapacityTuple = std::tuple<MaterialCapacityKind, std::string>;

CapacityTuple CapacityKey(MaterialCapacityKey const& value) { return {value.kind, value.authorityIdentity}; }

bool CheckedAdd(std::uint64_t& total, std::uint64_t value)
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
        return false;
    total += value;
    return true;
}

bool IsActive(MaterialCommitmentState state)
{
    return state == MaterialCommitmentState::Admitted || state == MaterialCommitmentState::PartiallyFulfilled;
}

bool ValidSelectedSourcePath(MaterialSourcePath const& path)
{
    if (path.phase != MaterialSourcePhase::Selected || path.startingInventoryQuantity != 0u)
        return false;
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const rebuilt =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath({
            .actorGuid = path.actorGuid,
            .materialItemId = path.materialItemId,
            .selectedQuantity = path.selectedQuantity,
            .gatheringSkillId = path.gatheringSkillId,
            .sourceEntry = path.sourceEntry,
            .sourceMapId = path.sourceMapId,
            .routeIdentity = path.routeIdentity,
            .capacityIdentity = path.capacityIdentity,
            .selectedAt = path.selectedAt,
            .sourceTravelBudgetSeconds = path.sourceTravelBudgetSeconds,
            .destinationConservativeYieldBasisPoints = path.destinationYieldBasisPoints,
            .observedGatheredQuantity = path.observedGatheredQuantity,
            .observedResourceAttempts = path.observedResourceAttempts,
            .observedResourceSeconds = path.observedResourceSeconds,
            .authoritativeInteractionSeconds = path.authoritativeInteractionSeconds,
            .remainingDedicatedActivitySeconds = path.remainingDedicatedActivitySeconds,
            .deliveryTravelBudgetSeconds = path.deliveryTravelBudgetSeconds,
            .completionObservationBudgetSeconds = path.completionObservationBudgetSeconds,
            .startingInventoryQuantity = path.startingInventoryQuantity,
            .availableResourceCount = path.availableResourceCount,
        });
    return rebuilt.status == MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path && rebuilt.path &&
           *rebuilt.path == path;
}

bool ValidSourcePath(MaterialSourcePath const& path, MaterialCommitment const& commitment)
{
    MaterialSourcePath selected = path;
    selected.phase = MaterialSourcePhase::Selected;
    selected.startingInventoryQuantity = 0u;
    if (!ValidSelectedSourcePath(selected) || path.materialItemId != commitment.materialItemId ||
        path.selectedQuantity != commitment.boundedQuantity || path.neededBy != commitment.neededBy)
    {
        return false;
    }
    if (IsActive(commitment.state))
    {
        if (commitment.state != MaterialCommitmentState::Admitted || commitment.reservations.size() != 1u)
            return false;
        MaterialReservation const& reservation = commitment.reservations.front();
        return (path.phase == MaterialSourcePhase::Selected || path.phase == MaterialSourcePhase::Acquiring) &&
               reservation.materialItemId == path.materialItemId &&
               reservation.capacity.kind == MaterialCapacityKind::GatheringCapacity &&
               reservation.capacity.authorityIdentity == path.capacityIdentity &&
               reservation.authorityRevision == path.sourceRevision &&
               reservation.initialBackedMaterialQuantity == path.selectedQuantity &&
               reservation.remainingBackedMaterialQuantity == path.selectedQuantity &&
               reservation.initialCapacityQuantity == path.requiredResourceCount &&
               reservation.remainingCapacityQuantity == path.requiredResourceCount;
    }
    if (commitment.state == MaterialCommitmentState::Completed)
        return path.phase == MaterialSourcePhase::Completed;
    return path.phase == MaterialSourcePhase::Released;
}

bool ValidRequirements(std::vector<MaterialRequirement> const& requirements)
{
    if (requirements.empty())
        return false;

    std::set<std::uint32_t> itemIds;
    return std::ranges::all_of(requirements,
                               [&itemIds](MaterialRequirement const& requirement) {
                                   return requirement.itemId != 0u && requirement.quantity != 0u &&
                                          itemIds.insert(requirement.itemId).second;
                               });
}

bool ValidIntent(MaterialIntent const& intent, bool restored)
{
    if (intent.originIdentity.empty() ||
        intent.originIdentity.size() > MaterialCommitmentEncoding::MAX_IDENTITY_BYTES || intent.ownerRevision == 0u ||
        intent.marketId == 0u || intent.boundedQuantity == 0u || !ValidRequirements(intent.requirements))
    {
        return false;
    }
    if (!restored)
        return intent.firstObservedAt == 0u && intent.lastObservedAt == 0u;
    return intent.firstObservedAt != 0u && intent.lastObservedAt >= intent.firstObservedAt;
}

MaterialCapacityUnit UnitForKind(MaterialCapacityKind kind)
{
    switch (kind)
    {
        case MaterialCapacityKind::OwnedItem:
        case MaterialCapacityKind::AuctionListing:
            return MaterialCapacityUnit::ItemUnits;
        case MaterialCapacityKind::Money:
            return MaterialCapacityUnit::Copper;
        case MaterialCapacityKind::GatheringCapacity:
            return MaterialCapacityUnit::GatheringUnits;
        case MaterialCapacityKind::ProductionCapacity:
            return MaterialCapacityUnit::ProductionUnits;
    }
    return MaterialCapacityUnit::Copper;
}

bool IsItemUnitKind(MaterialCapacityKind kind)
{
    return kind == MaterialCapacityKind::OwnedItem || kind == MaterialCapacityKind::AuctionListing;
}

bool SameIntentFacts(MaterialIntent const& left, MaterialIntent const& right)
{
    return left.originIdentity == right.originIdentity && left.ownerRevision == right.ownerRevision &&
           left.ownerKind == right.ownerKind && left.marketId == right.marketId &&
           left.boundedQuantity == right.boundedQuantity && left.neededBy == right.neededBy &&
           left.requirements == right.requirements;
}

MaterialIntent const* FindIntent(MaterialCommitmentStartup const& state, std::string const& identity)
{
    auto const found = std::ranges::find(state.intents, identity, &MaterialIntent::originIdentity);
    return found == state.intents.end() ? nullptr : &*found;
}

MaterialIntent* FindIntent(MaterialCommitmentStartup& state, std::string const& identity)
{
    auto const found = std::ranges::find(state.intents, identity, &MaterialIntent::originIdentity);
    return found == state.intents.end() ? nullptr : &*found;
}

MaterialCommitment* FindCommitment(MaterialCommitmentStartup& state, std::string const& identity)
{
    auto const found = std::ranges::find(state.commitments, identity, &MaterialCommitment::identity);
    return found == state.commitments.end() ? nullptr : &*found;
}

MaterialCommitment const* FindCommitment(MaterialCommitmentStartup const& state, std::string const& identity)
{
    auto const found = std::ranges::find(state.commitments, identity, &MaterialCommitment::identity);
    return found == state.commitments.end() ? nullptr : &*found;
}

MaterialCommitmentOperation const* FindOperation(MaterialCommitmentStartup const& state, std::string const& identity)
{
    auto const found = std::ranges::find(state.operations, identity, &MaterialCommitmentOperation::identity);
    return found == state.operations.end() ? nullptr : &*found;
}

bool ValidCommitment(MaterialCommitment const& commitment, MaterialCommitmentStartup const& startup)
{
    MaterialIntent const* intent = FindIntent(startup, commitment.originIdentity);
    if (commitment.identity.empty() || !intent || commitment.ownerRevision == 0u || commitment.marketId == 0u ||
        commitment.materialItemId == 0u || commitment.boundedQuantity == 0u || commitment.neededBy == 0u)
    {
        return false;
    }

    if (IsActive(commitment.state))
    {
        auto const requirement =
            std::ranges::find(intent->requirements, commitment.materialItemId, &MaterialRequirement::itemId);
        if (commitment.ownerKind != intent->ownerKind || commitment.ownerRevision != intent->ownerRevision ||
            commitment.marketId != intent->marketId || !intent->neededBy.has_value() ||
            commitment.neededBy != *intent->neededBy || requirement == intent->requirements.end() ||
            requirement->quantity != commitment.boundedQuantity)
        {
            return false;
        }
        if (commitment.remainingQuantity == 0u || commitment.remainingQuantity > commitment.boundedQuantity ||
            commitment.reservations.empty())
        {
            return false;
        }
        if (commitment.state == MaterialCommitmentState::Admitted &&
            commitment.remainingQuantity != commitment.boundedQuantity)
        {
            return false;
        }
        if (commitment.state == MaterialCommitmentState::PartiallyFulfilled &&
            commitment.remainingQuantity >= commitment.boundedQuantity)
        {
            return false;
        }
    }
    else if (commitment.remainingQuantity != 0u || !commitment.reservations.empty())
        return false;

    if (commitment.sourcePath && !ValidSourcePath(*commitment.sourcePath, commitment))
        return false;

    std::uint64_t reserved = 0u;
    std::set<CapacityTuple> capacities;
    for (MaterialReservation const& reservation : commitment.reservations)
    {
        bool const money = reservation.capacity.kind == MaterialCapacityKind::Money;
        bool const itemUnits = IsItemUnitKind(reservation.capacity.kind);
        if (reservation.materialItemId != commitment.materialItemId || reservation.capacity.authorityIdentity.empty() ||
            reservation.capacity.authorityIdentity.size() > MaterialCommitmentEncoding::MAX_IDENTITY_BYTES ||
            reservation.unit != UnitForKind(reservation.capacity.kind) || reservation.authorityRevision == 0u ||
            reservation.initialCapacityQuantity == 0u || reservation.remainingCapacityQuantity == 0u ||
            reservation.remainingCapacityQuantity > reservation.initialCapacityQuantity ||
            reservation.remainingBackedMaterialQuantity > reservation.initialBackedMaterialQuantity ||
            (money &&
             (reservation.unit != MaterialCapacityUnit::Copper || reservation.initialBackedMaterialQuantity != 0u)) ||
            (itemUnits && (reservation.initialBackedMaterialQuantity != reservation.initialCapacityQuantity ||
                           reservation.remainingBackedMaterialQuantity != reservation.remainingCapacityQuantity)) ||
            (!money &&
             (reservation.initialBackedMaterialQuantity == 0u || reservation.remainingBackedMaterialQuantity == 0u)) ||
            !capacities.insert(CapacityKey(reservation.capacity)).second)
        {
            return false;
        }
        if (!CheckedAdd(reserved, reservation.remainingBackedMaterialQuantity))
            return false;
    }
    return !IsActive(commitment.state) || reserved == commitment.remainingQuantity;
}

bool ValidStartup(MaterialCommitmentStartup const& startup)
{
    if (!startup.sourceAvailable)
        return false;

    std::set<std::string> origins;
    for (MaterialIntent const& intent : startup.intents)
    {
        if (!ValidIntent(intent, true) || !origins.insert(intent.originIdentity).second)
            return false;
    }

    std::set<std::string> identities;
    std::set<std::pair<std::string, std::uint32_t>> activeRequirements;
    std::map<CapacityTuple, std::uint64_t> activeCapacity;
    for (MaterialCommitment const& commitment : startup.commitments)
    {
        if (!ValidCommitment(commitment, startup) || !identities.insert(commitment.identity).second)
            return false;
        if (IsActive(commitment.state) &&
            !activeRequirements.insert({commitment.originIdentity, commitment.materialItemId}).second)
        {
            return false;
        }
        if (IsActive(commitment.state))
        {
            for (MaterialReservation const& reservation : commitment.reservations)
            {
                if (!CheckedAdd(activeCapacity[CapacityKey(reservation.capacity)],
                                reservation.remainingCapacityQuantity))
                {
                    return false;
                }
            }
        }
    }

    if (startup.operations.size() != startup.bookRevision)
        return false;
    std::set<std::string> operationIds;
    std::set<std::uint64_t> operationRevisions;
    for (MaterialCommitmentOperation const& operation : startup.operations)
    {
        if (operation.identity.empty() || operation.identity.size() > MaterialCommitmentEncoding::MAX_IDENTITY_BYTES ||
            operation.fingerprint.empty() ||
            operation.fingerprint.size() > MaterialCommitmentEncoding::MAX_FINGERPRINT_BYTES ||
            operation.resultingBookRevision == 0u || operation.resultingBookRevision > startup.bookRevision ||
            !operationIds.insert(operation.identity).second)
        {
            return false;
        }
        if (!operationRevisions.insert(operation.resultingBookRevision).second)
            return false;
        if (!std::ranges::all_of(operation.commitmentIdentities, [&startup](std::string const& identity)
                                 { return FindCommitment(startup, identity) != nullptr; }))
        {
            return false;
        }
    }
    return true;
}

bool HasActiveOrigin(MaterialCommitmentStartup const& state, std::string const& originIdentity)
{
    return std::ranges::any_of(state.commitments, [&originIdentity](MaterialCommitment const& commitment)
                               { return commitment.originIdentity == originIdentity && IsActive(commitment.state); });
}

MaterialCommitmentApplyStatus ObserveIntents(MaterialCommitmentStartup& state, MaterialCommitmentCommand const& command,
                                             std::uint64_t now)
{
    if (command.intents.empty() || !command.candidates.empty() || !command.capacityObservations.empty() ||
        !command.sourceStarts.empty() || !command.fulfillments.empty() || !command.commitmentIdentities.empty() ||
        now == 0u)
    {
        return MaterialCommitmentApplyStatus::InvalidCommand;
    }

    std::set<std::string> commandOrigins;
    for (MaterialIntent intent : command.intents)
    {
        if (!ValidIntent(intent, false) || !commandOrigins.insert(intent.originIdentity).second)
            return MaterialCommitmentApplyStatus::InvalidCommand;

        auto existing = std::ranges::find(state.intents, intent.originIdentity, &MaterialIntent::originIdentity);
        if (existing != state.intents.end())
        {
            if (intent.ownerRevision < existing->ownerRevision ||
                (intent.ownerRevision == existing->ownerRevision &&
                 (intent.ownerKind != existing->ownerKind || intent.marketId != existing->marketId ||
                  intent.boundedQuantity != existing->boundedQuantity || intent.neededBy != existing->neededBy ||
                  intent.requirements != existing->requirements)))
            {
                return MaterialCommitmentApplyStatus::StaleOwnerRevision;
            }
            if (HasActiveOrigin(state, intent.originIdentity) && !SameIntentFacts(intent, *existing))
                return MaterialCommitmentApplyStatus::ExistingCommitment;
            intent.firstObservedAt = existing->firstObservedAt;
            intent.lastObservedAt = now;
            *existing = std::move(intent);
        }
        else
        {
            intent.firstObservedAt = now;
            intent.lastObservedAt = now;
            state.intents.push_back(std::move(intent));
        }
    }
    return MaterialCommitmentApplyStatus::PendingPersistence;
}

bool ValidObservation(MaterialCapacityObservation const& observation)
{
    if (observation.capacity.authorityIdentity.empty() ||
        observation.capacity.authorityIdentity.size() > MaterialCommitmentEncoding::MAX_IDENTITY_BYTES ||
        observation.authorityRevision == 0u || observation.unit != UnitForKind(observation.capacity.kind))
        return false;
    switch (observation.capacity.kind)
    {
        case MaterialCapacityKind::OwnedItem:
        case MaterialCapacityKind::AuctionListing:
            return observation.materialItemId != 0u;
        case MaterialCapacityKind::Money:
            return observation.materialItemId == 0u;
        case MaterialCapacityKind::GatheringCapacity:
            return observation.materialItemId != 0u;
        case MaterialCapacityKind::ProductionCapacity:
            return observation.materialItemId != 0u;
    }
    return false;
}

MaterialCommitmentApplyStatus ValidateAdmissionFacts(MaterialCommitmentStartup const& state,
                                                     MaterialCommitmentCommand const& command, std::uint64_t now)
{
    if (command.candidates.empty() || !command.intents.empty() || !command.sourceStarts.empty() ||
        !command.fulfillments.empty())
        return MaterialCommitmentApplyStatus::InvalidCommand;

    std::set<std::string> origins;
    std::map<CapacityTuple, MaterialCapacityObservation const*> observations;
    for (MaterialCapacityObservation const& observation : command.capacityObservations)
    {
        if (!ValidObservation(observation) ||
            !observations.emplace(CapacityKey(observation.capacity), &observation).second)
        {
            return MaterialCommitmentApplyStatus::InvalidCommand;
        }
    }

    std::map<CapacityTuple, std::uint64_t> newlyReserved;
    for (MaterialAdmissionCandidate const& candidate : command.candidates)
    {
        if (candidate.originIdentity.empty() || !origins.insert(candidate.originIdentity).second)
            return MaterialCommitmentApplyStatus::InvalidCommand;
        MaterialIntent const* intent = FindIntent(state, candidate.originIdentity);
        if (!intent)
            return MaterialCommitmentApplyStatus::UnknownIntent;
        if (candidate.ownerRevision != intent->ownerRevision)
            return MaterialCommitmentApplyStatus::StaleOwnerRevision;
        if (candidate.sourcePaths.empty())
        {
            if (!intent->neededBy.has_value() || *intent->neededBy <= now)
                return MaterialCommitmentApplyStatus::MissingHorizon;
        }
        else
        {
            if (candidate.sourcePaths.size() != 1u || intent->requirements.size() != 1u ||
                candidate.reservations.size() != 1u || !ValidSelectedSourcePath(candidate.sourcePaths.front()))
            {
                return MaterialCommitmentApplyStatus::InvalidCommand;
            }
            MaterialSourcePath const& path = candidate.sourcePaths.front();
            MaterialRequirement const& requirement = intent->requirements.front();
            MaterialReservationRequest const& reservation = candidate.reservations.front();
            if (path.materialItemId != requirement.itemId || path.selectedQuantity != requirement.quantity ||
                path.neededBy <= now || (intent->neededBy.has_value() && *intent->neededBy != path.neededBy) ||
                reservation.materialItemId != path.materialItemId ||
                reservation.capacity.kind != MaterialCapacityKind::GatheringCapacity ||
                reservation.capacity.authorityIdentity != path.capacityIdentity ||
                reservation.authorityRevision != path.sourceRevision ||
                reservation.backedMaterialQuantity != path.selectedQuantity ||
                reservation.capacityQuantity != path.requiredResourceCount)
            {
                return MaterialCommitmentApplyStatus::InvalidCommand;
            }
        }
        if (HasActiveOrigin(state, candidate.originIdentity))
            return MaterialCommitmentApplyStatus::ExistingCommitment;

        std::map<std::uint32_t, std::uint64_t> materialBacking;
        std::set<std::pair<std::uint32_t, CapacityTuple>> candidateReservations;
        for (MaterialReservationRequest const& reservation : candidate.reservations)
        {
            if (reservation.materialItemId == 0u || reservation.capacity.authorityIdentity.empty() ||
                reservation.authorityRevision == 0u || reservation.capacityQuantity == 0u ||
                !candidateReservations.insert({reservation.materialItemId, CapacityKey(reservation.capacity)}).second)
            {
                return MaterialCommitmentApplyStatus::InvalidCommand;
            }
            auto const requirement =
                std::ranges::find(intent->requirements, reservation.materialItemId, &MaterialRequirement::itemId);
            if (requirement == intent->requirements.end())
                return MaterialCommitmentApplyStatus::InvalidCommand;
            if (!CheckedAdd(materialBacking[reservation.materialItemId], reservation.backedMaterialQuantity))
                return MaterialCommitmentApplyStatus::InvalidCommand;
        }
        for (MaterialRequirement const& requirement : intent->requirements)
        {
            std::uint64_t const backed = materialBacking[requirement.itemId];
            if (backed < requirement.quantity)
                return MaterialCommitmentApplyStatus::InsufficientCapacity;
            if (backed > requirement.quantity)
                return MaterialCommitmentApplyStatus::InvalidCommand;
        }

        for (MaterialReservationRequest const& reservation : candidate.reservations)
        {
            auto const observation = observations.find(CapacityKey(reservation.capacity));
            if (observation == observations.end() ||
                observation->second->authorityRevision != reservation.authorityRevision)
                return MaterialCommitmentApplyStatus::StaleCapacityRevision;
            bool const money = observation->second->capacity.kind == MaterialCapacityKind::Money;
            bool const itemUnits = IsItemUnitKind(observation->second->capacity.kind);
            if ((money && reservation.backedMaterialQuantity != 0u) ||
                (itemUnits && reservation.backedMaterialQuantity != reservation.capacityQuantity) ||
                (!money && (reservation.backedMaterialQuantity == 0u ||
                            observation->second->materialItemId != reservation.materialItemId)))
            {
                return MaterialCommitmentApplyStatus::InvalidCommand;
            }
            if (!CheckedAdd(newlyReserved[CapacityKey(reservation.capacity)], reservation.capacityQuantity))
                return MaterialCommitmentApplyStatus::InvalidCommand;
            if (!candidate.sourcePaths.empty() &&
                observation->second->availableQuantity != candidate.sourcePaths.front().availableResourceCount)
            {
                return MaterialCommitmentApplyStatus::StaleCapacityRevision;
            }
        }
    }

    std::map<CapacityTuple, std::uint64_t> retained;
    for (MaterialCommitment const& commitment : state.commitments)
    {
        if (!IsActive(commitment.state))
            continue;
        for (MaterialReservation const& reservation : commitment.reservations)
        {
            if (!CheckedAdd(retained[CapacityKey(reservation.capacity)], reservation.remainingCapacityQuantity))
                return MaterialCommitmentApplyStatus::InvalidCommand;
        }
    }
    for (auto const& [key, quantity] : newlyReserved)
    {
        MaterialCapacityObservation const* observation = observations.at(key);
        if (retained[key] > observation->availableQuantity || quantity > observation->availableQuantity - retained[key])
        {
            return MaterialCommitmentApplyStatus::InsufficientCapacity;
        }
    }
    return MaterialCommitmentApplyStatus::PendingPersistence;
}

MaterialCommitmentApplyStatus AdmitCandidates(MaterialCommitmentStartup& state,
                                              MaterialCommitmentCommand const& command, std::uint64_t now,
                                              std::vector<std::string>& admittedIdentities)
{
    MaterialCommitmentApplyStatus const validation = ValidateAdmissionFacts(state, command, now);
    if (validation != MaterialCommitmentApplyStatus::PendingPersistence)
        return validation;

    std::size_t ordinal = 0u;
    for (MaterialAdmissionCandidate const& candidate : command.candidates)
    {
        MaterialIntent* intentPointer = FindIntent(state, candidate.originIdentity);
        if (!candidate.sourcePaths.empty())
            intentPointer->neededBy = candidate.sourcePaths.front().neededBy;
        MaterialIntent const& intent = *intentPointer;
        for (MaterialRequirement const& requirement : intent.requirements)
        {
            std::string const identity =
                MaterialCommitmentEncoding::CommitmentIdentity(command.operationIdentity, ordinal++);
            if (FindCommitment(state, identity))
                return MaterialCommitmentApplyStatus::IdentityCollision;

            MaterialCommitment commitment{
                .identity = identity,
                .originIdentity = intent.originIdentity,
                .ownerKind = intent.ownerKind,
                .ownerRevision = intent.ownerRevision,
                .marketId = intent.marketId,
                .materialItemId = requirement.itemId,
                .boundedQuantity = requirement.quantity,
                .remainingQuantity = requirement.quantity,
                .neededBy = *intent.neededBy,
                .state = MaterialCommitmentState::Admitted,
            };
            if (!candidate.sourcePaths.empty())
                commitment.sourcePath = candidate.sourcePaths.front();
            for (MaterialReservationRequest const& reservation : candidate.reservations)
            {
                if (reservation.materialItemId != requirement.itemId)
                    continue;
                commitment.reservations.push_back({
                    .materialItemId = reservation.materialItemId,
                    .capacity = reservation.capacity,
                    .unit = std::ranges::find_if(command.capacityObservations,
                                                 [&reservation](MaterialCapacityObservation const& observation) {
                                                     return CapacityKey(observation.capacity) ==
                                                            CapacityKey(reservation.capacity);
                                                 })
                                ->unit,
                    .authorityRevision = reservation.authorityRevision,
                    .initialBackedMaterialQuantity = reservation.backedMaterialQuantity,
                    .remainingBackedMaterialQuantity = reservation.backedMaterialQuantity,
                    .initialCapacityQuantity = reservation.capacityQuantity,
                    .remainingCapacityQuantity = reservation.capacityQuantity,
                });
            }
            admittedIdentities.push_back(identity);
            state.commitments.push_back(std::move(commitment));
        }
    }
    return MaterialCommitmentApplyStatus::PendingPersistence;
}

MaterialCommitmentApplyStatus StartSources(MaterialCommitmentStartup& state, MaterialCommitmentCommand const& command)
{
    if (command.sourceStarts.empty() || !command.intents.empty() || !command.candidates.empty() ||
        !command.capacityObservations.empty() || !command.fulfillments.empty() || !command.commitmentIdentities.empty())
    {
        return MaterialCommitmentApplyStatus::InvalidCommand;
    }

    std::set<std::string> identities;
    for (MaterialSourceStart const& start : command.sourceStarts)
    {
        if (start.commitmentIdentity.empty() || !start.expectedSourceRevision ||
            !identities.insert(start.commitmentIdentity).second)
        {
            return MaterialCommitmentApplyStatus::InvalidCommand;
        }
        MaterialCommitment* commitment = FindCommitment(state, start.commitmentIdentity);
        if (!commitment)
            return MaterialCommitmentApplyStatus::UnknownCommitment;
        if (!IsActive(commitment->state))
            return MaterialCommitmentApplyStatus::TerminalCommitment;
        if (!commitment->sourcePath || commitment->sourcePath->phase != MaterialSourcePhase::Selected ||
            commitment->sourcePath->sourceRevision != start.expectedSourceRevision)
        {
            return MaterialCommitmentApplyStatus::StaleCapacityRevision;
        }
    }
    for (MaterialSourceStart const& start : command.sourceStarts)
    {
        MaterialCommitment* commitment = FindCommitment(state, start.commitmentIdentity);
        commitment->sourcePath->phase = MaterialSourcePhase::Acquiring;
        commitment->sourcePath->startingInventoryQuantity = start.startingInventoryQuantity;
    }
    return MaterialCommitmentApplyStatus::PendingPersistence;
}

MaterialCommitmentApplyStatus FulfillCommitments(MaterialCommitmentStartup& state,
                                                 MaterialCommitmentCommand const& command)
{
    if (command.fulfillments.empty() || !command.intents.empty() || !command.candidates.empty() ||
        !command.capacityObservations.empty() || !command.sourceStarts.empty() || !command.commitmentIdentities.empty())
    {
        return MaterialCommitmentApplyStatus::InvalidCommand;
    }

    std::set<std::string> identities;
    for (MaterialFulfillment const& fulfillment : command.fulfillments)
    {
        if (fulfillment.commitmentIdentity.empty() || fulfillment.quantity == 0u ||
            !identities.insert(fulfillment.commitmentIdentity).second)
        {
            return MaterialCommitmentApplyStatus::InvalidCommand;
        }
        MaterialCommitment* commitment = FindCommitment(state, fulfillment.commitmentIdentity);
        if (!commitment)
            return MaterialCommitmentApplyStatus::UnknownCommitment;
        if (!IsActive(commitment->state))
            return MaterialCommitmentApplyStatus::TerminalCommitment;
        if (fulfillment.quantity > commitment->remainingQuantity)
            return MaterialCommitmentApplyStatus::InvalidCommand;
        if (commitment->sourcePath)
        {
            MaterialSourcePath const& path = *commitment->sourcePath;
            if (path.phase != MaterialSourcePhase::Acquiring || fulfillment.quantity != commitment->remainingQuantity ||
                !fulfillment.observedInventoryQuantity.has_value() ||
                *fulfillment.observedInventoryQuantity < path.startingInventoryQuantity ||
                *fulfillment.observedInventoryQuantity - path.startingInventoryQuantity < path.selectedQuantity)
            {
                return MaterialCommitmentApplyStatus::InvalidCommand;
            }
        }
        else if (fulfillment.observedInventoryQuantity.has_value())
            return MaterialCommitmentApplyStatus::InvalidCommand;

        std::uint64_t totalRelease = 0u;
        std::set<CapacityTuple> releases;
        for (MaterialReservationSettlement const& release : fulfillment.reservationSettlements)
        {
            if (release.capacity.authorityIdentity.empty() || release.capacityQuantity == 0u ||
                !releases.insert(CapacityKey(release.capacity)).second)
            {
                return MaterialCommitmentApplyStatus::InvalidCommand;
            }
            auto const reservation =
                std::ranges::find_if(commitment->reservations, [&release](MaterialReservation const& value)
                                     { return CapacityKey(value.capacity) == CapacityKey(release.capacity); });
            if (reservation == commitment->reservations.end() ||
                release.backedMaterialQuantity > reservation->remainingBackedMaterialQuantity ||
                release.capacityQuantity > reservation->remainingCapacityQuantity ||
                (IsItemUnitKind(reservation->capacity.kind) &&
                 release.backedMaterialQuantity != release.capacityQuantity))
                return MaterialCommitmentApplyStatus::InvalidCommand;
            if (!CheckedAdd(totalRelease, release.backedMaterialQuantity))
                return MaterialCommitmentApplyStatus::InvalidCommand;
        }
        if (fulfillment.quantity < commitment->remainingQuantity && totalRelease != fulfillment.quantity)
            return MaterialCommitmentApplyStatus::InvalidCommand;

        commitment->remainingQuantity -= fulfillment.quantity;
        if (commitment->remainingQuantity == 0u)
        {
            commitment->state = MaterialCommitmentState::Completed;
            commitment->reservations.clear();
            if (commitment->sourcePath)
                commitment->sourcePath->phase = MaterialSourcePhase::Completed;
            continue;
        }
        for (MaterialReservationSettlement const& release : fulfillment.reservationSettlements)
        {
            auto reservation =
                std::ranges::find_if(commitment->reservations, [&release](MaterialReservation const& value)
                                     { return CapacityKey(value.capacity) == CapacityKey(release.capacity); });
            reservation->remainingBackedMaterialQuantity -= release.backedMaterialQuantity;
            reservation->remainingCapacityQuantity -= release.capacityQuantity;
        }
        std::erase_if(commitment->reservations,
                      [](MaterialReservation const& value) { return value.remainingCapacityQuantity == 0u; });
        commitment->state = MaterialCommitmentState::PartiallyFulfilled;
    }
    return MaterialCommitmentApplyStatus::PendingPersistence;
}

MaterialCommitmentApplyStatus TerminateCommitments(MaterialCommitmentStartup& state,
                                                   std::vector<std::string> const& identities,
                                                   MaterialCommitmentState terminalState)
{
    if (identities.empty())
        return MaterialCommitmentApplyStatus::InvalidCommand;
    std::set<std::string> unique;
    for (std::string const& identity : identities)
    {
        if (identity.empty() || !unique.insert(identity).second)
            return MaterialCommitmentApplyStatus::InvalidCommand;
        MaterialCommitment* commitment = FindCommitment(state, identity);
        if (!commitment)
            return MaterialCommitmentApplyStatus::UnknownCommitment;
        if (!IsActive(commitment->state))
            return MaterialCommitmentApplyStatus::TerminalCommitment;
    }
    for (std::string const& identity : identities)
    {
        MaterialCommitment* commitment = FindCommitment(state, identity);
        commitment->state = terminalState;
        commitment->remainingQuantity = 0u;
        commitment->reservations.clear();
        if (commitment->sourcePath)
            commitment->sourcePath->phase = MaterialSourcePhase::Released;
    }
    return MaterialCommitmentApplyStatus::PendingPersistence;
}

MaterialCommitmentApplyStatus ReleaseCommitments(MaterialCommitmentStartup& state,
                                                 MaterialCommitmentCommand const& command)
{
    if (!command.intents.empty() || !command.candidates.empty() || !command.capacityObservations.empty() ||
        !command.sourceStarts.empty() || !command.fulfillments.empty())
    {
        return MaterialCommitmentApplyStatus::InvalidCommand;
    }
    return TerminateCommitments(state, command.commitmentIdentities, MaterialCommitmentState::Released);
}

MaterialCommitmentApplyStatus SupersedeCommitments(MaterialCommitmentStartup& state,
                                                   MaterialCommitmentCommand const& command, std::uint64_t now,
                                                   std::vector<std::string>& admittedIdentities)
{
    if (!command.intents.empty() || !command.sourceStarts.empty() || !command.fulfillments.empty())
        return MaterialCommitmentApplyStatus::InvalidCommand;

    std::set<std::string> affectedOrigins;
    for (std::string const& identity : command.commitmentIdentities)
    {
        MaterialCommitment const* commitment = FindCommitment(state, identity);
        if (!commitment)
            return MaterialCommitmentApplyStatus::UnknownCommitment;
        affectedOrigins.insert(commitment->originIdentity);
    }
    for (MaterialCommitment const& commitment : state.commitments)
    {
        if (IsActive(commitment.state) && affectedOrigins.contains(commitment.originIdentity) &&
            std::ranges::find(command.commitmentIdentities, commitment.identity) == command.commitmentIdentities.end())
        {
            return MaterialCommitmentApplyStatus::InvalidCommand;
        }
    }

    MaterialCommitmentApplyStatus const terminated =
        TerminateCommitments(state, command.commitmentIdentities, MaterialCommitmentState::Superseded);
    if (terminated != MaterialCommitmentApplyStatus::PendingPersistence)
        return terminated;
    return AdmitCandidates(state, command, now, admittedIdentities);
}

MaterialCommitmentApplyStatus ApplyToReplacement(MaterialCommitmentStartup& replacement,
                                                 MaterialCommitmentCommand const& command, std::uint64_t now,
                                                 std::vector<std::string>& admittedIdentities)
{
    switch (command.kind)
    {
        case MaterialCommitmentCommandKind::Observe:
            return ObserveIntents(replacement, command, now);
        case MaterialCommitmentCommandKind::Admit:
            if (!command.commitmentIdentities.empty())
                return MaterialCommitmentApplyStatus::InvalidCommand;
            return AdmitCandidates(replacement, command, now, admittedIdentities);
        case MaterialCommitmentCommandKind::StartSource:
            return StartSources(replacement, command);
        case MaterialCommitmentCommandKind::Fulfill:
            return FulfillCommitments(replacement, command);
        case MaterialCommitmentCommandKind::Release:
            return ReleaseCommitments(replacement, command);
        case MaterialCommitmentCommandKind::Supersede:
            return SupersedeCommitments(replacement, command, now, admittedIdentities);
    }
    return MaterialCommitmentApplyStatus::InvalidCommand;
}
}  // namespace

PlayerbotMaterialCommitmentAuthority::PlayerbotMaterialCommitmentAuthority(AsyncWriter writer)
    : writer(std::move(writer))
{
}

bool PlayerbotMaterialCommitmentAuthority::Restore(MaterialCommitmentStartup startup)
{
    std::scoped_lock lock(mutex);
    pending.reset();
    if (!ValidStartup(startup))
    {
        durable = {};
        durable.sourceAvailable = false;
        persistenceHealthy = false;
        return false;
    }
    durable = std::move(startup);
    persistenceHealthy = true;
    return true;
}

MaterialCommitmentApplyResult PlayerbotMaterialCommitmentAuthority::Apply(MaterialCommitmentCommand command,
                                                                          std::uint64_t now)
{
    std::unique_lock lock(mutex);
    if (!persistenceHealthy || !writer)
        return {.status = MaterialCommitmentApplyStatus::PersistenceUnavailable};
    if (pending.has_value())
        return {.status = MaterialCommitmentApplyStatus::Busy};
    if (command.operationIdentity.empty() ||
        command.operationIdentity.size() > MaterialCommitmentEncoding::MAX_IDENTITY_BYTES)
        return {.status = MaterialCommitmentApplyStatus::InvalidCommand};

    std::string const fingerprint = MaterialCommitmentEncoding::Fingerprint(command);
    if (fingerprint.size() > MaterialCommitmentEncoding::MAX_FINGERPRINT_BYTES)
        return {.status = MaterialCommitmentApplyStatus::InvalidCommand};
    if (MaterialCommitmentOperation const* prior = FindOperation(durable, command.operationIdentity))
    {
        if (prior->fingerprint != fingerprint)
            return {.status = MaterialCommitmentApplyStatus::OperationConflict};
        return {
            .status = MaterialCommitmentApplyStatus::Idempotent,
            .commitmentIdentities = prior->commitmentIdentities,
        };
    }
    if (command.expectedBookRevision != durable.bookRevision)
        return {.status = MaterialCommitmentApplyStatus::StaleBookRevision};

    MaterialCommitmentStartup replacement = durable;
    std::vector<std::string> admittedIdentities;
    MaterialCommitmentApplyStatus const result = ApplyToReplacement(replacement, command, now, admittedIdentities);
    if (result != MaterialCommitmentApplyStatus::PendingPersistence)
        return {.status = result};

    ++replacement.bookRevision;
    replacement.operations.push_back({
        .identity = command.operationIdentity,
        .fingerprint = fingerprint,
        .resultingBookRevision = replacement.bookRevision,
        .commitmentIdentities = admittedIdentities,
    });
    if (!ValidStartup(replacement))
        return {.status = MaterialCommitmentApplyStatus::InvalidCommand};

    std::uint64_t const token = nextWriteToken++;
    MaterialCommitmentWrite write{
        .expectedBookRevision = durable.bookRevision,
        .newBookRevision = replacement.bookRevision,
        .operation = replacement.operations.back(),
        .replacement = replacement,
    };
    for (MaterialIntent const& intent : replacement.intents)
    {
        MaterialIntent const* existing = FindIntent(durable, intent.originIdentity);
        if (!existing || *existing != intent)
            write.changedOriginIdentities.push_back(intent.originIdentity);
    }
    for (MaterialCommitment const& commitment : replacement.commitments)
    {
        MaterialCommitment const* existing = FindCommitment(durable, commitment.identity);
        if (!existing || *existing != commitment)
            write.changedCommitmentIdentities.push_back(commitment.identity);
    }
    pending = PendingWrite{.token = token, .replacement = std::move(replacement)};
    AsyncWriter const selectedWriter = writer;
    lock.unlock();
    selectedWriter(token, write);
    return {
        .status = MaterialCommitmentApplyStatus::PendingPersistence,
        .writeToken = token,
    };
}

MaterialCommitmentApplyResult SettleCompletedMaterialSource(PlayerbotMaterialCommitmentAuthority& authority,
                                                            std::uint64_t expectedBookRevision,
                                                            MaterialCommitment const& commitment,
                                                            std::uint32_t currentInventoryQuantity, std::uint64_t now)
{
    if (!commitment.sourcePath)
        return {};

    MaterialSourcePath const& path = *commitment.sourcePath;
    std::vector<MaterialReservationSettlement> settlements;
    settlements.reserve(commitment.reservations.size());
    for (MaterialReservation const& reservation : commitment.reservations)
    {
        settlements.push_back({.capacity = reservation.capacity,
                               .backedMaterialQuantity = reservation.remainingBackedMaterialQuantity,
                               .capacityQuantity = reservation.remainingCapacityQuantity});
    }

    return authority.Apply(
        {.operationIdentity = commitment.identity + ":inventory-delivered:" + std::to_string(path.sourceRevision),
         .expectedBookRevision = expectedBookRevision,
         .kind = MaterialCommitmentCommandKind::Fulfill,
         .fulfillments = {{.commitmentIdentity = commitment.identity,
                           .quantity = commitment.remainingQuantity,
                           .observedInventoryQuantity = currentInventoryQuantity,
                           .reservationSettlements = std::move(settlements)}}},
        now);
}

void PlayerbotMaterialCommitmentAuthority::CompleteWrite(std::uint64_t writeToken, bool success)
{
    std::scoped_lock lock(mutex);
    if (!pending.has_value() || pending->token != writeToken)
        return;
    if (success)
        durable = std::move(pending->replacement);
    else
        persistenceHealthy = false;
    pending.reset();
}

MaterialCommitmentSnapshot PlayerbotMaterialCommitmentAuthority::Snapshot() const
{
    std::scoped_lock lock(mutex);
    return {
        .bookRevision = durable.bookRevision,
        .persistenceHealthy = persistenceHealthy,
        .busy = pending.has_value(),
        .intents = durable.intents,
        .commitments = durable.commitments,
    };
}
}  // namespace PlayerbotEconomy
