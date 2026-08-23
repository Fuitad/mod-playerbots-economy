/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyRuntime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "Ai/Base/Actions/AttackAction.h"
#include "Ai/Base/Actions/BuyAction.h"
#include "Ai/Base/Actions/ChooseTravelTargetAction.h"
#include "Ai/Base/Actions/EquipAction.h"
#include "Ai/Base/Actions/ListSpellsAction.h"
#include "Ai/Base/Actions/SellAction.h"
#include "Ai/Base/Actions/UseItemAction.h"
#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyConsumption.h"
#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "Bot/Economy/PlayerbotEconomyMail.h"
#include "Bot/Economy/PlayerbotEconomyMarket.h"
#include "Bot/Economy/PlayerbotEconomyTrace.h"
#include "Bot/Economy/PlayerbotEconomyTravel.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentEncoding.h"
#include "Bot/Economy/PlayerbotProfessionCapability.h"
#include "Bot/Personality/PlayerbotCareerAdapter.h"
#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "Bot/Personality/PlayerbotCareerProgression.h"
#include "Bot/Personality/PlayerbotPersonalityMgr.h"
#include "BudgetValues.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "LootObjectStack.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "TravelNode.h"
#include "World.h"

using namespace PlayerbotEconomy;

bool CanClearTimedOutProgressionWorkOrder(uint32 storedWorkOrderSpellId, uint32 progressionRecipeSpellId,
                                          uint32 characterGuid, std::vector<EconomyAssignment> const& claims)
{
    if (!storedWorkOrderSpellId || storedWorkOrderSpellId != progressionRecipeSpellId)
        return false;

    return std::none_of(claims.begin(), claims.end(),
                        [storedWorkOrderSpellId, characterGuid](EconomyAssignment const& claim)
                        {
                            return claim.characterGuid == characterGuid && claim.kind == EconomyClaimKind::Production &&
                                   claim.state == EconomyClaimState::Leased &&
                                   claim.recipeSpellId == storedWorkOrderSpellId;
                        });
}

namespace
{
constexpr char PROFESSION_WORK_ORDER_EVENT[] = "profession work order";
constexpr uint64 POSITION_ID_NAMESPACE = 0x6f4a7d19c3b258e1ULL;

bool IsGatheringProfessionSkill(uint16 skillId)
{
    return skillId == SKILL_HERBALISM || skillId == SKILL_MINING || skillId == SKILL_SKINNING;
}

bool IsUniversalProgressionSkill(uint16 skillId) { return skillId == SKILL_COOKING || skillId == SKILL_FIRST_AID; }

// Fishing advances by fishing. No trainer anywhere sells a fishing recipe, so asking for one can only
// produce a trip that finds nothing to buy, over and over. Its ranks are still bought at a trainer.
bool SkillAdvancesThroughRecipes(uint16 skillId) { return skillId != SKILL_FISHING; }

// Cooking recipes need a lit fire in range. Every bot that learns cooking also learns Basic Campfire,
// so a bot holding meat but standing nowhere near a fire can make its own instead of never cooking.
constexpr uint32 BASIC_CAMPFIRE_SPELL_ID = 818u;
constexpr uint32 DISENCHANT_SPELL_ID = 13262u;

// Disenchant loot id to the items it can yield. Read once from disenchant_loot_template: the in-memory
// loot store keeps grouped entries private, and every disenchant entry is grouped. A zero chance row
// inside a group takes whatever probability its siblings leave over, so it stays a possible yield.
std::unordered_map<uint32, std::vector<uint32>> const& DisenchantYields()
{
    static std::unordered_map<uint32, std::vector<uint32>> const yields = []
    {
        std::unordered_map<uint32, std::vector<uint32>> result;
        QueryResult rows = WorldDatabase.Query(
            "SELECT Entry, Item FROM disenchant_loot_template "
            "WHERE Reference = 0 AND QuestRequired = 0 AND Chance >= 0 AND (LootMode & 1) <> 0");
        if (rows)
        {
            do
            {
                Field* fields = rows->Fetch();
                result[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
            } while (rows->NextRow());
        }
        return result;
    }();
    return yields;
}

// A bot holding a fishing pole has its real weapon, and possibly its shield, stowed in the bags. Nothing
// may be disenchanted in that state: the bag scan cannot tell stowed gear from loot, and breaking the
// weapon would be far worse than a missed craft. Pierre set this as a total blocker on 2026-08-22.
bool HoldsFishingPole(Player* bot)
{
    Item const* const mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    return mainHand && mainHand->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
           mainHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
}

// What this bot would get from breaking the item: empty unless it is a green the bot can disenchant
// at its current skill and is in a state to disenchant at all. Greys never qualify: they carry no
// disenchant loot.
std::vector<uint32> const& BotDisenchantYields(Player* bot, ItemTemplate const* proto)
{
    static std::vector<uint32> const none;
    if (HoldsFishingPole(bot))
        return none;
    if (!proto || !proto->DisenchantID || proto->Quality != ITEM_QUALITY_UNCOMMON ||
        (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON) || !bot->HasSkill(SKILL_ENCHANTING) ||
        (proto->RequiredDisenchantSkill > 0 &&
         static_cast<uint32>(proto->RequiredDisenchantSkill) > bot->GetSkillValue(SKILL_ENCHANTING)))
    {
        return none;
    }
    auto const& yields = DisenchantYields();
    auto const entry = yields.find(proto->DisenchantID);
    return entry != yields.end() ? entry->second : none;
}

bool BotDisenchantYieldsItem(Player* bot, ItemTemplate const* proto, uint32 itemId)
{
    std::vector<uint32> const& yields = BotDisenchantYields(bot, proto);
    return std::find(yields.begin(), yields.end(), itemId) != yields.end();
}

// The green a bot would break for a reagent: a bag item it can disenchant whose loot table lists the
// reagent, and that the bot would not rather wear. Cheapest vendor price first, so the greens worth
// auctioning stay on the market side.
Item* SelectDisenchantSource(PlayerbotAI* botAI, uint32 reagentItemId,
                             std::unordered_set<uint64> const& controlledItemGuids)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    Item* selected = nullptr;
    auto const consider = [&](Item* item)
    {
        if (!item || item->m_lootGenerated || controlledItemGuids.contains(item->GetGUID().GetCounter()) ||
            !BotDisenchantYieldsItem(bot, item->GetTemplate(), reagentItemId))
        {
            return;
        }
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
            return;
        if (!selected || item->GetTemplate()->SellPrice < selected->GetTemplate()->SellPrice)
            selected = item;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* const container = bot->GetBagByPos(bagSlot);
        for (uint32 slot = 0u; container && slot < container->GetBagSize(); ++slot)
            consider(container->GetItemByPos(slot));
    }
    return selected;
}

// Disenchant opens a loot window on the broken item. The dust only lands in the bags once every slot
// is taken, and the core destroys the item on release. Nothing else in Playerbots does that pickup,
// so an untended window would leave the bot holding a half-broken green forever.
void CollectProfessionLoot(Player* bot)
{
    ObjectGuid const lootGuid = bot->GetLootGUID();
    if (!lootGuid.IsItem())
        return;
    Item* const item = bot->GetItemByGuid(lootGuid);
    if (!item || (item->loot.loot_type != LOOT_DISENCHANTING && item->loot.loot_type != LOOT_MILLING))
        return;
    // Storing the last slot releases the loot and destroys the item, so re-check the window each step.
    for (uint8 slot = 0u; bot->GetLootGUID() == lootGuid && slot < item->loot.items.size(); ++slot)
    {
        if (item->loot.items[slot].is_looted)
            continue;
        InventoryResult message = EQUIP_ERR_OK;
        bot->StoreLootItem(slot, &item->loot, message);
    }
    if (bot->GetLootGUID() == lootGuid)
        bot->GetSession()->DoLootRelease(lootGuid);
}

constexpr uint32 MILLING_SPELL_ID = 51005u;
// The core mills exactly this many herbs of one kind per cast.
constexpr uint32 MILLING_HERBS_PER_CAST = 5u;

// Herb item id to the pigments milling it yields. milling_loot_template keys on the herb and every row
// of it is a reference into reference_loot_template, so the read joins the two; a direct row is kept
// for completeness should one ever appear.
std::unordered_map<uint32, std::vector<uint32>> const& MillingYields()
{
    static std::unordered_map<uint32, std::vector<uint32>> const yields = []
    {
        std::unordered_map<uint32, std::vector<uint32>> result;
        QueryResult rows = WorldDatabase.Query(
            "SELECT m.Entry, r.Item FROM milling_loot_template m "
            "JOIN reference_loot_template r ON r.Entry = m.Reference "
            "WHERE m.Reference <> 0 AND m.QuestRequired = 0 AND (m.LootMode & 1) <> 0 "
            "UNION SELECT Entry, Item FROM milling_loot_template "
            "WHERE Reference = 0 AND QuestRequired = 0 AND (LootMode & 1) <> 0");
        if (rows)
        {
            do
            {
                Field* fields = rows->Fetch();
                result[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
            } while (rows->NextRow());
        }
        return result;
    }();
    return yields;
}

// Pigment item id to the herbs that mill into it, the inverse of MillingYields.
std::unordered_map<uint32, std::vector<uint32>> const& MillingInputs()
{
    static std::unordered_map<uint32, std::vector<uint32>> const inputs = []
    {
        std::unordered_map<uint32, std::vector<uint32>> result;
        for (auto const& [herbId, pigments] : MillingYields())
            for (uint32 pigment : pigments)
                result[pigment].push_back(herbId);
        return result;
    }();
    return inputs;
}

// What this bot would get from milling the herb: empty unless the herb is millable at its current
// Inscription skill, the same gates the core applies to the cast.
std::vector<uint32> const& BotMillingYields(Player* bot, ItemTemplate const* proto)
{
    static std::vector<uint32> const none;
    if (!proto || !proto->HasFlag(ITEM_FLAG_IS_MILLABLE) || !bot->HasSkill(SKILL_INSCRIPTION) ||
        proto->RequiredSkillRank > bot->GetSkillValue(SKILL_INSCRIPTION))
    {
        return none;
    }
    auto const& yields = MillingYields();
    auto const entry = yields.find(proto->ItemId);
    return entry != yields.end() ? entry->second : none;
}

bool BotMillingYieldsItem(Player* bot, ItemTemplate const* proto, uint32 itemId)
{
    std::vector<uint32> const& yields = BotMillingYields(bot, proto);
    return std::find(yields.begin(), yields.end(), itemId) != yields.end();
}

// The herb stack a bot would mill for a pigment: a bag stack of at least one cast's worth whose loot
// lists the pigment. Lowest skill herb first, so the rarer herbs stay for their own recipes or the market.
Item* SelectMillingSource(PlayerbotAI* botAI, uint32 reagentItemId,
                          std::unordered_set<uint64> const& controlledItemGuids)
{
    Player* const bot = botAI->GetBot();
    Item* selected = nullptr;
    auto const consider = [&](Item* item)
    {
        if (!item || item->m_lootGenerated || item->GetCount() < MILLING_HERBS_PER_CAST ||
            controlledItemGuids.contains(item->GetGUID().GetCounter()) ||
            !BotMillingYieldsItem(bot, item->GetTemplate(), reagentItemId))
        {
            return;
        }
        if (!selected || item->GetTemplate()->RequiredSkillRank < selected->GetTemplate()->RequiredSkillRank)
            selected = item;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* const container = bot->GetBagByPos(bagSlot);
        for (uint32 slot = 0u; container && slot < container->GetBagSize(); ++slot)
            consider(container->GetItemByPos(slot));
    }
    return selected;
}
// Yards short of a mailbox, auctioneer, vendor or trainer that an economy walk stops at.
constexpr float APPROACH_STAND_OFF_DISTANCE = 3.0f;

bool IsEnchantRecipeSpell(SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->Effects[EFFECT_0].Effect == SPELL_EFFECT_ENCHANT_ITEM;
}

// The piece of the bot's own gear an enchant can go on: the core's own fit check decides class, subclass
// and slot, and a piece already carrying this very enchant is skipped so the skill-up is not wasted on
// an overwrite of itself.
Item* SelectOwnGearEnchantTarget(Player* bot, SpellInfo const* spellInfo)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* const item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->IsFitToSpellRequirements(spellInfo))
            continue;
        if (item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) ==
            static_cast<uint32>(spellInfo->Effects[EFFECT_0].MiscValue))
            continue;
        return item;
    }
    return nullptr;
}

bool IsCookingRecipeSpell(uint32 spellId)
{
    SkillLineAbilityMapBounds const skillBounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto ability = skillBounds.first; ability != skillBounds.second; ++ability)
    {
        SkillLineAbilityEntry const* skill = ability->second;
        if (skill && skill->SkillLine == SKILL_COOKING)
            return true;
    }
    return false;
}

// The core's own verdict on a craft, including the two requirements the bags alone cannot
// satisfy: a spell focus object in range (forge, anvil) and a tool of the recipe's TotemCategory
// (mining pick, blacksmith hammer, enchanting rod).
SpellCastResult CraftCastResult(Player* bot, SpellInfo const* spellInfo)
{
    Spell* spell = new Spell(bot, spellInfo, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    spell->m_targets.SetUnitTarget(bot);
    SpellCastResult const result = spell->CheckCast(true);
    delete spell;
    return result;
}

// Recipes that depend on the surroundings or a tool are judged at the craft step, never while the
// snapshot is built: a passing cast check away from a forge is impossible, and a failing one says
// nothing about whether the recipe is worth a claim.
bool CraftNeedsFocusOrTool(SpellInfo const* spellInfo)
{
    return spellInfo && (spellInfo->RequiresSpellFocus || spellInfo->TotemCategory[0] || spellInfo->TotemCategory[1]);
}

std::unordered_set<uint32> ApplicableUnlimitedGoldVendorItems(Player* bot);

// The cheapest vendor-sold tool of a TotemCategory the spell needs and the bot does not own. Tools are
// never granted: a miner buys its pick, a blacksmith its hammer, exactly as a crafted tool (an
// enchanting rod) is crafted. Empty when every required tool is owned or none is sold by a vendor.
std::optional<uint32> MissingVendorTool(Player* bot, SpellInfo const* spellInfo)
{
    static std::unordered_map<uint32, std::vector<ItemTemplate const*>> const toolsByCategory = []
    {
        std::unordered_map<uint32, std::vector<ItemTemplate const*>> result;
        if (std::vector<ItemTemplate*> const* templates = sObjectMgr->GetItemTemplateStoreFast())
        {
            for (ItemTemplate const* item : *templates)
            {
                if (item && item->TotemCategory && item->BuyPrice > 0)
                    result[item->TotemCategory].push_back(item);
            }
        }
        for (auto& [category, items] : result)
        {
            (void)category;
            std::sort(items.begin(), items.end(),
                      [](ItemTemplate const* left, ItemTemplate const* right) {
                          return left->BuyPrice != right->BuyPrice ? left->BuyPrice < right->BuyPrice
                                                                   : left->ItemId < right->ItemId;
                      });
        }
        return result;
    }();

    if (!bot || !spellInfo)
        return std::nullopt;
    std::optional<std::unordered_set<uint32>> vendorItems;
    for (uint32 const category : spellInfo->TotemCategory)
    {
        if (!category || bot->HasItemTotemCategory(category))
            continue;
        auto const tools = toolsByCategory.find(category);
        if (tools == toolsByCategory.end())
            continue;
        if (!vendorItems)
            vendorItems = ApplicableUnlimitedGoldVendorItems(bot);
        for (ItemTemplate const* tool : tools->second)
        {
            if (vendorItems->contains(tool->ItemId))
                return tool->ItemId;
        }
    }
    return std::nullopt;
}

std::string ReagentGroup(uint32 itemId) { return "reagent:" + std::to_string(itemId); }

std::string ItemGroup(uint32 itemId) { return "item:" + std::to_string(itemId); }

bool StartRecipeLearning(PlayerbotAI* botAI, Item* item, uint32 expectedSpellId)
{
    if (!botAI || !expectedSpellId)
        return false;

    Player* const bot = botAI->GetBot();
    if (bot->HasSpell(expectedSpellId) || !item)
        return false;

    PlayerbotRecipeCandidate const recipe = PlayerbotCareer::DescribeRecipe(item->GetTemplate(), bot, 0u);
    if (recipe.recipeSpellId != expectedSpellId || recipe.isKnown || !recipe.isUsable ||
        bot->CanUseItem(item) != EQUIP_ERR_OK)
    {
        return false;
    }

    SpellCastTargets targets;
    targets.SetUnitTarget(bot);
    bot->CastItemUseSpell(item, targets, 1u, 0u);
    return true;
}

EconomyRiskConfiguration MarketRiskConfiguration()
{
    return {
        .enabled = sPlayerbotEconomyConfig.marketMakingEnabled,
        .perGroupExposurePercent = sPlayerbotEconomyConfig.marketMakingPerGroupExposurePercent,
        .totalExposurePercent = sPlayerbotEconomyConfig.marketMakingTotalExposurePercent,
        .minimumEvidence = sPlayerbotEconomyConfig.marketMakingMinimumEvidence,
        .holdingHorizonSeconds = sPlayerbotEconomyConfig.marketMakingHoldingHorizonSeconds,
        .maximumRelistAttempts = sPlayerbotEconomyConfig.marketMakingMaximumRelistAttempts,
        .cooldownSeconds = sPlayerbotEconomyConfig.marketMakingCooldownSeconds,
    };
}

std::string PositionPublicId(uint32 traderGuid, uint64 itemGuid, uint64 now)
{
    uint64 const high = PlayerbotPersonality::SplitMix64(itemGuid ^ (static_cast<uint64>(traderGuid) << 32u) ^ now ^
                                                         POSITION_ID_NAMESPACE);
    uint64 const low = PlayerbotPersonality::SplitMix64(high ^ POSITION_ID_NAMESPACE);
    return Acore::StringFormat("{:016x}{:04x}", high, low & 0xffffu);
}

struct AuctionMailDetails
{
    uint32 itemId = 0;
    MailAuctionAnswers response = AUCTION_OUTBIDDED;
    uint32 auctionId = 0;
    uint32 quantity = 0;
    uint64 bid = 0;
    uint64 buyout = 0;
    uint64 deposit = 0;
    uint64 cut = 0;
    uint32 bidderGuid = 0;
};

std::optional<std::vector<uint64>> ParseUnsignedFields(std::string_view text, bool firstFieldHex)
{
    std::vector<uint64> fields;
    while (!text.empty())
    {
        std::size_t const separator = text.find(':');
        std::string_view const field = text.substr(0, separator);
        if (field.empty())
            return std::nullopt;

        uint64 value = 0u;
        int const base = firstFieldHex && fields.empty() ? 16 : 10;
        auto const parsed = std::from_chars(field.data(), field.data() + field.size(), value, base);
        if (parsed.ec != std::errc() || parsed.ptr != field.data() + field.size())
            return std::nullopt;
        fields.push_back(value);
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1u);
    }
    return fields;
}

std::optional<AuctionMailDetails> ParseAuctionMail(Mail const* mail)
{
    if (!mail || mail->messageType != MAIL_AUCTION)
        return std::nullopt;
    std::optional<std::vector<uint64>> const subject = ParseUnsignedFields(mail->subject, false);
    std::optional<std::vector<uint64>> const body = ParseUnsignedFields(mail->body, true);
    if (!subject || subject->size() != 5u || !body || body->size() < 5u || (*subject)[0] > UINT32_MAX ||
        (*subject)[2] > AUCTION_SALE_PENDING || (*subject)[3] > UINT32_MAX || (*subject)[4] > UINT32_MAX ||
        (*body)[1] > UINT32_MAX || (*body)[2] > UINT32_MAX || (*body)[3] > UINT32_MAX || (*body)[4] > UINT32_MAX)
    {
        return std::nullopt;
    }
    return AuctionMailDetails{
        .itemId = static_cast<uint32>((*subject)[0]),
        .response = static_cast<MailAuctionAnswers>((*subject)[2]),
        .auctionId = static_cast<uint32>((*subject)[3]),
        .quantity = static_cast<uint32>((*subject)[4]),
        .bid = (*body)[1],
        .buyout = (*body)[2],
        .deposit = (*body)[3],
        .cut = (*body)[4],
        .bidderGuid = (*body)[0] <= UINT32_MAX ? static_cast<uint32>((*body)[0]) : 0u,
    };
}

std::string TraceChainForActor(uint32 actorGuid, uint64 now)
{
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyActorChainObservation const observation = coordinator.ObserveActor(actorGuid, now);
    if (!observation.chainPublicId.empty())
        return observation.chainPublicId;

    if (!observation.available || !observation.marketId)
        return {};
    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(now);
    auto const chain =
        std::find_if(snapshot.chains.rbegin(), snapshot.chains.rend(), [&observation](EconomyChain const& candidate)
                     { return candidate.marketId == observation.marketId && candidate.group == observation.group; });
    return chain == snapshot.chains.rend() ? std::string{} : chain->publicId;
}

uint32 TraceActorIfKnown(uint32 actorGuid, uint64 now)
{
    if (!actorGuid)
        return 0u;
    EconomyCoordinatorSnapshot const snapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    auto const actor =
        std::find_if(snapshot.actors.begin(), snapshot.actors.end(),
                     [actorGuid](EconomyActorFacts const& candidate) { return candidate.characterGuid == actorGuid; });
    return actor == snapshot.actors.end() ? 0u : actorGuid;
}

std::optional<EconomyTraceEvent> TraceEventForAuction(uint32 actorGuid, uint32 auctionId, EconomyTraceKind kind)
{
    EconomyTraceSnapshot const snapshot = GetPlayerbotEconomyTrace().Snapshot();
    auto const event = std::find_if(snapshot.events.rbegin(), snapshot.events.rend(),
                                    [actorGuid, auctionId, kind](EconomyTraceEvent const& candidate) {
                                        return candidate.actorGuid == actorGuid &&
                                               candidate.correlationAuctionId == auctionId && candidate.kind == kind;
                                    });
    return event == snapshot.events.rend() ? std::nullopt : std::optional<EconomyTraceEvent>(*event);
}

EconomyFinalUseKind TraceFinalUse(FinishedGoodUse use)
{
    switch (use)
    {
        case FinishedGoodUse::Equip:
            return EconomyFinalUseKind::Equipped;
        case FinishedGoodUse::SetAmmunition:
            return EconomyFinalUseKind::AmmunitionSet;
        case FinishedGoodUse::Consume:
            return EconomyFinalUseKind::Consumed;
        case FinishedGoodUse::Apply:
            return EconomyFinalUseKind::Applied;
        case FinishedGoodUse::Recover:
            return EconomyFinalUseKind::Recovered;
    }
    return EconomyFinalUseKind::Lost;
}

bool IsFinishedGoodUsage(ItemUsage usage)
{
    return usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_AMMO ||
           usage == ITEM_USAGE_USE;
}

bool IsUsefulCraftOutput(ItemUsage usage)
{
    return usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_AMMO ||
           usage == ITEM_USAGE_QUEST || usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_USE;
}

uint32 CraftOutputPriority(ItemUsage usage)
{
    if (IsUsefulCraftOutput(usage))
        return 0;

    if (usage == ITEM_USAGE_KEEP)
        return 1;

    if (usage == ITEM_USAGE_AH)
        return 2;

    return 3;
}

uint32 AuctionMarketId(uint32 factionTemplateId)
{
    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
        return static_cast<uint32>(AuctionHouseId::Neutral);

    AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(factionTemplateId);
    return entry ? entry->houseId : 0u;
}

std::optional<GatheringProfession> GatheringProfessionForSkill(uint32 skillId)
{
    if (skillId == SKILL_HERBALISM)
        return GatheringProfession::Herbalism;
    if (skillId == SKILL_MINING)
        return GatheringProfession::Mining;
    if (skillId == SKILL_SKINNING)
        return GatheringProfession::Skinning;
    if (skillId == HUNTING_SKILL_ID)
        return GatheringProfession::Hunting;
    return std::nullopt;
}

// A hunt needs no skill and no tool; every other source needs the learned skill and its tool.
bool ActorCanWorkSource(Player const* bot, uint32 skillId)
{
    return skillId == HUNTING_SKILL_ID || (bot->HasSkill(skillId) && HasRequiredGatheringTool(bot, skillId));
}

std::optional<uint32> GatheringSkillForItem(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return std::nullopt;
    bool const minedItem = itemTemplate->Class == ITEM_CLASS_TRADE_GOODS &&
                           itemTemplate->SubClass == ITEM_SUBCLASS_METAL_STONE &&
                           sPlayerbotEconomyTravelCatalog.MiningNodeYieldsItem(itemTemplate->ItemId);
    return PlayerbotEconomyGathering::GatheringSkillForTradeGood(itemTemplate->Class, itemTemplate->SubClass,
                                                                 minedItem);
}

uint32 PlannedInputCount(EconomySnapshot const& snapshot, uint32 itemId)
{
    auto const inventory = std::find_if(snapshot.inventory.begin(), snapshot.inventory.end(),
                                        [itemId](InventoryCount const& item) { return item.itemId == itemId; });
    if (inventory == snapshot.inventory.end())
        return 0u;
    return inventory->count + inventory->mailCount + inventory->purchasedCount + inventory->committedCount;
}

std::unordered_set<uint32> ApplicableUnlimitedGoldVendorItems(Player* bot)
{
    // The economy catalog indexes vendor spawns itself. TravelMgr's RPG destination table, which
    // this used to walk, is never loaded in this playerbots fork, so the scan found no vendor at all.
    return sPlayerbotEconomyTravelCatalog.ApplicableUnlimitedGoldVendorItems(bot);
}

struct GatheringOpportunity
{
    GatheringProfession profession = GatheringProfession::Herbalism;
    uint32 skillId = 0;
    uint32 itemId = 0;
    uint32 activeUncoveredDemand = 0;
    uint32 spellId = 0;
    bool coordinatorBacklog = false;
};

struct RecipeDeficit
{
    uint32 itemId = 0;
    uint32 quantity = 0;
    uint32 demandQuantity = 0;
    uint32 spellId = 0;
};

std::optional<RecipeDeficit> NextRecipeDeficit(EconomySnapshot const& snapshot)
{
    std::vector<RecipeCandidate const*> recipes;
    recipes.reserve(snapshot.recipes.size());
    for (RecipeCandidate const& recipe : snapshot.recipes)
        recipes.push_back(&recipe);
    std::stable_sort(recipes.begin(), recipes.end(),
                     [&snapshot](RecipeCandidate const* left, RecipeCandidate const* right) {
                         return left->spellId == snapshot.preferredRecipeSpellId &&
                                right->spellId != snapshot.preferredRecipeSpellId;
                     });

    for (RecipeCandidate const* recipe : recipes)
    {
        for (ReagentRequirement const& reagent : recipe->reagents)
        {
            if (reagent.unlimitedGoldVendorSupply)
                continue;

            uint32 const planned = PlannedInputCount(snapshot, reagent.itemId);
            if (planned < reagent.count)
                return RecipeDeficit{reagent.itemId, reagent.count - planned, reagent.count, recipe->spellId};
        }
    }
    return std::nullopt;
}

std::optional<GatheringOpportunity> DeficitGatheringOpportunity(Player const* bot, EconomySnapshot const& snapshot,
                                                                EconomyDecision const& decision)
{
    auto const build = [bot](uint32 itemId, uint32 quantity, uint32 spellId) -> std::optional<GatheringOpportunity>
    {
        // A gathering item is gathered with its skill; anything else is hunted from the creatures that drop
        // it, the same way the progression path sources a mob drop.
        std::optional<uint32> const gatheringSkill = GatheringSkillForItem(sObjectMgr->GetItemTemplate(itemId));
        uint32 const skillId = gatheringSkill.value_or(HUNTING_SKILL_ID);
        if (!ActorCanWorkSource(bot, skillId))
            return std::nullopt;
        std::optional<GatheringProfession> const profession = GatheringProfessionForSkill(skillId);
        if (!profession || !quantity)
            return std::nullopt;
        return GatheringOpportunity{*profession, skillId, itemId, quantity, spellId};
    };

    if (decision.phase == EconomyPhase::BuyReagent)
    {
        if (PlayerbotEconomyPolicy::IsKnownRecipeOutput(snapshot, decision.itemId))
            return std::nullopt;
        return build(decision.itemId, decision.count, decision.spellId);
    }
    if (decision.phase != EconomyPhase::None)
        return std::nullopt;

    std::optional<RecipeDeficit> const deficit = NextRecipeDeficit(snapshot);
    if (deficit && PlayerbotEconomyPolicy::IsKnownRecipeOutput(snapshot, deficit->itemId))
        return std::nullopt;
    return deficit ? build(deficit->itemId, deficit->quantity, deficit->spellId) : std::nullopt;
}

std::optional<GatheringOpportunity> CoordinatorGatheringOpportunity(Player const* bot,
                                                                    EconomyCoordinatorSnapshot const& snapshot,
                                                                    EconomySnapshot const& economy, uint32 marketId)
{
    for (EconomyDemandGap const& gap : snapshot.gaps)
    {
        if (gap.marketId != marketId || gap.group.kind != EconomySubstitutionKind::ExactReagent ||
            !gap.group.exactItemId || !gap.remainingQuantity)
        {
            continue;
        }
        if (PlayerbotEconomyPolicy::IsKnownRecipeOutput(economy, gap.group.exactItemId))
            continue;
        uint32 const skillId =
            GatheringSkillForItem(sObjectMgr->GetItemTemplate(gap.group.exactItemId)).value_or(HUNTING_SKILL_ID);
        if (!ActorCanWorkSource(bot, skillId))
            continue;
        std::optional<GatheringProfession> const profession = GatheringProfessionForSkill(skillId);
        if (profession)
            return GatheringOpportunity{*profession, skillId, gap.group.exactItemId, gap.remainingQuantity, 0u, true};
    }
    return std::nullopt;
}

uint8 EconomyAffinity(uint32 characterGuid)
{
    std::optional<PlayerbotPersonalityProfile> const personality = sPlayerbotPersonalityMgr.GetOrCreate(characterGuid);
    return personality ? personality->economyAffinity : 0u;
}

uint8 GatheringAffinity(uint32 characterGuid)
{
    std::optional<PlayerbotPersonalityProfile> const personality = sPlayerbotPersonalityMgr.GetOrCreate(characterGuid);
    return personality ? personality->gatheringAffinity : 0u;
}

uint32 ActorSelfReservation(EconomyCoordinatorSnapshot const& snapshot, uint32 characterGuid,
                            EconomySubstitutionGroup const& group)
{
    auto const actor =
        std::find_if(snapshot.actors.begin(), snapshot.actors.end(), [characterGuid](EconomyActorFacts const& candidate)
                     { return candidate.characterGuid == characterGuid; });
    if (actor == snapshot.actors.end())
        return 0u;

    uint64 demand = 0u;
    uint64 supply = 0u;
    for (EconomyDemandFact const& fact : actor->demands)
    {
        if (fact.group == group)
            demand += fact.quantity;
    }
    for (EconomySupplyFact const& fact : actor->supplies)
    {
        if (fact.group == group)
            supply += fact.quantity;
    }
    uint64 const residual = demand > supply ? demand - supply : 0u;
    return static_cast<uint32>(std::min<uint64>(residual, std::numeric_limits<uint32>::max()));
}

uint32 StorableGatheringQuantity(Player const* bot, uint32 itemId, uint32 requestedQuantity)
{
    if (!bot || !itemId || !requestedQuantity)
        return 0u;

    ItemPosCountVec destinations;
    uint32 unavailable = 0u;
    InventoryResult const result =
        bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, itemId, requestedQuantity, &unavailable);
    if (result == EQUIP_ERR_OK)
        return requestedQuantity;
    return unavailable < requestedQuantity ? requestedQuantity - unavailable : 0u;
}

struct RuntimeGatheringCandidate
{
    DedicatedGatheringCandidate policy;
    GatheringTravelDestination* destination = nullptr;
    WorldPosition* initialPoint = nullptr;
    uint32 outboundSeconds = 0;
    uint32 activityBudgetSeconds = 0;
    uint32 remainingDedicatedActivitySeconds = 0;
    uint32 destinationYieldBasisPoints = 0;
    uint32 availableResourceCount = 0;
    uint32 authoritativeInteractionSeconds = 0;
    uint64 observedGatheredQuantity = 0;
    uint64 observedResourceAttempts = 0;
    uint64 observedResourceSeconds = 0;
};

// Cold start estimate for one hunting kill: walk up, fight a creature at or below the bot's level, loot.
// Observed trip history blends this out the same way it does a gathering cast time.
constexpr uint32 HUNTING_KILL_SECONDS = 20u;

uint32 GatheringInteractionSeconds(Player* bot, uint32 skillId)
{
    if (skillId == HUNTING_SKILL_ID)
        return HUNTING_KILL_SECONDS;
    uint32 const spellId = GatheringInteractionSpellId(skillId);
    SpellInfo const* const spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
    if (!bot || !spellInfo)
        return 0u;
    uint32 const milliseconds = spellInfo->CalcCastTime(bot);
    return milliseconds ? (milliseconds + 999u) / 1000u : 0u;
}

