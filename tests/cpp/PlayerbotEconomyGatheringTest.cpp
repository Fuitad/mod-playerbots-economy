/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>

#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
GatheringResource Resource(GatheringProfession profession = GatheringProfession::Herbalism)
{
    GatheringResource resource;
    resource.resourceGuid = 1000u;
    resource.profession = profession;
    resource.mapId = 1u;
    resource.phaseMask = 3u;
    resource.requiredSkill = 75u;
    resource.spawned = true;
    return resource;
}

GatheringCandidate Candidate(uint32 characterGuid, float distance, uint8 affinity = 25u)
{
    GatheringCandidate candidate;
    candidate.characterGuid = characterGuid;
    candidate.profession = GatheringProfession::Herbalism;
    candidate.hasCareer = true;
    candidate.hasLearnedSkill = true;
    candidate.skillValue = 100u;
    candidate.economyAffinity = affinity;
    candidate.grouped = true;
    candidate.sameMap = true;
    candidate.samePhase = true;
    candidate.pathAvailable = true;
    candidate.safe = true;
    candidate.botDistance = distance;
    candidate.formationDistance = distance + 1.0f;
    candidate.lootDistance = 40.0f;
    return candidate;
}
}  // namespace

TEST(PlayerbotEconomyGatheringTest, GroupedClosestEligibleGathererOwnsCopiedClaim)
{
    PlayerbotEconomyGathering gathering;
    GatheringResource resource = Resource();
    std::array<GatheringCandidate, 2> candidates = {Candidate(10u, 25.0f), Candidate(11u, 8.0f)};

    GatheringClaimResult const result = gathering.ClaimGrouped(resource, candidates, 100u, 15u);
    ASSERT_TRUE(result.claim.has_value());
    EXPECT_EQ(result.claim->characterGuid, 11u);
    EXPECT_EQ(result.claim->resourceGuid, 1000u);
    EXPECT_EQ(result.claim->mapId, 1u);
    EXPECT_EQ(result.claim->phaseMask, 3u);
    EXPECT_EQ(result.claim->expiresAt, 115u);

    resource.resourceGuid = 2000u;
    candidates[1].characterGuid = 99u;
    GatheringClaimSnapshot const snapshot = gathering.Snapshot(100u);
    ASSERT_EQ(snapshot.claims.size(), 1u);
    EXPECT_EQ(snapshot.claims.front().resourceGuid, 1000u);
    EXPECT_EQ(snapshot.claims.front().characterGuid, 11u);
}

TEST(PlayerbotEconomyGatheringTest, GroupedAffinityThresholdAndForcedCommandAreExact)
{
    for (uint8 affinity : {0u, 24u})
    {
        PlayerbotEconomyGathering gathering;
        GatheringCandidate candidate = Candidate(10u, 8.0f, affinity);
        GatheringClaimResult const autonomous = gathering.ClaimGrouped(Resource(), {candidate}, 100u, 15u);
        EXPECT_FALSE(autonomous.claim.has_value());
        EXPECT_EQ(autonomous.blocker, GatheringBlocker::AffinityTooLow);

        candidate.directCommand = true;
        GatheringClaimResult const forced = gathering.ClaimGrouped(Resource(), {candidate}, 100u, 15u);
        ASSERT_TRUE(forced.claim.has_value());
        EXPECT_EQ(forced.claim->characterGuid, 10u);
        EXPECT_TRUE(forced.claim->directCommand);
    }

    PlayerbotEconomyGathering gathering;
    EXPECT_TRUE(gathering.ClaimGrouped(Resource(), {Candidate(10u, 8.0f, 25u)}, 100u, 15u).claim.has_value());
}

