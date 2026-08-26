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
bool IsSafeWalk(float distanceYards, std::uint32_t routeMaxAreaLevel, std::uint32_t botLevel,
                std::uint32_t originAreaLevel)
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

    // The bot is already standing in this much danger, so a route through no worse than its own
    // ground exposes it to nothing new. Every classic auction hub sits in a high level zone
    // (Gadgetzan 40, Booty Bay 45, Everlook 55), and a bot that has already FLOWN to one was
    // stranded on arrival: live 2026-08-26, bots at levels 19, 25 and 26 stood 114 to 410 yards
    // from Auctioneer Beardo in Gadgetzan and declined him every cycle on a route level of 40,
    // which is simply Tanaris, the ground under their feet. Refusing the last few hundred yards
    // of a journey the bot has already made protects it from nothing.
    if (originAreaLevel && routeMaxAreaLevel <= originAreaLevel)
        return true;

    return static_cast<std::uint64_t>(routeMaxAreaLevel) <=
           static_cast<std::uint64_t>(botLevel) + ECONOMY_SAFE_LEVEL_MARGIN;
}
}  // namespace

EconomyTravelMode ChooseEconomyTravelMode(float distanceYards, std::uint32_t routeMaxAreaLevel, std::uint32_t botLevel,
                                          bool flightPathAvailable, float flightMasterYards,
                                          std::uint32_t flightMasterRouteMaxAreaLevel, bool hearthReady,
                                          std::uint32_t originAreaLevel)
{
    if (IsSafeWalk(distanceYards, routeMaxAreaLevel, botLevel, originAreaLevel))
        return EconomyTravelMode::Walk;

    // The flight master is a separate leg. Franziska's unsafe route to the auctioneer says nothing
    // about whether she can safely reach the nearby taxi that lets her bypass it.
    if (flightPathAvailable && IsSafeWalk(flightMasterYards, flightMasterRouteMaxAreaLevel, botLevel, originAreaLevel))
        return EconomyTravelMode::Fly;

    if (hearthReady)
        return EconomyTravelMode::Hearth;

    return EconomyTravelMode::Unreachable;
}
