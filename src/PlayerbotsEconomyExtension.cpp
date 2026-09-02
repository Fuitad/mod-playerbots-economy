/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <sstream>
#include <string>
#include <vector>

#include "Ai/Base/Actions/EconomyAction.h"
#include "Ai/Base/Actions/EconomyGatheringAction.h"
#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "Bot/Economy/PlayerbotEconomyPurge.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"
#include "Bot/Extension/PlayerbotExtension.h"
#include "Bot/Personality/PlayerbotCareerAdapter.h"
#include "DatabaseEnv.h"
#include "Engine.h"
#include "GameTime.h"
#include "NamedObjectContext.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"
#include "Strategy.h"
#include "WorldScript.h"

namespace
{
class PlayerbotsEconomyStrategy final : public Strategy
{
public:
    explicit PlayerbotsEconomyStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "playerbots economy"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        triggers.push_back(new TriggerNode("timer", {NextAction("economy cycle", 100.0f)}));
        triggers.push_back(new TriggerNode("far from travel target", {NextAction("move to travel target", 101.0f)}));
    }
};

class PlayerbotsEconomyActionContext final : public NamedObjectContext<Action>
{
public:
    PlayerbotsEconomyActionContext()
    {
        creators["add gathering loot"] = [](PlayerbotAI* botAI) { return new EconomyGatheringLootAction(botAI); };
        creators["economy cycle"] = [](PlayerbotAI* botAI) { return new EconomyCycleAction(botAI); };
        creators["sell"] = [](PlayerbotAI* botAI) { return new EconomySellAction(botAI); };
    }
};

class PlayerbotsEconomyStrategyContext final : public NamedObjectContext<Strategy>
{
public:
    PlayerbotsEconomyStrategyContext()
    {
        creators["playerbots economy"] = [](PlayerbotAI* botAI) { return new PlayerbotsEconomyStrategy(botAI); };
    }
};

class PlayerbotsEconomyExtension final : public PlayerbotExtension
{
public:
    void AddActionContexts(SharedNamedObjectContextList<Action>& contexts) override
    {
        contexts.Add(new PlayerbotsEconomyActionContext());
    }

    void AddStrategyContexts(SharedNamedObjectContextList<Strategy>& contexts) override
    {
        contexts.Add(new PlayerbotsEconomyStrategyContext());
    }

    void AddDefaultNonCombatStrategies(Player*, PlayerbotAI*, Engine& engine) override
    {
        // Random bot classification may not be available when the engine is built. The cycle
        // action rechecks autonomous random bot eligibility before doing any work.
        engine.addStrategy("playerbots economy", false);
    }

    bool InitializeTradeSkills(Player* player) override
    {
        if (!player || !sRandomPlayerbotMgr.IsRandomBot(player))
            return false;

        PlayerbotCareerPlan plan;
        PlayerbotCareer::EnsurePersistentPlan(player, plan);
        return true;
    }

    bool HandleBotEvent(PlayerbotAI* botAI, PlayerbotEvent const& event) override
    {
        if (event.type == PlayerbotEventType::Loot)
            EconomyGatheringLootAction::HandleLoot(botAI, event.subjectId);
        return false;
    }

    void OnBotRemoved(PlayerbotAI* botAI) override { EconomyGatheringLootAction::Remove(botAI); }

    /*
     * Validation only. The registry runs every extension's PrepareBotPurge without short-circuiting, so
     * a refusal from any other module still aborts the purge after this one returns. Deleting here would
     * destroy the auction house for a purge that never happens, which is why the deletion is in
     * OnBotPurge, once the accounts are actually gone.
     */
    bool PrepareBotPurge(std::vector<std::uint32_t> const& botGuids) override
    {
        PlayerbotEconomy::AuctionPurgePlan const plan = BuildAuctionPurgePlan(botGuids);
        if (plan.MayMutate())
            return true;

        LOG_ERROR("playerbots.economy",
                  "Refusing the bot purge ({}): auction {}, bidder {} outside the purged cohort. "
                  "Settle that auction, or fix the database error, before retrying.",
                  PlayerbotEconomy::PlayerbotEconomyPurge::PurgeRefusalName(plan.refusal), plan.refusedAuctionId,
                  plan.refusedBidderGuid);
        return false;
    }

