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

TEST(PlayerbotCareerProgressionTest, MilestonePrefersARecipeWhoseMissingReagentTheBotCanObtain)
{
    std::vector<ProfessionProgressionState> const professions = {State(SKILL_ALCHEMY, 8u, 75u, 80u)};
    // Earthroot (2449) needs herbalism 15 the bot lacks; Peacebloom (2447) grows at its skill. Nothing is in the bags.
    ProfessionProgressionRecipe const earthroot = Recipe(SKILL_ALCHEMY, 2330u, 118u, true, {{2449u, 1u, 0u, false}});
    ProfessionProgressionRecipe peacebloom = Recipe(SKILL_ALCHEMY, 2331u, 929u, true, {{2447u, 1u, 0u, false}});
    peacebloom.reagents.front().obtainable = true;
    // Two scarce reagents, both obtainable: the material path only sources one, so this is not feedable.
    ProfessionProgressionRecipe twoScarce =
        Recipe(SKILL_ALCHEMY, 2329u, 2454u, true, {{2447u, 1u, 0u, false}, {2450u, 1u, 0u, false}});
    for (ProfessionProgressionReagent& reagent : twoScarce.reagents)
        reagent.obtainable = true;
    std::vector<ProfessionProgressionRecipe> const recipes = {twoScarce, earthroot, peacebloom};

    std::optional<ProfessionProgressionMilestone> const selected =
        SelectProgressionMilestone(professions, recipes, MAX_PRESSURE);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->recipeSpellId, 2331u);

    // A milestone stuck on the unobtainable recipe gives way to the feedable one.
    ProfessionProgressionMilestone const stuck = {
        .professionSkillId = SKILL_ALCHEMY, .targetSkill = 75u, .recipeSpellId = 2330u, .outputItemId = 118u};
    std::optional<ProfessionProgressionMilestone> const replaced =
        SelectProgressionMilestone(professions, recipes, MAX_PRESSURE, stuck);
    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ(replaced->recipeSpellId, 2331u);

    // A recipe already in the bags still beats a feedable one.
    ProfessionProgressionRecipe const inBags = Recipe(SKILL_ALCHEMY, 2332u, 2455u, true, {{2447u, 1u, 1u, false}});
    std::optional<ProfessionProgressionMilestone> const ready =
        SelectProgressionMilestone(professions, {peacebloom, inBags}, MAX_PRESSURE);
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(ready->recipeSpellId, 2332u);
}

TEST(PlayerbotCareerProgressionTest, AProfessionNobodyCanFeedYieldsToOneThatCanProgress)
{
    // Enchanting at rank 1 carries the most lag, but its only recipe needs two drop-only reagents.
    // Tailoring lags less and can be fed (Linen Cloth is listed on the auction house).
    std::vector<ProfessionProgressionState> const professions = {
        State(SKILL_ENCHANTING, 1u, 75u, 80u),
        State(SKILL_TAILORING, 20u, 75u, 80u),
    };
    ProfessionProgressionRecipe const runedRod =
        Recipe(SKILL_ENCHANTING, 7421u, 6218u, true,
               {{6217u, 1u, 0u, true}, {10940u, 1u, 0u, false}, {10938u, 1u, 0u, false}});
    ProfessionProgressionRecipe linenBag = Recipe(SKILL_TAILORING, 3755u, 4238u, true, {{2996u, 3u, 0u, false}});
    linenBag.reagents.front().obtainable = true;

    std::optional<ProfessionProgressionMilestone> const selected =
        SelectProgressionMilestone(professions, {runedRod, linenBag}, MAX_PRESSURE);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->professionSkillId, SKILL_TAILORING);
    EXPECT_EQ(selected->recipeSpellId, 3755u);

    // A milestone already stuck on the rod gives way to tailoring.
    ProfessionProgressionMilestone const stuck = {
        .professionSkillId = SKILL_ENCHANTING, .targetSkill = 75u, .recipeSpellId = 7421u, .outputItemId = 6218u};
    std::optional<ProfessionProgressionMilestone> const replaced =
        SelectProgressionMilestone(professions, {runedRod, linenBag}, MAX_PRESSURE, stuck);
    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ(replaced->professionSkillId, SKILL_TAILORING);

    // With nothing feedable anywhere, pressure still decides as before.
    linenBag.reagents.front().obtainable = false;
    std::optional<ProfessionProgressionMilestone> const fallback =
        SelectProgressionMilestone(professions, {runedRod, linenBag}, MAX_PRESSURE);
    ASSERT_TRUE(fallback.has_value());
    EXPECT_EQ(fallback->professionSkillId, SKILL_ENCHANTING);
}

