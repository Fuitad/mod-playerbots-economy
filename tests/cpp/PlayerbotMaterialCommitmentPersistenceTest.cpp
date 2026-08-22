/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentEncoding.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentPersistence.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr std::uint64_t NOW = 10'000u;

class PlayerbotMaterialCommitmentPersistenceIntegrationTest : public testing::Test
{
protected:
    void SetUp() override
    {
        char const* connection = std::getenv("PLAYERBOTS_MATERIAL_COMMITMENT_TEST_DATABASE");
        char const* disposable = std::getenv("PLAYERBOTS_MATERIAL_COMMITMENT_TEST_DISPOSABLE");
        if (!connection || !*connection || !disposable || std::string(disposable) != "1")
            GTEST_SKIP() << "isolated material commitment schema was not explicitly configured";

        database = std::make_unique<DatabaseWorkerPool<PlayerbotsDatabaseConnection>>();
        database->SetConnectionInfo(connection, 1u, 1u);
        ASSERT_EQ(database->Open(), 0u);
        opened = true;
        QueryResult schemaResult = database->Query("SELECT DATABASE()");
        ASSERT_TRUE(schemaResult);
        ASSERT_FALSE(schemaResult->Fetch()[0].IsNull());
        std::string const schema = schemaResult->Fetch()[0].Get<std::string>();
        ASSERT_TRUE(schema.starts_with("tmp_playerbot_material_commitment_"))
            << "refusing destructive fixture against non-test schema " << schema;
        QueryResult tableCount = database->Query(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN "
            "('playerbot_economy_material_book', 'playerbot_economy_material_intent', "
            "'playerbot_economy_material_requirement', 'playerbot_economy_material_commitment', "
            "'playerbot_economy_material_reservation', 'playerbot_economy_material_source_path', "
            "'playerbot_economy_material_operation', "
            "'playerbot_economy_material_operation_commitment')");
        ASSERT_TRUE(tableCount);
        ASSERT_EQ(tableCount->Fetch()[0].Get<std::uint64_t>(), 8u)
            << "disposable schema must contain exactly the eight ledger tables before reset";
        safeToReset = true;
        persistence = std::make_unique<PlayerbotMaterialCommitmentPersistence>(*database);
        ResetDatabase();
        MaterialCommitmentStartup const empty = persistence->Load();
        ASSERT_TRUE(empty.sourceAvailable);
        EXPECT_EQ(empty.bookRevision, 0u);
        EXPECT_TRUE(empty.intents.empty());
        EXPECT_TRUE(empty.commitments.empty());
        EXPECT_TRUE(empty.operations.empty());
    }

    void TearDown() override
    {
        if (!opened)
            return;
        if (safeToReset)
            ResetDatabase();
        persistence.reset();
        database->Close();
    }

    void ResetDatabase()
    {
        database->DirectExecute("DELETE FROM playerbot_economy_material_operation_commitment");
        database->DirectExecute("DELETE FROM playerbot_economy_material_reservation");
        database->DirectExecute("DELETE FROM playerbot_economy_material_source_path");
        database->DirectExecute("DELETE FROM playerbot_economy_material_commitment");
        database->DirectExecute("DELETE FROM playerbot_economy_material_requirement");
        database->DirectExecute("DELETE FROM playerbot_economy_material_intent");
        database->DirectExecute("DELETE FROM playerbot_economy_material_operation");
        database->DirectExecute("UPDATE playerbot_economy_material_book SET book_revision = 0 WHERE singleton_id = 1");
    }

    std::unique_ptr<PlayerbotMaterialCommitmentAuthority> MakeAuthority()
    {
        auto target = std::make_shared<PlayerbotMaterialCommitmentAuthority*>(nullptr);
        auto authority = std::make_unique<PlayerbotMaterialCommitmentAuthority>(
            [this, target](std::uint64_t token, MaterialCommitmentWrite const& write)
            {
                persistence->QueueWrite(token, write, [target](std::uint64_t completedToken, bool success)
                                        { (*target)->CompleteWrite(completedToken, success); });
            });
        *target = authority.get();
        return authority;
    }

