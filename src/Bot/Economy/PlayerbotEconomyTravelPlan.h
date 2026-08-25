/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// PLB-LOCAL FILE. Not present upstream, so it can never conflict on a merge.
// Prefer adding here over editing an upstream file. See docs/local-changes.md.

#ifndef PLAYERBOTS_PLAYERBOTECONOMYTRAVELPLAN_H
#define PLAYERBOTS_PLAYERBOTECONOMYTRAVELPLAN_H

#include <cstdint>

/*
 * The ordinary economy population is already close to its faction-correct NPC: 91 percent are
 * within 2500 yards of an auctioneer. The ceiling keeps that normal case cheap while separating
 * the live 5446, 5770, and 13906 yard failures that were continent journeys, not errands.
 */
inline constexpr float ECONOMY_MAX_WALK_YARDS = 2500.0f;
inline constexpr std::uint32_t ECONOMY_SAFE_LEVEL_MARGIN = 3;

enum class EconomyTravelMode : std::uint8_t
{
    Walk,
    Fly,
    Hearth,
    Unreachable
};

/*
 * The destination and nearest-flight-master routes are separate safety facts. Live, a level 26 bot
 * crossed level 45 to 49 territory on the direct auction route and died 44 times, while its local
 * flight-master approach could still have been safe. A zero route level means area sampling could
 * not resolve the segment; it is tolerated only when that segment is within the local walk ceiling.
 */
[[nodiscard]] EconomyTravelMode ChooseEconomyTravelMode(float distanceYards, std::uint32_t routeMaxAreaLevel,
                                                        std::uint32_t botLevel, bool flightPathAvailable,
                                                        float flightMasterYards,
                                                        std::uint32_t flightMasterRouteMaxAreaLevel, bool hearthReady);

#endif
