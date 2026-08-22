/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotCareerProgression.h"

#include <algorithm>
#include <limits>

using namespace PlayerbotCareer;

namespace
{
std::uint16_t ProfessionLag(ProfessionProgressionState const& profession)
{
    return profession.targetSkill > profession.currentSkill ? profession.targetSkill - profession.currentSkill : 0u;
}

bool RecipeHasFeasibleBatch(ProfessionProgressionRecipe const& recipe)
{
    return std::all_of(
        recipe.reagents.begin(), recipe.reagents.end(), [](ProfessionProgressionReagent const& reagent)
        { return !reagent.count || reagent.ordinaryVendorAvailable || reagent.ownedCount >= reagent.count; });
}

// A recipe the bot can feed on its own: every shortfall is obtainable, and there is at most one scarce
// reagent to source, which is all the material source path carries. Ranks above any other infeasible
// recipe, below one already in the bags.
bool RecipeHasFeedableBatch(ProfessionProgressionRecipe const& recipe)
{
    std::size_t scarce = 0u;
    for (ProfessionProgressionReagent const& reagent : recipe.reagents)
    {
        if (!reagent.count || reagent.ordinaryVendorAvailable || reagent.ownedCount >= reagent.count ||
            reagent.disenchantable)
        {
            continue;
        }
        if (!reagent.obtainable)
            return false;
        ++scarce;
    }
    return scarce <= 1u;
}

int RecipeRank(ProfessionProgressionRecipe const& recipe)
{
    return RecipeHasFeasibleBatch(recipe) ? 2 : RecipeHasFeedableBatch(recipe) ? 1 : 0;
}

// Best recipe rank a profession can offer right now: 0 when every advancing recipe is one the bot
// cannot feed, which is the normal state of a rank 1 profession whose only recipe needs drop-only
// reagents. Such a profession yields to any other planned one that can actually progress.
int BestRecipeRank(std::uint16_t professionSkillId, std::vector<ProfessionProgressionRecipe> const& recipes)
{
    int best = 0;
    for (ProfessionProgressionRecipe const& recipe : recipes)
    {
        if (recipe.professionSkillId == professionSkillId && recipe.known && recipe.advancesSkill)
            best = std::max(best, RecipeRank(recipe));
    }
    return best;
}

bool AnotherProfessionCanProgress(std::uint16_t professionSkillId,
                                  std::vector<ProfessionProgressionState> const& professions,
                                  std::vector<ProfessionProgressionRecipe> const& recipes)
{
    return std::any_of(professions.begin(), professions.end(),
                       [&](ProfessionProgressionState const& candidate)
                       {
                           return candidate.professionSkillId != professionSkillId && candidate.planned &&
                                  candidate.learned && ProfessionLag(candidate) &&
                                  BestRecipeRank(candidate.professionSkillId, recipes) > 0;
                       });
}

bool MilestoneStillValid(ProfessionProgressionMilestone const& milestone,
                         std::vector<ProfessionProgressionState> const& professions,
                         std::vector<ProfessionProgressionRecipe> const& recipes)
{
    auto const profession = std::find_if(professions.begin(), professions.end(),
                                         [&milestone](ProfessionProgressionState const& candidate) {
                                             return candidate.professionSkillId == milestone.professionSkillId &&
                                                    candidate.planned && candidate.learned;
                                         });
    if (profession == professions.end() || !ProfessionLag(*profession))
        return false;
    if (milestone.kind == ProfessionProgressionMilestoneKind::TrainerRank)
        return profession->trainerRankRequired;
    if (milestone.kind == ProfessionProgressionMilestoneKind::TrainerRecipe)
        return profession->trainerRecipeRequired;
    auto const recipe =
        std::find_if(recipes.begin(), recipes.end(), [&milestone](ProfessionProgressionRecipe const& value)
                     { return value.spellId == milestone.recipeSpellId; });
    if (recipe == recipes.end() || !recipe->known || !recipe->advancesSkill)
        return false;
    int const rank = RecipeRank(*recipe);
    if (!rank && AnotherProfessionCanProgress(milestone.professionSkillId, professions, recipes))
        return false;
    return std::none_of(recipes.begin(), recipes.end(),
                        [&milestone, rank](ProfessionProgressionRecipe const& candidate)
                        {
                            return candidate.professionSkillId == milestone.professionSkillId && candidate.known &&
                                   candidate.advancesSkill && RecipeRank(candidate) > rank;
                        });
}
}  // namespace

