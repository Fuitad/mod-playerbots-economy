/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <memory>
#include <optional>

#include "Ai/Base/Actions/EconomyGatheringAction.h"
#include "Ai/World/Rpg/Action/RpgSubActions.h"
#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "Bot/Economy/PlayerbotEconomyTrace.h"
#include "Bot/Economy/PlayerbotEconomyTravel.h"
#include "Bot/Engine/AiObjectContext.h"
#include "Bot/Extension/PlayerbotExtension.h"
#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "Bot/Personality/PlayerbotCareerProgression.h"
#include "GameTime.h"
#include "IntegrationTestFixture.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "Strategy.h"
#include "gtest/gtest.h"

void AddPlayerbotsEconomyScripts();

namespace
{
// Lowest affinity at which a profession candidate stays legal, so it is also the lowest
// engagement a planned career can carry.
constexpr uint8 ACTIVE_AFFINITY_FIXTURE_MINIMUM = 25u;

PlayerbotCareerCandidateSeed CraftingSeed(uint16 skillId)
{
    return {{skillId}, {}, true, false, 100u, "crafting profession"};
}

PlayerbotCareerCandidateSeed GatheringSeed(uint16 skillId)
{
    return {{skillId}, {}, false, true, 100u, "gathering profession"};
}

PlayerbotCareerCandidateSeed MixedSeed(uint16 craftingSkillId, uint16 gatheringSkillId)
{
    return {{craftingSkillId, gatheringSkillId}, {}, true, true, 100u, "crafting and gathering professions"};
}

PlayerbotPersonalityProfile Profile(uint8 crafting, uint8 gathering)
{
    PlayerbotPersonalityProfile profile;
    profile.craftingAffinity = crafting;
    profile.gatheringAffinity = gathering;
    profile.economyAffinity = 50;
    profile.explorationAffinity = 50;
    profile.sociability = 50;
    profile.voice = PlayerbotVoice::Pragmatic;
    return profile;
}

class TestCareerProvider : public PlayerbotCareerPlanProvider
{
public:
    bool TrySubmit(PlayerbotCareerPlanRequest const& request) override
    {
        submitted = request;
        return acceptsSubmission;
    }

    std::optional<PlayerbotCareerPlanResponse> Poll(uint64 requestId) override
    {
        if (!response || response->requestId != requestId)
            return std::nullopt;

        return response;
    }

    uint64 ResponseDeadlineMs() const override { return responseDeadlineMs; }

    bool acceptsSubmission = true;
    uint64 responseDeadlineMs = 250u;
    std::optional<PlayerbotCareerPlanRequest> submitted;
    std::optional<PlayerbotCareerPlanResponse> response;
};

}  // namespace

TEST(PlayerbotCareerPlanTest, LowAffinitiesProduceOnlyNoProfessionCandidate)
{
    std::vector<PlayerbotCareerCandidateSeed> const seeds = {CraftingSeed(101u), GatheringSeed(202u),
                                                             MixedSeed(101u, 202u)};

    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(0u, 0u), seeds, 2u);

    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_TRUE(candidates.front().primarySkills.empty());
    EXPECT_TRUE(candidates.front().secondarySkills.empty());
    EXPECT_EQ(candidates.front().spendingStyle, PlayerbotRecipeSpendingStyle::None);
    EXPECT_FALSE(candidates.front().marketEligible);
    EXPECT_EQ(candidates.front().engagement, 0u);
}

TEST(PlayerbotCareerPlanTest, IndependentAffinitiesShapeLegalCandidates)
{
    std::vector<PlayerbotCareerCandidateSeed> const seeds = {CraftingSeed(101u), GatheringSeed(202u),
                                                             MixedSeed(101u, 202u)};

    std::vector<PlayerbotCareerCandidate> const crafting =
        PlayerbotCareer::BuildCandidates(Profile(90u, 10u), seeds, 2u);
    std::vector<PlayerbotCareerCandidate> const gathering =
        PlayerbotCareer::BuildCandidates(Profile(10u, 90u), seeds, 2u);
    std::vector<PlayerbotCareerCandidate> const mixed = PlayerbotCareer::BuildCandidates(Profile(90u, 90u), seeds, 2u);

    ASSERT_EQ(crafting.size(), 2u);
    EXPECT_TRUE(crafting.front().primarySkills.empty());
    EXPECT_EQ(crafting.back().primarySkills, std::vector<uint16>({101u}));
    EXPECT_EQ(crafting.back().spendingStyle, PlayerbotRecipeSpendingStyle::Completionist);
    EXPECT_EQ(crafting.back().engagement, 90u);

    ASSERT_EQ(gathering.size(), 2u);
    EXPECT_TRUE(gathering.front().primarySkills.empty());
    EXPECT_EQ(gathering.back().primarySkills, std::vector<uint16>({202u}));
    EXPECT_EQ(gathering.back().spendingStyle, PlayerbotRecipeSpendingStyle::Minimal);
    EXPECT_EQ(gathering.back().engagement, 90u);

    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_TRUE(mixed.front().primarySkills.empty());
    EXPECT_EQ(mixed.back().primarySkills, std::vector<uint16>({101u, 202u}));
    EXPECT_EQ(mixed.back().engagement, 90u);
}

TEST(PlayerbotCareerPlanTest, PrimaryLimitAndDuplicateSkillsAreEnforced)
{
    std::vector<PlayerbotCareerCandidateSeed> const seeds = {
        CraftingSeed(101u), MixedSeed(101u, 202u), {{101u, 101u}, {}, true, false, 100u, "invalid duplicate"}};

    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(90u, 90u), seeds, 1u);

    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_TRUE(candidates.front().primarySkills.empty());
    EXPECT_EQ(candidates.back().primarySkills, std::vector<uint16>({101u}));
}

TEST(PlayerbotCareerPlanTest, TokensAreOpaqueAndFallbackIsDeterministic)
{
    std::vector<PlayerbotCareerCandidateSeed> const seeds = {CraftingSeed(101u), GatheringSeed(202u)};
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(70u, 70u), seeds, 2u);

    ASSERT_EQ(candidates.size(), 3u);
    EXPECT_EQ(candidates[1].token.find("career-"), 0u);
    EXPECT_EQ(candidates[2].token.find("career-"), 0u);
    EXPECT_EQ(candidates[1].token.find("101"), std::string::npos);
    EXPECT_EQ(candidates[2].token.find("202"), std::string::npos);
    EXPECT_EQ(PlayerbotCareer::SelectFallback(candidates, 42u).token,
              PlayerbotCareer::SelectFallback(candidates, 42u).token);
}

TEST(PlayerbotCareerPlanTest, CandidateTokensRemainStableWhenSeedOrderChanges)
{
    std::vector<PlayerbotCareerCandidateSeed> const firstOrder = {CraftingSeed(101u), GatheringSeed(202u)};
    std::vector<PlayerbotCareerCandidateSeed> const secondOrder = {GatheringSeed(202u), CraftingSeed(101u)};

    std::vector<PlayerbotCareerCandidate> const first =
        PlayerbotCareer::BuildCandidates(Profile(70u, 70u), firstOrder, 2u);
    std::vector<PlayerbotCareerCandidate> const second =
        PlayerbotCareer::BuildCandidates(Profile(70u, 70u), secondOrder, 2u);

    ASSERT_EQ(first.size(), 3u);
    ASSERT_EQ(second.size(), 3u);
    EXPECT_EQ(first[0].token, second[0].token);
    EXPECT_EQ(first[1].token, second[2].token);
    EXPECT_EQ(first[2].token, second[1].token);
}

TEST(PlayerbotCareerPlanTest, ActiveBotsCanStillSelectNoProfession)
{
    std::vector<PlayerbotCareerCandidateSeed> const seeds = {CraftingSeed(101u), GatheringSeed(202u),
                                                             MixedSeed(101u, 202u)};
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(90u, 90u), seeds, 2u);

    bool selectedNoProfession = false;
    bool selectedProfession = false;
    for (uint64 guidCounter = 1u; guidCounter <= 5000u; ++guidCounter)
    {
        PlayerbotCareerCandidate const selected = PlayerbotCareer::SelectFallback(candidates, guidCounter);
        selectedNoProfession |= selected.primarySkills.empty() && selected.secondarySkills.empty();
        selectedProfession |= !selected.primarySkills.empty() || !selected.secondarySkills.empty();
    }

    EXPECT_TRUE(selectedNoProfession);
    EXPECT_TRUE(selectedProfession);
}

TEST(PlayerbotCareerPlanTest, SecondaryProfessionsAreOptionalCandidateVariants)
{
    std::vector<PlayerbotCareerCandidateSeed> const seeds = {
        CraftingSeed(101u), {{101u}, {303u}, true, false, 100u, "crafting profession with optional secondary"}};
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(90u, 10u), seeds, 2u);

    ASSERT_EQ(candidates.size(), 3u);
    EXPECT_TRUE(candidates[1].secondarySkills.empty());
    EXPECT_EQ(candidates[2].secondarySkills, std::vector<uint16>({303u}));
}

