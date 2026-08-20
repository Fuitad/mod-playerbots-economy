/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotCareerAdapter.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "Bot/Personality/PlayerbotPersonalityMgr.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"
#include "SpellMgr.h"
#include "World.h"

namespace
{
constexpr char CAREER_PLAN_EVENT[] = "career plan";

struct WeightedProfessionPair
{
    uint16 firstSkill;
    uint16 secondSkill;
    uint32 weight;
};

bool IsGatheringSkill(uint16 skillId)
{
    return skillId == SKILL_HERBALISM || skillId == SKILL_MINING || skillId == SKILL_SKINNING;
}

bool IsCraftingSkill(uint16 skillId) { return IsPrimaryProfessionSkill(skillId) && !IsGatheringSkill(skillId); }

std::vector<WeightedProfessionPair> ClassProfessionPairs(Player const* bot)
{
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            return {{SKILL_MINING, SKILL_BLACKSMITHING, 45},
                    {SKILL_MINING, SKILL_ENGINEERING, 30},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 15},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 10}};
        case CLASS_PALADIN:
            return {{SKILL_MINING, SKILL_BLACKSMITHING, 45},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 30},
                    {SKILL_MINING, SKILL_ENGINEERING, 15},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 10}};
        case CLASS_DEATH_KNIGHT:
            return {{SKILL_MINING, SKILL_BLACKSMITHING, 45},
                    {SKILL_MINING, SKILL_ENGINEERING, 35},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 20}};
        case CLASS_HUNTER:
            return {{SKILL_SKINNING, SKILL_LEATHERWORKING, 45},
                    {SKILL_MINING, SKILL_ENGINEERING, 35},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 10},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 10}};
        case CLASS_ROGUE:
            return {{SKILL_SKINNING, SKILL_LEATHERWORKING, 35},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25},
                    {SKILL_MINING, SKILL_ENGINEERING, 25},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 10},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 5}};
        case CLASS_DRUID:
            return {{SKILL_SKINNING, SKILL_LEATHERWORKING, 35},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 35},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 20},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 10}};
        case CLASS_SHAMAN:
            return {{SKILL_HERBALISM, SKILL_ALCHEMY, 35},
                    {SKILL_SKINNING, SKILL_LEATHERWORKING, 25},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 25},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 15}};
        case CLASS_PRIEST:
            return {{SKILL_TAILORING, SKILL_ENCHANTING, 45},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 30},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25}};
        case CLASS_MAGE:
            return {{SKILL_TAILORING, SKILL_ENCHANTING, 50},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 25}};
        case CLASS_WARLOCK:
        default:
            return {{SKILL_TAILORING, SKILL_ENCHANTING, 50},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 25}};
    }
}

std::vector<WeightedProfessionPair> RandomProfessionPairs()
{
    return {{SKILL_MINING, SKILL_BLACKSMITHING, 20}, {SKILL_MINING, SKILL_ENGINEERING, 18},
            {SKILL_MINING, SKILL_JEWELCRAFTING, 16}, {SKILL_SKINNING, SKILL_LEATHERWORKING, 18},
            {SKILL_HERBALISM, SKILL_ALCHEMY, 18},    {SKILL_HERBALISM, SKILL_INSCRIPTION, 14},
            {SKILL_TAILORING, SKILL_ENCHANTING, 10}, {SKILL_HERBALISM, SKILL_MINING, 6},
            {SKILL_HERBALISM, SKILL_SKINNING, 5},    {SKILL_MINING, SKILL_SKINNING, 5}};
}

std::vector<uint16> const& PrimaryProfessionSkillIds()
{
    static std::vector<uint16> const skillIds = []
    {
        std::vector<uint16> result;
        for (uint32 skillId = 1; skillId < sSkillLineStore.GetNumRows(); ++skillId)
        {
            if (skillId <= std::numeric_limits<uint16>::max() && IsPrimaryProfessionSkill(skillId))
                result.push_back(static_cast<uint16>(skillId));
        }
        return result;
    }();
    return skillIds;
}

