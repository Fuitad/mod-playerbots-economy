/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTCAREERPROGRESSION_H
#define PLAYERBOTS_PLAYERBOTCAREERPROGRESSION_H

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace PlayerbotCareer
{
inline constexpr std::uint32_t PROFESSION_PROGRESSION_MAXIMUM_PRESSURE = 10'000u;
inline constexpr std::uint32_t PROFESSION_PROGRESSION_MAXIMUM_BATCH = 5u;

enum class ProfessionProgressionMilestoneKind : std::uint8_t
{
    AdvanceSkill,
    TrainerRank,
    TrainerRecipe
};

enum class ProfessionProgressionBlocker : std::uint8_t
{
    None,
    Aligned,
    NoAdvancingRecipe,
    MaterialSourceUnavailable,
    Combat,
    Survival,
    Transport,
    DirectObjective,
    GroupCommitment
};

struct ProfessionProgressionState
{
    std::uint16_t professionSkillId = 0;
    std::uint16_t currentSkill = 0;
    std::uint16_t targetSkill = 0;
    std::uint8_t affinity = 0;
    bool planned = false;
    bool learned = false;
    bool trainerRankRequired = false;
    bool trainerRecipeRequired = false;
};

struct ProfessionProgressionReagent
{
    std::uint32_t itemId = 0;
    std::uint32_t count = 0;
    std::uint32_t ownedCount = 0;
    bool ordinaryVendorAvailable = false;
    // The bot can source the shortfall itself: a gathering node it has the skill for, or an auction listing.
    bool obtainable = false;
    // The bot holds an item it can disenchant into this reagent. No travel and no material commitment is
    // involved, so such a shortfall never counts as scarce.
    bool disenchantable = false;
};

struct ProfessionProgressionRecipe
{
    std::uint16_t professionSkillId = 0;
    std::uint32_t spellId = 0;
    std::uint32_t outputItemId = 0;
    bool known = false;
    bool advancesSkill = false;
    bool trainerRecipe = false;
    std::vector<ProfessionProgressionReagent> reagents;
};

struct ProfessionProgressionMilestone
{
    ProfessionProgressionMilestoneKind kind = ProfessionProgressionMilestoneKind::AdvanceSkill;
    std::uint16_t professionSkillId = 0;
    std::uint16_t targetSkill = 0;
    std::uint32_t recipeSpellId = 0;
    std::uint32_t outputItemId = 0;

    bool operator==(ProfessionProgressionMilestone const&) const = default;
};

enum class ProfessionProgressionAdmissionState : std::uint8_t
{
    Waiting,
    Ready
};

struct ProfessionProgressionAdmission
{
    ProfessionProgressionAdmissionState state = ProfessionProgressionAdmissionState::Waiting;
    ProfessionProgressionMilestone milestone;
    ProfessionProgressionBlocker blocker = ProfessionProgressionBlocker::None;
    std::uint32_t missingItemId = 0;
    std::uint32_t batchQuantity = 0;
    bool usesVendorInputs = false;
    bool authorizesExecutableDemand = false;
    bool authorizesMaterialReservation = false;
    bool authorizesGatheringClaim = false;
    bool authorizesPurchaseClaim = false;
    bool authorizesProductionClaim = false;
    bool authorizesListing = false;
};

struct ProfessionProgressionAuthority
{
    bool combat = false;
    bool survival = false;
    bool transport = false;
    bool directObjective = false;
    bool groupCommitment = false;

    [[nodiscard]] ProfessionProgressionBlocker Blocker() const;
};

struct ProfessionProgressionObservation
{
    std::uint16_t currentSkill = 0;
    std::uint16_t maximumSkill = 0;
    bool knownRecipe = false;
    std::uint32_t producedQuantity = 0;
};

enum class ProfessionProgressionExecutionState : std::uint8_t
{
    Preempted,
    Ready,
    Complete
};

struct ProfessionProgressionExecution
{
    ProfessionProgressionExecutionState state = ProfessionProgressionExecutionState::Ready;
    ProfessionProgressionMilestone milestone;
    ProfessionProgressionBlocker blocker = ProfessionProgressionBlocker::None;
    std::uint32_t activeBatchCount = 0;
};

enum class ProfessionProgressionAttemptState : std::uint8_t
{
    Pending,
    Advanced,
    ObservationBlocked
};

struct ProfessionProgressionAttemptObservation
{
    std::uint16_t startingSkill = 0;
    std::uint16_t currentSkill = 0;
    std::uint32_t startingOutputQuantity = 0;
    std::uint32_t currentOutputQuantity = 0;
    std::uint32_t elapsedSeconds = 0;
    std::uint32_t timeoutSeconds = 60u;
};

struct ProfessionProgressionAttemptReconciliation
{
    ProfessionProgressionAttemptState state = ProfessionProgressionAttemptState::Pending;
    bool outputObserved = false;
    bool retainAttempt = true;
};

enum class ProfessionProgressionCycleAction : std::uint8_t
{
    None,
    Preempted,
    WaitObservation,
    ObservationBlocked,
    TrainerRank,
    TrainerRecipe,
    BuyVendorInput,
    Disenchant,
    Craft,
    AttemptAdvanced,
    Complete,
    Blocked
};

struct ProfessionProgressionCycleInput
{
    std::vector<ProfessionProgressionState> professions;
    std::vector<ProfessionProgressionRecipe> recipes;
    ProfessionProgressionAuthority authority;
    ProfessionProgressionObservation observation;
    std::optional<ProfessionProgressionMilestone> milestone;
    std::uint32_t batchRemaining = 0;
    std::optional<ProfessionProgressionAttemptObservation> attempt;
    std::uint32_t maximumPressure = PROFESSION_PROGRESSION_MAXIMUM_PRESSURE;
    std::uint32_t maximumBatch = PROFESSION_PROGRESSION_MAXIMUM_BATCH;
};

struct ProfessionProgressionCycleDecision
{
    ProfessionProgressionCycleAction action = ProfessionProgressionCycleAction::None;
    std::optional<ProfessionProgressionMilestone> milestone;
    std::uint32_t batchRemaining = 0;
    std::uint32_t itemId = 0;
    ProfessionProgressionBlocker blocker = ProfessionProgressionBlocker::None;
    bool outputObserved = false;
    bool retainAttempt = false;
};

struct ProfessionProgressionGameplay
{
    std::function<bool(ProfessionProgressionMilestone const&)> scheduleTrainer;
    std::function<bool(std::uint32_t itemId, std::uint32_t recipeSpellId)> buyVendorInput;
    std::function<bool(std::uint32_t itemId, std::uint32_t recipeSpellId)> disenchant;
    std::function<bool(std::uint32_t recipeSpellId, std::uint32_t outputItemId)> craft;
};

struct ProfessionProgressionGameplayExecution
{
    ProfessionProgressionCycleAction action = ProfessionProgressionCycleAction::None;
    bool attempted = false;
    bool succeeded = false;
};

[[nodiscard]] std::uint32_t ProgressionPressure(ProfessionProgressionState const& profession,
                                                std::uint32_t maximumPressure);
[[nodiscard]] std::uint8_t ProgressionSchedulingEngagement(std::uint8_t baseEngagement, std::uint32_t pressure,
                                                           std::uint32_t maximumPressure);
[[nodiscard]] std::uint32_t ProgressionBatchCeiling(std::uint8_t affinity, std::uint16_t lag,
                                                    std::uint32_t maximumBatch);
[[nodiscard]] std::optional<ProfessionProgressionMilestone> SelectProgressionMilestone(
    std::vector<ProfessionProgressionState> const& professions, std::vector<ProfessionProgressionRecipe> const& recipes,
    std::uint32_t maximumPressure, std::optional<ProfessionProgressionMilestone> const& existing = std::nullopt);
[[nodiscard]] ProfessionProgressionAdmission AdmitProgressionBatch(ProfessionProgressionMilestone const& milestone,
                                                                   ProfessionProgressionRecipe const& recipe,
                                                                   std::uint32_t maximumBatch);
[[nodiscard]] ProfessionProgressionExecution ReconcileProgressionExecution(
    ProfessionProgressionMilestone const& milestone, ProfessionProgressionAuthority const& authority,
    ProfessionProgressionObservation const& observation);
[[nodiscard]] ProfessionProgressionAttemptReconciliation ReconcileProgressionAttempt(
    ProfessionProgressionAttemptObservation const& observation);
[[nodiscard]] ProfessionProgressionCycleDecision DecideProfessionProgressionCycle(
    ProfessionProgressionCycleInput const& input);
[[nodiscard]] ProfessionProgressionGameplayExecution ExecuteProfessionProgressionGameplay(
    ProfessionProgressionCycleDecision const& decision, ProfessionProgressionGameplay const& gameplay);
[[nodiscard]] char const* ProgressionBlockerCode(ProfessionProgressionBlocker blocker);
}  // namespace PlayerbotCareer

#endif
