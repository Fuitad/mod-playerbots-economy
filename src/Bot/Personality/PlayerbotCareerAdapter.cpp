/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotCareerAdapter.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "Bot/Personality/PlayerbotCareerPopulation.h"
#include "Bot/Personality/PlayerbotCareerSeeds.h"
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

PlayerbotCareerPopulation::Targets ConfiguredPopulationTargets()
{
    return {sPlayerbotEconomyConfig.careerGatheringSharePercent, sPlayerbotEconomyConfig.careerProfessionFloorPermille,
            sPlayerbotEconomyConfig.careerFloorBoostPercent, sPlayerbotEconomyConfig.careerShareBoostPercent};
}

std::vector<PlayerbotCareerCandidate> BuildCareerCandidates(Player const* bot,
                                                            PlayerbotPersonalityProfile const& personality,
                                                            PlayerbotProfessionCensus const& census)
{
    uint32 const maxPrimarySkills = std::min<uint32>(2, sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL));
    std::vector<PlayerbotCareerCandidateSeed> const seeds =
        PlayerbotCareerSeeds::Build(bot->getClass(), LearnedPrimaryProfessionSkillIds(bot), maxPrimarySkills,
                                    sPlayerbotEconomyConfig.classMatchingProfessionChance);

    std::vector<PlayerbotCareerCandidate> candidates =
        PlayerbotCareer::BuildCandidates(personality, seeds, maxPrimarySkills);

    PlayerbotCareerPopulation::ApplyPopulationBias(candidates, census, ConfiguredPopulationTargets(), [](uint16 skillId)
                                                   { return PlayerbotCareerSeeds::IsGatheringSkill(skillId); });
    return candidates;
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

    // One snapshot serves both the bias and the provider decision, so they cannot disagree about the
    // population. The census is an in memory aggregate the telemetry already keeps for every career it
    // has published, so reading it costs one small copy and never a database query.
    PlayerbotProfessionCensus const census = GetPlayerbotEconomyTelemetry().SnapshotProfessionCensus();
    std::vector<PlayerbotCareerCandidate> const candidates = BuildCareerCandidates(bot, *personality, census);
    std::optional<std::string> serialized;
    if (sRandomPlayerbotMgr.GetValue(characterGuid, CAREER_PLAN_EVENT) == PLAYERBOT_CAREER_PLAN_VERSION)
        serialized = sRandomPlayerbotMgr.GetData(characterGuid, CAREER_PLAN_EVENT);

    // A career provider is told nothing about the population, so while a profession sits below its
    // floor the population weighted draw owns the assignment instead. Once every profession clears its
    // floor the provider decides again, exactly as it did before.
    PlayerbotCareerProviderUse const providerUse =
        PlayerbotCareerPopulation::PopulationNeedsCoverage(census, ConfiguredPopulationTargets(),
                                                           PrimaryProfessionSkillIds())
            ? PlayerbotCareerProviderUse::BypassedForPopulationCoverage
            : PlayerbotCareerProviderUse::Allowed;

    PlayerbotCareerPlanRecovery const recovery =
        ResolvePersistedPlan(serialized, characterGuid, *personality, candidates, PrimaryProfessionSkillIds(),
                             LearnedPrimaryProfessionSkillIds(bot), GameTime::GetGameTimeMS().count(), providerUse);
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
