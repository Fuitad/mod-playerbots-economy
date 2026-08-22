/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <limits>

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
    candidate.skillUpPossible = true;
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

// A 5 minute trip that spends 4 minutes walking gathers nothing; the planner must refuse such a walk.
TEST(PlayerbotEconomyGatheringTripBudget, OutboundWalkMayTakeAtMostHalfTheTripBudget)
{
    EXPECT_TRUE(PlayerbotEconomy::PlayerbotEconomyGathering::OutboundFitsTripBudget(0u, 300u));
    EXPECT_TRUE(PlayerbotEconomy::PlayerbotEconomyGathering::OutboundFitsTripBudget(149u, 300u));
    EXPECT_FALSE(PlayerbotEconomy::PlayerbotEconomyGathering::OutboundFitsTripBudget(150u, 300u));
    EXPECT_FALSE(PlayerbotEconomy::PlayerbotEconomyGathering::OutboundFitsTripBudget(540u, 300u));
    EXPECT_FALSE(PlayerbotEconomy::PlayerbotEconomyGathering::OutboundFitsTripBudget(0u, 0u));
}

// Dedicated skill-up trips stop once the skill matches the bot's level; the trained rank cap bounds it.
TEST(PlayerbotEconomyGatheringTripBudget, SkillUpTargetFollowsLevelUnderTheRankCap)
{
    using PlayerbotEconomy::PlayerbotEconomyGathering;
    EXPECT_EQ(PlayerbotEconomyGathering::GatheringSkillTargetForLevel(1u, 75u), 5u);
    EXPECT_EQ(PlayerbotEconomyGathering::GatheringSkillTargetForLevel(15u, 75u), 75u);
    EXPECT_EQ(PlayerbotEconomyGathering::GatheringSkillTargetForLevel(20u, 75u), 75u);
    EXPECT_EQ(PlayerbotEconomyGathering::GatheringSkillTargetForLevel(20u, 150u), 100u);
    EXPECT_EQ(PlayerbotEconomyGathering::GatheringSkillTargetForLevel(80u, 450u), 400u);
    EXPECT_EQ(PlayerbotEconomyGathering::GatheringSkillTargetForLevel(0u, 75u), 0u);
    EXPECT_EQ(PlayerbotEconomyGathering::GatheringSkillTargetForLevel(20u, 0u), 0u);
}

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

// Passive pickup is about bag space, not affinity: a node is taken when it still gives skill, feeds a planned
// crafting profession, or the bot sells; a dedicated trip or a direct command bypasses the gates.
TEST(PlayerbotEconomyGatheringTest, NearbyPickupFollowsUsefulnessNotAffinity)
{
    auto const claimWith = [](auto configure)
    {
        PlayerbotEconomyGathering gathering;
        GatheringCandidate candidate = Candidate(10u, 8.0f, 0u);
        candidate.skillUpPossible = false;
        configure(candidate);
        return gathering.ClaimGrouped(Resource(), {candidate}, 100u, 15u);
    };

    EXPECT_TRUE(claimWith([](GatheringCandidate& c) { c.skillUpPossible = true; }).claim.has_value());
    EXPECT_TRUE(claimWith([](GatheringCandidate& c) { c.craftingUsesYield = true; }).claim.has_value());
    EXPECT_TRUE(claimWith([](GatheringCandidate& c) { c.marketEligible = true; }).claim.has_value());
    EXPECT_TRUE(claimWith([](GatheringCandidate& c) { c.activeTrip = true; }).claim.has_value());
    EXPECT_TRUE(claimWith([](GatheringCandidate& c) { c.directCommand = true; }).claim.has_value());

    GatheringClaimResult const useless = claimWith([](GatheringCandidate&) {});
    EXPECT_FALSE(useless.claim.has_value());
    EXPECT_EQ(useless.blocker, GatheringBlocker::NotUseful);

    GatheringClaimResult const full = claimWith(
        [](GatheringCandidate& c)
        {
            c.skillUpPossible = true;
            c.yieldsAtCeiling = true;
        });
    EXPECT_FALSE(full.claim.has_value());
    EXPECT_EQ(full.blocker, GatheringBlocker::InventoryFull);
    EXPECT_TRUE(claimWith(
                    [](GatheringCandidate& c)
                    {
                        c.yieldsAtCeiling = true;
                        c.activeTrip = true;
                    })
                    .claim.has_value());

    GatheringClaimResult const noCareer = claimWith(
        [](GatheringCandidate& c)
        {
            c.skillUpPossible = true;
            c.hasCareer = false;
        });
    EXPECT_FALSE(noCareer.claim.has_value());
    EXPECT_EQ(noCareer.blocker, GatheringBlocker::MissingCareer);

    // A solo bot walking past a node: ClaimNearby forces the grouped flag, NotGrouped never applies.
    PlayerbotEconomyGathering gathering;
    GatheringCandidate solo = Candidate(10u, 8.0f, 0u);
    solo.grouped = false;
    solo.skillUpPossible = true;
    EXPECT_TRUE(gathering.ClaimNearby(Resource(), solo, 100u, 15u).claim.has_value());
}