ProfessionProgressionBlocker ProfessionProgressionAuthority::Blocker() const
{
    if (combat)
        return ProfessionProgressionBlocker::Combat;
    if (survival)
        return ProfessionProgressionBlocker::Survival;
    if (transport)
        return ProfessionProgressionBlocker::Transport;
    if (directObjective)
        return ProfessionProgressionBlocker::DirectObjective;
    if (groupCommitment)
        return ProfessionProgressionBlocker::GroupCommitment;
    return ProfessionProgressionBlocker::None;
}

std::uint32_t PlayerbotCareer::ProgressionPressure(ProfessionProgressionState const& profession,
                                                   std::uint32_t maximumPressure)
{
    std::uint16_t const lag = ProfessionLag(profession);
    if (!maximumPressure || !profession.planned || !profession.learned || !lag || !profession.targetSkill)
        return 0u;

    std::uint64_t const scaled = static_cast<std::uint64_t>(maximumPressure) * profession.affinity * lag;
    std::uint64_t const denominator = static_cast<std::uint64_t>(100u) * profession.targetSkill;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(maximumPressure, scaled / denominator));
}

std::uint8_t PlayerbotCareer::ProgressionSchedulingEngagement(std::uint8_t baseEngagement, std::uint32_t pressure,
                                                              std::uint32_t maximumPressure)
{
    if (!maximumPressure)
        return baseEngagement;
    std::uint32_t const pressurePercent = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(std::min(maximumPressure, pressure)) * 100u / maximumPressure);
    return static_cast<std::uint8_t>(
        std::min<std::uint32_t>(100u, std::max<std::uint32_t>(baseEngagement, pressurePercent)));
}

std::uint32_t PlayerbotCareer::ProgressionBatchCeiling(std::uint8_t affinity, std::uint16_t lag,
                                                       std::uint32_t maximumBatch)
{
    if (!maximumBatch || !lag)
        return 0u;
    std::uint32_t const pressureBatch = 1u + affinity / 25u + lag / 50u;
    return std::min(maximumBatch, pressureBatch);
}

std::optional<ProfessionProgressionMilestone> PlayerbotCareer::SelectProgressionMilestone(
    std::vector<ProfessionProgressionState> const& professions, std::vector<ProfessionProgressionRecipe> const& recipes,
    std::uint32_t maximumPressure, std::optional<ProfessionProgressionMilestone> const& existing)
{
    if (existing && MilestoneStillValid(*existing, professions, recipes))
        return existing;

    ProfessionProgressionState const* selectedProfession = nullptr;
    std::uint32_t selectedPressure = 0u;
    int selectedProfessionRank = 0;
    for (ProfessionProgressionState const& profession : professions)
    {
        std::uint32_t const pressure = ProgressionPressure(profession, maximumPressure);
        if (!pressure)
            continue;
        bool const hasAdvancingRecipe = std::any_of(
            recipes.begin(), recipes.end(),
            [&profession](ProfessionProgressionRecipe const& recipe) {
                return recipe.professionSkillId == profession.professionSkillId && recipe.known && recipe.advancesSkill;
            });
        if (!hasAdvancingRecipe && !profession.trainerRankRequired && !profession.trainerRecipeRequired)
            continue;
        // A profession that can feed a recipe (or needs a trainer visit, which is always doable) outranks
        // one whose recipes are all unfeedable, whatever the pressure; pressure decides within a tier.
        int const rank = (profession.trainerRankRequired || profession.trainerRecipeRequired)
                             ? 1
                             : std::min(1, BestRecipeRank(profession.professionSkillId, recipes));
        if (selectedProfession &&
            (rank < selectedProfessionRank || (rank == selectedProfessionRank && pressure <= selectedPressure)))
            continue;
        selectedProfession = &profession;
        selectedPressure = pressure;
        selectedProfessionRank = rank;
    }
    if (!selectedProfession)
        return std::nullopt;

    ProfessionProgressionRecipe const* selectedRecipe = nullptr;
    int selectedRank = 0;
    for (ProfessionProgressionRecipe const& recipe : recipes)
    {
        if (recipe.professionSkillId != selectedProfession->professionSkillId || !recipe.known || !recipe.advancesSkill)
            continue;
        int const rank = RecipeRank(recipe);
        if (!selectedRecipe || rank > selectedRank ||
            (rank == selectedRank && recipe.spellId < selectedRecipe->spellId))
        {
            selectedRecipe = &recipe;
            selectedRank = rank;
        }
    }
    if (!selectedRecipe)
    {
        return ProfessionProgressionMilestone{
            .kind = selectedProfession->trainerRankRequired ? ProfessionProgressionMilestoneKind::TrainerRank
                                                            : ProfessionProgressionMilestoneKind::TrainerRecipe,
            .professionSkillId = selectedProfession->professionSkillId,
            .targetSkill = selectedProfession->targetSkill,
        };
    }
    return ProfessionProgressionMilestone{
        .kind = ProfessionProgressionMilestoneKind::AdvanceSkill,
        .professionSkillId = selectedProfession->professionSkillId,
        .targetSkill = selectedProfession->targetSkill,
        .recipeSpellId = selectedRecipe->spellId,
        .outputItemId = selectedRecipe->outputItemId,
    };
}

