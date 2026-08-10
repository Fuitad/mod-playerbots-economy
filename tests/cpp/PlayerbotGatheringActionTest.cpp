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
GatheringResource Resource(GatheringProfession profession, uint64 resourceGuid = 1000u)
{
    GatheringResource resource;
    resource.resourceGuid = resourceGuid;
    resource.profession = profession;
    resource.mapId = 1u;
    resource.phaseMask = 3u;
    resource.requiredSkill = 75u;
    resource.spawned = true;
    return resource;
}

GatheringCandidate Candidate(uint32 characterGuid, GatheringProfession profession)
{
    GatheringCandidate candidate;
    candidate.characterGuid = characterGuid;
    candidate.profession = profession;
    candidate.skillValue = 100u;
    candidate.economyAffinity = 25u;
    candidate.botDistance = 8.0f;
    candidate.formationDistance = 9.0f;
    candidate.lootDistance = 15.0f;
    candidate.hasCareer = true;
    candidate.hasLearnedSkill = true;
    candidate.grouped = true;
    candidate.sameMap = true;
    candidate.samePhase = true;
    candidate.pathAvailable = true;
    candidate.safe = true;
    return candidate;
}
}  // namespace

TEST(PlayerbotGatheringActionTest, EveryPartyWinningConditionHasAnExactReleaseCause)
{
    struct Cancellation
    {
        bool GatheringContinuationFacts::* member;
        GatheringReleaseCause cause;
    };
    std::array<Cancellation, 9> const cancellations = {
        Cancellation{&GatheringContinuationFacts::inCombat, GatheringReleaseCause::Combat},
        Cancellation{&GatheringContinuationFacts::onTransport, GatheringReleaseCause::Transport},
        Cancellation{&GatheringContinuationFacts::commandReplaced, GatheringReleaseCause::CommandReplacement},
        Cancellation{&GatheringContinuationFacts::pathFailed, GatheringReleaseCause::PathFailure},
        Cancellation{&GatheringContinuationFacts::mapChanged, GatheringReleaseCause::MapChanged},
        Cancellation{&GatheringContinuationFacts::phaseChanged, GatheringReleaseCause::PhaseChanged},
        Cancellation{&GatheringContinuationFacts::formationMoved, GatheringReleaseCause::FormationMoved},
        Cancellation{&GatheringContinuationFacts::despawned, GatheringReleaseCause::Despawned},
        Cancellation{&GatheringContinuationFacts::higherPriorityBehavior,
                     GatheringReleaseCause::HigherPriorityBehavior}};

    for (Cancellation const& cancellation : cancellations)
    {
        GatheringContinuationFacts facts;
        facts.*cancellation.member = true;
        EXPECT_EQ(PlayerbotEconomyGathering::ReleaseCause(facts), cancellation.cause);
    }

    EXPECT_FALSE(PlayerbotEconomyGathering::ReleaseCause({}).has_value());
    GatheringContinuationFacts success;
    success.succeeded = true;
    EXPECT_EQ(PlayerbotEconomyGathering::ReleaseCause(success), GatheringReleaseCause::Success);
}

TEST(PlayerbotGatheringActionTest, CancellationReleasesOnceBeforeAnotherGathererMayOwnResource)
{
    PlayerbotEconomyGathering gathering;
    GatheringResource const resource = Resource(GatheringProfession::Mining);
    GatheringClaimResult const first =
        gathering.ClaimGrouped(resource, {Candidate(10u, GatheringProfession::Mining)}, 100u, 30u);
    ASSERT_TRUE(first.claim.has_value());

    EXPECT_TRUE(gathering.Release(first.claim->leaseId, GatheringReleaseCause::Combat));
    EXPECT_FALSE(gathering.Release(first.claim->leaseId, GatheringReleaseCause::Combat));

    GatheringClaimResult const second =
        gathering.ClaimGrouped(resource, {Candidate(11u, GatheringProfession::Mining)}, 101u, 30u);
    ASSERT_TRUE(second.claim.has_value());
    EXPECT_EQ(second.claim->characterGuid, 11u);
}

TEST(PlayerbotGatheringActionTest, SuccessfulOrdinaryInteractionEndsOneClaimWithoutChaining)
{
    std::array<GatheringProfession, 3> const professions = {GatheringProfession::Mining, GatheringProfession::Herbalism,
                                                            GatheringProfession::Skinning};

    uint64 resourceGuid = 1000u;
    for (GatheringProfession profession : professions)
    {
        PlayerbotEconomyGathering gathering;
        GatheringCandidate const candidate = Candidate(10u, profession);
        GatheringClaimResult const first =
            gathering.ClaimGrouped(Resource(profession, resourceGuid++), {candidate}, 100u, 30u);
        ASSERT_TRUE(first.claim.has_value());
        EXPECT_TRUE(gathering.Release(first.claim->leaseId, GatheringReleaseCause::Success));

        GatheringClaimResult const chained =
            gathering.ClaimGrouped(Resource(profession, resourceGuid++), {candidate}, 101u, 30u);
        EXPECT_FALSE(chained.claim.has_value());
        EXPECT_EQ(chained.blocker, GatheringBlocker::AlreadyClaimed);

        GatheringClaimResult const afterWindow =
            gathering.ClaimGrouped(Resource(profession, resourceGuid++), {candidate}, 131u, 30u);
        EXPECT_TRUE(afterWindow.claim.has_value());
    }
}