TEST(PlayerbotEconomyGatheringTest, ActiveTripRegistryRoundTripsPerCharacter)
{
    PlayerbotEconomyGathering gathering;
    EXPECT_EQ(gathering.ActiveTripSkill(10u), 0u);
    gathering.SetActiveTrip(10u, 186u);
    gathering.SetActiveTrip(11u, 182u);
    EXPECT_EQ(gathering.ActiveTripSkill(10u), 186u);
    EXPECT_EQ(gathering.ActiveTripSkill(11u), 182u);
    gathering.ClearActiveTrip(10u);
    EXPECT_EQ(gathering.ActiveTripSkill(10u), 0u);
    EXPECT_EQ(gathering.ActiveTripSkill(11u), 182u);
    gathering.RemoveActor(11u);
    EXPECT_EQ(gathering.ActiveTripSkill(11u), 0u);
}

TEST(PlayerbotEconomyGatheringTest, SkinnerMayKillGreyCreaturesButNothingDangerouslyAbove)
{
    // A level 22 skinner at skill 1 needs level 10 creatures: far below it, and allowed.
    EXPECT_TRUE(PlayerbotEconomyGathering::IsSkinningTargetLevelSafe(22u, 8u, 1u));
    EXPECT_TRUE(PlayerbotEconomyGathering::IsSkinningTargetLevelSafe(22u, 23u, 1u));
    EXPECT_FALSE(PlayerbotEconomyGathering::IsSkinningTargetLevelSafe(22u, 24u, 1u));
    EXPECT_TRUE(PlayerbotEconomyGathering::IsSkinningTargetLevelSafe(1u, 1u, 0u));
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

TEST(PlayerbotEconomyGatheringTest, AcceptedExternalSliceProtectsOnlyPostTripInventoryDelta)
{
    AcceptedExternalGatheringSlice const retained = PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice({
        .currentInventoryQuantity = 5u,
        .preTripInventoryQuantity = 3u,
        .acceptedQuantity = 5u,
        .retained = true,
    });
    EXPECT_EQ(retained.protectedQuantity, 2u);
    EXPECT_EQ(retained.progressionAvailableQuantity, 3u);

    AcceptedExternalGatheringSlice const noDelta = PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice({
        .currentInventoryQuantity = 2u,
        .preTripInventoryQuantity = 3u,
        .acceptedQuantity = 5u,
        .retained = true,
    });
    EXPECT_EQ(noDelta.protectedQuantity, 0u);
    EXPECT_EQ(noDelta.progressionAvailableQuantity, 2u);

    AutonomousSupplierListing const externalDisposition =
        PlayerbotEconomyGathering::BoundSupplierListing(7u, 5u, 5u, 700u, 840u);
    ASSERT_EQ(externalDisposition.count, 5u);
    AcceptedExternalGatheringSlice const afterExternalDisposition =
        PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice({
            .currentInventoryQuantity = 2u,
            .preTripInventoryQuantity = 0u,
            .acceptedQuantity = 0u,
            .retained = true,
        });
    EXPECT_EQ(afterExternalDisposition.protectedQuantity, 0u);
    EXPECT_EQ(afterExternalDisposition.progressionAvailableQuantity, 2u);
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

TEST(PlayerbotEconomyGatheringTest, DedicatedTripSharesOrderedActiveOriginsWithoutActivatingLatentDemand)
{
    DedicatedGatheringPlanRequest request{
        .tripIdentity = "trip-copper-1",
        .observedAt = 100u,
        .batchQuantity = 8u,
        .origins =
            {
                {.originIdentity = "active-smelting",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = 4u,
                 .expiresAt = 130u},
                {.originIdentity = "latent-training",
                 .state = DedicatedGatheringOriginState::Latent,
                 .quantity = 9u,
                 .expiresAt = 140u},
                {.originIdentity = "active-auction",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = 6u,
                 .expiresAt = 150u},
            },
    };
    std::array<DedicatedGatheringCandidate, 2> const candidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 5u},
        DedicatedGatheringCandidate{.characterGuid = 11u, .capacity = 3u},
    };

    std::optional<DedicatedGatheringProvenancePlan> const planned =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(planned.has_value());
    EXPECT_EQ(planned->tripIdentity, "trip-copper-1");
    EXPECT_EQ(planned->origins, request.origins);
    EXPECT_EQ(planned->assignedQuantity, 8u);
    EXPECT_EQ(planned->unassignedBatchQuantity, 0u);
    EXPECT_EQ(planned->deferredActiveQuantity, 2u);
    EXPECT_EQ(planned->latentQuantity, 9u);
    ASSERT_EQ(planned->workOrders.size(), 2u);
    EXPECT_EQ(planned->workOrders[0].characterGuid, 10u);
    EXPECT_EQ(planned->workOrders[0].quantity, 5u);
    EXPECT_EQ(planned->workOrders[0].allocations,
              (std::vector<DedicatedGatheringOriginAllocation>{{"active-smelting", 4u}, {"active-auction", 1u}}));
    EXPECT_EQ(planned->workOrders[1].characterGuid, 11u);
    EXPECT_EQ(planned->workOrders[1].quantity, 3u);
    EXPECT_EQ(planned->workOrders[1].allocations,
              (std::vector<DedicatedGatheringOriginAllocation>{{"active-auction", 3u}}));

    DedicatedGatheringTripProvenance const first =
        *PlayerbotEconomyGathering::ProvenanceForWorkOrder(*planned, planned->workOrders[0]);
    EXPECT_EQ(first.tripIdentity, "trip-copper-1");
    ASSERT_EQ(first.origins.size(), 3u);
    EXPECT_EQ(first.origins[0].origin, request.origins[0]);
    EXPECT_EQ(first.origins[0].allocatedQuantity, 4u);
    EXPECT_EQ(first.origins[1].origin, request.origins[1]);
    EXPECT_EQ(first.origins[1].allocatedQuantity, 0u);
    EXPECT_EQ(first.origins[2].origin, request.origins[2]);
    EXPECT_EQ(first.origins[2].allocatedQuantity, 1u);
}

