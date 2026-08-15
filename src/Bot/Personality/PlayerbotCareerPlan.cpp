/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotCareerPlan.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "ItemTemplate.h"
#include "Player.h"
#include "SpellMgr.h"
#include "Trainer.h"

namespace
{
constexpr uint8 ACTIVE_AFFINITY_MINIMUM = 25;
constexpr uint8 PROGRESSION_CRAFTING_MINIMUM = 50;
constexpr uint8 COMPLETIONIST_CRAFTING_MINIMUM = 75;
constexpr uint64 CAREER_FALLBACK_NAMESPACE = 0x5042434152454552ULL;
constexpr uint64 CAREER_TOKEN_NAMESPACE = 0x50424341544F4B4EULL;

struct PendingCareerRequest
{
    uint64 requestId;
    uint64 startedAtMs;
    uint64 responseDeadlineMs;
    std::vector<PlayerbotCareerCandidate> candidates;
};

PlayerbotCareerPlanProvider* careerProvider = nullptr;
uint64 nextCareerRequestId = 1u;
std::unordered_map<uint64, PendingCareerRequest> pendingCareerRequests;

PlayerbotCareerCandidate NoProfessionCandidate(PlayerbotPersonalityProfile const& profile)
{
    uint8 const highestAffinity = std::max(profile.craftingAffinity, profile.gatheringAffinity);
    return {"career-none",
            "no professions",
            {},
            {},
            PlayerbotRecipeSpendingStyle::None,
            false,
            0u,
            static_cast<uint64>(101u - highestAffinity) * 100u};
}

bool HasValidSkills(PlayerbotCareerCandidateSeed const& seed, uint32 maxPrimarySkills)
{
    if (seed.primarySkills.size() > maxPrimarySkills)
        return false;

    std::unordered_set<uint16> skills;
    for (uint16 skillId : seed.primarySkills)
        if (!skillId || !skills.insert(skillId).second)
            return false;
    for (uint16 skillId : seed.secondarySkills)
        if (!skillId || !skills.insert(skillId).second)
            return false;

    return !skills.empty();
}

bool MatchesActiveAffinities(PlayerbotCareerCandidateSeed const& seed, PlayerbotPersonalityProfile const& profile)
{
    if (!seed.hasCrafting && !seed.hasGathering)
        return false;
    if (seed.hasCrafting && profile.craftingAffinity < ACTIVE_AFFINITY_MINIMUM)
        return false;
    if (seed.hasGathering && profile.gatheringAffinity < ACTIVE_AFFINITY_MINIMUM)
        return false;

    return true;
}

PlayerbotRecipeSpendingStyle SpendingStyle(PlayerbotCareerCandidateSeed const& seed,
                                           PlayerbotPersonalityProfile const& profile)
{
    if (!seed.hasCrafting)
        return PlayerbotRecipeSpendingStyle::Minimal;
    if (profile.craftingAffinity >= COMPLETIONIST_CRAFTING_MINIMUM)
        return PlayerbotRecipeSpendingStyle::Completionist;
    if (profile.craftingAffinity >= PROGRESSION_CRAFTING_MINIMUM)
        return PlayerbotRecipeSpendingStyle::Progression;

    return PlayerbotRecipeSpendingStyle::Minimal;
}

uint64 CandidateWeight(PlayerbotCareerCandidateSeed const& seed, PlayerbotPersonalityProfile const& profile)
{
    bool const firstIsCrafting = seed.hasCrafting;
    bool const secondIsCrafting = seed.hasCrafting && !seed.hasGathering;
    uint32 const affinityFactor = PlayerbotPersonality::ProfessionPairWeightFactor(
        firstIsCrafting, secondIsCrafting, profile.craftingAffinity, profile.gatheringAffinity);
    return static_cast<uint64>(std::max(1u, seed.baseWeight)) * affinityFactor;
}

uint8 CareerEngagement(PlayerbotCareerCandidateSeed const& seed, PlayerbotPersonalityProfile const& profile)
{
    uint8 engagement = 0u;
    if (seed.hasCrafting)
        engagement = profile.craftingAffinity;
    if (seed.hasGathering)
        engagement = std::max(engagement, profile.gatheringAffinity);

    return engagement;
}

std::string CandidateToken(PlayerbotCareerCandidateSeed const& seed)
{
    std::vector<uint16> skills = seed.primarySkills;
    skills.insert(skills.end(), seed.secondarySkills.begin(), seed.secondarySkills.end());
    std::sort(skills.begin(), skills.end());

    uint64 hash = CAREER_TOKEN_NAMESPACE;
    hash = PlayerbotPersonality::SplitMix64(hash ^ static_cast<uint64>(seed.hasCrafting));
    hash = PlayerbotPersonality::SplitMix64(hash ^ (static_cast<uint64>(seed.hasGathering) << 1u));
    for (uint16 skillId : skills)
        hash = PlayerbotPersonality::SplitMix64(hash ^ skillId);

    std::ostringstream token;
    token << "career-" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return token.str();
}

template <typename T>
bool ParseUnsigned(std::string_view text, T& value)
{
    uint64 parsed = 0;
    auto const result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        parsed > static_cast<uint64>(std::numeric_limits<T>::max()))
        return false;

    value = static_cast<T>(parsed);
    return true;
}

std::vector<std::string_view> Split(std::string_view value, char separator)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= value.size())
    {
        size_t const end = value.find(separator, start);
        if (end == std::string_view::npos)
        {
            parts.push_back(value.substr(start));
            break;
        }

        parts.push_back(value.substr(start, end - start));
        start = end + 1u;
    }

    return parts;
}

std::string SerializeSkills(std::vector<uint16> const& skills)
{
    std::ostringstream serialized;
    for (size_t index = 0; index < skills.size(); ++index)
    {
        if (index)
            serialized << ',';
        serialized << skills[index];
    }

    return serialized.str();
}

bool ParseSkills(std::string_view serialized, std::vector<uint16>& skills)
{
    skills.clear();
    if (serialized.empty())
        return true;

    for (std::string_view part : Split(serialized, ','))
    {
        uint16 skill = 0;
        if (!ParseUnsigned(part, skill) || !skill)
            return false;
        skills.push_back(skill);
    }

    return true;
}