TEST(PlayerbotCareerProgressionTest, DisenchantableReagentsFeedAnEnchantingRecipeWithoutAMaterialSource)
{
    // Rank 1 enchanting: Runed Copper Rod needs Strange Dust and Lesser Magic Essence, neither of which is
    // gathered or vendored. The bot holds greens it can disenchant into both. That is a feedable recipe even
    // though two reagents are short, because disenchanting needs no travel and no material commitment.
    std::vector<ProfessionProgressionState> const professions = {
        State(SKILL_ENCHANTING, 1u, 75u, 80u),
        State(SKILL_TAILORING, 20u, 75u, 80u),
    };
    ProfessionProgressionRecipe runedRod =
        Recipe(SKILL_ENCHANTING, 7421u, 6218u, true,
               {{6217u, 1u, 1u, true}, {10940u, 1u, 0u, false}, {10938u, 1u, 0u, false}});
    for (ProfessionProgressionReagent& reagent : runedRod.reagents)
        reagent.disenchantable = !reagent.ordinaryVendorAvailable;
    ProfessionProgressionRecipe linenBag = Recipe(SKILL_TAILORING, 3755u, 4238u, true, {{2996u, 3u, 0u, false}});
    linenBag.reagents.front().obtainable = true;

    std::optional<ProfessionProgressionMilestone> const selected =
        SelectProgressionMilestone(professions, {runedRod, linenBag}, MAX_PRESSURE);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->professionSkillId, SKILL_ENCHANTING);
    EXPECT_EQ(selected->recipeSpellId, 7421u);

    // The cycle asks for a disenchant of the first short reagent instead of reporting no material source.
    ProfessionProgressionCycleDecision const decision = DecideProfessionProgressionCycle({
        .professions = professions,
        .recipes = {runedRod, linenBag},
    });
    ASSERT_TRUE(decision.milestone.has_value());
    EXPECT_EQ(decision.milestone->recipeSpellId, 7421u);
    EXPECT_EQ(decision.action, ProfessionProgressionCycleAction::Disenchant);
    EXPECT_EQ(decision.itemId, 10940u);
    EXPECT_EQ(decision.blocker, ProfessionProgressionBlocker::None);

    std::vector<std::pair<uint32, uint32>> disenchants;
    ProfessionProgressionGameplayExecution const execution = ExecuteProfessionProgressionGameplay(
        decision, {.disenchant = [&disenchants](uint32 itemId, uint32 recipeSpellId)
                   {
                       disenchants.emplace_back(itemId, recipeSpellId);
                       return true;
                   }});
    EXPECT_TRUE(execution.attempted);
    EXPECT_TRUE(execution.succeeded);
    ASSERT_EQ(disenchants.size(), 1u);
    EXPECT_EQ(disenchants.front(), std::make_pair(10940u, 7421u));

    // Once the greens are gone the same recipe is blocked again on its material source.
    for (ProfessionProgressionReagent& reagent : runedRod.reagents)
        reagent.disenchantable = false;
    ProfessionProgressionCycleDecision const blocked = DecideProfessionProgressionCycle({
        .professions = {professions.front()},
        .recipes = {runedRod},
    });
    EXPECT_EQ(blocked.action, ProfessionProgressionCycleAction::Blocked);
    EXPECT_EQ(blocked.blocker, ProfessionProgressionBlocker::MaterialSourceUnavailable);
    EXPECT_EQ(blocked.itemId, 10940u);
}