TEST(PlayerbotEconomyGatheringTest, GroupedForcedCommandPreservesEveryOtherSafeguard)
{
    struct Guard
    {
        using Member = bool GatheringCandidate::*;

        Member member;
        GatheringBlocker blocker;
    };
    std::array<Guard, 7> const guards = {Guard{&GatheringCandidate::hasCareer, GatheringBlocker::MissingCareer},
                                         Guard{&GatheringCandidate::hasLearnedSkill, GatheringBlocker::MissingSkill},
                                         Guard{&GatheringCandidate::grouped, GatheringBlocker::NotGrouped},
                                         Guard{&GatheringCandidate::sameMap, GatheringBlocker::WrongMap},
                                         Guard{&GatheringCandidate::samePhase, GatheringBlocker::WrongPhase},
                                         Guard{&GatheringCandidate::pathAvailable, GatheringBlocker::MissingPath},
                                         Guard{&GatheringCandidate::safe, GatheringBlocker::Unsafe}};

    for (Guard const& guard : guards)
    {
        PlayerbotEconomyGathering gathering;
        GatheringCandidate candidate = Candidate(10u, 8.0f, 0u);
        candidate.directCommand = true;
        candidate.*guard.member = false;
        if (guard.blocker == GatheringBlocker::MissingPath)
        {
            candidate.botDistance = 41.0f;
            candidate.formationDistance = 41.0f;
            candidate.discoveryDistance = 100.0f;
        }
        GatheringClaimResult const result = gathering.ClaimGrouped(Resource(), {candidate}, 100u, 15u);
        EXPECT_FALSE(result.claim.has_value());
        EXPECT_EQ(result.blocker, guard.blocker);
    }

    for (bool formationDistance : {false, true})
    {
        PlayerbotEconomyGathering gathering;
        GatheringCandidate candidate = Candidate(10u, 8.0f, 0u);
        candidate.directCommand = true;
        if (formationDistance)
            candidate.formationDistance = 41.0f;
        else
            candidate.botDistance = 41.0f;
        GatheringClaimResult const result = gathering.ClaimGrouped(Resource(), {candidate}, 100u, 15u);
        EXPECT_FALSE(result.claim.has_value());
        EXPECT_EQ(result.blocker, GatheringBlocker::OutOfRange);
    }

    PlayerbotEconomyGathering gathering;
    GatheringCandidate wrongProfession = Candidate(10u, 8.0f, 0u);
    wrongProfession.directCommand = true;
    wrongProfession.profession = GatheringProfession::Mining;
    EXPECT_EQ(gathering.ClaimGrouped(Resource(), {wrongProfession}, 100u, 15u).blocker,
              GatheringBlocker::WrongProfession);

    GatheringCandidate insufficientSkill = Candidate(10u, 8.0f, 0u);
    insufficientSkill.directCommand = true;
    insufficientSkill.skillValue = 74u;
    EXPECT_EQ(gathering.ClaimGrouped(Resource(), {insufficientSkill}, 100u, 15u).blocker,
              GatheringBlocker::InsufficientSkill);
}

TEST(PlayerbotEconomyGatheringTest, GroupedClaimExcludesSecondGathererUntilReleaseOrExpiry)
{
    PlayerbotEconomyGathering gathering;
    GatheringCandidate first = Candidate(10u, 8.0f);
    GatheringCandidate second = Candidate(11u, 10.0f);
    GatheringClaimResult const claimed = gathering.ClaimGrouped(Resource(), {first, second}, 100u, 15u);
    ASSERT_TRUE(claimed.claim.has_value());

    GatheringClaimResult const duplicate = gathering.ClaimGrouped(Resource(), {second}, 101u, 15u);
    EXPECT_FALSE(duplicate.claim.has_value());
    EXPECT_EQ(duplicate.blocker, GatheringBlocker::AlreadyClaimed);

    EXPECT_TRUE(gathering.Release(claimed.claim->leaseId, GatheringReleaseCause::HigherPriorityBehavior));
    GatheringClaimResult const transferred = gathering.ClaimGrouped(Resource(), {second}, 102u, 15u);
    ASSERT_TRUE(transferred.claim.has_value());
    EXPECT_EQ(transferred.claim->characterGuid, 11u);

    GatheringClaimResult const afterExpiry = gathering.ClaimGrouped(Resource(), {first}, 118u, 15u);
    ASSERT_TRUE(afterExpiry.claim.has_value());
    EXPECT_EQ(afterExpiry.claim->characterGuid, 10u);
}

