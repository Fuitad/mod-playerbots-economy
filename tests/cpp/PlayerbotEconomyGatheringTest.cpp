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

    facts.inventoryCapacity = true;
    facts.safe = false;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.blocker, AutonomousGatheringBlocker::Unsafe);

    facts.safe = true;
    facts.now = 200u;
    decision = PlayerbotEconomyGathering::DecideAutonomous(plan, facts);
    EXPECT_EQ(decision.blocker, AutonomousGatheringBlocker::DestinationExpired);
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