std::optional<RuntimeGatheringCandidate> BuildRuntimeGatheringCandidate(
    Player* bot, uint32 skillId, uint32 itemId, uint32 activeUncoveredDemand,
    EconomyCoordinatorSnapshot const& coordinatorSnapshot, uint32 marketId, uint64 now, bool reserveSelfNeed,
    bool pathDerived = false, std::string* failure = nullptr)
{
    auto const fail = [failure](char const* reason)
    {
        if (failure)
            *failure = reason;
        return std::optional<RuntimeGatheringCandidate>{};
    };
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() ||
        bot->GetHealthPct() <= sPlayerbotAIConfig.lowHealth || bot->GetTransport() || bot->InBattleground() ||
        bot->IsBeingTeleported() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->GetGroup() ||
        AuctionMarketId(bot->GetFaction()) != marketId || !ActorCanWorkSource(bot, skillId))
    {
        return fail("actor_state");
    }

    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || IsRealPlayer(botAI->GetMaster()) ||
        GatheringAffinity(bot->GetGUID().GetCounter()) < PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM)
    {
        return fail("affinity_or_master");
    }
    uint32 const characterGuid = bot->GetGUID().GetCounter();
    bool const alreadyAssigned =
        std::any_of(coordinatorSnapshot.claims.begin(), coordinatorSnapshot.claims.end(),
                    [characterGuid](EconomyAssignment const& claim)
                    { return claim.characterGuid == characterGuid && claim.state == EconomyClaimState::Leased; });
    if (alreadyAssigned)
        return fail("already_assigned");

    GatheringDestinationBlocker blocker = GatheringDestinationBlocker::Empty;
    std::vector<GatheringTravelDestination*> const destinations =
        sPlayerbotEconomyTravelCatalog.GatheringDestinations(bot, skillId, &blocker, false, 5000.0f, itemId);
    if (destinations.empty())
        return fail("no_destination");

    WorldPosition botPosition(bot);
    float const speed = bot->GetSpeed(MOVE_RUN);
    if (!std::isfinite(speed) || speed <= 0.0f)
        return fail("no_initial_point");

    // Nearest populations first. A direct navmesh route answers for a few hundred yards; beyond that
    // the travel node graph is tried, and when neither answers the walk is accepted on the straight
    // line with a detour allowance. That is what MoveToTravelTargetAction does for every bot walk: it
    // heads for the point and paths step by step. A walk that really cannot be made ends as a
    // gathering_destination_unavailable release when the trip clock runs out, not as a refusal here.
    std::vector<GatheringTravelDestination*> ranked = destinations;
    std::sort(ranked.begin(), ranked.end(),
              [&botPosition](GatheringTravelDestination* left, GatheringTravelDestination* right)
              { return left->distanceTo(&botPosition) < right->distanceTo(&botPosition); });
    constexpr std::size_t MAX_ROUTED_DESTINATIONS = 4u;
    constexpr float OPTIMISTIC_DETOUR_FACTOR = 1.5f;
    GatheringTravelDestination* destination = nullptr;
    WorldPosition* initialPoint = nullptr;
    float distance = 0.0f;
    for (std::size_t index = 0u; index < ranked.size() && index < MAX_ROUTED_DESTINATIONS; ++index)
    {
        WorldPosition* const point = ranked[index]->NextUnvisitedPoint(botPosition, bot->GetMapId(), {});
        if (!point)
            continue;
        float const directDistance = botPosition.distance(*point);
        if (!std::isfinite(directDistance) || directDistance < 0.0f)
            continue;
        float routeDistance = directDistance;
        if (directDistance > ranked[index]->getRadiusMin())
        {
            std::vector<WorldPosition> route = botPosition.getPathTo(*point, bot);
            if (!point->isPathTo(route, REACHABLE_POINT_TOLERANCE))
            {
                TravelPath nodePath = TravelNodeMap::getFullPath(botPosition, *point, bot);
                route = nodePath.empty() ? std::vector<WorldPosition>{} : nodePath.getPointPath();
            }
            routeDistance =
                route.empty() ? directDistance * OPTIMISTIC_DETOUR_FACTOR : botPosition.getPathLength(route);
            if (!std::isfinite(routeDistance) || routeDistance < 0.0f)
                continue;
        }
        destination = ranked[index];
        initialPoint = point;
        distance = routeDistance;
        break;
    }
    if (!destination)
        return fail("no_initial_point");

    uint32 const outboundSeconds = static_cast<uint32>(std::ceil(distance / speed));
    uint32 const baseBudgetSeconds = destination->getExpireDelay() / 1000u;
    PlayerbotEconomyGathering& gathering = GetPlayerbotEconomyGathering();
    uint32 const activityBudgetSeconds =
        gathering.AvailableDedicatedActivityBudget(characterGuid, baseBudgetSeconds, now);
    DedicatedGatheringExperience const experience = gathering.DedicatedExperience(characterGuid, itemId);
    TravelDestination* const deliveryDestination =
        reserveSelfNeed ? sPlayerbotEconomyTravelCatalog.SelectAuctioneer(bot) : nullptr;
    WorldPosition* const deliveryPoint =
        deliveryDestination ? deliveryDestination->nearestPoint(initialPoint) : nullptr;
    std::vector<WorldPosition> const deliveryRoute =
        deliveryPoint ? initialPoint->getPathTo(*deliveryPoint, bot) : std::vector<WorldPosition>{};
    bool const deliveryInRange = deliveryDestination && deliveryPoint &&
                                 initialPoint->distance(deliveryPoint) <= deliveryDestination->getRadiusMin();
    if (reserveSelfNeed && (!deliveryDestination || !deliveryPoint ||
                            (!deliveryPoint->isPathTo(deliveryRoute, REACHABLE_POINT_TOLERANCE) && !deliveryInRange)))
    {
        return fail("no_delivery_route");
    }
    float const deliveryDistance = deliveryRoute.empty() ? 0.0f : initialPoint->getPathLength(deliveryRoute);
    if (!std::isfinite(deliveryDistance) || deliveryDistance < 0.0f)
        return fail("bad_delivery_distance");
    uint32 const returnSeconds = pathDerived       ? 0u
                                 : reserveSelfNeed ? static_cast<uint32>(std::ceil(deliveryDistance / speed))
                                                   : outboundSeconds;

    // A path derived trip carries its walk in the source path's own travel budget (neededBy adds it on
    // top of the action budget), so the walk must not also eat the activity budget here. Otherwise a
    // population further than about a thousand yards could never be planned at all.
    uint64 const fixedTravelSeconds = pathDerived ? 0u : static_cast<uint64>(outboundSeconds) + returnSeconds;
    uint32 const sourceYieldBasisPoints = destination->ConservativeYieldBasisPoints(itemId);
    uint32 const requiredResourcesForExpectedItem =
        sourceYieldBasisPoints
            ? static_cast<uint32>((10'000ull + sourceYieldBasisPoints - 1ull) / sourceYieldBasisPoints)
            : 0u;
    uint32 const resourceTimeSeconds = fixedTravelSeconds < activityBudgetSeconds
                                           ? activityBudgetSeconds - static_cast<uint32>(fixedTravelSeconds)
                                           : 0u;
    uint32 const coldStartResourceSeconds =
        requiredResourcesForExpectedItem ? resourceTimeSeconds / requiredResourcesForExpectedItem : 0u;
    uint32 const interactionSeconds = GatheringInteractionSeconds(bot, skillId);
    uint32 const conservativeSecondsPerResource = std::max(
        interactionSeconds, experience.conservativeSecondsPerResource ? experience.conservativeSecondsPerResource
                                                                      : coldStartResourceSeconds);
    uint64 const blendedYield =
        experience.resourceAttempts
            ? (static_cast<uint64>(sourceYieldBasisPoints) + experience.gatheredQuantity * 10'000u) /
                  (experience.resourceAttempts + 1u)
            : sourceYieldBasisPoints;
    uint32 const conservativeYieldBasisPoints = static_cast<uint32>(
        std::min<uint64>(sourceYieldBasisPoints, std::min<uint64>(blendedYield, std::numeric_limits<uint32>::max())));
    uint64 const resourceTimeCapacity =
        conservativeSecondsPerResource ? resourceTimeSeconds / conservativeSecondsPerResource : 0u;
    // A path derived trip is bounded by the source path's own action budget (required resources times
    // the per resource rate), not by the activity window, so it counts every spawn the requirement
    // needs. Capping it at the window's throughput left no path backable beyond one or two items.
    uint64 const resourcesForDemand =
        conservativeYieldBasisPoints
            ? (static_cast<uint64>(activeUncoveredDemand) * 10'000u + conservativeYieldBasisPoints - 1u) /
                  conservativeYieldBasisPoints
            : 0u;
    uint32 const maximumReachableResources = static_cast<uint32>(
        std::min<uint64>(pathDerived ? std::max(resourceTimeCapacity, resourcesForDemand) : resourceTimeCapacity,
                         std::numeric_limits<uint32>::max()));
    EconomySubstitutionGroup const group = EconomySubstitutionGroup::ExactReagent(itemId);
    uint32 const selfReservedQuantity =
        reserveSelfNeed ? ActorSelfReservation(coordinatorSnapshot, characterGuid, group) : 0u;
    bool const deliveryAvailable = bot->GetSession() && marketId && (!reserveSelfNeed || deliveryDestination);
    uint32 const reachableResourceCount =
        destination->CountReachablePointsOnMap(bot, *initialPoint, maximumReachableResources);
    DedicatedGatheringCapacityFacts const facts{
        .activeUncoveredDemand = activeUncoveredDemand,
        .selfReservedQuantity = selfReservedQuantity,
        .reachableResourceCount = reachableResourceCount,
        .conservativeYieldBasisPoints = conservativeYieldBasisPoints,
        .inventoryCapacity = StorableGatheringQuantity(bot, itemId, activeUncoveredDemand),
        .outboundSeconds = pathDerived ? 0u : outboundSeconds,
        .returnSeconds = returnSeconds,
        .activityBudgetSeconds = activityBudgetSeconds,
        .conservativeSecondsPerResource = conservativeSecondsPerResource,
        .skillEligible = skillId == HUNTING_SKILL_ID || bot->GetSkillValue(skillId) > 0u,
        .routeAvailable = true,
        .safe = true,
        .deliveryAvailable = deliveryAvailable,
    };
    uint32 const capacity = PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(facts);
    if (!capacity)
        return fail("no_capacity");

    RuntimeGatheringCandidate candidate;
    candidate.policy.characterGuid = characterGuid;
    candidate.policy.capacity = capacity;
    candidate.policy.routeSeconds = outboundSeconds + returnSeconds;
    candidate.policy.recentWorkSeconds =
        baseBudgetSeconds > activityBudgetSeconds ? baseBudgetSeconds - activityBudgetSeconds : 0u;
    candidate.policy.skillValue = bot->GetSkillValue(skillId);
    candidate.policy.gatheringAffinity = GatheringAffinity(characterGuid);
    candidate.policy.reliabilitySuccesses = experience.successes;
    candidate.policy.reliabilityAttempts = experience.attempts;
    candidate.destination = destination;
    candidate.initialPoint = initialPoint;
    candidate.outboundSeconds = outboundSeconds;
    candidate.activityBudgetSeconds = activityBudgetSeconds;
    candidate.remainingDedicatedActivitySeconds = resourceTimeSeconds;
    candidate.destinationYieldBasisPoints = sourceYieldBasisPoints;
    candidate.availableResourceCount = reachableResourceCount;
    candidate.authoritativeInteractionSeconds = interactionSeconds;
    candidate.observedGatheredQuantity = experience.gatheredQuantity;
    candidate.observedResourceAttempts = experience.resourceAttempts;
    candidate.observedResourceSeconds = experience.resourceSeconds;
    return candidate;
}

bool IsPathDerivedNodeSource(uint32 skillId, GatheringTravelSource source)
{
    return (skillId == SKILL_HERBALISM && source == GatheringTravelSource::HerbalismNode) ||
           (skillId == SKILL_MINING && source == GatheringTravelSource::MiningNode) ||
           (skillId == SKILL_SKINNING && source == GatheringTravelSource::SkinningCreature) ||
           (skillId == HUNTING_SKILL_ID && source == GatheringTravelSource::LootCreature);
}

std::string GatheringRouteIdentity(GatheringTravelDestination& destination, uint32 mapId)
{
    return Acore::StringFormat("gathering-route:{}:{}:{}", static_cast<uint32>(destination.getSource()),
                               destination.getEntry(), mapId);
}

struct ResolvedMaterialSourceDestination
{
    GatheringTravelDestination* destination = nullptr;
    WorldPosition* point = nullptr;
};

std::optional<ResolvedMaterialSourceDestination> ResolveMaterialSourceDestination(Player* bot,
                                                                                  MaterialSourcePath const& path)
{
    bool const knownSource = path.gatheringSkillId == HUNTING_SKILL_ID ||
                             IsGatheringProfessionSkill(static_cast<uint16>(path.gatheringSkillId));
    if (!bot || bot->GetGUID().GetCounter() != path.actorGuid || bot->GetMapId() != path.sourceMapId || !knownSource ||
        !ActorCanWorkSource(bot, path.gatheringSkillId))
    {
        return std::nullopt;
    }
    GatheringDestinationBlocker blocker = GatheringDestinationBlocker::Empty;
    std::vector<GatheringTravelDestination*> const destinations = sPlayerbotEconomyTravelCatalog.GatheringDestinations(
        bot, path.gatheringSkillId, &blocker, false, 5000.0f, path.materialItemId);
    WorldPosition botPosition(bot);
    for (GatheringTravelDestination* destination : destinations)
    {
        if (!destination || !IsPathDerivedNodeSource(path.gatheringSkillId, destination->getSource()) ||
            destination->getEntry() != static_cast<int32>(path.sourceEntry) ||
            destination->ConservativeYieldBasisPoints(path.materialItemId) != path.destinationYieldBasisPoints ||
            !destination->HasPointOnMap(path.sourceMapId) ||
            GatheringRouteIdentity(*destination, path.sourceMapId) != path.routeIdentity)
        {
            continue;
        }
        WorldPosition* point = destination->NextUnvisitedPoint(botPosition, path.sourceMapId, {});
        if (point)
            return ResolvedMaterialSourceDestination{destination, point};
    }
    return std::nullopt;
}

char const* GatheringDestinationBlockerName(GatheringDestinationBlocker blocker);

std::optional<MaterialSourcePath> BuildProgressionMaterialSourcePath(Player* bot, PlayerbotCareerPlan const& careerPlan,
                                                                     MaterialRequirement const& requirement,
                                                                     uint32 marketId, uint64 now)
{
    // A gathering item is sourced through its skill. Anything else is a mob drop: the bot hunts the
    // creatures whose ordinary loot carries it, provided the catalog knows such a population.
    std::optional<uint32> const gatheringSkill = GatheringSkillForItem(sObjectMgr->GetItemTemplate(requirement.itemId));
    if (gatheringSkill &&
        (!IsGatheringProfessionSkill(static_cast<uint16>(*gatheringSkill)) || !bot->HasSkill(*gatheringSkill)))
    {
        return std::nullopt;
    }
    uint32 const skillId = gatheringSkill.value_or(HUNTING_SKILL_ID);
    bool const hunting = skillId == HUNTING_SKILL_ID;
    // A latent intent leaves no trace of its own, and the backoff that follows hides the reason for a
    // long time; name the stage so a stuck bot can be read from the log.
    auto const latent = [bot, &requirement, skillId](std::string_view stage) -> std::optional<MaterialSourcePath>
    {
        LOG_INFO("playerbots.economy", "Bot {} found no material source for item {} (skill {}, map {}): {}.",
                 bot->GetGUID().GetCounter(), requirement.itemId, skillId, bot->GetMapId(), stage);
        return std::nullopt;
    };

    EconomyCoordinatorSnapshot const coordinatorSnapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    std::string candidateFailure;
    std::optional<RuntimeGatheringCandidate> const candidate =
        BuildRuntimeGatheringCandidate(bot, skillId, requirement.itemId, requirement.quantity, coordinatorSnapshot,
                                       marketId, now, false, true, &candidateFailure);
    if (!candidate)
    {
        GatheringDestinationBlocker blocker = GatheringDestinationBlocker::Empty;
        std::size_t const destinations =
            sPlayerbotEconomyTravelCatalog
                .GatheringDestinations(bot, skillId, &blocker, false, 5000.0f, requirement.itemId)
                .size();
        return latent(Acore::StringFormat("no_candidate: {} ({} destinations, {})", candidateFailure, destinations,
                                          GatheringDestinationBlockerName(blocker)));
    }
    if (!candidate->destination || !candidate->initialPoint ||
        !IsPathDerivedNodeSource(skillId, candidate->destination->getSource()) ||
        candidate->destination->getEntry() <= 0 || candidate->initialPoint->GetMapId() != bot->GetMapId() ||
        candidate->observedGatheredQuantity > std::numeric_limits<uint32>::max() ||
        candidate->observedResourceAttempts > std::numeric_limits<uint32>::max() ||
        candidate->observedResourceSeconds > std::numeric_limits<uint32>::max())
    {
        return latent("candidate_not_path_derived");
    }

    uint64 const observationBudget = PlayerbotEconomyPolicy::CareerIntervalSeconds(
        sPlayerbotAIConfig.randomBotUpdateInterval, careerPlan.engagement);
    if (!observationBudget || observationBudget > std::numeric_limits<uint32>::max())
        return latent("observation_budget");

    uint32 const actorGuid = bot->GetGUID().GetCounter();
    uint32 const sourceEntry = static_cast<uint32>(candidate->destination->getEntry());
    uint32 const sourceMapId = candidate->initialPoint->GetMapId();
    std::string const capacityIdentity =
        hunting ? Acore::StringFormat("same-actor-hunting:{}:{}:{}:{}", actorGuid, requirement.itemId, sourceEntry,
                                      sourceMapId)
                : Acore::StringFormat("same-actor-gathering:{}:{}:{}:{}:{}", actorGuid, skillId, requirement.itemId,
                                      sourceEntry, sourceMapId);
    MaterialCommitmentEncoding::SameActorGatheringPathBuildResult const built =
        MaterialCommitmentEncoding::BuildSameActorGatheringPath({
            .kind = hunting ? MaterialSourceKind::SameActorHunting : MaterialSourceKind::SameActorGathering,
            .actorGuid = actorGuid,
            .materialItemId = requirement.itemId,
            .selectedQuantity = requirement.quantity,
            .gatheringSkillId = skillId,
            .sourceEntry = sourceEntry,
            .sourceMapId = sourceMapId,
            .routeIdentity = GatheringRouteIdentity(*candidate->destination, sourceMapId),
            .capacityIdentity = capacityIdentity,
            .selectedAt = now,
            .sourceTravelBudgetSeconds = candidate->outboundSeconds,
            .destinationConservativeYieldBasisPoints = candidate->destinationYieldBasisPoints,
            .observedGatheredQuantity = static_cast<uint32>(candidate->observedGatheredQuantity),
            .observedResourceAttempts = static_cast<uint32>(candidate->observedResourceAttempts),
            .observedResourceSeconds = static_cast<uint32>(candidate->observedResourceSeconds),
            .authoritativeInteractionSeconds = candidate->authoritativeInteractionSeconds,
            .remainingDedicatedActivitySeconds = candidate->remainingDedicatedActivitySeconds,
            .deliveryTravelBudgetSeconds = 0u,
            .completionObservationBudgetSeconds = static_cast<uint32>(observationBudget),
            .availableResourceCount = candidate->availableResourceCount,
        });
    if (!built.path)
    {
        return latent(
            Acore::StringFormat("path_invalid (yield {} bp, interaction {} s, activity {} s, available {})",
                                candidate->destinationYieldBasisPoints, candidate->authoritativeInteractionSeconds,
                                candidate->remainingDedicatedActivitySeconds, candidate->availableResourceCount));
    }
    return built.path;
}

DedicatedGatheringPlan PlanRuntimeGatheringWork(Player* currentBot, DedicatedGatheringCandidate const& current,
                                                uint32 skillId, uint32 itemId, uint32 activeUncoveredDemand,
                                                EconomyCoordinatorSnapshot const& coordinatorSnapshot, uint32 marketId,
                                                uint64 now)
{
    struct Cache
    {
        uint64 coordinatorGeneration = 0u;
        uint64 observedAt = 0u;
        uint32 skillId = 0u;
        uint32 itemId = 0u;
        uint32 marketId = 0u;
        uint32 activeUncoveredDemand = 0u;
        DedicatedGatheringPlan plan;
    };
    static std::mutex cacheMutex;
    static std::optional<Cache> cache;

    std::scoped_lock lock(cacheMutex);
    if (cache && cache->coordinatorGeneration == coordinatorSnapshot.generation && cache->observedAt == now &&
        cache->skillId == skillId && cache->itemId == itemId && cache->marketId == marketId &&
        cache->activeUncoveredDemand == activeUncoveredDemand)
    {
        return cache->plan;
    }

    std::vector<DedicatedGatheringCandidate> candidates;
    bool includedCurrent = false;
    for (auto candidate = sRandomPlayerbotMgr.GetPlayerBotsBegin(); candidate != sRandomPlayerbotMgr.GetPlayerBotsEnd();
         ++candidate)
    {
        Player* const candidateBot = candidate->second;
        if (candidateBot == currentBot)
        {
            candidates.push_back(current);
            includedCurrent = true;
            continue;
        }
        std::optional<RuntimeGatheringCandidate> capacity = BuildRuntimeGatheringCandidate(
            candidateBot, skillId, itemId, activeUncoveredDemand, coordinatorSnapshot, marketId, now, true);
        if (capacity)
            candidates.push_back(capacity->policy);
    }
    if (!includedCurrent)
        candidates.push_back(current);

    DedicatedGatheringPlan plan = PlayerbotEconomyGathering::PlanDedicatedWork(activeUncoveredDemand, candidates);
    cache = Cache{
        .coordinatorGeneration = coordinatorSnapshot.generation,
        .observedAt = now,
        .skillId = skillId,
        .itemId = itemId,
        .marketId = marketId,
        .activeUncoveredDemand = activeUncoveredDemand,
        .plan = plan,
    };
    return plan;
}

PlayerbotCareer::ProfessionProgressionAuthority ProgressionAuthority(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    return {
        .combat = bot->IsInCombat(),
        .survival = bot->GetHealthPct() <= sPlayerbotAIConfig.lowHealth,
        .transport = bot->GetTransport() != nullptr,
        .directObjective = IsRealPlayer(botAI->GetMaster()),
        .groupCommitment = bot->GetGroup() != nullptr,
    };
}

std::optional<MaterialCommitmentEncoding::ProfessionProgressionIntentInput> ProgressionIntentInput(
    uint32 characterGuid, uint32 marketId, PlayerbotCareer::ProfessionProgressionMilestone const& milestone,
    PlayerbotCareer::ProfessionProgressionCycleDecision const& decision,
    std::vector<PlayerbotCareer::ProfessionProgressionState> const& professions,
    std::vector<PlayerbotCareer::ProfessionProgressionRecipe> const& recipes)
{
    auto const profession = std::ranges::find(professions, milestone.professionSkillId,
                                              &PlayerbotCareer::ProfessionProgressionState::professionSkillId);
    auto const recipe = std::ranges::find_if(recipes,
                                             [&milestone](PlayerbotCareer::ProfessionProgressionRecipe const& candidate)
                                             {
                                                 return candidate.professionSkillId == milestone.professionSkillId &&
                                                        candidate.spellId == milestone.recipeSpellId &&
                                                        candidate.outputItemId == milestone.outputItemId;
                                             });
    if (profession == professions.end() || recipe == recipes.end())
        return std::nullopt;

    uint16 const lag =
        profession->targetSkill > profession->currentSkill ? profession->targetSkill - profession->currentSkill : 0u;
    uint32 const boundedBatch =
        decision.batchRemaining ? decision.batchRemaining
                                : PlayerbotCareer::ProgressionBatchCeiling(
                                      profession->affinity, lag, PlayerbotCareer::PROFESSION_PROGRESSION_MAXIMUM_BATCH);
    if (!boundedBatch)
        return std::nullopt;

    std::vector<MaterialCommitmentEncoding::ProfessionProgressionReagentFact> reagentFacts;
    reagentFacts.reserve(recipe->reagents.size());
    for (PlayerbotCareer::ProfessionProgressionReagent const& reagent : recipe->reagents)
    {
        reagentFacts.push_back({.itemId = reagent.itemId,
                                .perCraftQuantity = reagent.count,
                                .ordinaryVendorAvailable = reagent.ordinaryVendorAvailable,
                                .millingInputItemId = reagent.millingInputItemId});
    }
    std::optional<std::vector<MaterialRequirement>> requirements =
        MaterialCommitmentEncoding::ProfessionProgressionScarceRequirements(boundedBatch, reagentFacts);
    if (!requirements)
        return std::nullopt;
    return MaterialCommitmentEncoding::ProfessionProgressionIntentInput{
        .characterGuid = characterGuid,
        .marketId = marketId,
        .professionSkillId = milestone.professionSkillId,
        .targetSkill = milestone.targetSkill,
        .recipeSpellId = milestone.recipeSpellId,
        .outputItemId = milestone.outputItemId,
        .boundedBatch = boundedBatch,
        .scarceRequirements = std::move(*requirements),
    };
}

std::vector<uint16> const& PrimaryCapabilitySkillIds()
{
    static std::vector<uint16> const skillIds = []
    {
        std::vector<uint16> result;
        for (ProfessionCapability const& capability : PlayerbotProfessionCapabilityCatalog::All())
        {
            if (capability.primaryProfession)
                result.push_back(capability.professionSkillId);
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }();
    return skillIds;
}

std::vector<uint16> LearnedPrimaryCapabilitySkillIds(Player const* bot)
{
    std::vector<uint16> learned;
    for (uint16 skillId : PrimaryCapabilitySkillIds())
        if (bot->HasSkill(skillId))
            learned.push_back(skillId);
    return learned;
}

std::vector<uint16> LearnedCareerSkillIds(Player const* bot, PlayerbotCareerPlan const& plan)
{
    std::vector<uint16> learned;
    auto const appendLearned = [bot, &learned](uint16 skillId)
    {
        if (bot->HasSkill(skillId) && std::find(learned.begin(), learned.end(), skillId) == learned.end())
            learned.push_back(skillId);
    };
    for (uint16 skillId : PlayerbotCareer::EffectivePrimarySkills(plan))
        appendLearned(skillId);
    for (uint16 skillId : plan.secondarySkills)
        appendLearned(skillId);
    if (plan.capabilityGoal)
        appendLearned(plan.capabilityGoal->professionSkillId);
    return learned;
}

std::vector<uint32> KnownCapabilityRecipeSpellIds(Player const* bot)
{
    std::vector<uint32> known;
    for (ProfessionCapability const& capability : PlayerbotProfessionCapabilityCatalog::All())
    {
        if (capability.recipeSpellId && bot->HasSpell(capability.recipeSpellId))
            known.push_back(capability.recipeSpellId);
    }
    std::sort(known.begin(), known.end());
    known.erase(std::unique(known.begin(), known.end()), known.end());
    return known;
}

bool ProductionOutputMatchesGroup(Player const* bot, uint32 itemId, EconomySubstitutionGroup const& group)
{
    if (group.kind == EconomySubstitutionKind::ExactReagent)
        return group.exactItemId == itemId;

    std::optional<FinishedGoodDescription> const output =
        PlayerbotEconomyConsumption::Describe(bot, sObjectMgr->GetItemTemplate(itemId));
    if (!output || output->group.kind != group.kind)
        return false;
    if (group.kind == EconomySubstitutionKind::Consumable)
    {
        ConsumptionNeed requirement;
        requirement.group = group;
        requirement.requiredUtility = group.valueBand;
        return PlayerbotEconomyConsumption::MatchesNeed(requirement, output->group, output->utility);
    }
    if (group.kind != EconomySubstitutionKind::Equipment)
        return output->group == group;
    return output->group.equipmentSlot == group.equipmentSlot && output->group.tier == group.tier;
}

std::vector<EconomyProductionRecipe> ProductionRecipes(Player const* bot, EconomySnapshot const& economy,
                                                       EconomyCoordinatorSnapshot const& coordinator, uint32 marketId)
{
    std::vector<EconomyProductionRecipe> recipes;
    for (EconomyDemandGap const& gap : coordinator.gaps)
    {
        if (gap.marketId != marketId || !gap.demandQuantity)
            continue;
        for (RecipeCandidate const& candidate : economy.recipes)
        {
            if (!candidate.spellId || !candidate.craftedItemId ||
                !ProductionOutputMatchesGroup(bot, candidate.craftedItemId, gap.group))
            {
                continue;
            }
            ItemTemplate const* const outputTemplate = sObjectMgr->GetItemTemplate(candidate.craftedItemId);
            uint32 const stackSize = outputTemplate ? std::max(1u, outputTemplate->GetMaxStackSize()) : 1u;
            uint32 const ceiling = static_cast<uint32>(
                std::min<uint64>(static_cast<uint64>(stackSize) * sPlayerbotEconomyConfig.productionMaxBatchStacks,
                                 std::numeric_limits<uint32>::max()));
            uint32 const batchQuantity = PlayerbotEconomyPolicy::ProductionBatchQuantity(candidate, economy, ceiling);
            if (!batchQuantity)
                continue;
            EconomyProductionRecipe const recipe{gap.group, candidate.spellId, candidate.craftedItemId, batchQuantity};
            if (std::find(recipes.begin(), recipes.end(), recipe) == recipes.end())
                recipes.push_back(recipe);
        }
    }
    return recipes;
}

uint64 ProductionLeaseExpiry(PlayerbotCareerPlan const& careerPlan, uint64 now)
{
    uint64 const interval = PlayerbotEconomyPolicy::CareerIntervalSeconds(sPlayerbotAIConfig.randomBotUpdateInterval,
                                                                          careerPlan.engagement);
    uint64 const duration =
        interval > std::numeric_limits<uint64>::max() / 2u ? std::numeric_limits<uint64>::max() : interval * 2u;
    return duration > std::numeric_limits<uint64>::max() - now ? std::numeric_limits<uint64>::max() : now + duration;
}

void RemoveClaimBackedProductionSupply(std::vector<EconomySupplyFact>& supplies,
                                       EconomyCoordinatorSnapshot const& coordinator, uint32 characterGuid)
{
    for (EconomyAssignment const& claim : coordinator.claims)
    {
        if (claim.characterGuid != characterGuid || claim.kind != EconomyClaimKind::Production ||
            claim.state != EconomyClaimState::Leased || !claim.committedQuantity)
        {
            continue;
        }

        uint32 remaining = claim.committedQuantity;
        for (EconomySupplyFact& supply : supplies)
        {
            if (!remaining || supply.source != EconomySupplySource::Inventory || supply.itemId != claim.outputItemId ||
                supply.group != claim.group)
            {
                continue;
            }
            uint32 const removed = std::min(supply.quantity, remaining);
            supply.quantity -= removed;
            remaining -= removed;
        }
        std::erase_if(supplies, [](EconomySupplyFact const& supply) { return !supply.quantity; });
    }
}

void RefreshCoordinator(PlayerbotAI* botAI, EconomySnapshot const& snapshot, ConsumptionSnapshot const& consumption,
                        uint32 marketId, uint64 now, uint32 excludedItemId, uint32 excludedQuantity)
{
    Player* const bot = botAI->GetBot();
    EconomyActorFacts actor;
    actor.characterGuid = bot->GetGUID().GetCounter();
    actor.accountId = bot->GetSession()->GetAccountId();
    actor.marketId = marketId;
    actor.online = bot->IsInWorld();
    actor.autonomous = !IsRealPlayer(botAI->GetMaster());
    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(actor.characterGuid);
    if (personality)
    {
        actor.craftingAffinity = personality->craftingAffinity;
        actor.gatheringAffinity = personality->gatheringAffinity;
        actor.economyAffinity = personality->economyAffinity;
    }
    actor.freePrimaryProfessionSlots =
        static_cast<uint8>(std::min<uint32>(bot->GetFreePrimaryProfessionPoints(), std::numeric_limits<uint8>::max()));
    actor.professionSkillIds = LearnedPrimaryCapabilitySkillIds(bot);
    actor.recipeSpellIds = KnownCapabilityRecipeSpellIds(bot);

    std::optional<RecipeDeficit> const deficit = NextRecipeDeficit(snapshot);
    if (deficit)
        actor.demands.push_back({EconomySubstitutionGroup::ExactReagent(deficit->itemId), deficit->demandQuantity});
    std::vector<EconomyDemandFact> const consumerDemands = PlayerbotEconomyConsumption::DemandFacts(consumption);
    actor.demands.insert(actor.demands.end(), consumerDemands.begin(), consumerDemands.end());
    actor.supplies = PlayerbotEconomyConsumption::SupplyFacts(consumption);
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();

    std::map<uint32, uint32> inventorySupply;
    std::map<uint32, uint32> mailSupply;
    if (deficit)
    {
        auto const item =
            std::find_if(snapshot.inventory.begin(), snapshot.inventory.end(),
                         [&deficit](InventoryCount const& candidate) { return candidate.itemId == deficit->itemId; });
        if (item != snapshot.inventory.end())
        {
            inventorySupply[item->itemId] = item->count;
            mailSupply[item->itemId] = item->mailCount;
        }
    }
    if (excludedItemId && excludedQuantity)
    {
        uint32& quantity = inventorySupply[excludedItemId];
        quantity = quantity > excludedQuantity ? quantity - excludedQuantity : 0u;
    }
    for (auto const& [itemId, quantity] : inventorySupply)
    {
        if (quantity)
            actor.supplies.push_back(
                {EconomySubstitutionGroup::ExactReagent(itemId), quantity, EconomySupplySource::Inventory, itemId});
    }
    for (auto const& [itemId, quantity] : mailSupply)
    {
        if (quantity)
            actor.supplies.push_back(
                {EconomySubstitutionGroup::ExactReagent(itemId), quantity, EconomySupplySource::Mail, itemId});
    }

    RemoveClaimBackedProductionSupply(actor.supplies, coordinator.Snapshot(now), actor.characterGuid);
    coordinator.RefreshActor(std::move(actor), now);

    EconomyMarketFacts market;
    market.marketId = marketId;
    for (AuctionListingCandidate const& auction : snapshot.auctions)
    {
        if (auction.itemId && auction.count)
        {
            market.supplies.push_back({EconomySubstitutionGroup::ExactReagent(auction.itemId), auction.count,
                                       EconomySupplySource::ActiveAuction});
        }
    }
    for (ConsumptionOffer const& offer : consumption.offers)
    {
        if (offer.count)
            market.supplies.push_back({offer.group, offer.count, EconomySupplySource::ActiveAuction});
    }
    coordinator.RefreshMarket(std::move(market), now);
}

char const* AutonomousBlockerName(AutonomousGatheringBlocker blocker)
{
    switch (blocker)
    {
        case AutonomousGatheringBlocker::None:
            return "gathering_none";
        case AutonomousGatheringBlocker::DemandGone:
            return "gathering_demand_gone";
        case AutonomousGatheringBlocker::DestinationUnavailable:
            return "gathering_destination_unavailable";
        case AutonomousGatheringBlocker::DestinationExpired:
            return "gathering_destination_expired";
        case AutonomousGatheringBlocker::InventoryFull:
            return "gathering_inventory_full";
        case AutonomousGatheringBlocker::Unsafe:
            return "gathering_unsafe";
        case AutonomousGatheringBlocker::OneKillBoundReached:
            return "gathering_one_kill_bound";
    }
    return "gathering_unknown";
}

char const* GatheringDestinationBlockerName(GatheringDestinationBlocker blocker)
{
    switch (blocker)
    {
        case GatheringDestinationBlocker::None:
            return "gathering_destination_none";
        case GatheringDestinationBlocker::Empty:
            return "gathering_destination_empty";
        case GatheringDestinationBlocker::Full:
            return "gathering_destination_full";
        case GatheringDestinationBlocker::Expired:
            return "gathering_destination_expired";
        case GatheringDestinationBlocker::Cooldown:
            return "gathering_destination_cooldown";
        case GatheringDestinationBlocker::WrongMap:
            return "gathering_destination_wrong_map";
        case GatheringDestinationBlocker::WrongSkill:
            return "gathering_destination_wrong_skill";
        case GatheringDestinationBlocker::InsufficientSkill:
            return "gathering_destination_insufficient_skill";
        case GatheringDestinationBlocker::WrongLevel:
            return "gathering_destination_wrong_level";
        case GatheringDestinationBlocker::Inaccessible:
            return "gathering_destination_inaccessible";
        case GatheringDestinationBlocker::NotAttackable:
            return "gathering_destination_not_attackable";
    }
    return "gathering_destination_unknown";
}

uint64 LowestCompetingBuyoutPerItem(AuctionHouseObject* auctionHouse, uint32 itemId, uint32 botAccountId)
{
    if (!auctionHouse)
        return 0;

    uint64 lowest = 0;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        (void)auctionId;
        if (!auction || auction->item_template != itemId || !auction->itemCount || !auction->buyout)
            continue;

        uint32 const ownerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
        if (ownerAccountId == botAccountId)
            continue;

        uint64 const perItem = (static_cast<uint64>(auction->buyout) + auction->itemCount - 1u) / auction->itemCount;
        if (!lowest || perItem < lowest)
            lowest = perItem;
    }
    return lowest;
}

class EconomyTravelAction final : public ChooseTravelTargetAction
{
public:
    explicit EconomyTravelAction(PlayerbotAI* botAI) : ChooseTravelTargetAction(botAI, "economy travel") {}

    void Apply(TravelTarget* newTarget, TravelTarget* oldTarget) { setNewTarget(newTarget, oldTarget); }
    void Clear(TravelTarget* target) { SetNullTarget(target); }
};

// Engages one chosen creature through the ordinary attack path, so a skinning or hunting kill does not
// depend on the grind strategy happening to pick the same target.
class EconomyAttackAction final : public AttackAction
{
public:
    explicit EconomyAttackAction(PlayerbotAI* botAI) : AttackAction(botAI, "economy attack") {}

    bool Apply(Unit* target) { return Attack(target); }
};

class EconomyUseItemAction final : public UseItemAction
{
public:
    explicit EconomyUseItemAction(PlayerbotAI* botAI) : UseItemAction(botAI, "economy final use", true) {}

    bool Apply(Item* item) { return UseItemAuto(item); }
};

using ExecutionResult = EconomyExecutionResult;

class DefaultPlayerbotEconomyRuntime final : public PlayerbotEconomyRuntime
{
public:
    bool IsEligible(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan) const override;
    PlayerbotEconomyCycleResult ExecuteCycle(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan) override;
    void Reset(PlayerbotAI* botAI) override;
    // True while this runtime still owns a trip the bot is actively walking.
    [[nodiscard]] bool OwnsTripInFlight(PlayerbotAI* botAI);
    // True while the bot's forced travel target is still the one this runtime set, whether it is
    // walking or has arrived.
    [[nodiscard]] bool OwnsTravelTarget(PlayerbotAI* botAI);
    // Releases per-cycle state, but keeps a trip that is still under way. A cycle that simply
    // found nothing to do must not cancel the journey a previous cycle started.
    void ReleaseIdleCycleState(PlayerbotAI* botAI);

private:
    EconomySnapshot BuildSnapshot(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan);
    ConsumptionSnapshot BuildConsumptionSnapshot(PlayerbotAI* botAI, EconomySnapshot const& economy, uint32 marketId,
                                                 uint64 now);
    Creature* FindAuctioneer(PlayerbotAI* botAI);
    ExecutionResult ExecuteDecision(PlayerbotAI* botAI, EconomyDecision const& decision, Creature* auctioneer);
    ExecutionResult ExecuteConsumption(PlayerbotAI* botAI, ConsumptionDecision const& decision, Creature* auctioneer);
    ExecutionResult CollectAuctionMail(PlayerbotAI* botAI);
    ExecutionResult BuyReagent(PlayerbotAI* botAI, EconomyDecision const& decision, Creature* auctioneer,
                               EconomyClaimPriority priority = EconomyClaimPriority::Producer,
                               std::optional<EconomySubstitutionGroup> claimGroup = std::nullopt);
    ExecutionResult SellSurplus(PlayerbotAI* botAI, EconomyDecision const& decision, Creature* auctioneer);
    void ObserveMarketEvidence(PlayerbotAI* botAI, uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ReconcileMarketPositionMail(PlayerbotAI* botAI, uint32 marketId,
                                                                           uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ExecuteMarketMaking(PlayerbotAI* botAI, EconomySnapshot const& snapshot,
                                                                   Creature* auctioneer, uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ManageMarketPosition(PlayerbotAI* botAI, EconomyPosition const& position,
                                                                    EconomyMarketSnapshot const& market,
                                                                    Creature* auctioneer, bool ordinaryVendorSupply,
                                                                    uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ManagePendingMarketPosition(PlayerbotAI* botAI,
                                                                           EconomyPosition const& position,
                                                                           EconomyMarketSnapshot const& market,
                                                                           Creature* auctioneer, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> OpenMarketPosition(PlayerbotAI* botAI, EconomySnapshot const& snapshot,
                                                                  EconomyMarketSnapshot const& market,
                                                                  Creature* auctioneer, uint32 marketId, uint64 now);
    bool IsSafePositionItem(PlayerbotAI* botAI, Item const* item, EconomyPosition const& position) const;
    std::optional<PlayerbotEconomyCycleResult> ListMarketPosition(PlayerbotAI* botAI, EconomyPosition const& position,
                                                                  Item* item, Creature* auctioneer,
                                                                  EconomyReferencePrice const& reference, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> VendorMarketPosition(PlayerbotAI* botAI, EconomyPosition const& position,
                                                                    Item* item, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ExecuteAutonomousGathering(PlayerbotAI* botAI,
                                                                          PlayerbotCareerPlan const& careerPlan,
                                                                          EconomySnapshot const& snapshot,
                                                                          EconomyDecision const& productionDecision,
                                                                          uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> StartAutonomousGathering(PlayerbotAI* botAI,
                                                                        GatheringOpportunity const& opportunity,
                                                                        bool allowFailureResult, uint32 marketId,
                                                                        uint64 now);
    bool HasMatchingGatheringLoot(PlayerbotAI* botAI, uint32 skillId);
    bool StartOneCreatureKill(PlayerbotAI* botAI, GatheringTravelDestination* destination);
    bool TravelToGatheringPoint(PlayerbotAI* botAI, GatheringTravelDestination* destination, WorldPosition* point);
    bool TravelToAuctionHouse(PlayerbotAI* botAI);
    bool TravelToMailbox(PlayerbotAI* botAI);
    bool TravelToDestination(PlayerbotAI* botAI, TravelDestination* destination, float radius = INTERACTION_DISTANCE,
                             std::optional<EconomyApproachPoint> standPoint = std::nullopt);
    struct SpellFocusStand
    {
        EconomyApproachPoint point;
        float distance;
    };
    // A point next to a spell focus that a bot can craft from: inside the range the core accepts, and
    // neither in magma or slime nor off the platform. The Ironforge forges are lava pools 13 yards wide.
    // Empty when no such point exists, in which case the craft is skipped rather than attempted.
    std::optional<SpellFocusStand> SpellFocusStandPoint(
        Player* bot, PlayerbotEconomyTravelCatalog::SpellFocusDestination const& focus);
    bool IsInventoryBagItem(Item const* item) const;
    bool IsSafeSaleItem(PlayerbotAI* botAI, Item const* item, EconomyDecision const& decision);
    std::vector<ProfessionCapability> const& CapabilityCandidates(Player const* bot,
                                                                  EconomySubstitutionGroup const& group);
    void RevalidateCapabilities(PlayerbotAI* botAI, uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ReconcileCapabilityGoal(PlayerbotAI* botAI,
                                                                       PlayerbotCareerPlan const& careerPlan,
                                                                       uint32 marketId, uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ExecuteTrainerObjective(PlayerbotAI* botAI,
                                                                       PlayerbotCareerPlan const& careerPlan);
    std::optional<PlayerbotEconomyCycleResult> ExecuteProfessionProgression(PlayerbotAI* botAI,
                                                                            PlayerbotCareerPlan const& careerPlan,
                                                                            EconomySnapshot const& snapshot,
                                                                            uint64 now);
    // A progression craft that runs on a partial delivery consumes the material the active source path
    // is still collecting. The ledger settles a source path only in full, so the commitment is released
    // here and the next cycle re-admits a fresh path for what is still missing.
    void ReleaseMaterialSourceConsumedByCraft(PlayerbotAI* botAI,
                                              PlayerbotCareer::ProfessionProgressionMilestone const& milestone,
                                              uint64 now);
    std::optional<PlayerbotEconomyCycleResult> ExecuteProgressionMaterialSource(
        PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan, EconomySnapshot const& snapshot,
        MaterialCommitmentEncoding::ProfessionProgressionIntentInput const& intent, uint64 now);
    [[nodiscard]] uint32 ProgressionAvailableInventory(EconomySnapshot const& snapshot, uint32 itemId) const;
    PlayerbotEconomyCycleResult BuyProgressionVendorInput(PlayerbotAI* botAI, uint32 itemId, uint32 recipeSpellId,
                                                          uint32 count = 1u);
    PlayerbotEconomyCycleResult DisenchantProgressionInput(PlayerbotAI* botAI, EconomySnapshot const& snapshot,
                                                           uint32 itemId, uint32 recipeSpellId);
    PlayerbotEconomyCycleResult MillProgressionInput(PlayerbotAI* botAI, EconomySnapshot const& snapshot, uint32 itemId,
                                                     uint32 recipeSpellId);
    std::optional<PlayerbotEconomyCycleResult> ReconcileRecipeLearning(PlayerbotAI* botAI, uint64 now);
    void ReconcileCraftTrace(Player* bot, uint64 now);

    struct CommittedFinishedGood
    {
        EconomySubstitutionGroup group;
        FinishedGoodUse use = FinishedGoodUse::Equip;
        uint32 itemId = 0;
        uint32 quantity = 0;
        std::string chainPublicId;
        uint32 counterpartyGuid = 0;
    };

    struct PendingCraftTrace
    {
        std::string chainPublicId;
        uint64 coordinatorLeaseId = 0;
        uint32 actorGuid = 0;
        uint32 itemId = 0;
        uint32 recipeSpellId = 0;
        uint32 startingQuantity = 0;
        uint64 startedAt = 0;
    };

    struct PendingProgressionCraft
    {
        uint16 startingSkill = 0;
        uint32 startingOutputQuantity = 0;
        uint64 startedAt = 0;
    };

    struct CommittedRecipe
    {
        uint32 itemId = 0;
        uint32 recipeSpellId = 0;
        std::string chainPublicId;
        uint32 counterpartyGuid = 0;
    };

    struct ActiveGatheringTrip
    {
        AutonomousGatheringPlan plan;
        uint32 skillId = 0;
        uint32 spellId = 0;
        uint64 startedAt = 0;
        uint32 outboundSeconds = 0;
        uint64 coordinatorLeaseId = 0;
        uint32 committedQuantity = 0;
        bool coordinatorSettled = false;
        std::string materialCommitmentIdentity;
        GatheringTravelDestination* destination = nullptr;
        std::vector<WorldPosition*> attemptedPoints;
        ObjectGuid killTarget;
    };

    TravelDestination* ownedTravelDestination = nullptr;
    // The stand-off point handed to the travel target; it must outlive the TravelTarget that points at it.
    WorldPosition ownedTravelPoint;
    bool ownsTravelStrategy = false;
    std::vector<std::string> suspendedIdleStrategies;
    // Owns the travel strategy for an economy walk and parks the idle strategies that would wander the
    // bot off its route; Reset restores both.
    void AcquireTravelStrategies(PlayerbotAI* botAI)
    {
        if (!botAI->HasStrategy("travel", BOT_STATE_NON_COMBAT))
        {
            botAI->ChangeStrategy("+travel", BOT_STATE_NON_COMBAT);
            ownsTravelStrategy = true;
        }
        if (!suspendedIdleStrategies.empty())
            return;
        std::vector<std::string> active;
        for (char const* strategy : {"rpg", "new rpg", "move random"})
        {
            if (botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
                active.emplace_back(strategy);
        }
        suspendedIdleStrategies = PlayerbotEconomyGathering::IdleStrategiesToSuspend(active);
        for (std::string const& strategy : suspendedIdleStrategies)
            botAI->ChangeStrategy("-" + strategy, BOT_STATE_NON_COMBAT);
    }
    std::map<uint64, CommittedFinishedGood> committedFinishedGoods;
    std::map<uint64, CommittedRecipe> committedRecipes;
    std::map<uint32, uint32> pendingGatheredSupply;
    std::map<std::pair<uint8, EconomySubstitutionGroup>, std::vector<ProfessionCapability>> capabilityCandidates;
    TravelDestination* activeGatheringPointDestination = nullptr;
    std::optional<ActiveGatheringTrip> activeGathering;
    // The owned travel target is a spell focus object the craft step sent the bot to.
    bool craftFocusTravel = false;
    std::optional<PlayerbotTrainerTravelSelection> activeTrainer;
    std::optional<PlayerbotCareerTrainerObjective> activeTrainerObjective;
    std::optional<PendingCraftTrace> pendingCraftTrace;
    // Why the last craft step failed, reported as the cycle blocker.
    std::string lastCraftFailure;
    std::optional<PlayerbotCareer::ProfessionProgressionMilestone> activeProgressionMilestone;
    std::optional<PendingProgressionCraft> pendingProgressionCraft;
    std::unordered_set<uint32> progressionTrainingOutputs;
    uint32 activeProgressionBatchRemaining = 0;
};

void DefaultPlayerbotEconomyRuntime::ReconcileCraftTrace(Player* bot, uint64 now)
{
    if (!pendingCraftTrace)
        return;
    uint32 const currentQuantity = bot->GetItemCount(pendingCraftTrace->itemId);
    if (currentQuantity > pendingCraftTrace->startingQuantity)
    {
        uint32 const producedQuantity = currentQuantity - pendingCraftTrace->startingQuantity;
        EconomyProductionOutput productionOutput;
        if (pendingCraftTrace->coordinatorLeaseId)
        {
            productionOutput =
                ReconcileProductionInventory(GetPlayerbotEconomyCoordinator(), pendingCraftTrace->coordinatorLeaseId,
                                             pendingCraftTrace->startingQuantity, currentQuantity, now);
        }
        [[maybe_unused]] bool const recorded =
            PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                .Complete(true, {
                                    .deduplicationKey = Acore::StringFormat(
                                        "craft:{}:{}:{}", pendingCraftTrace->actorGuid,
                                        pendingCraftTrace->recipeSpellId, pendingCraftTrace->startedAt),
                                    .chainPublicId = pendingCraftTrace->chainPublicId,
                                    .actorGuid = pendingCraftTrace->actorGuid,
                                    .itemId = pendingCraftTrace->itemId,
                                    .recipeSpellId = pendingCraftTrace->recipeSpellId,
                                    .quantity = producedQuantity,
                                    .occurredAt = now,
                                    .kind = EconomyTraceKind::Crafted,
                                });
        if (!pendingCraftTrace->coordinatorLeaseId || productionOutput.completed)
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        pendingCraftTrace.reset();
        return;
    }
    if (now > pendingCraftTrace->startedAt + 60u)
    {
        if (!pendingCraftTrace->coordinatorLeaseId)
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        pendingCraftTrace.reset();
    }
}

uint32 DefaultPlayerbotEconomyRuntime::ProgressionAvailableInventory(EconomySnapshot const& snapshot,
                                                                     uint32 itemId) const
{
    auto const inventory = std::find_if(snapshot.inventory.begin(), snapshot.inventory.end(),
                                        [itemId](InventoryCount const& item) { return item.itemId == itemId; });
    uint32 const currentQuantity = inventory == snapshot.inventory.end() ? 0u : inventory->count;

    uint64 protectedQuantity = 0u;
    auto const pending = pendingGatheredSupply.find(itemId);
    if (pending != pendingGatheredSupply.end())
        protectedQuantity = std::min(currentQuantity, pending->second);

    if (activeGathering && activeGathering->plan.itemId == itemId &&
        ((!activeGathering->materialCommitmentIdentity.empty()) ||
         (activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled)))
    {
        AcceptedExternalGatheringSlice const active = PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice({
            .currentInventoryQuantity = currentQuantity,
            .preTripInventoryQuantity = activeGathering->plan.startingItemCount,
            .acceptedQuantity = activeGathering->plan.requestedQuantity,
            .retained = true,
        });
        protectedQuantity += active.protectedQuantity;
    }

    return currentQuantity - static_cast<uint32>(std::min<uint64>(currentQuantity, protectedQuantity));
}

void DefaultPlayerbotEconomyRuntime::ReleaseMaterialSourceConsumedByCraft(
    PlayerbotAI* botAI, PlayerbotCareer::ProfessionProgressionMilestone const& milestone, uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotMaterialCommitmentAuthority& authority = GetPlayerbotMaterialCommitmentAuthority();
    MaterialCommitmentSnapshot const book = authority.Snapshot();
    std::string const originPrefix =
        Acore::StringFormat("profession-progression:{}:{}:", bot->GetGUID().GetCounter(), milestone.professionSkillId);
    for (MaterialCommitment const& commitment : book.commitments)
    {
        if (!commitment.originIdentity.starts_with(originPrefix) || !commitment.sourcePath ||
            commitment.sourcePath->phase != MaterialSourcePhase::Acquiring ||
            (commitment.state != MaterialCommitmentState::Admitted &&
             commitment.state != MaterialCommitmentState::PartiallyFulfilled))
        {
            continue;
        }
        if (activeGathering && activeGathering->materialCommitmentIdentity == commitment.identity)
            Reset(botAI);
        MaterialCommitmentApplyResult const applied =
            authority.Apply({.operationIdentity = Acore::StringFormat("{}:craft-consumed:{}", commitment.identity,
                                                                      commitment.sourcePath->sourceRevision),
                             .expectedBookRevision = book.bookRevision,
                             .kind = MaterialCommitmentCommandKind::Release,
                             .commitmentIdentities = {commitment.identity}},
                            now);
        LOG_INFO("playerbots.economy", "Bot {} released material source {} for item {}: consumed by craft {} ({}).",
                 bot->GetGUID().GetCounter(), commitment.identity, commitment.materialItemId, milestone.recipeSpellId,
                 static_cast<uint32>(applied.status));
        return;
    }
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteProgressionMaterialSource(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan, EconomySnapshot const& snapshot,
    MaterialCommitmentEncoding::ProfessionProgressionIntentInput const& intentInput, uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotMaterialCommitmentAuthority& authority = GetPlayerbotMaterialCommitmentAuthority();
    MaterialCommitmentSnapshot book = authority.Snapshot();
    std::string const originIdentity = MaterialCommitmentEncoding::ProfessionProgressionOriginIdentity(intentInput);
    auto result =
        [&intentInput](PlayerbotEconomyCycleOutcome outcome, std::string blocker, EconomyAttemptOutcome effect)
    {
        PlayerbotEconomyCycleResult cycle;
        cycle.outcome = outcome;
        cycle.phase = EconomyPhase::BuyReagent;
        cycle.workIdentity = {
            intentInput.recipeSpellId,
            intentInput.scarceRequirements.empty() ? 0u : intentInput.scarceRequirements.front().itemId, 0u, 0u};
        cycle.blocker = std::move(blocker);
        cycle.schedulingEffect = effect;
        return cycle;
    };
    auto persistenceResult =
        [&result](MaterialCommitmentApplyStatus status, std::string pendingBlocker, std::string unavailableBlocker)
    {
        if (status == MaterialCommitmentApplyStatus::PendingPersistence ||
            status == MaterialCommitmentApplyStatus::Idempotent || status == MaterialCommitmentApplyStatus::Busy)
        {
            return result(PlayerbotEconomyCycleOutcome::Scheduled, std::move(pendingBlocker),
                          EconomyAttemptOutcome::InProgress);
        }
        return result(PlayerbotEconomyCycleOutcome::NoCandidate, std::move(unavailableBlocker),
                      EconomyAttemptOutcome::NoCandidate);
    };
    auto active = std::ranges::find_if(book.commitments,
                                       [&originIdentity](MaterialCommitment const& commitment)
                                       {
                                           return commitment.originIdentity == originIdentity &&
                                                  (commitment.state == MaterialCommitmentState::Admitted ||
                                                   commitment.state == MaterialCommitmentState::PartiallyFulfilled);
                                       });
    auto release = [&](MaterialCommitment const& commitment, std::string blocker)
    {
        if (activeGathering && activeGathering->materialCommitmentIdentity == commitment.identity)
            Reset(botAI);
        MaterialCommitmentApplyResult const applied =
            authority.Apply({.operationIdentity = Acore::StringFormat("{}:source-release:{}", commitment.identity,
                                                                      commitment.sourcePath->sourceRevision),
                             .expectedBookRevision = book.bookRevision,
                             .kind = MaterialCommitmentCommandKind::Release,
                             .commitmentIdentities = {commitment.identity}},
                            now);
        return persistenceResult(applied.status, "profession_material_source_release_persisting", std::move(blocker));
    };

    if (active != book.commitments.end())
    {
        if (!active->sourcePath || active->sourcePath->actorGuid != bot->GetGUID().GetCounter())
        {
            return result(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_commitment_not_executable",
                          EconomyAttemptOutcome::NoCandidate);
        }
        MaterialSourcePath const& path = *active->sourcePath;
        if (path.phase == MaterialSourcePhase::Selected)
        {
            std::optional<ResolvedMaterialSourceDestination> const resolved =
                ResolveMaterialSourceDestination(bot, path);
            if (!resolved ||
                resolved->destination->CountReachablePointsOnMap(bot, *resolved->point, path.requiredResourceCount) <
                    path.requiredResourceCount ||
                StorableGatheringQuantity(bot, path.materialItemId, path.selectedQuantity) < path.selectedQuantity)
            {
                return release(*active, "profession_material_source_capacity_changed");
            }
            MaterialCommitmentApplyResult const applied = authority.Apply(
                {.operationIdentity = Acore::StringFormat("{}:source-start:{}", active->identity, path.sourceRevision),
                 .expectedBookRevision = book.bookRevision,
                 .kind = MaterialCommitmentCommandKind::StartSource,
                 .sourceStarts = {{.commitmentIdentity = active->identity,
                                   .expectedSourceRevision = path.sourceRevision,
                                   .startingInventoryQuantity = bot->GetItemCount(path.materialItemId)}}},
                now);
            return persistenceResult(applied.status, "profession_material_source_start_persisting",
                                     "profession_material_source_start_rejected");
        }
        if (path.phase != MaterialSourcePhase::Acquiring)
        {
            return result(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_source_phase_invalid",
                          EconomyAttemptOutcome::NoCandidate);
        }

        uint32 const currentQuantity = bot->GetItemCount(path.materialItemId);
        uint32 const receivedQuantity =
            currentQuantity >= path.startingInventoryQuantity ? currentQuantity - path.startingInventoryQuantity : 0u;
        if (receivedQuantity >= path.selectedQuantity)
        {
            MaterialCommitmentApplyResult const applied =
                SettleCompletedMaterialSource(authority, book.bookRevision, *active, currentQuantity, now);
            return persistenceResult(applied.status, "profession_material_delivery_persisting",
                                     "profession_material_delivery_rejected");
        }

        uint64 const sourceDeadline = path.neededBy - path.completionObservationBudgetSeconds;
        if (now > sourceDeadline)
            return release(*active, "profession_material_source_deadline_elapsed");
        if (activeGathering && activeGathering->materialCommitmentIdentity != active->identity)
        {
            return result(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_source_actor_busy",
                          EconomyAttemptOutcome::NoCandidate);
        }
        if (!activeGathering)
        {
            std::optional<ResolvedMaterialSourceDestination> const resolved =
                ResolveMaterialSourceDestination(bot, path);
            std::optional<GatheringProfession> const profession = GatheringProfessionForSkill(path.gatheringSkillId);
            if (!resolved || !profession || !TravelToGatheringPoint(botAI, resolved->destination, resolved->point))
                return release(*active, "profession_material_source_route_unavailable");

            ActiveGatheringTrip trip;
            trip.plan.profession = *profession;
            trip.plan.itemId = path.materialItemId;
            trip.plan.requestedQuantity = path.selectedQuantity;
            trip.plan.startingItemCount = path.startingInventoryQuantity;
            trip.plan.startingSkillValue = bot->GetSkillValue(path.gatheringSkillId);
            trip.plan.expiresAt = sourceDeadline;
            trip.skillId = path.gatheringSkillId;
            trip.spellId = intentInput.recipeSpellId;
            trip.startedAt = now;
            trip.outboundSeconds = path.sourceTravelBudgetSeconds;
            trip.materialCommitmentIdentity = active->identity;
            trip.destination = resolved->destination;
            TravelTarget* const target = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (target->getPosition())
                trip.attemptedPoints.push_back(target->getPosition());
            activeGathering = std::move(trip);
            GetPlayerbotEconomyGathering().SetActiveTrip(bot->GetGUID().GetCounter(), activeGathering->skillId);
            return result(PlayerbotEconomyCycleOutcome::Scheduled, "profession_material_source_travel",
                          EconomyAttemptOutcome::Operation);
        }

        MaterialCommitment const retainedCommitment = *active;
        std::optional<PlayerbotEconomyCycleResult> gathering =
            ExecuteAutonomousGathering(botAI, careerPlan, snapshot, {}, intentInput.marketId, now);
        if (!gathering)
            return release(retainedCommitment, "profession_material_source_capability_changed");
        if (!activeGathering && gathering)
        {
            if (gathering->blocker == "gathering_complete")
            {
                MaterialCommitmentApplyResult const applied =
                    SettleCompletedMaterialSource(authority, book.bookRevision, retainedCommitment,
                                                  bot->GetItemCount(retainedCommitment.materialItemId), now);
                return persistenceResult(applied.status, "profession_material_delivery_persisting",
                                         "profession_material_delivery_rejected");
            }
            book = authority.Snapshot();
            return release(retainedCommitment, "profession_material_source_released_without_delivery");
        }
        return gathering;
    }

    if (std::ranges::any_of(book.commitments, [&originIdentity](MaterialCommitment const& commitment)
                            { return commitment.originIdentity == originIdentity; }))
    {
        return result(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_path_already_terminal",
                      EconomyAttemptOutcome::NoCandidate);
    }

    // The latent outcome has several silent exits; name each on the economy logger so a bot that
    // never reaches the path builder can still be read from the log.
    auto const latentStage = [bot, &intentInput](std::string_view stage)
    {
        LOG_INFO("playerbots.economy", "Bot {} material intent for item {} stays latent: {}.",
                 bot->GetGUID().GetCounter(),
                 intentInput.scarceRequirements.empty() ? 0u : intentInput.scarceRequirements.front().itemId, stage);
    };
    MaterialCommitmentEncoding::ProfessionProgressionObserveResult const observed =
        MaterialCommitmentEncoding::ObserveProfessionProgression(intentInput, book, authority, now);
    if (observed.status != MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::NoChange)
    {
        if (observed.status != MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::PendingPersistence &&
            observed.status != MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::Busy)
        {
            latentStage("observe_rejected");
        }
        return persistenceResult(
            observed.status == MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::PendingPersistence
                ? MaterialCommitmentApplyStatus::PendingPersistence
            : observed.status == MaterialCommitmentEncoding::ProfessionProgressionObserveStatus::Busy
                ? MaterialCommitmentApplyStatus::Busy
                : MaterialCommitmentApplyStatus::InvalidCommand,
            "profession_material_intent_persisting", "profession_material_intent_latent");
    }
    if (intentInput.scarceRequirements.size() != 1u || activeGathering)
    {
        latentStage(activeGathering
                        ? "actor_already_gathering"
                        : Acore::StringFormat("{} scarce requirements", intentInput.scarceRequirements.size()));
        return result(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_intent_latent",
                      EconomyAttemptOutcome::NoCandidate);
    }

    auto const durableIntent = std::ranges::find(book.intents, originIdentity, &MaterialIntent::originIdentity);
    if (durableIntent == book.intents.end() || durableIntent->neededBy.has_value())
    {
        latentStage(durableIntent == book.intents.end() ? "durable_intent_missing" : "intent_already_has_horizon");
        return result(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_intent_latent",
                      EconomyAttemptOutcome::NoCandidate);
    }
    std::optional<MaterialSourcePath> const path = BuildProgressionMaterialSourcePath(
        bot, careerPlan, intentInput.scarceRequirements.front(), intentInput.marketId, now);
    if (!path)
    {
        return result(PlayerbotEconomyCycleOutcome::NoCandidate, "profession_material_intent_latent",
                      EconomyAttemptOutcome::NoCandidate);
    }

    MaterialCapacityKey const capacity{MaterialCapacityKind::GatheringCapacity, path->capacityIdentity};
    MaterialCommitmentApplyResult const admitted =
        authority.Apply({.operationIdentity = Acore::StringFormat("material-source-admit:{}", path->sourceRevision),
                         .expectedBookRevision = book.bookRevision,
                         .kind = MaterialCommitmentCommandKind::Admit,
                         .candidates = {{.originIdentity = originIdentity,
                                         .ownerRevision = durableIntent->ownerRevision,
                                         .reservations = {{.materialItemId = path->materialItemId,
                                                           .capacity = capacity,
                                                           .authorityRevision = path->sourceRevision,
                                                           .backedMaterialQuantity = path->selectedQuantity,
                                                           .capacityQuantity = path->requiredResourceCount}},
                                         .sourcePaths = {*path}}},
                         .capacityObservations = {{.capacity = capacity,
                                                   .unit = MaterialCapacityUnit::GatheringUnits,
                                                   .materialItemId = path->materialItemId,
                                                   .authorityRevision = path->sourceRevision,
                                                   .availableQuantity = path->availableResourceCount}}},
                        now);
    if (admitted.status != MaterialCommitmentApplyStatus::PendingPersistence &&
        admitted.status != MaterialCommitmentApplyStatus::Idempotent &&
        admitted.status != MaterialCommitmentApplyStatus::Busy)
    {
        latentStage(Acore::StringFormat("admission_rejected (status {}, required {}, available {}, revision {})",
                                        static_cast<uint32>(admitted.status), path->requiredResourceCount,
                                        path->availableResourceCount, path->sourceRevision));
    }
    return persistenceResult(admitted.status, "profession_material_source_admission_persisting",
                             "profession_material_intent_latent");
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteProfessionProgression(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan, EconomySnapshot const& snapshot, uint64 now)
{
    Player* const bot = botAI->GetBot();
    if (activeGathering && !activeGathering->materialCommitmentIdentity.empty())
    {
        MaterialCommitmentSnapshot const commitments = GetPlayerbotMaterialCommitmentAuthority().Snapshot();
        auto const retained = std::ranges::find(commitments.commitments, activeGathering->materialCommitmentIdentity,
                                                &MaterialCommitment::identity);
        if (retained != commitments.commitments.end() && retained->state != MaterialCommitmentState::Admitted &&
            retained->state != MaterialCommitmentState::PartiallyFulfilled)
        {
            Reset(botAI);
        }
    }
    auto const availableInventory = [this, &snapshot](uint32 itemId)
    { return ProgressionAvailableInventory(snapshot, itemId); };
    if (pendingProgressionCraft && activeProgressionMilestone)
    {
        uint16 const currentSkill = bot->GetPureSkillValue(activeProgressionMilestone->professionSkillId);
        uint32 const currentOutput = bot->GetItemCount(activeProgressionMilestone->outputItemId);
        PlayerbotCareer::ProfessionProgressionCycleDecision const progression =
            PlayerbotCareer::DecideProfessionProgressionCycle({
                .authority = ProgressionAuthority(botAI),
                .observation = {.currentSkill = currentSkill},
                .milestone = activeProgressionMilestone,
                .batchRemaining = activeProgressionBatchRemaining,
                .attempt =
                    PlayerbotCareer::ProfessionProgressionAttemptObservation{
                        .startingSkill = pendingProgressionCraft->startingSkill,
                        .currentSkill = currentSkill,
                        .startingOutputQuantity = pendingProgressionCraft->startingOutputQuantity,
                        .currentOutputQuantity = currentOutput,
                        .elapsedSeconds = static_cast<uint32>(
                            now > pendingProgressionCraft->startedAt ? now - pendingProgressionCraft->startedAt : 0u),
                    },
            });
        if (!progression.retainAttempt)
            pendingProgressionCraft.reset();
        if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::Preempted)
        {
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
            result.phase = EconomyPhase::Craft;
            result.workIdentity = {activeProgressionMilestone->recipeSpellId, activeProgressionMilestone->outputItemId,
                                   0u, 0u};
            result.blocker = PlayerbotCareer::ProgressionBlockerCode(progression.blocker);
            result.schedulingEffect = EconomyAttemptOutcome::InProgress;
            return result;
        }
        if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::WaitObservation)
        {
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
            result.phase = EconomyPhase::Craft;
            result.workIdentity = {activeProgressionMilestone->recipeSpellId, activeProgressionMilestone->outputItemId,
                                   0u, 0u};
            result.blocker =
                progression.outputObserved ? "profession_skill_advance_pending" : "profession_craft_completion_pending";
            result.schedulingEffect = EconomyAttemptOutcome::InProgress;
            return result;
        }
        if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::ObservationBlocked)
        {
            uint32 const workOrderSpellId = sRandomPlayerbotMgr.GetValue(bot, PROFESSION_WORK_ORDER_EVENT);
            EconomyCoordinatorSnapshot const coordinator = GetPlayerbotEconomyCoordinator().Snapshot(now);
            if (CanClearTimedOutProgressionWorkOrder(workOrderSpellId, activeProgressionMilestone->recipeSpellId,
                                                     bot->GetGUID().GetCounter(), coordinator.claims))
            {
                sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
            }
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
            result.phase = EconomyPhase::Craft;
            result.workIdentity = {activeProgressionMilestone->recipeSpellId, activeProgressionMilestone->outputItemId,
                                   0u, 0u};
            result.blocker = progression.outputObserved ? "profession_skill_advance_unobserved_after_output"
                                                        : "profession_craft_completion_unobserved";
            result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
            return result;
        }
        if (progression.action != PlayerbotCareer::ProfessionProgressionCycleAction::AttemptAdvanced &&
            progression.action != PlayerbotCareer::ProfessionProgressionCycleAction::Complete)
        {
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
            result.phase = EconomyPhase::Craft;
            result.workIdentity = {activeProgressionMilestone->recipeSpellId, activeProgressionMilestone->outputItemId,
                                   0u, 0u};
            result.blocker = "profession_progression_invalid_attempt_state";
            result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
            return result;
        }

        bool const targetReached = currentSkill >= activeProgressionMilestone->targetSkill;
        bool const progressionComplete =
            progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::Complete;
        activeProgressionMilestone = progression.milestone;
        activeProgressionBatchRemaining = progression.batchRemaining;
        if (progressionComplete)
        {
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        }

        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.phase = EconomyPhase::Craft;
        result.blocker = targetReached ? "profession_milestone_completed" : "profession_batch_completed";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }

    // Whether the bot can source a reagent it lacks: a node it can gather at its current skill, or an
    // accessible auction listing, of the reagent or of a green it can break into the reagent. Decides
    // which advancing recipe the milestone picks.
    std::unordered_map<uint32, bool> obtainableByItem;
    auto const directlyObtainable = [&](uint32 itemId)
    {
        auto const cached = obtainableByItem.find(itemId);
        if (cached != obtainableByItem.end())
            return cached->second;
        bool result = std::any_of(
            snapshot.auctions.begin(), snapshot.auctions.end(),
            [itemId](AuctionListingCandidate const& listing)
            {
                return listing.accessible &&
                       (listing.itemId == itemId ||
                        std::find(listing.disenchantYieldItemIds.begin(), listing.disenchantYieldItemIds.end(),
                                  itemId) != listing.disenchantYieldItemIds.end());
            });
        if (!result)
        {
            std::optional<uint32> const skillId = GatheringSkillForItem(sObjectMgr->GetItemTemplate(itemId));
            if (skillId && bot->HasSkill(*skillId))
            {
                GatheringDestinationBlocker blocker = GatheringDestinationBlocker::Empty;
                result = !sPlayerbotEconomyTravelCatalog
                              .GatheringDestinations(bot, *skillId, &blocker, true, 5000.0f, itemId)
                              .empty();
            }
        }
        obtainableByItem.emplace(itemId, result);
        return result;
    };
    // The herb a pigment would be milled from: one the market lists, else one the bot's own Herbalism
    // reaches, else the lowest skill herb overall so a scribe without Herbalism still states a demand
    // a herbalist can answer. Zero for anything that is not a pigment.
    auto const millingInput = [&](uint32 itemId) -> uint32
    {
        auto const& inputs = MillingInputs();
        auto const herbs = inputs.find(itemId);
        if (herbs == inputs.end())
            return 0u;
        uint32 selected = 0u;
        int selectedRank = -1;
        for (uint32 herbId : herbs->second)
        {
            ItemTemplate const* const proto = sObjectMgr->GetItemTemplate(herbId);
            if (!proto)
                continue;
            int rank = 0;
            if (directlyObtainable(herbId))
                rank = 1;
            bool const better = rank > selectedRank ||
                                (rank == selectedRank &&
                                 proto->RequiredSkillRank < sObjectMgr->GetItemTemplate(selected)->RequiredSkillRank);
            if (better)
            {
                selected = herbId;
                selectedRank = rank;
            }
        }
        return selected;
    };
    auto const obtainable = [&](uint32 itemId)
    {
        if (directlyObtainable(itemId))
            return true;
        uint32 const herbId = millingInput(itemId);
        return herbId != 0u && directlyObtainable(herbId);
    };

    std::unordered_set<uint64> const progressionControlledGuids(snapshot.controlledItemGuids.begin(),
                                                                snapshot.controlledItemGuids.end());
    std::unordered_map<uint32, bool> disenchantableByItem;
    auto const disenchantable = [&](uint32 itemId)
    {
        auto const cached = disenchantableByItem.find(itemId);
        if (cached != disenchantableByItem.end())
            return cached->second;
        bool const result = SelectDisenchantSource(botAI, itemId, progressionControlledGuids) != nullptr;
        disenchantableByItem.emplace(itemId, result);
        return result;
    };

    std::unordered_map<uint32, bool> millableByItem;
    auto const millable = [&](uint32 itemId)
    {
        auto const cached = millableByItem.find(itemId);
        if (cached != millableByItem.end())
            return cached->second;
        bool const result = SelectMillingSource(botAI, itemId, progressionControlledGuids) != nullptr;
        millableByItem.emplace(itemId, result);
        return result;
    };

    std::vector<PlayerbotCareer::ProfessionProgressionRecipe> progressionRecipes;
    progressionRecipes.reserve(snapshot.recipes.size());
    for (RecipeCandidate const& candidate : snapshot.recipes)
    {
        if (!candidate.professionSkillId)
            continue;
        PlayerbotCareer::ProfessionProgressionRecipe recipe;
        recipe.professionSkillId = candidate.professionSkillId;
        recipe.spellId = candidate.spellId;
        recipe.outputItemId = candidate.craftedItemId;
        recipe.known = bot->HasSpell(candidate.spellId);
        recipe.advancesSkill = candidate.givesSkillUp;
        for (ReagentRequirement const& reagent : candidate.reagents)
        {
            recipe.reagents.push_back({
                .itemId = reagent.itemId,
                .count = reagent.count,
                .ownedCount = availableInventory(reagent.itemId),
                .ordinaryVendorAvailable = reagent.unlimitedGoldVendorSupply,
                .obtainable = !reagent.unlimitedGoldVendorSupply && obtainable(reagent.itemId),
                .disenchantable = !reagent.unlimitedGoldVendorSupply && disenchantable(reagent.itemId),
                .millable = !reagent.unlimitedGoldVendorSupply && millable(reagent.itemId),
                .millingInputItemId = reagent.unlimitedGoldVendorSupply ? 0u : millingInput(reagent.itemId),
            });
        }
        progressionRecipes.push_back(std::move(recipe));
    }

    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(bot->GetGUID().GetCounter());
    if (!personality)
        return std::nullopt;

    std::vector<uint16> plannedSkills = PlayerbotCareer::PlannedSkills(careerPlan);
    if (bot->HasSkill(SKILL_COOKING))
        plannedSkills.push_back(SKILL_COOKING);
    if (bot->HasSkill(SKILL_FIRST_AID))
        plannedSkills.push_back(SKILL_FIRST_AID);
    std::sort(plannedSkills.begin(), plannedSkills.end());
    plannedSkills.erase(std::unique(plannedSkills.begin(), plannedSkills.end()), plannedSkills.end());

    std::vector<PlayerbotCareer::ProfessionProgressionState> professions;
    for (uint16 skillId : plannedSkills)
    {
        if (!bot->HasSkill(skillId) || IsGatheringProfessionSkill(skillId))
            continue;
        uint16 const current = bot->GetPureSkillValue(skillId);
        uint16 const currentCap = bot->GetPureMaxSkillValue(skillId);
        if (!currentCap)
            continue;
        bool const hasAdvancingRecipe =
            std::any_of(progressionRecipes.begin(), progressionRecipes.end(), [skillId](auto const& recipe)
                        { return recipe.professionSkillId == skillId && recipe.known && recipe.advancesSkill; });
        bool const rankRequired = current >= currentCap && currentCap < sWorld->GetConfigMaxSkillValue();
        bool const recipeRequired = current < currentCap && !hasAdvancingRecipe && SkillAdvancesThroughRecipes(skillId);
        uint16 const target = rankRequired ? static_cast<uint16>(currentCap + 1u) : currentCap;
        professions.push_back({
            .professionSkillId = skillId,
            .currentSkill = current,
            .targetSkill = target,
            .affinity = personality->craftingAffinity,
            .planned = true,
            .learned = true,
            .trainerRankRequired = rankRequired,
            .trainerRecipeRequired = recipeRequired,
        });
    }

    PlayerbotCareer::ProfessionProgressionCycleDecision const progression =
        PlayerbotCareer::DecideProfessionProgressionCycle({
            .professions = professions,
            .recipes = progressionRecipes,
            .authority = ProgressionAuthority(botAI),
            .observation = {},
            .milestone = activeProgressionMilestone,
            .batchRemaining = activeProgressionBatchRemaining,
        });
    activeProgressionMilestone = progression.milestone;
    activeProgressionBatchRemaining = progression.batchRemaining;
    if (!activeProgressionMilestone)
    {
        activeProgressionBatchRemaining = 0u;
        return std::nullopt;
    }
    PlayerbotCareer::ProfessionProgressionMilestone const& selected = *activeProgressionMilestone;

    if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::TrainerRank ||
        progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::TrainerRecipe)
    {
        PlayerbotCareer::ProfessionProgressionGameplayExecution const execution =
            PlayerbotCareer::ExecuteProfessionProgressionGameplay(
                progression,
                {.scheduleTrainer = [this, &careerPlan, &progression](auto const& milestone)
                 {
                     std::vector<uint16> const primarySkills = PlayerbotCareer::EffectivePrimarySkills(careerPlan);
                     activeTrainerObjective = PlayerbotCareerTrainerObjective{
                         .kind = PlayerbotCareerTrainerObjectiveKind::Progression,
                         .professionSkillId = milestone.professionSkillId,
                         .primaryProfession = std::find(primarySkills.begin(), primarySkills.end(),
                                                        milestone.professionSkillId) != primarySkills.end(),
                         .rankOnly =
                             progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::TrainerRank,
                     };
                     return true;
                 }});
        PlayerbotEconomyCycleResult result;
        result.outcome = execution.succeeded ? PlayerbotEconomyCycleOutcome::Scheduled
                                             : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::TrainerRank
                             ? "profession_trainer_rank_selected"
                             : "profession_trainer_recipe_selected";
        result.schedulingEffect =
            execution.succeeded ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::BuyVendorInput)
    {
        std::optional<PlayerbotEconomyCycleResult> vendorResult;
        [[maybe_unused]] PlayerbotCareer::ProfessionProgressionGameplayExecution const execution =
            PlayerbotCareer::ExecuteProfessionProgressionGameplay(
                progression, {.buyVendorInput = [this, botAI, &vendorResult](uint32 itemId, uint32 recipeSpellId)
                              {
                                  vendorResult = BuyProgressionVendorInput(botAI, itemId, recipeSpellId);
                                  return vendorResult->outcome != PlayerbotEconomyCycleOutcome::FailedPrecondition;
                              }});
        return vendorResult;
    }
    if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::Disenchant)
    {
        std::optional<PlayerbotEconomyCycleResult> disenchantResult;
        [[maybe_unused]] PlayerbotCareer::ProfessionProgressionGameplayExecution const execution =
            PlayerbotCareer::ExecuteProfessionProgressionGameplay(
                progression,
                {.disenchant = [this, botAI, &snapshot, &disenchantResult](uint32 itemId, uint32 recipeSpellId)
                 {
                     disenchantResult = DisenchantProgressionInput(botAI, snapshot, itemId, recipeSpellId);
                     return disenchantResult->outcome != PlayerbotEconomyCycleOutcome::FailedPrecondition;
                 }});
        return disenchantResult;
    }
    if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::Mill)
    {
        std::optional<PlayerbotEconomyCycleResult> millResult;
        [[maybe_unused]] PlayerbotCareer::ProfessionProgressionGameplayExecution const execution =
            PlayerbotCareer::ExecuteProfessionProgressionGameplay(
                progression, {.mill = [this, botAI, &snapshot, &millResult](uint32 itemId, uint32 recipeSpellId)
                              {
                                  millResult = MillProgressionInput(botAI, snapshot, itemId, recipeSpellId);
                                  return millResult->outcome != PlayerbotEconomyCycleOutcome::FailedPrecondition;
                              }});
        return millResult;
    }
    if (progression.action == PlayerbotCareer::ProfessionProgressionCycleAction::Blocked)
    {
        if (progression.blocker == PlayerbotCareer::ProfessionProgressionBlocker::MaterialSourceUnavailable)
        {
            std::optional<MaterialCommitmentEncoding::ProfessionProgressionIntentInput> const intent =
                ProgressionIntentInput(bot->GetGUID().GetCounter(), AuctionMarketId(bot->GetFaction()), selected,
                                       progression, professions, progressionRecipes);
            if (intent)
                return ExecuteProgressionMaterialSource(botAI, careerPlan, snapshot, *intent, now);
        }
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.phase = EconomyPhase::Craft;
        result.workIdentity = {selected.recipeSpellId, progression.itemId, 0u, 0u};
        result.blocker =
            Acore::StringFormat("{}:item:{}:owned_or_ordinary_vendor",
                                PlayerbotCareer::ProgressionBlockerCode(progression.blocker), progression.itemId);
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }
    if (progression.action != PlayerbotCareer::ProfessionProgressionCycleAction::Craft)
        return std::nullopt;

    if (std::optional<uint32> const tool = MissingVendorTool(bot, sSpellMgr->GetSpellInfo(selected.recipeSpellId)))
        return BuyProgressionVendorInput(botAI, *tool, selected.recipeSpellId);

    PendingProgressionCraft const pending{
        .startingSkill = bot->GetPureSkillValue(selected.professionSkillId),
        .startingOutputQuantity = bot->GetItemCount(selected.outputItemId),
        .startedAt = now,
    };
    ExecutionResult gameplayResult = ExecutionResult::Failed;
    PlayerbotCareer::ProfessionProgressionGameplayExecution const execution =
        PlayerbotCareer::ExecuteProfessionProgressionGameplay(
            progression, {.craft = [this, botAI, bot, &gameplayResult](uint32 recipeSpellId, uint32 outputItemId)
                          {
                              EconomyDecision craft;
                              craft.phase = EconomyPhase::Craft;
                              craft.spellId = recipeSpellId;
                              craft.itemId = outputItemId;
                              sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, recipeSpellId);
                              gameplayResult = ExecuteDecision(botAI, craft, nullptr);
                              return gameplayResult == ExecutionResult::Operation;
                          }});

    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::Craft;
    result.workIdentity = {selected.recipeSpellId, selected.outputItemId, 0u, 0u};
    if (execution.succeeded)
    {
        progressionTrainingOutputs.insert(selected.outputItemId);
        pendingProgressionCraft = pending;
        ReleaseMaterialSourceConsumedByCraft(botAI, selected, now);
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
        result.blocker = "profession_craft_started";
        result.schedulingEffect = EconomyAttemptOutcome::InProgress;
    }
    else if (gameplayResult == ExecutionResult::Scheduled)
    {
        // The fire is lit or the bot is walking to a forge; the craft itself comes next cycle.
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
        result.blocker = "profession_craft_prerequisite_pending";
        result.schedulingEffect = EconomyAttemptOutcome::InProgress;
    }
    else
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "profession_craft_failed_precondition";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
    }
    return result;
}

PlayerbotEconomyCycleResult DefaultPlayerbotEconomyRuntime::DisenchantProgressionInput(PlayerbotAI* botAI,
                                                                                       EconomySnapshot const& snapshot,
                                                                                       uint32 itemId,
                                                                                       uint32 recipeSpellId)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::Craft;
    result.workIdentity = {recipeSpellId, itemId, 0u, 0u};
    if (HoldsFishingPole(bot))
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "profession_disenchant_blocked_fishing_pole";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    std::unordered_set<uint64> const controlledItemGuids(snapshot.controlledItemGuids.begin(),
                                                         snapshot.controlledItemGuids.end());
    Item* const source = SelectDisenchantSource(botAI, itemId, controlledItemGuids);
    if (!source)
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = Acore::StringFormat("profession_disenchant_source_missing:item:{}", itemId);
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    // Same footing as a craft: the cast check reports nothing useful while mounted or mid-step.
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear(true);
        bot->StopMoving();
    }
    if (!botAI->CanCastSpell(DISENCHANT_SPELL_ID, bot, true, source))
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = Acore::StringFormat("profession_disenchant_cast_blocked:item:{}", source->GetEntry());
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!botAI->CastSpell(DISENCHANT_SPELL_ID, bot, source))
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = Acore::StringFormat("profession_disenchant_cast_rejected:item:{}", source->GetEntry());
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    // The cast takes a few seconds and leaves a loot window behind; the next cycle collects it before
    // it looks at the bags again.
    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.blocker = Acore::StringFormat("profession_disenchant_started:item:{}", source->GetEntry());
    result.schedulingEffect = EconomyAttemptOutcome::InProgress;
    return result;
}

PlayerbotEconomyCycleResult DefaultPlayerbotEconomyRuntime::MillProgressionInput(PlayerbotAI* botAI,
                                                                                 EconomySnapshot const& snapshot,
                                                                                 uint32 itemId, uint32 recipeSpellId)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::Craft;
    result.workIdentity = {recipeSpellId, itemId, 0u, 0u};
    std::unordered_set<uint64> const controlledItemGuids(snapshot.controlledItemGuids.begin(),
                                                         snapshot.controlledItemGuids.end());
    Item* const source = SelectMillingSource(botAI, itemId, controlledItemGuids);
    if (!source)
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = Acore::StringFormat("profession_mill_source_missing:item:{}", itemId);
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    // Same footing as a disenchant: the cast check reports nothing useful while mounted or mid-step.
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    if (bot->isMoving())
    {
        bot->GetMotionMaster()->Clear(true);
        bot->StopMoving();
    }
    if (!botAI->CanCastSpell(MILLING_SPELL_ID, bot, true, source))
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = Acore::StringFormat("profession_mill_cast_blocked:item:{}", source->GetEntry());
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!botAI->CastSpell(MILLING_SPELL_ID, bot, source))
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = Acore::StringFormat("profession_mill_cast_rejected:item:{}", source->GetEntry());
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    // The cast leaves a pigment loot window behind; the next cycle collects it before the bag scan.
    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.blocker = Acore::StringFormat("profession_mill_started:item:{}", source->GetEntry());
    result.schedulingEffect = EconomyAttemptOutcome::InProgress;
    return result;
}

PlayerbotEconomyCycleResult DefaultPlayerbotEconomyRuntime::BuyProgressionVendorInput(PlayerbotAI* botAI, uint32 itemId,
                                                                                      uint32 recipeSpellId,
                                                                                      uint32 count)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    ItemTemplate const* const itemTemplate = sObjectMgr->GetItemTemplate(itemId);
    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {recipeSpellId, itemId, 0u, 0u};
    if (!itemTemplate)
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "profession_vendor_item_missing";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    GuidVector const nearby = AI_VALUE(GuidVector, "nearest npcs");
    Creature* vendor = nullptr;
    uint32 vendorSlot = 0u;
    for (ObjectGuid const guid : nearby)
    {
        Creature* const candidate = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_VENDOR);
        VendorItemData const* const offers = candidate ? candidate->GetVendorItems() : nullptr;
        if (!offers)
            continue;
        for (uint32 slot = 0u; slot < offers->m_items.size(); ++slot)
        {
            VendorItem const* offer = offers->m_items[slot];
            if (offer && offer->item == itemId && !offer->maxcount && !offer->ExtendedCost)
            {
                vendor = candidate;
                vendorSlot = slot;
                break;
            }
        }
        if (vendor)
            break;
    }

    if (vendor)
    {
        uint32 const quantity = std::clamp(count, 1u, static_cast<uint32>(std::numeric_limits<uint8>::max()));
        uint32 const price = static_cast<uint32>(
            std::floor(itemTemplate->BuyPrice * bot->GetReputationPriceDiscount(vendor)) * quantity);
        uint32 const protectedMoney =
            AI_VALUE2(uint32, "free money for", static_cast<uint32>(NeedMoneyFor::tradeskill));
        if (price > protectedMoney || price > bot->GetMoney())
        {
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = Acore::StringFormat("profession_vendor_budget_blocked:item:{}", itemId);
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            return result;
        }

        uint32 const before = bot->GetItemCount(itemId);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), vendorSlot, itemId, static_cast<uint8>(quantity), NULL_BAG,
                                   NULL_SLOT);
        if (bot->GetItemCount(itemId) > before)
        {
            Reset(botAI);
            result.outcome = PlayerbotEconomyCycleOutcome::Operation;
            result.blocker = "profession_vendor_input_purchased";
            result.schedulingEffect = EconomyAttemptOutcome::Operation;
            return result;
        }
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = Acore::StringFormat("profession_vendor_purchase_unobserved:item:{}", itemId);
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    TravelTarget* const target = AI_VALUE(TravelTarget*, "travel target");
    if (target->isForced() && (!ownedTravelDestination || target->getDestination() != ownedTravelDestination))
    {
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.blocker = Acore::StringFormat("profession_vendor_route_preempted:item:{}", itemId);
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }
    // TravelToDestination keeps a journey already under way and re-issues one that was cleared or expired,
    // so call it every cycle: guarding on ownedTravelDestination left a bot whose route had been dropped
    // reporting vendor travel forever while standing still.
    if (!TravelToDestination(botAI, sPlayerbotEconomyTravelCatalog.SelectVendor(bot, itemId)))
    {
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.blocker = Acore::StringFormat("profession_material_source_unavailable:item:{}:ordinary_vendor", itemId);
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }

    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.blocker = Acore::StringFormat("profession_vendor_travel:item:{}", itemId);
    result.schedulingEffect = EconomyAttemptOutcome::InProgress;
    return result;
}