TEST(PlayerbotCareerPlanTest, PersistedPlanRoundTripsAndRejectsMalformedData)
{
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(90u, 90u), {MixedSeed(101u, 202u)}, 2u);
    PlayerbotCareerPlan const plan =
        PlayerbotCareer::MakePlan(42u, candidates.back(), PlayerbotRecipeSpendingStyle::Progression);
    std::string const serialized = PlayerbotCareer::SerializePlan(plan);

    std::optional<PlayerbotCareerPlan> const restored = PlayerbotCareer::DeserializePlan(serialized, 42u, candidates);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->candidateToken, plan.candidateToken);
    EXPECT_EQ(restored->primarySkills, plan.primarySkills);
    EXPECT_EQ(restored->secondarySkills, plan.secondarySkills);
    EXPECT_EQ(restored->engagement, 90u);

    EXPECT_FALSE(PlayerbotCareer::DeserializePlan(serialized, 43u, candidates));
    EXPECT_FALSE(PlayerbotCareer::DeserializePlan("malformed", 42u, candidates));

    PlayerbotCareerPlan unknownCandidate = plan;
    unknownCandidate.candidateToken = "career-unknown";
    EXPECT_FALSE(PlayerbotCareer::DeserializePlan(PlayerbotCareer::SerializePlan(unknownCandidate), 42u, candidates));

    PlayerbotCareerPlan alteredSkills = plan;
    alteredSkills.primarySkills = {303u};
    EXPECT_FALSE(PlayerbotCareer::DeserializePlan(PlayerbotCareer::SerializePlan(alteredSkills), 42u, candidates));
}

TEST(PlayerbotCareerPlanTest, CapabilityGoalAssignmentPreservesBaseCareerAndRejectsInvalidGoals)
{
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(90u, 10u), {CraftingSeed(101u)}, 2u);
    PlayerbotCareerPlan plan =
        PlayerbotCareer::MakePlan(42u, candidates.back(), PlayerbotRecipeSpendingStyle::Progression);
    std::vector<uint16> const primaryProfessionSkills = {101u, 303u};
    std::vector<uint16> const originalPrimarySkills = plan.primarySkills;
    std::vector<uint16> const originalSecondarySkills = plan.secondarySkills;
    PlayerbotCareerCapabilityGoal const trainerGoal = {PlayerbotCareerCapabilityGoalKind::Trainer, 303u, 0u, 8001u};

    EXPECT_TRUE(PlayerbotCareer::TryAssignCapabilityGoal(plan, trainerGoal, primaryProfessionSkills));
    EXPECT_EQ(plan.capabilityGoal, trainerGoal);
    EXPECT_EQ(plan.primarySkills, originalPrimarySkills);
    EXPECT_EQ(plan.secondarySkills, originalSecondarySkills);
    EXPECT_FALSE(PlayerbotCareer::TryAssignCapabilityGoal(plan, trainerGoal, primaryProfessionSkills));

    EXPECT_TRUE(PlayerbotCareer::ClearCapabilityGoal(plan));
    EXPECT_FALSE(plan.capabilityGoal);
    EXPECT_FALSE(PlayerbotCareer::ClearCapabilityGoal(plan));

    EXPECT_FALSE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Trainer, 404u, 9001u, 8001u}, primaryProfessionSkills));
    EXPECT_FALSE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Trainer, 101u, 9001u, 8001u}, primaryProfessionSkills));
    EXPECT_FALSE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Recipe, 303u, 9001u, 8001u}, primaryProfessionSkills));
    EXPECT_FALSE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Recipe, 101u, 0u, 8001u}, primaryProfessionSkills));
    EXPECT_FALSE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Recipe, 101u, 9001u, 0u}, primaryProfessionSkills));
    EXPECT_TRUE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Recipe, 101u, 9001u, 8001u}, primaryProfessionSkills));
}

TEST(PlayerbotCareerPlanTest, CapabilityGoalRoundTripsWithAuthoritativePrimaryValidation)
{
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(90u, 10u), {CraftingSeed(101u)}, 2u);
    std::vector<uint16> const primaryProfessionSkills = {101u, 303u};
    PlayerbotCareerPlan plan =
        PlayerbotCareer::MakePlan(42u, candidates.back(), PlayerbotRecipeSpendingStyle::Progression);
    PlayerbotCareerCapabilityGoal const recipeGoal = {PlayerbotCareerCapabilityGoalKind::Recipe, 101u, 9001u, 8001u};
    ASSERT_TRUE(PlayerbotCareer::TryAssignCapabilityGoal(plan, recipeGoal, primaryProfessionSkills));

    std::string const serialized = PlayerbotCareer::SerializePlan(plan);
    std::optional<PlayerbotCareerPlan> const restored =
        PlayerbotCareer::DeserializePlan(serialized, 42u, candidates, primaryProfessionSkills);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->capabilityGoal, recipeGoal);
    EXPECT_EQ(restored->primarySkills, plan.primarySkills);
    EXPECT_EQ(restored->secondarySkills, plan.secondarySkills);

    EXPECT_FALSE(PlayerbotCareer::DeserializePlan(serialized, 42u, candidates, {303u}));
}

TEST(PlayerbotCareerPlanTest, CurrentStoredPlanLoadsWithoutRequestingReplacement)
{
    TestCareerProvider provider;
    ASSERT_TRUE(PlayerbotCareer::RegisterProvider(&provider));
    PlayerbotPersonalityProfile const profile = Profile(90u, 90u);
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(profile, {MixedSeed(101u, 202u)}, 2u);
    PlayerbotCareerPlan const plan =
        PlayerbotCareer::MakePlan(42u, candidates.back(), PlayerbotRecipeSpendingStyle::Progression);

    PlayerbotCareerPlanRecovery const recovery =
        PlayerbotCareer::ResolvePersistedPlan(PlayerbotCareer::SerializePlan(plan), 42u, profile, candidates, 1000u);

    ASSERT_EQ(recovery.status, PlayerbotCareerPlanResolutionStatus::Resolved);
    EXPECT_FALSE(recovery.shouldPersist);
    EXPECT_FALSE(provider.submitted.has_value());
    EXPECT_EQ(recovery.plan.candidateToken, plan.candidateToken);
    PlayerbotCareer::UnregisterProvider(&provider);
}

TEST(PlayerbotCareerPlanTest, PersistedCapabilityGoalUsesAuthoritativePrimaryProfessionValidation)
{
    PlayerbotPersonalityProfile const profile = Profile(90u, 90u);
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(profile, {CraftingSeed(101u)}, 2u);
    PlayerbotCareerPlan plan =
        PlayerbotCareer::MakePlan(42u, candidates.back(), PlayerbotRecipeSpendingStyle::Progression);
    PlayerbotCareerCapabilityGoal const goal = {PlayerbotCareerCapabilityGoalKind::Recipe, 303u, 9001u, 8001u};
    ASSERT_TRUE(PlayerbotCareer::TryAssignCapabilityGoal(plan, goal, {101u, 303u}, {303u}));

    PlayerbotCareerPlanRecovery const valid = PlayerbotCareer::ResolvePersistedPlan(
        PlayerbotCareer::SerializePlan(plan), 42u, profile, candidates, {101u, 303u}, {303u}, 1000u);
    ASSERT_EQ(valid.status, PlayerbotCareerPlanResolutionStatus::Resolved);
    EXPECT_FALSE(valid.shouldPersist);
    EXPECT_EQ(valid.plan.capabilityGoal, goal);

    PlayerbotCareerPlanRecovery const invalid = PlayerbotCareer::ResolvePersistedPlan(
        PlayerbotCareer::SerializePlan(plan), 42u, profile, candidates, {101u, 303u}, {}, 1000u);
    ASSERT_EQ(invalid.status, PlayerbotCareerPlanResolutionStatus::Resolved);
    EXPECT_TRUE(invalid.shouldPersist);
    EXPECT_FALSE(invalid.plan.capabilityGoal);
}

TEST(PlayerbotCareerPlanTest, InvalidPersistedPlanRegeneratesOnlyLegalCareerMetadata)
{
    PlayerbotPersonalityProfile const profile = Profile(90u, 90u);
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(profile, {MixedSeed(101u, 202u)}, 2u);
    PlayerbotCareerPlan invalidPlan =
        PlayerbotCareer::MakePlan(42u, candidates.back(), PlayerbotRecipeSpendingStyle::Progression);
    invalidPlan.candidateToken = "career-unknown";

    PlayerbotCareerPlanRecovery const recovery = PlayerbotCareer::ResolvePersistedPlan(
        PlayerbotCareer::SerializePlan(invalidPlan), 42u, profile, candidates, 1000u);

    ASSERT_EQ(recovery.status, PlayerbotCareerPlanResolutionStatus::Resolved);
    EXPECT_TRUE(recovery.shouldPersist);
    EXPECT_TRUE(PlayerbotCareer::DeserializePlan(PlayerbotCareer::SerializePlan(recovery.plan), 42u, candidates));
    EXPECT_EQ(recovery.plan.candidateToken, PlayerbotCareer::SelectFallback(candidates, 42u).token);
}

