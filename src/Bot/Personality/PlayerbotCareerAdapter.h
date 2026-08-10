/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTCAREERADAPTER_H
#define PLAYERBOTS_PLAYERBOTCAREERADAPTER_H

#include "PlayerbotCareerPlan.h"

class Player;

namespace PlayerbotCareer
{
bool EnsurePersistentPlan(Player* bot, PlayerbotCareerPlan& plan);
void SavePersistentPlan(std::uint32_t characterGuid, PlayerbotCareerPlan const& plan);
void UpdatePersistentPlans(std::uint64_t nowMs);
}  // namespace PlayerbotCareer

#endif