bool DefaultPlayerbotEconomyRuntime::IsEligible(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan) const
{
    Player* const bot = botAI->GetBot();
    PlayerbotCareer::ProfessionProgressionAuthority const authority = ProgressionAuthority(botAI);
    if (authority.Blocker() != PlayerbotCareer::ProfessionProgressionBlocker::None)
        return false;

    EconomyEligibility eligibility;
    eligibility.enabled = sPlayerbotEconomyConfig.lifecycleEnabled;
    eligibility.randomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
    eligibility.activePlayerMaster = IsRealPlayer(botAI->GetMaster());
    eligibility.inCombat = bot->IsInCombat();
    eligibility.inBattleground = bot->InBattleground();
    eligibility.dead = bot->isDead();
    eligibility.teleporting = bot->IsBeingTeleported();
    std::optional<PlayerbotPersonalityProfile> const personality =
        sPlayerbotPersonalityMgr.GetOrCreate(bot->GetGUID().GetCounter());
    bool const capabilityCandidate =
        personality &&
        (personality->craftingAffinity >= PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM ||
         personality->gatheringAffinity >= PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM) &&
        (bot->GetFreePrimaryProfessionPoints() || !LearnedPrimaryCapabilitySkillIds(bot).empty());
    bool const universalProgression = bot->HasSkill(SKILL_COOKING) || bot->HasSkill(SKILL_FIRST_AID);
    eligibility.careerMarketEligible =
        PlayerbotCareer::SchedulesProfessionWork(careerPlan) || capabilityCandidate || universalProgression;
    eligibility.hasActionableProfessionWork = !PlayerbotCareer::PlannedSkills(careerPlan).empty() ||
                                              careerPlan.capabilityGoal.has_value() || capabilityCandidate ||
                                              universalProgression;
    return PlayerbotEconomyPolicy::IsEligible(eligibility);
}

