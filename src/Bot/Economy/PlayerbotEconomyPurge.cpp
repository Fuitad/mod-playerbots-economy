/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyPurge.h"

#include <algorithm>
#include <unordered_set>

namespace PlayerbotEconomy
{
namespace
{
void SortAndDeduplicate(std::vector<uint32>& ids)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}
}  // namespace

AuctionPurgePlan PlayerbotEconomyPurge::BuildAuctionPurgePlan(std::vector<PurgeAuctionFact> const& auctions,
                                                              std::vector<uint32> const& purgedGuids)
{
    AuctionPurgePlan plan;
    if (purgedGuids.empty())
        return plan;

    std::unordered_set<uint32> const purged(purgedGuids.begin(), purgedGuids.end());

    for (PurgeAuctionFact const& auction : auctions)
    {
        if (!purged.contains(auction.itemOwner))
            continue;

        if (auction.bidderGuid && !purged.contains(auction.bidderGuid))
        {
            plan.auctionIds.clear();
            plan.itemGuids.clear();
            plan.refusal = PurgeRefusal::OutsideBidder;
            plan.refusedAuctionId = auction.auctionId;
            plan.refusedBidderGuid = auction.bidderGuid;
            return plan;
        }

        plan.auctionIds.push_back(auction.auctionId);
        // A listing whose item row is already gone still needs its auction row removed, so an absent
        // item guid is not a reason to skip the listing.
        if (auction.itemGuid)
            plan.itemGuids.push_back(auction.itemGuid);
    }

    SortAndDeduplicate(plan.auctionIds);
    SortAndDeduplicate(plan.itemGuids);
    return plan;
}

AuctionPurgePlan PlayerbotEconomyPurge::QueryFailedPlan()
{
    AuctionPurgePlan plan;
    plan.refusal = PurgeRefusal::QueryFailed;
    return plan;
}

char const* PlayerbotEconomyPurge::PurgeRefusalName(PurgeRefusal refusal)
{
    switch (refusal)
    {
        case PurgeRefusal::None:
            return "none";
        case PurgeRefusal::OutsideBidder:
            return "outside_bidder";
        case PurgeRefusal::QueryFailed:
            return "query_failed";
    }
    return "unknown";
}
}  // namespace PlayerbotEconomy
