/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentEncoding.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr std::uint64_t NOW = 1'000u;

MaterialCapacityKey Capacity(std::string identity = "inventory:bot:10:item:2589")
{
    return {.kind = MaterialCapacityKind::OwnedItem, .authorityIdentity = std::move(identity)};
}

MaterialCapacityKey MoneyCapacity(std::string identity = "money:bot:10")
{
    return {.kind = MaterialCapacityKind::Money, .authorityIdentity = std::move(identity)};
}

MaterialCapacityKey AuctionCapacity(std::string identity = "auction:listing:42")
{
    return {.kind = MaterialCapacityKind::AuctionListing, .authorityIdentity = std::move(identity)};
}

MaterialIntent Intent(std::string identity, std::uint32_t quantity, std::optional<std::uint64_t> neededBy)
{
    return {
        .originIdentity = std::move(identity),
        .ownerRevision = 7u,
        .marketId = 2u,
        .boundedQuantity = quantity,
        .neededBy = neededBy,
        .requirements = {{.itemId = 2589u, .quantity = quantity}},
    };
}

MaterialCommitmentCommand Observe(std::string operation, std::uint64_t revision, std::vector<MaterialIntent> intents)
{
    return {
        .operationIdentity = std::move(operation),
        .expectedBookRevision = revision,
        .kind = MaterialCommitmentCommandKind::Observe,
        .intents = std::move(intents),
    };
}

MaterialCommitmentCommand Admit(std::string operation, std::uint64_t revision,
                                std::vector<MaterialAdmissionCandidate> candidates,
                                std::vector<MaterialCapacityObservation> observations)
{
    return {
        .operationIdentity = std::move(operation),
        .expectedBookRevision = revision,
        .kind = MaterialCommitmentCommandKind::Admit,
        .candidates = std::move(candidates),
        .capacityObservations = std::move(observations),
    };
}

MaterialAdmissionCandidate Candidate(std::string origin, std::uint32_t quantity,
                                     MaterialCapacityKey capacity = Capacity())
{
    return {
        .originIdentity = std::move(origin),
        .ownerRevision = 7u,
        .reservations = {{.materialItemId = 2589u,
                          .capacity = std::move(capacity),
                          .authorityRevision = 11u,
                          .backedMaterialQuantity = quantity,
                          .capacityQuantity = quantity}},
    };
}

MaterialCapacityObservation Observation(std::uint64_t quantity, MaterialCapacityKey capacity = Capacity(),
                                        std::uint32_t materialItemId = 2589u,
                                        MaterialCapacityUnit unit = MaterialCapacityUnit::ItemUnits)
{
    return {
        .capacity = std::move(capacity),
        .unit = unit,
        .materialItemId = materialItemId,
        .authorityRevision = 11u,
        .availableQuantity = quantity,
    };
}

MaterialCommitmentEncoding::SameActorGatheringPathInput GatheringPathInput()
{
    return {
        .actorGuid = 10u,
        .materialItemId = 2589u,
        .selectedQuantity = 4u,
        .gatheringSkillId = 186u,
        .sourceEntry = 1'733u,
        .sourceMapId = 0u,
        .routeIdentity = "node:1733:map:0",
        .capacityIdentity = "same-actor-gathering:10:186:2589:1733:0",
        .selectedAt = NOW,
        .sourceTravelBudgetSeconds = 30u,
        .destinationConservativeYieldBasisPoints = 6'000u,
        .observedGatheredQuantity = 3u,
        .observedResourceAttempts = 5u,
        .observedResourceSeconds = 50u,
        .authoritativeInteractionSeconds = 5u,
        .remainingDedicatedActivitySeconds = 40u,
        .deliveryTravelBudgetSeconds = 0u,
        .completionObservationBudgetSeconds = 60u,
        .startingInventoryQuantity = 0u,
        .availableResourceCount = 8u,
    };
}

class AuthorityHarness
{
public:
    AuthorityHarness()
        : authority([this](std::uint64_t token, MaterialCommitmentWrite const& write)
                    { writes.push_back({token, write}); })
    {
        MaterialCommitmentStartup empty;
        empty.sourceAvailable = true;
        if (!authority.Restore(std::move(empty)))
            ADD_FAILURE() << "explicit empty startup must restore";
    }

    MaterialCommitmentApplyResult Apply(MaterialCommitmentCommand command)
    {
        return authority.Apply(std::move(command), NOW);
    }

    MaterialCommitmentSnapshot Commit(MaterialCommitmentCommand command)
    {
        MaterialCommitmentApplyResult result = Apply(std::move(command));
        if (result.status != MaterialCommitmentApplyStatus::PendingPersistence || result.writeToken == 0u)
        {
            ADD_FAILURE() << "mutation was not staged for persistence";
            return authority.Snapshot();
        }
        EXPECT_TRUE(result.commitmentIdentities.empty());
        authority.CompleteWrite(result.writeToken, true);
        return authority.Snapshot();
    }

    struct CapturedWrite
    {
        std::uint64_t token = 0u;
        MaterialCommitmentWrite write;
    };

    std::vector<CapturedWrite> writes;
    PlayerbotMaterialCommitmentAuthority authority;
};
}  // namespace