PlayerbotCareerCandidate const* FindCandidate(std::vector<PlayerbotCareerCandidate> const& candidates,
                                              std::string_view token)
{
    auto const candidate =
        std::find_if(candidates.begin(), candidates.end(),
                     [token](PlayerbotCareerCandidate const& current) { return current.token == token; });
    return candidate == candidates.end() ? nullptr : &*candidate;
}

bool IsValidSpendingStyle(PlayerbotRecipeSpendingStyle style)
{
    return style >= PlayerbotRecipeSpendingStyle::None && style <= PlayerbotRecipeSpendingStyle::Completionist;
}

bool ContainsSkill(std::vector<uint16> const& skills, uint16 skillId)
{
    return std::find(skills.begin(), skills.end(), skillId) != skills.end();
}

bool IsPlanStateValid(PlayerbotCareerPlan const& plan,
                      std::vector<uint16> const* primaryProfessionSkillIds = nullptr,
                      std::vector<uint16> const* learnedPrimaryProfessionSkillIds = nullptr)
{
    if (plan.primarySkills.size() + plan.primarySkillAmendments.size() > 2u)
        return false;

    std::unordered_set<uint16> uniqueSkills;
    auto const addUnique = [&uniqueSkills](std::vector<uint16> const& skills)
    {
        for (uint16 skillId : skills)
            if (!skillId || !uniqueSkills.insert(skillId).second)
                return false;
        return true;
    };
    if (!addUnique(plan.primarySkills) || !addUnique(plan.primarySkillAmendments) ||
        !addUnique(plan.secondarySkills))
    {
        return false;
    }

    if (primaryProfessionSkillIds &&
        std::any_of(plan.primarySkillAmendments.begin(), plan.primarySkillAmendments.end(),
                    [primaryProfessionSkillIds](uint16 skillId)
                    { return !ContainsSkill(*primaryProfessionSkillIds, skillId); }))
    {
        return false;
    }

    if (!plan.capabilityGoal)
        return true;

    PlayerbotCareerCapabilityGoal const& goal = *plan.capabilityGoal;
    if (!goal.professionSkillId || !goal.outputItemId ||
        (primaryProfessionSkillIds && !ContainsSkill(*primaryProfessionSkillIds, goal.professionSkillId)))
    {
        return false;
    }

    bool const basePrimary = ContainsSkill(plan.primarySkills, goal.professionSkillId);
    bool const amendedPrimary = ContainsSkill(plan.primarySkillAmendments, goal.professionSkillId);
    bool const plannedSecondary = ContainsSkill(plan.secondarySkills, goal.professionSkillId);
    bool const learnedPrimary = learnedPrimaryProfessionSkillIds &&
                                ContainsSkill(*learnedPrimaryProfessionSkillIds, goal.professionSkillId);
    switch (goal.kind)
    {
        case PlayerbotCareerCapabilityGoalKind::Trainer:
            return !basePrimary && amendedPrimary && !plannedSecondary && !learnedPrimary;
        case PlayerbotCareerCapabilityGoalKind::Recipe:
            return !plannedSecondary && goal.recipeSpellId != 0u &&
                   (!primaryProfessionSkillIds || basePrimary || amendedPrimary || learnedPrimary);
    }

    return false;
}

uint16 RecipeSkillId(ItemTemplate const* recipe)
{
    if (!recipe || recipe->Class != ITEM_CLASS_RECIPE)
        return 0u;

    switch (recipe->SubClass)
    {
        case ITEM_SUBCLASS_LEATHERWORKING_PATTERN:
            return SKILL_LEATHERWORKING;
        case ITEM_SUBCLASS_TAILORING_PATTERN:
            return SKILL_TAILORING;
        case ITEM_SUBCLASS_ENGINEERING_SCHEMATIC:
            return SKILL_ENGINEERING;
        case ITEM_SUBCLASS_BLACKSMITHING:
            return SKILL_BLACKSMITHING;
        case ITEM_SUBCLASS_COOKING_RECIPE:
            return SKILL_COOKING;
        case ITEM_SUBCLASS_ALCHEMY_RECIPE:
            return SKILL_ALCHEMY;
        case ITEM_SUBCLASS_FIRST_AID_MANUAL:
            return SKILL_FIRST_AID;
        case ITEM_SUBCLASS_ENCHANTING_FORMULA:
            return SKILL_ENCHANTING;
        case ITEM_SUBCLASS_FISHING_MANUAL:
            return SKILL_FISHING;
        case ITEM_SUBCLASS_JEWELCRAFTING_RECIPE:
            return SKILL_JEWELCRAFTING;
        default:
            return 0u;
    }
}

uint32 LearnedRecipeSpellId(ItemTemplate const* recipe)
{
    if (!recipe)
        return 0u;

    if ((recipe->Spells[0].SpellId == 483 || recipe->Spells[0].SpellId == 55884) &&
        recipe->Spells[1].SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID && recipe->Spells[1].SpellId > 0)
    {
        return static_cast<uint32>(recipe->Spells[1].SpellId);
    }

    for (auto const& itemSpell : recipe->Spells)
    {
        if (itemSpell.SpellId <= 0)
            continue;

        SpellInfo const* useSpell = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
        if (!useSpell)
            continue;

        for (SpellEffectInfo const& effect : useSpell->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
                return effect.TriggerSpell;
    }

    return 0u;
}

uint32 CreatedItemId(uint32 recipeSpellId)
{
    SpellInfo const* recipeSpell = sSpellMgr->GetSpellInfo(recipeSpellId);
    if (!recipeSpell)
        return 0u;

    for (SpellEffectInfo const& effect : recipeSpell->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_CREATE_ITEM) && effect.ItemType)
            return effect.ItemType;

    return 0u;
}

PlayerbotCareerPlanResolution FallbackResolution(uint64 botGuid,
                                                 std::vector<PlayerbotCareerCandidate> const& candidates)
{
    PlayerbotCareerCandidate const selected = PlayerbotCareer::SelectFallback(candidates, botGuid);
    return {PlayerbotCareerPlanResolutionStatus::Resolved,
            PlayerbotCareer::MakePlan(botGuid, selected, selected.spendingStyle)};
}

