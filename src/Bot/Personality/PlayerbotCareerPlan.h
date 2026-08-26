/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTCAREERPLAN_H
#define PLAYERBOTS_PLAYERBOTCAREERPLAN_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "PlayerbotPersonality.h"

class Player;
class SpellInfo;
struct ItemTemplate;
namespace Trainer
{
struct Spell;
class Trainer;
}  // namespace Trainer

inline constexpr std::uint32_t PLAYERBOT_CAREER_PLAN_VERSION = 2;

enum class PlayerbotRecipeSpendingStyle : std::uint8_t
{
    None = 0,
    Minimal,
    Progression,
    Completionist
};

struct PlayerbotCareerCandidateSeed
{
    std::vector<std::uint16_t> primarySkills;
    std::vector<std::uint16_t> secondarySkills;
    bool hasCrafting;
    bool hasGathering;
    std::uint32_t baseWeight;
    std::string summary;
    // The gathering skill in primarySkills is passive supply for the crafting skill: it must neither require
    // gathering affinity nor raise engagement, so hasGathering stays false and survives the learned-primary
    // rewrite (ReachableSeed).
    bool feeder = false;
};

struct PlayerbotCareerCandidate
{
    std::string token;
    std::string summary;
    std::vector<std::uint16_t> primarySkills;
    std::vector<std::uint16_t> secondarySkills;
    PlayerbotRecipeSpendingStyle spendingStyle = PlayerbotRecipeSpendingStyle::None;
    bool marketEligible = false;
    std::uint8_t engagement = 0;
    std::uint64_t weight = 0;
};

enum class PlayerbotCareerCapabilityGoalKind : std::uint8_t
{
    Trainer = 1,
    Recipe
};

struct PlayerbotCareerCapabilityGoal
{
    PlayerbotCareerCapabilityGoalKind kind = PlayerbotCareerCapabilityGoalKind::Trainer;
    std::uint16_t professionSkillId = 0;
    std::uint32_t recipeSpellId = 0;
    std::uint32_t outputItemId = 0;

    bool operator==(PlayerbotCareerCapabilityGoal const&) const = default;
};

struct PlayerbotCareerPlan
{
    std::uint32_t personalityVersion = PLAYERBOT_PERSONALITY_API_VERSION;
    std::uint32_t careerVersion = PLAYERBOT_CAREER_PLAN_VERSION;
    std::uint64_t botGuid = 0;
    std::string candidateToken;
    std::vector<std::uint16_t> primarySkills;
    std::vector<std::uint16_t> secondarySkills;
    PlayerbotRecipeSpendingStyle spendingStyle = PlayerbotRecipeSpendingStyle::None;
    bool marketEligible = false;
    std::uint8_t engagement = 0;
    std::optional<PlayerbotCareerCapabilityGoal> capabilityGoal;
    std::vector<std::uint16_t> primarySkillAmendments;
};

enum class PlayerbotCareerTrainerObjectiveKind : std::uint8_t
{
    BaseCareer,
    CapabilityRemediation,
    Progression
};

struct PlayerbotCareerTrainerObjective
{
    PlayerbotCareerTrainerObjectiveKind kind = PlayerbotCareerTrainerObjectiveKind::BaseCareer;
    std::uint16_t professionSkillId = 0;
    bool primaryProfession = false;
    bool rankOnly = true;

    bool operator==(PlayerbotCareerTrainerObjective const&) const = default;
};

enum class PlayerbotCareerAcquisitionState : std::uint8_t
{
    None,
    Blocked,
    Travel,
    Learn,
    Complete
};

enum class PlayerbotCareerAcquisitionBlocker : std::uint8_t
{
    None,
    PrimarySlotsOccupied,
    TrainerUnavailable,
    UnsafeRoute,
    TrainerIneligible,
    InsufficientProtectedMoney,
    CompletionUnobserved
};

struct PlayerbotCareerAcquisition
{
    std::optional<PlayerbotCareerTrainerObjective> objective;
    PlayerbotCareerAcquisitionState state = PlayerbotCareerAcquisitionState::None;
    PlayerbotCareerAcquisitionBlocker blocker = PlayerbotCareerAcquisitionBlocker::None;
};

