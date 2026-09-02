/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotMaterialCommitmentPersistence.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "Bot/Economy/PlayerbotMaterialCommitmentEncoding.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "Log.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include "StringFormat.h"

namespace PlayerbotEconomy
{
namespace
{
using PlayerbotsPool = DatabaseWorkerPool<PlayerbotsDatabaseConnection>;

std::string SqlString(PlayerbotsPool& database, std::string value)
{
    database.EscapeString(value);
    return Acore::StringFormat("'{}'", value);
}

char const* CapacityKindName(MaterialCapacityKind value)
{
    switch (value)
    {
        case MaterialCapacityKind::OwnedItem:
            return "owned_item";
        case MaterialCapacityKind::AuctionListing:
            return "auction_listing";
        case MaterialCapacityKind::Money:
            return "money";
        case MaterialCapacityKind::GatheringCapacity:
            return "gathering_capacity";
        case MaterialCapacityKind::ProductionCapacity:
            return "production_capacity";
    }
    return "money";
}

std::optional<MaterialCapacityKind> ParseCapacityKind(std::string const& value)
{
    if (value == "owned_item")
        return MaterialCapacityKind::OwnedItem;
    if (value == "auction_listing")
        return MaterialCapacityKind::AuctionListing;
    if (value == "money")
        return MaterialCapacityKind::Money;
    if (value == "gathering_capacity")
        return MaterialCapacityKind::GatheringCapacity;
    if (value == "production_capacity")
        return MaterialCapacityKind::ProductionCapacity;
    return std::nullopt;
}

char const* CapacityUnitName(MaterialCapacityUnit value)
{
    switch (value)
    {
        case MaterialCapacityUnit::ItemUnits:
            return "item_units";
        case MaterialCapacityUnit::Copper:
            return "copper";
        case MaterialCapacityUnit::GatheringUnits:
            return "gathering_units";
        case MaterialCapacityUnit::ProductionUnits:
            return "production_units";
    }
    return "copper";
}

std::optional<MaterialCapacityUnit> ParseCapacityUnit(std::string const& value)
{
    if (value == "item_units")
        return MaterialCapacityUnit::ItemUnits;
    if (value == "copper")
        return MaterialCapacityUnit::Copper;
    if (value == "gathering_units")
        return MaterialCapacityUnit::GatheringUnits;
    if (value == "production_units")
        return MaterialCapacityUnit::ProductionUnits;
    return std::nullopt;
}

char const* CommitmentStateName(MaterialCommitmentState value)
{
    switch (value)
    {
        case MaterialCommitmentState::Admitted:
            return "admitted";
        case MaterialCommitmentState::PartiallyFulfilled:
            return "partially_fulfilled";
        case MaterialCommitmentState::Completed:
            return "completed";
        case MaterialCommitmentState::Released:
            return "released";
        case MaterialCommitmentState::Superseded:
            return "superseded";
    }
    return "released";
}

std::optional<MaterialCommitmentState> ParseCommitmentState(std::string const& value)
{
    if (value == "admitted")
        return MaterialCommitmentState::Admitted;
    if (value == "partially_fulfilled")
        return MaterialCommitmentState::PartiallyFulfilled;
    if (value == "completed")
        return MaterialCommitmentState::Completed;
    if (value == "released")
        return MaterialCommitmentState::Released;
    if (value == "superseded")
        return MaterialCommitmentState::Superseded;
    return std::nullopt;
}

char const* OwnerKindName(MaterialCommitmentOwnerKind value)
{
    switch (value)
    {
        case MaterialCommitmentOwnerKind::ProfessionProgression:
            return "profession_progression";
        case MaterialCommitmentOwnerKind::StockMaintenance:
            return "stock_maintenance";
        case MaterialCommitmentOwnerKind::SupplyRemediation:
            return "supply_remediation";
        case MaterialCommitmentOwnerKind::ActivityCritical:
            return "activity_critical";
        case MaterialCommitmentOwnerKind::GroupCommitment:
            return "group_commitment";
    }
    return "profession_progression";
}

std::optional<MaterialCommitmentOwnerKind> ParseOwnerKind(std::string const& value)
{
    if (value == "profession_progression")
        return MaterialCommitmentOwnerKind::ProfessionProgression;
    if (value == "stock_maintenance")
        return MaterialCommitmentOwnerKind::StockMaintenance;
    if (value == "supply_remediation")
        return MaterialCommitmentOwnerKind::SupplyRemediation;
    if (value == "activity_critical")
        return MaterialCommitmentOwnerKind::ActivityCritical;
    if (value == "group_commitment")
        return MaterialCommitmentOwnerKind::GroupCommitment;
    return std::nullopt;
}

char const* SourceKindName(MaterialSourceKind value)
{
    switch (value)
    {
        case MaterialSourceKind::SameActorGathering:
            return "same_actor_gathering";
        case MaterialSourceKind::SameActorHunting:
            return "same_actor_hunting";
    }
    return "same_actor_gathering";
}

std::optional<MaterialSourceKind> ParseSourceKind(std::string const& value)
{
    if (value == "same_actor_gathering")
        return MaterialSourceKind::SameActorGathering;
    if (value == "same_actor_hunting")
        return MaterialSourceKind::SameActorHunting;
    return std::nullopt;
}

char const* SourcePhaseName(MaterialSourcePhase value)
{
    switch (value)
    {
        case MaterialSourcePhase::Selected:
            return "selected";
        case MaterialSourcePhase::Acquiring:
            return "acquiring";
        case MaterialSourcePhase::Completed:
            return "completed";
        case MaterialSourcePhase::Released:
            return "released";
    }
    return "released";
}

std::optional<MaterialSourcePhase> ParseSourcePhase(std::string const& value)
{
    if (value == "selected")
        return MaterialSourcePhase::Selected;
    if (value == "acquiring")
        return MaterialSourcePhase::Acquiring;
    if (value == "completed")
        return MaterialSourcePhase::Completed;
    if (value == "released")
        return MaterialSourcePhase::Released;
    return std::nullopt;
}

void AppendIntent(PlayerbotsPool& database, PlayerbotsDatabaseTransaction const& transaction,
                  MaterialIntent const& intent)
{
    std::string const neededBy = intent.neededBy.has_value() ? std::to_string(*intent.neededBy) : "NULL";
    transaction->Append(Acore::StringFormat(
        "INSERT INTO playerbot_economy_material_intent "
        "(origin_identity, owner_kind, owner_revision, market_id, bounded_quantity, needed_by, first_observed_at, "
        "last_observed_at) VALUES ({}, {}, {}, {}, {}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE owner_kind = VALUES(owner_kind), owner_revision = VALUES(owner_revision), "
        "market_id = VALUES(market_id), "
        "bounded_quantity = VALUES(bounded_quantity), needed_by = VALUES(needed_by), "
        "first_observed_at = VALUES(first_observed_at), last_observed_at = VALUES(last_observed_at)",
        SqlString(database, intent.originIdentity), SqlString(database, OwnerKindName(intent.ownerKind)),
        intent.ownerRevision, intent.marketId, intent.boundedQuantity, neededBy, intent.firstObservedAt,
        intent.lastObservedAt));
    transaction->Append(
        Acore::StringFormat("DELETE FROM playerbot_economy_material_requirement WHERE origin_identity = {}",
                            SqlString(database, intent.originIdentity)));
    for (std::size_t ordinal = 0u; ordinal < intent.requirements.size(); ++ordinal)
    {
        MaterialRequirement const& requirement = intent.requirements[ordinal];
        transaction->Append(Acore::StringFormat(
            "INSERT INTO playerbot_economy_material_requirement "
            "(origin_identity, requirement_ordinal, item_id, quantity) VALUES ({}, {}, {}, {})",
            SqlString(database, intent.originIdentity), ordinal, requirement.itemId, requirement.quantity));
    }
}

void AppendCommitment(PlayerbotsPool& database, PlayerbotsDatabaseTransaction const& transaction,
                      MaterialCommitment const& commitment)
{
    transaction->Append(Acore::StringFormat(
        "INSERT INTO playerbot_economy_material_commitment "
        "(public_id, origin_identity, owner_kind, owner_revision, market_id, material_item_id, bounded_quantity, "
        "remaining_quantity, needed_by, state) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}) "
        "ON DUPLICATE KEY UPDATE origin_identity = VALUES(origin_identity), owner_kind = VALUES(owner_kind), "
        "owner_revision = VALUES(owner_revision), market_id = VALUES(market_id), "
        "material_item_id = VALUES(material_item_id), bounded_quantity = VALUES(bounded_quantity), "
        "remaining_quantity = VALUES(remaining_quantity), needed_by = VALUES(needed_by), state = VALUES(state)",
        SqlString(database, commitment.identity), SqlString(database, commitment.originIdentity),
        SqlString(database, OwnerKindName(commitment.ownerKind)), commitment.ownerRevision, commitment.marketId,
        commitment.materialItemId, commitment.boundedQuantity, commitment.remainingQuantity, commitment.neededBy,
        SqlString(database, CommitmentStateName(commitment.state))));
    transaction->Append(
        Acore::StringFormat("DELETE FROM playerbot_economy_material_reservation WHERE commitment_public_id = {}",
                            SqlString(database, commitment.identity)));
    transaction->Append(
        Acore::StringFormat("DELETE FROM playerbot_economy_material_source_path WHERE commitment_public_id = {}",
                            SqlString(database, commitment.identity)));
    for (std::size_t ordinal = 0u; ordinal < commitment.reservations.size(); ++ordinal)
    {
        MaterialReservation const& reservation = commitment.reservations[ordinal];
        transaction->Append(Acore::StringFormat(
            "INSERT INTO playerbot_economy_material_reservation "
            "(commitment_public_id, reservation_ordinal, material_item_id, capacity_kind, capacity_identity, "
            "capacity_unit, authority_revision, initial_backed_material_quantity, "
            "remaining_backed_material_quantity, initial_capacity_quantity, remaining_capacity_quantity) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
            SqlString(database, commitment.identity), ordinal, reservation.materialItemId,
            SqlString(database, CapacityKindName(reservation.capacity.kind)),
            SqlString(database, reservation.capacity.authorityIdentity),
            SqlString(database, CapacityUnitName(reservation.unit)), reservation.authorityRevision,
            reservation.initialBackedMaterialQuantity, reservation.remainingBackedMaterialQuantity,
            reservation.initialCapacityQuantity, reservation.remainingCapacityQuantity));
    }
    if (commitment.sourcePath)
    {
        MaterialSourcePath const& path = *commitment.sourcePath;
        transaction->Append(Acore::StringFormat(
            "INSERT INTO playerbot_economy_material_source_path "
            "(commitment_public_id, source_kind, phase, actor_guid, material_item_id, selected_quantity, "
            "gathering_skill_id, source_entry, source_map_id, route_identity, capacity_identity, source_revision, "
            "selected_at, source_travel_budget_seconds, source_action_budget_seconds, "
            "delivery_travel_budget_seconds, completion_observation_budget_seconds, "
            "destination_yield_basis_points, conservative_yield_basis_points, observed_gathered_quantity, "
            "observed_resource_attempts, observed_resource_seconds, authoritative_interaction_seconds, "
            "remaining_dedicated_activity_seconds, required_resource_count, seconds_per_resource, "
            "starting_inventory_quantity, available_resource_count, needed_by) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, "
            "{}, {}, {}, {}, {}, {})",
            SqlString(database, commitment.identity), SqlString(database, SourceKindName(path.kind)),
            SqlString(database, SourcePhaseName(path.phase)), path.actorGuid, path.materialItemId,
            path.selectedQuantity, path.gatheringSkillId, path.sourceEntry, path.sourceMapId,
            SqlString(database, path.routeIdentity), SqlString(database, path.capacityIdentity), path.sourceRevision,
            path.selectedAt, path.sourceTravelBudgetSeconds, path.sourceActionBudgetSeconds,
            path.deliveryTravelBudgetSeconds, path.completionObservationBudgetSeconds, path.destinationYieldBasisPoints,
            path.conservativeYieldBasisPoints, path.observedGatheredQuantity, path.observedResourceAttempts,
            path.observedResourceSeconds, path.authoritativeInteractionSeconds, path.remainingDedicatedActivitySeconds,
            path.requiredResourceCount, path.secondsPerResource, path.startingInventoryQuantity,
            path.availableResourceCount, path.neededBy));
    }
}

void AppendOperation(PlayerbotsPool& database, PlayerbotsDatabaseTransaction const& transaction,
                     MaterialCommitmentOperation const& operation)
{
    transaction->Append(
        Acore::StringFormat("INSERT INTO playerbot_economy_material_operation "
                            "(operation_identity, fingerprint, resulting_book_revision) VALUES ({}, {}, {})",
                            SqlString(database, operation.identity), SqlString(database, operation.fingerprint),
                            operation.resultingBookRevision));
    for (std::size_t ordinal = 0u; ordinal < operation.commitmentIdentities.size(); ++ordinal)
    {
        transaction->Append(
            Acore::StringFormat("INSERT INTO playerbot_economy_material_operation_commitment "
                                "(operation_identity, commitment_ordinal, commitment_public_id) VALUES ({}, {}, {})",
                                SqlString(database, operation.identity), ordinal,
                                SqlString(database, operation.commitmentIdentities[ordinal])));
    }
}

std::optional<std::uint64_t> TableCount(PlayerbotsPool& database, char const* table)
{
    QueryResult result = database.Query(Acore::StringFormat("SELECT COUNT(*) FROM {}", table));
    if (!result)
        return std::nullopt;
    return result->Fetch()[0].Get<std::uint64_t>();
}

bool ReadExpected(PlayerbotsPool& database, std::string const& sql, std::uint64_t expected,
                  std::function<bool(Field*)> const& consume)
{
    if (expected == 0u)
        return true;
    QueryResult result = database.Query(sql);
    if (!result)
        return false;
    std::uint64_t rows = 0u;
    do
    {
        if (!consume(result->Fetch()))
            return false;
        ++rows;
    } while (result->NextRow());
    return rows == expected;
}

MaterialIntent* FindIntent(MaterialCommitmentStartup& startup, std::string const& identity)
{
    auto found = std::ranges::find(startup.intents, identity, &MaterialIntent::originIdentity);
    return found == startup.intents.end() ? nullptr : &*found;
}

MaterialCommitment* FindCommitment(MaterialCommitmentStartup& startup, std::string const& identity)
{
    auto found = std::ranges::find(startup.commitments, identity, &MaterialCommitment::identity);
    return found == startup.commitments.end() ? nullptr : &*found;
}

MaterialCommitmentOperation* FindOperation(MaterialCommitmentStartup& startup, std::string const& identity)
{
    auto found = std::ranges::find(startup.operations, identity, &MaterialCommitmentOperation::identity);
    return found == startup.operations.end() ? nullptr : &*found;
}

bool LoadIntents(PlayerbotsPool& database, MaterialCommitmentStartup& startup)
{
    std::optional<std::uint64_t> const intentCount = TableCount(database, "playerbot_economy_material_intent");
    std::optional<std::uint64_t> const requirementCount =
        TableCount(database, "playerbot_economy_material_requirement");
    if (!intentCount.has_value() || !requirementCount.has_value())
        return false;
    if (!ReadExpected(database,
                      "SELECT origin_identity, owner_kind, owner_revision, market_id, bounded_quantity, needed_by, "
                      "first_observed_at, last_observed_at "
                      "FROM playerbot_economy_material_intent ORDER BY origin_identity",
                      *intentCount,
                      [&startup](Field* fields)
                      {
                          std::optional<MaterialCommitmentOwnerKind> const ownerKind =
                              ParseOwnerKind(fields[1].Get<std::string>());
                          if (!ownerKind.has_value())
                              return false;
                          MaterialIntent intent{
                              .originIdentity = fields[0].Get<std::string>(),
                              .ownerKind = *ownerKind,
                              .ownerRevision = fields[2].Get<std::uint64_t>(),
                              .marketId = fields[3].Get<std::uint32_t>(),
                              .boundedQuantity = fields[4].Get<std::uint32_t>(),
                              .firstObservedAt = fields[6].Get<std::uint64_t>(),
                              .lastObservedAt = fields[7].Get<std::uint64_t>(),
                          };
                          if (!fields[5].IsNull())
                              intent.neededBy = fields[5].Get<std::uint64_t>();
                          startup.intents.push_back(std::move(intent));
                          return true;
                      }))
    {
        return false;
    }
    return ReadExpected(
        database,
        "SELECT origin_identity, item_id, quantity FROM playerbot_economy_material_requirement "
        "ORDER BY origin_identity, requirement_ordinal",
        *requirementCount,
        [&startup](Field* fields)
        {
            MaterialIntent* intent = FindIntent(startup, fields[0].Get<std::string>());
            if (!intent)
                return false;
            intent->requirements.push_back({fields[1].Get<std::uint32_t>(), fields[2].Get<std::uint32_t>()});
            return true;
        });
}

bool LoadCommitments(PlayerbotsPool& database, MaterialCommitmentStartup& startup)
{
    std::optional<std::uint64_t> const commitmentCount = TableCount(database, "playerbot_economy_material_commitment");
    std::optional<std::uint64_t> const reservationCount =
        TableCount(database, "playerbot_economy_material_reservation");
    if (!commitmentCount.has_value() || !reservationCount.has_value())
        return false;
    if (!ReadExpected(database,
                      "SELECT public_id, origin_identity, owner_kind, owner_revision, market_id, material_item_id, "
                      "bounded_quantity, remaining_quantity, needed_by, state "
                      "FROM playerbot_economy_material_commitment ORDER BY public_id",
                      *commitmentCount,
                      [&startup](Field* fields)
                      {
                          std::optional<MaterialCommitmentOwnerKind> const ownerKind =
                              ParseOwnerKind(fields[2].Get<std::string>());
                          std::optional<MaterialCommitmentState> const state =
                              ParseCommitmentState(fields[9].Get<std::string>());
                          if (!ownerKind.has_value() || !state.has_value())
                              return false;
                          startup.commitments.push_back({
                              .identity = fields[0].Get<std::string>(),
                              .originIdentity = fields[1].Get<std::string>(),
                              .ownerKind = *ownerKind,
                              .ownerRevision = fields[3].Get<std::uint64_t>(),
                              .marketId = fields[4].Get<std::uint32_t>(),
                              .materialItemId = fields[5].Get<std::uint32_t>(),
                              .boundedQuantity = fields[6].Get<std::uint32_t>(),
                              .remainingQuantity = fields[7].Get<std::uint32_t>(),
                              .neededBy = fields[8].Get<std::uint64_t>(),
                              .state = *state,
                          });
                          return true;
                      }))
    {
        return false;
    }
    return ReadExpected(
        database,
        "SELECT commitment_public_id, material_item_id, capacity_kind, capacity_identity, capacity_unit, "
        "authority_revision, initial_backed_material_quantity, remaining_backed_material_quantity, "
        "initial_capacity_quantity, remaining_capacity_quantity "
        "FROM playerbot_economy_material_reservation ORDER BY commitment_public_id, reservation_ordinal",
        *reservationCount,
        [&startup](Field* fields)
        {
            MaterialCommitment* commitment = FindCommitment(startup, fields[0].Get<std::string>());
            std::optional<MaterialCapacityKind> const kind = ParseCapacityKind(fields[2].Get<std::string>());
            std::optional<MaterialCapacityUnit> const unit = ParseCapacityUnit(fields[4].Get<std::string>());
            if (!commitment || !kind.has_value() || !unit.has_value())
                return false;
            commitment->reservations.push_back({
                .materialItemId = fields[1].Get<std::uint32_t>(),
                .capacity = {.kind = *kind, .authorityIdentity = fields[3].Get<std::string>()},
                .unit = *unit,
                .authorityRevision = fields[5].Get<std::uint64_t>(),
                .initialBackedMaterialQuantity = fields[6].Get<std::uint64_t>(),
                .remainingBackedMaterialQuantity = fields[7].Get<std::uint64_t>(),
                .initialCapacityQuantity = fields[8].Get<std::uint64_t>(),
                .remainingCapacityQuantity = fields[9].Get<std::uint64_t>(),
            });
            return true;
        });
}

bool LoadSourcePaths(PlayerbotsPool& database, MaterialCommitmentStartup& startup)
{
    std::optional<std::uint64_t> const pathCount = TableCount(database, "playerbot_economy_material_source_path");
    if (!pathCount.has_value())
        return false;
    return ReadExpected(
        database,
        "SELECT commitment_public_id, source_kind, phase, actor_guid, material_item_id, selected_quantity, "
        "gathering_skill_id, source_entry, source_map_id, route_identity, capacity_identity, source_revision, "
        "selected_at, source_travel_budget_seconds, source_action_budget_seconds, delivery_travel_budget_seconds, "
        "completion_observation_budget_seconds, destination_yield_basis_points, conservative_yield_basis_points, "
        "observed_gathered_quantity, observed_resource_attempts, observed_resource_seconds, "
        "authoritative_interaction_seconds, remaining_dedicated_activity_seconds, required_resource_count, "
        "seconds_per_resource, starting_inventory_quantity, available_resource_count, needed_by "
        "FROM playerbot_economy_material_source_path ORDER BY commitment_public_id",
        *pathCount,
        [&startup](Field* fields)
        {
            MaterialCommitment* commitment = FindCommitment(startup, fields[0].Get<std::string>());
            std::optional<MaterialSourceKind> const kind = ParseSourceKind(fields[1].Get<std::string>());
            std::optional<MaterialSourcePhase> const phase = ParseSourcePhase(fields[2].Get<std::string>());
            if (!commitment || !kind || !phase || commitment->sourcePath)
                return false;
            commitment->sourcePath = MaterialSourcePath{
                .kind = *kind,
                .phase = *phase,
                .actorGuid = fields[3].Get<std::uint32_t>(),
                .materialItemId = fields[4].Get<std::uint32_t>(),
                .selectedQuantity = fields[5].Get<std::uint32_t>(),
                .gatheringSkillId = fields[6].Get<std::uint32_t>(),
                .sourceEntry = fields[7].Get<std::uint32_t>(),
                .sourceMapId = fields[8].Get<std::uint32_t>(),
                .routeIdentity = fields[9].Get<std::string>(),
                .capacityIdentity = fields[10].Get<std::string>(),
                .sourceRevision = fields[11].Get<std::uint64_t>(),
                .selectedAt = fields[12].Get<std::uint64_t>(),
                .sourceTravelBudgetSeconds = fields[13].Get<std::uint32_t>(),
                .sourceActionBudgetSeconds = fields[14].Get<std::uint32_t>(),
                .deliveryTravelBudgetSeconds = fields[15].Get<std::uint32_t>(),
                .completionObservationBudgetSeconds = fields[16].Get<std::uint32_t>(),
                .destinationYieldBasisPoints = fields[17].Get<std::uint32_t>(),
                .conservativeYieldBasisPoints = fields[18].Get<std::uint32_t>(),
                .observedGatheredQuantity = fields[19].Get<std::uint32_t>(),
                .observedResourceAttempts = fields[20].Get<std::uint32_t>(),
                .observedResourceSeconds = fields[21].Get<std::uint32_t>(),
                .authoritativeInteractionSeconds = fields[22].Get<std::uint32_t>(),
                .remainingDedicatedActivitySeconds = fields[23].Get<std::uint32_t>(),
                .requiredResourceCount = fields[24].Get<std::uint32_t>(),
                .secondsPerResource = fields[25].Get<std::uint32_t>(),
                .startingInventoryQuantity = fields[26].Get<std::uint32_t>(),
                .availableResourceCount = fields[27].Get<std::uint32_t>(),
                .neededBy = fields[28].Get<std::uint64_t>(),
            };
            return true;
        });
}

bool LoadOperations(PlayerbotsPool& database, MaterialCommitmentStartup& startup)
{
    std::optional<std::uint64_t> const operationCount = TableCount(database, "playerbot_economy_material_operation");
    std::optional<std::uint64_t> const receiptCount =
        TableCount(database, "playerbot_economy_material_operation_commitment");
    if (!operationCount.has_value() || !receiptCount.has_value())
        return false;
    if (!ReadExpected(database,
                      "SELECT operation_identity, fingerprint, resulting_book_revision "
                      "FROM playerbot_economy_material_operation ORDER BY resulting_book_revision, operation_identity",
                      *operationCount,
                      [&startup](Field* fields)
                      {
                          startup.operations.push_back({
                              .identity = fields[0].Get<std::string>(),
                              .fingerprint = fields[1].Get<std::string>(),
                              .resultingBookRevision = fields[2].Get<std::uint64_t>(),
                          });
                          return true;
                      }))
    {
        return false;
    }
    return ReadExpected(database,
                        "SELECT operation_identity, commitment_public_id "
                        "FROM playerbot_economy_material_operation_commitment "
                        "ORDER BY operation_identity, commitment_ordinal",
                        *receiptCount,
                        [&startup](Field* fields)
                        {
                            MaterialCommitmentOperation* operation =
                                FindOperation(startup, fields[0].Get<std::string>());
                            if (!operation)
                                return false;
                            operation->commitmentIdentities.push_back(fields[1].Get<std::string>());
                            return true;
                        });
}
}  // namespace

PlayerbotMaterialCommitmentPersistence::PlayerbotMaterialCommitmentPersistence(PlayerbotsPool& database)
    : database(database)
{
}

void PlayerbotMaterialCommitmentPersistence::QueueWrite(std::uint64_t token, MaterialCommitmentWrite const& write,
                                                        Completion completion)
{
    PlayerbotsDatabaseTransaction transaction = database.BeginTransaction();
    transaction->Append(
        Acore::StringFormat("UPDATE playerbot_economy_material_book SET book_revision = {} "
                            "WHERE singleton_id = 1 AND book_revision = {}",
                            write.newBookRevision, write.expectedBookRevision));
    for (std::string const& identity : write.changedOriginIdentities)
    {
        auto const intent = std::ranges::find(write.replacement.intents, identity, &MaterialIntent::originIdentity);
        if (intent != write.replacement.intents.end())
            AppendIntent(database, transaction, *intent);
    }
    for (std::string const& identity : write.changedCommitmentIdentities)
    {
        auto const commitment =
            std::ranges::find(write.replacement.commitments, identity, &MaterialCommitment::identity);
        if (commitment != write.replacement.commitments.end())
            AppendCommitment(database, transaction, *commitment);
    }
    for (std::string const& identity : write.removedOriginIdentities)
    {
        transaction->Append(
            Acore::StringFormat("DELETE FROM playerbot_economy_material_requirement WHERE origin_identity = {}",
                                SqlString(database, identity)));
        transaction->Append(Acore::StringFormat(
            "DELETE FROM playerbot_economy_material_intent WHERE origin_identity = {}", SqlString(database, identity)));
    }
    for (std::string const& identity : write.removedCommitmentIdentities)
    {
        transaction->Append(
            Acore::StringFormat("DELETE FROM playerbot_economy_material_reservation WHERE commitment_public_id = {}",
                                SqlString(database, identity)));
        transaction->Append(
            Acore::StringFormat("DELETE FROM playerbot_economy_material_source_path WHERE commitment_public_id = {}",
                                SqlString(database, identity)));
        transaction->Append(Acore::StringFormat(
            "DELETE FROM playerbot_economy_material_commitment WHERE public_id = {}", SqlString(database, identity)));
    }
    for (std::string const& identity : write.removedOperationIdentities)
    {
        transaction->Append(Acore::StringFormat(
            "DELETE FROM playerbot_economy_material_operation_commitment WHERE operation_identity = {}",
            SqlString(database, identity)));
        transaction->Append(
            Acore::StringFormat("DELETE FROM playerbot_economy_material_operation WHERE operation_identity = {}",
                                SqlString(database, identity)));
    }
    AppendOperation(database, transaction, write.operation);
    transactionProcessor.AddCallback(database.AsyncCommitTransaction(transaction))
        .AfterComplete([token, completion = std::move(completion)](bool success) { completion(token, success); });
}

MaterialCommitmentStartup PlayerbotMaterialCommitmentPersistence::Load()
{
    MaterialCommitmentStartup startup;
    QueryResult book =
        database.Query("SELECT book_revision FROM playerbot_economy_material_book WHERE singleton_id = 1");
    if (book)
    {
        startup.sourceAvailable = true;
        startup.bookRevision = book->Fetch()[0].Get<std::uint64_t>();
    }
    if (startup.sourceAvailable && (!LoadIntents(database, startup) || !LoadCommitments(database, startup) ||
                                    !LoadSourcePaths(database, startup) || !LoadOperations(database, startup)))
    {
        startup.sourceAvailable = false;
    }
    return startup;
}

void PlayerbotMaterialCommitmentPersistence::ProcessCallbacks() { transactionProcessor.ProcessReadyCallbacks(); }

namespace
{
PlayerbotMaterialCommitmentPersistence& MaterialCommitmentPersistence()
{
    static PlayerbotMaterialCommitmentPersistence persistence(PlayerbotsDatabase);
    return persistence;
}
}  // namespace

PlayerbotMaterialCommitmentAuthority& GetPlayerbotMaterialCommitmentAuthority()
{
    static PlayerbotMaterialCommitmentAuthority authority(
        [](std::uint64_t token, MaterialCommitmentWrite const& write)
        {
            MaterialCommitmentPersistence().QueueWrite(
                token, write, [](std::uint64_t completedToken, bool success)
                { GetPlayerbotMaterialCommitmentAuthority().CompleteWrite(completedToken, success); });
        });
    return authority;
}

void LoadPlayerbotMaterialCommitmentsFromDatabase()
{
    MaterialCommitmentStartup startup = MaterialCommitmentPersistence().Load();

    std::size_t const intents = startup.intents.size();
    std::size_t const commitments = startup.commitments.size();
    if (!GetPlayerbotMaterialCommitmentAuthority().Restore(std::move(startup)))
    {
        LOG_ERROR("playerbots.economy", "Material commitment authority persistence is unavailable or corrupt.");
        return;
    }
    LOG_INFO("playerbots.economy", "Loaded {} material intents and {} durable material commitments.", intents,
             commitments);
    CompactPlayerbotMaterialBook({}, true);
}

void CompactPlayerbotMaterialBook(std::vector<std::uint32_t> const& purgedGuids, bool ageBased)
{
    constexpr std::uint64_t staleAfterSeconds = 7u * 86'400u;
    PlayerbotMaterialCommitmentAuthority& authority = GetPlayerbotMaterialCommitmentAuthority();
    MaterialCommitmentSnapshot const snapshot = authority.Snapshot();
    if (!snapshot.persistenceHealthy)
        return;

    // Bots deleted by a wipe leave their intents behind; only the character table knows who is gone.
    std::vector<std::uint32_t> absent(purgedGuids.begin(), purgedGuids.end());
    if (ageBased)
    {
        std::set<std::uint32_t> referenced;
        for (MaterialIntent const& intent : snapshot.intents)
        {
            if (std::optional<std::uint32_t> const guid =
                    MaterialCommitmentEncoding::ProfessionProgressionOriginGuid(intent.originIdentity))
            {
                referenced.insert(*guid);
            }
        }
        if (!referenced.empty())
        {
            std::string list;
            for (std::uint32_t guid : referenced)
                list += (list.empty() ? "" : ",") + std::to_string(guid);
            std::set<std::uint32_t> present;
            if (QueryResult rows = CharacterDatabase.Query(
                    Acore::StringFormat("SELECT guid FROM characters WHERE guid IN ({})", list)))
            {
                do
                {
                    present.insert(rows->Fetch()[0].Get<std::uint32_t>());
                } while (rows->NextRow());
            }
            else if (!referenced.empty())
            {
                // An empty answer for a non-empty question is a failed query as often as a wiped realm;
                // neither is a reason to drop every intent, so the departed set stays empty.
                referenced.clear();
            }
            for (std::uint32_t guid : referenced)
            {
                if (!present.contains(guid))
                    absent.push_back(guid);
            }
        }
    }

    std::optional<MaterialCommitmentCommand> const command = MaterialCommitmentEncoding::BuildCompaction(
        snapshot, static_cast<std::uint64_t>(GameTime::GetGameTime().count()), absent,
        ageBased ? staleAfterSeconds : std::numeric_limits<std::uint64_t>::max(), MATERIAL_BOOK_RETAINED_OPERATIONS);
    if (!command)
        return;
    std::size_t const origins = command->originIdentities.size();
    std::size_t const commitments = command->commitmentIdentities.size();
    MaterialCommitmentApplyResult const applied =
        authority.Apply(*command, static_cast<std::uint64_t>(GameTime::GetGameTime().count()));
    if (applied.status == MaterialCommitmentApplyStatus::PendingPersistence)
    {
        LOG_INFO("playerbots.economy",
                 "Compacting the material book: dropping {} intents ({} for departed bots) and {} terminal "
                 "commitments, keeping the newest {} operations.",
                 origins, absent.size(), commitments, MATERIAL_BOOK_RETAINED_OPERATIONS);
    }
    else if (applied.status != MaterialCommitmentApplyStatus::Idempotent &&
             applied.status != MaterialCommitmentApplyStatus::Busy)
    {
        LOG_WARN("playerbots.economy", "Material book compaction refused with status {}.",
                 static_cast<unsigned>(applied.status));
    }
}

void UpdatePlayerbotMaterialCommitmentDatabaseCallbacks() { MaterialCommitmentPersistence().ProcessCallbacks(); }
}  // namespace PlayerbotEconomy