TEST(PlayerbotCareerPlanTest, MissingProviderResolvesDeterministicFallbackImmediately)
{
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(70u, 70u), {CraftingSeed(101u), GatheringSeed(202u)}, 2u);

    PlayerbotCareerPlanResolution const resolution =
        PlayerbotCareer::ResolvePlan(42u, Profile(70u, 70u), candidates, 1000u);

    ASSERT_EQ(resolution.status, PlayerbotCareerPlanResolutionStatus::Resolved);
    EXPECT_EQ(resolution.plan.candidateToken, PlayerbotCareer::SelectFallback(candidates, 42u).token);
}

TEST(PlayerbotCareerPlanTest, ProviderUsesOpaqueCandidatesAndValidCorrelatedResponse)
{
    TestCareerProvider provider;
    ASSERT_TRUE(PlayerbotCareer::RegisterProvider(&provider));
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(90u, 90u), {MixedSeed(101u, 202u)}, 2u);

    PlayerbotCareerPlanResolution const pending =
        PlayerbotCareer::ResolvePlan(42u, Profile(90u, 90u), candidates, 1000u);
    ASSERT_EQ(pending.status, PlayerbotCareerPlanResolutionStatus::Pending);
    ASSERT_TRUE(provider.submitted);
    ASSERT_EQ(provider.submitted->candidates.size(), candidates.size());
    EXPECT_EQ(provider.submitted->candidates.back().token, candidates.back().token);
    EXPECT_EQ(provider.submitted->candidates.back().summary, candidates.back().summary);

    provider.response =
        PlayerbotCareerPlanResponse{provider.submitted->requestId,     42u,
                                    PLAYERBOT_PERSONALITY_API_VERSION, PLAYERBOT_CAREER_PLAN_VERSION,
                                    candidates.back().token,           PlayerbotRecipeSpendingStyle::Progression};
    std::vector<PlayerbotCareerPlan> const resolved = PlayerbotCareer::PollPendingPlans(1001u);

    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved.front().candidateToken, candidates.back().token);
    EXPECT_EQ(resolved.front().spendingStyle, PlayerbotRecipeSpendingStyle::Progression);
    EXPECT_EQ(resolved.front().engagement, 90u);
    PlayerbotCareer::UnregisterProvider(&provider);
}

TEST(PlayerbotCareerPlanTest, InvalidAndTimedOutResponsesUseDeterministicFallback)
{
    TestCareerProvider provider;
    ASSERT_TRUE(PlayerbotCareer::RegisterProvider(&provider));
    std::vector<PlayerbotCareerCandidate> const candidates =
        PlayerbotCareer::BuildCandidates(Profile(70u, 70u), {CraftingSeed(101u), GatheringSeed(202u)}, 2u);

    ASSERT_EQ(PlayerbotCareer::ResolvePlan(42u, Profile(70u, 70u), candidates, 1000u).status,
              PlayerbotCareerPlanResolutionStatus::Pending);
    ASSERT_TRUE(provider.submitted);
    provider.response = PlayerbotCareerPlanResponse{provider.submitted->requestId,
                                                    99u,
                                                    PLAYERBOT_PERSONALITY_API_VERSION,
                                                    PLAYERBOT_CAREER_PLAN_VERSION,
                                                    "unknown-token",
                                                    PlayerbotRecipeSpendingStyle::Completionist};

    PlayerbotCareerPlanResolution const invalid =
        PlayerbotCareer::ResolvePlan(42u, Profile(70u, 70u), candidates, 1001u);
    ASSERT_EQ(invalid.status, PlayerbotCareerPlanResolutionStatus::Resolved);
    EXPECT_EQ(invalid.plan.candidateToken, PlayerbotCareer::SelectFallback(candidates, 42u).token);

    provider.response.reset();
    ASSERT_EQ(PlayerbotCareer::ResolvePlan(43u, Profile(70u, 70u), candidates, 2000u).status,
              PlayerbotCareerPlanResolutionStatus::Pending);
    PlayerbotCareerPlanResolution const timedOut =
        PlayerbotCareer::ResolvePlan(43u, Profile(70u, 70u), candidates, 2250u);
    ASSERT_EQ(timedOut.status, PlayerbotCareerPlanResolutionStatus::Resolved);
    EXPECT_EQ(timedOut.plan.candidateToken, PlayerbotCareer::SelectFallback(candidates, 43u).token);
    PlayerbotCareer::UnregisterProvider(&provider);
}

TEST(PlayerbotCareerPlanTest, TrainerLessonsFollowCareerAndSpendingStyle)
{
    PlayerbotCareerCandidate const candidate =
        PlayerbotCareer::BuildCandidates(Profile(90u, 90u), {MixedSeed(101u, 202u)}, 2u).back();
    std::vector<PlayerbotTrainerLessonCandidate> const lessons = {{1001u, 101u, 100u, true, true},
                                                                  {1002u, 101u, 50u, false, true},
                                                                  {1003u, 101u, 10u, false, false},
                                                                  {1004u, 303u, 1u, true, true}};

    PlayerbotCareerPlan progression =
        PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Progression);
    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(progression, lessons), std::vector<uint32>({1001u, 1002u}));

    PlayerbotCareerPlan completionist =
        PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Completionist);
    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(completionist, lessons),
              std::vector<uint32>({1001u, 1002u, 1003u}));

    PlayerbotCareerPlan noProfession =
        PlayerbotCareer::MakePlan(42u, PlayerbotCareerCandidate{}, PlayerbotRecipeSpendingStyle::None);
    EXPECT_TRUE(PlayerbotCareer::SelectTrainerLessons(noProfession, lessons).empty());
}

TEST(PlayerbotCareerPlanTest, TrainerCapabilityGoalSelectsOnlyRankLessonsForTheGoalSkill)
{
    PlayerbotCareerPlan plan =
        PlayerbotCareer::MakePlan(42u, PlayerbotCareerCandidate{}, PlayerbotRecipeSpendingStyle::None);
    ASSERT_TRUE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Trainer, 303u, 9001u, 8001u}, {303u}));
    std::vector<PlayerbotTrainerLessonCandidate> const lessons = {
        {1001u, 303u, 100u, true, true}, {1002u, 303u, 10u, false, true}, {1003u, 404u, 1u, true, true}};

    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(plan, lessons), std::vector<uint32>({1001u}));
}

TEST(PlayerbotCareerPlanTest, NoDemandCareerAcquisitionRequiresAuthoritativeSkillConfirmation)
{
    PlayerbotCareerPlan plan;
    plan.primarySkills = {SKILL_JEWELCRAFTING};

    PlayerbotCareerAcquisition const acquisition =
        PlayerbotCareer::SelectTrainerObjective(plan, {}, {SKILL_JEWELCRAFTING}, 1u);

    ASSERT_TRUE(acquisition.objective);
    EXPECT_EQ(acquisition.objective->kind, PlayerbotCareerTrainerObjectiveKind::BaseCareer);
    EXPECT_EQ(acquisition.objective->professionSkillId, SKILL_JEWELCRAFTING);
    EXPECT_TRUE(acquisition.objective->primaryProfession);
    EXPECT_EQ(acquisition.state, PlayerbotCareerAcquisitionState::Travel);
    EXPECT_EQ(acquisition.blocker, PlayerbotCareerAcquisitionBlocker::None);
    EXPECT_FALSE(plan.capabilityGoal);

    PlayerbotCareerAcquisitionFacts facts = {false, true, true, true, true, true, true};
    PlayerbotCareerAcquisition const unconfirmed =
        PlayerbotCareer::EvaluateTrainerObjective(*acquisition.objective, facts);
    EXPECT_EQ(unconfirmed.state, PlayerbotCareerAcquisitionState::Blocked);
    EXPECT_EQ(unconfirmed.blocker, PlayerbotCareerAcquisitionBlocker::CompletionUnobserved);

    facts.professionLearned = true;
    PlayerbotCareerAcquisition const complete =
        PlayerbotCareer::EvaluateTrainerObjective(*acquisition.objective, facts);
    EXPECT_EQ(complete.state, PlayerbotCareerAcquisitionState::Complete);
    EXPECT_EQ(complete.blocker, PlayerbotCareerAcquisitionBlocker::None);
}

TEST(PlayerbotCareerPlanTest, BaseCareerAndCapabilityRemediationObjectivesRemainDistinct)
{
    PlayerbotCareerPlan plan;
    plan.primarySkills = {SKILL_JEWELCRAFTING};
    plan.capabilityGoal =
        PlayerbotCareerCapabilityGoal{PlayerbotCareerCapabilityGoalKind::Trainer, SKILL_BLACKSMITHING, 0u, 2840u};

    PlayerbotCareerAcquisition const base =
        PlayerbotCareer::SelectTrainerObjective(plan, {}, {SKILL_JEWELCRAFTING, SKILL_BLACKSMITHING}, 1u);
    ASSERT_TRUE(base.objective);
    EXPECT_EQ(base.objective->kind, PlayerbotCareerTrainerObjectiveKind::BaseCareer);
    EXPECT_EQ(base.objective->professionSkillId, SKILL_JEWELCRAFTING);
    ASSERT_TRUE(plan.capabilityGoal);
    EXPECT_EQ(plan.capabilityGoal->professionSkillId, SKILL_BLACKSMITHING);

    PlayerbotCareerPlan remediationOnly;
    remediationOnly.capabilityGoal = plan.capabilityGoal;
    PlayerbotCareerAcquisition const remediation =
        PlayerbotCareer::SelectTrainerObjective(remediationOnly, {}, {SKILL_JEWELCRAFTING, SKILL_BLACKSMITHING}, 1u);
    ASSERT_TRUE(remediation.objective);
    EXPECT_EQ(remediation.objective->kind, PlayerbotCareerTrainerObjectiveKind::CapabilityRemediation);
    EXPECT_EQ(remediation.objective->professionSkillId, SKILL_BLACKSMITHING);
}