std::vector<ProfessionCapability> const& DefaultPlayerbotEconomyRuntime::CapabilityCandidates(
    Player const* bot, EconomySubstitutionGroup const& group)
{
    auto const key = std::make_pair(bot->getClass(), group);
    auto const existing = capabilityCandidates.find(key);
    if (existing != capabilityCandidates.end())
        return existing->second;

    std::vector<ProfessionCapability> candidates;
    for (ProfessionCapability const& capability : PlayerbotProfessionCapabilityCatalog::All())
    {
        if (!capability.primaryProfession)
            continue;

        bool const matches = ProductionOutputMatchesGroup(bot, capability.outputItemId, group);
        if (matches)
            candidates.push_back(capability);
    }

    return capabilityCandidates.emplace(key, std::move(candidates)).first->second;
}

void DefaultPlayerbotEconomyRuntime::RevalidateCapabilities(PlayerbotAI* botAI, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(now);
    std::vector<uint32> knownRecipes;
    for (EconomyActorFacts const& actor : snapshot.actors)
    {
        if (actor.online && actor.autonomous && actor.marketId == marketId)
            knownRecipes.insert(knownRecipes.end(), actor.recipeSpellIds.begin(), actor.recipeSpellIds.end());
    }
    std::sort(knownRecipes.begin(), knownRecipes.end());
    knownRecipes.erase(std::unique(knownRecipes.begin(), knownRecipes.end()), knownRecipes.end());

    for (EconomyDemandGap const& gap : snapshot.gaps)
    {
        if (gap.marketId != marketId || !gap.remainingQuantity)
            continue;

        std::vector<ProfessionCapability> candidates = CapabilityCandidates(bot, gap.group);
        std::vector<ProfessionCapability> available;
        for (ProfessionCapability const& capability : candidates)
        {
            bool const hasProvider =
                std::any_of(snapshot.actors.begin(), snapshot.actors.end(),
                            [marketId, &capability](EconomyActorFacts const& actor)
                            {
                                if (!actor.online || !actor.autonomous || actor.marketId != marketId ||
                                    std::find(actor.professionSkillIds.begin(), actor.professionSkillIds.end(),
                                              capability.professionSkillId) == actor.professionSkillIds.end())
                                {
                                    return false;
                                }
                                return capability.kind == ProfessionCapabilityKind::Gathering ||
                                       std::find(actor.recipeSpellIds.begin(), actor.recipeSpellIds.end(),
                                                 capability.recipeSpellId) != actor.recipeSpellIds.end();
                            });
            if (hasProvider)
                available.push_back(capability);
        }
        if (!available.empty())
            candidates = std::move(available);

        std::optional<ProfessionCapability> const selected =
            PlayerbotProfessionCapabilityCatalog::Select(std::move(candidates), knownRecipes, true);
        if (!selected)
            continue;

        coordinator.RevalidateCapability({{marketId, gap.group, *selected}, true}, now);
    }
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ReconcileCapabilityGoal(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    EconomyCoordinatorSnapshot const snapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    auto const matchesGoal = [&careerPlan, marketId](EconomyCapabilityBlocker const& blocker)
    {
        if (!careerPlan.capabilityGoal || blocker.requirement.marketId != marketId)
            return false;
        PlayerbotCareerCapabilityGoal const& goal = *careerPlan.capabilityGoal;
        ProfessionCapability const& capability = blocker.requirement.capability;
        return goal.professionSkillId == capability.professionSkillId &&
               goal.recipeSpellId == capability.recipeSpellId && goal.outputItemId == capability.outputItemId;
    };

    if (careerPlan.capabilityGoal)
    {
        PlayerbotCareerCapabilityGoal const& goal = *careerPlan.capabilityGoal;
        auto const matching =
            std::find_if(snapshot.capabilityBlockers.begin(), snapshot.capabilityBlockers.end(), matchesGoal);
        bool const satisfied = goal.kind == PlayerbotCareerCapabilityGoalKind::Trainer
                                   ? bot->HasSkill(goal.professionSkillId)
                                   : bot->HasSpell(goal.recipeSpellId);
        bool const reassigned = matching != snapshot.capabilityBlockers.end() &&
                                matching->state == EconomyCapabilityBlockerState::Persistent &&
                                matching->assignedActorGuid &&
                                matching->assignedActorGuid != bot->GetGUID().GetCounter();
        if (satisfied || matching == snapshot.capabilityBlockers.end() || reassigned)
        {
            PlayerbotCareerPlan updated = careerPlan;
            if (PlayerbotCareer::ClearCapabilityGoal(updated))
            {
                PlayerbotCareer::SavePersistentPlan(bot->GetGUID().GetCounter(), updated);
                PlayerbotEconomyCycleResult result;
                result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
                result.phase = goal.kind == PlayerbotCareerCapabilityGoalKind::Recipe ? EconomyPhase::BuyRecipe
                                                                                      : EconomyPhase::None;
                result.workIdentity = {goal.recipeSpellId, goal.outputItemId, 0u, 0u};
                result.blocker = "capability_goal_cleared";
                result.schedulingEffect = EconomyAttemptOutcome::Operation;
                return result;
            }
        }
        return std::nullopt;
    }

    auto const assigned = std::find_if(snapshot.capabilityBlockers.begin(), snapshot.capabilityBlockers.end(),
                                       [bot, marketId](EconomyCapabilityBlocker const& blocker)
                                       {
                                           return blocker.requirement.marketId == marketId &&
                                                  blocker.state == EconomyCapabilityBlockerState::Persistent &&
                                                  blocker.assignedActorGuid == bot->GetGUID().GetCounter() &&
                                                  blocker.assignedWorkKind.has_value();
                                       });
    if (assigned == snapshot.capabilityBlockers.end())
        return std::nullopt;

    PlayerbotCareerPlan updated = careerPlan;
    ProfessionCapability const& capability = assigned->requirement.capability;
    PlayerbotCareerCapabilityGoal const goal = {
        *assigned->assignedWorkKind == EconomyWorkKind::Recipe ? PlayerbotCareerCapabilityGoalKind::Recipe
                                                               : PlayerbotCareerCapabilityGoalKind::Trainer,
        capability.professionSkillId,
        capability.recipeSpellId,
        capability.outputItemId,
    };
    if (!PlayerbotCareer::TryAssignCapabilityGoal(updated, goal, PrimaryCapabilitySkillIds(),
                                                  LearnedPrimaryCapabilitySkillIds(bot)))
    {
        return std::nullopt;
    }

    PlayerbotCareer::SavePersistentPlan(bot->GetGUID().GetCounter(), updated);
    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.phase =
        goal.kind == PlayerbotCareerCapabilityGoalKind::Recipe ? EconomyPhase::BuyRecipe : EconomyPhase::None;
    result.workIdentity = {goal.recipeSpellId, goal.outputItemId, 0u, 0u};
    result.blocker = "capability_goal_assigned";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteTrainerObjective(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan)
{
    Player* const bot = botAI->GetBot();
    if (activeTrainerObjective && activeTrainerObjective->kind != PlayerbotCareerTrainerObjectiveKind::Progression &&
        bot->HasSkill(activeTrainerObjective->professionSkillId))
    {
        PlayerbotCareerTrainerObjective const completed = *activeTrainerObjective;
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.blocker = completed.kind == PlayerbotCareerTrainerObjectiveKind::BaseCareer
                             ? "base_career_profession_learned"
                             : "capability_profession_learned";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }

    PlayerbotCareerAcquisition selectedObjective;
    if (activeTrainerObjective && activeTrainerObjective->kind == PlayerbotCareerTrainerObjectiveKind::Progression)
    {
        selectedObjective.objective = activeTrainerObjective;
        selectedObjective.state = PlayerbotCareerAcquisitionState::Travel;
    }
    else
    {
        selectedObjective = PlayerbotCareer::SelectTrainerObjective(
            careerPlan, LearnedCareerSkillIds(bot, careerPlan), PrimaryCapabilitySkillIds(),
            static_cast<uint8>(
                std::min<uint32>(bot->GetFreePrimaryProfessionPoints(), std::numeric_limits<uint8>::max())));
    }
    if (!selectedObjective.objective)
    {
        if (activeTrainerObjective || activeTrainer)
            Reset(botAI);
        return std::nullopt;
    }
    if (selectedObjective.state == PlayerbotCareerAcquisitionState::Blocked)
    {
        if (activeTrainerObjective || activeTrainer)
            Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = PlayerbotCareer::AcquisitionBlockerCode(selectedObjective.blocker);
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!activeTrainerObjective || *activeTrainerObjective != *selectedObjective.objective)
    {
        if (activeTrainerObjective || activeTrainer)
            Reset(botAI);
        activeTrainerObjective = selectedObjective.objective;
    }
    PlayerbotCareerTrainerObjective const objective = *activeTrainerObjective;

    AiObjectContext* const context = botAI->GetAiObjectContext();
    uint32 const availableMoney = AI_VALUE2(uint32, "free money for", static_cast<uint32>(NeedMoneyFor::tradeskill));
    if (!activeTrainer)
    {
        PlayerbotTrainerTravelSelection const selected =
            sPlayerbotEconomyTravelCatalog.SelectTrainer(bot, objective, availableMoney);
        if (!selected.destination)
        {
            // No trainer on the realm can serve this objective. Market work usually claims the cycle
            // before this diagnosis reaches telemetry, so it is logged where it is decided.
            LOG_INFO("playerbots.economy", "Bot {} found no trainer for skill {} (objective kind {}, rankOnly {}): {}.",
                     bot->GetGUID().GetCounter(), objective.professionSkillId, static_cast<uint32>(objective.kind),
                     objective.rankOnly ? 1u : 0u, PlayerbotCareer::AcquisitionBlockerCode(selected.blocker));
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = PlayerbotCareer::AcquisitionBlockerCode(selected.blocker);
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            return result;
        }
        activeTrainer = selected;
    }

    if (!TravelToDestination(botAI, activeTrainer->destination))
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
        result.blocker = "trainer_travel_deferred";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }

    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    if (target->getStatus() != TRAVEL_STATUS_WORK)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
        result.blocker = "trainer_travel";
        result.schedulingEffect = EconomyAttemptOutcome::InProgress;
        return result;
    }

    Creature* trainerCreature = bot->FindNearestCreature(activeTrainer->entry, INTERACTION_DISTANCE * 3.0f);
    Trainer::Trainer* trainer = trainerCreature ? sObjectMgr->GetTrainer(activeTrainer->entry) : nullptr;
    if (!trainerCreature || !trainerCreature->IsAlive() || !trainer)
    {
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "trainer_unavailable";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    float const reputationDiscount = bot->GetReputationPriceDiscount(trainerCreature);
    if (!trainer->IsTrainerValidForPlayer(bot) ||
        !PlayerbotCareer::TrainerOffersCareerLesson(objective, bot, trainer, reputationDiscount,
                                                    std::numeric_limits<uint32>::max()))
    {
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "trainer_ineligible";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!PlayerbotCareer::TrainerOffersCareerLesson(objective, bot, trainer, reputationDiscount, availableMoney))
    {
        Reset(botAI);
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "insufficient_protected_money";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    auto const describeOfferedLessons = [bot, trainer, reputationDiscount]()
    {
        std::vector<PlayerbotTrainerLessonCandidate> offered;
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
            SpellInfo const* spellInfo = trainerSpell ? sSpellMgr->GetSpellInfo(trainerSpell->SpellId) : nullptr;
            if (!trainerSpell || !spellInfo || !trainer->CanTeachSpell(bot, trainerSpell))
                continue;
            uint32 const cost = static_cast<uint32>(std::floor(trainerSpell->MoneyCost * reputationDiscount));
            offered.push_back(PlayerbotCareer::DescribeTrainerLesson(*trainerSpell, spellInfo, bot, cost));
        }
        return offered;
    };

    std::vector<PlayerbotTrainerLessonCandidate> const lessons = describeOfferedLessons();
    bool const heldSkillOnArrival = bot->HasSkill(objective.professionSkillId);
    std::vector<uint32> const selected = PlayerbotCareer::SelectTrainerLessons(objective, lessons);
    uint32 remainingMoney = availableMoney;
    bool attempted = false;
    bool progressionCompleted = false;
    uint16 const startingSkillCap = bot->GetPureMaxSkillValue(objective.professionSkillId);
    PlayerbotCareerAcquisitionBlocker rejectedLesson = PlayerbotCareerAcquisitionBlocker::TrainerIneligible;
    for (uint32 spellId : selected)
    {
        auto const lesson =
            std::find_if(lessons.begin(), lessons.end(), [spellId](PlayerbotTrainerLessonCandidate const& candidate)
                         { return candidate.spellId == spellId; });
        if (lesson == lessons.end())
            continue;
        if (lesson->cost > remainingMoney || lesson->cost > bot->GetMoney())
        {
            rejectedLesson = PlayerbotCareerAcquisitionBlocker::InsufficientProtectedMoney;
            continue;
        }

        uint32 const moneyBefore = bot->GetMoney();
        trainer->TeachSpell(trainerCreature, bot, spellId);
        uint32 const spent = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0u;
        remainingMoney -= std::min(remainingMoney, spent);
        attempted = attempted || spent || bot->HasSpell(spellId);
        if (objective.kind == PlayerbotCareerTrainerObjectiveKind::Progression)
        {
            progressionCompleted = objective.rankOnly
                                       ? bot->GetPureMaxSkillValue(objective.professionSkillId) > startingSkillCap
                                       : bot->HasSpell(spellId);
            if (progressionCompleted)
                break;
        }
        if (bot->HasSkill(objective.professionSkillId))
            break;
    }

    // Learning the profession is only half of what the trainer is standing there for: until the bot
    // owns a recipe it has a skill it can never use. The trainer could not offer its recipes a moment
    // ago because they require the skill, so ask again now that the bot has it.
    if (!heldSkillOnArrival && bot->HasSkill(objective.professionSkillId))
    {
        // Learning a profession also grants the abilities that make it usable: Smelting with Mining,
        // Disenchant with Enchanting, Prospecting with Jewelcrafting, Milling with Inscription. The
        // trainer teach path was not delivering them, so ask the core for the skill's own reward
        // spells. Which spells those are comes from the game's skill data, not from a list kept here.
        bot->learnSkillRewardedSpells(objective.professionSkillId, bot->GetPureSkillValue(objective.professionSkillId));

        PlayerbotCareerTrainerObjective starterRecipes = objective;
        starterRecipes.kind = PlayerbotCareerTrainerObjectiveKind::Progression;
        starterRecipes.rankOnly = false;

        std::vector<PlayerbotTrainerLessonCandidate> const nowOffered = describeOfferedLessons();
        for (uint32 spellId : PlayerbotCareer::SelectTrainerLessons(starterRecipes, nowOffered))
        {
            auto const lesson = std::find_if(nowOffered.begin(), nowOffered.end(),
                                             [spellId](PlayerbotTrainerLessonCandidate const& candidate)
                                             { return candidate.spellId == spellId; });
            if (lesson == nowOffered.end() || lesson->isRank)
                continue;
            if (lesson->cost > remainingMoney || lesson->cost > bot->GetMoney())
                continue;

            uint32 const moneyBefore = bot->GetMoney();
            trainer->TeachSpell(trainerCreature, bot, spellId);
            uint32 const spent = moneyBefore > bot->GetMoney() ? moneyBefore - bot->GetMoney() : 0u;
            remainingMoney -= std::min(remainingMoney, spent);
        }
    }

    if (objective.kind == PlayerbotCareerTrainerObjectiveKind::Progression)
    {
        if (progressionCompleted)
        {
            Reset(botAI);
            activeProgressionMilestone.reset();
            activeProgressionBatchRemaining = 0u;
        }
        PlayerbotEconomyCycleResult result;
        result.outcome = progressionCompleted ? PlayerbotEconomyCycleOutcome::Operation
                                              : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = progressionCompleted ? (objective.rankOnly ? "profession_trainer_rank_learned"
                                                                    : "profession_trainer_recipe_learned")
                         : attempted          ? "profession_trainer_completion_unobserved"
                                              : PlayerbotCareer::AcquisitionBlockerCode(rejectedLesson);
        result.schedulingEffect =
            progressionCompleted ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    PlayerbotCareerAcquisition const acquisition = PlayerbotCareer::EvaluateTrainerObjective(
        objective, {
                       .professionLearned = bot->HasSkill(objective.professionSkillId),
                       .atTrainer = true,
                       .lessonAttempted = attempted,
                   });
    bool const learned = acquisition.state == PlayerbotCareerAcquisitionState::Complete;
    if (learned)
        Reset(botAI);
    PlayerbotEconomyCycleResult result;
    result.outcome =
        learned ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
    result.blocker =
        learned ? (objective.kind == PlayerbotCareerTrainerObjectiveKind::BaseCareer ? "base_career_profession_learned"
                                                                                     : "capability_profession_learned")
        : attempted ? PlayerbotCareer::AcquisitionBlockerCode(acquisition.blocker)
                    : PlayerbotCareer::AcquisitionBlockerCode(rejectedLesson);
    result.schedulingEffect = learned ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ReconcileRecipeLearning(PlayerbotAI* botAI,
                                                                                                   uint64 now)
{
    Player* const bot = botAI->GetBot();
    for (auto committed = committedRecipes.begin(); committed != committedRecipes.end(); ++committed)
    {
        uint64 const itemGuidCounter = committed->first;
        CommittedRecipe const recipe = committed->second;
        if (bot->HasSpell(recipe.recipeSpellId))
        {
            if (!recipe.chainPublicId.empty())
            {
                [[maybe_unused]] bool const recorded =
                    PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                        .Complete(true,
                                  {
                                      .deduplicationKey = Acore::StringFormat(
                                          "recipe-learned:{}:{}", bot->GetGUID().GetCounter(), recipe.recipeSpellId),
                                      .chainPublicId = recipe.chainPublicId,
                                      .actorGuid = bot->GetGUID().GetCounter(),
                                      .counterpartyGuid = recipe.counterpartyGuid,
                                      .itemId = recipe.itemId,
                                      .recipeSpellId = recipe.recipeSpellId,
                                      .quantity = 1u,
                                      .occurredAt = now,
                                      .kind = EconomyTraceKind::FinalUse,
                                      .finalUse = EconomyFinalUseKind::Learned,
                                  });
            }

            committedRecipes.erase(committed);
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::Operation;
            result.phase = EconomyPhase::BuyRecipe;
            result.workIdentity = {recipe.recipeSpellId, recipe.itemId, 0u, itemGuidCounter};
            result.blocker = "recipe_learned";
            result.schedulingEffect = EconomyAttemptOutcome::Operation;
            return result;
        }

        Item* const item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuidCounter));
        if (!item)
            continue;
        if (item->GetEntry() != recipe.itemId)
        {
            committedRecipes.erase(committed);
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
            result.phase = EconomyPhase::BuyRecipe;
            result.workIdentity = {recipe.recipeSpellId, recipe.itemId, 0u, itemGuidCounter};
            result.blocker = "recipe_item_mismatch";
            result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
            return result;
        }
        if (!StartRecipeLearning(botAI, item, recipe.recipeSpellId))
            continue;

        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.phase = EconomyPhase::BuyRecipe;
        result.workIdentity = {recipe.recipeSpellId, recipe.itemId, 0u, itemGuidCounter};
        result.blocker = "recipe_learning_started";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        return result;
    }
    return std::nullopt;
}

PlayerbotEconomyCycleResult DefaultPlayerbotEconomyRuntime::ExecuteCycle(PlayerbotAI* botAI,
                                                                         PlayerbotCareerPlan const& careerPlan)
{
    Player* const bot = botAI->GetBot();
    uint32 const marketId = AuctionMarketId(bot->GetFaction());
    uint64 const now = GameTime::GetGameTime().count();
    ReconcileCraftTrace(bot, now);
    if (std::optional<PlayerbotEconomyCycleResult> learned = ReconcileRecipeLearning(botAI, now))
        return *learned;
    std::optional<PlayerbotEconomyCycleResult> stalledCareerStage;
    char const* releasedConsumptionBlocker = nullptr;
    bool trainerStageStalled = false;
    if (std::optional<PlayerbotEconomyCycleResult> trainerResult = ExecuteTrainerObjective(botAI, careerPlan))
    {
        if (CareerStageOwnsCycle(*trainerResult))
            return *trainerResult;
        stalledCareerStage = std::move(*trainerResult);
        trainerStageStalled = true;
    }
    ObserveMarketEvidence(botAI, marketId, now);
    if (std::optional<PlayerbotEconomyCycleResult> const reconciled = ReconcileMarketPositionMail(botAI, marketId, now))
    {
        return *reconciled;
    }

    Creature* auctioneer = FindAuctioneer(botAI);
    CollectProfessionLoot(bot);
    EconomySnapshot snapshot = BuildSnapshot(botAI, careerPlan);
    ConsumptionSnapshot const consumptionSnapshot = BuildConsumptionSnapshot(botAI, snapshot, marketId, now);
    if (std::optional<PlayerbotEconomyCycleResult> progression =
            ExecuteProfessionProgression(botAI, careerPlan, snapshot, now))
    {
        if (ProgressionStageOwnsCycle(*progression, trainerStageStalled))
            return *progression;
        if (!stalledCareerStage)
            stalledCareerStage = std::move(*progression);
    }
    uint32 excludedItemId = 0u;
    uint32 excludedQuantity = 0u;
    if (activeGathering && activeGathering->plan.itemId)
    {
        excludedItemId = activeGathering->plan.itemId;
        uint32 const current = bot->GetItemCount(excludedItemId);
        excludedQuantity =
            current > activeGathering->plan.startingItemCount
                ? std::min(current - activeGathering->plan.startingItemCount, activeGathering->plan.requestedQuantity)
                : 0u;
        if (activeGathering->coordinatorLeaseId && excludedQuantity > activeGathering->committedQuantity)
        {
            [[maybe_unused]] bool const committed = GetPlayerbotEconomyCoordinator().RecordOutcome(
                activeGathering->coordinatorLeaseId, EconomyAssignmentOutcome::Committed, excludedQuantity, now);
            activeGathering->committedQuantity = excludedQuantity;
        }
        if (activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled &&
            excludedQuantity >= activeGathering->plan.requestedQuantity)
        {
            [[maybe_unused]] bool const completed = GetPlayerbotEconomyCoordinator().RecordOutcome(
                activeGathering->coordinatorLeaseId, EconomyAssignmentOutcome::Completed,
                activeGathering->plan.requestedQuantity, now);
            activeGathering->coordinatorSettled = true;
        }
    }
    RefreshCoordinator(botAI, snapshot, consumptionSnapshot, marketId, now, excludedItemId, excludedQuantity);
    RevalidateCapabilities(botAI, marketId, now);
    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyProductionRequest productionRequest{
        .characterGuid = bot->GetGUID().GetCounter(),
        .marketId = marketId,
        .recipes = ProductionRecipes(bot, snapshot, coordinator.Snapshot(now), marketId),
        .expiresAt = ProductionLeaseExpiry(careerPlan, now),
    };
    EconomyAssignmentLease const productionLease = AssignProduction(coordinator, std::move(productionRequest), now);
    std::optional<EconomyAssignment> const activeProduction = productionLease.assignment;
    if (activeProduction)
    {
        snapshot.preferredRecipeSpellId = activeProduction->recipeSpellId;
        sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, activeProduction->recipeSpellId);
    }
    EconomyDecision const productionDecision = PlayerbotEconomyPolicy::Decide(snapshot);
    if (std::optional<PlayerbotEconomyCycleResult> const capability =
            ReconcileCapabilityGoal(botAI, careerPlan, marketId, now))
    {
        return *capability;
    }
    EconomyMarketSnapshot const marketSnapshot = GetPlayerbotEconomyMarket().Snapshot(now);
    auto const pendingPosition = std::find_if(marketSnapshot.positions.begin(), marketSnapshot.positions.end(),
                                              [bot, marketId](EconomyPosition const& position)
                                              {
                                                  return position.traderGuid == bot->GetGUID().GetCounter() &&
                                                         position.marketId == marketId &&
                                                         position.state == EconomyPositionState::Pending;
                                              });
    if (pendingPosition != marketSnapshot.positions.end())
    {
        if (std::optional<PlayerbotEconomyCycleResult> const pending =
                ManagePendingMarketPosition(botAI, *pendingPosition, marketSnapshot, auctioneer, now))
        {
            return *pending;
        }
    }
    ConsumptionDecision consumptionDecision;
    if (productionDecision.phase != EconomyPhase::CollectAuctionMail)
        consumptionDecision = PlayerbotEconomyConsumption::Decide(consumptionSnapshot);
    std::optional<ExecutionResult> finalUseExecution;
    if (consumptionDecision.action == ConsumptionAction::FinalUse)
    {
        finalUseExecution = ExecuteConsumption(botAI, consumptionDecision, auctioneer);
        if (*finalUseExecution == ExecutionResult::Failed)
            consumptionDecision = {};
    }

    if (consumptionDecision.action != ConsumptionAction::None)
    {
        // Using something already in the bags leaves a gathering trip untouched. Purchases were
        // already deferred while a trip is in flight, so only a recovery still ends one here.
        if (activeGathering && consumptionDecision.action != ConsumptionAction::FinalUse)
        {
            if (activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    activeGathering->coordinatorLeaseId,
                    activeGathering->committedQuantity ? EconomyAssignmentOutcome::InventoryReceived
                                                       : EconomyAssignmentOutcome::NeedChanged,
                    activeGathering->committedQuantity, now);
                activeGathering->coordinatorSettled = true;
            }
            if (activeGathering->plan.itemId && activeGathering->committedQuantity)
                pendingGatheredSupply[activeGathering->plan.itemId] += activeGathering->committedQuantity;
            Reset(botAI);
        }
        PlayerbotEconomyCycleResult result;
        result.phase = consumptionDecision.action == ConsumptionAction::Purchase   ? EconomyPhase::BuyFinishedGood
                       : consumptionDecision.action == ConsumptionAction::FinalUse ? EconomyPhase::UseFinishedGood
                                                                                   : EconomyPhase::RecoverFinishedGood;
        result.workIdentity = {0u, consumptionDecision.itemId, consumptionDecision.auctionId,
                               consumptionDecision.itemGuidCounter};

        ExecutionResult const execution = finalUseExecution.has_value()
                                              ? *finalUseExecution
                                              : ExecuteConsumption(botAI, consumptionDecision, auctioneer);
        if (ConsumptionStepOwnsCycle(execution))
        {
            if (execution == ExecutionResult::Recovery)
            {
                result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
                result.blocker = "obsolete_committed_purchase_recovery";
                result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
                Reset(botAI);
                return result;
            }
            if (execution == ExecutionResult::Scheduled)
                result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
            else if (execution == ExecutionResult::Operation)
                result.outcome = PlayerbotEconomyCycleOutcome::Operation;
            else
            {
                result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
                result.blocker = "finished_good_failed_precondition";
            }
            result.schedulingEffect = execution == ExecutionResult::Failed ? EconomyAttemptOutcome::FailedPrecondition
                                                                           : EconomyAttemptOutcome::Operation;
            return result;
        }
        // The bot could not buy: the listing went to someone else, or the purchase itself did not
        // complete. Keep the cycle either way so it still gets its production and selling work.
        releasedConsumptionBlocker = execution == ExecutionResult::Superseded ? "finished_good_listing_superseded"
                                                                              : "finished_good_purchase_unavailable";
    }

    EconomyDecision const& decision = productionDecision;

    if (decision.phase == EconomyPhase::CollectAuctionMail || decision.phase == EconomyPhase::Craft ||
        decision.phase == EconomyPhase::BuyRecipe || decision.phase == EconomyPhase::SellSurplus)
    {
        if (activeGathering)
        {
            if (activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    activeGathering->coordinatorLeaseId,
                    activeGathering->committedQuantity ? EconomyAssignmentOutcome::InventoryReceived
                                                       : EconomyAssignmentOutcome::NeedChanged,
                    activeGathering->committedQuantity, now);
                activeGathering->coordinatorSettled = true;
            }
            if (activeGathering->plan.itemId && activeGathering->committedQuantity)
                pendingGatheredSupply[activeGathering->plan.itemId] += activeGathering->committedQuantity;
            Reset(botAI);
        }
        if (decision.phase == EconomyPhase::Craft)
        {
            if (std::optional<uint32> const tool = MissingVendorTool(bot, sSpellMgr->GetSpellInfo(decision.spellId)))
                return BuyProgressionVendorInput(botAI, *tool, decision.spellId);
        }
    }
    else if (decision.phase == EconomyPhase::BuyReagent && decision.vendorPurchase)
    {
        // Vendor inputs (vials, thread, flux) are bought at a vendor, not the auction house.
        sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, decision.spellId);
        return BuyProgressionVendorInput(botAI, decision.itemId, decision.spellId, decision.count);
    }
    else if (std::optional<PlayerbotEconomyCycleResult> const gathering =
                 ExecuteAutonomousGathering(botAI, careerPlan, snapshot, decision, marketId, now))
    {
        return *gathering;
    }

    PlayerbotEconomyCycleResult result;
    result.phase = decision.phase;
    result.workIdentity = {snapshot.preferredRecipeSpellId, decision.itemId, decision.auctionId,
                           decision.itemGuidCounter};

    if (decision.phase == EconomyPhase::None)
    {
        if (std::optional<PlayerbotEconomyCycleResult> const marketMaking =
                ExecuteMarketMaking(botAI, snapshot, auctioneer, marketId, now))
        {
            return *marketMaking;
        }

        if (snapshot.preferredRecipeSpellId && !activeProduction)
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);

        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        if (decision.blocker == EconomyDecisionBlocker::PriceCorridor)
            result.blocker = "price_corridor";
        else if (PlayerbotEconomyConsumption::IsStuckBlocker(consumptionDecision.blocker))
            result.blocker = PlayerbotEconomyConsumption::BlockerName(consumptionDecision.blocker);
        else if (stalledCareerStage)
        {
            // Market work found nothing either, so the career stall is the honest diagnosis to report.
            ReleaseIdleCycleState(botAI);
            return *stalledCareerStage;
        }
        else if (releasedConsumptionBlocker)
            result.blocker = releasedConsumptionBlocker;
        else
            result.blocker = "no_candidate";
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        ReleaseIdleCycleState(botAI);
        return result;
    }

    if (decision.phase == EconomyPhase::Craft || decision.phase == EconomyPhase::BuyReagent)
    {
        sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, decision.spellId);
        result.workIdentity.spellId = decision.spellId;
    }

    uint32 const craftStartingQuantity =
        decision.phase == EconomyPhase::Craft ? bot->GetItemCount(decision.itemId) : 0u;
    bool const productionCraft = decision.phase == EconomyPhase::Craft && activeProduction &&
                                 activeProduction->recipeSpellId == decision.spellId &&
                                 activeProduction->outputItemId == decision.itemId;
    std::string const craftChain = decision.phase != EconomyPhase::Craft ? std::string{}
                                   : productionCraft                     ? activeProduction->chainPublicId
                                                     : TraceChainForActor(bot->GetGUID().GetCounter(), now);
    ExecutionResult const execution = ExecuteDecision(botAI, decision, auctioneer);
    if (execution == ExecutionResult::Operation && decision.phase == EconomyPhase::Craft)
    {
        if (!craftChain.empty())
        {
            pendingCraftTrace = PendingCraftTrace{
                .chainPublicId = craftChain,
                .coordinatorLeaseId = productionCraft ? activeProduction->leaseId : 0u,
                .actorGuid = bot->GetGUID().GetCounter(),
                .itemId = decision.itemId,
                .recipeSpellId = decision.spellId,
                .startingQuantity = craftStartingQuantity,
                .startedAt = now,
            };
            ReconcileCraftTrace(bot, now);
        }
        else
        {
            sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        }
    }

    if (execution == ExecutionResult::Scheduled)
        result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    else if (execution == ExecutionResult::Operation)
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    else
    {
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = decision.phase == EconomyPhase::Craft && !lastCraftFailure.empty() ? lastCraftFailure
                                                                                            : "failed_precondition";
    }
    lastCraftFailure.clear();

    result.schedulingEffect = execution == ExecutionResult::Failed ? EconomyAttemptOutcome::FailedPrecondition
                                                                   : EconomyAttemptOutcome::Operation;
    return result;
}

