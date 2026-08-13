/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EconomyAction.h"

#include <algorithm>
#include <utility>

#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Personality/PlayerbotCareerAdapter.h"
#include "Bot/Personality/PlayerbotCareerProgression.h"
#include "Bot/Personality/PlayerbotPersonalityMgr.h"
#include "GameTime.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotCareerPlan.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr char PROFESSION_WORK_ORDER_EVENT[] = "profession work order";

uint8 ProgressionEngagement(Player* bot, PlayerbotCareerPlan const& plan)
{
    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(bot->GetGUID().GetCounter());
    if (!personality)
        return plan.engagement;

    uint32 highestPressure = 0u;
    std::vector<uint16> skills = plan.primarySkills;
    skills.insert(skills.end(), plan.secondarySkills.begin(), plan.secondarySkills.end());
    if (bot->HasSkill(SKILL_COOKING))
        skills.push_back(SKILL_COOKING);
    if (bot->HasSkill(SKILL_FIRST_AID))
        skills.push_back(SKILL_FIRST_AID);
    for (uint16 skillId : skills)
    {
        if (!bot->HasSkill(skillId) || skillId == SKILL_HERBALISM || skillId == SKILL_MINING ||
            skillId == SKILL_SKINNING)
        {
            continue;
        }
        uint16 const current = bot->GetPureSkillValue(skillId);
        uint16 const currentCap = bot->GetPureMaxSkillValue(skillId);
        uint16 const target = current >= currentCap && currentCap < sWorld->GetConfigMaxSkillValue()
                                  ? static_cast<uint16>(currentCap + 1u)
                                  : currentCap;
        highestPressure = std::max(highestPressure, PlayerbotCareer::ProgressionPressure(
                                                        {
                                                            .professionSkillId = skillId,
                                                            .currentSkill = current,
                                                            .targetSkill = target,
                                                            .affinity = personality->craftingAffinity,
                                                            .planned = true,
                                                            .learned = true,
                                                        },
                                                        PlayerbotCareer::PROFESSION_PROGRESSION_MAXIMUM_PRESSURE));
    }
    return PlayerbotCareer::ProgressionSchedulingEngagement(plan.engagement, highestPressure,
                                                            PlayerbotCareer::PROFESSION_PROGRESSION_MAXIMUM_PRESSURE);
}

std::string ItemFamily(EconomySubstitutionGroup const& group)
{
    switch (group.kind)
    {
        case EconomySubstitutionKind::ExactReagent:
            return "exact_reagent:" + std::to_string(group.exactItemId);
        case EconomySubstitutionKind::Equipment:
            return "equipment:" + std::to_string(group.equipmentSlot) + ":" + std::to_string(group.roleMask) + ":" +
                   std::to_string(group.tier);
        case EconomySubstitutionKind::Bag:
            return "bag:" + std::to_string(group.bagCapacity);
        case EconomySubstitutionKind::Ammunition:
            return "ammunition:" + std::to_string(group.ammunitionType) + ":" + std::to_string(group.tier);
        case EconomySubstitutionKind::Consumable:
            return "consumable:" + std::to_string(group.effectFamily) + ":" + std::to_string(group.tier);
        case EconomySubstitutionKind::Enhancement:
            return "enhancement:" + std::to_string(group.enhancementTarget) + ":" + std::to_string(group.valueBand);
    }
    return {};
}

std::string WorkIdentity(PlayerbotEconomyCycleResult const& result, EconomyActorChainObservation const& chain)
{
    if (!chain.workIdentity.empty())
        return chain.workIdentity;
    if (result.workIdentity.auctionId)
        return "auction:" + std::to_string(result.workIdentity.auctionId);
    if (result.workIdentity.spellId)
        return "spell:" + std::to_string(result.workIdentity.spellId);
    if (result.workIdentity.itemId)
        return "item:" + std::to_string(result.workIdentity.itemId);
    return result.blocker;
}

