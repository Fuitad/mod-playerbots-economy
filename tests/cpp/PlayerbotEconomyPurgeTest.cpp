/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <vector>

#include "Bot/Economy/PlayerbotEconomyPurge.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr uint32 PURGED_SELLER = 11u;
constexpr uint32 OTHER_PURGED_SELLER = 12u;
constexpr uint32 SURVIVING_PLAYER = 99u;
}  // namespace

TEST(PlayerbotEconomyPurgeTest, SelectsOnlyListingsOwnedByThePurgedCohort)
{
    std::vector<PurgeAuctionFact> const auctions{
        {.auctionId = 1u, .itemGuid = 501u, .itemOwner = PURGED_SELLER, .bidderGuid = 0u},
        {.auctionId = 2u, .itemGuid = 502u, .itemOwner = SURVIVING_PLAYER, .bidderGuid = 0u},
        {.auctionId = 3u, .itemGuid = 503u, .itemOwner = OTHER_PURGED_SELLER, .bidderGuid = 0u}};

    AuctionPurgePlan const plan =
        PlayerbotEconomyPurge::BuildAuctionPurgePlan(auctions, {PURGED_SELLER, OTHER_PURGED_SELLER});

    ASSERT_TRUE(plan.MayMutate());
    EXPECT_EQ(plan.auctionIds, std::vector<uint32>({1u, 3u}));
    EXPECT_EQ(plan.itemGuids, std::vector<uint32>({501u, 503u}));
}

TEST(PlayerbotEconomyPurgeTest, RefusesAndPlansNothingWhenASurvivorHoldsABid)
{
    std::vector<PurgeAuctionFact> const auctions{
        {.auctionId = 1u, .itemGuid = 501u, .itemOwner = PURGED_SELLER, .bidderGuid = 0u},
        {.auctionId = 2u, .itemGuid = 502u, .itemOwner = PURGED_SELLER, .bidderGuid = SURVIVING_PLAYER}};

    AuctionPurgePlan const plan = PlayerbotEconomyPurge::BuildAuctionPurgePlan(auctions, {PURGED_SELLER});

    EXPECT_FALSE(plan.MayMutate());
    EXPECT_EQ(plan.refusal, PurgeRefusal::OutsideBidder);
    EXPECT_EQ(plan.refusedAuctionId, 2u);
    EXPECT_EQ(plan.refusedBidderGuid, SURVIVING_PLAYER);
    // The listing that was already accepted must not survive the refusal, or an aborted purge would
    // still delete part of the auction house.
    EXPECT_TRUE(plan.auctionIds.empty());
    EXPECT_TRUE(plan.itemGuids.empty());
}

TEST(PlayerbotEconomyPurgeTest, AcceptsABidHeldByAnotherPurgedBot)
{
    std::vector<PurgeAuctionFact> const auctions{
        {.auctionId = 7u, .itemGuid = 507u, .itemOwner = PURGED_SELLER, .bidderGuid = OTHER_PURGED_SELLER}};

    AuctionPurgePlan const plan =
        PlayerbotEconomyPurge::BuildAuctionPurgePlan(auctions, {PURGED_SELLER, OTHER_PURGED_SELLER});

    ASSERT_TRUE(plan.MayMutate());
    EXPECT_EQ(plan.auctionIds, std::vector<uint32>({7u}));
}

/*
 * The state this whole change exists to clean up. An orphaned listing keeps its original nonzero
 * `itemguid`; it is the `item_instance` row that is gone. The planner cannot see that absence, so it must
 * plan the listing on ownership alone. A future implementation that only planned listings whose item row
 * still exists would silently skip every real orphan.
 */
TEST(PlayerbotEconomyPurgeTest, PlansAnOrphanedListingThatStillNamesAMissingItem)
{
    std::vector<PurgeAuctionFact> const auctions{
        {.auctionId = 4u, .itemGuid = 504u, .itemOwner = PURGED_SELLER, .bidderGuid = 0u}};

    AuctionPurgePlan const plan = PlayerbotEconomyPurge::BuildAuctionPurgePlan(auctions, {PURGED_SELLER});

    ASSERT_TRUE(plan.MayMutate());
    EXPECT_EQ(plan.auctionIds, std::vector<uint32>({4u}));
    EXPECT_EQ(plan.itemGuids, std::vector<uint32>({504u}));
}

TEST(PlayerbotEconomyPurgeTest, KeepsTheAuctionRowWhenTheListingCarriesNoItemGuid)
{
    std::vector<PurgeAuctionFact> const auctions{
        {.auctionId = 4u, .itemGuid = 0u, .itemOwner = PURGED_SELLER, .bidderGuid = 0u}};

    AuctionPurgePlan const plan = PlayerbotEconomyPurge::BuildAuctionPurgePlan(auctions, {PURGED_SELLER});

    ASSERT_TRUE(plan.MayMutate());
    EXPECT_EQ(plan.auctionIds, std::vector<uint32>({4u}));
    EXPECT_TRUE(plan.itemGuids.empty());
}

/*
 * A null QueryResult in AzerothCore means either "no rows" or "the query failed". Treating a failure as an
 * empty auction house would approve the purge without ever running the outside-bidder check, so the
 * database layer turns an unreadable result into this refusal.
 */
TEST(PlayerbotEconomyPurgeTest, RefusesWhenTheAuctionQueryCouldNotBeRead)
{
    AuctionPurgePlan const plan = PlayerbotEconomyPurge::QueryFailedPlan();

    EXPECT_FALSE(plan.MayMutate());
    EXPECT_EQ(plan.refusal, PurgeRefusal::QueryFailed);
    EXPECT_TRUE(plan.auctionIds.empty());
    EXPECT_TRUE(plan.itemGuids.empty());
}

TEST(PlayerbotEconomyPurgeTest, PlansNothingWhenNoCharacterIsBeingPurged)
{
    std::vector<PurgeAuctionFact> const auctions{
        {.auctionId = 1u, .itemGuid = 501u, .itemOwner = PURGED_SELLER, .bidderGuid = 0u}};

    AuctionPurgePlan const plan = PlayerbotEconomyPurge::BuildAuctionPurgePlan(auctions, {});

    ASSERT_TRUE(plan.MayMutate());
    EXPECT_TRUE(plan.auctionIds.empty());
    EXPECT_TRUE(plan.itemGuids.empty());
}

TEST(PlayerbotEconomyPurgeTest, NamesTheRefusalForTheCleanupLog)
{
    EXPECT_STREQ(PlayerbotEconomyPurge::PurgeRefusalName(PurgeRefusal::OutsideBidder), "outside_bidder");
    EXPECT_STREQ(PlayerbotEconomyPurge::PurgeRefusalName(PurgeRefusal::QueryFailed), "query_failed");
    EXPECT_STREQ(PlayerbotEconomyPurge::PurgeRefusalName(PurgeRefusal::None), "none");
}
