/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// PLB-LOCAL FILE. Not present upstream, so it can never conflict on a merge.
// Prefer adding here over editing an upstream file. See docs/local-changes.md.

#include "PlayerbotEconomyTravelPlan.h"

#include <cmath>
#include <cstdint>

namespace
{
bool IsSafeWalk(float distanceYards, std::uint32_t routeMaxAreaLevel, std::uint32_t botLevel)
{
    if (!std::isfinite(distanceYards) || distanceYards < 0.0f || distanceYards > ECONOMY_MAX_WALK_YARDS)
        return false;

    // A destination the bot is already standing beside carries no route to judge. Declining it does
    // not avoid the area level, because the bot occupies that area either way; it only strands the
    // objective, or spends the hearthstone to travel away from the goal.
    if (distanceYards <= ECONOMY_PROXIMITY_WALK_YARDS)
        return true;

    // An unresolved area is tolerated only after the distance gate has proven this is an ordinary
    // local errand. It must never turn the pathological cross-continent tail back into a walk.
    if (!routeMaxAreaLevel)
        return true;

    return static_cast<std::uint64_t>(routeMaxAreaLevel) <=
           static_cast<std::uint64_t>(botLevel) + ECONOMY_SAFE_LEVEL_MARGIN;
}
}  // namespace

EconomyTravelMode ChooseEconomyTravelMode(float distanceYards, std::uint32_t routeMaxAreaLevel, std::uint32_t botLevel,
                                          bool flightPathAvailable, float flightMasterYards,
                                          std::uint32_t flightMasterRouteMaxAreaLevel, bool hearthReady)
{
    if (IsSafeWalk(distanceYards, routeMaxAreaLevel, botLevel))
        return EconomyTravelMode::Walk;

    // The flight master is a separate leg. Franziska's unsafe route to the auctioneer says nothing
    // about whether she can safely reach the nearby taxi that lets her bypass it.
    if (flightPathAvailable && IsSafeWalk(flightMasterYards, flightMasterRouteMaxAreaLevel, botLevel))
        return EconomyTravelMode::Fly;

    if (hearthReady)
        return EconomyTravelMode::Hearth;

    return EconomyTravelMode::Unreachable;
}