TEST(PlayerbotEconomyGatheringTest, AutonomousTripsStopAtDemandCapacitySafetyAndLeaseBounds)
{
    AutonomousGatheringPlan plan;
    plan.profession = GatheringProfession::Mining;
    plan.itemId = 2770u;
    plan.requestedQuantity = 4u;
    plan.startingItemCount = 2u;
    plan.expiresAt = 200u;

    AutonomousGatheringFacts facts;
    facts.now = 100u;
    facts.currentItemCount = 3u;
    facts.currentSkillValue = 75u;
    facts.demandStillExists = true;
    facts.destinationAvailable = true;
    facts.inventoryCapacity = true;
    facts.safe = true;

    AutonomousGatheringDecision decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.action, AutonomousGatheringAction::Travel);
    EXPECT_EQ(decision.gatheredQuantity, 1u);
    EXPECT_EQ(decision.remainingQuantity, 3u);

    facts.atDestination = true;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.action, AutonomousGatheringAction::Travel);

    facts.resourceAvailable = true;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.action, AutonomousGatheringAction::Gather);

    facts.currentItemCount = 6u;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.action, AutonomousGatheringAction::Complete);
    EXPECT_EQ(decision.remainingQuantity, 0u);

    facts.currentItemCount = 4u;
    facts.demandStillExists = false;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.action, AutonomousGatheringAction::Release);
    EXPECT_EQ(decision.blocker, AutonomousGatheringBlocker::DemandGone);
    EXPECT_EQ(decision.gatheredQuantity, 2u);

    facts.demandStillExists = true;
    facts.inventoryCapacity = false;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.blocker, AutonomousGatheringBlocker::InventoryFull);

    // A momentarily unsafe bot (combat, flight, teleport) pauses the trip instead of
    // abandoning it; the destination expiry above still bounds how long it can wait.
    facts.inventoryCapacity = true;
    facts.safe = false;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.action, AutonomousGatheringAction::Wait);
    EXPECT_EQ(decision.blocker, AutonomousGatheringBlocker::None);

    facts.safe = true;
    facts.now = 200u;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.blocker, AutonomousGatheringBlocker::DestinationExpired);
}

TEST(PlayerbotEconomyGatheringTest, ReleaseWithGatheredLootCountsAsProgressNotFailure)
{
    AutonomousGatheringPlan plan;
    plan.profession = GatheringProfession::Mining;
    plan.itemId = 2770u;
    plan.requestedQuantity = 4u;
    plan.startingItemCount = 2u;
    plan.expiresAt = 200u;

    AutonomousGatheringFacts facts;
    facts.now = 200u;
    facts.currentItemCount = 3u;
    facts.demandStillExists = true;

    AutonomousGatheringDecision const partial = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(partial.action, AutonomousGatheringAction::Release);
    EXPECT_EQ(partial.blocker, AutonomousGatheringBlocker::DestinationExpired);
    EXPECT_TRUE(PlayerbotEconomyGathering::ReleaseCountsAsProgress(partial));

    facts.currentItemCount = 2u;
    AutonomousGatheringDecision const empty = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(empty.action, AutonomousGatheringAction::Release);
    EXPECT_FALSE(PlayerbotEconomyGathering::ReleaseCountsAsProgress(empty));
}

TEST(PlayerbotEconomyGatheringTest, AutonomousProgressionCompletesOnlyAfterRealSkillIncrease)
{
    AutonomousGatheringPlan plan;
    plan.profession = GatheringProfession::Herbalism;
    plan.startingSkillValue = 124u;
    plan.expiresAt = 200u;

    AutonomousGatheringFacts facts;
    facts.now = 100u;
    facts.currentSkillValue = 124u;
    facts.demandStillExists = true;
    facts.destinationAvailable = true;
    facts.inventoryCapacity = true;
    facts.safe = true;
    facts.atDestination = true;

    EXPECT_EQ(PlayerbotEconomyGathering::DecideAutonomous(plan, facts).action, AutonomousGatheringAction::Travel);
    facts.resourceAvailable = true;
    EXPECT_EQ(PlayerbotEconomyGathering::DecideAutonomous(plan, facts).action, AutonomousGatheringAction::Gather);
    facts.currentSkillValue = 125u;
    EXPECT_EQ(PlayerbotEconomyGathering::DecideAutonomous(plan, facts).action, AutonomousGatheringAction::Complete);
}

TEST(PlayerbotEconomyGatheringTest, AutonomousSkinningUsesCorpseBeforeOneOrdinaryKill)
{
    AutonomousGatheringPlan plan;
    plan.profession = GatheringProfession::Skinning;
    plan.itemId = 2318u;
    plan.requestedQuantity = 1u;
    plan.expiresAt = 200u;

    AutonomousGatheringFacts facts;
    facts.now = 100u;
    facts.demandStillExists = true;
    facts.destinationAvailable = true;
    facts.inventoryCapacity = true;
    facts.safe = true;
    facts.atDestination = true;
    facts.existingSkinningCorpse = true;

    EXPECT_EQ(PlayerbotEconomyGathering::DecideAutonomous(plan, facts).action, AutonomousGatheringAction::Gather);

    facts.existingSkinningCorpse = false;
    EXPECT_EQ(PlayerbotEconomyGathering::DecideAutonomous(plan, facts).action,
              AutonomousGatheringAction::GrindOneCreature);

    facts.creatureKillStarted = true;
    facts.creatureKillActive = true;
    EXPECT_EQ(PlayerbotEconomyGathering::DecideAutonomous(plan, facts).action, AutonomousGatheringAction::Wait);

    facts.creatureKillActive = false;
    AutonomousGatheringDecision const bounded = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(bounded.action, AutonomousGatheringAction::Release);
    EXPECT_EQ(bounded.blocker, AutonomousGatheringBlocker::OneKillBoundReached);
}

