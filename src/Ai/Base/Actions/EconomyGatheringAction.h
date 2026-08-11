/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ECONOMYGATHERINGACTION_H
#define PLAYERBOTS_ECONOMYGATHERINGACTION_H

#include "Ai/Base/Actions/AddLootAction.h"

class EconomyGatheringLootAction final : public AddGatheringLootAction
{
public:
    explicit EconomyGatheringLootAction(PlayerbotAI* botAI) : AddGatheringLootAction(botAI) {}
    static void HandleLoot(PlayerbotAI* botAI, uint32 itemId);
    static void Remove(PlayerbotAI* botAI);

protected:
    bool AddLoot(ObjectGuid guid) override;
};

#endif