ProfessionProgressionAdmission PlayerbotCareer::AdmitProgressionBatch(ProfessionProgressionMilestone const& milestone,
                                                                      ProfessionProgressionRecipe const& recipe,
                                                                      std::uint32_t maximumBatch)
{
    ProfessionProgressionAdmission admission;
    admission.milestone = milestone;
    if (!maximumBatch || !recipe.known || !recipe.advancesSkill || recipe.spellId != milestone.recipeSpellId ||
        recipe.professionSkillId != milestone.professionSkillId)
    {
        admission.blocker = ProfessionProgressionBlocker::NoAdvancingRecipe;
        return admission;
    }

    std::uint32_t feasible = maximumBatch;
    for (ProfessionProgressionReagent const& reagent : recipe.reagents)
    {
        if (!reagent.count)
            continue;
        if (reagent.ordinaryVendorAvailable)
        {
            admission.usesVendorInputs |= reagent.ownedCount < reagent.count * maximumBatch;
            continue;
        }
        std::uint32_t const owned = reagent.ownedCount / reagent.count;
        // A shortfall the bot can disenchant its way out of admits one craft: the cycle disenchants
        // first, then crafts once the reagent lands in the bags.
        feasible = std::min(feasible, owned ? owned : reagent.disenchantable ? 1u : 0u);
        if (!feasible)
        {
            admission.blocker = ProfessionProgressionBlocker::MaterialSourceUnavailable;
            admission.missingItemId = reagent.itemId;
            return admission;
        }
    }

    admission.state = ProfessionProgressionAdmissionState::Ready;
    admission.batchQuantity = feasible;
    return admission;
}

ProfessionProgressionExecution PlayerbotCareer::ReconcileProgressionExecution(
    ProfessionProgressionMilestone const& milestone, ProfessionProgressionAuthority const& authority,
    ProfessionProgressionObservation const& observation)
{
    ProfessionProgressionExecution execution;
    execution.milestone = milestone;
    execution.blocker = authority.Blocker();
    if (execution.blocker != ProfessionProgressionBlocker::None)
    {
        execution.state = ProfessionProgressionExecutionState::Preempted;
        return execution;
    }
    bool const skillComplete = observation.currentSkill >= milestone.targetSkill;
    bool const rankComplete = milestone.kind == ProfessionProgressionMilestoneKind::TrainerRank &&
                              observation.maximumSkill >= milestone.targetSkill;
    bool const recipeComplete =
        milestone.kind == ProfessionProgressionMilestoneKind::TrainerRecipe && observation.knownRecipe;
    if (skillComplete || rankComplete || recipeComplete)
    {
        execution.state = ProfessionProgressionExecutionState::Complete;
        return execution;
    }
    execution.state = ProfessionProgressionExecutionState::Ready;
    execution.activeBatchCount = 1u;
    return execution;
}

