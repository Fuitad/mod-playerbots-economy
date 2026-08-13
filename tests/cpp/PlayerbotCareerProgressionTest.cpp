/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <array>
#include <utility>

#include "Bot/Economy/PlayerbotEconomyPolicy.h"
#include "Bot/Personality/PlayerbotCareerProgression.h"
#include "SharedDefines.h"
#include "gtest/gtest.h"

using namespace PlayerbotCareer;

namespace
{
constexpr uint32 MAX_PRESSURE = PROFESSION_PROGRESSION_MAXIMUM_PRESSURE;
constexpr uint32 MAX_BATCH = PROFESSION_PROGRESSION_MAXIMUM_BATCH;

ProfessionProgressionRecipe Recipe(uint16 skillId, uint32 spellId, uint32 outputItemId, bool advances,
                                   std::vector<ProfessionProgressionReagent> reagents)
{
    return {
        .professionSkillId = skillId,
        .spellId = spellId,
        .outputItemId = outputItemId,
        .known = true,
        .advancesSkill = advances,
        .reagents = std::move(reagents),
    };
}

ProfessionProgressionState State(uint16 skillId, uint16 currentSkill, uint16 targetSkill, uint8 affinity,
                                 bool trainerRankRequired = false, bool trainerRecipeRequired = false)
{
    return {
        .professionSkillId = skillId,
        .currentSkill = currentSkill,
        .targetSkill = targetSkill,
        .affinity = affinity,
        .planned = true,
        .learned = true,
        .trainerRankRequired = trainerRankRequired,
        .trainerRecipeRequired = trainerRecipeRequired,
    };
}
}  // namespace

TEST(PlayerbotCareerProgressionTest, SelectsTheNextTrainerMilestoneWhenKnownRecipesCannotAdvance)
{
    std::vector<ProfessionProgressionRecipe> const noRecipes;

    std::optional<ProfessionProgressionMilestone> const rank =
        SelectProgressionMilestone({State(SKILL_TAILORING, 75u, 76u, 80u, true, false)}, noRecipes, MAX_PRESSURE);
    ASSERT_TRUE(rank.has_value());
    EXPECT_EQ(rank->kind, ProfessionProgressionMilestoneKind::TrainerRank);
    EXPECT_EQ(rank->professionSkillId, SKILL_TAILORING);
    ProfessionProgressionCycleDecision const rankDecision{
        .action = ProfessionProgressionCycleAction::TrainerRank,
        .milestone = rank,
    };
    uint32 trainerCommands = 0u;
    ProfessionProgressionGameplayExecution const rankExecution = ExecuteProfessionProgressionGameplay(
        rankDecision, {.scheduleTrainer = [&trainerCommands](ProfessionProgressionMilestone const&)
                       {
                           ++trainerCommands;
                           return true;
                       }});
    EXPECT_TRUE(rankExecution.succeeded);
    EXPECT_EQ(ReconcileProgressionExecution(*rank, {}, {.currentSkill = 75u, .maximumSkill = 150u}).state,
              ProfessionProgressionExecutionState::Complete);

    std::optional<ProfessionProgressionMilestone> const recipe =
        SelectProgressionMilestone({State(SKILL_TAILORING, 50u, 75u, 80u, false, true)}, noRecipes, MAX_PRESSURE);
    ASSERT_TRUE(recipe.has_value());
    EXPECT_EQ(recipe->kind, ProfessionProgressionMilestoneKind::TrainerRecipe);
    EXPECT_EQ(recipe->professionSkillId, SKILL_TAILORING);
    ProfessionProgressionCycleDecision const recipeDecision{
        .action = ProfessionProgressionCycleAction::TrainerRecipe,
        .milestone = recipe,
    };
    ProfessionProgressionGameplayExecution const recipeExecution = ExecuteProfessionProgressionGameplay(
        recipeDecision, {.scheduleTrainer = [&trainerCommands](ProfessionProgressionMilestone const&)
                         {
                             ++trainerCommands;
                             return true;
                         }});
    EXPECT_TRUE(recipeExecution.succeeded);
    EXPECT_EQ(trainerCommands, 2u);
    EXPECT_EQ(ReconcileProgressionExecution(*recipe, {}, {.currentSkill = 50u, .knownRecipe = true}).state,
              ProfessionProgressionExecutionState::Complete);
}