    void WaitForAcknowledgment(PlayerbotMaterialCommitmentAuthority& authority)
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (authority.Snapshot().busy && std::chrono::steady_clock::now() < deadline)
        {
            persistence->ProcessCallbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        persistence->ProcessCallbacks();
        ASSERT_FALSE(authority.Snapshot().busy);
    }

    std::unique_ptr<DatabaseWorkerPool<PlayerbotsDatabaseConnection>> database;
    std::unique_ptr<PlayerbotMaterialCommitmentPersistence> persistence;
    bool opened = false;
    bool safeToReset = false;
};
}  // namespace

TEST_F(PlayerbotMaterialCommitmentPersistenceIntegrationTest, RoundTripsAndRejectsStaleCompareAndSave)
{
    std::unique_ptr<PlayerbotMaterialCommitmentAuthority> authority = MakeAuthority();
    ASSERT_TRUE(authority->Restore(persistence->Load()));
    MaterialIntent intent{
        .originIdentity = "Progression:A",
        .ownerKind = MaterialCommitmentOwnerKind::GroupCommitment,
        .ownerRevision = 17u,
        .marketId = 4u,
        .boundedQuantity = 2u,
        .neededBy = NOW + 600u,
        .requirements = {{.itemId = 2589u, .quantity = 4u}},
    };
    MaterialCommitmentApplyResult observe = authority->Apply(
        {
            .operationIdentity = "Observe:A",
            .expectedBookRevision = 0u,
            .kind = MaterialCommitmentCommandKind::Observe,
            .intents = {intent},
        },
        NOW);
    ASSERT_EQ(observe.status, MaterialCommitmentApplyStatus::PendingPersistence);
    EXPECT_TRUE(authority->Snapshot().intents.empty());
    WaitForAcknowledgment(*authority);
    ASSERT_EQ(authority->Snapshot().intents.size(), 1u);

    MaterialCapacityKey listing{.kind = MaterialCapacityKind::AuctionListing, .authorityIdentity = "Listing:A"};
    MaterialCapacityKey money{.kind = MaterialCapacityKind::Money, .authorityIdentity = "Money:A"};
    MaterialCommitmentApplyResult admission = authority->Apply(
        {
            .operationIdentity = "Admit:A",
            .expectedBookRevision = 1u,
            .kind = MaterialCommitmentCommandKind::Admit,
            .candidates = {{
                .originIdentity = intent.originIdentity,
                .ownerRevision = intent.ownerRevision,
                .reservations =
                    {
                        {.materialItemId = 2589u,
                         .capacity = listing,
                         .authorityRevision = 23u,
                         .backedMaterialQuantity = 4u,
                         .capacityQuantity = 4u},
                        {.materialItemId = 2589u,
                         .capacity = money,
                         .authorityRevision = 29u,
                         .backedMaterialQuantity = 0u,
                         .capacityQuantity = 500u},
                    },
            }},
            .capacityObservations =
                {
                    {.capacity = listing,
                     .unit = MaterialCapacityUnit::ItemUnits,
                     .materialItemId = 2589u,
                     .authorityRevision = 23u,
                     .availableQuantity = 4u},
                    {.capacity = money,
                     .unit = MaterialCapacityUnit::Copper,
                     .materialItemId = 0u,
                     .authorityRevision = 29u,
                     .availableQuantity = 500u},
                },
        },
        NOW);
    ASSERT_EQ(admission.status, MaterialCommitmentApplyStatus::PendingPersistence);
    EXPECT_TRUE(admission.commitmentIdentities.empty());
    EXPECT_TRUE(authority->Snapshot().commitments.empty());
    WaitForAcknowledgment(*authority);

    MaterialCommitmentStartup roundTrip = persistence->Load();
    ASSERT_TRUE(roundTrip.sourceAvailable);
    EXPECT_EQ(roundTrip.bookRevision, 2u);
    ASSERT_EQ(roundTrip.intents.size(), 1u);
    EXPECT_EQ(roundTrip.intents.front().originIdentity, "Progression:A");
    EXPECT_EQ(roundTrip.intents.front().ownerKind, MaterialCommitmentOwnerKind::GroupCommitment);
    EXPECT_EQ(roundTrip.intents.front().firstObservedAt, NOW);
    EXPECT_EQ(roundTrip.intents.front().lastObservedAt, NOW);
    ASSERT_EQ(roundTrip.commitments.size(), 1u);
    MaterialCommitment const& commitment = roundTrip.commitments.front();
    EXPECT_EQ(commitment.originIdentity, "Progression:A");
    EXPECT_EQ(commitment.materialItemId, 2589u);
    EXPECT_EQ(commitment.remainingQuantity, 4u);
    ASSERT_EQ(commitment.reservations.size(), 2u);
    EXPECT_EQ(commitment.reservations[0].capacity, listing);
    EXPECT_EQ(commitment.reservations[0].remainingBackedMaterialQuantity, 4u);
    EXPECT_EQ(commitment.reservations[0].remainingCapacityQuantity, 4u);
    EXPECT_EQ(commitment.reservations[1].capacity, money);
    EXPECT_EQ(commitment.reservations[1].remainingBackedMaterialQuantity, 0u);
    EXPECT_EQ(commitment.reservations[1].remainingCapacityQuantity, 500u);
    ASSERT_EQ(roundTrip.operations.size(), 2u);
    ASSERT_EQ(roundTrip.operations.back().commitmentIdentities.size(), 1u);
    EXPECT_EQ(roundTrip.operations.back().commitmentIdentities.front(), commitment.identity);

    std::unique_ptr<PlayerbotMaterialCommitmentAuthority> first = MakeAuthority();
    std::unique_ptr<PlayerbotMaterialCommitmentAuthority> stale = MakeAuthority();
    ASSERT_TRUE(first->Restore(roundTrip));
    ASSERT_TRUE(stale->Restore(roundTrip));
    MaterialIntent firstIntent = intent;
    firstIntent.originIdentity = "Progression:B";
    firstIntent.ownerRevision = 18u;
    MaterialIntent staleIntent = intent;
    staleIntent.originIdentity = "Progression:C";
    staleIntent.ownerRevision = 19u;
    EXPECT_EQ(first
                  ->Apply({.operationIdentity = "Observe:B",
                           .expectedBookRevision = 2u,
                           .kind = MaterialCommitmentCommandKind::Observe,
                           .intents = {firstIntent}},
                          NOW + 1u)
                  .status,
              MaterialCommitmentApplyStatus::PendingPersistence);
    EXPECT_EQ(stale
                  ->Apply({.operationIdentity = "Observe:C",
                           .expectedBookRevision = 2u,
                           .kind = MaterialCommitmentCommandKind::Observe,
                           .intents = {staleIntent}},
                          NOW + 1u)
                  .status,
              MaterialCommitmentApplyStatus::PendingPersistence);
    WaitForAcknowledgment(*first);
    WaitForAcknowledgment(*stale);
    EXPECT_TRUE(first->Snapshot().persistenceHealthy);
    EXPECT_FALSE(stale->Snapshot().persistenceHealthy);

    MaterialCommitmentStartup afterRace = persistence->Load();
    ASSERT_TRUE(afterRace.sourceAvailable);
    EXPECT_EQ(afterRace.bookRevision, 3u);
    EXPECT_EQ(afterRace.intents.size(), 2u);
    EXPECT_NE(std::ranges::find(afterRace.intents, std::string("Progression:B"), &MaterialIntent::originIdentity),
              afterRace.intents.end());
    EXPECT_EQ(std::ranges::find(afterRace.intents, std::string("Progression:C"), &MaterialIntent::originIdentity),
              afterRace.intents.end());
}