TEST(PlayerbotCareerProgressionTest, MillableReagentsFeedAnInkRecipeWithoutAMaterialSource)
{
    // Rank 1 inscription: Ivory Ink needs Alabaster Pigment, which no node and no vendor offers. The bot
    // holds herbs it can mill into pigment, so the recipe is feedable without travel or a commitment.
    std::vector<ProfessionProgressionState> const professions = {State(SKILL_INSCRIPTION, 1u, 75u, 80u)};
    ProfessionProgressionRecipe ivoryInk = Recipe(SKILL_INSCRIPTION, 52738u, 37101u, true, {{39151u, 1u, 0u, false}});
    ivoryInk.reagents.front().millable = true;

    std::optional<ProfessionProgressionMilestone> const selected =
        SelectProgressionMilestone(professions, {ivoryInk}, MAX_PRESSURE);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->recipeSpellId, 52738u);

    ProfessionProgressionCycleDecision const decision = DecideProfessionProgressionCycle({
        .professions = professions,
        .recipes = {ivoryInk},
    });
    ASSERT_TRUE(decision.milestone.has_value());
    EXPECT_EQ(decision.action, ProfessionProgressionCycleAction::Mill);
    EXPECT_EQ(decision.itemId, 39151u);
    EXPECT_EQ(decision.blocker, ProfessionProgressionBlocker::None);

    std::vector<std::pair<uint32, uint32>> millings;
    ProfessionProgressionGameplayExecution const execution =
        ExecuteProfessionProgressionGameplay(decision, {.mill = [&millings](uint32 itemId, uint32 recipeSpellId)
                                                        {
                                                            millings.emplace_back(itemId, recipeSpellId);
                                                            return true;
                                                        }});
    EXPECT_TRUE(execution.attempted);
    EXPECT_TRUE(execution.succeeded);
    ASSERT_EQ(millings.size(), 1u);
    EXPECT_EQ(millings.front(), std::make_pair(39151u, 52738u));

    // With the herbs gone the recipe is blocked on its material source again.
    ivoryInk.reagents.front().millable = false;
    ProfessionProgressionCycleDecision const blocked = DecideProfessionProgressionCycle({
        .professions = professions,
        .recipes = {ivoryInk},
    });
    EXPECT_EQ(blocked.action, ProfessionProgressionCycleAction::Blocked);
    EXPECT_EQ(blocked.blocker, ProfessionProgressionBlocker::MaterialSourceUnavailable);
    EXPECT_EQ(blocked.itemId, 39151u);
}

TEST(PlayerbotCareerProgressionTest, FeasibleSpiceBreadReplacesInfeasibleLowerSpellRecipe)
{
    ProfessionProgressionMilestone const charredWolfMeat = {
        .professionSkillId = SKILL_COOKING,
        .targetSkill = 75u,
        .recipeSpellId = 2538u,
        .outputItemId = 2679u,
    };
    std::vector<ProfessionProgressionRecipe> const recipes = {
        Recipe(SKILL_COOKING, 2538u, 2679u, true, {{2672u, 1u, 0u, false}}),
        Recipe(SKILL_COOKING, 37836u, 30816u, true, {{30817u, 1u, 0u, true}, {2678u, 1u, 0u, true}}),
    };
    std::array<std::optional<ProfessionProgressionMilestone>, 2> const existingMilestones = {
        std::nullopt,
        charredWolfMeat,
    };

    for (std::optional<ProfessionProgressionMilestone> const& existing : existingMilestones)
    {
        ProfessionProgressionCycleDecision const decision = DecideProfessionProgressionCycle({
            .professions = {State(SKILL_COOKING, 1u, 75u, 100u)},
            .recipes = recipes,
            .milestone = existing,
        });

        ASSERT_TRUE(decision.milestone.has_value());
        EXPECT_EQ(decision.milestone->recipeSpellId, 37836u);
        EXPECT_EQ(decision.milestone->outputItemId, 30816u);
        EXPECT_EQ(decision.action, ProfessionProgressionCycleAction::BuyVendorInput);
        EXPECT_EQ(decision.itemId, 30817u);
        EXPECT_EQ(decision.blocker, ProfessionProgressionBlocker::None);
    }
}

TEST(PlayerbotCareerProgressionTest, NoFeasibleRecipeRetainsExactLowestRecipeMaterialBlocker)
{
    ProfessionProgressionCycleDecision const decision = DecideProfessionProgressionCycle({
        .professions = {State(SKILL_COOKING, 1u, 75u, 100u)},
        .recipes =
            {
                Recipe(SKILL_COOKING, 2538u, 2679u, true, {{2672u, 1u, 0u, false}}),
                Recipe(SKILL_COOKING, 37836u, 30816u, true, {{30817u, 1u, 0u, false}, {2678u, 1u, 0u, true}}),
            },
    });

    ASSERT_TRUE(decision.milestone.has_value());
    EXPECT_EQ(decision.milestone->recipeSpellId, 2538u);
    EXPECT_EQ(decision.action, ProfessionProgressionCycleAction::Blocked);
    EXPECT_EQ(decision.blocker, ProfessionProgressionBlocker::MaterialSourceUnavailable);
    EXPECT_EQ(decision.itemId, 2672u);
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
    EXPECT_FALSE(blocked.retainAttempt);

    outputOnly.currentSkill = 2u;
    ProfessionProgressionAttemptReconciliation const advanced = ReconcileProgressionAttempt(outputOnly);
    EXPECT_EQ(advanced.state, ProfessionProgressionAttemptState::Advanced);
    EXPECT_FALSE(advanced.retainAttempt);
}