TEST(PlayerbotCareerProgressionTest, PressureIsMonotonicInAffinityAndRealLagAndBounded)
{
    EXPECT_EQ(ProgressionPressure(State(SKILL_COOKING, 75u, 75u, 100u), MAX_PRESSURE), 0u);

    uint32 const lowAffinity = ProgressionPressure(State(SKILL_COOKING, 25u, 75u, 40u), MAX_PRESSURE);
    uint32 const highAffinity = ProgressionPressure(State(SKILL_COOKING, 25u, 75u, 90u), MAX_PRESSURE);
    uint32 const highLag = ProgressionPressure(State(SKILL_COOKING, 1u, 75u, 90u), MAX_PRESSURE);

    EXPECT_GT(lowAffinity, 0u);
    EXPECT_GT(highAffinity, lowAffinity);
    EXPECT_GT(highLag, highAffinity);
    EXPECT_LE(highLag, MAX_PRESSURE);
    EXPECT_EQ(ProgressionSchedulingEngagement(25u, 0u, MAX_PRESSURE), 25u);
    EXPECT_EQ(ProgressionSchedulingEngagement(25u, MAX_PRESSURE, MAX_PRESSURE), 100u);
    EXPECT_LE(ProgressionSchedulingEngagement(100u, MAX_PRESSURE, MAX_PRESSURE), 100u);
    EXPECT_LE(ProgressionBatchCeiling(100u, 74u, MAX_BATCH), MAX_BATCH);
}

TEST(PlayerbotCareerProgressionTest, MilestoneSelectsTheStrongestLagAndRejectsGrayRecipes)
{
    std::vector<ProfessionProgressionState> const professions = {
        State(SKILL_COOKING, 1u, 75u, 80u),
        State(SKILL_FIRST_AID, 20u, 75u, 60u),
        State(SKILL_TAILORING, 74u, 75u, 100u),
    };
    std::vector<ProfessionProgressionRecipe> const recipes = {
        Recipe(SKILL_COOKING, 37836u, 30816u, false, {{30817u, 1u, 0u, true}, {2678u, 1u, 0u, true}}),
        Recipe(SKILL_COOKING, 2538u, 2679u, true, {{2672u, 1u, 1u, false}}),
        Recipe(SKILL_FIRST_AID, 3275u, 1251u, true, {{2589u, 1u, 1u, false}}),
        Recipe(SKILL_TAILORING, 2963u, 2996u, true, {{2589u, 2u, 2u, false}}),
    };

    std::optional<ProfessionProgressionMilestone> const selected =
        SelectProgressionMilestone(professions, recipes, MAX_PRESSURE);

    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->professionSkillId, SKILL_COOKING);
    EXPECT_EQ(selected->recipeSpellId, 2538u);
    EXPECT_NE(selected->recipeSpellId, 37836u);
}

TEST(PlayerbotCareerProgressionTest, OwnedInputsPrecedeVendorInputsAndBlockedWorkCreatesNoClaims)
{
    ProfessionProgressionMilestone const cooking = {
        .professionSkillId = SKILL_COOKING,
        .targetSkill = 75u,
        .recipeSpellId = 37836u,
        .outputItemId = 30816u,
    };
    ProfessionProgressionRecipe vendorBacked =
        Recipe(SKILL_COOKING, 37836u, 30816u, true, {{30817u, 1u, 1u, true}, {2678u, 1u, 0u, true}});

    ProfessionProgressionAdmission admission = AdmitProgressionBatch(cooking, vendorBacked, 5u);
    ASSERT_EQ(admission.state, ProfessionProgressionAdmissionState::Ready);
    EXPECT_EQ(admission.batchQuantity, 5u);
    EXPECT_TRUE(admission.usesVendorInputs);

    vendorBacked.reagents[1].ordinaryVendorAvailable = false;
    admission = AdmitProgressionBatch(cooking, vendorBacked, 5u);
    EXPECT_EQ(admission.state, ProfessionProgressionAdmissionState::Waiting);
    EXPECT_EQ(admission.blocker, ProfessionProgressionBlocker::MaterialSourceUnavailable);
    EXPECT_EQ(admission.missingItemId, 2678u);
    EXPECT_EQ(admission.milestone, cooking);
    EXPECT_FALSE(admission.authorizesExecutableDemand);
    EXPECT_FALSE(admission.authorizesMaterialReservation);
    EXPECT_FALSE(admission.authorizesGatheringClaim);
    EXPECT_FALSE(admission.authorizesPurchaseClaim);
    EXPECT_FALSE(admission.authorizesProductionClaim);
    EXPECT_FALSE(admission.authorizesListing);
}