TEST(PlayerbotEconomyGatheringTest, AutonomousSupplierListingUsesOnlyRevalidatedDeficit)
{
    AutonomousSupplierListing const partial =
        PlayerbotEconomyGathering::BoundSupplierListing(20u, 7u, 3u, 2000u, 2400u);
    EXPECT_EQ(partial.count, 3u);
    EXPECT_EQ(partial.startBid, 300u);
    EXPECT_EQ(partial.buyout, 360u);

    AutonomousSupplierListing const unavailable =
        PlayerbotEconomyGathering::BoundSupplierListing(20u, 7u, 0u, 2000u, 2400u);
    EXPECT_EQ(unavailable.count, 0u);
    EXPECT_EQ(unavailable.startBid, 0u);
    EXPECT_EQ(unavailable.buyout, 0u);
}

TEST(PlayerbotEconomyGatheringTest, DedicatedWorkOrdersAreBoundedByActorRouteResourcesInventoryAndSelfNeed)
{
    DedicatedGatheringCapacityFacts facts;
    facts.activeUncoveredDemand = 398u;
    facts.selfReservedQuantity = 2u;
    facts.reachableResourceCount = 5u;
    facts.conservativeYieldBasisPoints = 20'000u;
    facts.inventoryCapacity = 20u;
    facts.outboundSeconds = 20u;
    facts.returnSeconds = 20u;
    facts.activityBudgetSeconds = 100u;
    facts.conservativeSecondsPerResource = 10u;
    facts.skillEligible = true;
    facts.routeAvailable = true;
    facts.safe = true;
    facts.deliveryAvailable = true;

    // Five reachable resources at two items each are the limiting fact. The
    // population backlog is not itself an actor capacity.
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 8u);

    // Even when the active population gap is 398 Copper Ore and hundreds of
    // veins exist, the five-minute trip budget plus the authoritative
    // three-second gathering interaction exposes only this trip's capacity.
    facts.selfReservedQuantity = 0u;
    facts.reachableResourceCount = 398u;
    facts.conservativeYieldBasisPoints = 10'000u;
    facts.inventoryCapacity = 398u;
    facts.outboundSeconds = 0u;
    facts.returnSeconds = 0u;
    facts.activityBudgetSeconds = 300u;
    facts.conservativeSecondsPerResource = 3u;
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 100u);

    // A probabilistic grouped skinning source is capacity-bearing, but only at
    // its conservative expected yield. Two 63.13 percent resources expose one
    // expected item, never two guaranteed items.
    facts.reachableResourceCount = 2u;
    facts.conservativeYieldBasisPoints = 6'313u;
    facts.inventoryCapacity = 398u;
    facts.activityBudgetSeconds = 300u;
    facts.conservativeSecondsPerResource = 3u;
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 1u);

    facts.reachableResourceCount = 5u;
    facts.conservativeYieldBasisPoints = 20'000u;
    facts.inventoryCapacity = 20u;
    facts.outboundSeconds = 20u;
    facts.returnSeconds = 20u;
    facts.activityBudgetSeconds = 100u;
    facts.conservativeSecondsPerResource = 10u;
    facts.selfReservedQuantity = 2u;
    facts.activeUncoveredDemand = 7u;
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 5u);

    facts.activeUncoveredDemand = 398u;
    facts.selfReservedQuantity = 395u;
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 0u);

    facts.selfReservedQuantity = 0u;
    facts.outboundSeconds = 50u;
    facts.returnSeconds = 50u;
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 0u);

    facts.outboundSeconds = 20u;
    facts.returnSeconds = 20u;
    facts.inventoryCapacity = 0u;
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 0u);

    facts.inventoryCapacity = 20u;
    facts.safe = false;
    EXPECT_EQ(PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts), 0u);
}