EconomySnapshot DefaultPlayerbotEconomyRuntime::BuildSnapshot(PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    EconomySnapshot snapshot;
    snapshot.guidCounter = bot->GetGUID().GetCounter();
    snapshot.botAccountId = bot->GetSession()->GetAccountId();
    snapshot.freeMoneyForTradeskill =
        AI_VALUE2(uint32, "free money for", static_cast<uint32>(NeedMoneyFor::tradeskill));
    snapshot.preferredRecipeSpellId = sRandomPlayerbotMgr.GetValue(bot, PROFESSION_WORK_ORDER_EVENT);
    std::unordered_set<uint32> const applicableVendorItems = ApplicableUnlimitedGoldVendorItems(bot);
    snapshot.applicableUnlimitedGoldVendorItemIds.assign(applicableVendorItems.begin(), applicableVendorItems.end());
    std::sort(snapshot.applicableUnlimitedGoldVendorItemIds.begin(),
              snapshot.applicableUnlimitedGoldVendorItemIds.end());

    time_t const now = GameTime::GetGameTime().count();
    uint32 const marketId = AuctionMarketId(bot->GetFaction());
    EconomyCoordinatorSnapshot const coordinatorSnapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    auto const coordinatorDemandsOutput = [bot, marketId, &coordinatorSnapshot](uint32 itemId)
    {
        return std::any_of(coordinatorSnapshot.gaps.begin(), coordinatorSnapshot.gaps.end(),
                           [bot, marketId, itemId](EconomyDemandGap const& gap)
                           {
                               return gap.marketId == marketId && gap.HasResidualDemand() &&
                                      ProductionOutputMatchesGroup(bot, itemId, gap.group);
                           });
    };
    snapshot.controlledItemGuids = GetPlayerbotEconomyMarket().ControlledItemGuids(snapshot.guidCounter, marketId);
    std::unordered_set<uint64> const controlledItemGuids(snapshot.controlledItemGuids.begin(),
                                                         snapshot.controlledItemGuids.end());
    std::map<uint32, uint32> controlledInventory;
    for (uint64 itemGuid : snapshot.controlledItemGuids)
    {
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuid));
        if (item)
            controlledInventory[item->GetEntry()] += item->GetCount();
    }
    auto const availableInventory = [bot, &controlledInventory](uint32 itemId)
    {
        uint32 const total = bot->GetItemCount(itemId);
        uint32 const controlled = controlledInventory[itemId];
        return total > controlled ? total - controlled : 0u;
    };
    AuctionHouseEntry const* auctionHouseEntry =
        marketId ? AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId(marketId)) : nullptr;
    std::map<uint32, uint32> mailedInputs;
    for (Mail const* mail : bot->GetMails())
    {
        if (!mail || mail->messageType != MAIL_AUCTION || mail->state == MAIL_STATE_DELETED)
            continue;

        snapshot.auctionMail.push_back(
            {mail->messageID, mail->deliver_time <= now, mail->money, static_cast<uint32>(mail->items.size())});
        for (MailItemInfo const& mailedItem : mail->items)
        {
            Item const* item = bot->GetMItem(mailedItem.item_guid);
            if (item && !controlledItemGuids.contains(mailedItem.item_guid))
                mailedInputs[mailedItem.item_template] += item->GetCount();
        }
    }

    std::map<uint32, uint32> inventory;
    std::unordered_set<uint32> craftedOutputs;
    std::unordered_set<uint32> trainingOutputs;
    std::erase_if(progressionTrainingOutputs,
                  [bot, this](uint32 itemId)
                  {
                      bool const pendingOutput = pendingProgressionCraft && activeProgressionMilestone &&
                                                 activeProgressionMilestone->outputItemId == itemId;
                      return !pendingOutput && !bot->GetItemCount(itemId);
                  });
    trainingOutputs.insert(progressionTrainingOutputs.begin(), progressionTrainingOutputs.end());
    auto const hasCareerSkill = [bot, &careerPlan](uint16 skillId)
    {
        return ((IsPrimaryProfessionSkill(skillId) || IsUniversalProgressionSkill(skillId)) &&
                bot->HasSkill(skillId)) ||
               PlayerbotCareer::PlansSkill(careerPlan, skillId);
    };
    ListSpellsAction listSpells(botAI);
    for (auto const& [spellId, spellName] : listSpells.GetSpellList())
    {
        (void)spellName;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !bot->HasSpell(spellId) ||
            !PlayerbotEconomyPolicy::IsProfessionRecipeSpell(spellInfo->Effects[EFFECT_0].Effect,
                                                             spellInfo->Effects[EFFECT_0].ItemType,
                                                             spellInfo->ReagentCount[EFFECT_0], spellInfo->SchoolMask))
        {
            continue;
        }

        RecipeCandidate recipe;
        SkillLineAbilityMapBounds const skillBounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        bool careerRecipe = false;
        for (auto ability = skillBounds.first; ability != skillBounds.second; ++ability)
        {
            SkillLineAbilityEntry const* skill = ability->second;
            if (!skill || !hasCareerSkill(static_cast<uint16>(skill->SkillLine)))
                continue;
            careerRecipe = true;
            if (!recipe.professionSkillId)
                recipe.professionSkillId = static_cast<uint16>(skill->SkillLine);
        }
        if (!careerRecipe)
            continue;

        recipe.spellId = spellId;
        recipe.craftedItemId = spellInfo->Effects[EFFECT_0].ItemType;
        craftedOutputs.insert(recipe.craftedItemId);
        recipe.givesSkillUp = ItemUsageValue::SpellGivesSkillUp(spellId, bot);
        if (recipe.givesSkillUp)
            trainingOutputs.insert(recipe.craftedItemId);
        ItemUsage const outputUsage = AI_VALUE2(ItemUsage, "item usage", recipe.craftedItemId);
        recipe.outputUsagePriority = CraftOutputPriority(outputUsage);

        bool hasAllReagents = true;
        for (uint8 index = 0; index < MAX_SPELL_REAGENTS; ++index)
        {
            if (spellInfo->Reagent[index] <= 0 || spellInfo->ReagentCount[index] <= 0)
                continue;

            uint32 const itemId = static_cast<uint32>(spellInfo->Reagent[index]);
            uint32 const count = static_cast<uint32>(spellInfo->ReagentCount[index]);
            recipe.reagents.push_back({itemId, count, applicableVendorItems.contains(itemId)});
            uint32 const inventoryCount = availableInventory(itemId);
            inventory[itemId] = inventoryCount;
            hasAllReagents = hasAllReagents && inventoryCount >= count;
        }

        if (recipe.reagents.empty() || (!recipe.givesSkillUp && !IsUsefulCraftOutput(outputUsage) &&
                                        !coordinatorDemandsOutput(recipe.craftedItemId)))
            continue;

        // A recipe waiting only on a forge, an anvil or a tool stays: the craft step walks to the
        // focus object or buys the tool instead of forgetting the recipe exists.
        if (hasAllReagents && !CraftNeedsFocusOrTool(spellInfo) && !IsEnchantRecipeSpell(spellInfo) &&
            !botAI->CanCastSpell(spellId, bot, true))
            continue;

        snapshot.recipes.push_back(std::move(recipe));
    }

    for (auto const& [itemId, count] : inventory)
        snapshot.inventory.push_back({itemId, count, mailedInputs[itemId]});

    bool const capabilityMarketEligible =
        careerPlan.capabilityGoal.has_value() || !LearnedPrimaryCapabilitySkillIds(bot).empty();
    AuctionHouseObject* auctionHouse = careerPlan.marketEligible || capabilityMarketEligible
                                           ? sAuctionMgr->GetAuctionsMap(bot->GetFaction())
                                           : nullptr;
    if (auctionHouse)
    {
        for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
        {
            if (!auction || !sAuctionMgr->GetAItem(auction->item_guid))
                continue;

            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(auction->item_template);
            if (!itemTemplate)
                continue;

            PlayerbotRecipeCandidate const recipe = PlayerbotCareer::DescribeRecipe(itemTemplate, bot, 0u);

            AuctionListingCandidate listing{
                auctionId,
                sCharacterCache->GetCharacterAccountIdByGuid(auction->owner),
                auction->item_template,
                auction->itemCount,
                auction->buyout,
                static_cast<uint32>(std::max(0, itemTemplate->BuyPrice)),
                itemTemplate->GetMaxStackSize() * 2u,
                PlayerbotCareer::IsRecipeAcquisitionAllowed(careerPlan, recipe, PlayerbotRecipeSource::AuctionHouse)};
            std::optional<EconomyReferencePrice> const reference =
                GetPlayerbotEconomyMarket().ReferencePrice(marketId, ReagentGroup(auction->item_template), now);
            listing.buyerCeilingPerItem = reference.has_value() ? reference->unitPrice : listing.templateBuyPrice;
            listing.recipeSpellId = recipe.recipeSpellId;
            listing.disenchantYieldItemIds = BotDisenchantYields(bot, itemTemplate);
            snapshot.auctions.push_back(std::move(listing));
        }
    }

    // A material the bot's skill flags (Rough Stone for a miner, dust for an enchanter) is a sale too
    // once the production reserve is satisfied; the policy draws that line from the reserve floor.
    std::vector<Item*> saleItems =
        AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(ITEM_USAGE_AH));
    std::vector<Item*> const skillItems =
        AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(ITEM_USAGE_SKILL));
    saleItems.insert(saleItems.end(), skillItems.begin(), skillItems.end());
    auto const isGatheringSkill = [](uint16 skillId)
    { return skillId == SKILL_HERBALISM || skillId == SKILL_MINING || skillId == SKILL_SKINNING; };
    std::vector<uint16> const learnedPrimarySkills = LearnedPrimaryCapabilitySkillIds(bot);
    bool const pureGatheringCareer =
        !learnedPrimarySkills.empty() &&
        std::all_of(learnedPrimarySkills.begin(), learnedPrimarySkills.end(), isGatheringSkill);
    for (Item* item : saleItems)
    {
        if (!item || controlledItemGuids.contains(item->GetGUID().GetCounter()))
            continue;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        bool const circulationMaterial =
            PlayerbotEconomyPolicy::IsCirculationMaterial(itemTemplate->Class, itemTemplate->SubClass);
        bool const gatheringMaterial =
            itemTemplate->Class == ITEM_CLASS_TRADE_GOODS &&
            ((itemTemplate->SubClass == ITEM_SUBCLASS_HERB && hasCareerSkill(SKILL_HERBALISM)) ||
             (itemTemplate->SubClass == ITEM_SUBCLASS_METAL_STONE && hasCareerSkill(SKILL_MINING)) ||
             (itemTemplate->SubClass == ITEM_SUBCLASS_LEATHER && hasCareerSkill(SKILL_SKINNING)));
        bool const professionReagent = inventory.find(item->GetEntry()) != inventory.end();
        bool const professionOutput = craftedOutputs.contains(item->GetEntry());
        bool const professionRelated = circulationMaterial || professionReagent || professionOutput;
        if (!professionRelated)
            continue;

        uint64 const marketBuyout = LowestCompetingBuyoutPerItem(auctionHouse, item->GetEntry(), snapshot.botAccountId);
        uint64 const perItemBuyout =
            marketBuyout ? marketBuyout : static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        if (perItemBuyout * item->GetCount() > MAX_MONEY_AMOUNT)
            continue;

        uint64 allocatedInputCost = 0u;
        for (RecipeCandidate const& recipe : snapshot.recipes)
        {
            if (recipe.craftedItemId != item->GetEntry())
                continue;
            for (ReagentRequirement const& reagent : recipe.reagents)
            {
                std::optional<EconomyReferencePrice> const inputReference =
                    GetPlayerbotEconomyMarket().ReferencePrice(marketId, ReagentGroup(reagent.itemId), now);
                ItemTemplate const* inputTemplate = sObjectMgr->GetItemTemplate(reagent.itemId);
                uint64 const unitCost =
                    inputReference.has_value()
                        ? inputReference->unitPrice
                        : (inputTemplate ? static_cast<uint32>(std::max(0, inputTemplate->BuyPrice)) : 0u);
                allocatedInputCost += unitCost * reagent.count;
            }
            break;
        }

        std::string const group =
            gatheringMaterial || professionReagent ? ReagentGroup(item->GetEntry()) : ItemGroup(item->GetEntry());
        std::optional<EconomyReferencePrice> const outputReference =
            GetPlayerbotEconomyMarket().ReferencePrice(marketId, group, now);

        SaleItemCandidate sale;
        sale.itemGuidCounter = item->GetGUID().GetCounter();
        sale.itemId = item->GetEntry();
        sale.count = item->GetCount();
        sale.usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
        sale.canBeTraded = item->CanBeTraded();
        sale.bound = item->IsSoulBound();
        sale.container = item->IsBag();
        sale.containerItemCount = item->IsNotEmptyBag() ? 1u : 0u;
        sale.conjured = itemTemplate->HasFlag(ITEM_FLAG_CONJURED);
        sale.duration = item->GetUInt32Value(ITEM_FIELD_DURATION);
        sale.alreadyAuctioned = sAuctionMgr->GetAItem(item->GetGUID()) != nullptr;
        sale.templateBuyPrice = static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        sale.templateSellPrice = itemTemplate->SellPrice;
        sale.lowestCompetingBuyoutPerItem = marketBuyout;
        sale.inventoryCount = bot->GetItemCount(item->GetEntry());
        uint64 const configuredReserve =
            static_cast<uint64>(itemTemplate->GetMaxStackSize()) * sPlayerbotEconomyConfig.professionReserveStacks;
        sale.professionReserveFloor =
            professionReagent
                ? PlayerbotEconomyPolicy::ProductionReserve(
                      snapshot, item->GetEntry(),
                      static_cast<uint32>(std::min<uint64>(configuredReserve, std::numeric_limits<uint32>::max())))
                : 0u;
        sale.professionRelated = professionRelated;
        sale.allocatedInputCost = allocatedInputCost;
        sale.deposit = auctionHouseEntry ? AuctionHouseMgr::GetAuctionDeposit(auctionHouseEntry, MIN_AUCTION_TIME, item,
                                                                              item->GetCount())
                                         : 0u;
        sale.auctionCutBasisPoints =
            auctionHouseEntry
                ? static_cast<uint32>(auctionHouseEntry->cutPercent * sWorld->getRate(RATE_AUCTION_CUT) * 100.0f)
                : 0u;
        sale.buyerCeilingPerItem = outputReference.has_value() ? outputReference->unitPrice
                                                               : (marketBuyout ? marketBuyout : sale.templateBuyPrice);
        sale.pureGatheringMaterial = gatheringMaterial && pureGatheringCareer;
        sale.ordinaryVendorSupply = applicableVendorItems.contains(item->GetEntry());
        sale.trainingOutput = trainingOutputs.contains(item->GetEntry());
        sale.independentDemand = coordinatorDemandsOutput(item->GetEntry());
        snapshot.saleItems.push_back(std::move(sale));
    }

    return snapshot;
}