TEST(PlayerbotCareerProgressionTest, EveryHardAuthorityPreemptsAndResumeKeepsOneMilestone)
{
    ProfessionProgressionMilestone const milestone = {
        .professionSkillId = SKILL_FIRST_AID,
        .targetSkill = 75u,
        .recipeSpellId = 3275u,
        .outputItemId = 1251u,
    };
    std::array<ProfessionProgressionAuthority, 5> authorities = {
        ProfessionProgressionAuthority{.combat = true},
        ProfessionProgressionAuthority{.survival = true},
        ProfessionProgressionAuthority{.transport = true},
        ProfessionProgressionAuthority{.directObjective = true},
        ProfessionProgressionAuthority{.groupCommitment = true},
    };

    for (ProfessionProgressionAuthority const& authority : authorities)
    {
        SCOPED_TRACE(static_cast<uint32>(authority.Blocker()));
        ProfessionProgressionCycleInput cycle{
            .authority = authority,
            .observation = {.currentSkill = 10u},
            .milestone = milestone,
            .batchRemaining = 1u,
            .attempt =
                ProfessionProgressionAttemptObservation{
                    .startingSkill = 10u,
                    .currentSkill = 10u,
                    .elapsedSeconds = 1u,
                },
        };
        ProfessionProgressionCycleDecision const paused = DecideProfessionProgressionCycle(cycle);
        EXPECT_EQ(paused.action, ProfessionProgressionCycleAction::Preempted);
        ASSERT_TRUE(paused.milestone.has_value());
        EXPECT_EQ(*paused.milestone, milestone);
        EXPECT_EQ(paused.batchRemaining, 1u);
        EXPECT_TRUE(paused.retainAttempt);

        cycle.authority = {};
        ProfessionProgressionCycleDecision const resumed = DecideProfessionProgressionCycle(cycle);
        EXPECT_EQ(resumed.action, ProfessionProgressionCycleAction::WaitObservation);
        ASSERT_TRUE(resumed.milestone.has_value());
        EXPECT_EQ(*resumed.milestone, milestone);
        EXPECT_EQ(resumed.batchRemaining, 1u);
        EXPECT_TRUE(resumed.retainAttempt);
        uint32 duplicateCrafts = 0u;
        ProfessionProgressionGameplayExecution const resumedExecution =
            ExecuteProfessionProgressionGameplay(resumed, {.craft = [&duplicateCrafts](uint32, uint32)
                                                           {
                                                               ++duplicateCrafts;
                                                               return true;
                                                           }});
        EXPECT_FALSE(resumedExecution.attempted);
        EXPECT_EQ(duplicateCrafts, 0u);
    }
}