bool IsValidResponse(PlayerbotCareerPlanResponse const& response, PendingCareerRequest const& pending, uint64 botGuid,
                     std::vector<PlayerbotCareerCandidate> const& candidates, PlayerbotCareerCandidate const*& selected)
{
    if (response.requestId != pending.requestId || response.botGuid != botGuid ||
        response.personalityVersion != PLAYERBOT_PERSONALITY_API_VERSION ||
        response.careerVersion != PLAYERBOT_CAREER_PLAN_VERSION || !IsValidSpendingStyle(response.spendingStyle))
        return false;

    selected = FindCandidate(candidates, response.candidateToken);
    return selected && static_cast<uint8>(response.spendingStyle) <= static_cast<uint8>(selected->spendingStyle);
}
}  // namespace

std::vector<PlayerbotCareerCandidate> PlayerbotCareer::BuildCandidates(
    PlayerbotPersonalityProfile const& profile, std::vector<PlayerbotCareerCandidateSeed> const& seeds,
    uint32 maxPrimarySkills)
{
    std::vector<PlayerbotCareerCandidate> candidates = {NoProfessionCandidate(profile)};

    if (profile.craftingAffinity < ACTIVE_AFFINITY_MINIMUM && profile.gatheringAffinity < ACTIVE_AFFINITY_MINIMUM)
        return candidates;

    for (PlayerbotCareerCandidateSeed const& seed : seeds)
    {
        if (!HasValidSkills(seed, maxPrimarySkills) || !MatchesActiveAffinities(seed, profile))
            continue;

        candidates.push_back({CandidateToken(seed), seed.summary, seed.primarySkills, seed.secondarySkills,
                              SpendingStyle(seed, profile),
                              profile.gatheringAffinity >= ACTIVE_AFFINITY_MINIMUM ||
                                  profile.craftingAffinity >= PROGRESSION_CRAFTING_MINIMUM,
                              CareerEngagement(seed, profile), CandidateWeight(seed, profile)});
    }

    return candidates;
}

PlayerbotCareerCandidate PlayerbotCareer::SelectFallback(std::vector<PlayerbotCareerCandidate> const& candidates,
                                                         uint64 guidCounter)
{
    if (candidates.empty())
        return {};

    uint64 totalWeight = 0;
    for (PlayerbotCareerCandidate const& candidate : candidates)
        totalWeight += candidate.weight;

    if (!totalWeight)
        return candidates.front();

    uint64 roll = PlayerbotPersonality::SplitMix64(guidCounter ^ CAREER_FALLBACK_NAMESPACE) % totalWeight;
    for (PlayerbotCareerCandidate const& candidate : candidates)
    {
        if (roll < candidate.weight)
            return candidate;

        roll -= candidate.weight;
    }

    return candidates.back();
}

PlayerbotCareerPlan PlayerbotCareer::MakePlan(uint64 botGuid, PlayerbotCareerCandidate const& candidate,
                                              PlayerbotRecipeSpendingStyle spendingStyle)
{
    return {PLAYERBOT_PERSONALITY_API_VERSION,
            PLAYERBOT_CAREER_PLAN_VERSION,
            botGuid,
            candidate.token,
            candidate.primarySkills,
            candidate.secondarySkills,
            spendingStyle,
            candidate.marketEligible && spendingStyle != PlayerbotRecipeSpendingStyle::None,
            candidate.engagement};
}

std::vector<uint16> PlayerbotCareer::EffectivePrimarySkills(PlayerbotCareerPlan const& plan)
{
    std::vector<uint16> skills = plan.primarySkills;
    skills.insert(skills.end(), plan.primarySkillAmendments.begin(), plan.primarySkillAmendments.end());
    return skills;
}

std::vector<uint16> PlayerbotCareer::PlannedSkills(PlayerbotCareerPlan const& plan)
{
    std::vector<uint16> skills = EffectivePrimarySkills(plan);
    skills.insert(skills.end(), plan.secondarySkills.begin(), plan.secondarySkills.end());
    return skills;
}

bool PlayerbotCareer::PlansSkill(PlayerbotCareerPlan const& plan, uint16 skillId)
{
    return ContainsSkill(plan.primarySkills, skillId) || ContainsSkill(plan.primarySkillAmendments, skillId) ||
           ContainsSkill(plan.secondarySkills, skillId);
}

std::string PlayerbotCareer::SerializePlan(PlayerbotCareerPlan const& plan)
{
    uint32 goalKind = 0u;
    uint16 professionSkillId = 0u;
    uint32 recipeSpellId = 0u;
    uint32 outputItemId = 0u;
    if (plan.capabilityGoal)
    {
        goalKind = static_cast<uint32>(plan.capabilityGoal->kind);
        professionSkillId = plan.capabilityGoal->professionSkillId;
        recipeSpellId = plan.capabilityGoal->recipeSpellId;
        outputItemId = plan.capabilityGoal->outputItemId;
    }

    std::ostringstream serialized;
    serialized << plan.personalityVersion << '|' << plan.careerVersion << '|' << plan.botGuid << '|'
               << plan.candidateToken << '|' << static_cast<uint32>(plan.spendingStyle) << '|'
               << static_cast<uint32>(plan.marketEligible) << '|' << static_cast<uint32>(plan.engagement) << '|'
               << SerializeSkills(plan.primarySkills) << '|' << SerializeSkills(plan.secondarySkills) << '|' << goalKind
               << '|' << professionSkillId << '|' << recipeSpellId << '|' << outputItemId << '|'
               << SerializeSkills(plan.primarySkillAmendments);
    return serialized.str();
}

