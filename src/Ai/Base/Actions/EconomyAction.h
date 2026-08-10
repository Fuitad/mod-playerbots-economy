/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ECONOMYACTION_H
#define PLAYERBOTS_ECONOMYACTION_H

#include "Bot/Economy/PlayerbotEconomyRuntime.h"
#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "ChooseTravelTargetAction.h"

class PlayerbotAI;
struct PlayerbotCareerPlan;

class EconomyCycleAction : public ChooseTravelTargetAction
{
public:
    explicit EconomyCycleAction(PlayerbotAI* botAI);
    EconomyCycleAction(PlayerbotAI* botAI, std::unique_ptr<PlayerbotEconomyRuntime> runtime);

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    std::unique_ptr<PlayerbotEconomyRuntime> runtime;
    uint64 nextEligibleTime = 0;
    uint32 careerIntervalSeconds = 0;
    PlayerbotEconomyFailureTracker failureTracker;
};

#endif  // PLAYERBOTS_ECONOMYACTION_H