struct PlayerbotCareerAcquisitionFacts
{
    bool professionLearned = false;
    bool trainerAvailable = true;
    bool routeSafe = true;
    bool trainerEligible = true;
    bool affordable = true;
    bool atTrainer = false;
    bool lessonAttempted = false;
};

struct PlayerbotTrainerLessonCandidate
{
    std::uint32_t spellId = 0;
    std::uint16_t skillId = 0;
    std::uint32_t cost = 0;
    bool isRank = false;
    bool canRaiseSkill = false;
};

enum class PlayerbotRecipeSource : std::uint8_t
{
    Vendor,
    Drop,
    OwnedItem,
    AuctionHouse
};

struct PlayerbotRecipeCandidate
{
    std::uint32_t itemId = 0;
    std::uint16_t skillId = 0;
    std::uint32_t cost = 0;
    bool canRaiseSkill = false;
    bool isKnown = false;
    bool isUsable = false;
    std::uint32_t recipeSpellId = 0;
    std::uint32_t outputItemId = 0;
};

struct PlayerbotCareerCandidateView
{
    std::string token;
    std::string summary;
    PlayerbotRecipeSpendingStyle maximumSpendingStyle = PlayerbotRecipeSpendingStyle::None;
    bool marketEligible = false;
    std::uint8_t engagement = 0;
};

struct PlayerbotCareerPlanRequest
{
    std::uint64_t requestId = 0;
    std::uint64_t botGuid = 0;
    std::uint32_t personalityVersion = PLAYERBOT_PERSONALITY_API_VERSION;
    std::uint32_t careerVersion = PLAYERBOT_CAREER_PLAN_VERSION;
    PlayerbotPersonalityProfile profile;
    std::vector<PlayerbotCareerCandidateView> candidates;
};

struct PlayerbotCareerPlanResponse
{
    std::uint64_t requestId = 0;
    std::uint64_t botGuid = 0;
    std::uint32_t personalityVersion = 0;
    std::uint32_t careerVersion = 0;
    std::string candidateToken;
    PlayerbotRecipeSpendingStyle spendingStyle = PlayerbotRecipeSpendingStyle::None;
};

class PlayerbotCareerPlanProvider
{
public:
    virtual ~PlayerbotCareerPlanProvider() = default;
    virtual bool TrySubmit(PlayerbotCareerPlanRequest const& request) = 0;
    virtual std::optional<PlayerbotCareerPlanResponse> Poll(std::uint64_t requestId) = 0;
    virtual std::uint64_t ResponseDeadlineMs() const = 0;
};

enum class PlayerbotCareerPlanResolutionStatus : std::uint8_t
{
    Pending,
    Resolved
};

struct PlayerbotCareerPlanResolution
{
    PlayerbotCareerPlanResolutionStatus status = PlayerbotCareerPlanResolutionStatus::Pending;
    PlayerbotCareerPlan plan;
};

struct PlayerbotCareerPlanRecovery
{
    PlayerbotCareerPlanResolutionStatus status = PlayerbotCareerPlanResolutionStatus::Pending;
    PlayerbotCareerPlan plan;
    bool shouldPersist = false;
};