TEST(PlayerbotCareerPlanTest, PrimarySlotsBlockOnlyPrimaryCareerAcquisition)
{
    PlayerbotCareerPlan plan;
    plan.primarySkills = {SKILL_JEWELCRAFTING};

    PlayerbotCareerAcquisition const blocked =
        PlayerbotCareer::SelectTrainerObjective(plan, {}, {SKILL_JEWELCRAFTING}, 0u);
    ASSERT_TRUE(blocked.objective);
    EXPECT_EQ(blocked.state, PlayerbotCareerAcquisitionState::Blocked);
    EXPECT_EQ(blocked.blocker, PlayerbotCareerAcquisitionBlocker::PrimarySlotsOccupied);
    EXPECT_STREQ(PlayerbotCareer::AcquisitionBlockerCode(blocked.blocker), "primary_slots_occupied");

    plan.secondarySkills = {SKILL_COOKING, SKILL_FIRST_AID};
    PlayerbotCareerAcquisition const cooking =
        PlayerbotCareer::SelectTrainerObjective(plan, {}, {SKILL_JEWELCRAFTING}, 0u);
    ASSERT_TRUE(cooking.objective);
    EXPECT_EQ(cooking.objective->professionSkillId, SKILL_COOKING);
    EXPECT_FALSE(cooking.objective->primaryProfession);
    EXPECT_EQ(cooking.state, PlayerbotCareerAcquisitionState::Travel);

    PlayerbotCareerAcquisition const firstAid =
        PlayerbotCareer::SelectTrainerObjective(plan, {SKILL_COOKING}, {SKILL_JEWELCRAFTING}, 0u);
    ASSERT_TRUE(firstAid.objective);
    EXPECT_EQ(firstAid.objective->professionSkillId, SKILL_FIRST_AID);
    EXPECT_FALSE(firstAid.objective->primaryProfession);
    EXPECT_EQ(firstAid.state, PlayerbotCareerAcquisitionState::Travel);
}

TEST(PlayerbotCareerPlanTest, AcquisitionBlockersRemainExplicitAndStable)
{
    PlayerbotCareerTrainerObjective const objective = {PlayerbotCareerTrainerObjectiveKind::BaseCareer,
                                                       SKILL_JEWELCRAFTING, true};
    struct Scenario
    {
        PlayerbotCareerAcquisitionFacts facts;
        PlayerbotCareerAcquisitionBlocker blocker;
        char const* code;
    };
    std::array<Scenario, 5> const scenarios = {{
        {{false, false, true, true, true, false, false},
         PlayerbotCareerAcquisitionBlocker::TrainerUnavailable,
         "trainer_unavailable"},
        {{false, true, false, true, true, false, false},
         PlayerbotCareerAcquisitionBlocker::UnsafeRoute,
         "unsafe_route"},
        {{false, true, true, false, true, false, false},
         PlayerbotCareerAcquisitionBlocker::TrainerIneligible,
         "trainer_ineligible"},
        {{false, true, true, true, false, false, false},
         PlayerbotCareerAcquisitionBlocker::InsufficientProtectedMoney,
         "insufficient_protected_money"},
        {{false, true, true, true, true, true, true},
         PlayerbotCareerAcquisitionBlocker::CompletionUnobserved,
         "completion_unobserved"},
    }};

    for (Scenario const& scenario : scenarios)
    {
        PlayerbotCareerAcquisition const acquisition =
            PlayerbotCareer::EvaluateTrainerObjective(objective, scenario.facts);
        EXPECT_EQ(acquisition.state, PlayerbotCareerAcquisitionState::Blocked);
        EXPECT_EQ(acquisition.blocker, scenario.blocker);
        EXPECT_STREQ(PlayerbotCareer::AcquisitionBlockerCode(acquisition.blocker), scenario.code);
    }
}

TEST(PlayerbotCareerPlanTest, AcquisitionSelectsOnlyAffordableRankLessonForItsObjective)
{
    PlayerbotCareerTrainerObjective const objective = {PlayerbotCareerTrainerObjectiveKind::BaseCareer,
                                                       SKILL_JEWELCRAFTING, true};
    std::vector<PlayerbotTrainerLessonCandidate> const lessons = {
        {1001u, SKILL_JEWELCRAFTING, 100u, true, true},
        {1002u, SKILL_JEWELCRAFTING, 10u, false, true},
        {1003u, SKILL_BLACKSMITHING, 1u, true, true},
    };

    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(objective, lessons), std::vector<uint32>({1001u}));
    EXPECT_FALSE(PlayerbotCareer::HasAffordableTrainerLesson(objective, lessons, 99u));
    EXPECT_TRUE(PlayerbotCareer::HasAffordableTrainerLesson(objective, lessons, 100u));
}

TEST(PlayerbotCareerPlanTest, ProgressionTrainerObjectiveSelectsRankOrCheapestAdvancingRecipe)
{
    std::vector<PlayerbotTrainerLessonCandidate> const lessons = {
        {1001u, SKILL_TAILORING, 100u, true, true},
        {1002u, SKILL_TAILORING, 50u, false, true},
        {1003u, SKILL_TAILORING, 20u, false, true},
    };
    PlayerbotCareerTrainerObjective rank{
        .kind = PlayerbotCareerTrainerObjectiveKind::Progression,
        .professionSkillId = SKILL_TAILORING,
        .primaryProfession = true,
        .rankOnly = true,
    };
    PlayerbotCareerTrainerObjective recipe = rank;
    recipe.rankOnly = false;

    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(rank, lessons), std::vector<uint32>({1001u}));
    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(recipe, lessons), std::vector<uint32>({1003u}));
}

TEST(PlayerbotCareerPlanTest, MinimalTrainerStyleSelectsCheapestProgressionRecipePerSkill)
{
    PlayerbotCareerCandidate const candidate =
        PlayerbotCareer::BuildCandidates(Profile(40u, 10u), {CraftingSeed(101u)}, 2u).back();
    PlayerbotCareerPlan minimal = PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Minimal);
    std::vector<PlayerbotTrainerLessonCandidate> const lessons = {
        {1001u, 101u, 100u, true, true}, {1002u, 101u, 50u, false, true}, {1003u, 101u, 20u, false, true}};

    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(minimal, lessons), std::vector<uint32>({1001u, 1003u}));
}

TEST(PlayerbotCareerPlanTest, MinimalTrainerStyleIncludesRankWithoutProgressionRecipe)
{
    PlayerbotCareerCandidate const candidate =
        PlayerbotCareer::BuildCandidates(Profile(40u, 10u), {CraftingSeed(101u)}, 2u).back();
    PlayerbotCareerPlan minimal = PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Minimal);
    std::vector<PlayerbotTrainerLessonCandidate> const lessons = {{1001u, 101u, 100u, true, true}};

    EXPECT_EQ(PlayerbotCareer::SelectTrainerLessons(minimal, lessons), std::vector<uint32>({1001u}));
}

TEST(PlayerbotCareerPlanTest, CareerTelemetryDistinguishesUnavailablePendingLoadedAndSaved)
{
    PlayerbotEconomyTelemetry telemetry;
    EXPECT_FALSE(telemetry.FindCareer(42u).has_value());

    telemetry.PublishCareerPending(42u);
    PlayerbotCareerObservation career = *telemetry.FindCareer(42u);
    EXPECT_EQ(career.status, PlayerbotCareerTelemetryStatus::Pending);
    EXPECT_EQ(career.source, PlayerbotCareerTelemetrySource::None);

    career = {
        .status = PlayerbotCareerTelemetryStatus::Valid,
        .source = PlayerbotCareerTelemetrySource::Loaded,
        .version = PLAYERBOT_CAREER_PLAN_VERSION,
        .candidateToken = "career-loaded",
        .primarySkills = {164u},
        .secondarySkills = {185u},
        .spendingStyle = static_cast<std::uint8_t>(PlayerbotRecipeSpendingStyle::Progression),
        .marketEligible = true,
        .engagement = 75u,
    };
    telemetry.PublishCareer(42u, career);
    EXPECT_EQ(telemetry.FindCareer(42u), career);

    career.source = PlayerbotCareerTelemetrySource::Saved;
    career.candidateToken = "career-saved";
    telemetry.PublishCareer(42u, career);
    EXPECT_EQ(telemetry.FindCareer(42u), career);
}