TEST_F(PlayerbotMaterialCommitmentPersistenceIntegrationTest, RoundTripsActiveSameActorGatheringPath)
{
    std::unique_ptr<PlayerbotMaterialCommitmentAuthority> authority = MakeAuthority();
    ASSERT_TRUE(authority->Restore(persistence->Load()));
    MaterialIntent intent{
        .originIdentity = "profession-progression:10:164:75:2660:2862",
        .ownerKind = MaterialCommitmentOwnerKind::ProfessionProgression,
        .ownerRevision = 1u,
        .marketId = 2u,
        .boundedQuantity = 1u,
        .requirements = {{.itemId = 2770u, .quantity = 1u}},
    };
    ASSERT_EQ(authority
                  ->Apply({.operationIdentity = "observe:gathering",
                           .expectedBookRevision = 0u,
                           .kind = MaterialCommitmentCommandKind::Observe,
                           .intents = {intent}},
                          NOW)
                  .status,
              MaterialCommitmentApplyStatus::PendingPersistence);
    WaitForAcknowledgment(*authority);

    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const built =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath({
            .actorGuid = 10u,
            .materialItemId = 2770u,
            .selectedQuantity = 1u,
            .gatheringSkillId = 186u,
            .sourceEntry = 1'733u,
            .sourceMapId = 0u,
            .routeIdentity = "node:1733:map:0",
            .capacityIdentity = "same-actor-gathering:10:186:2770:1733:0",
            .selectedAt = NOW,
            .sourceTravelBudgetSeconds = 10u,
            .destinationConservativeYieldBasisPoints = 10'000u,
            .authoritativeInteractionSeconds = 5u,
            .remainingDedicatedActivitySeconds = 30u,
            .completionObservationBudgetSeconds = 20u,
            .availableResourceCount = 1u,
        });
    ASSERT_EQ(built.status, MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);
    ASSERT_TRUE(built.path);
    MaterialCapacityKey capacity{MaterialCapacityKind::GatheringCapacity, built.path->capacityIdentity};
    MaterialCommitmentApplyResult const admission =
        authority->Apply({.operationIdentity = "admit:gathering",
                          .expectedBookRevision = 1u,
                          .kind = MaterialCommitmentCommandKind::Admit,
                          .candidates = {{.originIdentity = intent.originIdentity,
                                          .ownerRevision = 1u,
                                          .reservations = {{.materialItemId = 2770u,
                                                            .capacity = capacity,
                                                            .authorityRevision = built.path->sourceRevision,
                                                            .backedMaterialQuantity = 1u,
                                                            .capacityQuantity = 1u}},
                                          .sourcePaths = {*built.path}}},
                          .capacityObservations = {{.capacity = capacity,
                                                    .unit = MaterialCapacityUnit::GatheringUnits,
                                                    .materialItemId = 2770u,
                                                    .authorityRevision = built.path->sourceRevision,
                                                    .availableQuantity = 1u}}},
                         NOW);
    ASSERT_EQ(admission.status, MaterialCommitmentApplyStatus::PendingPersistence);
    WaitForAcknowledgment(*authority);
    MaterialCommitmentSnapshot admitted = authority->Snapshot();
    ASSERT_EQ(admitted.commitments.size(), 1u);
    ASSERT_EQ(authority
                  ->Apply({.operationIdentity = "start:gathering",
                           .expectedBookRevision = 2u,
                           .kind = MaterialCommitmentCommandKind::StartSource,
                           .sourceStarts = {{.commitmentIdentity = admitted.commitments.front().identity,
                                             .expectedSourceRevision = built.path->sourceRevision,
                                             .startingInventoryQuantity = 2u}}},
                          NOW)
                  .status,
              MaterialCommitmentApplyStatus::PendingPersistence);
    WaitForAcknowledgment(*authority);

    MaterialCommitmentStartup const roundTrip = persistence->Load();
    ASSERT_TRUE(roundTrip.sourceAvailable);
    ASSERT_EQ(roundTrip.commitments.size(), 1u);
    ASSERT_TRUE(roundTrip.commitments.front().sourcePath);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->phase, MaterialSourcePhase::Acquiring);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->startingInventoryQuantity, 2u);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->neededBy, NOW + 60u);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->sourceRevision, built.path->sourceRevision);
}