namespace PlayerbotCareer
{
std::vector<PlayerbotCareerCandidate> BuildCandidates(PlayerbotPersonalityProfile const& profile,
                                                      std::vector<PlayerbotCareerCandidateSeed> const& seeds,
                                                      std::uint32_t maxPrimarySkills);

PlayerbotCareerCandidate SelectFallback(std::vector<PlayerbotCareerCandidate> const& candidates,
                                        std::uint64_t guidCounter);

// The primary professions a bot can actually end up with: every profession it has already learned,
// since a primary slot cannot be reclaimed, plus as many of the proposed ones as the remaining slots
// hold. A candidate that omits a learned primary is unreachable and would strand the career.
std::vector<std::uint16_t> AchievablePrimarySkills(std::vector<std::uint16_t> const& proposed,
                                                   std::vector<std::uint16_t> const& learned,
                                                   std::uint32_t maxPrimarySkills);

// Gathering profession whose yield feeds a crafting profession (Mining for the ore users, Skinning for
// Leatherworking, Herbalism for Alchemy and Inscription). nullopt when the skill has no feeder or is not a
// crafting profession.
std::optional<std::uint16_t> FeederGatheringSkill(std::uint16_t craftingSkillId);

// Seed for a crafting profession carried together with its feeder gathering skill, flagged crafting-only.
PlayerbotCareerCandidateSeed FeederCraftingSeed(std::uint16_t craftingSkillId, std::uint16_t feederSkillId,
                                                std::uint32_t baseWeight);

using SkillPredicate = std::function<bool(std::uint16_t)>;

// The seed rewritten onto the primaries the bot can still reach (AchievablePrimarySkills), with hasCrafting
// and hasGathering recomputed from the result except that a feeder seed keeps hasGathering false.
PlayerbotCareerCandidateSeed ReachableSeed(PlayerbotCareerCandidateSeed const& seed,
                                           std::vector<std::uint16_t> const& learnedPrimaries,
                                           std::uint32_t maxPrimarySkills, SkillPredicate const& isCrafting,
                                           SkillPredicate const& isGathering);

// Adds weight to the pair seed holding both skills (used when a crafting skill without feeder loses its single
// seed). Returns false when no such pair seed exists.
bool FoldSingleSeedWeight(std::vector<PlayerbotCareerCandidateSeed>& seeds, std::uint16_t firstSkillId,
                          std::uint16_t secondSkillId, std::uint32_t weight);

PlayerbotCareerPlan MakePlan(std::uint64_t botGuid, PlayerbotCareerCandidate const& candidate,
                             PlayerbotRecipeSpendingStyle spendingStyle);
std::vector<std::uint16_t> EffectivePrimarySkills(PlayerbotCareerPlan const& plan);
std::vector<std::uint16_t> PlannedSkills(PlayerbotCareerPlan const& plan);
bool PlansSkill(PlayerbotCareerPlan const& plan, std::uint16_t skillId);
// True when any planned primary is Herbalism, Mining or Skinning (a gathering career or a feeder).
bool PlansGatheringSkill(PlayerbotCareerPlan const& plan);
std::string SerializePlan(PlayerbotCareerPlan const& plan);
std::optional<PlayerbotCareerPlan> DeserializePlan(std::string const& serialized, std::uint64_t expectedBotGuid);
std::optional<PlayerbotCareerPlan> DeserializePlan(std::string const& serialized, std::uint64_t expectedBotGuid,
                                                   std::vector<PlayerbotCareerCandidate> const& legalCandidates);
std::optional<PlayerbotCareerPlan> DeserializePlan(std::string const& serialized, std::uint64_t expectedBotGuid,
                                                   std::vector<PlayerbotCareerCandidate> const& legalCandidates,
                                                   std::vector<std::uint16_t> const& primaryProfessionSkillIds);
std::optional<PlayerbotCareerPlan> DeserializePlan(std::string const& serialized, std::uint64_t expectedBotGuid,
                                                   std::vector<PlayerbotCareerCandidate> const& legalCandidates,
                                                   std::vector<std::uint16_t> const& primaryProfessionSkillIds,
                                                   std::vector<std::uint16_t> const& learnedPrimaryProfessionSkillIds);
bool TryAssignCapabilityGoal(PlayerbotCareerPlan& plan, PlayerbotCareerCapabilityGoal const& goal,
                             std::vector<std::uint16_t> const& primaryProfessionSkillIds,
                             std::vector<std::uint16_t> const& learnedPrimaryProfessionSkillIds = {});
bool ClearCapabilityGoal(PlayerbotCareerPlan& plan);
PlayerbotCareerAcquisition SelectTrainerObjective(PlayerbotCareerPlan const& plan,
                                                  std::vector<std::uint16_t> const& learnedSkillIds,
                                                  std::vector<std::uint16_t> const& primaryProfessionSkillIds,
                                                  std::uint8_t freePrimaryProfessionSlots);
PlayerbotCareerAcquisition EvaluateTrainerObjective(PlayerbotCareerTrainerObjective const& objective,
                                                    PlayerbotCareerAcquisitionFacts const& facts);
char const* AcquisitionBlockerCode(PlayerbotCareerAcquisitionBlocker blocker);

bool RegisterProvider(PlayerbotCareerPlanProvider* provider);
void UnregisterProvider(PlayerbotCareerPlanProvider* provider);
PlayerbotCareerPlanResolution ResolvePlan(std::uint64_t botGuid, PlayerbotPersonalityProfile const& profile,
                                          std::vector<PlayerbotCareerCandidate> const& candidates, std::uint64_t nowMs);
PlayerbotCareerPlanRecovery ResolvePersistedPlan(std::optional<std::string> const& serialized, std::uint64_t botGuid,
                                                 PlayerbotPersonalityProfile const& profile,
                                                 std::vector<PlayerbotCareerCandidate> const& candidates,
                                                 std::uint64_t nowMs);
PlayerbotCareerPlanRecovery ResolvePersistedPlan(std::optional<std::string> const& serialized, std::uint64_t botGuid,
                                                 PlayerbotPersonalityProfile const& profile,
                                                 std::vector<PlayerbotCareerCandidate> const& candidates,
                                                 std::vector<std::uint16_t> const& primaryProfessionSkillIds,
                                                 std::uint64_t nowMs);
PlayerbotCareerPlanRecovery ResolvePersistedPlan(std::optional<std::string> const& serialized, std::uint64_t botGuid,
                                                 PlayerbotPersonalityProfile const& profile,
                                                 std::vector<PlayerbotCareerCandidate> const& candidates,
                                                 std::vector<std::uint16_t> const& primaryProfessionSkillIds,
                                                 std::vector<std::uint16_t> const& learnedPrimaryProfessionSkillIds,
                                                 std::uint64_t nowMs);
std::vector<PlayerbotCareerPlan> PollPendingPlans(std::uint64_t nowMs);
std::vector<std::uint32_t> SelectTrainerLessons(PlayerbotCareerPlan const& plan,
                                                std::vector<PlayerbotTrainerLessonCandidate> const& lessons);
std::vector<std::uint32_t> SelectTrainerLessons(PlayerbotCareerTrainerObjective const& objective,
                                                std::vector<PlayerbotTrainerLessonCandidate> const& lessons);
bool HasAffordableTrainerLesson(PlayerbotCareerPlan const& plan,
                                std::vector<PlayerbotTrainerLessonCandidate> const& lessons,
                                std::uint32_t availableMoney);
bool HasAffordableTrainerLesson(PlayerbotCareerTrainerObjective const& objective,
                                std::vector<PlayerbotTrainerLessonCandidate> const& lessons,
                                std::uint32_t availableMoney);
bool IsTrainerDestinationSafe(std::uint8_t botLevel, std::uint32_t botZoneId, std::uint32_t trainerZoneId,
                              std::uint32_t trainerMinimumLevel, bool trainerZoneIsCapital);
bool SchedulesProfessionWork(PlayerbotCareerPlan const& plan);
std::uint32_t ProfessionWorkWeight(PlayerbotCareerPlan const& plan, std::uint32_t baseWeight);
bool TrainerOffersCareerLesson(PlayerbotCareerPlan const& plan, Player const* bot, Trainer::Trainer const* trainer,
                               float reputationDiscount, std::uint32_t availableMoney);
bool TrainerOffersCareerLesson(PlayerbotCareerTrainerObjective const& objective, Player const* bot,
                               Trainer::Trainer const* trainer, float reputationDiscount, std::uint32_t availableMoney);
PlayerbotTrainerLessonCandidate DescribeTrainerLesson(Trainer::Spell const& trainerSpell, SpellInfo const* spellInfo,
                                                      Player const* bot, std::uint32_t cost);
bool IsRecipeAcquisitionAllowed(PlayerbotCareerPlan const& plan, PlayerbotRecipeCandidate const& recipe,
                                PlayerbotRecipeSource source);
std::vector<std::uint32_t> SelectRecipePurchases(PlayerbotCareerPlan const& plan,
                                                 std::vector<PlayerbotRecipeCandidate> const& recipes,
                                                 PlayerbotRecipeSource source);
PlayerbotRecipeCandidate DescribeRecipe(ItemTemplate const* recipe, Player const* bot, std::uint32_t cost);
}  // namespace PlayerbotCareer

#endif  // PLAYERBOTS_PLAYERBOTCAREERPLAN_H