TEST(PlayerbotCareerPlanTest, TrainerTravelRequiresAnAffordableSelectedLesson)
{
    PlayerbotCareerCandidate const candidate =
        PlayerbotCareer::BuildCandidates(Profile(70u, 10u), {CraftingSeed(101u)}, 2u).back();
    PlayerbotCareerPlan progression =
        PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Progression);
    std::vector<PlayerbotTrainerLessonCandidate> const lessons = {
        {1001u, 101u, 100u, true, true}, {1002u, 101u, 50u, false, true}, {1003u, 202u, 1u, true, true}};

    EXPECT_FALSE(PlayerbotCareer::HasAffordableTrainerLesson(progression, lessons, 49u));
    EXPECT_TRUE(PlayerbotCareer::HasAffordableTrainerLesson(progression, lessons, 50u));

    PlayerbotCareerPlan noProfession =
        PlayerbotCareer::MakePlan(42u, PlayerbotCareerCandidate{}, PlayerbotRecipeSpendingStyle::None);
    EXPECT_FALSE(PlayerbotCareer::HasAffordableTrainerLesson(noProfession, lessons, 1000u));
}

TEST(PlayerbotCareerPlanTest, ProfessionWorkIsScheduledOnlyForCareersWithAPlannedProfession)
{
    PlayerbotCareerCandidate const candidate =
        PlayerbotCareer::BuildCandidates(Profile(70u, 10u), {CraftingSeed(101u)}, 2u).back();
    PlayerbotCareerPlan const planned =
        PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Progression);

    EXPECT_TRUE(PlayerbotCareer::SchedulesProfessionWork(planned));
    EXPECT_GT(PlayerbotCareer::ProfessionWorkWeight(planned, 25u), 0u);

    PlayerbotCareerPlan const noProfession =
        PlayerbotCareer::MakePlan(42u, PlayerbotCareerCandidate{}, PlayerbotRecipeSpendingStyle::None);
    EXPECT_FALSE(PlayerbotCareer::SchedulesProfessionWork(noProfession));
    EXPECT_EQ(PlayerbotCareer::ProfessionWorkWeight(noProfession, 25u), 0u);

    PlayerbotCareerPlan capabilityGoal = noProfession;
    ASSERT_TRUE(PlayerbotCareer::TryAssignCapabilityGoal(
        capabilityGoal, {PlayerbotCareerCapabilityGoalKind::Trainer, 303u, 0u, 8001u}, {303u}));
    EXPECT_TRUE(PlayerbotCareer::SchedulesProfessionWork(capabilityGoal));
    EXPECT_GT(PlayerbotCareer::ProfessionWorkWeight(capabilityGoal, 25u), 0u);

    // Spending style controls recipe purchases. It cannot suppress acquisition of a persisted career.
    PlayerbotCareerPlan const disabled = PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::None);
    EXPECT_TRUE(PlayerbotCareer::SchedulesProfessionWork(disabled));
    EXPECT_GT(PlayerbotCareer::ProfessionWorkWeight(disabled, 25u), 0u);
}

TEST(PlayerbotCareerPlanTest, CareerTrainerDestinationsStayInsideTheBotsSafeLevelRange)
{
    EXPECT_TRUE(PlayerbotCareer::IsTrainerDestinationSafe(3u, 14u, 14u, 5u));
    EXPECT_FALSE(PlayerbotCareer::IsTrainerDestinationSafe(3u, 14u, 17u, 10u));
    EXPECT_FALSE(PlayerbotCareer::IsTrainerDestinationSafe(5u, 357u, 357u, 40u));
    EXPECT_TRUE(PlayerbotCareer::IsTrainerDestinationSafe(10u, 14u, 17u, 10u));
}

TEST(PlayerbotCareerPlanTest, NearbySameMapTrainerDoesNotRequireTravelGraphNodes)
{
    EXPECT_TRUE(PlayerbotEconomyTravelCatalog::IsTrainerRouteReachable(
        {.sameMap = true, .withinLocalRange = true, .travelNodePath = false}));
    EXPECT_FALSE(PlayerbotEconomyTravelCatalog::IsTrainerRouteReachable(
        {.sameMap = true, .withinLocalRange = false, .travelNodePath = false}));
    EXPECT_FALSE(PlayerbotEconomyTravelCatalog::IsTrainerRouteReachable(
        {.sameMap = false, .withinLocalRange = true, .travelNodePath = false}));
    EXPECT_TRUE(PlayerbotEconomyTravelCatalog::IsTrainerRouteReachable(
        {.sameMap = false, .withinLocalRange = false, .travelNodePath = true}));
}

TEST(PlayerbotCareerPlanTest, ProfessionWorkWeightRisesWithCareerEngagement)
{
    auto const plannedAt = [](uint8 craftingAffinity)
    {
        PlayerbotCareerCandidate const candidate =
            PlayerbotCareer::BuildCandidates(Profile(craftingAffinity, 10u), {CraftingSeed(101u)}, 2u).back();
        return PlayerbotCareer::MakePlan(42u, candidate, candidate.spendingStyle);
    };

    PlayerbotCareerPlan const low = plannedAt(ACTIVE_AFFINITY_FIXTURE_MINIMUM);
    PlayerbotCareerPlan const middle = plannedAt(60u);
    PlayerbotCareerPlan const high = plannedAt(100u);

    ASSERT_EQ(low.engagement, ACTIVE_AFFINITY_FIXTURE_MINIMUM);
    ASSERT_EQ(middle.engagement, 60u);
    ASSERT_EQ(high.engagement, 100u);

    uint32 const lowWeight = PlayerbotCareer::ProfessionWorkWeight(low, 40u);
    uint32 const middleWeight = PlayerbotCareer::ProfessionWorkWeight(middle, 40u);
    uint32 const highWeight = PlayerbotCareer::ProfessionWorkWeight(high, 40u);

    // Engagement changes scheduling preference only: it never disables an active career and
    // never exceeds the configured base weight.
    EXPECT_GT(lowWeight, 0u);
    EXPECT_GT(middleWeight, lowWeight);
    EXPECT_GT(highWeight, middleWeight);
    EXPECT_EQ(highWeight, 40u);
}

TEST(PlayerbotCareerPlanTest, EconomyModuleRegistersItsCycleActionThroughTheGenericSeam)
{
    AddPlayerbotsEconomyScripts();
    SharedNamedObjectContextList<::Action> contexts;
    GetPlayerbotExtensionRegistry().ForEach([&contexts](PlayerbotExtension& extension)
                                            { extension.AddActionContexts(contexts); });
    EXPECT_TRUE(contexts.creators.contains("economy cycle"));
}

TEST(PlayerbotCareerPlanTest, EconomyModuleOverridesNearbyGatheringThroughTheGenericSeam)
{
    AddPlayerbotsEconomyScripts();
    SharedNamedObjectContextList<::Action> contexts;
    GetPlayerbotExtensionRegistry().ForEach([&contexts](PlayerbotExtension& extension)
                                            { extension.AddActionContexts(contexts); });
    EXPECT_TRUE(contexts.creators.contains("add gathering loot"));
}

class PlayerbotProfessionInteractionTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();
        AddPlayerbotsEconomyScripts();

        static bool contextsBuilt = false;
        if (!contextsBuilt)
        {
            AiObjectContext::BuildAllSharedContexts();
            contextsBuilt = true;
        }

        auto& templates = *const_cast<ItemTemplateContainer*>(sObjectMgr->GetItemTemplateStore());
        auto& fastTemplates = *const_cast<std::vector<ItemTemplate*>*>(sObjectMgr->GetItemTemplateStoreFast());
        originalFastTemplateCount = fastTemplates.size();

        ItemTemplate material{};
        material.ItemId = static_cast<uint32>(std::max<std::size_t>(originalFastTemplateCount, 60'000u));
        material.Class = ITEM_CLASS_TRADE_GOODS;
        material.AllowableClass = -1;
        material.AllowableRace = -1;
        material.Stackable = 20;
        materialItemId = RegisterItemTemplate(templates, fastTemplates, std::move(material));
    }

    void TearDown() override
    {
        IntegrationTestFixture::TearDown();

        auto& templates = *const_cast<ItemTemplateContainer*>(sObjectMgr->GetItemTemplateStore());
        auto& fastTemplates = *const_cast<std::vector<ItemTemplate*>*>(sObjectMgr->GetItemTemplateStoreFast());
        for (auto const& [itemId, originalPointer] : originalItemPointers)
            if (itemId < fastTemplates.size())
                fastTemplates[itemId] = originalPointer;
        for (uint32 itemId : addedItemIds)
            templates.erase(itemId);
        templates.erase(materialItemId);
        fastTemplates.resize(originalFastTemplateCount);
    }

    static uint32 RegisterItemTemplate(ItemTemplateContainer& templates, std::vector<ItemTemplate*>& fastTemplates,
                                       ItemTemplate itemTemplate)
    {
        uint32 const itemId = itemTemplate.ItemId;
        auto const [iterator, inserted] = templates.emplace(itemId, std::move(itemTemplate));
        EXPECT_TRUE(inserted);
        if (fastTemplates.size() <= itemId)
            fastTemplates.resize(itemId + 1u);
        fastTemplates[itemId] = &iterator->second;
        return itemId;
    }

    static PlayerbotEconomy::GatheringResource GatheringResourceFor(PlayerbotEconomy::GatheringProfession profession,
                                                                    uint64 resourceGuid)
    {
        return {
            .resourceGuid = resourceGuid,
            .profession = profession,
            .mapId = 1u,
            .phaseMask = 1u,
            .requiredSkill = 1u,
            .spawned = true,
        };
    }

    static PlayerbotEconomy::GatheringCandidate GatheringCandidateFor(uint32 characterGuid,
                                                                      PlayerbotEconomy::GatheringProfession profession)
    {
        return {
            .characterGuid = characterGuid,
            .profession = profession,
            .skillValue = 100u,
            .economyAffinity = 100u,
            .botDistance = 1.0f,
            .formationDistance = 1.0f,
            .lootDistance = 5.0f,
            .hasCareer = true,
            .hasLearnedSkill = true,
            .grouped = false,
            .sameMap = true,
            .samePhase = true,
            .pathAvailable = true,
            .safe = true,
        };
    }

    static Item* StoreItem(TestPlayer* bot, uint32 itemId, uint32 quantity, uint32 itemGuid)
    {
        return StoreItem(bot, itemId, quantity, itemGuid, 0u);
    }

    static Item* StoreItem(TestPlayer* bot, uint32 itemId, uint32 quantity, uint32 itemGuid, uint8 slotOffset)
    {
        Item* item = new Item();
        EXPECT_TRUE(item->Create(itemGuid, itemId, bot));
        item->SetCount(quantity);
        uint16 const position = (INVENTORY_SLOT_BAG_0 << 8) | (INVENTORY_SLOT_ITEM_START + slotOffset);
        EXPECT_EQ(bot->StoreItem({ItemPosCount(position, quantity)}, item, false), item);
        return item;
    }

    void EnsureItemTemplate(uint32 itemId)
    {
        auto& templates = *const_cast<ItemTemplateContainer*>(sObjectMgr->GetItemTemplateStore());
        auto& fastTemplates = *const_cast<std::vector<ItemTemplate*>*>(sObjectMgr->GetItemTemplateStoreFast());
        if (!templates.contains(itemId))
        {
            ItemTemplate item{};
            item.ItemId = itemId;
            item.Class = ITEM_CLASS_TRADE_GOODS;
            item.AllowableClass = -1;
            item.AllowableRace = -1;
            item.Stackable = 20;
            RegisterItemTemplate(templates, fastTemplates, std::move(item));
            addedItemIds.push_back(itemId);
            return;
        }
        if (!originalItemPointers.contains(itemId))
        {
            ItemTemplate* const originalPointer = itemId < fastTemplates.size() ? fastTemplates[itemId] : nullptr;
            originalItemPointers.emplace(itemId, originalPointer);
        }
        if (fastTemplates.size() <= itemId)
            fastTemplates.resize(itemId + 1u);
        fastTemplates[itemId] = &templates.at(itemId);
    }

    uint32 materialItemId = 0u;
    std::size_t originalFastTemplateCount = 0u;
    std::map<uint32, ItemTemplate*> originalItemPointers;
    std::vector<uint32> addedItemIds;
};

TEST_F(PlayerbotProfessionInteractionTest, EconomyCycleIsInstalledForConcretePlayerClassBeforeRandomBotClassification)
{
    TestPlayer* bot = CreateTestPlayer(1, "EconomyStrategyBot");
    bot->SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
    PlayerbotAI botAI(bot);

    std::vector<std::string> const strategies = botAI.GetStrategies(BOT_STATE_NON_COMBAT);
    EXPECT_NE(std::find(strategies.begin(), strategies.end(), "playerbots economy"), strategies.end());
}

TEST_F(PlayerbotProfessionInteractionTest, GatheringDestinationDelegatesLongRangePathingToTravelTarget)
{
    if (!sSkillLineStore.LookupEntry(SKILL_HERBALISM))
    {
        auto* skill = new SkillLineEntry{};
        skill->id = SKILL_HERBALISM;
        sSkillLineStore.SetEntry(SKILL_HERBALISM, skill);
    }
    TestPlayer* bot = CreateTestPlayer(2, "GatheringTraveler");
    bot->SetSkill(SKILL_HERBALISM, 1u, 75u, 75u);
    GatheringTravelDestination destination(GatheringTravelSource::HerbalismNode, 1'618u, SKILL_HERBALISM, 1u, 0u, 0u,
                                           {WorldPosition(0u, 1'000.0f, 1'000.0f, 0.0f, 0.0f)});

    EXPECT_EQ(destination.GetBlocker(bot), GatheringDestinationBlocker::None);
}

TEST_F(PlayerbotProfessionInteractionTest, GatheringDestinationSearchesEverySameMapPointOnce)
{
    GatheringTravelDestination destination(
        GatheringTravelSource::HerbalismNode, 1'618u, SKILL_HERBALISM, 1u, 0u, 0u,
        {WorldPosition(0u, 100.0f, 0.0f, 0.0f, 0.0f), WorldPosition(0u, 10.0f, 0.0f, 0.0f, 0.0f),
         WorldPosition(1u, 1.0f, 0.0f, 0.0f, 0.0f)});
    WorldPosition origin(0u, 0.0f, 0.0f, 0.0f, 0.0f);

    WorldPosition* const nearest = destination.NextUnvisitedPoint(origin, 0u, {});
    ASSERT_NE(nearest, nullptr);
    EXPECT_FLOAT_EQ(nearest->distance(&origin), 10.0f);

    WorldPosition* const remaining = destination.NextUnvisitedPoint(origin, 0u, {nearest});
    ASSERT_NE(remaining, nullptr);
    EXPECT_FLOAT_EQ(remaining->distance(&origin), 100.0f);

    EXPECT_EQ(destination.NextUnvisitedPoint(origin, 0u, {nearest, remaining}), nullptr);
}

TEST_F(PlayerbotProfessionInteractionTest, GatheringPointDestinationDoesNotArriveAtAPreviouslyVisitedPoint)
{
    GatheringTravelDestination destination(
        GatheringTravelSource::HerbalismNode, 1'618u, SKILL_HERBALISM, 1u, 0u, 0u,
        {WorldPosition(0u, 0.0f, 0.0f, 0.0f, 0.0f), WorldPosition(0u, 100.0f, 0.0f, 0.0f, 0.0f)});
    WorldPosition origin(0u, 0.0f, 0.0f, 0.0f, 0.0f);
    WorldPosition selectedPosition(0u, 100.0f, 0.0f, 0.0f, 0.0f);
    WorldPosition* const selectedPoint = destination.NextUnvisitedPoint(origin, 0u, {});
    ASSERT_NE(selectedPoint, nullptr);
    ASSERT_FLOAT_EQ(selectedPoint->distance(&origin), 0.0f);

    WorldPosition* const nextPoint = destination.NextUnvisitedPoint(origin, 0u, {selectedPoint});
    ASSERT_NE(nextPoint, nullptr);
    EXPECT_EQ(destination.MakePointDestination(nullptr), nullptr);
    EXPECT_EQ(destination.MakePointDestination(&selectedPosition), nullptr);
    std::unique_ptr<TravelDestination> pointDestination = destination.MakePointDestination(nextPoint);
    ASSERT_NE(pointDestination, nullptr);

    EXPECT_FALSE(pointDestination->isIn(&origin, INTERACTION_DISTANCE));
    EXPECT_TRUE(pointDestination->isIn(&selectedPosition, INTERACTION_DISTANCE));
}