TEST(PlayerbotEconomyGatheringTest, DedicatedTripActivationRespondsOnlyToActiveDemandAndCallerCapacity)
{
    DedicatedGatheringPlanRequest request{
        .tripIdentity = "trip-demand",
        .observedAt = 100u,
        .batchQuantity = 5u,
        .origins =
            {
                {.originIdentity = "active",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = 4u,
                 .expiresAt = 130u},
                {.originIdentity = "latent",
                 .state = DedicatedGatheringOriginState::Latent,
                 .quantity = 50u,
                 .expiresAt = 130u},
            },
    };
    std::array<DedicatedGatheringCandidate, 2> candidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 4u},
        DedicatedGatheringCandidate{.characterGuid = 11u, .capacity = 2u},
    };

    std::optional<DedicatedGatheringProvenancePlan> latentDoesNotActivate =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(latentDoesNotActivate.has_value());
    ASSERT_EQ(latentDoesNotActivate->workOrders.size(), 1u);
    EXPECT_EQ(latentDoesNotActivate->latentQuantity, 50u);

    request.origins[0].quantity = 5u;
    std::optional<DedicatedGatheringProvenancePlan> const moreActiveDemand =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(moreActiveDemand.has_value());
    EXPECT_EQ(moreActiveDemand->workOrders.size(), 2u);

    request.origins[0].quantity = 4u;
    request.origins[0].expiresAt = 110u;
    candidates[0].capacity = 3u;
    std::optional<DedicatedGatheringProvenancePlan> const tighterCallerCapacity =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(tighterCallerCapacity.has_value());
    EXPECT_EQ(tighterCallerCapacity->workOrders.size(), 2u);
    EXPECT_EQ(tighterCallerCapacity->origins[0].expiresAt, 110u);
    std::optional<DedicatedGatheringTripProvenance> const firstTightProvenance =
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(*tighterCallerCapacity, tighterCallerCapacity->workOrders[0]);
    std::optional<DedicatedGatheringTripProvenance> const secondTightProvenance =
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(*tighterCallerCapacity, tighterCallerCapacity->workOrders[1]);
    ASSERT_TRUE(firstTightProvenance.has_value());
    ASSERT_TRUE(secondTightProvenance.has_value());
    EXPECT_EQ(firstTightProvenance->origins[0].origin.expiresAt, 110u);
    EXPECT_EQ(secondTightProvenance->origins[0].origin.expiresAt, 110u);

    request.origins[0].state = DedicatedGatheringOriginState::Latent;
    std::optional<DedicatedGatheringProvenancePlan> const allLatent =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(allLatent.has_value());
    EXPECT_TRUE(allLatent->workOrders.empty());
    EXPECT_EQ(allLatent->assignedQuantity, 0u);
    EXPECT_EQ(allLatent->unassignedBatchQuantity, 0u);
    EXPECT_EQ(allLatent->deferredActiveQuantity, 0u);
    EXPECT_EQ(allLatent->latentQuantity, 54u);
}