std::optional<PlayerbotCareerPlan> PlayerbotCareer::DeserializePlan(std::string const& serialized,
                                                                    uint64 expectedBotGuid)
{
    std::vector<std::string_view> const parts = Split(serialized, '|');
    if (parts.size() != 13u && parts.size() != 14u)
        return std::nullopt;

    PlayerbotCareerPlan plan;
    uint32 spendingStyle = 0;
    uint8 marketEligible = 0;
    uint8 goalKind = 0;
    uint16 professionSkillId = 0;
    uint32 recipeSpellId = 0;
    uint32 outputItemId = 0;
    if (!ParseUnsigned(parts[0], plan.personalityVersion) || !ParseUnsigned(parts[1], plan.careerVersion) ||
        !ParseUnsigned(parts[2], plan.botGuid) || !ParseUnsigned(parts[4], spendingStyle) ||
        !ParseUnsigned(parts[5], marketEligible) || !ParseUnsigned(parts[6], plan.engagement) ||
        !ParseSkills(parts[7], plan.primarySkills) || !ParseSkills(parts[8], plan.secondarySkills) ||
        !ParseUnsigned(parts[9], goalKind) || !ParseUnsigned(parts[10], professionSkillId) ||
        !ParseUnsigned(parts[11], recipeSpellId) || !ParseUnsigned(parts[12], outputItemId) ||
        (parts.size() == 14u && !ParseSkills(parts[13], plan.primarySkillAmendments)))
        return std::nullopt;

    plan.candidateToken = parts[3];
    plan.spendingStyle = static_cast<PlayerbotRecipeSpendingStyle>(spendingStyle);
    plan.marketEligible = marketEligible == 1u;
    if (goalKind)
    {
        plan.capabilityGoal = PlayerbotCareerCapabilityGoal{static_cast<PlayerbotCareerCapabilityGoalKind>(goalKind),
                                                            professionSkillId, recipeSpellId, outputItemId};
    }
    else if (professionSkillId || recipeSpellId || outputItemId)
        return std::nullopt;

    if (parts.size() == 13u && plan.capabilityGoal &&
        plan.capabilityGoal->kind == PlayerbotCareerCapabilityGoalKind::Trainer)
    {
        if (plan.primarySkills.size() < 2u)
            plan.primarySkillAmendments.push_back(plan.capabilityGoal->professionSkillId);
        else
            plan.capabilityGoal.reset();
    }

    if (plan.personalityVersion != PLAYERBOT_PERSONALITY_API_VERSION ||
        plan.careerVersion != PLAYERBOT_CAREER_PLAN_VERSION || plan.botGuid != expectedBotGuid || marketEligible > 1u ||
        !IsValidSpendingStyle(plan.spendingStyle) || plan.engagement > 100u ||
        plan.candidateToken.find("career-") != 0u)
        return std::nullopt;

    if (!IsPlanStateValid(plan))
        return std::nullopt;

    return plan;
}

std::optional<PlayerbotCareerPlan> PlayerbotCareer::DeserializePlan(
    std::string const& serialized, uint64 expectedBotGuid, std::vector<PlayerbotCareerCandidate> const& legalCandidates)
{
    std::optional<PlayerbotCareerPlan> parsed = DeserializePlan(serialized, expectedBotGuid);
    if (!parsed)
        return std::nullopt;

    PlayerbotCareerPlan const& plan = *parsed;
    PlayerbotCareerCandidate const* candidate = FindCandidate(legalCandidates, plan.candidateToken);
    if (!candidate || plan.primarySkills != candidate->primarySkills ||
        plan.secondarySkills != candidate->secondarySkills ||
        plan.marketEligible !=
            (candidate->marketEligible && plan.spendingStyle != PlayerbotRecipeSpendingStyle::None) ||
        plan.engagement != candidate->engagement ||
        static_cast<uint8>(plan.spendingStyle) > static_cast<uint8>(candidate->spendingStyle))
        return std::nullopt;

    return parsed;
}

std::optional<PlayerbotCareerPlan> PlayerbotCareer::DeserializePlan(
    std::string const& serialized, uint64 expectedBotGuid, std::vector<PlayerbotCareerCandidate> const& legalCandidates,
    std::vector<uint16> const& primaryProfessionSkillIds)
{
    std::optional<PlayerbotCareerPlan> parsed = DeserializePlan(serialized, expectedBotGuid, legalCandidates);
    if (!parsed || !IsPlanStateValid(*parsed, &primaryProfessionSkillIds))
        return std::nullopt;

    return parsed;
}

std::optional<PlayerbotCareerPlan> PlayerbotCareer::DeserializePlan(
    std::string const& serialized, uint64 expectedBotGuid, std::vector<PlayerbotCareerCandidate> const& legalCandidates,
    std::vector<uint16> const& primaryProfessionSkillIds, std::vector<uint16> const& learnedPrimaryProfessionSkillIds)
{
    std::optional<PlayerbotCareerPlan> parsed = DeserializePlan(serialized, expectedBotGuid, legalCandidates);
    if (!parsed)
        return std::nullopt;

    if (parsed->capabilityGoal && parsed->capabilityGoal->kind == PlayerbotCareerCapabilityGoalKind::Trainer &&
        ContainsSkill(learnedPrimaryProfessionSkillIds, parsed->capabilityGoal->professionSkillId))
    {
        parsed->capabilityGoal.reset();
    }

    if (!IsPlanStateValid(*parsed, &primaryProfessionSkillIds, &learnedPrimaryProfessionSkillIds))
    {
        return std::nullopt;
    }

    return parsed;
}

bool PlayerbotCareer::TryAssignCapabilityGoal(PlayerbotCareerPlan& plan, PlayerbotCareerCapabilityGoal const& goal,
                                              std::vector<uint16> const& primaryProfessionSkillIds,
                                              std::vector<uint16> const& learnedPrimaryProfessionSkillIds)
{
    if (plan.capabilityGoal)
        return false;

    PlayerbotCareerPlan updated = plan;
    updated.capabilityGoal = goal;
    if (goal.kind == PlayerbotCareerCapabilityGoalKind::Trainer &&
        !ContainsSkill(updated.primarySkillAmendments, goal.professionSkillId))
    {
        updated.primarySkillAmendments.push_back(goal.professionSkillId);
    }
    if (!IsPlanStateValid(updated, &primaryProfessionSkillIds, &learnedPrimaryProfessionSkillIds))
        return false;

    plan = std::move(updated);
    return true;
}

bool PlayerbotCareer::ClearCapabilityGoal(PlayerbotCareerPlan& plan)
{
    if (!plan.capabilityGoal)
        return false;

    plan.capabilityGoal.reset();
    return true;
}