std::vector<uint16> LearnedPrimaryProfessionSkillIds(Player const* bot)
{
    std::vector<uint16> learned;
    for (uint16 skillId : PrimaryProfessionSkillIds())
    {
        if (bot->HasSkill(skillId))
            learned.push_back(skillId);
    }
    return learned;
}

std::vector<PlayerbotCareerCandidate> BuildCareerCandidates(Player const* bot,
                                                            PlayerbotPersonalityProfile const& personality)
{
    uint32 const maxPrimarySkills = std::min<uint32>(2, sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL));
    uint32 const classWeight = sPlayerbotEconomyConfig.classMatchingProfessionChance;
    uint32 const randomWeight = 100u - classWeight;

    std::map<std::pair<uint16, uint16>, uint32> weightedPairs;
    auto const addPairs = [&weightedPairs](std::vector<WeightedProfessionPair> const& pairs, uint32 poolWeight)
    {
        if (!poolWeight)
            return;
        for (WeightedProfessionPair const& pair : pairs)
            weightedPairs[{pair.firstSkill, pair.secondSkill}] += pair.weight * poolWeight;
    };
    addPairs(ClassProfessionPairs(bot), classWeight);
    addPairs(RandomProfessionPairs(), randomWeight);

    std::map<uint16, uint32> weightedSkills;
    std::vector<PlayerbotCareerCandidateSeed> primarySeeds;
    for (auto const& [skills, weight] : weightedPairs)
    {
        weightedSkills[skills.first] += weight;
        weightedSkills[skills.second] += weight;
        if (maxPrimarySkills < 2u)
            continue;

        bool const hasCrafting = IsCraftingSkill(skills.first) || IsCraftingSkill(skills.second);
        bool const hasGathering = IsGatheringSkill(skills.first) || IsGatheringSkill(skills.second);
        primarySeeds.push_back(
            {{skills.first, skills.second},
             {},
             hasCrafting,
             hasGathering,
             weight,
             hasCrafting && hasGathering ? "mixed primary professions" : "complementary primary professions"});
    }

    if (maxPrimarySkills)
    {
        for (auto const& [skill, weight] : weightedSkills)
        {
            bool const hasCrafting = IsCraftingSkill(skill);
            primarySeeds.push_back({{skill},
                                    {},
                                    hasCrafting,
                                    !hasCrafting,
                                    weight,
                                    hasCrafting ? "single crafting profession" : "single gathering profession"});
        }
    }

    // A primary slot cannot be reclaimed, so a seed proposing professions the bot could never fit is
    // unreachable and would strand its career on a trainer objective it can never satisfy. Rewrite
    // every seed onto what the bot can actually reach, merging the duplicates that collapses.
    std::vector<uint16> const learnedPrimaries = LearnedPrimaryProfessionSkillIds(bot);
    if (!learnedPrimaries.empty())
    {
        std::vector<PlayerbotCareerCandidateSeed> reachableSeeds;
        for (PlayerbotCareerCandidateSeed const& seed : primarySeeds)
        {
            PlayerbotCareerCandidateSeed reachable = seed;
            reachable.primarySkills =
                PlayerbotCareer::AchievablePrimarySkills(seed.primarySkills, learnedPrimaries, maxPrimarySkills);
            reachable.hasCrafting = std::any_of(reachable.primarySkills.begin(), reachable.primarySkills.end(),
                                                [](uint16 skillId) { return IsCraftingSkill(skillId); });
            reachable.hasGathering = std::any_of(reachable.primarySkills.begin(), reachable.primarySkills.end(),
                                                 [](uint16 skillId) { return IsGatheringSkill(skillId); });
            auto const merged = std::find_if(reachableSeeds.begin(), reachableSeeds.end(),
                                             [&reachable](PlayerbotCareerCandidateSeed const& candidate)
                                             { return candidate.primarySkills == reachable.primarySkills; });
            if (merged != reachableSeeds.end())
                merged->baseWeight += reachable.baseWeight;
            else
                reachableSeeds.push_back(std::move(reachable));
        }
        primarySeeds = std::move(reachableSeeds);
    }

    std::vector<PlayerbotCareerCandidateSeed> seeds = primarySeeds;
    std::array<std::pair<uint16, bool>, 3> const secondarySkills = {
        {{SKILL_COOKING, true}, {SKILL_FIRST_AID, true}, {SKILL_FISHING, false}}};
    for (auto const& [secondarySkill, isCrafting] : secondarySkills)
    {
        seeds.push_back({{},
                         {secondarySkill},
                         isCrafting,
                         !isCrafting,
                         100u,
                         isCrafting ? "secondary crafting profession" : "secondary gathering profession"});
        for (PlayerbotCareerCandidateSeed const& primary : primarySeeds)
        {
            PlayerbotCareerCandidateSeed variant = primary;
            variant.secondarySkills = {secondarySkill};
            variant.hasCrafting |= isCrafting;
            variant.hasGathering |= !isCrafting;
            variant.summary +=
                isCrafting ? " with an optional crafting secondary" : " with an optional gathering secondary";
            seeds.push_back(std::move(variant));
        }
    }

    return PlayerbotCareer::BuildCandidates(personality, seeds, maxPrimarySkills);
}