TEST(PlayerbotMaterialCommitmentAuthorityTest, ProfessionProgressionIdentityEncodesStableMilestoneFacts)
{
    MaterialCommitmentEncoding::ProfessionProgressionIntentInput input{
        .characterGuid = 42u,
        .marketId = 7u,
        .professionSkillId = 185u,
        .targetSkill = 75u,
        .recipeSpellId = 37836u,
        .outputItemId = 30816u,
        .boundedBatch = 5u,
        .scarceRequirements = {{.itemId = 2678u, .quantity = 5u}, {.itemId = 2692u, .quantity = 5u}},
    };
    std::string const origin = MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(input);
    EXPECT_FALSE(origin.empty());
    EXPECT_EQ(MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(input), origin);

    MaterialCommitmentEncoding::ProfessionProgressionIntentInput changed = input;
    changed.characterGuid++;
    EXPECT_NE(MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(changed), origin);
    changed = input;
    changed.professionSkillId++;
    EXPECT_NE(MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(changed), origin);
    changed = input;
    changed.targetSkill++;
    EXPECT_NE(MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(changed), origin);
    changed = input;
    changed.recipeSpellId++;
    EXPECT_NE(MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(changed), origin);
    changed = input;
    changed.outputItemId++;
    EXPECT_NE(MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(changed), origin);
    changed = input;
    changed.boundedBatch--;
    EXPECT_EQ(MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(changed), origin);

    std::string const firstOperation =
        MaterialCommitmentEncoding::ProfessionProgressionObserveOperationIdentity(origin, 1u);
    EXPECT_EQ(MaterialCommitmentEncoding::ProfessionProgressionObserveOperationIdentity(origin, 1u), firstOperation);
    EXPECT_NE(MaterialCommitmentEncoding::ProfessionProgressionObserveOperationIdentity(origin, 2u), firstOperation);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ProfessionProgressionScarceBillIsCompleteAndExcludesVendorSupply)
{
    std::optional<std::vector<MaterialRequirement>> const requirements =
        MaterialCommitmentEncoding::ProfessionProgressionScarceRequirements(
            3u, {{.itemId = 2678u, .perCraftQuantity = 1u},
                 {.itemId = 2692u, .perCraftQuantity = 2u},
                 {.itemId = 2692u, .perCraftQuantity = 1u},
                 {.itemId = 2880u, .perCraftQuantity = 1u, .ordinaryVendorAvailable = true}});

    ASSERT_TRUE(requirements.has_value());
    EXPECT_EQ(*requirements,
              (std::vector<MaterialRequirement>{{.itemId = 2678u, .quantity = 3u}, {.itemId = 2692u, .quantity = 9u}}));
    EXPECT_FALSE(MaterialCommitmentEncoding::ProfessionProgressionScarceRequirements(
                     1u, {{.itemId = 2880u, .perCraftQuantity = 1u, .ordinaryVendorAvailable = true}})
                     .has_value());
    EXPECT_FALSE(MaterialCommitmentEncoding::ProfessionProgressionScarceRequirements(
                     1u, {{.itemId = 2678u, .perCraftQuantity = 1u}, {.itemId = 0u, .perCraftQuantity = 1u}})
                     .has_value());
    EXPECT_FALSE(MaterialCommitmentEncoding::ProfessionProgressionScarceRequirements(
                     1u, {{.itemId = 2678u, .perCraftQuantity = 1u},
                          {.itemId = 0u, .perCraftQuantity = 1u, .ordinaryVendorAvailable = true}})
                     .has_value());
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ProfessionProgressionScarceBillNamesTheHerbBehindAMilledPigment)
{
    // Three Ivory Ink crafts need three Alabaster Pigment. Pigment is never gathered or sold, so the bill
    // asks for the herb the bot will mill: two castings of five Peacebloom cover three pigments.
    std::optional<std::vector<MaterialRequirement>> const requirements =
        MaterialCommitmentEncoding::ProfessionProgressionScarceRequirements(
            3u, {{.itemId = 39151u, .perCraftQuantity = 1u, .millingInputItemId = 2447u},
                 {.itemId = 39354u, .perCraftQuantity = 1u, .ordinaryVendorAvailable = true}});
    ASSERT_TRUE(requirements.has_value());
    EXPECT_EQ(*requirements, (std::vector<MaterialRequirement>{{.itemId = 2447u, .quantity = 10u}}));

    // Two recipes sharing the herb add up under the herb, and a pigment without a known input stays a
    // pigment requirement so the blocker still names what is missing.
    std::optional<std::vector<MaterialRequirement>> const mixed =
        MaterialCommitmentEncoding::ProfessionProgressionScarceRequirements(
            1u, {{.itemId = 39151u, .perCraftQuantity = 2u, .millingInputItemId = 2447u},
                 {.itemId = 39334u, .perCraftQuantity = 1u, .millingInputItemId = 2447u},
                 {.itemId = 43103u, .perCraftQuantity = 1u}});
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(*mixed, (std::vector<MaterialRequirement>{{.itemId = 2447u, .quantity = 10u},
                                                        {.itemId = 43103u, .quantity = 1u}}));
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ProfessionProgressionProducerStopsEveryObserveOutcome)
{
    AuthorityHarness harness;
    MaterialCommitmentEncoding::ProfessionProgressionIntentInput input{
        .characterGuid = 42u,
        .marketId = 7u,
        .professionSkillId = 185u,
        .targetSkill = 75u,
        .recipeSpellId = 37836u,
        .outputItemId = 30816u,
        .boundedBatch = 5u,
        .scarceRequirements = {{.itemId = 2692u, .quantity = 5u}, {.itemId = 2678u, .quantity = 5u}},
    };
    MaterialCommitmentEncoding::ProfessionProgressionObserveBuildResult const build =
        MaterialCommitmentEncoding::BuildProfessionProgressionObserve(input, harness.authority.Snapshot());
    ASSERT_EQ(build.status, MaterialCommitmentEncoding::ProfessionProgressionObserveBuildStatus::Command);
    ASSERT_TRUE(build.command.has_value());
    EXPECT_EQ(build.command->kind, MaterialCommitmentCommandKind::Observe);
    EXPECT_TRUE(build.command->candidates.empty());
    EXPECT_TRUE(build.command->capacityObservations.empty());
    EXPECT_TRUE(build.command->fulfillments.empty());
    EXPECT_TRUE(build.command->commitmentIdentities.empty());
    ASSERT_EQ(build.command->intents.size(), 1u);
    MaterialIntent const& intent = build.command->intents.front();
    EXPECT_EQ(intent.ownerKind, MaterialCommitmentOwnerKind::ProfessionProgression);
    EXPECT_EQ(intent.ownerRevision, 1u);
    EXPECT_EQ(intent.marketId, 7u);
    EXPECT_EQ(intent.boundedQuantity, 5u);
    EXPECT_FALSE(intent.neededBy.has_value());
    EXPECT_EQ(intent.requirements,
              (std::vector<MaterialRequirement>{{.itemId = 2678u, .quantity = 5u}, {.itemId = 2692u, .quantity = 5u}}));

    auto blockedCycle = [](std::optional<MaterialCommitmentEncoding::ProfessionProgressionIntentInput> observedInput,
                           MaterialCommitmentSnapshot const& snapshot, PlayerbotMaterialCommitmentAuthority& authority,
                           std::uint64_t now)
    {
        return MaterialCommitmentEncoding::ObserveBlockedProfessionProgression(
            {.intent = std::move(observedInput),
             .recipeSpellId = 37836u,
             .materialItemId = 2678u,
             .blockerCode = "profession_material_source_unavailable"},
            snapshot, authority, now);
    };
    auto expectBlocked = [](MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const& blocked)
    {
        EXPECT_EQ(blocked.cycleResult.outcome, PlayerbotEconomyCycleOutcome::NoCandidate);
        EXPECT_EQ(blocked.cycleResult.phase, EconomyPhase::Craft);
        EXPECT_EQ(blocked.cycleResult.workIdentity.spellId, 37836u);
        EXPECT_EQ(blocked.cycleResult.workIdentity.itemId, 2678u);
        EXPECT_EQ(blocked.cycleResult.blocker,
                  "profession_material_source_unavailable:item:2678:owned_or_ordinary_vendor");
        EXPECT_EQ(blocked.cycleResult.schedulingEffect, EconomyAttemptOutcome::NoCandidate);
    };

    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const pending =
        blockedCycle(input, harness.authority.Snapshot(), harness.authority, NOW);
    expectBlocked(pending);
    ASSERT_EQ(pending.observationStatus,
              MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::PendingPersistence);
    EXPECT_TRUE(harness.authority.Snapshot().intents.empty());
    EXPECT_TRUE(harness.authority.Snapshot().commitments.empty());
    ASSERT_EQ(harness.writes.size(), 1u);

    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const busy =
        blockedCycle(input, harness.authority.Snapshot(), harness.authority, NOW + 1u);
    expectBlocked(busy);
    EXPECT_EQ(busy.observationStatus, MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::Busy);
    EXPECT_EQ(harness.writes.size(), 1u);
    harness.authority.CompleteWrite(harness.writes.front().token, true);

    MaterialCommitmentSnapshot const acknowledged = harness.authority.Snapshot();
    ASSERT_EQ(acknowledged.intents.size(), 1u);
    EXPECT_EQ(acknowledged.intents.front().firstObservedAt, NOW);
    EXPECT_TRUE(acknowledged.commitments.empty());
    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const noChange =
        blockedCycle(input, acknowledged, harness.authority, NOW + 2u);
    expectBlocked(noChange);
    EXPECT_EQ(noChange.observationStatus, MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::NoChange);
    EXPECT_EQ(harness.writes.size(), 1u);
    EXPECT_EQ(harness.authority.Snapshot().intents.front().firstObservedAt, NOW);

    PlayerbotMaterialCommitmentAuthority restored([](std::uint64_t, MaterialCommitmentWrite const&) {});
    ASSERT_TRUE(restored.Restore(harness.writes.back().write.replacement));
    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const restartNoChange =
        blockedCycle(input, restored.Snapshot(), restored, NOW + 3u);
    expectBlocked(restartNoChange);
    EXPECT_EQ(restartNoChange.observationStatus,
              MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::NoChange);

    input.boundedBatch = 4u;
    input.scarceRequirements = {{.itemId = 2678u, .quantity = 4u}, {.itemId = 2692u, .quantity = 4u}};
    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const revised =
        blockedCycle(input, harness.authority.Snapshot(), harness.authority, NOW + 4u);
    expectBlocked(revised);
    ASSERT_EQ(revised.observationStatus,
              MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::PendingPersistence);
    harness.authority.CompleteWrite(harness.writes.back().token, true);
    MaterialCommitmentSnapshot const revisionTwo = harness.authority.Snapshot();
    ASSERT_EQ(revisionTwo.intents.size(), 1u);
    EXPECT_EQ(revisionTwo.intents.front().originIdentity, acknowledged.intents.front().originIdentity);
    EXPECT_EQ(revisionTwo.intents.front().ownerRevision, 2u);
    EXPECT_EQ(revisionTwo.intents.front().firstObservedAt, NOW);
    EXPECT_EQ(revisionTwo.intents.front().lastObservedAt, NOW + 4u);
    EXPECT_EQ(revisionTwo.intents.front().boundedQuantity, 4u);
    EXPECT_TRUE(revisionTwo.commitments.empty());

    AuthorityHarness staleHarness;
    MaterialCommitmentSnapshot const staleSnapshot = staleHarness.authority.Snapshot();
    staleHarness.Commit(Observe("advance-book", 0u, {Intent("unrelated", 1u, {})}));
    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const stale =
        blockedCycle(input, staleSnapshot, staleHarness.authority, NOW + 5u);
    expectBlocked(stale);
    EXPECT_EQ(stale.observationStatus, MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::Stale);
    EXPECT_EQ(staleHarness.authority.Snapshot().bookRevision, 1u);
    EXPECT_TRUE(staleHarness.authority.Snapshot().commitments.empty());

    input.boundedBatch = 3u;
    input.scarceRequirements = {{.itemId = 2678u, .quantity = 3u}, {.itemId = 2692u, .quantity = 3u}};
    MaterialCommitmentEncoding::ProfessionProgressionIntentInput invalid = input;
    invalid.scarceRequirements.clear();
    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const invalidResult =
        blockedCycle(invalid, revisionTwo, harness.authority, NOW + 6u);
    expectBlocked(invalidResult);
    EXPECT_EQ(invalidResult.observationStatus, MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::Invalid);
    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const absentIntent =
        blockedCycle(std::nullopt, revisionTwo, harness.authority, NOW + 6u);
    expectBlocked(absentIntent);
    EXPECT_EQ(absentIntent.observationStatus, MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::Invalid);

    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const failedWrite =
        blockedCycle(input, revisionTwo, harness.authority, NOW + 7u);
    expectBlocked(failedWrite);
    ASSERT_EQ(failedWrite.observationStatus,
              MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::PendingPersistence);
    harness.authority.CompleteWrite(harness.writes.back().token, false);
    input.boundedBatch = 2u;
    input.scarceRequirements = {{.itemId = 2678u, .quantity = 2u}, {.itemId = 2692u, .quantity = 2u}};
    MaterialCommitmentEncoding::ProfessionProgressionBlockedCycleResult const unavailable =
        blockedCycle(input, harness.authority.Snapshot(), harness.authority, NOW + 8u);
    expectBlocked(unavailable);
    EXPECT_EQ(unavailable.observationStatus,
              MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::PersistenceUnavailable);
    EXPECT_EQ(harness.authority.Snapshot().intents, revisionTwo.intents);
    EXPECT_TRUE(harness.authority.Snapshot().commitments.empty());
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, MissingHorizonRemainsDurableLatentAndNonExecutable)
{
    AuthorityHarness harness;
    MaterialCommitmentApplyResult const result =
        harness.Apply(Observe("observe-latent", 0u, {Intent("first-aid", 8u, {})}));

    EXPECT_EQ(result.status, MaterialCommitmentApplyStatus::PendingPersistence);
    EXPECT_TRUE(harness.authority.Snapshot().intents.empty());
    EXPECT_TRUE(harness.authority.Snapshot().commitments.empty());

    harness.authority.CompleteWrite(result.writeToken, true);
    MaterialCommitmentSnapshot const snapshot = harness.authority.Snapshot();
    ASSERT_EQ(snapshot.intents.size(), 1u);
    EXPECT_FALSE(snapshot.intents.front().neededBy.has_value());
    EXPECT_TRUE(snapshot.commitments.empty());

    MaterialCommitmentApplyResult const admission =
        harness.Apply(Admit("admit-latent", snapshot.bookRevision, {Candidate("first-aid", 8u)}, {Observation(8u)}));
    EXPECT_EQ(admission.status, MaterialCommitmentApplyStatus::MissingHorizon);
    EXPECT_EQ(admission.writeToken, 0u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, SameActorGatheringDerivesHorizonAndCompletesOnlyAfterInventoryReceipt)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe-gathering", 0u, {Intent("mining", 4u, std::nullopt)}));

    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const path =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(GatheringPathInput());
    ASSERT_EQ(path.status, MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);
    ASSERT_TRUE(path.path.has_value());
    EXPECT_EQ(path.path->conservativeYieldBasisPoints, 6'000u);
    EXPECT_EQ(path.path->requiredResourceCount, 7u);
    EXPECT_EQ(path.path->secondsPerResource, 10u);
    EXPECT_EQ(path.path->sourceActionBudgetSeconds, 70u);
    EXPECT_EQ(path.path->neededBy, NOW + 30u + 70u + 60u);

    MaterialCapacityKey capacity{
        .kind = MaterialCapacityKind::GatheringCapacity,
        .authorityIdentity = path.path->capacityIdentity,
    };
    MaterialAdmissionCandidate candidate{
        .originIdentity = "mining",
        .ownerRevision = 7u,
        .reservations = {{.materialItemId = 2589u,
                          .capacity = capacity,
                          .authorityRevision = path.path->sourceRevision,
                          .backedMaterialQuantity = 4u,
                          .capacityQuantity = path.path->requiredResourceCount}},
        .sourcePaths = {*path.path},
    };
    MaterialAdmissionCandidate forgedBaseline = candidate;
    forgedBaseline.sourcePaths.front().startingInventoryQuantity = 1u;
    EXPECT_EQ(harness
                  .Apply(Admit("admit-forged-baseline", 1u, {forgedBaseline},
                               {{.capacity = capacity,
                                 .unit = MaterialCapacityUnit::GatheringUnits,
                                 .materialItemId = 2589u,
                                 .authorityRevision = path.path->sourceRevision,
                                 .availableQuantity = path.path->availableResourceCount}}))
                  .status,
              MaterialCommitmentApplyStatus::InvalidCommand);
    MaterialCommitmentSnapshot const admitted =
        harness.Commit(Admit("admit-gathering", 1u, {candidate},
                             {{.capacity = capacity,
                               .unit = MaterialCapacityUnit::GatheringUnits,
                               .materialItemId = 2589u,
                               .authorityRevision = path.path->sourceRevision,
                               .availableQuantity = path.path->availableResourceCount}}));

    ASSERT_EQ(admitted.intents.size(), 1u);
    EXPECT_EQ(admitted.intents.front().neededBy, path.path->neededBy);
    ASSERT_EQ(admitted.commitments.size(), 1u);
    ASSERT_TRUE(admitted.commitments.front().sourcePath.has_value());
    EXPECT_EQ(admitted.commitments.front().sourcePath->phase, MaterialSourcePhase::Selected);

    std::optional<MaterialCommitmentWrite> releaseWrite;
    PlayerbotMaterialCommitmentAuthority releaseAuthority(
        [&releaseWrite](std::uint64_t, MaterialCommitmentWrite const& write) { releaseWrite = write; });
    ASSERT_TRUE(releaseAuthority.Restore(harness.writes.back().write.replacement));
    MaterialCommitmentApplyResult const released =
        releaseAuthority.Apply({.operationIdentity = "release-gathering",
                                .expectedBookRevision = admitted.bookRevision,
                                .kind = MaterialCommitmentCommandKind::Release,
                                .commitmentIdentities = {admitted.commitments.front().identity}},
                               NOW);
    ASSERT_EQ(released.status, MaterialCommitmentApplyStatus::PendingPersistence);
    ASSERT_TRUE(releaseWrite);
    ASSERT_EQ(releaseWrite->replacement.commitments.size(), 1u);
    EXPECT_EQ(releaseWrite->replacement.commitments.front().state, MaterialCommitmentState::Released);
    ASSERT_TRUE(releaseWrite->replacement.commitments.front().sourcePath);
    EXPECT_EQ(releaseWrite->replacement.commitments.front().sourcePath->phase, MaterialSourcePhase::Released);

    MaterialCommitmentSnapshot const started = harness.Commit({
        .operationIdentity = "start-gathering",
        .expectedBookRevision = admitted.bookRevision,
        .kind = MaterialCommitmentCommandKind::StartSource,
        .sourceStarts = {{.commitmentIdentity = admitted.commitments.front().identity,
                          .expectedSourceRevision = path.path->sourceRevision,
                          .startingInventoryQuantity = 6u}},
    });
    ASSERT_EQ(started.commitments.size(), 1u);
    ASSERT_TRUE(started.commitments.front().sourcePath.has_value());
    EXPECT_EQ(started.commitments.front().sourcePath->phase, MaterialSourcePhase::Acquiring);
    EXPECT_EQ(started.commitments.front().sourcePath->startingInventoryQuantity, 6u);
    PlayerbotMaterialCommitmentAuthority restarted([](std::uint64_t, MaterialCommitmentWrite const&) {});
    ASSERT_TRUE(restarted.Restore(harness.writes.back().write.replacement));
    ASSERT_EQ(restarted.Snapshot().commitments.size(), 1u);
    ASSERT_TRUE(restarted.Snapshot().commitments.front().sourcePath.has_value());
    EXPECT_EQ(restarted.Snapshot().commitments.front().sourcePath->phase, MaterialSourcePhase::Acquiring);
    MaterialCommitmentStartup corruptPath = harness.writes.back().write.replacement;
    corruptPath.commitments.front().reservations.front().capacity.authorityIdentity = "foreign-capacity";
    PlayerbotMaterialCommitmentAuthority corruptRestart([](std::uint64_t, MaterialCommitmentWrite const&) {});
    EXPECT_FALSE(corruptRestart.Restore(std::move(corruptPath)));

    MaterialCommitment const& active = started.commitments.front();
    EXPECT_EQ(harness
                  .Apply({.operationIdentity = "partial-gathering",
                          .expectedBookRevision = started.bookRevision,
                          .kind = MaterialCommitmentCommandKind::Fulfill,
                          .fulfillments = {{.commitmentIdentity = active.identity,
                                            .quantity = 2u,
                                            .observedInventoryQuantity = 8u,
                                            .reservationSettlements = {{.capacity = capacity,
                                                                        .backedMaterialQuantity = 2u,
                                                                        .capacityQuantity = 3u}}}}})
                  .status,
              MaterialCommitmentApplyStatus::InvalidCommand);
    EXPECT_EQ(harness
                  .Apply({.operationIdentity = "unobserved-gathering",
                          .expectedBookRevision = started.bookRevision,
                          .kind = MaterialCommitmentCommandKind::Fulfill,
                          .fulfillments = {{.commitmentIdentity = active.identity,
                                            .quantity = 4u,
                                            .reservationSettlements = {{.capacity = capacity,
                                                                        .backedMaterialQuantity = 4u,
                                                                        .capacityQuantity = 7u}}}}})
                  .status,
              MaterialCommitmentApplyStatus::InvalidCommand);
    std::string const commitmentIdentity = active.identity;
    std::string const originIdentity = active.originIdentity;
    std::uint32_t const materialItemId = active.materialItemId;
    std::uint32_t const boundedQuantity = active.boundedQuantity;
    std::uint64_t const neededBy = active.neededBy;
    MaterialCommitmentApplyResult const settlement =
        SettleCompletedMaterialSource(harness.authority, started.bookRevision, active, 10u, NOW);
    ASSERT_EQ(settlement.status, MaterialCommitmentApplyStatus::PendingPersistence);
    ASSERT_NE(settlement.writeToken, 0u);
    MaterialCommitmentSnapshot const beforePersistence = harness.authority.Snapshot();
    ASSERT_EQ(beforePersistence.commitments.size(), 1u);
    EXPECT_EQ(beforePersistence.commitments.front().state, MaterialCommitmentState::Admitted);
    EXPECT_EQ(beforePersistence.commitments.front().reservations.size(), 1u);
    ASSERT_TRUE(beforePersistence.commitments.front().sourcePath.has_value());
    EXPECT_EQ(beforePersistence.commitments.front().sourcePath->phase, MaterialSourcePhase::Acquiring);

    harness.authority.CompleteWrite(settlement.writeToken, true);
    MaterialCommitmentSnapshot const completed = harness.authority.Snapshot();
    ASSERT_EQ(completed.commitments.size(), 1u);
    EXPECT_EQ(completed.commitments.front().state, MaterialCommitmentState::Completed);
    EXPECT_TRUE(completed.commitments.front().reservations.empty());
    ASSERT_TRUE(completed.commitments.front().sourcePath.has_value());
    EXPECT_EQ(completed.commitments.front().sourcePath->phase, MaterialSourcePhase::Completed);
    EXPECT_EQ(completed.commitments.front().identity, commitmentIdentity);
    EXPECT_EQ(completed.commitments.front().originIdentity, originIdentity);
    EXPECT_EQ(completed.commitments.front().materialItemId, materialItemId);
    EXPECT_EQ(completed.commitments.front().boundedQuantity, boundedQuantity);
    EXPECT_EQ(completed.commitments.front().neededBy, neededBy);
    EXPECT_EQ(completed.commitments.front().sourcePath->capacityIdentity, capacity.authorityIdentity);
    ASSERT_TRUE(restarted.Restore(harness.writes.back().write.replacement));
    EXPECT_EQ(restarted.Snapshot().commitments.front().sourcePath->phase, MaterialSourcePhase::Completed);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, SameActorGatheringDerivesColdStartAndRejectsIncompleteOrNonNodeEvidence)
{
    MaterialCommitmentEncoding::SameActorGatheringPathInput coldStart = GatheringPathInput();
    coldStart.observedGatheredQuantity = 0u;
    coldStart.observedResourceAttempts = 0u;
    coldStart.observedResourceSeconds = 0u;
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const coldPath =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(coldStart);
    ASSERT_EQ(coldPath.status, MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);
    ASSERT_TRUE(coldPath.path);
    EXPECT_EQ(coldPath.path->secondsPerResource, 20u);
    EXPECT_EQ(coldPath.path->sourceActionBudgetSeconds, 140u);

    MaterialCommitmentEncoding::SameActorGatheringPathInput multiItemNode = coldStart;
    multiItemNode.destinationConservativeYieldBasisPoints = 20'000u;
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const multiItemPath =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(multiItemNode);
    ASSERT_EQ(multiItemPath.status, MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);
    ASSERT_TRUE(multiItemPath.path);
    EXPECT_EQ(multiItemPath.path->requiredResourceCount, 2u);
    EXPECT_EQ(multiItemPath.path->secondsPerResource, 40u);
    EXPECT_EQ(multiItemPath.path->sourceActionBudgetSeconds, 80u);

    MaterialCommitmentEncoding::SameActorGatheringPathInput missingObservation = GatheringPathInput();
    missingObservation.completionObservationBudgetSeconds = 0u;
    EXPECT_EQ(MaterialCommitmentEncoding::BuildSameActorGatheringPath(missingObservation).status,
              MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Invalid);

    MaterialCommitmentEncoding::SameActorGatheringPathInput missingColdStart = GatheringPathInput();
    missingColdStart.remainingDedicatedActivitySeconds = 0u;
    EXPECT_EQ(MaterialCommitmentEncoding::BuildSameActorGatheringPath(missingColdStart).status,
              MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Invalid);

    // Skinning sources leather from creature spawns the travel catalog already models as nodes, so a
    // leatherworking progression can source its own material the same way herbs and ore do.
    MaterialCommitmentEncoding::SameActorGatheringPathInput skinning = GatheringPathInput();
    skinning.gatheringSkillId = 393u;
    EXPECT_EQ(MaterialCommitmentEncoding::BuildSameActorGatheringPath(skinning).status,
              MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);

    // A crafting profession still cannot gather its own input.
    MaterialCommitmentEncoding::SameActorGatheringPathInput tailoring = GatheringPathInput();
    tailoring.gatheringSkillId = 197u;
    EXPECT_EQ(MaterialCommitmentEncoding::BuildSameActorGatheringPath(tailoring).status,
              MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Invalid);

    // Four items at a 60 percent yield need seven spawns. Six spawned nodes back three items, so the
    // path shrinks to what the map can deliver instead of refusing: a herb bill that waited for
    // fifteen spawned nodes at once never left the latent state on live.
    MaterialCommitmentEncoding::SameActorGatheringPathInput insufficientCapacity = GatheringPathInput();
    insufficientCapacity.availableResourceCount = 6u;
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const partial =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(insufficientCapacity);
    ASSERT_EQ(partial.status, MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);
    ASSERT_TRUE(partial.path);
    EXPECT_EQ(partial.path->selectedQuantity, 3u);
    EXPECT_EQ(partial.path->requiredResourceCount, 5u);
    EXPECT_EQ(partial.path->sourceActionBudgetSeconds, 50u);
    EXPECT_EQ(partial.path->availableResourceCount, 6u);
    // Admission and restore rebuild a path from its own fields and demand equality, so the clamped
    // path has to be its own fixed point.
    MaterialCommitmentEncoding::SameActorGatheringPathInput rebuilt = insufficientCapacity;
    rebuilt.selectedQuantity = partial.path->selectedQuantity;
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const rebuiltPath =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(rebuilt);
    ASSERT_TRUE(rebuiltPath.path);
    EXPECT_EQ(*rebuiltPath.path, *partial.path);

    // One spawned node at that yield backs no whole item: nothing to admit.
    MaterialCommitmentEncoding::SameActorGatheringPathInput noWholeItem = GatheringPathInput();
    noWholeItem.availableResourceCount = 1u;
    EXPECT_EQ(MaterialCommitmentEncoding::BuildSameActorGatheringPath(noWholeItem).status,
              MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Invalid);

    // A one node path keeps the whole activity window: required times the observed seconds per node
    // gave a 10 second action budget, and live such trips expired before the bot touched a node
    // (49 of 57 released paths gathered nothing on 2026-09-01).
    MaterialCommitmentEncoding::SameActorGatheringPathInput oneNode = GatheringPathInput();
    oneNode.destinationConservativeYieldBasisPoints = 10'000u;
    oneNode.observedGatheredQuantity = 5u;
    oneNode.availableResourceCount = 1u;
    oneNode.remainingDedicatedActivitySeconds = 300u;
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const oneNodePath =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(oneNode);
    ASSERT_TRUE(oneNodePath.path);
    EXPECT_EQ(oneNodePath.path->selectedQuantity, 1u);
    EXPECT_EQ(oneNodePath.path->requiredResourceCount, 1u);
    EXPECT_EQ(oneNodePath.path->secondsPerResource, 10u);
    EXPECT_EQ(oneNodePath.path->sourceActionBudgetSeconds, 300u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, SameActorHuntingPathNeedsNoSkillAndSurvivesAdmissionAndRestore)
{
    // A mob drop has no gathering skill behind it: the actor kills the creature and loots it. The
    // hunting kind therefore carries skill id 0 and nothing else, and the gathering kind keeps
    // refusing skill id 0 so the two cannot be confused for one another.
    MaterialCommitmentEncoding::SameActorGatheringPathInput hunting = GatheringPathInput();
    hunting.kind = MaterialSourceKind::SameActorHunting;
    hunting.gatheringSkillId = 0u;
    hunting.materialItemId = 2672u;
    hunting.sourceEntry = 299u;
    hunting.capacityIdentity = "same-actor-hunting:10:2672:299:0";
    hunting.routeIdentity = "hunting-route:299:0";
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const built =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(hunting);
    ASSERT_EQ(built.status, MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Path);
    ASSERT_TRUE(built.path);
    EXPECT_EQ(built.path->kind, MaterialSourceKind::SameActorHunting);
    EXPECT_EQ(built.path->gatheringSkillId, 0u);
    EXPECT_EQ(built.path->requiredResourceCount, 7u);

    MaterialCommitmentEncoding::SameActorGatheringPathInput huntingWithSkill = hunting;
    huntingWithSkill.gatheringSkillId = 186u;
    EXPECT_EQ(MaterialCommitmentEncoding::BuildSameActorGatheringPath(huntingWithSkill).status,
              MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Invalid);

    MaterialCommitmentEncoding::SameActorGatheringPathInput gatheringWithoutSkill = GatheringPathInput();
    gatheringWithoutSkill.gatheringSkillId = 0u;
    EXPECT_EQ(MaterialCommitmentEncoding::BuildSameActorGatheringPath(gatheringWithoutSkill).status,
              MaterialCommitmentEncoding::SameActorGatheringPathBuildStatus::Invalid);

    MaterialCommitmentEncoding::SameActorGatheringPathInput relabeled = hunting;
    relabeled.kind = MaterialSourceKind::SameActorGathering;
    relabeled.gatheringSkillId = 186u;
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const relabeledPath =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath(relabeled);
    ASSERT_TRUE(relabeledPath.path);
    EXPECT_NE(relabeledPath.path->sourceRevision, built.path->sourceRevision);

    AuthorityHarness harness;
    // The intent must want the hunted material itself: admission matches the path against the
    // intent's requirement item, so the fixture's default Coarse Stone (2589) would be rejected.
    MaterialIntent huntingIntent = Intent("cooking", 4u, std::nullopt);
    huntingIntent.requirements = {{.itemId = 2672u, .quantity = 4u}};
    harness.Commit(Observe("observe-hunting", 0u, {huntingIntent}));
    MaterialCapacityKey capacity{
        .kind = MaterialCapacityKind::GatheringCapacity,
        .authorityIdentity = built.path->capacityIdentity,
    };
    MaterialAdmissionCandidate candidate{
        .originIdentity = "cooking",
        .ownerRevision = 7u,
        .reservations = {{.materialItemId = 2672u,
                          .capacity = capacity,
                          .authorityRevision = built.path->sourceRevision,
                          .backedMaterialQuantity = 4u,
                          .capacityQuantity = built.path->requiredResourceCount}},
        .sourcePaths = {*built.path},
    };
    MaterialAdmissionCandidate forgedKind = candidate;
    forgedKind.sourcePaths.front().kind = MaterialSourceKind::SameActorGathering;
    EXPECT_EQ(harness
                  .Apply(Admit("admit-forged-kind", 1u, {forgedKind},
                               {{.capacity = capacity,
                                 .unit = MaterialCapacityUnit::GatheringUnits,
                                 .materialItemId = 2672u,
                                 .authorityRevision = built.path->sourceRevision,
                                 .availableQuantity = built.path->availableResourceCount}}))
                  .status,
              MaterialCommitmentApplyStatus::InvalidCommand);
    MaterialCommitmentSnapshot const admitted =
        harness.Commit(Admit("admit-hunting", 1u, {candidate},
                             {{.capacity = capacity,
                               .unit = MaterialCapacityUnit::GatheringUnits,
                               .materialItemId = 2672u,
                               .authorityRevision = built.path->sourceRevision,
                               .availableQuantity = built.path->availableResourceCount}}));
    ASSERT_EQ(admitted.commitments.size(), 1u);
    ASSERT_TRUE(admitted.commitments.front().sourcePath.has_value());
    EXPECT_EQ(admitted.commitments.front().sourcePath->kind, MaterialSourceKind::SameActorHunting);

    MaterialCommitmentSnapshot const started = harness.Commit({
        .operationIdentity = "start-hunting",
        .expectedBookRevision = admitted.bookRevision,
        .kind = MaterialCommitmentCommandKind::StartSource,
        .sourceStarts = {{.commitmentIdentity = admitted.commitments.front().identity,
                          .expectedSourceRevision = built.path->sourceRevision,
                          .startingInventoryQuantity = 0u}},
    });
    ASSERT_EQ(started.commitments.size(), 1u);
    EXPECT_EQ(started.commitments.front().sourcePath->phase, MaterialSourcePhase::Acquiring);
    PlayerbotMaterialCommitmentAuthority restarted([](std::uint64_t, MaterialCommitmentWrite const&) {});
    ASSERT_TRUE(restarted.Restore(harness.writes.back().write.replacement));
    ASSERT_EQ(restarted.Snapshot().commitments.size(), 1u);
    EXPECT_EQ(restarted.Snapshot().commitments.front().sourcePath->kind, MaterialSourceKind::SameActorHunting);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, UnrestoredAuthorityFailsClosed)
{
    PlayerbotMaterialCommitmentAuthority authority([](std::uint64_t, MaterialCommitmentWrite const&) {});
    EXPECT_EQ(authority.Apply(Observe("observe", 0u, {Intent("latent", 1u, {})}), NOW).status,
              MaterialCommitmentApplyStatus::PersistenceUnavailable);
    EXPECT_FALSE(authority.Snapshot().persistenceHealthy);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, AdmissionRejectsEqualOrPastAbsoluteHorizon)
{
    AuthorityHarness equal;
    equal.Commit(Observe("observe-equal", 0u, {Intent("equal", 1u, NOW)}));
    EXPECT_EQ(equal.Apply(Admit("admit-equal", 1u, {Candidate("equal", 1u)}, {Observation(1u)})).status,
              MaterialCommitmentApplyStatus::MissingHorizon);

    AuthorityHarness past;
    past.Commit(Observe("observe-past", 0u, {Intent("past", 1u, NOW - 1u)}));
    EXPECT_EQ(past.Apply(Admit("admit-past", 1u, {Candidate("past", 1u)}, {Observation(1u)})).status,
              MaterialCommitmentApplyStatus::MissingHorizon);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ReobservationPreservesWaitingStartAndUpdatesLastObservation)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe-first", 0u, {Intent("tailoring", 6u, {})}));
    MaterialCommitmentSnapshot firstSnapshot = harness.authority.Snapshot();
    ASSERT_EQ(firstSnapshot.intents.size(), 1u);
    MaterialIntent const first = firstSnapshot.intents.front();
    EXPECT_EQ(first.firstObservedAt, NOW);
    EXPECT_EQ(first.lastObservedAt, NOW);

    MaterialIntent conflicting = Intent("tailoring", 6u, {});
    conflicting.ownerKind = MaterialCommitmentOwnerKind::StockMaintenance;
    EXPECT_EQ(harness.authority.Apply(Observe("observe-conflict", 1u, {conflicting}), NOW + 10u).status,
              MaterialCommitmentApplyStatus::StaleOwnerRevision);
    EXPECT_EQ(harness.writes.size(), 1u);

    MaterialIntent updated = Intent("tailoring", 6u, NOW + 300u);
    updated.ownerKind = MaterialCommitmentOwnerKind::StockMaintenance;
    updated.ownerRevision = 8u;
    MaterialCommitmentApplyResult const pending =
        harness.authority.Apply(Observe("observe-again", 1u, {updated}), NOW + 20u);
    ASSERT_EQ(pending.status, MaterialCommitmentApplyStatus::PendingPersistence);
    harness.authority.CompleteWrite(pending.writeToken, true);

    MaterialCommitmentSnapshot observedSnapshot = harness.authority.Snapshot();
    ASSERT_EQ(observedSnapshot.intents.size(), 1u);
    MaterialIntent const observed = observedSnapshot.intents.front();
    EXPECT_EQ(observed.firstObservedAt, NOW);
    EXPECT_EQ(observed.lastObservedAt, NOW + 20u);
    EXPECT_EQ(observed.ownerKind, MaterialCommitmentOwnerKind::StockMaintenance);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, SnapshotPublishesOnlyAcknowledgedReplacementAndFailureBlocksMutation)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe-old", 0u, {Intent("tailoring", 6u, {})}));
    MaterialCommitmentSnapshot const oldSnapshot = harness.authority.Snapshot();

    MaterialIntent updated = Intent("tailoring", 6u, NOW + 300u);
    updated.ownerRevision = 8u;
    MaterialCommitmentApplyResult const pending =
        harness.Apply(Observe("observe-new", oldSnapshot.bookRevision, {updated}));
    EXPECT_EQ(pending.status, MaterialCommitmentApplyStatus::PendingPersistence);
    ASSERT_EQ(harness.authority.Snapshot().intents.size(), 1u);
    EXPECT_EQ(harness.authority.Snapshot().intents.front().neededBy, std::nullopt);
    EXPECT_EQ(harness.Apply(Observe("busy", oldSnapshot.bookRevision, {})).status, MaterialCommitmentApplyStatus::Busy);

    harness.authority.CompleteWrite(pending.writeToken, false);
    MaterialCommitmentSnapshot const failed = harness.authority.Snapshot();
    ASSERT_EQ(failed.intents.size(), 1u);
    EXPECT_EQ(failed.intents.front().neededBy, std::nullopt);
    EXPECT_FALSE(failed.persistenceHealthy);
    EXPECT_EQ(harness.Apply(Observe("blocked", failed.bookRevision, {})).status,
              MaterialCommitmentApplyStatus::PersistenceUnavailable);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, SelectedSetAdmissionIsAtomicAndCannotDoubleReserve)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe-two", 0u, {Intent("bandages", 6u, NOW + 300u), Intent("bolts", 5u, NOW + 300u)}));

    MaterialCommitmentApplyResult const result =
        harness.Apply(Admit("admit-two", 1u, {Candidate("bandages", 6u), Candidate("bolts", 5u)}, {Observation(10u)}));

    EXPECT_EQ(result.status, MaterialCommitmentApplyStatus::InsufficientCapacity);
    EXPECT_EQ(result.writeToken, 0u);
    EXPECT_TRUE(harness.authority.Snapshot().commitments.empty());
    EXPECT_FALSE(harness.authority.Snapshot().busy);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, MultiItemAdmissionRequiresExactPerItemBacking)
{
    AuthorityHarness harness;
    MaterialIntent recipe = Intent("recipe", 2u, NOW + 300u);
    recipe.requirements = {{.itemId = 2589u, .quantity = 4u}, {.itemId = 2320u, .quantity = 2u}};
    harness.Commit(Observe("observe-recipe", 0u, {recipe}));

    MaterialAdmissionCandidate missing = Candidate("recipe", 4u);
    EXPECT_EQ(harness.Apply(Admit("missing-item", 1u, {missing}, {Observation(4u)})).status,
              MaterialCommitmentApplyStatus::InsufficientCapacity);

    MaterialAdmissionCandidate wrong = Candidate("recipe", 4u);
    wrong.reservations.push_back({.materialItemId = 9999u,
                                  .capacity = Capacity("vendor:thread"),
                                  .authorityRevision = 11u,
                                  .backedMaterialQuantity = 2u,
                                  .capacityQuantity = 2u});
    EXPECT_EQ(
        harness.Apply(Admit("wrong-item", 1u, {wrong}, {Observation(4u), Observation(2u, Capacity("vendor:thread"))}))
            .status,
        MaterialCommitmentApplyStatus::InvalidCommand);

    MaterialAdmissionCandidate over = Candidate("recipe", 5u);
    over.reservations.push_back({.materialItemId = 2320u,
                                 .capacity = Capacity("vendor:thread"),
                                 .authorityRevision = 11u,
                                 .backedMaterialQuantity = 2u,
                                 .capacityQuantity = 2u});
    EXPECT_EQ(
        harness.Apply(Admit("over-backed", 1u, {over}, {Observation(5u), Observation(2u, Capacity("vendor:thread"))}))
            .status,
        MaterialCommitmentApplyStatus::InvalidCommand);

    MaterialAdmissionCandidate exact = Candidate("recipe", 4u);
    exact.reservations.push_back({.materialItemId = 2320u,
                                  .capacity = Capacity("vendor:thread"),
                                  .authorityRevision = 11u,
                                  .backedMaterialQuantity = 2u,
                                  .capacityQuantity = 2u});
    MaterialCommitmentSnapshot const snapshot = harness.Commit(
        Admit("exact", 1u, {exact}, {Observation(4u), Observation(2u, Capacity("vendor:thread"), 2320u)}));
    ASSERT_EQ(snapshot.commitments.size(), 2u);
    EXPECT_FALSE(snapshot.commitments[0].identity.empty());
    EXPECT_FALSE(snapshot.commitments[1].identity.empty());
    EXPECT_NE(snapshot.commitments[0].identity, snapshot.commitments[1].identity);
    EXPECT_EQ(snapshot.commitments[0].materialItemId, 2589u);
    EXPECT_EQ(snapshot.commitments[0].boundedQuantity, 4u);
    EXPECT_EQ(snapshot.commitments[1].materialItemId, 2320u);
    EXPECT_EQ(snapshot.commitments[1].boundedQuantity, 2u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, CapacityKindCannotBeRelabeledAndMoneyNeverBacksMaterialUnits)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));

    MaterialAdmissionCandidate relabeled = Candidate("bandages", 4u, Capacity("shared-authority"));
    MaterialCapacityObservation moneyObservation =
        Observation(400u, MoneyCapacity("shared-authority"), 0u, MaterialCapacityUnit::Copper);
    EXPECT_EQ(harness.Apply(Admit("relabel", 1u, {relabeled}, {moneyObservation})).status,
              MaterialCommitmentApplyStatus::StaleCapacityRevision);

    MaterialAdmissionCandidate moneyBacked = Candidate("bandages", 3u);
    moneyBacked.reservations.push_back({
        .materialItemId = 2589u,
        .capacity = MoneyCapacity(),
        .authorityRevision = 11u,
        .backedMaterialQuantity = 1u,
        .capacityQuantity = 100u,
    });
    EXPECT_EQ(harness
                  .Apply(Admit("money-as-item", 1u, {moneyBacked},
                               {Observation(3u), Observation(100u, MoneyCapacity(), 0u, MaterialCapacityUnit::Copper)}))
                  .status,
              MaterialCommitmentApplyStatus::InvalidCommand);

    MaterialAdmissionCandidate exact = Candidate("bandages", 4u);
    exact.reservations.push_back({
        .materialItemId = 2589u,
        .capacity = MoneyCapacity(),
        .authorityRevision = 11u,
        .backedMaterialQuantity = 0u,
        .capacityQuantity = 100u,
    });
    MaterialCommitmentSnapshot const admitted =
        harness.Commit(Admit("item-plus-money", 1u, {exact},
                             {Observation(4u), Observation(100u, MoneyCapacity(), 0u, MaterialCapacityUnit::Copper)}));
    ASSERT_EQ(admitted.commitments.size(), 1u);
    ASSERT_EQ(admitted.commitments.front().reservations.size(), 2u);
    EXPECT_EQ(admitted.commitments.front().reservations[0].remainingBackedMaterialQuantity, 4u);
    EXPECT_EQ(admitted.commitments.front().reservations[1].remainingBackedMaterialQuantity, 0u);
    EXPECT_EQ(admitted.commitments.front().reservations[1].remainingCapacityQuantity, 100u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ItemUnitAdmissionRequiresMatchingMaterialAndCapacityQuantities)
{
    for (MaterialCapacityKey capacity : {Capacity(), AuctionCapacity()})
    {
        AuthorityHarness harness;
        harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
        MaterialAdmissionCandidate candidate = Candidate("bandages", 4u, capacity);
        ASSERT_EQ(candidate.reservations.size(), 1u);
        candidate.reservations.front().capacityQuantity = 5u;

        MaterialCommitmentApplyResult const result =
            harness.Apply(Admit("admit", 1u, {candidate}, {Observation(5u, capacity)}));

        EXPECT_EQ(result.status, MaterialCommitmentApplyStatus::InvalidCommand);
        EXPECT_EQ(result.writeToken, 0u);
        EXPECT_EQ(harness.writes.size(), 1u);
        EXPECT_TRUE(harness.authority.Snapshot().commitments.empty());
    }
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, WorkCapacitiesKeepIndependentMaterialAndNativeQuantities)
{
    struct WorkCapacity
    {
        MaterialCapacityKind kind;
        MaterialCapacityUnit unit;
    };
    for (WorkCapacity const work : {
             WorkCapacity{MaterialCapacityKind::GatheringCapacity, MaterialCapacityUnit::GatheringUnits},
             WorkCapacity{MaterialCapacityKind::ProductionCapacity, MaterialCapacityUnit::ProductionUnits},
         })
    {
        AuthorityHarness harness;
        harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
        MaterialCapacityKey capacity{.kind = work.kind, .authorityIdentity = "work:bot:10:item:2589"};
        MaterialAdmissionCandidate candidate = Candidate("bandages", 4u, capacity);
        ASSERT_EQ(candidate.reservations.size(), 1u);
        candidate.reservations.front().capacityQuantity = 2u;

        MaterialCommitmentSnapshot const admitted =
            harness.Commit(Admit("admit", 1u, {candidate}, {Observation(2u, capacity, 2589u, work.unit)}));

        ASSERT_EQ(admitted.commitments.size(), 1u);
        ASSERT_EQ(admitted.commitments.front().reservations.size(), 1u);
        EXPECT_EQ(admitted.commitments.front().reservations.front().remainingBackedMaterialQuantity, 4u);
        EXPECT_EQ(admitted.commitments.front().reservations.front().remainingCapacityQuantity, 2u);
    }
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, CapacityMaterialIdentityCannotBeRelabeled)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialAdmissionCandidate candidate = Candidate("bandages", 4u, Capacity("inventory:shared-key"));
    MaterialCapacityObservation relabeled = Observation(4u, Capacity("inventory:shared-key"), 2320u);

    MaterialCommitmentApplyResult const result = harness.Apply(Admit("relabel-item", 1u, {candidate}, {relabeled}));

    EXPECT_EQ(result.status, MaterialCommitmentApplyStatus::InvalidCommand);
    EXPECT_EQ(result.writeToken, 0u);
    EXPECT_EQ(harness.writes.size(), 1u);
    EXPECT_TRUE(harness.authority.Snapshot().commitments.empty());
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, MaterialBackingOverflowFailsClosed)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 1u, NOW + 300u)}));
    MaterialAdmissionCandidate candidate = Candidate("bandages", 1u);
    ASSERT_EQ(candidate.reservations.size(), 1u);
    candidate.reservations.front().backedMaterialQuantity = std::numeric_limits<std::uint64_t>::max();
    candidate.reservations.front().capacityQuantity = std::numeric_limits<std::uint64_t>::max();
    candidate.reservations.push_back({
        .materialItemId = 2589u,
        .capacity = Capacity("inventory:second-stack"),
        .authorityRevision = 11u,
        .backedMaterialQuantity = 2u,
        .capacityQuantity = 2u,
    });
    EXPECT_EQ(harness
                  .Apply(Admit("overflow", 1u, {candidate},
                               {Observation(std::numeric_limits<std::uint64_t>::max()),
                                Observation(2u, Capacity("inventory:second-stack"))}))
                  .status,
              MaterialCommitmentApplyStatus::InvalidCommand);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, PersistedIdentityLimitsFailBeforeWrite)
{
    AuthorityHarness harness;
    std::string const tooLong(192u, 'x');
    EXPECT_EQ(harness.Apply(Observe(tooLong, 0u, {Intent("bandages", 1u, {})})).status,
              MaterialCommitmentApplyStatus::InvalidCommand);
    EXPECT_EQ(harness.Apply(Observe("operation", 0u, {Intent(tooLong, 1u, {})})).status,
              MaterialCommitmentApplyStatus::InvalidCommand);
    EXPECT_TRUE(harness.writes.empty());
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ExistingAdmissionIsRetainedAndCannotBeStolen)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe-first", 0u, {Intent("bandages", 6u, NOW + 300u)}));
    harness.Commit(Admit("admit-first", 1u, {Candidate("bandages", 6u)}, {Observation(10u)}));
    harness.Commit(Observe("observe-second", 2u, {Intent("bolts", 5u, NOW + 300u)}));

    MaterialCommitmentApplyResult const result =
        harness.Apply(Admit("admit-second", 3u, {Candidate("bolts", 5u)}, {Observation(10u)}));

    EXPECT_EQ(result.status, MaterialCommitmentApplyStatus::InsufficientCapacity);
    MaterialCommitmentSnapshot const snapshot = harness.authority.Snapshot();
    ASSERT_EQ(snapshot.commitments.size(), 1u);
    EXPECT_EQ(snapshot.commitments.front().originIdentity, "bandages");
    ASSERT_EQ(snapshot.commitments.front().reservations.size(), 1u);
    EXPECT_EQ(snapshot.commitments.front().reservations.front().remainingBackedMaterialQuantity, 6u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, AdmissionRejectsStaleBookOwnerAndCapacityRevisions)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));

    EXPECT_EQ(harness.Apply(Admit("stale-book", 0u, {Candidate("bandages", 4u)}, {Observation(4u)})).status,
              MaterialCommitmentApplyStatus::StaleBookRevision);

    MaterialAdmissionCandidate staleOwner = Candidate("bandages", 4u);
    staleOwner.ownerRevision = 6u;
    EXPECT_EQ(harness.Apply(Admit("stale-owner", 1u, {staleOwner}, {Observation(4u)})).status,
              MaterialCommitmentApplyStatus::StaleOwnerRevision);

    MaterialAdmissionCandidate staleCapacity = Candidate("bandages", 4u);
    ASSERT_EQ(staleCapacity.reservations.size(), 1u);
    staleCapacity.reservations.front().authorityRevision = 10u;
    EXPECT_EQ(harness.Apply(Admit("stale-capacity", 1u, {staleCapacity}, {Observation(4u)})).status,
              MaterialCommitmentApplyStatus::StaleCapacityRevision);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, OperationIdentityIsIdempotentAndConflictingReuseFailsClosed)
{
    AuthorityHarness harness;
    MaterialCommitmentCommand command = Observe("repeatable", 0u, {Intent("cooking", 2u, {})});
    MaterialCommitmentSnapshot const committed = harness.Commit(command);

    MaterialCommitmentApplyResult const repeated = harness.Apply(command);
    EXPECT_EQ(repeated.status, MaterialCommitmentApplyStatus::Idempotent);
    EXPECT_TRUE(repeated.commitmentIdentities.empty());
    EXPECT_EQ(repeated.writeToken, 0u);

    ASSERT_EQ(command.intents.size(), 1u);
    command.intents.front().boundedQuantity = 3u;
    EXPECT_EQ(harness.Apply(command).status, MaterialCommitmentApplyStatus::OperationConflict);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, FulfillmentTransitionsThroughPartialAndCompletedAtomically)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 10u, NOW + 300u)}));
    MaterialCommitmentSnapshot const admitted =
        harness.Commit(Admit("admit", 1u, {Candidate("bandages", 10u)}, {Observation(10u)}));
    ASSERT_EQ(admitted.commitments.size(), 1u);
    std::string const identity = admitted.commitments.front().identity;

    MaterialCommitmentCommand partial{
        .operationIdentity = "partial",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Fulfill,
        .fulfillments = {{.commitmentIdentity = identity,
                          .quantity = 4u,
                          .reservationSettlements =
                              {{.capacity = Capacity(), .backedMaterialQuantity = 4u, .capacityQuantity = 4u}}}},
    };
    harness.Commit(std::move(partial));
    MaterialCommitmentSnapshot partlySnapshot = harness.authority.Snapshot();
    ASSERT_EQ(partlySnapshot.commitments.size(), 1u);
    MaterialCommitment const partly = partlySnapshot.commitments.front();
    EXPECT_EQ(partly.state, MaterialCommitmentState::PartiallyFulfilled);
    EXPECT_EQ(partly.remainingQuantity, 6u);
    ASSERT_EQ(partly.reservations.size(), 1u);
    EXPECT_EQ(partly.reservations.front().remainingBackedMaterialQuantity, 6u);
    EXPECT_EQ(partly.reservations.front().remainingCapacityQuantity, 6u);

    MaterialCommitmentCommand complete{
        .operationIdentity = "complete",
        .expectedBookRevision = 3u,
        .kind = MaterialCommitmentCommandKind::Fulfill,
        .fulfillments = {{.commitmentIdentity = identity, .quantity = 6u}},
    };
    harness.Commit(std::move(complete));
    MaterialCommitmentSnapshot completedSnapshot = harness.authority.Snapshot();
    ASSERT_EQ(completedSnapshot.commitments.size(), 1u);
    MaterialCommitment const completed = completedSnapshot.commitments.front();
    EXPECT_EQ(completed.state, MaterialCommitmentState::Completed);
    EXPECT_EQ(completed.remainingQuantity, 0u);
    EXPECT_TRUE(completed.reservations.empty());
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, SettlementUsesIndependentMaterialAndNativeCapacityUnits)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialAdmissionCandidate candidate = Candidate("bandages", 4u);
    candidate.reservations.push_back({
        .materialItemId = 2589u,
        .capacity = MoneyCapacity(),
        .authorityRevision = 11u,
        .backedMaterialQuantity = 0u,
        .capacityQuantity = 100u,
    });
    MaterialCommitmentSnapshot const admitted =
        harness.Commit(Admit("admit", 1u, {candidate},
                             {Observation(4u), Observation(100u, MoneyCapacity(), 0u, MaterialCapacityUnit::Copper)}));
    ASSERT_EQ(admitted.commitments.size(), 1u);

    harness.Commit({
        .operationIdentity = "settle",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Fulfill,
        .fulfillments = {{
            .commitmentIdentity = admitted.commitments.front().identity,
            .quantity = 2u,
            .reservationSettlements =
                {{.capacity = Capacity(), .backedMaterialQuantity = 2u, .capacityQuantity = 2u},
                 {.capacity = MoneyCapacity(), .backedMaterialQuantity = 0u, .capacityQuantity = 50u}},
        }},
    });
    MaterialCommitmentSnapshot const partly = harness.authority.Snapshot();
    ASSERT_EQ(partly.commitments.size(), 1u);
    ASSERT_EQ(partly.commitments.front().reservations.size(), 2u);
    EXPECT_EQ(partly.commitments.front().remainingQuantity, 2u);
    EXPECT_EQ(partly.commitments.front().reservations[0].remainingBackedMaterialQuantity, 2u);
    EXPECT_EQ(partly.commitments.front().reservations[1].remainingCapacityQuantity, 50u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ItemUnitSettlementRequiresMatchingMaterialAndCapacityQuantities)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialCommitmentSnapshot const admitted =
        harness.Commit(Admit("admit", 1u, {Candidate("bandages", 4u)}, {Observation(4u)}));
    ASSERT_EQ(admitted.commitments.size(), 1u);

    MaterialCommitmentApplyResult const result = harness.Apply({
        .operationIdentity = "mismatched-item-settlement",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Fulfill,
        .fulfillments = {{
            .commitmentIdentity = admitted.commitments.front().identity,
            .quantity = 1u,
            .reservationSettlements = {{.capacity = Capacity(), .backedMaterialQuantity = 1u, .capacityQuantity = 2u}},
        }},
    });

    EXPECT_EQ(result.status, MaterialCommitmentApplyStatus::InvalidCommand);
    EXPECT_EQ(result.writeToken, 0u);
    EXPECT_EQ(harness.writes.size(), 2u);
    EXPECT_FALSE(harness.authority.Snapshot().busy);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, NonMoneyCapacityCannotRemainAfterItsBackingIsSettled)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialAdmissionCandidate candidate = Candidate("bandages", 2u);
    candidate.reservations.push_back({
        .materialItemId = 2589u,
        .capacity = Capacity("inventory:second-stack"),
        .authorityRevision = 11u,
        .backedMaterialQuantity = 2u,
        .capacityQuantity = 2u,
    });
    MaterialCommitmentSnapshot const admitted = harness.Commit(
        Admit("admit", 1u, {candidate}, {Observation(2u), Observation(2u, Capacity("inventory:second-stack"))}));
    ASSERT_EQ(admitted.commitments.size(), 1u);

    MaterialCommitmentApplyResult const invalid = harness.Apply({
        .operationIdentity = "bad-settlement",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Fulfill,
        .fulfillments = {{
            .commitmentIdentity = admitted.commitments.front().identity,
            .quantity = 2u,
            .reservationSettlements = {{.capacity = Capacity(), .backedMaterialQuantity = 2u, .capacityQuantity = 1u}},
        }},
    });
    EXPECT_EQ(invalid.status, MaterialCommitmentApplyStatus::InvalidCommand);
    EXPECT_FALSE(harness.authority.Snapshot().busy);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, ReleaseIsExplicitAndTerminal)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialCommitmentSnapshot const admitted =
        harness.Commit(Admit("admit", 1u, {Candidate("bandages", 4u)}, {Observation(4u)}));
    ASSERT_EQ(admitted.commitments.size(), 1u);
    std::string const identity = admitted.commitments.front().identity;
    MaterialCommitmentCommand release{
        .operationIdentity = "release",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Release,
        .commitmentIdentities = {identity},
    };
    harness.Commit(release);

    MaterialCommitmentSnapshot releasedSnapshot = harness.authority.Snapshot();
    ASSERT_EQ(releasedSnapshot.commitments.size(), 1u);
    MaterialCommitment const released = releasedSnapshot.commitments.front();
    EXPECT_EQ(released.state, MaterialCommitmentState::Released);
    EXPECT_EQ(released.remainingQuantity, 0u);
    EXPECT_TRUE(released.reservations.empty());
    release.operationIdentity = "release-again";
    release.expectedBookRevision = 3u;
    EXPECT_EQ(harness.Apply(release).status, MaterialCommitmentApplyStatus::TerminalCommitment);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, TerminalOriginCanBeReadmittedWithFreshIdentity)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe-old", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialCommitmentSnapshot const first =
        harness.Commit(Admit("admit-old", 1u, {Candidate("bandages", 4u)}, {Observation(4u)}));
    ASSERT_EQ(first.commitments.size(), 1u);
    std::string const oldIdentity = first.commitments.front().identity;
    harness.Commit({
        .operationIdentity = "release-old",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Release,
        .commitmentIdentities = {oldIdentity},
    });

    MaterialIntent updated = Intent("bandages", 4u, NOW + 600u);
    updated.ownerKind = MaterialCommitmentOwnerKind::GroupCommitment;
    updated.ownerRevision = 8u;
    harness.Commit(Observe("observe-new", 3u, {updated}));
    MaterialAdmissionCandidate candidate = Candidate("bandages", 4u);
    candidate.ownerRevision = 8u;
    MaterialCommitmentSnapshot const readmitted =
        harness.Commit(Admit("admit-new", 4u, {candidate}, {Observation(4u)}));
    ASSERT_EQ(readmitted.commitments.size(), 2u);
    EXPECT_NE(readmitted.commitments[1].identity, oldIdentity);
    EXPECT_EQ(readmitted.commitments[0].ownerKind, MaterialCommitmentOwnerKind::ProfessionProgression);
    EXPECT_EQ(readmitted.commitments[0].marketId, 2u);
    EXPECT_EQ(readmitted.commitments[1].ownerKind, MaterialCommitmentOwnerKind::GroupCommitment);
    EXPECT_EQ(readmitted.commitments[1].state, MaterialCommitmentState::Admitted);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, GeneratedIdentityCollisionFailsClosed)
{
    AuthorityHarness source;
    source.Commit(Observe("observe-old", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialCommitmentSnapshot const admitted =
        source.Commit(Admit("collision-operation", 1u, {Candidate("bandages", 4u)}, {Observation(4u)}));
    ASSERT_EQ(admitted.commitments.size(), 1u);
    source.Commit({
        .operationIdentity = "release",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Release,
        .commitmentIdentities = {admitted.commitments.front().identity},
    });
    MaterialIntent updated = Intent("bandages", 4u, NOW + 600u);
    updated.ownerRevision = 8u;
    source.Commit(Observe("observe-new", 3u, {updated}));

    MaterialCommitmentStartup startup = source.writes.back().write.replacement;
    auto collisionReceipt = std::ranges::find(startup.operations, std::string("collision-operation"),
                                              &MaterialCommitmentOperation::identity);
    ASSERT_NE(collisionReceipt, startup.operations.end());
    collisionReceipt->identity = "historical-collision-receipt";
    PlayerbotMaterialCommitmentAuthority restored([](std::uint64_t, MaterialCommitmentWrite const&) {});
    ASSERT_TRUE(restored.Restore(std::move(startup)));
    MaterialAdmissionCandidate candidate = Candidate("bandages", 4u);
    candidate.ownerRevision = 8u;
    EXPECT_EQ(restored.Apply(Admit("collision-operation", 4u, {candidate}, {Observation(4u)}), NOW).status,
              MaterialCommitmentApplyStatus::IdentityCollision);
    EXPECT_FALSE(restored.Snapshot().busy);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, SupersessionReleasesOldCapacityAndAdmitsReplacementAtomically)
{
    AuthorityHarness harness;
    harness.Commit(Observe("observe-old", 0u, {Intent("old", 6u, NOW + 300u)}));
    MaterialCommitmentSnapshot const oldAdmission =
        harness.Commit(Admit("admit-old", 1u, {Candidate("old", 6u)}, {Observation(6u)}));
    ASSERT_EQ(oldAdmission.commitments.size(), 1u);
    std::string const oldIdentity = oldAdmission.commitments.front().identity;
    harness.Commit(Observe("observe-new", 2u, {Intent("new", 4u, NOW + 400u)}));

    MaterialCommitmentCommand supersede{
        .operationIdentity = "supersede",
        .expectedBookRevision = 3u,
        .kind = MaterialCommitmentCommandKind::Supersede,
        .candidates = {Candidate("new", 4u)},
        .capacityObservations = {Observation(6u)},
        .commitmentIdentities = {oldIdentity},
    };
    MaterialCommitmentSnapshot const committed = harness.Commit(std::move(supersede));

    MaterialCommitmentSnapshot const snapshot = harness.authority.Snapshot();
    ASSERT_EQ(snapshot.commitments.size(), 2u);
    EXPECT_EQ(snapshot.commitments[0].state, MaterialCommitmentState::Superseded);
    EXPECT_TRUE(snapshot.commitments[0].reservations.empty());
    EXPECT_EQ(snapshot.commitments[1].state, MaterialCommitmentState::Admitted);
    EXPECT_EQ(snapshot.commitments[1].remainingQuantity, 4u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, RestorePreservesIdentityRemainingQuantityAndReservation)
{
    AuthorityHarness original;
    original.Commit(Observe("observe", 0u, {Intent("bandages", 10u, NOW + 300u)}));
    MaterialCommitmentSnapshot const admission =
        original.Commit(Admit("admit", 1u, {Candidate("bandages", 10u)}, {Observation(10u)}));
    ASSERT_EQ(admission.commitments.size(), 1u);
    std::string const identity = admission.commitments.front().identity;
    original.Commit({
        .operationIdentity = "partial",
        .expectedBookRevision = 2u,
        .kind = MaterialCommitmentCommandKind::Fulfill,
        .fulfillments = {{.commitmentIdentity = identity,
                          .quantity = 3u,
                          .reservationSettlements =
                              {{.capacity = Capacity(), .backedMaterialQuantity = 3u, .capacityQuantity = 3u}}}},
    });

    PlayerbotMaterialCommitmentAuthority restored([](std::uint64_t, MaterialCommitmentWrite const&) {});
    EXPECT_TRUE(restored.Restore(original.writes.back().write.replacement));
    MaterialCommitmentSnapshot const snapshot = restored.Snapshot();
    ASSERT_EQ(snapshot.commitments.size(), 1u);
    EXPECT_EQ(snapshot.commitments.front().identity, identity);
    EXPECT_EQ(snapshot.commitments.front().ownerKind, MaterialCommitmentOwnerKind::ProfessionProgression);
    EXPECT_EQ(snapshot.commitments.front().marketId, 2u);
    EXPECT_EQ(snapshot.commitments.front().remainingQuantity, 7u);
    ASSERT_EQ(snapshot.commitments.front().reservations.size(), 1u);
    EXPECT_EQ(snapshot.commitments.front().reservations.front().remainingBackedMaterialQuantity, 7u);
    EXPECT_EQ(snapshot.commitments.front().reservations.front().remainingCapacityQuantity, 7u);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, RestoreRetainsAdmissionOperationReceipt)
{
    AuthorityHarness original;
    original.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    MaterialCommitmentCommand admission = Admit("admit-receipt", 1u, {Candidate("bandages", 4u)}, {Observation(4u)});
    MaterialCommitmentSnapshot const committed = original.Commit(admission);
    ASSERT_EQ(committed.commitments.size(), 1u);

    std::vector<MaterialCommitmentWrite> writes;
    PlayerbotMaterialCommitmentAuthority restored([&writes](std::uint64_t, MaterialCommitmentWrite const& write)
                                                  { writes.push_back(write); });
    ASSERT_TRUE(restored.Restore(original.writes.back().write.replacement));
    MaterialCommitmentApplyResult const replay = restored.Apply(admission, NOW + 20u);
    EXPECT_EQ(replay.status, MaterialCommitmentApplyStatus::Idempotent);
    ASSERT_EQ(replay.commitmentIdentities.size(), 1u);
    EXPECT_EQ(replay.commitmentIdentities.front(), committed.commitments.front().identity);
    EXPECT_TRUE(writes.empty());

    ASSERT_EQ(admission.candidates.size(), 1u);
    ASSERT_EQ(admission.candidates.front().reservations.size(), 1u);
    admission.candidates.front().reservations.front().capacityQuantity = 3u;
    EXPECT_EQ(restored.Apply(admission, NOW + 20u).status, MaterialCommitmentApplyStatus::OperationConflict);
    EXPECT_EQ(
        restored.Apply(Admit("fresh-admission", 2u, {Candidate("bandages", 4u)}, {Observation(4u)}), NOW + 20u).status,
        MaterialCommitmentApplyStatus::ExistingCommitment);
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, RestoreRejectsMissingReceiptRevisionAndInvalidCapacityUnit)
{
    AuthorityHarness source;
    source.Commit(Observe("observe", 0u, {Intent("bandages", 4u, NOW + 300u)}));
    source.Commit(Admit("admit", 1u, {Candidate("bandages", 4u)}, {Observation(4u)}));
    MaterialCommitmentStartup missingReceipt = source.writes.back().write.replacement;
    ASSERT_EQ(missingReceipt.operations.size(), 2u);
    missingReceipt.operations.erase(missingReceipt.operations.begin());
    PlayerbotMaterialCommitmentAuthority missing([](std::uint64_t, MaterialCommitmentWrite const&) {});
    EXPECT_FALSE(missing.Restore(std::move(missingReceipt)));

    MaterialCommitmentStartup invalidUnit = source.writes.back().write.replacement;
    ASSERT_EQ(invalidUnit.commitments.size(), 1u);
    ASSERT_EQ(invalidUnit.commitments.front().reservations.size(), 1u);
    invalidUnit.commitments.front().reservations.front().unit = MaterialCapacityUnit::Copper;
    PlayerbotMaterialCommitmentAuthority corrupt([](std::uint64_t, MaterialCommitmentWrite const&) {});
    EXPECT_FALSE(corrupt.Restore(std::move(invalidUnit)));

    MaterialCommitmentStartup invalidItemUnits = source.writes.back().write.replacement;
    ASSERT_EQ(invalidItemUnits.commitments.size(), 1u);
    ASSERT_EQ(invalidItemUnits.commitments.front().reservations.size(), 1u);
    invalidItemUnits.commitments.front().reservations.front().initialCapacityQuantity = 5u;
    PlayerbotMaterialCommitmentAuthority mismatchedInitial([](std::uint64_t, MaterialCommitmentWrite const&) {});
    EXPECT_FALSE(mismatchedInitial.Restore(std::move(invalidItemUnits)));

    MaterialCommitmentStartup invalidRemainingItemUnits = source.writes.back().write.replacement;
    ASSERT_EQ(invalidRemainingItemUnits.commitments.size(), 1u);
    ASSERT_EQ(invalidRemainingItemUnits.commitments.front().reservations.size(), 1u);
    invalidRemainingItemUnits.commitments.front().reservations.front().remainingCapacityQuantity = 3u;
    PlayerbotMaterialCommitmentAuthority mismatchedRemaining([](std::uint64_t, MaterialCommitmentWrite const&) {});
    EXPECT_FALSE(mismatchedRemaining.Restore(std::move(invalidRemainingItemUnits)));
}

TEST(PlayerbotMaterialCommitmentAuthorityTest, RestoreRejectsUnavailableOrCorruptStartup)
{
    PlayerbotMaterialCommitmentAuthority authority([](std::uint64_t, MaterialCommitmentWrite const&) {});
    MaterialCommitmentStartup unavailable;
    EXPECT_FALSE(authority.Restore(unavailable));
    EXPECT_FALSE(authority.Snapshot().persistenceHealthy);

    MaterialCommitmentStartup empty;
    empty.sourceAvailable = true;
    EXPECT_TRUE(authority.Restore(empty));
    EXPECT_TRUE(authority.Snapshot().persistenceHealthy);

    PlayerbotMaterialCommitmentAuthority corrupt([](std::uint64_t, MaterialCommitmentWrite const&) {});
    MaterialCommitmentStartup startup;
    startup.bookRevision = 1u;
    startup.intents.push_back(Intent("bad", 4u, NOW + 300u));
    startup.commitments.push_back({
        .identity = "collision",
        .originIdentity = "bad",
        .ownerRevision = 7u,
        .materialItemId = 2589u,
        .boundedQuantity = 4u,
        .remainingQuantity = 5u,
        .neededBy = NOW + 300u,
        .state = MaterialCommitmentState::Admitted,
    });
    EXPECT_FALSE(corrupt.Restore(std::move(startup)));
    EXPECT_FALSE(corrupt.Snapshot().persistenceHealthy);
    EXPECT_TRUE(corrupt.Snapshot().commitments.empty());
}
