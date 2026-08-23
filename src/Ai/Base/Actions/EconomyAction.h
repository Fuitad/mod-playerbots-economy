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
#include "SellAction.h"
#include "UseItemAction.h"

class Item;
class PlayerbotAI;
struct PlayerbotCareerPlan;

class EconomyUseItemAction final : public UseItemAction
{
public:
    explicit EconomyUseItemAction(PlayerbotAI* botAI) : UseItemAction(botAI, "economy final use", true) {}

    bool Apply(Item* item);
    bool ApplyToItem(Item* item, Item* itemTarget);
    bool ApplyGlyph(Item* item, uint8 glyphSlot);
    bool Socket(Item* gem, Item* itemTarget, uint8 socketIndex);
};

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

// Upstream "sell" hands a vendor everything marked VENDOR or AH. An economy bot keeps its AH-marked
// trade goods for the auction house; the rpg vendor visit only clears real vendor trash.
class EconomySellAction : public SellAction
{
public:
    explicit EconomySellAction(PlayerbotAI* botAI) : SellAction(botAI) {}

    bool Execute(Event event) override;
};

#endif  // PLAYERBOTS_ECONOMYACTION_H
