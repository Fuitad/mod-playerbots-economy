/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <memory>
#include <optional>

#include "Ai/World/Rpg/Action/RpgSubActions.h"
#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "Bot/Engine/AiObjectContext.h"
#include "Bot/Extension/PlayerbotExtension.h"
#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "IntegrationTestFixture.h"
#include "ItemTemplate.h"
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

    // A career token with skills but a `none` spending style buys nothing, so it schedules nothing.
    PlayerbotCareerPlan const disabled = PlayerbotCareer::MakePlan(42u, candidate, PlayerbotRecipeSpendingStyle::None);
    EXPECT_FALSE(PlayerbotCareer::SchedulesProfessionWork(disabled));
    EXPECT_EQ(PlayerbotCareer::ProfessionWorkWeight(disabled, 25u), 0u);
}

TEST(PlayerbotCareerPlanTest, CareerTrainerDestinationsStayInsideTheBotsSafeLevelRange)
{
    EXPECT_TRUE(PlayerbotCareer::IsTrainerDestinationSafe(3u, 14u, 14u, 5u));
    EXPECT_FALSE(PlayerbotCareer::IsTrainerDestinationSafe(3u, 14u, 17u, 10u));
    EXPECT_FALSE(PlayerbotCareer::IsTrainerDestinationSafe(5u, 357u, 357u, 40u));
    EXPECT_TRUE(PlayerbotCareer::IsTrainerDestinationSafe(10u, 14u, 17u, 10u));
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

class PlayerbotProfessionInteractionTest : public IntegrationTestFixture
{
protected:
    void SetUp() override
    {
        IntegrationTestFixture::SetUp();

        static bool contextsBuilt = false;
        if (!contextsBuilt)
        {
            AiObjectContext::BuildAllSharedContexts();
            contextsBuilt = true;
        }
    }
};

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
    classicRecipe.Spells[1].SpellId = 2329;

    ItemTemplate burningCrusadeRecipe = classicRecipe;
    burningCrusadeRecipe.ItemId = 33792u;
    burningCrusadeRecipe.Spells[1].SpellId = 43549;

    PlayerbotRecipeCandidate classic = PlayerbotCareer::DescribeRecipe(&classicRecipe, bot, 100u);
    PlayerbotRecipeCandidate burningCrusade = PlayerbotCareer::DescribeRecipe(&burningCrusadeRecipe, bot, 100u);

    EXPECT_EQ(classic.itemId, classicRecipe.ItemId);
    EXPECT_EQ(classic.skillId, SKILL_BLACKSMITHING);
    EXPECT_EQ(burningCrusade.itemId, burningCrusadeRecipe.ItemId);
    EXPECT_EQ(burningCrusade.skillId, SKILL_BLACKSMITHING);

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
    EXPECT_FALSE(PlayerbotCareer::IsRecipeAcquisitionAllowed(minimal, recipes[0], PlayerbotRecipeSource::AuctionHouse));

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