PlayerbotCareerAcquisition PlayerbotCareer::SelectTrainerObjective(PlayerbotCareerPlan const& plan,
                                                                   std::vector<uint16> const& learnedSkillIds,
                                                                   std::vector<uint16> const& primaryProfessionSkillIds,
                                                                   uint8 freePrimaryProfessionSlots)
{
    std::optional<PlayerbotCareerTrainerObjective> blockedPrimary;
    for (uint16 skillId : EffectivePrimarySkills(plan))
    {
        bool const activeTrainerRemediation =
            plan.capabilityGoal && plan.capabilityGoal->kind == PlayerbotCareerCapabilityGoalKind::Trainer &&
            plan.capabilityGoal->professionSkillId == skillId;
        if (activeTrainerRemediation)
            continue;
        if (ContainsSkill(learnedSkillIds, skillId))
            continue;

        PlayerbotCareerTrainerObjective const objective = {PlayerbotCareerTrainerObjectiveKind::BaseCareer, skillId,
                                                           true};
        if (freePrimaryProfessionSlots)
            return {objective, PlayerbotCareerAcquisitionState::Travel, PlayerbotCareerAcquisitionBlocker::None};
        if (!blockedPrimary)
            blockedPrimary = objective;
    }

    for (uint16 skillId : plan.secondarySkills)
    {
        if (!ContainsSkill(learnedSkillIds, skillId))
        {
            PlayerbotCareerTrainerObjective const objective = {PlayerbotCareerTrainerObjectiveKind::BaseCareer, skillId,
                                                               false};
            return {objective, PlayerbotCareerAcquisitionState::Travel, PlayerbotCareerAcquisitionBlocker::None};
        }
    }

    if (blockedPrimary)
    {
        return {*blockedPrimary, PlayerbotCareerAcquisitionState::Blocked,
                PlayerbotCareerAcquisitionBlocker::PrimarySlotsOccupied};
    }

    if (plan.capabilityGoal && plan.capabilityGoal->kind == PlayerbotCareerCapabilityGoalKind::Trainer &&
        !ContainsSkill(learnedSkillIds, plan.capabilityGoal->professionSkillId))
    {
        bool const primary = ContainsSkill(primaryProfessionSkillIds, plan.capabilityGoal->professionSkillId);
        PlayerbotCareerTrainerObjective const objective = {PlayerbotCareerTrainerObjectiveKind::CapabilityRemediation,
                                                           plan.capabilityGoal->professionSkillId, primary};
        if (!primary || freePrimaryProfessionSlots)
            return {objective, PlayerbotCareerAcquisitionState::Travel, PlayerbotCareerAcquisitionBlocker::None};
        return {objective, PlayerbotCareerAcquisitionState::Blocked,
                PlayerbotCareerAcquisitionBlocker::PrimarySlotsOccupied};
    }

    return {};
}

PlayerbotCareerAcquisition PlayerbotCareer::EvaluateTrainerObjective(PlayerbotCareerTrainerObjective const& objective,
                                                                     PlayerbotCareerAcquisitionFacts const& facts)
{
    if (facts.professionLearned)
        return {objective, PlayerbotCareerAcquisitionState::Complete, PlayerbotCareerAcquisitionBlocker::None};
    if (!facts.trainerAvailable)
    {
        return {objective, PlayerbotCareerAcquisitionState::Blocked,
                PlayerbotCareerAcquisitionBlocker::TrainerUnavailable};
    }
    if (!facts.routeSafe)
        return {objective, PlayerbotCareerAcquisitionState::Blocked, PlayerbotCareerAcquisitionBlocker::UnsafeRoute};
    if (!facts.trainerEligible)
    {
        return {objective, PlayerbotCareerAcquisitionState::Blocked,
                PlayerbotCareerAcquisitionBlocker::TrainerIneligible};
    }
    if (!facts.affordable)
    {
        return {objective, PlayerbotCareerAcquisitionState::Blocked,
                PlayerbotCareerAcquisitionBlocker::InsufficientProtectedMoney};
    }
    if (!facts.atTrainer)
        return {objective, PlayerbotCareerAcquisitionState::Travel, PlayerbotCareerAcquisitionBlocker::None};
    if (!facts.lessonAttempted)
        return {objective, PlayerbotCareerAcquisitionState::Learn, PlayerbotCareerAcquisitionBlocker::None};
    return {objective, PlayerbotCareerAcquisitionState::Blocked,
            PlayerbotCareerAcquisitionBlocker::CompletionUnobserved};
}

char const* PlayerbotCareer::AcquisitionBlockerCode(PlayerbotCareerAcquisitionBlocker blocker)
{
    switch (blocker)
    {
        case PlayerbotCareerAcquisitionBlocker::None:
            return "";
        case PlayerbotCareerAcquisitionBlocker::PrimarySlotsOccupied:
            return "primary_slots_occupied";
        case PlayerbotCareerAcquisitionBlocker::TrainerUnavailable:
            return "trainer_unavailable";
        case PlayerbotCareerAcquisitionBlocker::UnsafeRoute:
            return "unsafe_route";
        case PlayerbotCareerAcquisitionBlocker::TrainerIneligible:
            return "trainer_ineligible";
        case PlayerbotCareerAcquisitionBlocker::InsufficientProtectedMoney:
            return "insufficient_protected_money";
        case PlayerbotCareerAcquisitionBlocker::CompletionUnobserved:
            return "completion_unobserved";
    }

    return "unknown";
}

bool PlayerbotCareer::RegisterProvider(PlayerbotCareerPlanProvider* provider)
{
    if (!provider || careerProvider)
        return false;

    careerProvider = provider;
    return true;
}

void PlayerbotCareer::UnregisterProvider(PlayerbotCareerPlanProvider* provider)
{
    if (careerProvider != provider)
        return;

    careerProvider = nullptr;
    pendingCareerRequests.clear();
}

