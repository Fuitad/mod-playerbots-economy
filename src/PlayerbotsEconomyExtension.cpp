/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Ai/Base/Actions/EconomyAction.h"
#include "Ai/Base/Actions/EconomyGatheringAction.h"
#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"
#include "Bot/Extension/PlayerbotExtension.h"
#include "Bot/Personality/PlayerbotCareerAdapter.h"
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