TEST_F(PlayerbotProfessionInteractionTest, RegisteredGatheringActionRecordsOnlyObservedLootForEveryProfession)
{
    using namespace PlayerbotEconomy;

    std::array<GatheringProfession, 3> const professions = {
        GatheringProfession::Herbalism,
        GatheringProfession::Mining,
        GatheringProfession::Skinning,
    };
    EconomyTraceSnapshot const traceBefore = GetPlayerbotEconomyTrace().Snapshot();
    uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());

    for (std::size_t index = 0; index < professions.size(); ++index)
    {
        uint32 const actorGuid = 70u + static_cast<uint32>(index);
        TestPlayer* bot = CreateTestPlayer(actorGuid, "GatheringBot");
        PlayerbotAI botAI(bot);

        SharedNamedObjectContextList<::Action> sharedContexts;
        GetPlayerbotExtensionRegistry().ForEach([&sharedContexts](PlayerbotExtension& extension)
                                                { extension.AddActionContexts(sharedContexts); });
        std::unique_ptr<::Action> action(sharedContexts.creators.at("add gathering loot")(&botAI));
        ASSERT_NE(dynamic_cast<EconomyGatheringLootAction*>(action.get()), nullptr);

        GatheringClaimResult const claimed =
            GetPlayerbotEconomyGathering().ClaimNearby(GatheringResourceFor(professions[index], 10'000u + index),
                                                       GatheringCandidateFor(actorGuid, professions[index]), now, 30u);
        ASSERT_TRUE(claimed.claim.has_value());
        ASSERT_TRUE(GetPlayerbotEconomyGathering().Observe(*claimed.claim, {}));

        EconomyTraceSnapshot const beforeNoDelta = GetPlayerbotEconomyTrace().Snapshot();
        GetPlayerbotExtensionRegistry().HandleBotEvent(&botAI,
                                                       {PlayerbotEventType::Loot, materialItemId, "gathered material"});
        EXPECT_EQ(GetPlayerbotEconomyTrace().Snapshot().totalCount, beforeNoDelta.totalCount);

        StoreItem(bot, materialItemId, 2u, 80'000u + static_cast<uint32>(index));
        ASSERT_EQ(bot->GetItemCount(materialItemId), 2u);
        bot->m_Events.Update(std::max(1u, sPlayerbotAIConfig.lootDelay));

        EconomyTraceSnapshot const afterDelta = GetPlayerbotEconomyTrace().Snapshot();
        EXPECT_EQ(afterDelta.totalCount, beforeNoDelta.totalCount + 1u);
        auto const gathered = std::find_if(afterDelta.events.begin(), afterDelta.events.end(),
                                           [actorGuid, this](EconomyTraceEvent const& event) {
                                               return event.kind == EconomyTraceKind::Gathered &&
                                                      event.actorGuid == actorGuid && event.itemId == materialItemId;
                                           });
        ASSERT_NE(gathered, afterDelta.events.end());
        EXPECT_EQ(gathered->quantity, 2u);
        EXPECT_TRUE(gathered->chainPublicId.empty());
    }

    EXPECT_EQ(GetPlayerbotEconomyTrace().Snapshot().totalCount, traceBefore.totalCount + professions.size());
}

TEST_F(PlayerbotProfessionInteractionTest, NativeProfessionOracleRequiresAuthoritativeSkillAdvanceForExactScenarios)
{
    using namespace PlayerbotCareer;
    using namespace PlayerbotEconomy;

    struct Scenario
    {
        uint16 skillId;
        uint32 spellId;
        uint32 outputItemId;
        std::vector<ProfessionProgressionReagent> reagents;
    };
    std::array<Scenario, 3> const scenarios = {
        Scenario{SKILL_COOKING, 37836u, 30816u, {{30817u, 1u, 0u, true}, {2678u, 1u, 0u, true}}},
        Scenario{SKILL_FIRST_AID, 3275u, 1251u, {{2589u, 1u, 1u, false}}},
        Scenario{SKILL_TAILORING, 2963u, 2996u, {{2589u, 2u, 2u, false}}},
    };
    for (Scenario const& scenario : scenarios)
    {
        SCOPED_TRACE(scenario.spellId);
        TestPlayer* bot = CreateTestPlayer(scenario.spellId, "ProfessionProgressionBot");
        if (!sSkillLineStore.LookupEntry(scenario.skillId))
        {
            auto* skill = new SkillLineEntry{};
            skill->id = scenario.skillId;
            sSkillLineStore.SetEntry(scenario.skillId, skill);
        }

        EnsureItemTemplate(scenario.outputItemId);
        for (ProfessionProgressionReagent const& reagent : scenario.reagents)
            EnsureItemTemplate(reagent.itemId);
        bot->SetSkill(scenario.skillId, 1u, 1u, 75u);
        ASSERT_EQ(bot->GetPureSkillValue(scenario.skillId), 1u);
        for (std::size_t index = 0; index < scenario.reagents.size(); ++index)
        {
            ProfessionProgressionReagent const& reagent = scenario.reagents[index];
            if (reagent.ownedCount)
                StoreItem(bot, reagent.itemId, reagent.ownedCount, scenario.spellId + 100u + index,
                          static_cast<uint8>(index));
        }

        ProfessionProgressionRecipe recipe{
            .professionSkillId = scenario.skillId,
            .spellId = scenario.spellId,
            .outputItemId = scenario.outputItemId,
            .known = true,
            .advancesSkill = true,
            .reagents = scenario.reagents,
        };
        EconomyCoordinatorSnapshot const coordinatorBefore = GetPlayerbotEconomyCoordinator().Snapshot(1u);
        EconomyMarketSnapshot const marketBefore = GetPlayerbotEconomyMarket().Snapshot(1u);
        ASSERT_TRUE(coordinatorBefore.gaps.empty());
        ASSERT_TRUE(coordinatorBefore.claims.empty());
        ASSERT_TRUE(marketBefore.positions.empty());
        ProfessionProgressionCycleInput cycle{
            .professions =
                {
                    {scenario.skillId, bot->GetPureSkillValue(scenario.skillId), 2u, 100u, true, true, false, false},
                },
            .recipes = {recipe},
            .observation = {.currentSkill = bot->GetPureSkillValue(scenario.skillId)},
        };
        ProfessionProgressionCycleDecision decision = DecideProfessionProgressionCycle(cycle);
        if (scenario.skillId == SKILL_COOKING)
        {
            std::array<uint32, 2> const vendorItems = {30817u, 2678u};
            for (uint32 vendorItemId : vendorItems)
            {
                ASSERT_EQ(decision.action, ProfessionProgressionCycleAction::BuyVendorInput);
                EXPECT_EQ(decision.itemId, vendorItemId);
                cycle.milestone = decision.milestone;
                cycle.batchRemaining = decision.batchRemaining;
                auto const reagent =
                    std::find_if(cycle.recipes.front().reagents.begin(), cycle.recipes.front().reagents.end(),
                                 [vendorItemId](auto const& value) { return value.itemId == vendorItemId; });
                ASSERT_NE(reagent, cycle.recipes.front().reagents.end());
                uint8 const reagentSlot = static_cast<uint8>(reagent - cycle.recipes.front().reagents.begin());
                ProfessionProgressionGameplayExecution const vendorExecution = ExecuteProfessionProgressionGameplay(
                    decision,
                    {.buyVendorInput = [this, bot, reagent, reagentSlot, &scenario](uint32 itemId, uint32 recipeSpellId)
                     {
                         EXPECT_EQ(recipeSpellId, scenario.spellId);
                         StoreItem(bot, itemId, reagent->count, scenario.spellId + 300u + itemId, reagentSlot);
                         return true;
                     }});
                EXPECT_TRUE(vendorExecution.attempted);
                EXPECT_TRUE(vendorExecution.succeeded);
                reagent->ownedCount = reagent->count;
                decision = DecideProfessionProgressionCycle(cycle);
            }
        }
        ASSERT_EQ(decision.action, ProfessionProgressionCycleAction::Craft);
        ASSERT_TRUE(decision.milestone.has_value());
        EXPECT_EQ(decision.milestone->recipeSpellId, scenario.spellId);
        EXPECT_EQ(decision.milestone->outputItemId, scenario.outputItemId);
        EXPECT_EQ(bot->GetPureSkillValue(scenario.skillId), 1u);
        EconomyCoordinatorSnapshot const afterAdmission = GetPlayerbotEconomyCoordinator().Snapshot(1u);
        EXPECT_TRUE(afterAdmission.gaps.empty());
        EXPECT_TRUE(afterAdmission.claims.empty());
        EXPECT_TRUE(GetPlayerbotEconomyMarket().Snapshot(1u).positions.empty());

        uint16 const startingSkill = bot->GetPureSkillValue(scenario.skillId);
        uint32 const startingOutput = bot->GetItemCount(scenario.outputItemId);
        ProfessionProgressionGameplayExecution const craftExecution = ExecuteProfessionProgressionGameplay(
            decision, {.craft = [this, bot, &scenario](uint32 recipeSpellId, uint32 outputItemId)
                       {
                           EXPECT_EQ(recipeSpellId, scenario.spellId);
                           EXPECT_EQ(outputItemId, scenario.outputItemId);
                           StoreItem(bot, outputItemId, 1u, scenario.spellId + 200u, 3u);
                           return true;
                       }});
        EXPECT_TRUE(craftExecution.attempted);
        EXPECT_TRUE(craftExecution.succeeded);
        cycle.milestone = decision.milestone;
        cycle.batchRemaining = decision.batchRemaining;
        cycle.observation.currentSkill = bot->GetPureSkillValue(scenario.skillId);
        cycle.attempt = ProfessionProgressionAttemptObservation{
            .startingSkill = startingSkill,
            .currentSkill = bot->GetPureSkillValue(scenario.skillId),
            .startingOutputQuantity = startingOutput,
            .currentOutputQuantity = bot->GetItemCount(scenario.outputItemId),
            .elapsedSeconds = 61u,
        };
        decision = DecideProfessionProgressionCycle(cycle);
        EXPECT_EQ(decision.action, ProfessionProgressionCycleAction::ObservationBlocked);
        EXPECT_TRUE(decision.outputObserved);
        EXPECT_TRUE(decision.retainAttempt);

        bot->SetSkill(scenario.skillId, 1u, 2u, 75u);
        ASSERT_EQ(bot->GetPureSkillValue(scenario.skillId), 2u);
        cycle.observation.currentSkill = bot->GetPureSkillValue(scenario.skillId);
        cycle.attempt->currentSkill = bot->GetPureSkillValue(scenario.skillId);
        cycle.attempt->elapsedSeconds = 62u;
        decision = DecideProfessionProgressionCycle(cycle);
        EXPECT_EQ(decision.action, ProfessionProgressionCycleAction::Complete);
        EXPECT_FALSE(decision.milestone.has_value());
        EXPECT_FALSE(decision.retainAttempt);
        EconomyCoordinatorSnapshot const afterCompletion = GetPlayerbotEconomyCoordinator().Snapshot(2u);
        EXPECT_TRUE(afterCompletion.gaps.empty());
        EXPECT_TRUE(afterCompletion.claims.empty());
        EXPECT_TRUE(GetPlayerbotEconomyMarket().Snapshot(2u).positions.empty());
    }
}

TEST_F(PlayerbotProfessionInteractionTest, BotRemovalReleasesObservedGatheringWithoutRecordingLoot)
{
    using namespace PlayerbotEconomy;

    TestPlayer* bot = CreateTestPlayer(79u, "RemovedGatheringBot");
    PlayerbotAI botAI(bot);
    uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());
    GatheringClaimResult const claimed = GetPlayerbotEconomyGathering().ClaimNearby(
        GatheringResourceFor(GatheringProfession::Mining, 10'100u),
        GatheringCandidateFor(bot->GetGUID().GetCounter(), GatheringProfession::Mining), now, 30u);
    ASSERT_TRUE(claimed.claim.has_value());
    ASSERT_TRUE(GetPlayerbotEconomyGathering().Observe(*claimed.claim, {}));

    EconomyTraceSnapshot const traceBefore = GetPlayerbotEconomyTrace().Snapshot();
    GetPlayerbotExtensionRegistry().OnBotRemoved(&botAI);
    StoreItem(bot, materialItemId, 1u, 80'100u);
    GetPlayerbotExtensionRegistry().HandleBotEvent(
        &botAI, {PlayerbotEventType::Loot, materialItemId, "late gathered material"});

    EXPECT_EQ(GetPlayerbotEconomyTrace().Snapshot().totalCount, traceBefore.totalCount);
    GatheringClaimSnapshot const gathering = GetPlayerbotEconomyGathering().Snapshot(now + 1u);
    auto const released =
        std::find_if(gathering.claims.begin(), gathering.claims.end(),
                     [&claimed](GatheringClaim const& claim) { return claim.leaseId == claimed.claim->leaseId; });
    ASSERT_NE(released, gathering.claims.end());
    EXPECT_EQ(released->state, GatheringClaimState::Released);
    EXPECT_EQ(released->releaseCause, GatheringReleaseCause::Disabled);
}

TEST_F(PlayerbotProfessionInteractionTest, LegacyRpgHelperSelectsItsInteractionTarget)
{
    TestPlayer* bot = CreateTestPlayer(1, "ProfessionBot");
    TestCreature* trainer = CreateTestCreature(2, 100, TEST_FACTION_HOSTILE_TO_MONSTERS);
    PlayerbotAI botAI(bot);
    botAI.GetAiObjectContext()->GetValue<GuidPosition>("rpg target")->Set(GuidPosition(trainer));

    RpgHelper helper(&botAI);
    helper.BeforeExecute();

    EXPECT_EQ(bot->GetTarget(), trainer->GetGUID());
}

TEST_F(PlayerbotProfessionInteractionTest, DescribeRecipeAllowsExpansionRecipesSupportedByWrath)
{
    TestPlayer* bot = CreateTestPlayer(3, "RecipeBot");

    ItemTemplate classicRecipe{};
    classicRecipe.ItemId = 3609u;
    classicRecipe.Class = ITEM_CLASS_RECIPE;
    classicRecipe.SubClass = ITEM_SUBCLASS_BLACKSMITHING;
    classicRecipe.Spells[0].SpellId = 483;
    classicRecipe.Spells[1].SpellId = 3321;
    classicRecipe.Spells[1].SpellTrigger = ITEM_SPELLTRIGGER_LEARN_SPELL_ID;

    ItemTemplate burningCrusadeRecipe = classicRecipe;
    burningCrusadeRecipe.ItemId = 33792u;
    burningCrusadeRecipe.Spells[1].SpellId = 43549;

    PlayerbotRecipeCandidate classic = PlayerbotCareer::DescribeRecipe(&classicRecipe, bot, 100u);
    PlayerbotRecipeCandidate burningCrusade = PlayerbotCareer::DescribeRecipe(&burningCrusadeRecipe, bot, 100u);

    EXPECT_EQ(classic.itemId, classicRecipe.ItemId);
    EXPECT_EQ(classic.skillId, SKILL_BLACKSMITHING);
    EXPECT_EQ(classic.recipeSpellId, 3321u);
    EXPECT_EQ(burningCrusade.itemId, burningCrusadeRecipe.ItemId);
    EXPECT_EQ(burningCrusade.skillId, SKILL_BLACKSMITHING);
    EXPECT_EQ(burningCrusade.recipeSpellId, 43549u);

    classic.isUsable = true;
    classic.canRaiseSkill = true;
    burningCrusade.isUsable = true;
    burningCrusade.canRaiseSkill = true;
    PlayerbotCareerPlan const plan = {
        .primarySkills = {SKILL_BLACKSMITHING},
        .spendingStyle = PlayerbotRecipeSpendingStyle::Completionist,
    };
    for (PlayerbotRecipeSource source : {
             PlayerbotRecipeSource::Vendor,
             PlayerbotRecipeSource::Drop,
             PlayerbotRecipeSource::OwnedItem,
             PlayerbotRecipeSource::AuctionHouse,
         })
    {
        EXPECT_TRUE(PlayerbotCareer::IsRecipeAcquisitionAllowed(plan, classic, source));
        EXPECT_TRUE(PlayerbotCareer::IsRecipeAcquisitionAllowed(plan, burningCrusade, source));
    }
}

TEST(PlayerbotCareerPlanTest, RecipeAcquisitionFollowsCareerStyleAndSource)
{
    PlayerbotCareerCandidate const candidate =
        PlayerbotCareer::BuildCandidates(Profile(90u, 10u), {CraftingSeed(101u)}, 2u).back();
    std::vector<PlayerbotRecipeCandidate> const recipes = {{2001u, 101u, 100u, true, false, true},
                                                           {2002u, 101u, 50u, true, false, true},
                                                           {2003u, 101u, 10u, false, false, true},
                                                           {2004u, 202u, 1u, true, false, true},
                                                           {2005u, 101u, 1u, true, true, true}};

    PlayerbotCareerPlan minimal = PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Minimal);
    EXPECT_EQ(PlayerbotCareer::SelectRecipePurchases(minimal, recipes, PlayerbotRecipeSource::Vendor),
              std::vector<uint32>({2002u}));
    EXPECT_TRUE(PlayerbotCareer::IsRecipeAcquisitionAllowed(minimal, recipes[0], PlayerbotRecipeSource::Drop));
    EXPECT_FALSE(PlayerbotCareer::IsRecipeAcquisitionAllowed(minimal, recipes[2], PlayerbotRecipeSource::Drop));
    EXPECT_TRUE(PlayerbotCareer::IsRecipeAcquisitionAllowed(minimal, recipes[0], PlayerbotRecipeSource::AuctionHouse));
    EXPECT_FALSE(PlayerbotCareer::IsRecipeAcquisitionAllowed(minimal, recipes[2], PlayerbotRecipeSource::AuctionHouse));
    EXPECT_FALSE(PlayerbotCareer::IsRecipeAcquisitionAllowed(minimal, recipes[3], PlayerbotRecipeSource::AuctionHouse));
    EXPECT_FALSE(PlayerbotCareer::IsRecipeAcquisitionAllowed(minimal, recipes[4], PlayerbotRecipeSource::AuctionHouse));

    PlayerbotCareerPlan completionist =
        PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Completionist);
    EXPECT_EQ(PlayerbotCareer::SelectRecipePurchases(completionist, recipes, PlayerbotRecipeSource::Vendor),
              std::vector<uint32>({2001u, 2002u, 2003u}));
    EXPECT_TRUE(
        PlayerbotCareer::IsRecipeAcquisitionAllowed(completionist, recipes[2], PlayerbotRecipeSource::AuctionHouse));
    EXPECT_FALSE(PlayerbotCareer::IsRecipeAcquisitionAllowed(completionist, recipes[3], PlayerbotRecipeSource::Vendor));
    EXPECT_FALSE(PlayerbotCareer::IsRecipeAcquisitionAllowed(completionist, recipes[4], PlayerbotRecipeSource::Vendor));
}

TEST(PlayerbotCareerPlanTest, RecipeCapabilityGoalSelectsOnlyItsExactRecipeAndOutput)
{
    PlayerbotCareerCandidate const candidate =
        PlayerbotCareer::BuildCandidates(Profile(90u, 10u), {CraftingSeed(101u)}, 2u).back();
    PlayerbotCareerPlan plan = PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::Minimal);
    ASSERT_TRUE(PlayerbotCareer::TryAssignCapabilityGoal(
        plan, {PlayerbotCareerCapabilityGoalKind::Recipe, 101u, 9001u, 8001u}, {101u}));
    std::vector<PlayerbotRecipeCandidate> const recipes = {{2001u, 101u, 1u, true, false, true, 9001u, 8001u},
                                                           {2002u, 101u, 1u, true, false, true, 9002u, 8001u},
                                                           {2003u, 101u, 1u, true, false, true, 9001u, 8002u},
                                                           {2004u, 202u, 1u, true, false, true, 9001u, 8001u}};

    EXPECT_EQ(PlayerbotCareer::SelectRecipePurchases(plan, recipes, PlayerbotRecipeSource::Vendor),
              std::vector<uint32>({2001u}));
}
