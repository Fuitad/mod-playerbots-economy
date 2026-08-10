/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYRUNTIME_H
#define PLAYERBOTS_PLAYERBOTECONOMYRUNTIME_H

#include <memory>
#include <string>

#include "Bot/Economy/PlayerbotEconomyPolicy.h"

class PlayerbotAI;
struct PlayerbotCareerPlan;

enum class PlayerbotEconomyCycleOutcome : uint8
{
    NoCandidate,
    Scheduled,
    Operation,
    FailedPrecondition
};

struct PlayerbotEconomyWorkIdentity
{
    uint32 spellId = 0;
    uint32 itemId = 0;
    uint32 auctionId = 0;
    uint64 itemGuidCounter = 0;
};

struct PlayerbotEconomyCycleResult
{
    PlayerbotEconomyCycleOutcome outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
    PlayerbotEconomy::EconomyPhase phase = PlayerbotEconomy::EconomyPhase::None;
    PlayerbotEconomyWorkIdentity workIdentity;
    std::string blocker;
    PlayerbotEconomy::EconomyAttemptOutcome schedulingEffect =
        PlayerbotEconomy::EconomyAttemptOutcome::FailedPrecondition;
};

class PlayerbotEconomyRuntime
{
public:
    virtual ~PlayerbotEconomyRuntime() = default;

    [[nodiscard]] virtual bool IsEligible(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan) const = 0;
    [[nodiscard]] virtual PlayerbotEconomyCycleResult ExecuteCycle(PlayerbotAI* botAI,
                                                                   PlayerbotCareerPlan const& careerPlan) = 0;
    virtual void Reset(PlayerbotAI* botAI) = 0;
};

[[nodiscard]] std::unique_ptr<PlayerbotEconomyRuntime> CreatePlayerbotEconomyRuntime();

#endif