PlayerbotEconomyTelemetryPhase VerificationPhase(EconomyPhase phase)
{
    switch (phase)
    {
        case EconomyPhase::None:
            return PlayerbotEconomyTelemetryPhase::None;
        case EconomyPhase::CollectAuctionMail:
            return PlayerbotEconomyTelemetryPhase::CollectAuctionMail;
        case EconomyPhase::Craft:
            return PlayerbotEconomyTelemetryPhase::Craft;
        case EconomyPhase::BuyReagent:
            return PlayerbotEconomyTelemetryPhase::BuyReagent;
        case EconomyPhase::BuyRecipe:
            return PlayerbotEconomyTelemetryPhase::BuyRecipe;
        case EconomyPhase::BuyFinishedGood:
            return PlayerbotEconomyTelemetryPhase::BuyFinishedGood;
        case EconomyPhase::UseFinishedGood:
            return PlayerbotEconomyTelemetryPhase::UseFinishedGood;
        case EconomyPhase::RecoverFinishedGood:
            return PlayerbotEconomyTelemetryPhase::RecoverFinishedGood;
        case EconomyPhase::SellSurplus:
            return PlayerbotEconomyTelemetryPhase::SellSurplus;
    }

    return PlayerbotEconomyTelemetryPhase::None;
}

PlayerbotEconomyOutcome VerificationOutcome(PlayerbotEconomyCycleOutcome outcome)
{
    switch (outcome)
    {
        case PlayerbotEconomyCycleOutcome::NoCandidate:
            return PlayerbotEconomyOutcome::NoCandidate;
        case PlayerbotEconomyCycleOutcome::Scheduled:
            return PlayerbotEconomyOutcome::Scheduled;
        case PlayerbotEconomyCycleOutcome::Operation:
            return PlayerbotEconomyOutcome::Operation;
        case PlayerbotEconomyCycleOutcome::FailedPrecondition:
            return PlayerbotEconomyOutcome::FailedPrecondition;
    }

    return PlayerbotEconomyOutcome::FailedPrecondition;
}

bool IsTransferRelease(EconomyActorChainObservation const& chain)
{
    if (!chain.available || chain.claimState != EconomyClaimState::Released)
        return false;
    return chain.assignmentOutcome == EconomyAssignmentOutcome::NeedChanged ||
           chain.assignmentOutcome == EconomyAssignmentOutcome::LoggedOut ||
           chain.assignmentOutcome == EconomyAssignmentOutcome::Disabled;
}
}  // namespace

EconomyCycleAction::EconomyCycleAction(PlayerbotAI* botAI) : EconomyCycleAction(botAI, CreatePlayerbotEconomyRuntime())
{
}

EconomyCycleAction::EconomyCycleAction(PlayerbotAI* botAI, std::unique_ptr<PlayerbotEconomyRuntime> runtime)
    : ChooseTravelTargetAction(botAI, "economy cycle"), runtime(std::move(runtime))
{
}

bool EconomyCycleAction::isUseful()
{
    if (!sPlayerbotEconomyConfig.lifecycleEnabled || !sRandomPlayerbotMgr.IsRandomBot(bot) ||
        botAI->HasActivePlayerMaster())
    {
        EconomyAssignmentOutcome const outcome = !sPlayerbotEconomyConfig.lifecycleEnabled
                                                     ? EconomyAssignmentOutcome::Disabled
                                                     : EconomyAssignmentOutcome::CapabilityLost;
        GetPlayerbotEconomyCoordinator().InvalidateActor(bot->GetGUID().GetCounter(), outcome,
                                                         GameTime::GetGameTime().count());
        runtime->Reset(botAI);
        return false;
    }

    PlayerbotCareerPlan careerPlan;
    if (!PlayerbotCareer::EnsurePersistentPlan(bot, careerPlan))
    {
        GetPlayerbotEconomyCoordinator().InvalidateActor(
            bot->GetGUID().GetCounter(), EconomyAssignmentOutcome::CapabilityLost, GameTime::GetGameTime().count());
        runtime->Reset(botAI);
        return false;
    }
    if (!runtime->IsEligible(botAI, careerPlan))
    {
        runtime->Reset(botAI);
        return false;
    }

    uint64 const now = GameTime::GetGameTime().count();
    careerIntervalSeconds = PlayerbotEconomyPolicy::CareerIntervalSeconds(sPlayerbotAIConfig.randomBotUpdateInterval,
                                                                          ProgressionEngagement(bot, careerPlan));
    if (!nextEligibleTime)
    {
        nextEligibleTime =
            PlayerbotEconomyPolicy::InitialEligibleTime(now, bot->GetGUID().GetCounter(), careerIntervalSeconds);
        GetPlayerbotEconomyTelemetry().Publish(
            bot->GetGUID().GetCounter(),
            {
                .outcome = PlayerbotEconomyOutcome::Scheduled,
                .phase = PlayerbotEconomyTelemetryPhase::None,
                .workOrderSpellId = sRandomPlayerbotMgr.GetValue(bot, PROFESSION_WORK_ORDER_EVENT),
                .consecutiveFailures = failureTracker.Count(),
                .nextEligibleTime = nextEligibleTime,
            });
    }

    return now >= nextEligibleTime;
}