PlayerbotCareerPlanResolution PlayerbotCareer::ResolvePlan(uint64 botGuid, PlayerbotPersonalityProfile const& profile,
                                                           std::vector<PlayerbotCareerCandidate> const& candidates,
                                                           uint64 nowMs)
{
    auto pending = pendingCareerRequests.find(botGuid);
    if (pending != pendingCareerRequests.end())
    {
        if (!careerProvider)
        {
            pendingCareerRequests.erase(pending);
            return FallbackResolution(botGuid, candidates);
        }

        if (std::optional<PlayerbotCareerPlanResponse> response = careerProvider->Poll(pending->second.requestId))
        {
            PendingCareerRequest const request = pending->second;
            pendingCareerRequests.erase(pending);

            PlayerbotCareerCandidate const* selected = nullptr;
            if (!IsValidResponse(*response, request, botGuid, candidates, selected))
                return FallbackResolution(botGuid, candidates);

            return {PlayerbotCareerPlanResolutionStatus::Resolved,
                    MakePlan(botGuid, *selected, response->spendingStyle)};
        }

        if (nowMs - pending->second.startedAtMs >= pending->second.responseDeadlineMs)
        {
            pendingCareerRequests.erase(pending);
            return FallbackResolution(botGuid, candidates);
        }

        return {};
    }

    if (!careerProvider)
        return FallbackResolution(botGuid, candidates);

    PlayerbotCareerPlanRequest request;
    request.requestId = nextCareerRequestId++;
    request.botGuid = botGuid;
    request.profile = profile;
    request.candidates.reserve(candidates.size());
    for (PlayerbotCareerCandidate const& candidate : candidates)
    {
        request.candidates.push_back({candidate.token, candidate.summary, candidate.spendingStyle,
                                      candidate.marketEligible, candidate.engagement});
    }

    if (!careerProvider->TrySubmit(request))
        return FallbackResolution(botGuid, candidates);

    pendingCareerRequests.emplace(
        botGuid, PendingCareerRequest{request.requestId, nowMs, careerProvider->ResponseDeadlineMs(), candidates});
    return {};
}

PlayerbotCareerPlanRecovery PlayerbotCareer::ResolvePersistedPlan(
    std::optional<std::string> const& serialized, uint64 botGuid, PlayerbotPersonalityProfile const& profile,
    std::vector<PlayerbotCareerCandidate> const& candidates, uint64 nowMs)
{
    if (serialized)
    {
        std::optional<PlayerbotCareerPlan> const loaded = DeserializePlan(*serialized, botGuid, candidates);
        if (loaded)
            return {PlayerbotCareerPlanResolutionStatus::Resolved, *loaded, SerializePlan(*loaded) != *serialized};
    }

    PlayerbotCareerPlanResolution const resolution = ResolvePlan(botGuid, profile, candidates, nowMs);
    return {resolution.status, resolution.plan, resolution.status == PlayerbotCareerPlanResolutionStatus::Resolved};
}

PlayerbotCareerPlanRecovery PlayerbotCareer::ResolvePersistedPlan(
    std::optional<std::string> const& serialized, uint64 botGuid, PlayerbotPersonalityProfile const& profile,
    std::vector<PlayerbotCareerCandidate> const& candidates, std::vector<uint16> const& primaryProfessionSkillIds,
    uint64 nowMs)
{
    if (serialized)
    {
        std::optional<PlayerbotCareerPlan> const loaded =
            DeserializePlan(*serialized, botGuid, candidates, primaryProfessionSkillIds);
        if (loaded)
            return {PlayerbotCareerPlanResolutionStatus::Resolved, *loaded, SerializePlan(*loaded) != *serialized};
    }

    PlayerbotCareerPlanResolution const resolution = ResolvePlan(botGuid, profile, candidates, nowMs);
    return {resolution.status, resolution.plan, resolution.status == PlayerbotCareerPlanResolutionStatus::Resolved};
}

PlayerbotCareerPlanRecovery PlayerbotCareer::ResolvePersistedPlan(
    std::optional<std::string> const& serialized, uint64 botGuid, PlayerbotPersonalityProfile const& profile,
    std::vector<PlayerbotCareerCandidate> const& candidates, std::vector<uint16> const& primaryProfessionSkillIds,
    std::vector<uint16> const& learnedPrimaryProfessionSkillIds, uint64 nowMs)
{
    if (serialized)
    {
        std::optional<PlayerbotCareerPlan> const loaded = DeserializePlan(
            *serialized, botGuid, candidates, primaryProfessionSkillIds, learnedPrimaryProfessionSkillIds);
        if (loaded)
            return {PlayerbotCareerPlanResolutionStatus::Resolved, *loaded, SerializePlan(*loaded) != *serialized};
    }

    PlayerbotCareerPlanResolution const resolution = ResolvePlan(botGuid, profile, candidates, nowMs);
    return {resolution.status, resolution.plan, resolution.status == PlayerbotCareerPlanResolutionStatus::Resolved};
}

std::vector<PlayerbotCareerPlan> PlayerbotCareer::PollPendingPlans(uint64 nowMs)
{
    std::vector<PlayerbotCareerPlan> plans;
    for (auto pending = pendingCareerRequests.begin(); pending != pendingCareerRequests.end();)
    {
        uint64 const botGuid = pending->first;
        PendingCareerRequest const& request = pending->second;
        std::optional<PlayerbotCareerPlanResponse> response =
            careerProvider ? careerProvider->Poll(request.requestId) : std::nullopt;

        PlayerbotCareerCandidate const* selected = nullptr;
        if (response && IsValidResponse(*response, request, botGuid, request.candidates, selected))
        {
            plans.push_back(MakePlan(botGuid, *selected, response->spendingStyle));
            pending = pendingCareerRequests.erase(pending);
            continue;
        }

        bool const invalidResponse = response.has_value();
        bool const timedOut = nowMs - request.startedAtMs >= request.responseDeadlineMs;
        if (!careerProvider || invalidResponse || timedOut)
        {
            plans.push_back(FallbackResolution(botGuid, request.candidates).plan);
            pending = pendingCareerRequests.erase(pending);
            continue;
        }

        ++pending;
    }

    return plans;
}