ConsumptionSnapshot DefaultPlayerbotEconomyRuntime::BuildConsumptionSnapshot(PlayerbotAI* botAI,
                                                                             EconomySnapshot const& economy,
                                                                             uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    ConsumptionSnapshot snapshot;
    snapshot.botAccountId = bot->GetSession()->GetAccountId();
    std::unordered_set<uint64> const controlledItemGuids(economy.controlledItemGuids.begin(),
                                                         economy.controlledItemGuids.end());

    std::map<EconomySubstitutionGroup, ConsumptionNeed> needs;
    std::map<EconomySubstitutionGroup, uint32> inventorySupply;
    std::map<EconomySubstitutionGroup, uint32> mailSupply;

    auto const budgetFor = [context](EconomySubstitutionKind kind)
    {
        NeedMoneyFor lane = NeedMoneyFor::gear;
        if (kind == EconomySubstitutionKind::Ammunition)
            lane = NeedMoneyFor::ammo;
        else if (kind == EconomySubstitutionKind::Consumable || kind == EconomySubstitutionKind::Enhancement)
        {
            lane = NeedMoneyFor::consumables;
        }
        return static_cast<uint64>(AI_VALUE2(uint32, "free money for", static_cast<uint32>(lane)));
    };

    auto const restorationUtility = [](uint32 maximum, uint32 triggerPercent)
    {
        uint32 const boundedPercent = std::min(triggerPercent, 100u);
        return static_cast<uint32>(static_cast<uint64>(maximum) * (100u - boundedPercent) / 100u);
    };
    auto const addConsumableNeed = [&](ConsumableCapability capability, uint32 requiredUtility)
    {
        ConsumptionNeed need = PlayerbotEconomyConsumption::BuildNeed(
            {capability, requiredUtility, 1u, true, budgetFor(EconomySubstitutionKind::Consumable)});
        needs.emplace(need.group, std::move(need));
    };
    if (botAI->HasStrategy("food", BOT_STATE_NON_COMBAT))
    {
        addConsumableNeed(ConsumableCapability::Food,
                          restorationUtility(bot->GetMaxHealth(), sPlayerbotAIConfig.lowHealth));
        if (uint32 const maximumMana = bot->GetMaxPower(POWER_MANA))
        {
            addConsumableNeed(ConsumableCapability::Drink, restorationUtility(maximumMana, sPlayerbotAIConfig.lowMana));
        }
    }
    if (botAI->HasStrategy("potions", BOT_STATE_COMBAT))
    {
        addConsumableNeed(ConsumableCapability::HealthRestoration,
                          restorationUtility(bot->GetMaxHealth(), sPlayerbotAIConfig.criticalHealth));
        if (uint32 const maximumMana = bot->GetMaxPower(POWER_MANA))
        {
            addConsumableNeed(ConsumableCapability::ManaRestoration,
                              restorationUtility(maximumMana, sPlayerbotAIConfig.mediumMana));
        }
    }

    auto const ensureNeed = [&](FinishedGoodDescription const& description,
                                ItemTemplate const* itemTemplate) -> ConsumptionNeed&
    {
        ConsumptionNeed& need = needs[description.group];
        if (!need.quantity)
        {
            need.group = description.group;
            need.use = description.use;
            need.quantity = description.group.kind == EconomySubstitutionKind::Ammunition ||
                                    description.group.kind == EconomySubstitutionKind::Consumable
                                ? std::max<uint32>(1u, itemTemplate->GetMaxStackSize())
                                : 1u;
            need.requiredUtility = description.utility;
            need.compatibleActivity = true;
            need.remainingUses = need.quantity;
            need.protectedBudget = budgetFor(description.group.kind);
        }
        else
            need.requiredUtility = std::min(need.requiredUtility, description.utility);

        std::optional<EconomyReferencePrice> const reference = GetPlayerbotEconomyMarket().ReferencePrice(
            marketId, PlayerbotEconomyConsumption::GroupKey(description.group), now);
        uint64 const templateCeiling = static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        need.buyerCeilingPerItem =
            std::max(need.buyerCeilingPerItem, reference.has_value() ? reference->unitPrice : templateCeiling);
        return need;
    };

    auto const findNeed = [&needs](FinishedGoodDescription const& description) -> ConsumptionNeed*
    {
        auto const found = std::find_if(
            needs.begin(), needs.end(), [&description](auto const& entry)
            { return PlayerbotEconomyConsumption::MatchesNeed(entry.second, description.group, description.utility); });
        return found == needs.end() ? nullptr : &found->second;
    };
    auto const updatePrice = [marketId, now](ConsumptionNeed& need, ItemTemplate const* itemTemplate)
    {
        std::optional<EconomyReferencePrice> const reference = GetPlayerbotEconomyMarket().ReferencePrice(
            marketId, PlayerbotEconomyConsumption::GroupKey(need.group), now);
        uint64 const templateCeiling = static_cast<uint32>(std::max(0, itemTemplate->BuyPrice));
        need.buyerCeilingPerItem =
            std::max(need.buyerCeilingPerItem, reference.has_value() ? reference->unitPrice : templateCeiling);
    };

    std::unordered_set<uint64> visitedItems;
    auto const inspectInventoryItem = [&](Item* item)
    {
        if (!item || controlledItemGuids.contains(item->GetGUID().GetCounter()) ||
            !visitedItems.insert(item->GetGUID().GetCounter()).second)
            return;

        std::optional<FinishedGoodDescription> const description =
            PlayerbotEconomyConsumption::Describe(bot, item->GetTemplate());
        if (!description.has_value())
            return;

        snapshot.held.push_back({description->group, item->GetEntry(), item->GetCount(), EconomySupplySource::Inventory,
                                 description->utility});
        std::string const qualifier =
            std::to_string(item->GetEntry()) + "," + std::to_string(item->GetItemRandomPropertyId());
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", qualifier);
        bool const compatible = IsFinishedGoodUsage(usage) && bot->CanUseItem(item) == EQUIP_ERR_OK;
        ConsumptionNeed* need = nullptr;
        if (description->group.kind == EconomySubstitutionKind::Consumable)
            need = findNeed(*description);
        else if (compatible)
            need = &ensureNeed(*description, item->GetTemplate());
        if (!compatible || !need)
            return;

        inventorySupply[need->group] += item->GetCount();
        snapshot.owned.push_back({description->group, item->GetGUID().GetCounter(), item->GetEntry(), item->GetCount(),
                                  description->utility, true});
    };

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        inspectInventoryItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = static_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            inspectInventoryItem(bag->GetItemByPos(slot));
    }

    for (Mail const* mail : bot->GetMails())
    {
        if (!mail || mail->messageType != MAIL_AUCTION || mail->state == MAIL_STATE_DELETED)
            continue;
        for (MailItemInfo const& mailedItem : mail->items)
        {
            Item const* item = bot->GetMItem(mailedItem.item_guid);
            if (!item || controlledItemGuids.contains(mailedItem.item_guid))
                continue;
            std::optional<FinishedGoodDescription> const description =
                PlayerbotEconomyConsumption::Describe(bot, item->GetTemplate());
            if (description.has_value())
            {
                snapshot.held.push_back({description->group, item->GetEntry(), item->GetCount(),
                                         EconomySupplySource::Mail, description->utility});
                ConsumptionNeed* need = findNeed(*description);
                if (need && bot->CanUseItem(item->GetTemplate()) == EQUIP_ERR_OK)
                    mailSupply[need->group] += item->GetCount();
            }
        }
    }

    for (AuctionListingCandidate const& listing : economy.auctions)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(listing.itemId);
        std::optional<FinishedGoodDescription> const description =
            PlayerbotEconomyConsumption::Describe(bot, itemTemplate);
        if (!description.has_value() || !itemTemplate)
            continue;

        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", listing.itemId);
        bool const equipment = itemTemplate->Class == ITEM_CLASS_ARMOR || itemTemplate->Class == ITEM_CLASS_WEAPON;
        if ((equipment &&
             !PlayerbotEconomyConsumption::IsMarketEquipment(itemTemplate->Class, itemTemplate->Quality, usage)) ||
            (!equipment && !IsFinishedGoodUsage(usage)))
            continue;

        ConsumptionNeed* need = description->group.kind == EconomySubstitutionKind::Consumable
                                    ? findNeed(*description)
                                    : &ensureNeed(*description, itemTemplate);
        if (!need)
            continue;
        if (description->group.kind == EconomySubstitutionKind::Consumable)
            updatePrice(*need, itemTemplate);
        snapshot.offers.push_back({description->group, listing.auctionId, listing.ownerAccountId, listing.itemId,
                                   listing.count, listing.buyout, description->utility,
                                   bot->CanUseItem(itemTemplate) == EQUIP_ERR_OK});
    }

    for (auto const& [itemGuid, committed] : committedFinishedGoods)
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Item>(itemGuid);
        Item* item = bot->GetItemByGuid(guid);
        Item const* physicalItem = item ? item : bot->GetMItem(static_cast<ObjectGuid::LowType>(itemGuid));
        if (!physicalItem)
            continue;

        ConsumptionNeed& need = needs[committed.group];
        if (!need.quantity)
        {
            need.group = committed.group;
            need.use = committed.use;
            need.quantity = committed.quantity;
            need.compatibleActivity = true;
            need.remainingUses = committed.quantity;
            need.protectedBudget = budgetFor(committed.group.kind);
        }
        need.committedPurchaseQuantity += committed.quantity;
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", committed.itemId);
        need.committedPurchaseStillUseful = IsFinishedGoodUsage(usage);
    }

    for (auto& [group, need] : needs)
    {
        if (group.kind != EconomySubstitutionKind::Consumable)
            continue;
        need.ordinaryVendorSupply = std::any_of(
            economy.applicableUnlimitedGoldVendorItemIds.begin(), economy.applicableUnlimitedGoldVendorItemIds.end(),
            [bot, &need](uint32 itemId)
            {
                std::optional<FinishedGoodDescription> const description =
                    PlayerbotEconomyConsumption::Describe(bot, sObjectMgr->GetItemTemplate(itemId));
                return description &&
                       PlayerbotEconomyConsumption::MatchesNeed(need, description->group, description->utility);
            });
    }

    for (auto& [group, need] : needs)
    {
        need.inventoryQuantity = inventorySupply[group];
        need.mailQuantity = mailSupply[group];
        snapshot.needs.push_back(std::move(need));
    }
    snapshot.workTripInFlight = activeGathering.has_value() || (craftFocusTravel && OwnsTravelTarget(botAI));
    return snapshot;
}

Creature* DefaultPlayerbotEconomyRuntime::FindAuctioneer(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    GuidVector const npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        if (Creature* auctioneer = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))
            return auctioneer;
    }

    return nullptr;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::ExecuteDecision(PlayerbotAI* botAI, EconomyDecision const& decision,
                                                                Creature* auctioneer)
{
    switch (decision.phase)
    {
        case EconomyPhase::CollectAuctionMail:
            return CollectAuctionMail(botAI);
        case EconomyPhase::Craft:
        {
            Player* const bot = botAI->GetBot();
            // A mounted bot cannot craft and its cast check reports nothing else
            // (SPELL_FAILED_NOT_MOUNTED), so dismount before asking whether a forge is in range.
            if (bot->IsMounted())
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (!botAI->CanCastSpell(decision.spellId, bot, true) && IsCookingRecipeSpell(decision.spellId))
            {
                // Learning cooking teaches Basic Campfire, but a bot granted the skill programmatically
                // never received the ability, so it could never make the fire its recipes require.
                if (!bot->HasSpell(BASIC_CAMPFIRE_SPELL_ID))
                    bot->learnSpell(BASIC_CAMPFIRE_SPELL_ID, false);
                if (botAI->CanCastSpell(BASIC_CAMPFIRE_SPELL_ID, bot, true) &&
                    botAI->CastSpell(BASIC_CAMPFIRE_SPELL_ID, bot))
                {
                    // The fire is what the recipe was missing. Cook on it next cycle.
                    return ExecutionResult::Scheduled;
                }
            }
            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(decision.spellId);
                spellInfo && spellInfo->RequiresSpellFocus && !botAI->CanCastSpell(decision.spellId, bot, true) &&
                CraftCastResult(bot, spellInfo) == SPELL_FAILED_REQUIRES_SPELL_FOCUS)
            {
                // Smelting needs a forge and blacksmithing an anvil. Neither can be conjured, so walk
                // to the nearest one and craft there on a later cycle.
                // The core accepts a focus object within half its listed range, measured in three
                // dimensions. Stop close, but not on it: world campfires burn whoever stands inside them
                // (go_flames), the Ironforge forges are lava pools, and each burn interrupts the craft.
                PlayerbotEconomyTravelCatalog::SpellFocusDestination* const focus =
                    sPlayerbotEconomyTravelCatalog.SelectSpellFocus(bot, spellInfo->RequiresSpellFocus);
                if (!focus)
                {
                    lastCraftFailure = "craft_focus_unreachable";
                    return ExecutionResult::Failed;
                }
                std::optional<SpellFocusStand> const stand = SpellFocusStandPoint(bot, *focus);
                if (!stand)
                {
                    lastCraftFailure = "craft_focus_unsafe";
                    return ExecutionResult::Failed;
                }
                // Arrival is judged by distance to the focus object itself, so the travel radius has to
                // reach the stand point or the bot never settles into the working state.
                if (TravelToDestination(botAI, &focus->destination, stand->distance + 1.0f, stand->point))
                {
                    craftFocusTravel = true;
                    return ExecutionResult::Scheduled;
                }
                lastCraftFailure = "craft_focus_unreachable";
                return ExecutionResult::Failed;
            }
            if (bot->isMoving())
            {
                // A timed craft cannot start mid-step. The bot is where it needs to be (a focus object
                // was handled above), so plant it before asking the core whether the cast can go.
                bot->GetMotionMaster()->Clear(true);
                bot->StopMoving();
            }
            // An enchant goes onto a piece of the bot's own gear; the skill-up is the point, and the
            // gear is the bot's to enchant.
            Item* enchantTarget = nullptr;
            if (SpellInfo const* const spellInfo = sSpellMgr->GetSpellInfo(decision.spellId);
                IsEnchantRecipeSpell(spellInfo))
            {
                enchantTarget = SelectOwnGearEnchantTarget(bot, spellInfo);
                if (!enchantTarget)
                {
                    lastCraftFailure = "profession_enchant_target_missing";
                    return ExecutionResult::Failed;
                }
            }
            if (!botAI->CanCastSpell(decision.spellId, bot, true, enchantTarget))
            {
                // Name the core's verdict so telemetry shows why a craft did not start.
                SpellInfo const* const spellInfo = sSpellMgr->GetSpellInfo(decision.spellId);
                lastCraftFailure = Acore::StringFormat(
                    "craft_cast_check:{}:{}", spellInfo ? static_cast<uint32>(CraftCastResult(bot, spellInfo)) : 0u,
                    bot->isMoving() ? "moving" : "still");
                return ExecutionResult::Failed;
            }
            if (!botAI->CastSpell(decision.spellId, bot, enchantTarget))
            {
                lastCraftFailure = "craft_cast_rejected";
                return ExecutionResult::Failed;
            }
            Reset(botAI);
            return ExecutionResult::Operation;
        }
        case EconomyPhase::BuyReagent:
        case EconomyPhase::BuyRecipe:
            return BuyReagent(botAI, decision, auctioneer);
        case EconomyPhase::BuyFinishedGood:
        case EconomyPhase::UseFinishedGood:
        case EconomyPhase::RecoverFinishedGood:
            return ExecutionResult::Failed;
        case EconomyPhase::SellSurplus:
            return SellSurplus(botAI, decision, auctioneer);
        case EconomyPhase::None:
            return ExecutionResult::Failed;
    }

    return ExecutionResult::Failed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::ExecuteConsumption(PlayerbotAI* botAI,
                                                                   ConsumptionDecision const& decision,
                                                                   Creature* auctioneer)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    if (decision.action == ConsumptionAction::Recovery)
    {
        auto const committed =
            std::find_if(committedFinishedGoods.begin(), committedFinishedGoods.end(),
                         [&decision](auto const& entry) { return entry.second.group == decision.group; });
        if (committed != committedFinishedGoods.end())
            committedFinishedGoods.erase(committed);
        return ExecutionResult::Recovery;
    }

    if (decision.action == ConsumptionAction::Purchase)
    {
        if (!auctioneer)
            return TravelToAuctionHouse(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

        AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
        AuctionEntry* auction = auctionHouse ? auctionHouse->GetAuction(decision.auctionId) : nullptr;
        if (!auction || auction->item_template != decision.itemId || auction->itemCount != decision.count ||
            auction->buyout != decision.buyout)
        {
            return ExecutionResult::Superseded;
        }

        uint64 const itemGuid = auction->item_guid.GetCounter();
        EconomyDecision transaction;
        transaction.phase = EconomyPhase::BuyFinishedGood;
        transaction.itemId = decision.itemId;
        transaction.auctionId = decision.auctionId;
        transaction.count = decision.count;
        transaction.buyout = decision.buyout;
        transaction.purchases.push_back({decision.auctionId, decision.itemId, decision.count, decision.buyout});
        ExecutionResult const result =
            BuyReagent(botAI, transaction, auctioneer, EconomyClaimPriority::Consumer, decision.group);
        if (result == ExecutionResult::Operation)
        {
            std::optional<EconomyTraceEvent> const purchased =
                TraceEventForAuction(bot->GetGUID().GetCounter(), decision.auctionId, EconomyTraceKind::Purchased);
            committedFinishedGoods[itemGuid] = {
                decision.group,
                decision.use,
                decision.itemId,
                decision.count,
                purchased ? purchased->chainPublicId
                          : TraceChainForActor(bot->GetGUID().GetCounter(), GameTime::GetGameTime().count()),
                purchased ? purchased->counterpartyGuid : 0u,
            };
        }
        return result;
    }

    if (decision.action != ConsumptionAction::FinalUse || !decision.itemGuidCounter)
        return ExecutionResult::Failed;

    ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(decision.itemGuidCounter);
    Item* item = bot->GetItemByGuid(itemGuid);
    if (!item || item->GetEntry() != decision.itemId || item->GetCount() < decision.count ||
        bot->CanUseItem(item) != EQUIP_ERR_OK)
    {
        return ExecutionResult::Failed;
    }

    std::optional<FinishedGoodDescription> const description =
        PlayerbotEconomyConsumption::Describe(bot, item->GetTemplate());
    std::string const qualifier =
        std::to_string(item->GetEntry()) + "," + std::to_string(item->GetItemRandomPropertyId());
    if (!description.has_value() || description->group != decision.group || description->use != decision.use ||
        !IsFinishedGoodUsage(AI_VALUE2(ItemUsage, "item usage", qualifier)))
    {
        return ExecutionResult::Failed;
    }

    bool used = false;
    if (decision.use == FinishedGoodUse::Equip || decision.use == FinishedGoodUse::SetAmmunition)
    {
        EquipAction(botAI, "economy equip").EquipItems({decision.itemId});
        if (decision.use == FinishedGoodUse::SetAmmunition)
            used = bot->GetUInt32Value(PLAYER_AMMO_ID) == decision.itemId;
        else
        {
            Item* equipped = bot->GetItemByGuid(itemGuid);
            used = equipped && equipped->GetBagSlot() == INVENTORY_SLOT_BAG_0 &&
                   equipped->GetSlot() < INVENTORY_SLOT_ITEM_START;
        }
    }
    else if (decision.use == FinishedGoodUse::Consume || decision.use == FinishedGoodUse::Apply)
        used = EconomyUseItemAction(botAI).Apply(item);

    if (!used)
        return ExecutionResult::Failed;

    auto const committed = committedFinishedGoods.find(decision.itemGuidCounter);
    std::string const chainPublicId =
        committed != committedFinishedGoods.end()
            ? committed->second.chainPublicId
            : TraceChainForActor(bot->GetGUID().GetCounter(), GameTime::GetGameTime().count());
    if (!chainPublicId.empty())
    {
        [[maybe_unused]] bool const recorded =
            PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                .Complete(true,
                          {
                              .deduplicationKey = Acore::StringFormat("final-use:{}:{}", decision.itemGuidCounter,
                                                                      static_cast<uint32>(decision.use)),
                              .chainPublicId = chainPublicId,
                              .actorGuid = bot->GetGUID().GetCounter(),
                              .counterpartyGuid =
                                  committed != committedFinishedGoods.end() ? committed->second.counterpartyGuid : 0u,
                              .itemId = decision.itemId,
                              .quantity = decision.count,
                              .occurredAt = GameTime::GetGameTime().count(),
                              .kind = EconomyTraceKind::FinalUse,
                              .finalUse = TraceFinalUse(decision.use),
                          });
    }
    committedFinishedGoods.erase(decision.itemGuidCounter);
    return ExecutionResult::Operation;
}

bool HasAuctionMailBagSpace(Player* bot)
{
    uint32 used = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++used;

    uint32 free = 16u - used;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag const* container = static_cast<Bag const*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag));
        if (!container)
            continue;
        ItemTemplate const* bagTemplate = container->GetTemplate();
        if (bagTemplate->Class == ITEM_CLASS_CONTAINER && bagTemplate->SubClass == ITEM_SUBCLASS_CONTAINER)
            free += container->GetFreeSlots();
    }
    return free >= 2u;
}

void DeleteCollectedMail(Player* bot, ObjectGuid mailbox, uint32 mailId)
{
    WorldPacket packet;
    packet << mailbox;
    packet << mailId;
    packet << uint32(0);
    bot->GetSession()->HandleMailDelete(packet);
}

bool CollectOneAuctionMail(Player* bot, ObjectGuid mailbox, uint32 mailId)
{
    Mail* mail = bot->GetMail(mailId);
    if (!mail || mail->messageType != MAIL_AUCTION)
        return false;

    bool processed = false;
    if (mail->money)
    {
        WorldPacket packet;
        packet << mailbox;
        packet << mailId;
        bot->GetSession()->HandleMailTakeMoney(packet);
        mail = bot->GetMail(mailId);
        processed = mail && mail->money == 0;
    }

    mail = bot->GetMail(mailId);
    if (mail && !mail->items.empty())
    {
        if (!HasAuctionMailBagSpace(bot))
            return processed;

        std::vector<uint32> itemGuids;
        for (MailItemInfo const& item : mail->items)
            if (sObjectMgr->GetItemTemplate(item.item_template))
                itemGuids.push_back(item.item_guid);

        for (uint32 itemGuid : itemGuids)
        {
            mail = bot->GetMail(mailId);
            if (!mail)
                break;
            auto attachment = std::find_if(mail->items.begin(), mail->items.end(),
                                           [itemGuid](MailItemInfo const& item) { return item.item_guid == itemGuid; });
            if (attachment == mail->items.end() || !bot->GetMItem(itemGuid))
                continue;

            WorldPacket packet;
            packet << mailbox;
            packet << mailId;
            packet << itemGuid;
            bot->GetSession()->HandleMailTakeItem(packet);

            mail = bot->GetMail(mailId);
            processed |=
                mail && std::none_of(mail->items.begin(), mail->items.end(),
                                     [itemGuid](MailItemInfo const& item) { return item.item_guid == itemGuid; });
        }
    }

    mail = bot->GetMail(mailId);
    if (mail && PlayerbotEconomyMailIsFullyCollected(mail->money, mail->items.size()))
        DeleteCollectedMail(bot, mailbox, mailId);
    return processed;
}

// MailProcessor::FindMailbox returns the first mailbox in the unsorted "nearest game objects" value, which in a
// capital is often one 50+ yards away, so a bot standing on a mailbox would keep failing the interaction check and
// re-travel to the spot it already occupies. Pick the first mailbox the bot can actually interact with instead.
ObjectGuid FindInteractableMailbox(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    GuidVector const gameObjects = *botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects");
    for (ObjectGuid const guid : gameObjects)
        if (bot->GetGameObjectIfCanInteractWith(guid, GAMEOBJECT_TYPE_MAILBOX))
            return guid;
    return ObjectGuid::Empty;
}

bool CollectAvailableAuctionMail(PlayerbotAI* botAI, ObjectGuid mailbox)
{
    Player* bot = botAI->GetBot();
    WorldPacket packet;
    packet << mailbox;
    bot->GetSession()->HandleGetMailList(packet);

    time_t const now = GameTime::GetGameTime().count();
    std::vector<uint32> mailIds;
    for (Mail const* mail : bot->GetMails())
        if (mail && mail->messageType == MAIL_AUCTION && mail->state != MAIL_STATE_DELETED && mail->deliver_time <= now)
            mailIds.push_back(mail->messageID);

    bool processed = false;
    for (uint32 mailId : mailIds)
        processed = CollectOneAuctionMail(bot, mailbox, mailId) || processed;
    return processed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::CollectAuctionMail(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    ObjectGuid const mailbox = FindInteractableMailbox(botAI);
    if (!mailbox)
        return TravelToMailbox(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    struct TraceableMail
    {
        uint32 mailId = 0;
        uint64 money = 0;
        AuctionMailDetails details;
    };
    uint64 const now = GameTime::GetGameTime().count();
    std::vector<TraceableMail> traceable;
    for (Mail const* mail : bot->GetMails())
    {
        std::optional<AuctionMailDetails> const details = ParseAuctionMail(mail);
        if (!mail || !details || mail->state == MAIL_STATE_DELETED || mail->deliver_time > static_cast<time_t>(now))
        {
            continue;
        }
        if (details->response == AUCTION_WON || details->response == AUCTION_SUCCESSFUL ||
            details->response == AUCTION_EXPIRED)
            traceable.push_back({mail->messageID, mail->money, *details});
    }
    std::vector<EconomyTraceRecord> traceRecords;
    traceRecords.reserve(traceable.size());
    for (TraceableMail const& mail : traceable)
    {
        EconomyTraceKind const priorKind =
            mail.details.response == AUCTION_WON ? EconomyTraceKind::Purchased : EconomyTraceKind::Listed;
        std::optional<EconomyTraceEvent> const prior =
            TraceEventForAuction(bot->GetGUID().GetCounter(), mail.details.auctionId, priorKind);
        if (!prior)
            continue;
        EconomyTraceRecord record;
        std::string_view const outcome = mail.details.response == AUCTION_WON          ? "delivered"
                                         : mail.details.response == AUCTION_SUCCESSFUL ? "settled"
                                                                                       : "expired";
        record.deduplicationKey = Acore::StringFormat("mail:{}:{}", mail.mailId, outcome);
        record.chainPublicId = prior->chainPublicId;
        record.actorGuid = bot->GetGUID().GetCounter();
        record.counterpartyGuid = mail.details.response == AUCTION_SUCCESSFUL
                                      ? TraceActorIfKnown(mail.details.bidderGuid, now)
                                      : prior->counterpartyGuid;
        record.itemId = mail.details.itemId;
        record.quantity = mail.details.quantity;
        record.unitPriceCopper = mail.details.response != AUCTION_EXPIRED && mail.details.quantity
                                     ? (mail.details.bid + mail.details.quantity - 1u) / mail.details.quantity
                                     : 0u;
        record.depositCopper = mail.details.deposit;
        record.auctionCutCopper = mail.details.cut;
        record.proceedsCopper = mail.money;
        record.referenceUnitPriceCopper = prior->referenceUnitPriceCopper;
        record.competingUnitPriceCopper = prior->competingUnitPriceCopper;
        record.occurredAt = now;
        record.correlationAuctionId = mail.details.auctionId;
        record.correlationMailId = mail.mailId;
        record.kind = mail.details.response == AUCTION_WON          ? EconomyTraceKind::Delivered
                      : mail.details.response == AUCTION_SUCCESSFUL ? EconomyTraceKind::SaleSettled
                                                                    : EconomyTraceKind::Expired;
        traceRecords.push_back(std::move(record));
    }
    bool const collected = CollectAvailableAuctionMail(botAI, mailbox);
    [[maybe_unused]] std::size_t const recorded =
        PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace()).CompleteMailScan(collected, std::move(traceRecords));
    if (collected)
    {
        Reset(botAI);
    }
    return collected ? ExecutionResult::Operation : ExecutionResult::Failed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::BuyReagent(PlayerbotAI* botAI, EconomyDecision const& decision,
                                                           Creature* auctioneer, EconomyClaimPriority priority,
                                                           std::optional<EconomySubstitutionGroup> claimGroup)
{
    Player* const bot = botAI->GetBot();
    if (!auctioneer)
        return TravelToAuctionHouse(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse)
        return ExecutionResult::Failed;

    std::vector<EconomyDecision::AuctionPurchase> purchases = decision.purchases;
    if (purchases.empty())
        purchases.push_back({decision.auctionId, decision.itemId, decision.count, decision.buyout});

    bool boughtAny = false;
    for (EconomyDecision::AuctionPurchase const& purchase : purchases)
    {
        AuctionEntry* auction = auctionHouse->GetAuction(purchase.auctionId);
        if (!auction || auction->item_template != purchase.itemId || auction->itemCount != purchase.count ||
            auction->buyout != purchase.buyout || auction->buyout == 0 ||
            sCharacterCache->GetCharacterAccountIdByGuid(auction->owner) == bot->GetSession()->GetAccountId())
        {
            return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
        }
        uint32 const sellerGuid = auction->owner.GetCounter();
        uint64 const itemGuidCounter = auction->item_guid.GetCounter();

        std::optional<EconomyAssignment> assignment;
        if (decision.disenchantSourcePurchase)
        {
            // The coordinator keys purchase leases on a demand gap for the exact item, and it has one
            // for the dust, not for the green that breaks into it. Keep the affinity gate the lease would
            // have applied and buy without a claim, as a progression vendor input is bought.
            EconomyWorkPolicyInput policy;
            policy.kind = EconomyWorkKind::Buy;
            policy.economyAffinity = EconomyAffinity(bot->GetGUID().GetCounter());
            policy.sameAccountPurchase = false;
            if (PlayerbotEconomyPolicy::EvaluateWork(policy) != EconomyWorkBlocker::None)
                return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
        }
        else if (decision.phase == EconomyPhase::BuyReagent || decision.phase == EconomyPhase::BuyFinishedGood)
        {
            EconomyAssignmentRequest request;
            request.characterGuid = bot->GetGUID().GetCounter();
            request.marketId = AuctionMarketId(bot->GetFaction());
            request.group = claimGroup.value_or(EconomySubstitutionGroup::ExactReagent(purchase.itemId));
            request.quantity = purchase.count;
            request.kind = EconomyClaimKind::Purchase;
            request.priority = priority;
            request.workKind = EconomyWorkKind::Buy;
            request.workIdentity = "auction:" + std::to_string(purchase.auctionId);
            request.recipeSpellId = decision.phase == EconomyPhase::BuyReagent ? decision.spellId : 0u;
            request.sellerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
            request.expiresAt = GameTime::GetGameTime().count() + 1u;
            EconomyAssignmentLease const lease =
                GetPlayerbotEconomyCoordinator().Lease(std::move(request), GameTime::GetGameTime().count());
            if (!lease.assignment)
                return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
            assignment = lease.assignment;
        }

        WorldPacket packet;
        packet << auctioneer->GetGUID();
        packet << purchase.auctionId;
        packet << static_cast<uint32>(purchase.buyout);
        bot->GetSession()->HandleAuctionPlaceBid(packet);

        if (auctionHouse->GetAuction(purchase.auctionId))
        {
            if (assignment)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    assignment->leaseId, EconomyAssignmentOutcome::FailedPurchase, 0u, GameTime::GetGameTime().count());
            }
            return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
        }
        if (assignment)
        {
            [[maybe_unused]] bool const completed =
                GetPlayerbotEconomyCoordinator().RecordOutcome(assignment->leaseId, EconomyAssignmentOutcome::Completed,
                                                               purchase.count, GameTime::GetGameTime().count());
        }
        uint64 const now = GameTime::GetGameTime().count();
        std::string const chainPublicId =
            assignment ? assignment->chainPublicId : TraceChainForActor(bot->GetGUID().GetCounter(), now);
        if (decision.phase == EconomyPhase::BuyRecipe && decision.recipeSpellId)
        {
            committedRecipes[itemGuidCounter] = {
                purchase.itemId,
                decision.recipeSpellId,
                chainPublicId,
                TraceActorIfKnown(sellerGuid, now),
            };
        }
        if (!chainPublicId.empty())
        {
            std::optional<EconomyReferencePrice> const reference = GetPlayerbotEconomyMarket().ReferencePrice(
                AuctionMarketId(bot->GetFaction()), ReagentGroup(purchase.itemId), now);
            [[maybe_unused]] bool const recorded =
                PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                    .Complete(true,
                              {
                                  .deduplicationKey = Acore::StringFormat(
                                      "purchase:{}:{}", AuctionMarketId(bot->GetFaction()), purchase.auctionId),
                                  .chainPublicId = chainPublicId,
                                  .actorGuid = bot->GetGUID().GetCounter(),
                                  .counterpartyGuid = TraceActorIfKnown(sellerGuid, now),
                                  .itemId = purchase.itemId,
                                  .quantity = purchase.count,
                                  .unitPriceCopper = (purchase.buyout + purchase.count - 1u) / purchase.count,
                                  .referenceUnitPriceCopper = reference ? reference->unitPrice : 0u,
                                  .competingUnitPriceCopper = (purchase.buyout + purchase.count - 1u) / purchase.count,
                                  .occurredAt = now,
                                  .correlationAuctionId = purchase.auctionId,
                                  .kind = EconomyTraceKind::Purchased,
                              });
        }
        boughtAny = true;
    }

    if (boughtAny)
        Reset(botAI);
    return boughtAny ? ExecutionResult::Operation : ExecutionResult::Failed;
}