void PublishCareer(std::uint32_t characterGuid, PlayerbotCareerPlan const& plan, PlayerbotCareerTelemetrySource source)
{
    GetPlayerbotEconomyTelemetry().PublishCareer(characterGuid,
                                                 {
                                                     .status = PlayerbotCareerTelemetryStatus::Valid,
                                                     .source = source,
                                                     .version = plan.careerVersion,
                                                     .candidateToken = plan.candidateToken,
                                                     .primarySkills = plan.primarySkills,
                                                     .primarySkillAmendments = plan.primarySkillAmendments,
                                                     .secondarySkills = plan.secondarySkills,
                                                     .spendingStyle = static_cast<std::uint8_t>(plan.spendingStyle),
                                                     .marketEligible = plan.marketEligible,
                                                     .engagement = plan.engagement,
                                                 });
}
}  // namespace

bool PlayerbotCareer::EnsurePersistentPlan(Player* bot, PlayerbotCareerPlan& plan)
{
    if (!bot)
        return false;

    uint32 const characterGuid = bot->GetGUID().GetCounter();
    std::optional<PlayerbotPersonalityProfile> const personality = sPlayerbotPersonalityMgr.GetOrCreate(characterGuid);
    if (!personality)
    {
        GetPlayerbotEconomyTelemetry().PublishCareerPending(characterGuid);
        return false;
    }

    std::vector<PlayerbotCareerCandidate> const candidates = BuildCareerCandidates(bot, *personality);
    std::optional<std::string> serialized;
    if (sRandomPlayerbotMgr.GetValue(characterGuid, CAREER_PLAN_EVENT) == PLAYERBOT_CAREER_PLAN_VERSION)
        serialized = sRandomPlayerbotMgr.GetData(characterGuid, CAREER_PLAN_EVENT);

    PlayerbotCareerPlanRecovery const recovery =
        ResolvePersistedPlan(serialized, characterGuid, *personality, candidates, PrimaryProfessionSkillIds(),
                             LearnedPrimaryProfessionSkillIds(bot), GameTime::GetGameTimeMS().count());
    if (recovery.status != PlayerbotCareerPlanResolutionStatus::Resolved)
    {
        GetPlayerbotEconomyTelemetry().PublishCareerPending(characterGuid);
        return false;
    }

    plan = recovery.plan;
    if (recovery.shouldPersist)
        SavePersistentPlan(characterGuid, plan);
    else
        PublishCareer(characterGuid, plan, PlayerbotCareerTelemetrySource::Loaded);
    return true;
}

void PlayerbotCareer::SavePersistentPlan(uint32 characterGuid, PlayerbotCareerPlan const& plan)
{
    sRandomPlayerbotMgr.SetValue(characterGuid, CAREER_PLAN_EVENT, PLAYERBOT_CAREER_PLAN_VERSION, SerializePlan(plan));
    PublishCareer(characterGuid, plan, PlayerbotCareerTelemetrySource::Saved);
}

void PlayerbotCareer::UpdatePersistentPlans(uint64 nowMs)
{
    for (PlayerbotCareerPlan const& plan : PollPendingPlans(nowMs))
        SavePersistentPlan(static_cast<uint32>(plan.botGuid), plan);
}