TEST_F(PlayerbotMaterialCommitmentPersistenceIntegrationTest, RoundTripsActiveSameActorHuntingPath)
{
    std::unique_ptr<PlayerbotMaterialCommitmentAuthority> authority = MakeAuthority();
    ASSERT_TRUE(authority->Restore(persistence->Load()));
    MaterialIntent intent{
        .originIdentity = "profession-progression:10:185:75:2538:2679",
        .ownerKind = MaterialCommitmentOwnerKind::ProfessionProgression,
        .ownerRevision = 1u,
        .marketId = 2u,
        .boundedQuantity = 1u,
        .requirements = {{.itemId = 2672u, .quantity = 1u}},
    };
    ASSERT_EQ(authority
                  ->Apply({.operationIdentity = "observe:hunting",
                           .expectedBookRevision = 0u,
                           .kind = MaterialCommitmentCommandKind::Observe,
                           .intents = {intent}},
                          NOW)
                  .status,
              MaterialCommitmentApplyStatus::PendingPersistence);
    WaitForAcknowledgment(*authority);

    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const built =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath({
            .kind = MaterialSourceKind::SameActorHunting,
            .actorGuid = 10u,
            .materialItemId = 2672u,
            .selectedQuantity = 1u,
            .gatheringSkillId = 0u,
            .sourceEntry = 299u,
            .sourceMapId = 0u,
            .routeIdentity = "hunting-route:299:0",
            .capacityIdentity = "same-actor-hunting:10:2672:299:0",
            .selectedAt = NOW,
            .sourceTravelBudgetSeconds = 10u,
            .destinationConservativeYieldBasisPoints = 10'000u,
            .authoritativeInteractionSeconds = 5u,
            .remainingDedicatedActivitySeconds = 30u,
            .completionObservationBudgetSeconds = 20u,
            .availableResourceCount = 1u,
        });
    ASSERT_EQ(built.status, MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);
    ASSERT_TRUE(built.path);
    MaterialCapacityKey capacity{MaterialCapacityKind::GatheringCapacity, built.path->capacityIdentity};
    MaterialCommitmentApplyResult const admission =
        authority->Apply({.operationIdentity = "admit:hunting",
                          .expectedBookRevision = 1u,
                          .kind = MaterialCommitmentCommandKind::Admit,
                          .candidates = {{.originIdentity = intent.originIdentity,
                                          .ownerRevision = 1u,
                                          .reservations = {{.materialItemId = 2672u,
                                                            .capacity = capacity,
                                                            .authorityRevision = built.path->sourceRevision,
                                                            .backedMaterialQuantity = 1u,
                                                            .capacityQuantity = 1u}},
                                          .sourcePaths = {*built.path}}},
                          .capacityObservations = {{.capacity = capacity,
                                                    .unit = MaterialCapacityUnit::GatheringUnits,
                                                    .materialItemId = 2672u,
                                                    .authorityRevision = built.path->sourceRevision,
                                                    .availableQuantity = 1u}}},
                         NOW);
    ASSERT_EQ(admission.status, MaterialCommitmentApplyStatus::PendingPersistence);
    WaitForAcknowledgment(*authority);
    MaterialCommitmentSnapshot admitted = authority->Snapshot();
    ASSERT_EQ(admitted.commitments.size(), 1u);
    ASSERT_EQ(authority
                  ->Apply({.operationIdentity = "start:hunting",
                           .expectedBookRevision = 2u,
                           .kind = MaterialCommitmentCommandKind::StartSource,
                           .sourceStarts = {{.commitmentIdentity = admitted.commitments.front().identity,
                                             .expectedSourceRevision = built.path->sourceRevision,
                                             .startingInventoryQuantity = 2u}}},
                          NOW)
                  .status,
              MaterialCommitmentApplyStatus::PendingPersistence);
    WaitForAcknowledgment(*authority);

    MaterialCommitmentStartup const roundTrip = persistence->Load();
    ASSERT_TRUE(roundTrip.sourceAvailable);
    ASSERT_EQ(roundTrip.commitments.size(), 1u);
    ASSERT_TRUE(roundTrip.commitments.front().sourcePath);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->kind, MaterialSourceKind::SameActorHunting);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->gatheringSkillId, 0u);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->phase, MaterialSourcePhase::Acquiring);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->startingInventoryQuantity, 2u);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->neededBy, NOW + 60u);
    EXPECT_EQ(roundTrip.commitments.front().sourcePath->sourceRevision, built.path->sourceRevision);
}