ProfessionProgressionAttemptReconciliation PlayerbotCareer::ReconcileProgressionAttempt(
    ProfessionProgressionAttemptObservation const& observation)
{
    ProfessionProgressionAttemptReconciliation reconciliation;
    reconciliation.outputObserved = observation.currentOutputQuantity > observation.startingOutputQuantity;
    if (observation.currentSkill > observation.startingSkill)
    {
        reconciliation.state = ProfessionProgressionAttemptState::Advanced;
        reconciliation.retainAttempt = false;
        return reconciliation;
    }
    if (observation.elapsedSeconds >= observation.timeoutSeconds)
    {
        reconciliation.state = ProfessionProgressionAttemptState::ObservationBlocked;
        reconciliation.retainAttempt = false;
    }
    return reconciliation;
}

ProfessionProgressionCycleDecision PlayerbotCareer::DecideProfessionProgressionCycle(
    ProfessionProgressionCycleInput const& input)
{
    ProfessionProgressionCycleDecision decision;
    decision.milestone = input.milestone;
    decision.batchRemaining = input.batchRemaining;
    decision.blocker = input.authority.Blocker();
    if (decision.blocker != ProfessionProgressionBlocker::None)
    {
        decision.action = ProfessionProgressionCycleAction::Preempted;
        decision.retainAttempt = input.attempt.has_value();
        return decision;
    }

    if (input.attempt && input.milestone)
    {
        ProfessionProgressionAttemptReconciliation const reconciliation = ReconcileProgressionAttempt(*input.attempt);
        decision.outputObserved = reconciliation.outputObserved;
        decision.retainAttempt = reconciliation.retainAttempt;
        if (reconciliation.state == ProfessionProgressionAttemptState::Pending)
        {
            decision.action = ProfessionProgressionCycleAction::WaitObservation;
            return decision;
        }
        if (reconciliation.state == ProfessionProgressionAttemptState::ObservationBlocked)
        {
            decision.action = ProfessionProgressionCycleAction::ObservationBlocked;
            return decision;
        }
        if (decision.batchRemaining)
            --decision.batchRemaining;
        bool const complete = input.observation.currentSkill >= input.milestone->targetSkill;
        decision.action = complete || !decision.batchRemaining ? ProfessionProgressionCycleAction::Complete
                                                               : ProfessionProgressionCycleAction::AttemptAdvanced;
        if (decision.action == ProfessionProgressionCycleAction::Complete)
        {
            decision.milestone.reset();
            decision.batchRemaining = 0u;
        }
        return decision;
    }

    decision.milestone =
        SelectProgressionMilestone(input.professions, input.recipes, input.maximumPressure, input.milestone);
    if (!decision.milestone)
        return decision;
    if (!input.milestone || *input.milestone != *decision.milestone)
        decision.batchRemaining = 0u;
    if (decision.milestone->kind == ProfessionProgressionMilestoneKind::TrainerRank)
    {
        decision.action = ProfessionProgressionCycleAction::TrainerRank;
        return decision;
    }
    if (decision.milestone->kind == ProfessionProgressionMilestoneKind::TrainerRecipe)
    {
        decision.action = ProfessionProgressionCycleAction::TrainerRecipe;
        return decision;
    }

    auto const recipe = std::find_if(input.recipes.begin(), input.recipes.end(), [&decision](auto const& candidate)
                                     { return candidate.spellId == decision.milestone->recipeSpellId; });
    auto const profession =
        std::find_if(input.professions.begin(), input.professions.end(), [&decision](auto const& candidate)
                     { return candidate.professionSkillId == decision.milestone->professionSkillId; });
    if (recipe == input.recipes.end() || profession == input.professions.end())
    {
        decision.action = ProfessionProgressionCycleAction::Blocked;
        decision.blocker = ProfessionProgressionBlocker::NoAdvancingRecipe;
        return decision;
    }
    if (!decision.batchRemaining)
    {
        std::uint16_t const lag = profession->targetSkill > profession->currentSkill
                                      ? profession->targetSkill - profession->currentSkill
                                      : 0u;
        ProfessionProgressionAdmission const admission = AdmitProgressionBatch(
            *decision.milestone, *recipe, ProgressionBatchCeiling(profession->affinity, lag, input.maximumBatch));
        if (admission.state != ProfessionProgressionAdmissionState::Ready)
        {
            decision.action = ProfessionProgressionCycleAction::Blocked;
            decision.blocker = admission.blocker;
            decision.itemId = admission.missingItemId;
            return decision;
        }
        decision.batchRemaining = admission.batchQuantity;
    }
    for (ProfessionProgressionReagent const& reagent : recipe->reagents)
    {
        if (reagent.ownedCount >= reagent.count)
            continue;
        decision.itemId = reagent.itemId;
        if (reagent.ordinaryVendorAvailable)
            decision.action = ProfessionProgressionCycleAction::BuyVendorInput;
        else if (reagent.disenchantable)
            decision.action = ProfessionProgressionCycleAction::Disenchant;
        else
        {
            decision.action = ProfessionProgressionCycleAction::Blocked;
            decision.blocker = ProfessionProgressionBlocker::MaterialSourceUnavailable;
        }
        return decision;
    }
    decision.action = ProfessionProgressionCycleAction::Craft;
    return decision;
}

