/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYPURGE_H
#define PLAYERBOTS_PLAYERBOTECONOMYPURGE_H

#include <vector>

#include "Define.h"

namespace PlayerbotEconomy
{
/*
 * Auction rows survive a character deletion. Neither Player::DeleteFromDB nor AccountMgr::DeleteAccount
 * touches `auctionhouse`, while DeleteFromDB does delete the seller's `item_instance` rows. Deleting a
 * bot cohort without this leaves one `auctionhouse` row per listing pointing at an item that no longer
 * exists, which is the state a population wipe used to produce.
 *
 * This is the same ordering the Python population executor already encodes, where auctions are phase 30
 * and `item_instance` is phase 60: auctions are settled against the character before its items go.
 */
struct PurgeAuctionFact
{
    uint32 auctionId = 0;
    uint32 itemGuid = 0;
    uint32 itemOwner = 0;
    // Zero when nobody has bid. A live bid is gold already taken from the bidder, refunded by mail when
    // the auction resolves normally.
    uint32 bidderGuid = 0;
};

enum class PurgeRefusal : uint8
{
    None,
    /*
     * A character that is NOT being purged holds a live bid on a listing that is. Deleting the row would
     * take that gold with no refund mail, and a human bidder is exactly who survives a bot wipe. Refuse
     * and let an operator settle the auction rather than silently destroying someone's money.
     */
    OutsideBidder,
    /*
     * The auction query did not succeed. AzerothCore returns a null QueryResult both for a successful
     * query with no rows and for a failed one, so "no auctions" cannot be trusted on its own. Treating a
     * failure as an empty auction house would approve the purge without ever checking for an outside
     * bidder, and would then silently skip the deletion this hook exists to perform.
     */
    QueryFailed
};

struct AuctionPurgePlan
{
    std::vector<uint32> auctionIds;
    std::vector<uint32> itemGuids;
    PurgeRefusal refusal = PurgeRefusal::None;
    uint32 refusedAuctionId = 0;
    uint32 refusedBidderGuid = 0;

    [[nodiscard]] bool MayMutate() const { return refusal == PurgeRefusal::None; }
};

class PlayerbotEconomyPurge
{
public:
    /*
     * Selects the listings owned by the purged cohort. A listing owned by anyone else is not ours to
     * delete, so it is ignored rather than refused.
     */
    static AuctionPurgePlan BuildAuctionPurgePlan(std::vector<PurgeAuctionFact> const& auctions,
                                                  std::vector<uint32> const& purgedGuids);
    // A plan that refuses because the database could not be read. Kept here so the refusal vocabulary
    // lives in one place rather than being constructed ad hoc at the call site.
    static AuctionPurgePlan QueryFailedPlan();
    static char const* PurgeRefusalName(PurgeRefusal refusal);
};
}  // namespace PlayerbotEconomy

#endif