TEST(PlayerbotEconomyGatheringTest, DedicatedTripRejectsInvalidOriginProvenance)
{
    DedicatedGatheringPlanRequest valid{
        .tripIdentity = "trip-valid",
        .observedAt = 100u,
        .batchQuantity = 4u,
        .origins =
            {
                {.originIdentity = "origin-valid",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = 4u,
                 .expiresAt = 101u},
            },
    };
    std::array<DedicatedGatheringCandidate, 1> const candidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 4u},
    };
    auto expectInvalid = [&candidates](DedicatedGatheringPlanRequest const& request)
    { EXPECT_FALSE(PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates).has_value()); };

    DedicatedGatheringPlanRequest invalid = valid;
    invalid.tripIdentity.clear();
    expectInvalid(invalid);
    invalid = valid;
    invalid.origins.clear();
    expectInvalid(invalid);
    invalid = valid;
    invalid.origins[0].originIdentity.clear();
    expectInvalid(invalid);
    invalid = valid;
    invalid.origins[0].originIdentity = invalid.tripIdentity;
    expectInvalid(invalid);
    invalid = valid;
    invalid.origins.push_back(invalid.origins[0]);
    expectInvalid(invalid);
    invalid = valid;
    invalid.batchQuantity = 0u;
    expectInvalid(invalid);
    invalid = valid;
    invalid.origins[0].quantity = 0u;
    expectInvalid(invalid);
    invalid = valid;
    invalid.origins[0].expiresAt = invalid.observedAt;
    expectInvalid(invalid);
    invalid.origins[0].expiresAt = invalid.observedAt - 1u;
    expectInvalid(invalid);

    DedicatedGatheringPlanRequest const duplicateActorRequest{
        .tripIdentity = "trip-duplicate-actor",
        .observedAt = 100u,
        .batchQuantity = 8u,
        .origins = {{.originIdentity = "active-duplicate-actor",
                     .state = DedicatedGatheringOriginState::Active,
                     .quantity = 8u,
                     .expiresAt = 130u}},
    };
    std::array<DedicatedGatheringCandidate, 2> const duplicateActors = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 4u},
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 4u},
    };
    EXPECT_FALSE(PlayerbotEconomyGathering::PlanDedicatedWork(duplicateActorRequest, duplicateActors).has_value());
}