    /*
     * Neither Player::DeleteFromDB nor AccountMgr::DeleteAccount touches `auctionhouse`, so without this
     * a wiped cohort leaves one auction row per listing pointing at an item row DeleteFromDB already
     * removed. The item guids are collected here as well, because a listing whose item was not owned by
     * the seller would otherwise survive as an orphan of its own.
     */
    void OnBotPurge(std::vector<std::uint32_t> const& botGuids) override
    {
        PlayerbotEconomy::AuctionPurgePlan const plan = BuildAuctionPurgePlan(botGuids);
        if (!plan.MayMutate())
        {
            LOG_ERROR("playerbots.economy", "Auction rows were left in place: the purge plan refused with {}.",
                      PlayerbotEconomy::PlayerbotEconomyPurge::PurgeRefusalName(plan.refusal));
            return;
        }
        if (plan.auctionIds.empty())
            return;

        CharacterDatabase.DirectExecute("DELETE FROM auctionhouse WHERE id IN ({})", JoinIds(plan.auctionIds));
        /*
         * Scoped to items the purged cohort actually owns. The auction row's item pointer is not deletion
         * authority: a stale or corrupt listing can name an item that now belongs to a character who
         * survives the purge, and deleting by that pointer alone would destroy it. In the healthy case
         * Player::DeleteFromDB has already removed these rows, so this only catches an item held under a
         * different guid inside the same cohort.
         */
        if (!plan.itemGuids.empty())
            CharacterDatabase.DirectExecute("DELETE FROM item_instance WHERE guid IN ({}) AND owner_guid IN ({})",
                                            JoinIds(plan.itemGuids), JoinIds(botGuids));

        /*
         * DirectExecute reports nothing back and OnBotPurge cannot fail, so the only honest completion
         * signal is to read the rows again. Claiming a count of what was planned would report success for
         * a delete that never happened.
         */
        if (QueryResult const remaining =
                CharacterDatabase.Query("SELECT COUNT(*) FROM auctionhouse WHERE itemowner IN ({})", JoinIds(botGuids)))
        {
            uint64 const left = remaining->Fetch()[0].Get<uint64>();
            if (left)
                LOG_ERROR("playerbots.economy",
                          "{} auction listings for the purged cohort survived the delete. They are now orphaned "
                          "and must be removed by hand.",
                          left);
            else
                LOG_INFO("playerbots.economy", "Deleted {} auction listings for {} purged bots; none remain.",
                         plan.auctionIds.size(), botGuids.size());
        }
        else
        {
            LOG_ERROR("playerbots.economy",
                      "Could not confirm the auction delete for {} purged bots. Verify `auctionhouse` by hand.",
                      botGuids.size());
        }
    }

private:
    static std::string JoinIds(std::vector<std::uint32_t> const& ids)
    {
        std::ostringstream joined;
        for (std::size_t index = 0; index < ids.size(); ++index)
        {
            if (index)
                joined << ',';
            joined << ids[index];
        }
        return joined.str();
    }

    static PlayerbotEconomy::AuctionPurgePlan BuildAuctionPurgePlan(std::vector<std::uint32_t> const& botGuids)
    {
        if (botGuids.empty())
            return {};

        std::string const cohort = JoinIds(botGuids);

        /*
         * COUNT(*) returns exactly one row whenever the query succeeds, so a null result here means the
         * query itself failed. Without this probe an unreadable database is indistinguishable from an
         * empty auction house, and the purge would be approved with the bidder check never performed.
         */
        QueryResult const expected =
            CharacterDatabase.Query("SELECT COUNT(*) FROM auctionhouse WHERE itemowner IN ({})", cohort);
        if (!expected)
            return PlayerbotEconomy::PlayerbotEconomyPurge::QueryFailedPlan();

        uint64 const expectedRows = expected->Fetch()[0].Get<uint64>();
        if (!expectedRows)
            return {};

        std::vector<PlayerbotEconomy::PurgeAuctionFact> auctions;
        QueryResult result = CharacterDatabase.Query(
            "SELECT id, itemguid, itemowner, buyguid FROM auctionhouse WHERE itemowner IN ({})", cohort);
        if (!result)
            return PlayerbotEconomy::PlayerbotEconomyPurge::QueryFailedPlan();

        do
        {
            Field* fields = result->Fetch();
            auctions.push_back({.auctionId = fields[0].Get<uint32>(),
                                .itemGuid = fields[1].Get<uint32>(),
                                .itemOwner = fields[2].Get<uint32>(),
                                .bidderGuid = fields[3].Get<uint32>()});
        } while (result->NextRow());

        // A short read is a partial view of the cohort's listings, which would hide an outside bidder.
        if (auctions.size() != expectedRows)
            return PlayerbotEconomy::PlayerbotEconomyPurge::QueryFailedPlan();

        return PlayerbotEconomy::PlayerbotEconomyPurge::BuildAuctionPurgePlan(auctions, botGuids);
    }
};

class PlayerbotsEconomyWorldScript final : public WorldScript
{
public:
    PlayerbotsEconomyWorldScript()
        : WorldScript("PlayerbotsEconomyWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_UPDATE})
    {
    }

    void OnAfterConfigLoad(bool) override { ReloadPlayerbotEconomyConfig(); }

    void OnBeforeWorldInitialized() override
    {
        PlayerbotEconomy::LoadPlayerbotEconomyMarketFromDatabase(static_cast<uint64>(GameTime::GetGameTime().count()));
        PlayerbotEconomy::LoadPlayerbotMaterialCommitmentsFromDatabase();
    }

    void OnUpdate(uint32) override
    {
        PlayerbotEconomy::UpdatePlayerbotEconomyMarketDatabaseCallbacks();
        PlayerbotEconomy::UpdatePlayerbotMaterialCommitmentDatabaseCallbacks();
        PlayerbotCareer::UpdatePersistentPlans(GameTime::GetGameTimeMS().count());
    }
};
}  // namespace

void AddPlayerbotsEconomyScripts()
{
    static PlayerbotsEconomyExtension extension;
    GetPlayerbotExtensionRegistry().Register(extension);
    new PlayerbotsEconomyWorldScript();
}