bool EconomyCycleAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    PlayerbotCareerPlan careerPlan;
    if (!PlayerbotCareer::EnsurePersistentPlan(bot, careerPlan))
        return false;

    uint64 const now = GameTime::GetGameTime().count();
    PlayerbotEconomyCycleResult const result = runtime->ExecuteCycle(botAI, careerPlan);
    bool const executed = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled ||
                          result.outcome == PlayerbotEconomyCycleOutcome::Operation;

    EconomyActorChainObservation const chain =
        GetPlayerbotEconomyCoordinator().ObserveActor(bot->GetGUID().GetCounter(), now);
    std::string const operationIdentity = WorkIdentity(result, chain);
    std::string const failureKey = chain.chainPublicId + ':' + operationIdentity + ':' + result.blocker + ':' +
                                   std::to_string(static_cast<uint8>(result.phase));
    if (executed)
        failureTracker.Clear();
    else
        failureTracker.RecordFailure(failureKey);
    uint8 const consecutiveFailures = failureTracker.Count();
    nextEligibleTime = PlayerbotEconomyPolicy::NextEligibleTime(now, careerIntervalSeconds, result.schedulingEffect,
                                                                consecutiveFailures);
    bool const quarantined = !executed && failureTracker.IsQuarantined();
    PlayerbotEconomyOutcome outcome = VerificationOutcome(result.outcome);
    if (quarantined)
        outcome = PlayerbotEconomyOutcome::Quarantined;
    else if (!executed && IsTransferRelease(chain))
        outcome = PlayerbotEconomyOutcome::Released;
    PlayerbotEconomyTelemetryPhase phase = VerificationPhase(result.phase);
    if (result.blocker.starts_with("market_position_"))
        phase = PlayerbotEconomyTelemetryPhase::MarketMaking;
    else if (result.blocker.starts_with("gathering_"))
        phase = PlayerbotEconomyTelemetryPhase::Gather;
    GetPlayerbotEconomyTelemetry().Publish(bot->GetGUID().GetCounter(),
                                           {
                                               .outcome = outcome,
                                               .phase = phase,
                                               .chainPublicId = chain.chainPublicId,
                                               .operationIdentity = operationIdentity,
                                               .marketId = chain.marketId,
                                               .itemFamily = chain.available ? ItemFamily(chain.group) : std::string(),
                                               .workOrderSpellId = result.workIdentity.spellId,
                                               .remainingQuantity = chain.remainingQuantity,
                                               .claimAgeSeconds = chain.claimAgeSeconds,
                                               .blockerCode = result.blocker,
                                               .consecutiveFailures = consecutiveFailures,
                                               .cooldownSeconds = nextEligibleTime > now ? nextEligibleTime - now : 0u,
                                               .nextEligibleTime = nextEligibleTime,
                                               .quarantined = quarantined,
                                           });
    return executed;
}