ExecutionResult DefaultPlayerbotEconomyRuntime::SellSurplus(PlayerbotAI* botAI, EconomyDecision const& decision,
                                                            Creature* auctioneer)
{
    Player* const bot = botAI->GetBot();
    if (!auctioneer)
        return TravelToAuctionHouse(botAI) ? ExecutionResult::Scheduled : ExecutionResult::Failed;

    ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(decision.itemGuidCounter);
    Item* item = bot->GetItemByGuid(itemGuid);
    EconomyDecision sale = decision;
    auto const pending = pendingGatheredSupply.find(decision.itemId);
    if (pending != pendingGatheredSupply.end())
    {
        uint32 remainingDeficit = 0u;
        EconomyCoordinatorSnapshot const coordinator =
            GetPlayerbotEconomyCoordinator().Snapshot(GameTime::GetGameTime().count());
        auto const gap =
            std::find_if(coordinator.gaps.begin(), coordinator.gaps.end(),
                         [&decision, bot](EconomyDemandGap const& candidate)
                         {
                             return candidate.marketId == AuctionMarketId(bot->GetFaction()) &&
                                    candidate.group == EconomySubstitutionGroup::ExactReagent(decision.itemId);
                         });
        if (gap != coordinator.gaps.end())
            remainingDeficit = gap->remainingQuantity;

        AutonomousSupplierListing const bounded = PlayerbotEconomyGathering::BoundSupplierListing(
            sale.count, pending->second, remainingDeficit, sale.startBid, sale.buyout);
        sale.count = bounded.count;
        sale.startBid = bounded.startBid;
        sale.buyout = bounded.buyout;
        if (!sale.count)
        {
            pendingGatheredSupply.erase(pending);
            return ExecutionResult::Failed;
        }
    }
    if (!sale.count || !IsSafeSaleItem(botAI, item, sale))
        return ExecutionResult::Failed;

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse)
        return ExecutionResult::Failed;
    std::unordered_set<uint32> existingAuctions;
    existingAuctions.reserve(auctionHouse->GetAuctions().size());
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        (void)auction;
        existingAuctions.insert(auctionId);
    }

    WorldPacket packet;
    packet << auctioneer->GetGUID();
    packet << uint32(1);
    packet << itemGuid;
    packet << sale.count;
    packet << static_cast<uint32>(sale.startBid);
    packet << static_cast<uint32>(sale.buyout);
    packet << static_cast<uint32>(MIN_AUCTION_TIME / MINUTE);
    bot->GetSession()->HandleAuctionSellItem(packet);

    AuctionEntry const* listed = nullptr;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        if (!existingAuctions.contains(auctionId) && auction && auction->owner == bot->GetGUID() &&
            auction->item_template == sale.itemId && auction->itemCount == sale.count &&
            auction->startbid == sale.startBid && auction->buyout == sale.buyout)
        {
            listed = auction;
            break;
        }
    }
    if (listed && pending != pendingGatheredSupply.end())
    {
        pending->second -= sale.count;
        if (!pending->second)
            pendingGatheredSupply.erase(pending);
    }
    if (listed)
    {
        uint64 const now = GameTime::GetGameTime().count();
        std::string const chainPublicId = TraceChainForActor(bot->GetGUID().GetCounter(), now);
        if (!chainPublicId.empty())
        {
            [[maybe_unused]] bool const recorded =
                PlayerbotEconomyTraceRuntime(GetPlayerbotEconomyTrace())
                    .Complete(true, {
                                        .deduplicationKey = Acore::StringFormat(
                                            "listing:{}:{}", AuctionMarketId(bot->GetFaction()), listed->Id),
                                        .chainPublicId = chainPublicId,
                                        .actorGuid = bot->GetGUID().GetCounter(),
                                        .itemId = sale.itemId,
                                        .quantity = sale.count,
                                        .unitPriceCopper = (sale.buyout + sale.count - 1u) / sale.count,
                                        .depositCopper = sale.deposit,
                                        .referenceUnitPriceCopper = sale.buyerCeilingPerItem,
                                        .competingUnitPriceCopper = sale.lowestCompetingBuyoutPerItem,
                                        .occurredAt = now,
                                        .correlationAuctionId = listed->Id,
                                        .kind = EconomyTraceKind::Listed,
                                    });
        }
        Reset(botAI);
    }
    return listed ? ExecutionResult::Operation : ExecutionResult::Failed;
}

void DefaultPlayerbotEconomyRuntime::ObserveMarketEvidence(PlayerbotAI* botAI, uint32 marketId, uint64 now)
{
    if (!marketId)
        return;

    Player* const bot = botAI->GetBot();
    PlayerbotEconomyMarket& market = GetPlayerbotEconomyMarket();
    EconomyMarketSnapshot const snapshot = market.Snapshot(now);
    std::unordered_set<uint32> controlledAuctions;
    for (EconomyCirculation const& event : snapshot.circulation)
    {
        if (event.auctionId && event.provenance != EconomyCirculationProvenance::Ordinary)
            controlledAuctions.insert(event.auctionId);
    }

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(marketId));
    if (auctionHouse)
    {
        for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
        {
            if (!auction || controlledAuctions.contains(auctionId) || !auction->itemCount || !auction->buyout ||
                auction->expire_time <= static_cast<time_t>(now))
            {
                continue;
            }
            [[maybe_unused]] uint64 const evidenceToken = market.AppendEvidence(
                {
                    .marketId = marketId,
                    .itemId = auction->item_template,
                    .substitutionGroup = ReagentGroup(auction->item_template),
                    .source = EconomyEvidenceSource::Listing,
                    .auctionId = auctionId,
                    .unitPrice = (static_cast<uint64>(auction->buyout) + auction->itemCount - 1u) / auction->itemCount,
                    .quantity = auction->itemCount,
                    .observedAt = now,
                    .expiresAt = static_cast<uint64>(auction->expire_time),
                },
                0u, true);
        }
    }

    for (Mail const* mail : bot->GetMails())
    {
        std::optional<AuctionMailDetails> const details = ParseAuctionMail(mail);
        if (!details || details->response != AUCTION_SUCCESSFUL || !details->bid || !details->quantity ||
            mail->deliver_time > static_cast<time_t>(now) || controlledAuctions.contains(details->auctionId))
        {
            continue;
        }
        uint64 const observedAt = mail->deliver_time;
        [[maybe_unused]] uint64 const evidenceToken = market.AppendEvidence(
            {
                .marketId = marketId,
                .itemId = details->itemId,
                .substitutionGroup = ReagentGroup(details->itemId),
                .source = EconomyEvidenceSource::Sale,
                .auctionId = details->auctionId,
                .unitPrice = (details->bid + details->quantity - 1u) / details->quantity,
                .quantity = details->quantity,
                .observedAt = observedAt,
                .expiresAt = observedAt + MIN_AUCTION_TIME,
            },
            0u, true);
    }
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ReconcileMarketPositionMail(
    PlayerbotAI* botAI, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyMarket& market = GetPlayerbotEconomyMarket();
    EconomyMarketSnapshot const snapshot = market.Snapshot(now);
    if (!snapshot.persistenceHealthy)
        return std::nullopt;
    AuctionHouseObject* const auctionHouse =
        marketId ? sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(marketId)) : nullptr;

    for (EconomyPosition const& position : snapshot.positions)
    {
        if (position.traderGuid != bot->GetGUID().GetCounter() || position.marketId != marketId)
            continue;
        if (market.HasPendingWrite(position.publicId))
            continue;

        std::map<uint64, EconomyCirculation const*> latest;
        for (EconomyCirculation const& event : snapshot.circulation)
        {
            if (event.positionPublicId != position.publicId)
                continue;
            auto const current = latest.find(event.itemGuid);
            if (current == latest.end() || current->second->occurredAt < event.occurredAt)
                latest[event.itemGuid] = &event;
        }

        for (auto const& [itemGuid, event] : latest)
        {
            if (event->state != EconomyCirculationState::Listed || !event->auctionId ||
                (auctionHouse && auctionHouse->GetAuction(event->auctionId)))
            {
                continue;
            }

            for (Mail const* mail : bot->GetMails())
            {
                std::optional<AuctionMailDetails> const details = ParseAuctionMail(mail);
                if (!details || details->auctionId != event->auctionId || details->itemId != position.itemId ||
                    details->quantity != event->quantity)
                {
                    continue;
                }

                EconomyPositionEvent outcome;
                outcome.positionPublicId = position.publicId;
                outcome.itemGuid = itemGuid;
                outcome.quantity = details->quantity;
                outcome.auctionId = details->auctionId;
                outcome.occurredAt = now;
                if (details->response == AUCTION_SUCCESSFUL)
                {
                    outcome.kind = EconomyPositionEventKind::Sold;
                    outcome.proceeds = details->bid;
                    outcome.fees = details->cut;
                }
                else if (details->response == AUCTION_EXPIRED)
                {
                    outcome.kind = EconomyPositionEventKind::Expired;
                    outcome.fees = details->deposit;
                }
                else
                    continue;

                if (!market.ApplyPositionEvent(outcome, 0u, true).accepted)
                    return std::nullopt;

                PlayerbotEconomyCycleResult result;
                result.outcome = PlayerbotEconomyCycleOutcome::Operation;
                result.phase = EconomyPhase::SellSurplus;
                result.workIdentity = {0u, position.itemId, details->auctionId, itemGuid};
                result.blocker =
                    details->response == AUCTION_SUCCESSFUL ? "market_position_sold" : "market_position_expired";
                result.schedulingEffect = EconomyAttemptOutcome::Operation;
                return result;
            }
        }
    }
    return std::nullopt;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteMarketMaking(
    PlayerbotAI* botAI, EconomySnapshot const& snapshot, Creature* auctioneer, uint32 marketId, uint64 now)
{
    EconomyMarketSnapshot const market = GetPlayerbotEconomyMarket().Snapshot(now);
    uint32 const traderGuid = botAI->GetBot()->GetGUID().GetCounter();
    auto const position = std::find_if(market.positions.begin(), market.positions.end(),
                                       [traderGuid, marketId](EconomyPosition const& value)
                                       { return value.traderGuid == traderGuid && value.marketId == marketId; });
    if (position != market.positions.end())
    {
        bool const ordinaryVendorSupply =
            std::binary_search(snapshot.applicableUnlimitedGoldVendorItemIds.begin(),
                               snapshot.applicableUnlimitedGoldVendorItemIds.end(), position->itemId);
        return ManageMarketPosition(botAI, *position, market, auctioneer, ordinaryVendorSupply, now);
    }
    return OpenMarketPosition(botAI, snapshot, market, auctioneer, marketId, now);
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ManageMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, EconomyMarketSnapshot const& market, Creature* auctioneer,
    bool ordinaryVendorSupply, uint64 now)
{
    Player* const bot = botAI->GetBot();
    if (position.state == EconomyPositionState::Pending)
        return ManagePendingMarketPosition(botAI, position, market, auctioneer, now);
    if (!market.persistenceHealthy)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, 0u};
        result.blocker = "market_position_persistence_unavailable";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (GetPlayerbotEconomyMarket().HasPendingWrite(position.publicId))
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, 0u};
        result.blocker = "market_position_persistence_pending";
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }

    std::map<uint64, EconomyCirculation const*> latest;
    for (EconomyCirculation const& event : market.circulation)
    {
        if (event.positionPublicId != position.publicId)
            continue;
        auto const current = latest.find(event.itemGuid);
        if (current == latest.end() || current->second->occurredAt < event.occurredAt)
            latest[event.itemGuid] = &event;
    }

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(position.marketId));
    for (auto const& [itemGuid, event] : latest)
    {
        (void)itemGuid;
        if (event->state == EconomyCirculationState::Listed && event->auctionId && auctionHouse &&
            auctionHouse->GetAuction(event->auctionId))
        {
            return std::nullopt;
        }
    }

    Item* item = nullptr;
    uint64 missingItemGuid = 0u;
    bool mailBacked = false;
    for (auto const& [itemGuid, event] : latest)
    {
        if (event->state != EconomyCirculationState::Acquired && event->state != EconomyCirculationState::Delivered)
        {
            continue;
        }
        missingItemGuid = itemGuid;
        Item* candidate = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuid));
        if (candidate && candidate->GetEntry() == position.itemId && candidate->GetCount() >= event->quantity)
        {
            item = candidate;
            break;
        }
        Item const* mailed = bot->GetMItem(static_cast<ObjectGuid::LowType>(itemGuid));
        mailBacked =
            mailBacked || (mailed && mailed->GetEntry() == position.itemId && mailed->GetCount() >= event->quantity);
    }
    if (!item)
    {
        if (mailBacked || !missingItemGuid)
            return std::nullopt;

        EconomyPositionEvent lost{
            .kind = EconomyPositionEventKind::Lost,
            .positionPublicId = position.publicId,
            .itemGuid = missingItemGuid,
            .quantity = position.remainingQuantity,
            .occurredAt = now,
        };
        if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(lost, 0u, true).accepted)
            return std::nullopt;

        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, missingItemGuid};
        result.blocker = "market_position_lost";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        Reset(botAI);
        return result;
    }

    if (!PlayerbotEconomyPolicy::AllowsAutonomousListing({ordinaryVendorSupply, false, false}))
        return VendorMarketPosition(botAI, position, item, now);

    if (now >= position.holdingDeadline || position.relistAttempts >= position.maximumRelistAttempts)
        return VendorMarketPosition(botAI, position, item, now);

    bool const coolingDown = std::any_of(market.cooldowns.begin(), market.cooldowns.end(),
                                         [&position, now](EconomyCooldown const& cooldown)
                                         {
                                             return cooldown.traderGuid == position.traderGuid &&
                                                    cooldown.marketId == position.marketId &&
                                                    cooldown.substitutionGroup == position.substitutionGroup &&
                                                    cooldown.nextEligibleAt > now;
                                         });
    if (coolingDown)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, item->GetGUID().GetCounter()};
        result.blocker = "market_position_cooldown";
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }

    EconomyCoordinatorSnapshot const coordinator = GetPlayerbotEconomyCoordinator().Snapshot(now);
    bool const hasDemand =
        std::any_of(coordinator.gaps.begin(), coordinator.gaps.end(),
                    [&position](EconomyDemandGap const& gap)
                    {
                        return gap.marketId == position.marketId &&
                               PlayerbotEconomyConsumption::GroupKey(gap.group) == position.substitutionGroup &&
                               gap.remainingQuantity != 0u;
                    });
    std::optional<EconomyReferencePrice> const reference =
        GetPlayerbotEconomyMarket().ReferencePrice(position.marketId, position.substitutionGroup, now);
    if (!hasDemand || !reference || !reference->confident)
        return std::nullopt;
    return ListMarketPosition(botAI, position, item, auctioneer, *reference, now);
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ManagePendingMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, EconomyMarketSnapshot const& market, Creature* auctioneer,
    uint64 now)
{
    Player* const bot = botAI->GetBot();
    PlayerbotEconomyMarket& positionMarket = GetPlayerbotEconomyMarket();
    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {0u, position.itemId, 0u, 0u};
    result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
    result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;

    auto const pending = std::find_if(
        market.circulation.rbegin(), market.circulation.rend(), [&position](EconomyCirculation const& event)
        { return event.positionPublicId == position.publicId && event.state == EconomyCirculationState::Pending; });
    if (pending == market.circulation.rend())
    {
        result.blocker = "market_position_pending_provenance";
        return result;
    }
    result.workIdentity.auctionId = pending->auctionId;
    result.workIdentity.itemGuidCounter = pending->itemGuid;

    if (positionMarket.HasPendingWrite(position.publicId))
    {
        result.blocker = "market_position_persistence_pending";
        return result;
    }
    if (!market.persistenceHealthy)
    {
        result.blocker = "market_position_persistence_unavailable";
        return result;
    }

    ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(pending->itemGuid);
    Item* item = bot->GetItemByGuid(itemGuid);
    if (!item)
        item = bot->GetMItem(static_cast<ObjectGuid::LowType>(pending->itemGuid));
    if (item && item->GetEntry() == position.itemId && item->GetCount() >= position.remainingQuantity)
    {
        EconomyPositionMutationResult const activated =
            positionMarket.ActivatePendingPosition(position.publicId, 0u, now);
        result.outcome = activated.accepted ? PlayerbotEconomyCycleOutcome::Operation
                                            : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = activated.accepted ? "market_position_purchase_recovered" : "market_position_recovery_failed";
        result.schedulingEffect =
            activated.accepted ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    EconomyRiskFacts configurationFacts;
    configurationFacts.economyAffinity = 100u;
    configurationFacts.freeTradeskillMoney = MAX_MONEY_AMOUNT;
    configurationFacts.qualifiedEvidence = PlayerbotEconomyMarket::MAX_EVIDENCE_PER_GROUP;
    EconomyRiskDecision const configuration =
        PlayerbotEconomyMarket::EvaluateRisk(MarketRiskConfiguration(), configurationFacts);
    if (configuration.blocker != EconomyRiskBlocker::None)
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? PlayerbotEconomyMarket::RiskBlockerName(configuration.blocker)
                                   : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    if (now >= position.holdingDeadline)
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_stage_expired" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(position.marketId));
    AuctionEntry const* auction = auctionHouse ? auctionHouse->GetAuction(pending->auctionId) : nullptr;
    if (!auction)
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_candidate_gone" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    uint32 const sellerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
    if (auction->item_guid.GetCounter() != pending->itemGuid || auction->item_template != position.itemId ||
        auction->itemCount != position.remainingQuantity || auction->buyout != position.acquisitionCost ||
        !sellerAccountId || sellerAccountId == bot->GetSession()->GetAccountId())
    {
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_candidate_changed" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    EconomyAssignmentRequest request;
    request.characterGuid = bot->GetGUID().GetCounter();
    request.marketId = position.marketId;
    request.group = EconomySubstitutionGroup::ExactReagent(position.itemId);
    request.quantity = position.remainingQuantity;
    request.kind = EconomyClaimKind::Purchase;
    request.priority = EconomyClaimPriority::Speculation;
    request.workKind = EconomyWorkKind::MarketMaking;
    request.workIdentity = "auction:" + std::to_string(pending->auctionId);
    request.sellerAccountId = sellerAccountId;
    request.expiresAt = now + 1u;
    EconomyAssignmentLease const lease = GetPlayerbotEconomyCoordinator().Lease(std::move(request), now);
    if (!lease.assignment || lease.assignment->quantity != position.remainingQuantity)
    {
        if (lease.assignment)
        {
            [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                lease.assignment->leaseId, EconomyAssignmentOutcome::NeedChanged, 0u, now);
        }
        uint64 const cancelled = positionMarket.CancelPendingPosition(position.publicId, 0u);
        result.outcome =
            cancelled ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = cancelled ? "market_position_displaced" : "market_position_stage_cancel_failed";
        result.schedulingEffect =
            cancelled ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    if (!auctioneer)
    {
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::FailedTravel, 0u, now);
        result.outcome = TravelToAuctionHouse(botAI) ? PlayerbotEconomyCycleOutcome::Scheduled
                                                     : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                             ? "market_position_travel"
                             : "market_position_missing_auctioneer";
        result.schedulingEffect = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                                      ? EconomyAttemptOutcome::Operation
                                      : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    WorldPacket packet;
    packet << auctioneer->GetGUID();
    packet << pending->auctionId;
    packet << static_cast<uint32>(position.acquisitionCost);
    bot->GetSession()->HandleAuctionPlaceBid(packet);
    if (auctionHouse->GetAuction(pending->auctionId))
    {
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::FailedPurchase, 0u, now);
        [[maybe_unused]] uint64 const cooldown = positionMarket.SaveCooldown(
            {
                .traderGuid = position.traderGuid,
                .marketId = position.marketId,
                .substitutionGroup = position.substitutionGroup,
                .cause = EconomyCooldownCause::FailedPurchase,
                .nextEligibleAt = now + position.cooldownSeconds,
            },
            lease.assignment->leaseId, false);
        [[maybe_unused]] uint64 const cancelled =
            positionMarket.CancelPendingPosition(position.publicId, lease.assignment->leaseId);
        result.outcome = PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.blocker = "market_position_purchase_failed";
        result.schedulingEffect = EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }

    [[maybe_unused]] bool const committed = GetPlayerbotEconomyCoordinator().RecordOutcome(
        lease.assignment->leaseId, EconomyAssignmentOutcome::Committed, position.remainingQuantity, now);
    EconomyPositionMutationResult const activated =
        positionMarket.ActivatePendingPosition(position.publicId, lease.assignment->leaseId, now);
    if (activated.accepted)
    {
        [[maybe_unused]] bool const completed = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::Completed, position.remainingQuantity, now);
    }
    result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    result.blocker = activated.accepted ? "market_position_opened" : "market_position_commit_pending";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    Reset(botAI);
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::OpenMarketPosition(
    PlayerbotAI* botAI, EconomySnapshot const& snapshot, EconomyMarketSnapshot const& market, Creature* auctioneer,
    uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    (void)auctioneer;
    EconomyRiskConfiguration const configuration = MarketRiskConfiguration();
    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(marketId));
    AuctionHouseEntry const* const houseEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId(marketId));
    if (!auctionHouse || !houseEntry)
        return std::nullopt;

    uint64 totalExposure = 0u;
    std::map<std::string, uint64> groupExposure;
    for (EconomyPosition const& position : market.positions)
    {
        if (position.traderGuid != bot->GetGUID().GetCounter() || position.marketId != marketId)
            continue;
        totalExposure += position.acquisitionCost;
        groupExposure[position.substitutionGroup] += position.acquisitionCost;
    }

    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        if (!auction || !auction->itemCount || !auction->buyout || bot->GetItemCount(auction->item_template) != 0u)
        {
            continue;
        }
        bool const ordinaryVendorSupply =
            std::binary_search(snapshot.applicableUnlimitedGoldVendorItemIds.begin(),
                               snapshot.applicableUnlimitedGoldVendorItemIds.end(), auction->item_template);
        if (!PlayerbotEconomyPolicy::AllowsAutonomousListing({ordinaryVendorSupply, false, false}))
            continue;
        bool alreadyMailed = false;
        for (Mail const* mail : bot->GetMails())
        {
            alreadyMailed =
                alreadyMailed || std::any_of(mail->items.begin(), mail->items.end(), [auction](MailItemInfo const& item)
                                             { return item.item_template == auction->item_template; });
        }
        if (alreadyMailed)
            continue;

        std::string const group = ReagentGroup(auction->item_template);
        std::optional<EconomyReferencePrice> const reference =
            GetPlayerbotEconomyMarket().ReferencePrice(marketId, group, now);
        Item* const auctionItem = sAuctionMgr->GetAItem(auction->item_guid);
        if (!reference || !auctionItem)
            continue;

        EconomyMarketEntryFacts facts;
        facts.risk.economyAffinity = EconomyAffinity(bot->GetGUID().GetCounter());
        facts.risk.freeTradeskillMoney = snapshot.freeMoneyForTradeskill;
        facts.risk.groupExposure = groupExposure[group];
        facts.risk.totalExposure = totalExposure;
        facts.risk.qualifiedEvidence = reference->acceptedSales + reference->acceptedListings;
        facts.traderGuid = bot->GetGUID().GetCounter();
        facts.buyerAccountId = bot->GetSession()->GetAccountId();
        facts.sellerAccountId = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
        facts.marketId = marketId;
        facts.itemId = auction->item_template;
        facts.substitutionGroup = group;
        facts.itemGuid = auction->item_guid.GetCounter();
        facts.quantity = auction->itemCount;
        facts.buyout = auction->buyout;
        facts.referenceUnitPrice = reference->unitPrice;
        facts.referenceConfident = reference->confident;
        facts.depositPerListing =
            AuctionHouseMgr::GetAuctionDeposit(houseEntry, MIN_AUCTION_TIME, auctionItem, auction->itemCount);
        facts.auctionCutBasisPoints =
            static_cast<uint32>(houseEntry->cutPercent * sWorld->getRate(RATE_AUCTION_CUT) * 100.0f);
        facts.now = now;
        EconomyRiskDecision const risk = GetPlayerbotEconomyMarket().EvaluateEntry(configuration, facts);
        if (risk.blocker != EconomyRiskBlocker::None)
            continue;

        EconomyAssignmentRequest request;
        request.characterGuid = bot->GetGUID().GetCounter();
        request.marketId = marketId;
        request.group = EconomySubstitutionGroup::ExactReagent(auction->item_template);
        request.quantity = auction->itemCount;
        request.kind = EconomyClaimKind::Purchase;
        request.priority = EconomyClaimPriority::Speculation;
        request.workKind = EconomyWorkKind::MarketMaking;
        request.workIdentity = "auction:" + std::to_string(auctionId);
        request.sellerAccountId = facts.sellerAccountId;
        request.expiresAt = now + 1u;
        EconomyAssignmentLease const lease = GetPlayerbotEconomyCoordinator().Lease(std::move(request), now);
        if (!lease.assignment || lease.assignment->quantity != auction->itemCount)
        {
            if (lease.assignment)
            {
                [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
                    lease.assignment->leaseId, EconomyAssignmentOutcome::NeedChanged, 0u, now);
            }
            continue;
        }

        uint32 const purchasedItemId = auction->item_template;
        uint32 const purchasedQuantity = auction->itemCount;
        uint64 const purchasedItemGuid = auction->item_guid.GetCounter();
        uint64 const purchaseBuyout = auction->buyout;
        EconomyPosition position;
        position.publicId = PositionPublicId(bot->GetGUID().GetCounter(), purchasedItemGuid, now);
        position.traderGuid = bot->GetGUID().GetCounter();
        position.marketId = marketId;
        position.itemId = purchasedItemId;
        position.substitutionGroup = group;
        position.initialQuantity = purchasedQuantity;
        position.remainingQuantity = purchasedQuantity;
        position.acquisitionCost = purchaseBuyout;
        position.state = EconomyPositionState::Pending;
        position.maximumRelistAttempts = static_cast<uint8>(configuration.maximumRelistAttempts);
        position.cooldownSeconds = configuration.cooldownSeconds;
        position.holdingDeadline = now + configuration.holdingHorizonSeconds;
        EconomyPositionMutationResult const staged = GetPlayerbotEconomyMarket().StagePosition(
            std::move(position), purchasedItemGuid, auctionId, lease.assignment->leaseId, now);
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            lease.assignment->leaseId, EconomyAssignmentOutcome::NeedChanged, 0u, now);
        if (!staged.accepted)
            continue;

        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.phase = EconomyPhase::BuyReagent;
        result.workIdentity = {0u, purchasedItemId, auctionId, purchasedItemGuid};
        result.blocker = "market_position_staged";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        Reset(botAI);
        return result;
    }
    return std::nullopt;
}

