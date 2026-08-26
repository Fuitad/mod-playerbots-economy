/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <limits>

#include "Bot/Economy/PlayerbotEconomyTravelPlan.h"
#include "gtest/gtest.h"

namespace
{
EconomyTravelMode Choose(float distanceYards, std::uint32_t routeMaxAreaLevel, std::uint32_t botLevel,
                         bool flightPathAvailable = false, float flightMasterYards = 0.0f,
                         std::uint32_t flightMasterRouteMaxAreaLevel = 0, bool hearthReady = false)
{
    return ChooseEconomyTravelMode(distanceYards, routeMaxAreaLevel, botLevel, flightPathAvailable, flightMasterYards,
                                   flightMasterRouteMaxAreaLevel, hearthReady);
}
}  // namespace

TEST(PlayerbotEconomyTravelPlanTest, WalksOnlyWithinInclusiveDistanceBoundary)
{
    EXPECT_EQ(Choose(2500.0f, 23u, 20u, true, 2500.0f, 23u, true), EconomyTravelMode::Walk);
    EXPECT_EQ(Choose(2500.01f, 23u, 20u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, WalksOnlyWithinInclusiveSafetyMargin)
{
    EXPECT_EQ(Choose(2000.0f, 29u, 26u), EconomyTravelMode::Walk);
    EXPECT_EQ(Choose(2000.0f, 30u, 26u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(2000.0f, 46u, 26u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, UnknownRouteIsToleratedOnlyForShortTrip)
{
    EXPECT_EQ(Choose(1200.0f, 0u, 20u), EconomyTravelMode::Walk);
    EXPECT_EQ(Choose(5446.0f, 0u, 20u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, UsesDirectedFlightForObservedPathologicalDistances)
{
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, 500.0f, 20u, true), EconomyTravelMode::Fly);
    EXPECT_EQ(Choose(5770.0f, 46u, 24u, true, 500.0f, 24u), EconomyTravelMode::Fly);
    EXPECT_EQ(Choose(13906.0f, 46u, 28u, true, 500.0f, 28u), EconomyTravelMode::Fly);
}

TEST(PlayerbotEconomyTravelPlanTest, ObservedPathologicalDistancesNeverFallBackToWalking)
{
    EXPECT_EQ(Choose(5446.0f, 20u, 20u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(5770.0f, 24u, 24u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(13906.0f, 28u, 28u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, EvaluatesDestinationAndFlightMasterRoutesIndependently)
{
    EXPECT_EQ(Choose(2000.0f, 46u, 26u, true, 500.0f, 26u), EconomyTravelMode::Fly);
    EXPECT_EQ(Choose(3000.0f, 26u, 26u, true, 500.0f, 46u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, RejectsDistantOrDangerousFlightMasterApproach)
{
    EXPECT_EQ(Choose(5433.0f, 46u, 26u, true, 2500.01f, 26u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(5433.0f, 46u, 26u, true, 2500.0f, 30u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(5433.0f, 46u, 26u, true, 2500.0f, 29u), EconomyTravelMode::Fly);
}

TEST(PlayerbotEconomyTravelPlanTest, AppliesUnknownRuleToFlightMasterApproach)
{
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, 2500.0f, 0u), EconomyTravelMode::Fly);
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, 2500.01f, 0u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, RejectsInvalidDistanceMeasurements)
{
    float const infinity = std::numeric_limits<float>::infinity();
    float const nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(Choose(-1.0f, 20u, 20u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(infinity, 20u, 20u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(nan, 20u, 20u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, -1.0f, 20u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, infinity, 20u), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, nan, 20u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, HearthsWhenNeitherWalkingNorFlyingIsSafe)
{
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, false, 0.0f, 0u, true), EconomyTravelMode::Hearth);
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, false, 0.0f, 0u, false), EconomyTravelMode::Unreachable);
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, 500.0f, 46u, true), EconomyTravelMode::Hearth);
}

TEST(PlayerbotEconomyTravelPlanTest, WalksToAdjacentDestinationInsideDangerousArea)
{
    // Live 2026-08-25: bot 810 (level 28) stood 9 to 14 yards from its economy NPC inside a level 55
    // area and declined the destination on every cycle. The bot already occupies that area, so the
    // route gate was rejecting exposure the bot had no way to avoid, and the objective never resolved.
    EXPECT_EQ(Choose(9.0f, 55u, 28u), EconomyTravelMode::Walk);
    EXPECT_EQ(Choose(20.0f, 55u, 25u), EconomyTravelMode::Walk);

    // The same gate sent bots to the hearthstone to escape a short walk. That burns a long cooldown
    // to teleport the bot thousands of yards away from the destination it was standing next to.
    EXPECT_EQ(Choose(10.0f, 55u, 25u, false, 0.0f, 0u, true), EconomyTravelMode::Walk);
}

TEST(PlayerbotEconomyTravelPlanTest, ProximityWalkEndsAtTheRouteSampleInterval)
{
    EXPECT_EQ(Choose(100.0f, 55u, 25u), EconomyTravelMode::Walk);
    EXPECT_EQ(Choose(100.01f, 55u, 25u), EconomyTravelMode::Unreachable);
}

TEST(PlayerbotEconomyTravelPlanTest, AppliesProximityWalkToFlightMasterApproach)
{
    // A flight master the bot is already standing beside is reachable for the same reason.
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, 50.0f, 55u), EconomyTravelMode::Fly);
    EXPECT_EQ(Choose(5446.0f, 46u, 20u, true, 100.01f, 55u), EconomyTravelMode::Unreachable);
}