std::vector<uint32> PlayerbotCareer::SelectTrainerLessons(PlayerbotCareerPlan const& plan,
                                                          std::vector<PlayerbotTrainerLessonCandidate> const& lessons)
{
    bool const hasTrainerGoal =
        plan.capabilityGoal && plan.capabilityGoal->kind == PlayerbotCareerCapabilityGoalKind::Trainer;
    if (plan.spendingStyle == PlayerbotRecipeSpendingStyle::None && !hasTrainerGoal)
        return {};

    std::vector<uint16> const careerSkills = PlannedSkills(plan);
    std::unordered_set<uint16> plannedSkills(careerSkills.begin(), careerSkills.end());

    std::unordered_map<uint16, PlayerbotTrainerLessonCandidate const*> cheapestProgressionRecipe;
    if (plan.spendingStyle == PlayerbotRecipeSpendingStyle::Minimal)
    {
        for (PlayerbotTrainerLessonCandidate const& lesson : lessons)
        {
            if (lesson.isRank || !lesson.canRaiseSkill || !plannedSkills.contains(lesson.skillId))
                continue;

            auto current = cheapestProgressionRecipe.find(lesson.skillId);
            if (current == cheapestProgressionRecipe.end() || lesson.cost < current->second->cost)
                cheapestProgressionRecipe[lesson.skillId] = &lesson;
        }
    }

    std::vector<uint32> selected;
    for (PlayerbotTrainerLessonCandidate const& lesson : lessons)
    {
        if (hasTrainerGoal && lesson.skillId == plan.capabilityGoal->professionSkillId)
        {
            if (lesson.isRank)
                selected.push_back(lesson.spellId);
            continue;
        }

        if (plan.spendingStyle == PlayerbotRecipeSpendingStyle::None)
            continue;

        if (!plannedSkills.contains(lesson.skillId))
            continue;

        bool include = lesson.isRank;
        switch (plan.spendingStyle)
        {
            case PlayerbotRecipeSpendingStyle::Completionist:
                include = true;
                break;
            case PlayerbotRecipeSpendingStyle::Progression:
                include |= lesson.canRaiseSkill;
                break;
            case PlayerbotRecipeSpendingStyle::Minimal:
            {
                auto const cheapest = cheapestProgressionRecipe.find(lesson.skillId);
                include |= lesson.canRaiseSkill && cheapest != cheapestProgressionRecipe.end() &&
                           cheapest->second->spellId == lesson.spellId;
                break;
            }
            case PlayerbotRecipeSpendingStyle::None:
                break;
        }

        if (include)
            selected.push_back(lesson.spellId);
    }

    return selected;
}

std::vector<uint32> PlayerbotCareer::SelectTrainerLessons(PlayerbotCareerTrainerObjective const& objective,
                                                          std::vector<PlayerbotTrainerLessonCandidate> const& lessons)
{
    if (objective.kind == PlayerbotCareerTrainerObjectiveKind::Progression && !objective.rankOnly)
    {
        PlayerbotTrainerLessonCandidate const* cheapest = nullptr;
        for (PlayerbotTrainerLessonCandidate const& lesson : lessons)
        {
            if (lesson.skillId != objective.professionSkillId || lesson.isRank || !lesson.canRaiseSkill)
                continue;
            if (!cheapest || lesson.cost < cheapest->cost)
                cheapest = &lesson;
        }
        return cheapest ? std::vector<uint32>{cheapest->spellId} : std::vector<uint32>{};
    }

    std::vector<uint32> selected;
    for (PlayerbotTrainerLessonCandidate const& lesson : lessons)
    {
        if (lesson.skillId == objective.professionSkillId && lesson.isRank)
            selected.push_back(lesson.spellId);
    }
    return selected;
}

bool PlayerbotCareer::HasAffordableTrainerLesson(PlayerbotCareerPlan const& plan,
                                                 std::vector<PlayerbotTrainerLessonCandidate> const& lessons,
                                                 uint32 availableMoney)
{
    std::vector<uint32> const selectedLessons = SelectTrainerLessons(plan, lessons);
    std::unordered_set<uint32> const selected(selectedLessons.begin(), selectedLessons.end());

    return std::any_of(lessons.begin(), lessons.end(),
                       [&selected, availableMoney](PlayerbotTrainerLessonCandidate const& lesson)
                       { return selected.contains(lesson.spellId) && lesson.cost <= availableMoney; });
}

bool PlayerbotCareer::HasAffordableTrainerLesson(PlayerbotCareerTrainerObjective const& objective,
                                                 std::vector<PlayerbotTrainerLessonCandidate> const& lessons,
                                                 uint32 availableMoney)
{
    std::vector<uint32> const selectedLessons = SelectTrainerLessons(objective, lessons);
    std::unordered_set<uint32> const selected(selectedLessons.begin(), selectedLessons.end());

    return std::any_of(lessons.begin(), lessons.end(),
                       [&selected, availableMoney](PlayerbotTrainerLessonCandidate const& lesson)
                       { return selected.contains(lesson.spellId) && lesson.cost <= availableMoney; });
}

bool PlayerbotCareer::IsTrainerDestinationSafe(std::uint8_t botLevel, std::uint32_t botZoneId,
                                               std::uint32_t trainerZoneId, std::uint32_t trainerMinimumLevel)
{
    if (botLevel <= 5u && trainerZoneId != botZoneId)
        return false;

    std::uint32_t const effectiveLevel = std::max<std::uint32_t>(botLevel, 5u);
    return trainerMinimumLevel == 0u || trainerMinimumLevel <= effectiveLevel;
}

bool PlayerbotCareer::SchedulesProfessionWork(PlayerbotCareerPlan const& plan)
{
    return plan.capabilityGoal.has_value() || !PlannedSkills(plan).empty();
}

uint32 PlayerbotCareer::ProfessionWorkWeight(PlayerbotCareerPlan const& plan, uint32 baseWeight)
{
    if (!baseWeight || !SchedulesProfessionWork(plan))
        return 0u;
    if (plan.capabilityGoal)
        return baseWeight;

    // Engagement scales how often profession work outranks the other autonomous activities. It
    // never disables an active career, so the lowest legal engagement still keeps a unit of weight.
    uint32 const engagement = std::min<uint32>(plan.engagement, 100u);
    return std::max(1u, baseWeight * engagement / 100u);
}

bool PlayerbotCareer::TrainerOffersCareerLesson(PlayerbotCareerPlan const& plan, Player const* bot,
                                                Trainer::Trainer const* trainer, float reputationDiscount,
                                                uint32 availableMoney)
{
    if (!bot || !trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill ||
        !trainer->IsTrainerValidForPlayer(bot))
        return false;

    std::vector<PlayerbotTrainerLessonCandidate> lessons;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
        if (!trainerSpell || !trainer->CanTeachSpell(bot, trainerSpell))
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(trainerSpell->SpellId);
        if (!spellInfo)
            continue;

        uint32 const cost = static_cast<uint32>(std::floor(trainerSpell->MoneyCost * reputationDiscount));
        lessons.push_back(DescribeTrainerLesson(*trainerSpell, spellInfo, bot, cost));
    }

    return HasAffordableTrainerLesson(plan, lessons, availableMoney);
}