TEST(PlayerbotEconomyGatheringTest, DedicatedTripPlanOwnsValuesAndRejectsForeignOrInconsistentWorkOrders)
{
    DedicatedGatheringPlanRequest request{
        .tripIdentity = "trip-owned",
        .observedAt = 100u,
        .batchQuantity = 4u,
        .origins =
            {
                {.originIdentity = "active-owned",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = 4u,
                 .expiresAt = 130u},
                {.originIdentity = "latent-owned",
                 .state = DedicatedGatheringOriginState::Latent,
                 .quantity = 3u,
                 .expiresAt = 140u},
            },
    };
    std::array<DedicatedGatheringCandidate, 1> const candidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 4u},
    };
    std::optional<DedicatedGatheringProvenancePlan> const planned =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(planned.has_value());
    ASSERT_EQ(planned->workOrders.size(), 1u);

    request.tripIdentity = "changed";
    request.origins[0].originIdentity = "changed-origin";
    EXPECT_EQ(planned->tripIdentity, "trip-owned");
    EXPECT_EQ(planned->origins[0].originIdentity, "active-owned");

    DedicatedGatheringWorkOrder invalid = planned->workOrders[0];
    invalid.characterGuid = 99u;
    EXPECT_FALSE(PlayerbotEconomyGathering::ProvenanceForWorkOrder(*planned, invalid).has_value());
    invalid = planned->workOrders[0];
    ++invalid.quantity;
    EXPECT_FALSE(PlayerbotEconomyGathering::ProvenanceForWorkOrder(*planned, invalid).has_value());
    DedicatedGatheringProvenancePlan inconsistent = *planned;
    inconsistent.workOrders[0].allocations.push_back(inconsistent.workOrders[0].allocations.front());
    EXPECT_FALSE(
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(inconsistent, inconsistent.workOrders[0]).has_value());
    inconsistent = *planned;
    inconsistent.workOrders[0].allocations[0].originIdentity = "unknown";
    EXPECT_FALSE(
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(inconsistent, inconsistent.workOrders[0]).has_value());
    inconsistent = *planned;
    inconsistent.workOrders[0].allocations[0].originIdentity = "latent-owned";
    EXPECT_FALSE(
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(inconsistent, inconsistent.workOrders[0]).has_value());
    inconsistent = *planned;
    inconsistent.workOrders[0].allocations[0].quantity = 3u;
    EXPECT_FALSE(
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(inconsistent, inconsistent.workOrders[0]).has_value());

    inconsistent = *planned;
    inconsistent.workOrders.push_back(inconsistent.workOrders[0]);
    inconsistent.workOrders.back().characterGuid = 11u;
    inconsistent.assignedQuantity += inconsistent.workOrders.back().quantity;
    inconsistent.unassignedBatchQuantity = 0u;
    EXPECT_FALSE(
        PlayerbotEconomyGathering::ProvenanceForWorkOrder(inconsistent, inconsistent.workOrders[0]).has_value());

    DedicatedGatheringPlanRequest const splitRequest{
        .tripIdentity = "trip-global-allocation",
        .observedAt = 100u,
        .batchQuantity = 8u,
        .origins =
            {
                {.originIdentity = "active-global-a",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = 4u,
                 .expiresAt = 130u},
                {.originIdentity = "active-global-b",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = 4u,
                 .expiresAt = 130u},
            },
    };
    std::array<DedicatedGatheringCandidate, 2> const splitCandidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 4u},
        DedicatedGatheringCandidate{.characterGuid = 11u, .capacity = 4u},
    };
    std::optional<DedicatedGatheringProvenancePlan> splitPlan =
        PlayerbotEconomyGathering::PlanDedicatedWork(splitRequest, splitCandidates);
    ASSERT_TRUE(splitPlan.has_value());
    ASSERT_EQ(splitPlan->workOrders.size(), 2u);
    splitPlan->workOrders[1].allocations[0].originIdentity = "active-global-a";
    EXPECT_FALSE(PlayerbotEconomyGathering::ProvenanceForWorkOrder(*splitPlan, splitPlan->workOrders[0]).has_value());
}

