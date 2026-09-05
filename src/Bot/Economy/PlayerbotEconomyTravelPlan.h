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

/*
 * The route danger gate stops a bot crossing territory it cannot survive, so it only means anything
 * when the route actually crosses somewhere. Within one route sample interval the bot already stands
 * in whatever area the destination sits in, and refusing the walk protects it from nothing. Live
 * 2026-08-25, a level 28 bot declined a destination 9 yards away in a level 55 area on every cycle,
 * and others hearthed to escape a 10 yard walk, which teleported them thousands of yards from the
 * destination they were standing next to. This matches ECONOMY_ROUTE_SAMPLE_INTERVAL_YARDS: below it
 * the sampler reads only the two endpoints, so there is no intervening route to judge.
 */
inline constexpr float ECONOMY_PROXIMITY_WALK_YARDS = 100.0f;

/*
 * How far along the route the bot's OWN ground is read. A single point under the bot is not a fair
 * comparison against a route level that is a maximum over up to 256 samples: the one point resolves
 * to zero far more often, and the comparison then silently fails closed. It fails exactly where it
 * is needed, because a town carries no level of its own and neither does the zone record beneath it.
 * Gadgetzan is a sub-area of Tanaris and both read zero, so a bot standing in Gadgetzan reported
 * ground level 0 while the route out across the Tanaris sand reported 40, and the destination was
 * declined on every cycle. Reading a short stretch of the route instead reaches the ground the town
 * sits in, while still leaving a route that ends somewhere worse correctly gated.
 */
inline constexpr float ECONOMY_ORIGIN_GROUND_YARDS = 250.0f;

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
                                                        std::uint32_t flightMasterRouteMaxAreaLevel, bool hearthReady,
                                                        std::uint32_t originAreaLevel = 0u);

/*
 * A vendor within this many yards of an auctioneer the bot can use stands in a hub: a capital or an
 * auction town, where the other reagents, the trainer and the auction house are all in reach. The
 * capitals put their alchemy and trade supplies within 50 to 350 yards of the auctioneers
 * (Stormwind 50 and 190, Ironforge 110 and 350, Undercity 45 and 240, Booty Bay 125).
 */
inline constexpr float ECONOMY_HUB_VENDOR_RADIUS_YARDS = 500.0f;

/*
 * Whether a vendor candidate beats the one currently held for a profession reagent trip. A hub
 * vendor beats any lone vendor however near the lone one is; between two of the same kind the
 * nearer wins. Pierre, 2026-09-05: a bot working its profession "should be doing so in a capital
 * city where reagents are plenty", not walking 1600 yards to the one vendor in a zone that sells
 * an Empty Vial.
 */
[[nodiscard]] inline bool PrefersVendor(bool candidateHub, float candidateYards, bool currentHub, float currentYards)
{
    if (candidateHub != currentHub)
        return candidateHub;
    return candidateYards < currentYards;
}

#endif