TEST(PlayerbotCareerProgressionTest, RepresentativeProfessionsCompleteOnlyFromAuthoritativeObservations)
{
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
        ProfessionProgressionRecipe const recipe =
            Recipe(scenario.skillId, scenario.spellId, scenario.outputItemId, true, scenario.reagents);
        std::optional<ProfessionProgressionMilestone> const selected =
            SelectProgressionMilestone({State(scenario.skillId, 1u, 2u, 80u)}, {recipe}, MAX_PRESSURE);
        ASSERT_TRUE(selected.has_value());
        ProfessionProgressionMilestone const milestone = *selected;
        ProfessionProgressionAdmission const admitted = AdmitProgressionBatch(milestone, recipe, MAX_BATCH);
        ASSERT_EQ(admitted.state, ProfessionProgressionAdmissionState::Ready);
        EXPECT_FALSE(admitted.authorizesExecutableDemand);
        EXPECT_FALSE(admitted.authorizesMaterialReservation);
        EXPECT_FALSE(admitted.authorizesGatheringClaim);
        EXPECT_FALSE(admitted.authorizesPurchaseClaim);
        EXPECT_FALSE(admitted.authorizesProductionClaim);
        EXPECT_FALSE(admitted.authorizesListing);

        ProfessionProgressionExecution const before =
            ReconcileProgressionExecution(milestone, {}, {.currentSkill = 1u, .knownRecipe = true});
        EXPECT_EQ(before.state, ProfessionProgressionExecutionState::Ready);

        ProfessionProgressionExecution const completed = ReconcileProgressionExecution(
            milestone, {}, {.currentSkill = 2u, .knownRecipe = true, .producedQuantity = 1u});
        EXPECT_EQ(completed.state, ProfessionProgressionExecutionState::Complete);
        EXPECT_EQ(completed.activeBatchCount, 0u);
    }
}

TEST(PlayerbotCareerProgressionTest, CraftAttemptCompletesOnlyAfterAuthoritativeSkillAdvance)
{
    ProfessionProgressionAttemptObservation outputOnly{
        .startingSkill = 1u,
        .currentSkill = 1u,
        .startingOutputQuantity = 0u,
        .currentOutputQuantity = 1u,
        .elapsedSeconds = 1u,
    };

    ProfessionProgressionAttemptReconciliation const pending = ReconcileProgressionAttempt(outputOnly);
    EXPECT_EQ(pending.state, ProfessionProgressionAttemptState::Pending);
    EXPECT_TRUE(pending.outputObserved);
    EXPECT_TRUE(pending.retainAttempt);

    outputOnly.elapsedSeconds = 61u;
    ProfessionProgressionAttemptReconciliation const blocked = ReconcileProgressionAttempt(outputOnly);
    EXPECT_EQ(blocked.state, ProfessionProgressionAttemptState::ObservationBlocked);
    EXPECT_TRUE(blocked.outputObserved);
    EXPECT_TRUE(blocked.retainAttempt);

    outputOnly.currentSkill = 2u;
    ProfessionProgressionAttemptReconciliation const advanced = ReconcileProgressionAttempt(outputOnly);
    EXPECT_EQ(advanced.state, ProfessionProgressionAttemptState::Advanced);
    EXPECT_FALSE(advanced.retainAttempt);
}

TEST(PlayerbotCareerProgressionTest, TimedOutAttemptStaysHeldUntilLateSkillObservationArrives)
{
    ProfessionProgressionAttemptObservation observation{
        .startingSkill = 1u,
        .currentSkill = 1u,
        .elapsedSeconds = 600u,
    };

    ProfessionProgressionAttemptReconciliation const blocked = ReconcileProgressionAttempt(observation);
    ASSERT_EQ(blocked.state, ProfessionProgressionAttemptState::ObservationBlocked);
    EXPECT_TRUE(blocked.retainAttempt);

    observation.currentSkill = 2u;
    ProfessionProgressionAttemptReconciliation const reconciled = ReconcileProgressionAttempt(observation);
    EXPECT_EQ(reconciled.state, ProfessionProgressionAttemptState::Advanced);
    EXPECT_FALSE(reconciled.retainAttempt);
}

TEST(PlayerbotCareerProgressionTest, SpiceBreadTrainingOutputNeedsIndependentResidualDemandToList)
{
    PlayerbotEconomy::AutonomousListingPolicyInput spiceBread{
        .ordinaryVendorSupply = false,
        .trainingOutput = true,
        .independentDemand = false,
    };
    EXPECT_FALSE(PlayerbotEconomy::PlayerbotEconomyPolicy::AllowsAutonomousListing(spiceBread));

    spiceBread.independentDemand = true;
    EXPECT_TRUE(PlayerbotEconomy::PlayerbotEconomyPolicy::AllowsAutonomousListing(spiceBread));
}
