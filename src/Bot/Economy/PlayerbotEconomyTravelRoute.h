/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// PLB-LOCAL FILE. Not present upstream, so it can never conflict on a merge.
// Prefer adding here over editing an upstream file. See docs/local-changes.md.

#ifndef PLAYERBOTS_PLAYERBOTECONOMYTRAVELROUTE_H
#define PLAYERBOTS_PLAYERBOTECONOMYTRAVELROUTE_H

#include <cstdint>
#include <optional>
#include <vector>

#include "TravelMgr.h"

class Player;

struct EconomyDirectedFlightPlan
{
    std::uint32_t flightMasterEntry = 0;
    WorldPosition flightMasterPos;
    std::vector<std::uint32_t> path;
};

[[nodiscard]] std::uint32_t SampleEconomyRouteMaxAreaLevel(WorldPosition const& start, WorldPosition const& end);
[[nodiscard]] std::optional<EconomyDirectedFlightPlan> FindEconomyDirectedFlightPlan(Player* bot,
                                                                                     WorldPosition const& destination);

#endif