ProfessionProgressionGameplayExecution PlayerbotCareer::ExecuteProfessionProgressionGameplay(
    ProfessionProgressionCycleDecision const& decision, ProfessionProgressionGameplay const& gameplay)
{
    ProfessionProgressionGameplayExecution execution{.action = decision.action};
    if (!decision.milestone)
        return execution;
    switch (decision.action)
    {
        case ProfessionProgressionCycleAction::TrainerRank:
        case ProfessionProgressionCycleAction::TrainerRecipe:
            if (gameplay.scheduleTrainer)
            {
                execution.attempted = true;
                execution.succeeded = gameplay.scheduleTrainer(*decision.milestone);
            }
            return execution;
        case ProfessionProgressionCycleAction::BuyVendorInput:
            if (gameplay.buyVendorInput)
            {
                execution.attempted = true;
                execution.succeeded = gameplay.buyVendorInput(decision.itemId, decision.milestone->recipeSpellId);
            }
            return execution;
        case ProfessionProgressionCycleAction::Disenchant:
            if (gameplay.disenchant)
            {
                execution.attempted = true;
                execution.succeeded = gameplay.disenchant(decision.itemId, decision.milestone->recipeSpellId);
            }
            return execution;
        case ProfessionProgressionCycleAction::Craft:
            if (gameplay.craft)
            {
                execution.attempted = true;
                execution.succeeded =
                    gameplay.craft(decision.milestone->recipeSpellId, decision.milestone->outputItemId);
            }
            return execution;
        default:
            return execution;
    }
}

char const* PlayerbotCareer::ProgressionBlockerCode(ProfessionProgressionBlocker blocker)
{
    switch (blocker)
    {
        case ProfessionProgressionBlocker::None:
            return "";
        case ProfessionProgressionBlocker::Aligned:
            return "profession_aligned";
        case ProfessionProgressionBlocker::NoAdvancingRecipe:
            return "profession_no_advancing_recipe";
        case ProfessionProgressionBlocker::MaterialSourceUnavailable:
            return "profession_material_source_unavailable";
        case ProfessionProgressionBlocker::Combat:
            return "profession_preempted_combat";
        case ProfessionProgressionBlocker::Survival:
            return "profession_preempted_survival";
        case ProfessionProgressionBlocker::Transport:
            return "profession_preempted_transport";
        case ProfessionProgressionBlocker::DirectObjective:
            return "profession_preempted_direct_objective";
        case ProfessionProgressionBlocker::GroupCommitment:
            return "profession_preempted_group_commitment";
    }
    return "profession_unknown";
}