TEST(PlayerbotCareerProgressionTest, TimedOutAttemptReleasesExecutionAndPreservesCheckpoint)
{
    ProfessionProgressionAttemptObservation observation{
        .startingSkill = 1u,
        .currentSkill = 1u,
        .elapsedSeconds = 60u,
    };

    ProfessionProgressionAttemptReconciliation const blocked = ReconcileProgressionAttempt(observation);
    ASSERT_EQ(blocked.state, ProfessionProgressionAttemptState::ObservationBlocked);
    EXPECT_FALSE(blocked.retainAttempt);

    ProfessionProgressionMilestone const checkpoint{
        .kind = ProfessionProgressionMilestoneKind::AdvanceSkill,
        .professionSkillId = 185u,
        .targetSkill = 25u,
        .recipeSpellId = 2963u,
        .outputItemId = 2996u,
    };
    ProfessionProgressionCycleDecision const decision = DecideProfessionProgressionCycle({
        .observation = {.currentSkill = 1u},
        .milestone = checkpoint,
        .batchRemaining = 3u,
        .attempt = observation,
    });
    EXPECT_EQ(decision.action, ProfessionProgressionCycleAction::ObservationBlocked);
    EXPECT_FALSE(decision.retainAttempt);
    EXPECT_EQ(decision.milestone, checkpoint);
    EXPECT_EQ(decision.batchRemaining, 3u);
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

TEST(PlayerbotCareerProgressionTest, AGatheringProfessionAtItsRankCapEarnsATrainerVisit)
{
    // Live 2026-08-26: 57 miners and 49 herbalists sat at exactly 75 of 75 across 200 online bots, and
    // 350 of 372 primary professions were pinned at the Apprentice cap. Progression shipped scoped to
    // crafting and dropped herbalism, mining and skinning before a milestone was ever considered, so
    // nothing ever asked a trainer for a gathering RANK, and a gathering skill cannot pass its rank cap
    // without one. Every craft downstream of a gatherer starved with it.
    std::vector<ProfessionProgressionRecipe> const noRecipes;

    // At the cap with nothing to craft, the only move is the trainer. Herbalism has no item-creating
    // ability anywhere in SkillLineAbility.dbc, so a recipe milestone here would be a trip to buy
    // something that does not exist.
    std::optional<ProfessionProgressionMilestone> const herbalism =
        SelectProgressionMilestone({State(SKILL_HERBALISM, 75u, 76u, 80u, true, false)}, noRecipes, MAX_PRESSURE);
    ASSERT_TRUE(herbalism.has_value());
    EXPECT_EQ(herbalism->kind, ProfessionProgressionMilestoneKind::TrainerRank);
    EXPECT_EQ(herbalism->professionSkillId, SKILL_HERBALISM);
    EXPECT_EQ(herbalism->targetSkill, 76u);

    uint32 trainerCommands = 0u;
    ProfessionProgressionGameplayExecution const execution = ExecuteProfessionProgressionGameplay(
        {
            .action = ProfessionProgressionCycleAction::TrainerRank,
            .milestone = herbalism,
        },
        {.scheduleTrainer = [&trainerCommands](ProfessionProgressionMilestone const&)
         {
             ++trainerCommands;
             return true;
         }});
    EXPECT_TRUE(execution.succeeded);
    EXPECT_EQ(trainerCommands, 1u);

    // And the rank actually settles once the trainer has been visited.
    EXPECT_EQ(ReconcileProgressionExecution(*herbalism, {}, {.currentSkill = 75u, .maximumSkill = 150u}).state,
              ProfessionProgressionExecutionState::Complete);

    // Below the cap a gatherer has nothing to ask anyone for: it advances by gathering. Selecting
    // anything here would send it to a trainer every cycle for the rest of its life.
    EXPECT_FALSE(
        SelectProgressionMilestone({State(SKILL_SKINNING, 40u, 75u, 80u, false, false)}, noRecipes, MAX_PRESSURE)
            .has_value());
}