bool DefaultPlayerbotEconomyRuntime::IsSafePositionItem(PlayerbotAI* botAI, Item const* item,
                                                        EconomyPosition const& position) const
{
    return item && item->GetEntry() == position.itemId && item->GetCount() == position.remainingQuantity &&
           IsInventoryBagItem(item) && item->CanBeTraded() && !item->IsSoulBound() && !item->IsNotEmptyBag() &&
           !item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) && item->GetUInt32Value(ITEM_FIELD_DURATION) == 0u &&
           !sAuctionMgr->GetAItem(item->GetGUID()) &&
           botAI->GetBot()->GetItemCount(position.itemId) >= position.remainingQuantity;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ListMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, Item* item, Creature* auctioneer,
    EconomyReferencePrice const& reference, uint64 now)
{
    Player* const bot = botAI->GetBot();
    if (!auctioneer)
    {
        PlayerbotEconomyCycleResult result;
        result.outcome = TravelToAuctionHouse(botAI) ? PlayerbotEconomyCycleOutcome::Scheduled
                                                     : PlayerbotEconomyCycleOutcome::FailedPrecondition;
        result.phase = EconomyPhase::SellSurplus;
        result.workIdentity = {0u, position.itemId, 0u, item->GetGUID().GetCounter()};
        result.blocker = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                             ? "market_position_travel"
                             : "market_position_missing_auctioneer";
        result.schedulingEffect = result.outcome == PlayerbotEconomyCycleOutcome::Scheduled
                                      ? EconomyAttemptOutcome::Operation
                                      : EconomyAttemptOutcome::FailedPrecondition;
        return result;
    }
    if (!IsSafePositionItem(botAI, item, position))
        return std::nullopt;

    AuctionHouseObject* const auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    AuctionHouseEntry const* const houseEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId(position.marketId));
    if (!auctionHouse || !houseEntry)
        return std::nullopt;

    uint32 const quantity = position.remainingQuantity;
    uint64 const ceiling = static_cast<uint64>(reference.unitPrice) * quantity;
    if (!ceiling || ceiling > MAX_MONEY_AMOUNT)
        return std::nullopt;
    SaleItemCandidate sale;
    sale.count = quantity;
    sale.templateSellPrice = item->GetTemplate()->SellPrice;
    sale.deposit = AuctionHouseMgr::GetAuctionDeposit(houseEntry, MIN_AUCTION_TIME, item, quantity);
    sale.auctionCutBasisPoints =
        static_cast<uint32>(houseEntry->cutPercent * sWorld->getRate(RATE_AUCTION_CUT) * 100.0f);
    sale.buyerCeilingPerItem = reference.unitPrice;
    sale.minimumTransactionBasis = 1u;
    uint64 const floor = PlayerbotEconomyPolicy::SellerFloor(sale);
    if (!floor || floor > ceiling)
        return std::nullopt;

    std::unordered_set<uint32> existingAuctions;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        (void)auction;
        existingAuctions.insert(auctionId);
    }
    ObjectGuid const originalGuid = item->GetGUID();
    WorldPacket packet;
    packet << auctioneer->GetGUID();
    packet << uint32(1);
    packet << originalGuid;
    packet << quantity;
    packet << static_cast<uint32>(floor);
    packet << static_cast<uint32>(ceiling);
    packet << static_cast<uint32>(MIN_AUCTION_TIME / MINUTE);
    bot->GetSession()->HandleAuctionSellItem(packet);

    AuctionEntry const* listed = nullptr;
    for (auto const& [auctionId, auction] : auctionHouse->GetAuctions())
    {
        if (!existingAuctions.contains(auctionId) && auction && auction->owner == bot->GetGUID() &&
            auction->item_template == position.itemId && auction->itemCount == quantity && auction->startbid == floor &&
            auction->buyout == ceiling)
        {
            listed = auction;
            break;
        }
    }
    if (!listed)
        return std::nullopt;

    if (listed->item_guid != originalGuid)
    {
        Item* const remaining = bot->GetItemByGuid(originalGuid);
        if (!remaining)
            return std::nullopt;
        EconomyPositionEvent split{
            .kind = EconomyPositionEventKind::Split,
            .positionPublicId = position.publicId,
            .itemGuid = originalGuid.GetCounter(),
            .replacementItemGuid = listed->item_guid.GetCounter(),
            .quantity = remaining->GetCount(),
            .replacementQuantity = quantity,
            .occurredAt = now,
        };
        if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(split, 0u, true).accepted)
            return std::nullopt;
    }

    EconomyMarketSnapshot const market = GetPlayerbotEconomyMarket().Snapshot(now);
    bool const previouslyListed = std::any_of(
        market.circulation.begin(), market.circulation.end(), [&position](EconomyCirculation const& event)
        { return event.positionPublicId == position.publicId && event.state == EconomyCirculationState::Listed; });
    EconomyPositionEvent event{
        .kind = previouslyListed ? EconomyPositionEventKind::Relisted : EconomyPositionEventKind::Listed,
        .positionPublicId = position.publicId,
        .itemGuid = listed->item_guid.GetCounter(),
        .quantity = quantity,
        .auctionId = listed->Id,
        .occurredAt = now,
    };
    if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(event, 0u, true).accepted)
        return std::nullopt;

    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    result.phase = EconomyPhase::SellSurplus;
    result.workIdentity = {0u, position.itemId, listed->Id, listed->item_guid.GetCounter()};
    result.blocker = previouslyListed ? "market_position_relisted" : "market_position_listed";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    Reset(botAI);
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::VendorMarketPosition(
    PlayerbotAI* botAI, EconomyPosition const& position, Item* item, uint64 now)
{
    Player* const bot = botAI->GetBot();
    if (!item || item->GetCount() != position.remainingQuantity || item->GetTemplate()->SellPrice == 0u)
    {
        return std::nullopt;
    }
    ObjectGuid const itemGuid = item->GetGUID();
    uint32 const quantity = item->GetCount();
    uint32 const moneyBefore = bot->GetMoney();
    SellAction(botAI, "economy position vendor").Sell(item);
    if (bot->GetItemByGuid(itemGuid))
        return std::nullopt;

    EconomyPositionEvent event{
        .kind = EconomyPositionEventKind::Vendored,
        .positionPublicId = position.publicId,
        .itemGuid = itemGuid.GetCounter(),
        .quantity = quantity,
        .proceeds = bot->GetMoney() >= moneyBefore ? bot->GetMoney() - moneyBefore : 0u,
        .occurredAt = now,
    };
    if (!GetPlayerbotEconomyMarket().ApplyPositionEvent(event, 0u, true).accepted)
        return std::nullopt;

    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Operation;
    result.phase = EconomyPhase::SellSurplus;
    result.workIdentity = {0u, position.itemId, 0u, itemGuid.GetCounter()};
    result.blocker = "market_position_vendored";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    Reset(botAI);
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::ExecuteAutonomousGathering(
    PlayerbotAI* botAI, PlayerbotCareerPlan const& careerPlan, EconomySnapshot const& snapshot,
    EconomyDecision const& productionDecision, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    // A feeder gathering skill rides on crafting affinity (PlayerbotCareer::FeederCraftingSeed), so a career
    // that plans a gathering skill keeps its trips regardless of the bot's own gathering affinity.
    if (GatheringAffinity(bot->GetGUID().GetCounter()) < PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM &&
        !PlayerbotCareer::PlansGatheringSkill(careerPlan))
        return std::nullopt;

    if (!activeGathering && bot->HasSkill(SKILL_MINING) && !HasRequiredGatheringTool(bot, SKILL_MINING))
    {
        // Mining needs a pick in the bags. It is bought, never granted.
        SpellInfo const* const miningSpell = sSpellMgr->GetSpellInfo(GatheringInteractionSpellId(SKILL_MINING));
        if (std::optional<uint32> const tool = MissingVendorTool(bot, miningSpell))
            return BuyProgressionVendorInput(botAI, *tool, miningSpell->Id);
    }

    PlayerbotEconomyCoordinator& coordinator = GetPlayerbotEconomyCoordinator();
    EconomyCoordinatorSnapshot const coordinatorSnapshot = coordinator.Snapshot(now);
    std::optional<GatheringOpportunity> const selfOpportunity =
        DeficitGatheringOpportunity(bot, snapshot, productionDecision);
    std::optional<GatheringOpportunity> const coordinatorOpportunity =
        CoordinatorGatheringOpportunity(bot, coordinatorSnapshot, snapshot, marketId);
    if (!activeGathering)
    {
        std::optional<PlayerbotEconomyCycleResult> selfFailure;
        if (selfOpportunity)
        {
            std::optional<PlayerbotEconomyCycleResult> const result = StartAutonomousGathering(
                botAI, *selfOpportunity, productionDecision.phase == EconomyPhase::None, marketId, now);
            if (result && result->outcome != PlayerbotEconomyCycleOutcome::NoCandidate)
                return result;
            selfFailure = result;
        }
        if (coordinatorOpportunity)
        {
            std::optional<PlayerbotEconomyCycleResult> const result = StartAutonomousGathering(
                botAI, *coordinatorOpportunity, productionDecision.phase == EconomyPhase::None, marketId, now);
            if (result)
                return result;
        }
        if (selfFailure)
            return selfFailure;

        if (productionDecision.phase != EconomyPhase::None)
            return std::nullopt;
        for (uint16 skillId : LearnedPrimaryCapabilitySkillIds(bot))
        {
            std::optional<GatheringProfession> const profession = GatheringProfessionForSkill(skillId);
            if (!profession || !bot->HasSkill(skillId) ||
                bot->GetSkillValue(skillId) >= PlayerbotEconomyGathering::GatheringSkillTargetForLevel(
                                                   bot->GetLevel(), bot->GetMaxSkillValue(skillId)))
            {
                continue;
            }
            return StartAutonomousGathering(botAI, GatheringOpportunity{*profession, skillId, 0u, 0u, 0u}, false,
                                            marketId, now);
        }
        return std::nullopt;
    }

    ActiveGatheringTrip& trip = *activeGathering;
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
    bool const targetOwned = activeGatheringPointDestination &&
                             ownedTravelDestination == activeGatheringPointDestination && target->isForced() &&
                             target->getDestination() == ownedTravelDestination;

    AutonomousGatheringFacts facts;
    facts.now = now;
    facts.currentItemCount = trip.plan.itemId ? bot->GetItemCount(trip.plan.itemId) : 0u;
    facts.currentSkillValue = bot->GetSkillValue(trip.skillId);
    auto const activeClaim =
        std::find_if(coordinatorSnapshot.claims.begin(), coordinatorSnapshot.claims.end(),
                     [&trip](EconomyAssignment const& claim) { return claim.leaseId == trip.coordinatorLeaseId; });
    facts.demandStillExists =
        !trip.coordinatorLeaseId || (!trip.coordinatorSettled && activeClaim != coordinatorSnapshot.claims.end() &&
                                     activeClaim->state == EconomyClaimState::Leased);
    facts.destinationAvailable = targetOwned && target->isActive() && target->getStatus() != TRAVEL_STATUS_COOLDOWN &&
                                 target->getStatus() != TRAVEL_STATUS_EXPIRED;
    facts.atDestination = facts.destinationAvailable && target->getStatus() == TRAVEL_STATUS_WORK;
    facts.safe = bot->IsInWorld() && bot->IsAlive() && !bot->GetTransport() && !bot->InBattleground() &&
                 !bot->IsBeingTeleported() && !bot->HasUnitState(UNIT_STATE_IN_FLIGHT) &&
                 (!bot->IsInCombat() || trip.killTarget);
    if (trip.plan.itemId)
    {
        ItemPosCountVec destinations;
        facts.inventoryCapacity =
            bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, trip.plan.itemId, 1u) == EQUIP_ERR_OK;
    }
    else
        facts.inventoryCapacity = AI_VALUE(uint8, "bag space") <= 80u;

    if (facts.atDestination)
    {
        facts.resourceAvailable = HasMatchingGatheringLoot(botAI, trip.skillId);
        facts.existingSkinningCorpse = facts.resourceAvailable;
    }
    facts.creatureKillStarted = static_cast<bool>(trip.killTarget);
    if (trip.killTarget)
    {
        Unit* const targetUnit = botAI->GetUnit(trip.killTarget);
        facts.creatureKillActive = targetUnit && targetUnit->IsAlive();
        facts.existingSkinningCorpse =
            facts.existingSkinningCorpse || (targetUnit && !targetUnit->IsAlive() && targetUnit->IsInWorld());
        Creature const* const corpse = targetUnit ? targetUnit->ToCreature() : nullptr;
        facts.corpseLootPending = corpse && !corpse->IsAlive() && corpse->IsInWorld() && corpse->isTappedBy(bot) &&
                                  corpse->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
    }

    AutonomousGatheringDecision const decision = PlayerbotEconomyGathering::DecideAutonomous(trip.plan, facts);
    if (trip.coordinatorLeaseId && !trip.coordinatorSettled && decision.gatheredQuantity > trip.committedQuantity)
    {
        trip.committedQuantity = std::min(decision.gatheredQuantity, trip.plan.requestedQuantity);
        [[maybe_unused]] bool const committed = coordinator.RecordOutcome(
            trip.coordinatorLeaseId, EconomyAssignmentOutcome::Committed, trip.committedQuantity, now);
    }
    PlayerbotEconomyCycleResult result;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {trip.spellId, trip.plan.itemId, 0u, 0u};

    if (decision.action == AutonomousGatheringAction::Complete)
    {
        if (trip.coordinatorLeaseId && !trip.coordinatorSettled)
        {
            [[maybe_unused]] bool const completed = coordinator.RecordOutcome(
                trip.coordinatorLeaseId, EconomyAssignmentOutcome::Completed, trip.plan.requestedQuantity, now);
            trip.coordinatorSettled = true;
        }
        if (trip.plan.itemId && trip.committedQuantity)
            pendingGatheredSupply[trip.plan.itemId] += trip.committedQuantity;
        result.outcome = PlayerbotEconomyCycleOutcome::Operation;
        result.blocker = "gathering_complete";
        result.schedulingEffect = EconomyAttemptOutcome::Operation;
        Reset(botAI);
        return result;
    }
    if (decision.action == AutonomousGatheringAction::Release)
    {
        bool const authoritativeRelease = decision.blocker == AutonomousGatheringBlocker::DemandGone;
        if (trip.coordinatorLeaseId && !trip.coordinatorSettled)
        {
            EconomyAssignmentOutcome const outcome = authoritativeRelease ? EconomyAssignmentOutcome::NeedChanged
                                                     : trip.committedQuantity
                                                         ? EconomyAssignmentOutcome::InventoryReceived
                                                         : EconomyAssignmentOutcome::FailedTravel;
            [[maybe_unused]] bool const released =
                coordinator.RecordOutcome(trip.coordinatorLeaseId, outcome, trip.committedQuantity, now);
            trip.coordinatorSettled = true;
        }
        if (!authoritativeRelease && trip.plan.itemId && trip.committedQuantity)
            pendingGatheredSupply[trip.plan.itemId] += trip.committedQuantity;
        // A trip that captured loot before it ended did real work; only an empty-handed
        // release counts as a failure for backoff and quarantine purposes.
        bool const progress = PlayerbotEconomyGathering::ReleaseCountsAsProgress(decision);
        result.outcome = progress ? PlayerbotEconomyCycleOutcome::Operation : PlayerbotEconomyCycleOutcome::NoCandidate;
        result.blocker = AutonomousBlockerName(decision.blocker);
        result.schedulingEffect = progress ? EconomyAttemptOutcome::Operation : EconomyAttemptOutcome::NoCandidate;
        Reset(botAI);
        return result;
    }
    // Moves the trip to the next spawn point of its destination. Returns the terminal cycle result when
    // there is none left or the walk cannot be set up; otherwise the trip is searching.
    auto const searchNextPoint = [&]() -> std::optional<PlayerbotEconomyCycleResult>
    {
        WorldPosition botPosition(bot);
        WorldPosition* const nextPoint =
            trip.destination->NextUnvisitedPoint(botPosition, bot->GetMapId(), trip.attemptedPoints);
        if (!nextPoint)
        {
            if (trip.coordinatorLeaseId && !trip.coordinatorSettled)
            {
                [[maybe_unused]] bool const released = PlayerbotEconomyGathering::SettleUnavailableDestination(
                    coordinator, trip.coordinatorLeaseId, trip.committedQuantity, now);
                trip.coordinatorSettled = true;
            }
            if (trip.plan.itemId && trip.committedQuantity)
                pendingGatheredSupply[trip.plan.itemId] += trip.committedQuantity;
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = "gathering_resource_unavailable";
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            Reset(botAI);
            return result;
        }

        if (!TravelToGatheringPoint(botAI, trip.destination, nextPoint))
        {
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.blocker = "gathering_destination_unavailable";
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            Reset(botAI);
            return result;
        }
        trip.attemptedPoints.push_back(nextPoint);
        result.blocker = "gathering_search";
        return std::nullopt;
    };

    if (decision.action == AutonomousGatheringAction::Gather)
        result.blocker = "gathering_resource";
    else if (decision.action == AutonomousGatheringAction::GrindOneCreature)
    {
        bool const hunting = trip.plan.profession == GatheringProfession::Hunting;
        // A hunt engages creature after creature; the previous corpse is emptied by now.
        if (hunting)
            trip.killTarget.Clear();
        if (!StartOneCreatureKill(botAI, trip.destination))
        {
            if (hunting)
            {
                // Nothing of the population stands here; walk to its next spawn point.
                if (std::optional<PlayerbotEconomyCycleResult> const terminal = searchNextPoint())
                    return *terminal;
            }
            else
            {
                result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
                result.blocker = "gathering_skinning_creature_missing";
                result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
                Reset(botAI);
                return result;
            }
        }
        else
            result.blocker = hunting ? "gathering_hunting_kill" : "gathering_skinning_grind";
    }
    else if (decision.action == AutonomousGatheringAction::Travel && facts.atDestination)
    {
        if (std::optional<PlayerbotEconomyCycleResult> const terminal = searchNextPoint())
            return *terminal;
    }
    else
        result.blocker = decision.action == AutonomousGatheringAction::Travel ? "gathering_travel" : "gathering_wait";

    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.schedulingEffect = EconomyAttemptOutcome::Tracking;
    return result;
}

std::optional<PlayerbotEconomyCycleResult> DefaultPlayerbotEconomyRuntime::StartAutonomousGathering(
    PlayerbotAI* botAI, GatheringOpportunity const& opportunity, bool allowFailureResult, uint32 marketId, uint64 now)
{
    Player* const bot = botAI->GetBot();
    GatheringDestinationBlocker blocker = GatheringDestinationBlocker::Empty;
    std::vector<GatheringTravelDestination*> const destinations = sPlayerbotEconomyTravelCatalog.GatheringDestinations(
        bot, opportunity.skillId, &blocker, false, 5000.0f, opportunity.itemId);
    if (destinations.empty())
    {
        if (!allowFailureResult)
            return std::nullopt;
        PlayerbotEconomyCycleResult result;
        result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
        result.phase = EconomyPhase::BuyReagent;
        result.workIdentity = {opportunity.spellId, opportunity.itemId, 0u, 0u};
        result.blocker = GatheringDestinationBlockerName(blocker);
        result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
        return result;
    }

    // Nearest point whose node is actually up, across every eligible destination. A destination's own
    // distance counts despawned points too, so it cannot pick the walk the bot is really about to make.
    WorldPosition botPosition(bot);
    GatheringTravelDestination* destination = nullptr;
    WorldPosition* initialPoint = nullptr;
    float initialDistance = std::numeric_limits<float>::infinity();
    for (GatheringTravelDestination* candidate : destinations)
    {
        WorldPosition* const point = candidate->NextUnvisitedPoint(botPosition, bot->GetMapId(), {});
        if (!point)
            continue;
        float const distance = botPosition.distance(point);
        if (std::isfinite(distance) && distance < initialDistance)
        {
            destination = candidate;
            initialPoint = point;
            initialDistance = distance;
        }
    }
    if (!destination)
        return std::nullopt;

    uint32 requestedQuantity = opportunity.activeUncoveredDemand;
    uint32 outboundSeconds = 0u;
    if (!opportunity.itemId)
    {
        // Skill-up trips have no work order capacity check, so budget the walk here: the trip clock starts
        // now and includes the outbound leg, and a walk that eats the budget gathers nothing.
        float const speed = bot->GetSpeed(MOVE_RUN);
        std::vector<WorldPosition> const route = botPosition.getPathTo(*initialPoint, bot);
        float const distance = route.empty() ? initialDistance : botPosition.getPathLength(route);
        if (!std::isfinite(speed) || speed <= 0.0f || !std::isfinite(distance) || distance < 0.0f)
            return std::nullopt;
        outboundSeconds = static_cast<uint32>(std::ceil(distance / speed));
        if (!PlayerbotEconomyGathering::OutboundFitsTripBudget(outboundSeconds, destination->getExpireDelay() / 1000u))
        {
            if (!allowFailureResult)
                return std::nullopt;
            PlayerbotEconomyCycleResult result;
            result.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
            result.phase = EconomyPhase::BuyReagent;
            result.workIdentity = {opportunity.spellId, opportunity.itemId, 0u, 0u};
            result.blocker = "gathering_destination_too_far";
            result.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
            return result;
        }
    }
    EconomyCoordinatorSnapshot const coordinatorSnapshot = GetPlayerbotEconomyCoordinator().Snapshot(now);
    if (opportunity.itemId)
    {
        std::optional<RuntimeGatheringCandidate> current = BuildRuntimeGatheringCandidate(
            bot, opportunity.skillId, opportunity.itemId, opportunity.activeUncoveredDemand, coordinatorSnapshot,
            marketId, now, opportunity.coordinatorBacklog);
        if (!current)
            return std::nullopt;

        if (opportunity.coordinatorBacklog)
        {
            DedicatedGatheringPlan const plan =
                PlanRuntimeGatheringWork(bot, current->policy, opportunity.skillId, opportunity.itemId,
                                         opportunity.activeUncoveredDemand, coordinatorSnapshot, marketId, now);
            auto const workOrder = std::find_if(plan.workOrders.begin(), plan.workOrders.end(),
                                                [bot](DedicatedGatheringWorkOrder const& order)
                                                { return order.characterGuid == bot->GetGUID().GetCounter(); });
            if (workOrder == plan.workOrders.end())
                return std::nullopt;
            requestedQuantity = std::min(workOrder->quantity, current->policy.capacity);
        }
        else
            requestedQuantity = current->policy.capacity;

        destination = current->destination;
        initialPoint = current->initialPoint;
        outboundSeconds = current->outboundSeconds;
    }

    if (!TravelToGatheringPoint(botAI, destination, initialPoint))
        return std::nullopt;

    std::optional<EconomyAssignment> assignment;
    if (opportunity.itemId)
    {
        EconomyAssignmentRequest request;
        request.characterGuid = botAI->GetBot()->GetGUID().GetCounter();
        request.marketId = marketId;
        request.group = EconomySubstitutionGroup::ExactReagent(opportunity.itemId);
        request.quantity = requestedQuantity;
        request.kind = EconomyClaimKind::Resource;
        request.priority = EconomyClaimPriority::Producer;
        request.workKind = EconomyWorkKind::Gather;
        request.workIdentity =
            "gather:" + std::to_string(opportunity.itemId) + ":" + std::to_string(opportunity.skillId);
        request.expiresAt = now + destination->getExpireDelay() / 1000u;
        EconomyAssignmentLease const lease = GetPlayerbotEconomyCoordinator().Lease(std::move(request), now);
        if (!lease.assignment)
        {
            Reset(botAI);
            return std::nullopt;
        }
        assignment = lease.assignment;
    }

    ActiveGatheringTrip trip;
    trip.plan.profession = opportunity.profession;
    trip.plan.itemId = opportunity.itemId;
    trip.plan.requestedQuantity = assignment ? assignment->quantity : requestedQuantity;
    trip.plan.startingItemCount = opportunity.itemId ? bot->GetItemCount(opportunity.itemId) : 0u;
    trip.plan.startingSkillValue = bot->GetSkillValue(opportunity.skillId);
    trip.plan.expiresAt = now + destination->getExpireDelay() / 1000u;
    trip.skillId = opportunity.skillId;
    trip.spellId = opportunity.spellId;
    trip.startedAt = now;
    trip.outboundSeconds = outboundSeconds;
    trip.coordinatorLeaseId = assignment ? assignment->leaseId : 0u;
    trip.destination = destination;
    TravelTarget* const target = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (target->getPosition())
        trip.attemptedPoints.push_back(target->getPosition());
    activeGathering = std::move(trip);
    GetPlayerbotEconomyGathering().SetActiveTrip(bot->GetGUID().GetCounter(), activeGathering->skillId);
    if (opportunity.profession == GatheringProfession::Hunting)
    {
        LOG_INFO("playerbots.economy", "Bot {} starts a {} hunt for {} x item {} at population {} (recipe {}).",
                 bot->GetGUID().GetCounter(), opportunity.coordinatorBacklog ? "coordinator" : "deficit",
                 activeGathering->plan.requestedQuantity, opportunity.itemId, destination->getEntry(),
                 opportunity.spellId);
    }

    PlayerbotEconomyCycleResult result;
    result.outcome = PlayerbotEconomyCycleOutcome::Scheduled;
    result.phase = EconomyPhase::BuyReagent;
    result.workIdentity = {opportunity.spellId, opportunity.itemId, 0u, 0u};
    result.blocker = "gathering_travel";
    result.schedulingEffect = EconomyAttemptOutcome::Operation;
    return result;
}

bool DefaultPlayerbotEconomyRuntime::HasMatchingGatheringLoot(PlayerbotAI* botAI, uint32 skillId)
{
    AiObjectContext* const context = botAI->GetAiObjectContext();
    botAI->DoSpecificAction("add gathering loot", Event(), true);
    if (!AI_VALUE(bool, "has available loot"))
        return false;
    // Same radius the loot strategy acts on; a node visible further out kept trips idling in
    // gathering_resource instead of advancing to the next spawn point.
    LootObject loot = AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.lootDistance);
    return !loot.IsEmpty() && loot.skillId == skillId;
}

bool DefaultPlayerbotEconomyRuntime::StartOneCreatureKill(PlayerbotAI* botAI, GatheringTravelDestination* destination)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "nearest npcs"))
    {
        Unit* const unit = botAI->GetUnit(guid);
        // Most skinnable beasts are neutral, not hostile. Anything the bot may legally attack will do.
        if (!unit || !unit->IsAlive() || unit->GetEntry() != static_cast<uint32>(destination->getEntry()) ||
            !bot->IsValidAttackTarget(unit) || bot->IsFriendlyTo(unit) || unit->IsInCombat() ||
            !WorldPosition(bot).canPathTo(WorldPosition(unit), bot))
        {
            continue;
        }
        // Setting the pull target alone engages nothing for a non-tank bot; open the fight here.
        if (!EconomyAttackAction(botAI).Apply(unit))
            continue;
        activeGathering->killTarget = guid;
        SET_AI_VALUE(ObjectGuid, "pull target", guid);
        return true;
    }
    return false;
}

bool DefaultPlayerbotEconomyRuntime::TravelToGatheringPoint(PlayerbotAI* botAI, GatheringTravelDestination* destination,
                                                            WorldPosition* point)
{
    if (!destination || !point)
        return false;

    AiObjectContext* const context = botAI->GetAiObjectContext();
    TravelTarget* const currentTarget = AI_VALUE(TravelTarget*, "travel target");
    if (currentTarget->isForced() &&
        (!ownedTravelDestination || currentTarget->getDestination() != ownedTravelDestination))
    {
        return false;
    }

    TravelDestination* const pointDestination = destination->PointDestination(point);
    if (!pointDestination)
        return false;

    AcquireTravelStrategies(botAI);

    TravelTarget newTarget(botAI);
    newTarget.setTarget(pointDestination, point);
    newTarget.setRadius(INTERACTION_DISTANCE);
    newTarget.setForced(true);
    EconomyTravelAction(botAI).Apply(&newTarget, currentTarget);
    activeGatheringPointDestination = pointDestination;
    ownedTravelDestination = pointDestination;
    return true;
}

bool DefaultPlayerbotEconomyRuntime::TravelToAuctionHouse(PlayerbotAI* botAI)
{
    return TravelToDestination(botAI, sPlayerbotEconomyTravelCatalog.SelectAuctioneer(botAI->GetBot()));
}

bool DefaultPlayerbotEconomyRuntime::TravelToMailbox(PlayerbotAI* botAI)
{
    return TravelToDestination(botAI, sPlayerbotEconomyTravelCatalog.SelectMailbox(botAI->GetBot()));
}

std::optional<DefaultPlayerbotEconomyRuntime::SpellFocusStand> DefaultPlayerbotEconomyRuntime::SpellFocusStandPoint(
    Player* bot, PlayerbotEconomyTravelCatalog::SpellFocusDestination const& focus)
{
    WorldPosition const& object = focus.position;
    Map* const map = bot->GetMap();
    if (!map)
        return std::nullopt;

    constexpr float pi = 3.14159265358979f;
    // Sweep the whole circle on every ring, the bot's own side first: the dry rim of a pool may lie
    // anywhere around it, and a bot that arrives from the wrong side still has to find it.
    constexpr std::array<float, 12> sweepDegrees = {0.0f,   30.0f,  -30.0f,  60.0f,  -60.0f,  90.0f,
                                                    -90.0f, 120.0f, -120.0f, 150.0f, -150.0f, 180.0f};
    float const dx = bot->GetPositionX() - object.GetPositionX();
    float const dy = bot->GetPositionY() - object.GetPositionY();
    float const base = (dx * dx + dy * dy) > 0.01f
                           ? std::atan2(dy, dx)
                           : 2.0f * pi * static_cast<float>(bot->GetGUID().GetCounter() % 360u) / 360.0f;
    uint32 rejected = 0u;
    for (float const distance : SpellFocusStandOffDistances(focus.focusRange))
    {
        for (float const degrees : sweepDegrees)
        {
            float const angle = base + degrees * pi / 180.0f;
            EconomyApproachPoint const candidate = {object.GetPositionX() + std::cos(angle) * distance,
                                                    object.GetPositionY() + std::sin(angle) * distance};
            // Find the floor first, then sample the liquid just above it: the Ironforge crust around the
            // pool sits 0.7 yards above the forge origin, and a sample below the floor reports no liquid
            // even though the crust is covered by magma. Any magma or slime at the spot disqualifies it.
            float const ground =
                map->GetHeight(bot->GetPhaseMask(), candidate.x, candidate.y, object.GetPositionZ() + 2.0f, true);
            float const sampleZ = ground > INVALID_HEIGHT ? ground + 1.0f : object.GetPositionZ();
            LiquidData const liquid =
                map->GetLiquidData(bot->GetPhaseMask(), candidate.x, candidate.y, sampleZ, bot->GetCollisionHeight(),
                                   MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME);
            if (liquid.Status != LIQUID_MAP_NO_WATER || ground <= INVALID_HEIGHT ||
                std::fabs(object.GetPositionZ() - ground) > SPELL_FOCUS_STAND_POINT_MAX_DROP)
            {
                ++rejected;
                continue;
            }
            LOG_INFO("playerbots.economy",
                     "Bot {} stands {:.1f} yards off spell focus ({:.1f}, {:.1f}, {:.1f}) at ({:.1f}, {:.1f}) "
                     "ground {:.1f} after {} rejected points.",
                     bot->GetGUID().GetCounter(), distance, object.GetPositionX(), object.GetPositionY(),
                     object.GetPositionZ(), candidate.x, candidate.y, ground, rejected);
            return SpellFocusStand{candidate, distance};
        }
    }
    // Nothing level and dry inside focus range. Refusing the craft beats a lava bath.
    LOG_INFO("playerbots.economy",
             "Bot {} found no dry, level stand point within {} yards of spell focus ({:.1f}, {:.1f}, {:.1f}); "
             "skipping the craft.",
             bot->GetGUID().GetCounter(), focus.focusRange / 2u, object.GetPositionX(), object.GetPositionY(),
             object.GetPositionZ());
    return std::nullopt;
}

bool DefaultPlayerbotEconomyRuntime::TravelToDestination(PlayerbotAI* botAI, TravelDestination* destination,
                                                         float radius, std::optional<EconomyApproachPoint> standPoint)
{
    if (!destination)
        return false;

    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    TravelTarget* currentTarget = AI_VALUE(TravelTarget*, "travel target");
    if (currentTarget->isForced() &&
        (!ownedTravelDestination || currentTarget->getDestination() != ownedTravelDestination))
    {
        return false;
    }

    if (currentTarget->isForced() && currentTarget->getDestination() == ownedTravelDestination)
    {
        if (currentTarget->getDestination() == destination && currentTarget->isActive())
            return true;

        Reset(botAI);
    }

    AcquireTravelStrategies(botAI);

    WorldPosition botPosition(bot);
    WorldPosition const* const point = destination->nearestPoint(&botPosition);
    // Stand a few yards off the object rather than on top of it: still inside interaction range, but the
    // bots no longer pile onto the mailbox or auctioneer itself. A tighter radius (spell focus) wins,
    // unless the caller already probed for a safe stand point.
    EconomyApproachPoint const approach =
        standPoint ? *standPoint
                   : ApproachPoint(point->GetPositionX(), point->GetPositionY(), botPosition.GetPositionX(),
                                   botPosition.GetPositionY(), std::min(radius, APPROACH_STAND_OFF_DISTANCE),
                                   bot->GetGUID().GetCounter());
    ownedTravelPoint =
        WorldPosition(point->GetMapId(), approach.x, approach.y, point->GetPositionZ(), point->GetOrientation());
    TravelTarget newTarget(botAI);
    newTarget.setTarget(destination, &ownedTravelPoint);
    newTarget.setRadius(radius);
    newTarget.setForced(true);
    ownedTravelDestination = destination;
    EconomyTravelAction(botAI).Apply(&newTarget, currentTarget);
    return true;
}

bool DefaultPlayerbotEconomyRuntime::IsInventoryBagItem(Item const* item) const
{
    if (!item)
        return false;

    uint8 const bag = item->GetBagSlot();
    uint8 const slot = item->GetSlot();
    if (bag == INVENTORY_SLOT_BAG_0)
        return slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END;

    return bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END;
}

bool DefaultPlayerbotEconomyRuntime::IsSafeSaleItem(PlayerbotAI* botAI, Item const* item,
                                                    EconomyDecision const& decision)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    if (!item || item->GetGUID().GetCounter() != decision.itemGuidCounter || item->GetEntry() != decision.itemId ||
        item->GetCount() < decision.count || !IsInventoryBagItem(item))
    {
        return false;
    }
    ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
    if (usage != ITEM_USAGE_AH && usage != ITEM_USAGE_SKILL)
        return false;

    if (!PlayerbotEconomyPolicy::PreservesProfessionReserve(bot->GetItemCount(item->GetEntry()), decision.count,
                                                            decision.professionReserveFloor))
    {
        return false;
    }

    return item->CanBeTraded() && !item->IsSoulBound() && !item->IsNotEmptyBag() &&
           !item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) && item->GetUInt32Value(ITEM_FIELD_DURATION) == 0 &&
           !sAuctionMgr->GetAItem(item->GetGUID()) && decision.startBid > 0 && decision.startBid <= decision.buyout &&
           decision.buyout <= MAX_MONEY_AMOUNT;
}

bool DefaultPlayerbotEconomyRuntime::OwnsTripInFlight(PlayerbotAI* botAI)
{
    if (!ownedTravelDestination)
        return false;

    AiObjectContext* const context = botAI->GetAiObjectContext();
    TravelTarget* const target = AI_VALUE(TravelTarget*, "travel target");
    return target && target->isForced() && target->getDestination() == ownedTravelDestination &&
           target->getStatus() == TRAVEL_STATUS_TRAVEL;
}

bool DefaultPlayerbotEconomyRuntime::OwnsTravelTarget(PlayerbotAI* botAI)
{
    if (!ownedTravelDestination)
        return false;

    AiObjectContext* const context = botAI->GetAiObjectContext();
    TravelTarget* const target = AI_VALUE(TravelTarget*, "travel target");
    return target && target->isForced() && target->getDestination() == ownedTravelDestination;
}

void DefaultPlayerbotEconomyRuntime::ReleaseIdleCycleState(PlayerbotAI* botAI)
{
    // The bot is still walking somewhere this runtime sent it. Cancelling now restarts the journey
    // every cycle and the bot never arrives, so leave the trip alone and release nothing else either:
    // the state Reset would drop is what the trip is for.
    if (OwnsTripInFlight(botAI))
        return;

    Reset(botAI);
}

void DefaultPlayerbotEconomyRuntime::Reset(PlayerbotAI* botAI)
{
    Player* const bot = botAI->GetBot();
    AiObjectContext* const context = botAI->GetAiObjectContext();
    uint64 const now = GameTime::GetGameTime().count();
    if (activeGathering && activeGathering->startedAt)
    {
        uint32 const currentItemCount =
            activeGathering->plan.itemId ? bot->GetItemCount(activeGathering->plan.itemId) : 0u;
        uint32 const gatheredQuantity = currentItemCount > activeGathering->plan.startingItemCount
                                            ? currentItemCount - activeGathering->plan.startingItemCount
                                            : 0u;
        uint32 const currentSkill = bot->GetSkillValue(activeGathering->skillId);
        uint32 const skillPoints = currentSkill > activeGathering->plan.startingSkillValue
                                       ? currentSkill - activeGathering->plan.startingSkillValue
                                       : 0u;
        GetPlayerbotEconomyGathering().RecordDedicatedTrip({
            .characterGuid = bot->GetGUID().GetCounter(),
            .itemId = activeGathering->plan.itemId,
            .startedAt = activeGathering->startedAt,
            .finishedAt = now,
            .outboundSeconds = activeGathering->outboundSeconds,
            .attemptedResources = static_cast<uint32>(activeGathering->attemptedPoints.size()),
            .gatheredQuantity = gatheredQuantity,
            .skillPoints = skillPoints,
        });
        activeGathering->startedAt = 0u;
    }
    if (activeGathering && activeGathering->coordinatorLeaseId && !activeGathering->coordinatorSettled)
    {
        uint32 committedQuantity = activeGathering->committedQuantity;
        if (activeGathering->plan.itemId)
        {
            uint32 const current = bot->GetItemCount(activeGathering->plan.itemId);
            if (current > activeGathering->plan.startingItemCount)
            {
                committedQuantity = std::min(current - activeGathering->plan.startingItemCount,
                                             activeGathering->plan.requestedQuantity);
            }
        }
        EconomyAssignmentOutcome const outcome = committedQuantity ? EconomyAssignmentOutcome::InventoryReceived
                                                 : !sPlayerbotEconomyConfig.lifecycleEnabled
                                                     ? EconomyAssignmentOutcome::Disabled
                                                 : !bot->IsInWorld() ? EconomyAssignmentOutcome::LoggedOut
                                                                     : EconomyAssignmentOutcome::CapabilityLost;
        [[maybe_unused]] bool const released = GetPlayerbotEconomyCoordinator().RecordOutcome(
            activeGathering->coordinatorLeaseId, outcome, committedQuantity, now);
        activeGathering->coordinatorSettled = true;
    }

    if (activeGathering && activeGathering->killTarget &&
        AI_VALUE(ObjectGuid, "pull target") == activeGathering->killTarget)
    {
        SET_AI_VALUE(ObjectGuid, "pull target", ObjectGuid::Empty);
    }

    if (ownedTravelDestination)
    {
        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
        if (target->isForced() && target->getDestination() == ownedTravelDestination)
            EconomyTravelAction(botAI).Clear(target);

        ownedTravelDestination = nullptr;
    }
    activeGatheringPointDestination = nullptr;

    if (ownsTravelStrategy)
    {
        botAI->ChangeStrategy("-travel", BOT_STATE_NON_COMBAT);
        ownsTravelStrategy = false;
    }
    for (std::string const& strategy : suspendedIdleStrategies)
        botAI->ChangeStrategy("+" + strategy, BOT_STATE_NON_COMBAT);
    suspendedIdleStrategies.clear();

    activeGathering.reset();
    GetPlayerbotEconomyGathering().ClearActiveTrip(botAI->GetBot()->GetGUID().GetCounter());
    activeTrainer.reset();
    activeTrainerObjective.reset();
    craftFocusTravel = false;

    if (!sPlayerbotEconomyConfig.lifecycleEnabled || IsRealPlayer(botAI->GetMaster()) || !bot->IsInWorld())
    {
        EconomyAssignmentOutcome const outcome = !sPlayerbotEconomyConfig.lifecycleEnabled
                                                     ? EconomyAssignmentOutcome::Disabled
                                                 : !bot->IsInWorld() ? EconomyAssignmentOutcome::LoggedOut
                                                                     : EconomyAssignmentOutcome::CapabilityLost;
        GetPlayerbotEconomyCoordinator().InvalidateActor(bot->GetGUID().GetCounter(), outcome, now);
        pendingCraftTrace.reset();
        if (!sPlayerbotEconomyConfig.lifecycleEnabled || !bot->IsInWorld())
        {
            activeProgressionMilestone.reset();
            pendingProgressionCraft.reset();
            progressionTrainingOutputs.clear();
            activeProgressionBatchRemaining = 0u;
        }
        sRandomPlayerbotMgr.SetValue(bot, PROFESSION_WORK_ORDER_EVENT, 0u);
        EconomyActorFacts actor;
        actor.characterGuid = bot->GetGUID().GetCounter();
        actor.accountId = bot->GetSession() ? bot->GetSession()->GetAccountId() : 0u;
        actor.marketId = AuctionMarketId(bot->GetFaction());
        actor.online = bot->IsInWorld();
        actor.autonomous = false;
        GetPlayerbotEconomyCoordinator().RefreshActor(std::move(actor), now);
    }
}
}  // namespace

bool CareerStageOwnsCycle(PlayerbotEconomyCycleResult const& result)
{
    return result.outcome == PlayerbotEconomyCycleOutcome::Scheduled ||
           result.outcome == PlayerbotEconomyCycleOutcome::Operation;
}

bool ProgressionStageOwnsCycle(PlayerbotEconomyCycleResult const& progression, bool trainerStageStalled)
{
    if (!CareerStageOwnsCycle(progression))
        return false;
    if (!trainerStageStalled)
        return true;

    return progression.blocker != "profession_trainer_rank_selected" &&
           progression.blocker != "profession_trainer_recipe_selected";
}

bool ConsumptionStepOwnsCycle(EconomyExecutionResult execution)
{
    return execution != EconomyExecutionResult::Superseded && execution != EconomyExecutionResult::Failed;
}

EconomyAssignmentLease PlayerbotEconomyRuntime::AssignProduction(PlayerbotEconomyCoordinator& coordinator,
                                                                 EconomyProductionRequest request, uint64 now)
{
    return coordinator.AssignProduction(std::move(request), now);
}

EconomyProductionOutput PlayerbotEconomyRuntime::ReconcileProductionInventory(PlayerbotEconomyCoordinator& coordinator,
                                                                              uint64 leaseId, uint32 startingQuantity,
                                                                              uint32 currentQuantity, uint64 now)
{
    return coordinator.RecordProductionInventory(leaseId, startingQuantity, currentQuantity, now);
}

std::unique_ptr<PlayerbotEconomyRuntime> CreatePlayerbotEconomyRuntime()
{
    return std::make_unique<DefaultPlayerbotEconomyRuntime>();
}