TEST(PlayerbotEconomyGatheringTest, DedicatedWorkUsesSmallestSufficientSetAndLeavesResidualUnclaimed)
{
    std::array<DedicatedGatheringCandidate, 4> candidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u,
                                    .capacity = 3u,
                                    .routeSeconds = 20u,
                                    .recentWorkSeconds = 0u,
                                    .reliabilitySuccesses = 3u,
                                    .reliabilityAttempts = 3u},
        DedicatedGatheringCandidate{.characterGuid = 11u,
                                    .capacity = 7u,
                                    .routeSeconds = 30u,
                                    .recentWorkSeconds = 10u,
                                    .reliabilitySuccesses = 4u,
                                    .reliabilityAttempts = 5u},
        DedicatedGatheringCandidate{.characterGuid = 12u,
                                    .capacity = 5u,
                                    .routeSeconds = 10u,
                                    .recentWorkSeconds = 0u,
                                    .reliabilitySuccesses = 5u,
                                    .reliabilityAttempts = 5u},
        DedicatedGatheringCandidate{.characterGuid = 13u,
                                    .capacity = 0u,
                                    .routeSeconds = 1u,
                                    .recentWorkSeconds = 0u,
                                    .reliabilitySuccesses = 5u,
                                    .reliabilityAttempts = 5u},
    };

    DedicatedGatheringPlan const covered = PlayerbotEconomyGathering::PlanDedicatedWork(10u, candidates);
    ASSERT_EQ(covered.workOrders.size(), 2u);
    EXPECT_EQ(covered.workOrders[0].characterGuid, 11u);
    EXPECT_EQ(covered.workOrders[0].quantity, 7u);
    EXPECT_EQ(covered.workOrders[1].characterGuid, 12u);
    EXPECT_EQ(covered.workOrders[1].quantity, 3u);
    EXPECT_EQ(covered.assignedQuantity, 10u);
    EXPECT_EQ(covered.unassignedQuantity, 0u);

    DedicatedGatheringPlan const residual = PlayerbotEconomyGathering::PlanDedicatedWork(20u, candidates);
    ASSERT_EQ(residual.workOrders.size(), 3u);
    EXPECT_EQ(residual.assignedQuantity, 15u);
    EXPECT_EQ(residual.unassignedQuantity, 5u);
}

TEST(PlayerbotEconomyGatheringTest, DedicatedActivityBudgetChargesOnceAndRecoversWithOrdinaryTime)
{
    PlayerbotEconomyGathering gathering;

    gathering.RecordDedicatedActivity(10u, 100u, 140u);
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 140u), 60u);
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 160u), 80u);
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 180u), 100u);

    // A multipurpose trip can report material and skill outcomes separately,
    // but its physical duration is charged through this one activity record.
    gathering.RecordDedicatedActivity(10u, 180u, 210u);
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 210u), 70u);
}

TEST(PlayerbotEconomyGatheringTest, DedicatedExperienceUsesObservedYieldAndOnePhysicalTripDuration)
{
    PlayerbotEconomyGathering gathering;
    gathering.RecordDedicatedTrip({.characterGuid = 10u,
                                   .itemId = 2770u,
                                   .startedAt = 100u,
                                   .finishedAt = 140u,
                                   .outboundSeconds = 10u,
                                   .attemptedResources = 3u,
                                   .gatheredQuantity = 6u,
                                   .skillPoints = 2u});

    DedicatedGatheringExperience experience = gathering.DedicatedExperience(10u, 2770u);
    EXPECT_EQ(experience.observedYieldBasisPoints, 20'000u);
    EXPECT_EQ(experience.conservativeSecondsPerResource, 10u);
    EXPECT_EQ(experience.successes, 1u);
    EXPECT_EQ(experience.attempts, 1u);
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 140u), 60u);

    gathering.RecordDedicatedTrip({.characterGuid = 10u,
                                   .itemId = 2770u,
                                   .startedAt = 140u,
                                   .finishedAt = 170u,
                                   .outboundSeconds = 10u,
                                   .attemptedResources = 2u,
                                   .gatheredQuantity = 0u,
                                   .skillPoints = 0u});

    experience = gathering.DedicatedExperience(10u, 2770u);
    EXPECT_EQ(experience.observedYieldBasisPoints, 12'000u);
    EXPECT_EQ(experience.conservativeSecondsPerResource, 10u);
    EXPECT_EQ(experience.successes, 1u);
    EXPECT_EQ(experience.attempts, 2u);
    // Back-to-back gathering is additional activity. Time spent on the second
    // trip cannot simultaneously recover the first trip's activity debt.
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 170u), 30u);
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 200u), 60u);
}