TEST(PlayerbotEconomyGatheringTest, DedicatedTripSeparatesBatchShortfallFromDeferredActiveDemand)
{
    DedicatedGatheringPlanRequest const request{
        .tripIdentity = "trip-bounded",
        .observedAt = 100u,
        .batchQuantity = 6u,
        .origins = {{.originIdentity = "active-bounded",
                     .state = DedicatedGatheringOriginState::Active,
                     .quantity = 10u,
                     .expiresAt = 130u}},
    };
    std::array<DedicatedGatheringCandidate, 1> const candidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = 4u},
    };

    std::optional<DedicatedGatheringProvenancePlan> const plan =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->assignedQuantity, 4u);
    EXPECT_EQ(plan->unassignedBatchQuantity, 2u);
    EXPECT_EQ(plan->deferredActiveQuantity, 4u);
}

TEST(PlayerbotEconomyGatheringTest, DedicatedTripUsesWideActiveAndLatentAccounting)
{
    constexpr uint32 MAX_QUANTITY = std::numeric_limits<uint32>::max();
    DedicatedGatheringPlanRequest const request{
        .tripIdentity = "trip-wide",
        .observedAt = 100u,
        .batchQuantity = MAX_QUANTITY,
        .origins =
            {
                {.originIdentity = "active-wide-a",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = MAX_QUANTITY,
                 .expiresAt = 130u},
                {.originIdentity = "active-wide-b",
                 .state = DedicatedGatheringOriginState::Active,
                 .quantity = MAX_QUANTITY,
                 .expiresAt = 130u},
                {.originIdentity = "latent-wide",
                 .state = DedicatedGatheringOriginState::Latent,
                 .quantity = MAX_QUANTITY,
                 .expiresAt = 130u},
            },
    };
    std::array<DedicatedGatheringCandidate, 1> const candidates = {
        DedicatedGatheringCandidate{.characterGuid = 10u, .capacity = MAX_QUANTITY},
    };

    std::optional<DedicatedGatheringProvenancePlan> const plan =
        PlayerbotEconomyGathering::PlanDedicatedWork(request, candidates);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->assignedQuantity, MAX_QUANTITY);
    EXPECT_EQ(plan->unassignedBatchQuantity, 0u);
    EXPECT_EQ(plan->deferredActiveQuantity, static_cast<uint64>(MAX_QUANTITY));
    EXPECT_EQ(plan->latentQuantity, static_cast<uint64>(MAX_QUANTITY));
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
    EXPECT_EQ(experience.resourceAttempts, 3u);
    EXPECT_EQ(experience.resourceSeconds, 30u);
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
    EXPECT_EQ(experience.resourceAttempts, 5u);
    EXPECT_EQ(experience.resourceSeconds, 50u);
    // Back-to-back gathering is additional activity. Time spent on the second
    // trip cannot simultaneously recover the first trip's activity debt.
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 170u), 30u);
    EXPECT_EQ(gathering.AvailableDedicatedActivityBudget(10u, 100u, 200u), 60u);
}

TEST(PlayerbotEconomyGatheringTest, IdleWanderingStrategiesAreSuspendedForAnEconomyWalk)
{
    // Random bots carry grind plus one idle strategy; only the idle one leaves during the walk.
    EXPECT_EQ(PlayerbotEconomyGathering::IdleStrategiesToSuspend({"grind", "rpg", "loot"}),
              std::vector<std::string>{"rpg"});
    EXPECT_EQ(PlayerbotEconomyGathering::IdleStrategiesToSuspend({"new rpg", "grind"}),
              std::vector<std::string>{"new rpg"});
    EXPECT_EQ(PlayerbotEconomyGathering::IdleStrategiesToSuspend({"move random"}),
              std::vector<std::string>{"move random"});
    EXPECT_TRUE(PlayerbotEconomyGathering::IdleStrategiesToSuspend({"grind", "loot", "travel"}).empty());
}