bool PlayerbotCareer::TrainerOffersCareerLesson(PlayerbotCareerTrainerObjective const& objective, Player const* bot,
                                                Trainer::Trainer const* trainer, float reputationDiscount,
                                                uint32 availableMoney)
{
    if (!bot || !trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill ||
        !trainer->IsTrainerValidForPlayer(bot))
        return false;

    std::vector<PlayerbotTrainerLessonCandidate> lessons;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
        if (!trainerSpell || !trainer->CanTeachSpell(bot, trainerSpell))
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(trainerSpell->SpellId);
        if (!spellInfo)
            continue;

        uint32 const cost = static_cast<uint32>(std::floor(trainerSpell->MoneyCost * reputationDiscount));
        lessons.push_back(DescribeTrainerLesson(*trainerSpell, spellInfo, bot, cost));
    }

    return HasAffordableTrainerLesson(objective, lessons, availableMoney);
}

PlayerbotTrainerLessonCandidate PlayerbotCareer::DescribeTrainerLesson(Trainer::Spell const& trainerSpell,
                                                                       SpellInfo const* spellInfo, Player const* bot,
                                                                       uint32 cost)
{
    PlayerbotTrainerLessonCandidate lesson{trainerSpell.SpellId, 0u, cost, false, false};
    auto const inspectSpell = [&lesson, bot](uint32 spellId)
    {
        if (SpellLearnSkillNode const* learnedSkill = sSpellMgr->GetSpellLearnSkill(spellId))
        {
            lesson.skillId = learnedSkill->skill;
            lesson.isRank = true;
            lesson.canRaiseSkill = true;
        }

        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto ability = bounds.first; ability != bounds.second; ++ability)
        {
            SkillLineAbilityEntry const* skill = ability->second;
            if (!skill || !skill->SkillLine)
                continue;

            lesson.skillId = static_cast<uint16>(skill->SkillLine);
            lesson.canRaiseSkill |= bot->GetPureSkillValue(skill->SkillLine) < skill->TrivialSkillLineRankHigh;
        }
    };

    if (trainerSpell.ReqSkillLine)
        lesson.skillId = static_cast<uint16>(trainerSpell.ReqSkillLine);
    inspectSpell(trainerSpell.SpellId);
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
            inspectSpell(effect.TriggerSpell);

    return lesson;
}

bool PlayerbotCareer::IsRecipeAcquisitionAllowed(PlayerbotCareerPlan const& plan,
                                                 PlayerbotRecipeCandidate const& recipe, PlayerbotRecipeSource source)
{
    if (recipe.isKnown || !recipe.isUsable)
        return false;

    if (plan.capabilityGoal && plan.capabilityGoal->kind == PlayerbotCareerCapabilityGoalKind::Recipe)
    {
        return recipe.skillId == plan.capabilityGoal->professionSkillId &&
               recipe.recipeSpellId == plan.capabilityGoal->recipeSpellId &&
               recipe.outputItemId == plan.capabilityGoal->outputItemId;
    }

    if (plan.spendingStyle == PlayerbotRecipeSpendingStyle::None)
        return false;

    if (!PlansSkill(plan, recipe.skillId))
        return false;

    return plan.spendingStyle == PlayerbotRecipeSpendingStyle::Completionist || recipe.canRaiseSkill;
}

std::vector<uint32> PlayerbotCareer::SelectRecipePurchases(PlayerbotCareerPlan const& plan,
                                                           std::vector<PlayerbotRecipeCandidate> const& recipes,
                                                           PlayerbotRecipeSource source)
{
    std::vector<uint32> selected;
    std::unordered_map<uint16, PlayerbotRecipeCandidate const*> cheapestBySkill;
    if (plan.spendingStyle == PlayerbotRecipeSpendingStyle::Minimal)
    {
        for (PlayerbotRecipeCandidate const& recipe : recipes)
        {
            if (!IsRecipeAcquisitionAllowed(plan, recipe, source))
                continue;

            auto current = cheapestBySkill.find(recipe.skillId);
            if (current == cheapestBySkill.end() || recipe.cost < current->second->cost)
                cheapestBySkill[recipe.skillId] = &recipe;
        }
    }

    for (PlayerbotRecipeCandidate const& recipe : recipes)
    {
        if (!IsRecipeAcquisitionAllowed(plan, recipe, source))
            continue;

        if (plan.spendingStyle == PlayerbotRecipeSpendingStyle::Minimal && cheapestBySkill[recipe.skillId] != &recipe)
            continue;

        selected.push_back(recipe.itemId);
    }

    return selected;
}

PlayerbotRecipeCandidate PlayerbotCareer::DescribeRecipe(ItemTemplate const* recipe, Player const* bot, uint32 cost)
{
    PlayerbotRecipeCandidate candidate;
    if (!recipe || !bot)
        return candidate;
    candidate.itemId = recipe->ItemId;
    candidate.skillId = RecipeSkillId(recipe);
    candidate.cost = cost;

    uint32 const learnedSpellId = LearnedRecipeSpellId(recipe);
    candidate.recipeSpellId = learnedSpellId;
    candidate.outputItemId = CreatedItemId(learnedSpellId);
    candidate.isKnown = learnedSpellId && bot->HasSpell(learnedSpellId);
    candidate.isUsable = candidate.skillId && learnedSpellId && bot->BotCanUseItem(recipe) == EQUIP_ERR_OK;
    if (!candidate.isUsable || candidate.isKnown)
        return candidate;

    SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(learnedSpellId);
    for (auto ability = bounds.first; ability != bounds.second; ++ability)
    {
        SkillLineAbilityEntry const* skill = ability->second;
        if (!skill || skill->SkillLine != candidate.skillId)
            continue;

        candidate.canRaiseSkill |= bot->GetPureSkillValue(candidate.skillId) < skill->TrivialSkillLineRankHigh;
    }

    return candidate;
}
