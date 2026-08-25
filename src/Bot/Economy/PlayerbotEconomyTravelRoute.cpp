/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// PLB-LOCAL FILE. Not present upstream, so it can never conflict on a merge.
// Prefer adding here over editing an upstream file. See docs/local-changes.md.

#include "PlayerbotEconomyTravelRoute.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "DBCStores.h"
#include "MapMgr.h"
#include "Player.h"
#include "TravelNode.h"

namespace
{
// One hundred yards is fine-grained enough to catch a zone crossing without paying for pathfinding.
// The capped, even spacing keeps exceptionally long segments bounded while still sampling both ends.
constexpr float ECONOMY_ROUTE_SAMPLE_INTERVAL_YARDS = 100.0f;
constexpr std::size_t ECONOMY_ROUTE_MAX_SAMPLES = 256;

std::uint32_t ResolveAreaLevel(std::uint32_t mapId, float x, float y, float z)
{
    std::uint32_t const areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, mapId, x, y, z);
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    if (!area)
        return 0;

    // Sub-areas often omit their own level. The parent zone carries the intended progression level.
    if (area->area_level == 0 && area->zone)
    {
        if (AreaTableEntry const* const zone = sAreaTableStore.LookupEntry(area->zone))
            area = zone;
    }

    return area->area_level > 0 ? static_cast<std::uint32_t>(area->area_level) : 0;
}
}  // namespace

std::uint32_t SampleEconomyRouteMaxAreaLevel(WorldPosition const& start, WorldPosition const& end)
{
    if (start.GetMapId() != end.GetMapId())
        return 0;

    float const dx = end.GetPositionX() - start.GetPositionX();
    float const dy = end.GetPositionY() - start.GetPositionY();
    float const dz = end.GetPositionZ() - start.GetPositionZ();
    float const distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance))
        return 0;
    std::size_t const desiredSamples = std::max<std::size_t>(
        2, static_cast<std::size_t>(std::ceil(distance / ECONOMY_ROUTE_SAMPLE_INTERVAL_YARDS)) + 1);
    std::size_t const sampleCount = std::min(desiredSamples, ECONOMY_ROUTE_MAX_SAMPLES);

    std::uint32_t maximumLevel = 0;
    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        float const progress = static_cast<float>(index) / static_cast<float>(sampleCount - 1);
        maximumLevel = std::max(
            maximumLevel, ResolveAreaLevel(start.GetMapId(), start.GetPositionX() + dx * progress,
                                           start.GetPositionY() + dy * progress, start.GetPositionZ() + dz * progress));
    }

    // This straight-line sample is a cheap safety proxy, not the route the bot will actually walk.
    // It catches the live failure that matters: sending a low-level bot across a high-level zone.
    return maximumLevel;
}

std::optional<EconomyDirectedFlightPlan> FindEconomyDirectedFlightPlan(Player* bot, WorldPosition const& destination)
{
    if (!bot || destination.GetMapId() != bot->GetMapId())
        return std::nullopt;

    TravelMgr::FlightMasterInfo const* const flightMaster = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!flightMaster || !flightMaster->taxiNodeId)
        return std::nullopt;

    std::uint32_t const areaId =
        sMapMgr->GetAreaId(PHASEMASK_NORMAL, destination.GetMapId(), destination.GetPositionX(),
                           destination.GetPositionY(), destination.GetPositionZ());
    AreaTableEntry const* const area = sAreaTableStore.LookupEntry(areaId);
    if (!area)
        return std::nullopt;

    std::uint32_t const destinationZone = area->zone ? area->zone : area->ID;
    if (destinationZone == flightMaster->zoneId)
        return std::nullopt;

    std::vector<std::uint32_t> const candidates =
        sTravelMgr.GetFlightNodesInZone(destinationZone, bot->GetTeamId(), flightMaster->taxiNodeId);
    for (std::uint32_t const candidate : candidates)
    {
        std::vector<std::uint32_t> path = sTravelNodeMap.FindTaxiPath(flightMaster->taxiNodeId, candidate);
        if (!path.empty())
        {
            return EconomyDirectedFlightPlan{
                .flightMasterEntry = flightMaster->templateEntry,
                .flightMasterPos = flightMaster->pos,
                .path = std::move(path),
            };
        }
    }

    return std::nullopt;
}
