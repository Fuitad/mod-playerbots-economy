/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotEconomyTravel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "ChatHelper.h"
#include "ConditionMgr.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PoolMgr.h"
#include "Trainer.h"
#include "TravelNode.h"

namespace
{
constexpr float MAX_LOCAL_TRAINER_ROUTE_DISTANCE = 5000.0f;

using ConservativeLootYields = std::unordered_map<uint32, std::map<uint32, uint32>>;

using ConservativeLootRows = std::unordered_map<uint32, std::vector<ConservativeLootYieldRow>>;

std::vector<ConservativeLootYieldRow> LoadConservativeLootRows(char const* table, uint32 conditionSourceType)
{
    std::vector<ConservativeLootYieldRow> result;
    QueryResult rows = WorldDatabase.Query(
        "SELECT loot.Entry, loot.Item, loot.Reference, loot.Chance, loot.GroupId, loot.MinCount, loot.MaxCount "
        "FROM {} loot WHERE loot.QuestRequired = 0 AND loot.Chance >= 0 AND (loot.LootMode & 1) <> 0 AND NOT EXISTS ("
        "SELECT 1 FROM conditions condition_row WHERE condition_row.SourceTypeOrReferenceId = {} "
        "AND condition_row.SourceGroup = loot.Entry AND condition_row.SourceEntry = loot.Item)",
        table, conditionSourceType);
    if (!rows)
        return result;

    do
    {
        Field* fields = rows->Fetch();
        result.push_back({
            .lootId = fields[0].Get<uint32>(),
            .itemId = fields[1].Get<uint32>(),
            .referenceId = fields[2].Get<int32>(),
            .chanceBasisPoints = static_cast<uint32>(std::floor(std::min(100.0f, fields[3].Get<float>()) * 100.0f)),
            .groupId = fields[4].Get<uint8>(),
            .minimum = fields[5].Get<uint8>(),
            .maximum = fields[6].Get<uint8>(),
        });
    } while (rows->NextRow());
    return result;
}

void AddConservativeYield(std::map<uint32, uint32>& yields, uint32 itemId, uint64 yieldBasisPoints)
{
    if (!itemId || !yieldBasisPoints)
        return;
    uint32& retained = yields[itemId];
    retained = static_cast<uint32>(
        std::min<uint64>(std::numeric_limits<uint32>::max(), static_cast<uint64>(retained) + yieldBasisPoints));
}

uint64 DirectYieldBasisPoints(ConservativeLootYieldRow const& row)
{
    return static_cast<uint64>(row.chanceBasisPoints) * row.minimum;
}

std::vector<ConservativeLootYieldRow> NormalizeEqualChancedGroups(std::span<ConservativeLootYieldRow const> input)
{
    std::vector<ConservativeLootYieldRow> rows(input.begin(), input.end());
    using GroupKey = std::pair<uint32, uint32>;
    std::map<GroupKey, uint32> explicitChance;
    std::map<GroupKey, uint32> equalCount;
    for (ConservativeLootYieldRow const& row : rows)
    {
        if (!row.groupId)
            continue;
        GroupKey const key{row.lootId, row.groupId};
        if (row.chanceBasisPoints)
            explicitChance[key] = std::min(10'000u, explicitChance[key] + row.chanceBasisPoints);
        else
            ++equalCount[key];
    }
    for (ConservativeLootYieldRow& row : rows)
    {
        if (!row.groupId || row.chanceBasisPoints)
            continue;
        GroupKey const key{row.lootId, row.groupId};
        uint32 const count = equalCount[key];
        row.chanceBasisPoints = count ? (10'000u - explicitChance[key]) / count : 0u;
    }
    return rows;
}

std::map<uint32, uint32> ResolveReferenceYields(
    uint32 referenceId, ConservativeLootRows const& referenceRows,
    std::unordered_map<uint32, std::map<uint32, uint32>>& resolvedReferences,
    std::unordered_set<uint32>& resolvingReferences)
{
    if (auto const cached = resolvedReferences.find(referenceId); cached != resolvedReferences.end())
        return cached->second;
    if (!referenceId || !resolvingReferences.insert(referenceId).second)
        return {};

    std::map<uint32, uint32> resolved;
    if (auto const found = referenceRows.find(referenceId); found != referenceRows.end())
    {
        for (ConservativeLootYieldRow const& row : found->second)
        {
            if (!row.referenceId)
            {
                AddConservativeYield(resolved, row.itemId, DirectYieldBasisPoints(row));
                continue;
            }

            uint32 const nestedReferenceId = static_cast<uint32>(std::abs(row.referenceId));
            std::map<uint32, uint32> const nested =
                ResolveReferenceYields(nestedReferenceId, referenceRows, resolvedReferences, resolvingReferences);
            for (auto const& [itemId, childYieldBasisPoints] : nested)
            {
                uint64 const scaled =
                    static_cast<uint64>(childYieldBasisPoints) * row.chanceBasisPoints * row.maximum / 10'000u;
                AddConservativeYield(resolved, itemId, scaled);
            }
        }
    }
    resolvingReferences.erase(referenceId);
    resolvedReferences.emplace(referenceId, resolved);
    return resolved;
}

ConservativeLootYields LoadConservativeLootYields(char const* table, uint32 conditionSourceType)
{
    std::vector<ConservativeLootYieldRow> const sourceRows = LoadConservativeLootRows(table, conditionSourceType);
    std::vector<ConservativeLootYieldRow> const referenceRows =
        LoadConservativeLootRows("reference_loot_template", CONDITION_SOURCE_TYPE_REFERENCE_LOOT_TEMPLATE);
    return ResolveConservativeLootYields(sourceRows, referenceRows);
}

bool IsConfiguredMap(uint32 mapId)
{
    return std::find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(), mapId) !=
           sPlayerbotAIConfig.randomBotMaps.end();
}

class GatheringPointTravelDestination final : public TravelDestination
{
public:
    // TravelTarget tests arrival against every point owned by its destination. A one-point view makes a pooled
    // resource rotation require movement to the selected spawn instead of accepting the previously visited point.
    GatheringPointTravelDestination(GatheringTravelDestination* parent, WorldPosition* point)
        : TravelDestination({point}, parent->getRadiusMin(), sPlayerbotAIConfig.sightDistance), parent(parent)
    {
        setExpireDelay(parent->getExpireDelay());
        setCooldownDelay(parent->getCooldownDelay());
        setMaxVisitors(1u, 1u);
    }

    bool isActive(Player* bot) override { return parent->isActive(bot); }
    std::string const getName() override { return "GatheringPointTravelDestination"; }
    int32 getEntry() override { return parent->getEntry(); }
    std::string const getTitle() override { return parent->getTitle(); }

private:
    GatheringTravelDestination* parent;
};
}  // namespace

ConservativeLootYields ResolveConservativeLootYields(std::span<ConservativeLootYieldRow const> sourceRows,
                                                     std::span<ConservativeLootYieldRow const> referenceRows)
{
    ConservativeLootYields result;
    ConservativeLootRows sourceRowsById;
    ConservativeLootRows referenceRowsById;
    std::vector<ConservativeLootYieldRow> const normalizedSourceRows = NormalizeEqualChancedGroups(sourceRows);
    std::vector<ConservativeLootYieldRow> const normalizedReferenceRows = NormalizeEqualChancedGroups(referenceRows);
    for (ConservativeLootYieldRow const& row : normalizedSourceRows)
        sourceRowsById[row.lootId].push_back(row);
    for (ConservativeLootYieldRow const& row : normalizedReferenceRows)
        referenceRowsById[row.lootId].push_back(row);

    std::unordered_map<uint32, std::map<uint32, uint32>> resolvedReferences;
    std::unordered_set<uint32> resolvingReferences;
    for (auto const& [lootId, rows] : sourceRowsById)
    {
        for (ConservativeLootYieldRow const& row : rows)
        {
            if (!row.referenceId)
            {
                AddConservativeYield(result[lootId], row.itemId, DirectYieldBasisPoints(row));
                continue;
            }

            uint32 const referenceId = static_cast<uint32>(std::abs(row.referenceId));
            std::map<uint32, uint32> const referenced =
                ResolveReferenceYields(referenceId, referenceRowsById, resolvedReferences, resolvingReferences);
            for (auto const& [itemId, childYieldBasisPoints] : referenced)
            {
                uint64 const scaled =
                    static_cast<uint64>(childYieldBasisPoints) * row.chanceBasisPoints * row.maximum / 10'000u;
                AddConservativeYield(result[lootId], itemId, scaled);
            }
        }
    }
    return result;
}

GatheringTravelDestination::GatheringTravelDestination(GatheringTravelSource source, uint32 entry, uint32 skillId,
                                                       uint32 requiredSkill, uint8 minimumLevel, uint8 maximumLevel,
                                                       std::vector<WorldPosition> points,
                                                       std::map<uint32, uint32> conservativeItemYieldBasisPoints,
                                                       std::vector<uint32> pointSpawnIds, SpawnProbe spawnProbe)
    : TravelDestination(sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance),
      source(source),
      entry(entry),
      skillId(skillId),
      requiredSkill(requiredSkill),
      minimumLevel(minimumLevel),
      maximumLevel(maximumLevel),
      ownedPoints(std::move(points)),
      pointSpawnIds(std::move(pointSpawnIds)),
      spawnProbe(spawnProbe ? std::move(spawnProbe) : SpawnProbe(&GatheringTravelDestination::IsGameObjectSpawned)),
      conservativeItemYieldBasisPoints(std::move(conservativeItemYieldBasisPoints))
{
    if (this->pointSpawnIds.size() != ownedPoints.size())
        this->pointSpawnIds.clear();
    for (WorldPosition& point : ownedPoints)
        addPoint(&point);
    setExpireDelay(5u * 60u * 1000u);
    setCooldownDelay(60u * 1000u);
    setMaxVisitors(static_cast<uint32>(ownedPoints.size()), 1u);
}

GatheringDestinationBlocker GatheringTravelDestination::Evaluate(GatheringDestinationFacts const& facts)
{
    if (!facts.hasPoints)
        return GatheringDestinationBlocker::Empty;
    if (facts.full)
        return GatheringDestinationBlocker::Full;
    if (facts.expired)
        return GatheringDestinationBlocker::Expired;
    if (facts.coolingDown)
        return GatheringDestinationBlocker::Cooldown;
    if (!facts.sameMap)
        return GatheringDestinationBlocker::WrongMap;
    if (!facts.requiredSkillId || facts.requiredSkillId != facts.learnedSkillId)
        return GatheringDestinationBlocker::WrongSkill;
    if (facts.skillValue < facts.requiredSkillValue)
        return GatheringDestinationBlocker::InsufficientSkill;
    if (!facts.levelAppropriate)
        return GatheringDestinationBlocker::WrongLevel;
    if (!facts.accessible)
        return GatheringDestinationBlocker::Inaccessible;
    return GatheringDestinationBlocker::None;
}

bool GatheringTravelDestination::IsGameObjectSpawned(uint32 spawnId)
{
    if (!spawnId || !sPoolMgr->IsPartOfAPool<GameObject>(spawnId))
        return true;
    return sPoolMgr->IsSpawnedObject<GameObject>(spawnId);
}

bool GatheringTravelDestination::PointSpawned(WorldPosition const* point) const
{
    if (pointSpawnIds.empty() || !point)
        return true;
    auto const index = static_cast<size_t>(point - ownedPoints.data());
    return index >= pointSpawnIds.size() || spawnProbe(pointSpawnIds[index]);
}

bool GatheringTravelDestination::HasPointOnMap(uint32 mapId) const
{
    return std::any_of(points.begin(), points.end(),
                       [mapId](WorldPosition const* point) { return point && point->GetMapId() == mapId; });
}

uint32 GatheringTravelDestination::CountAvailablePointsOnMap(uint32 mapId) const
{
    return static_cast<uint32>(
        std::count_if(points.begin(), points.end(), [this, mapId](WorldPosition* point)
                      { return point && point->GetMapId() == mapId && !point->getVisitors() && PointSpawned(point); }));
}

uint32 GatheringTravelDestination::CountReachablePointsOnMap(Player* bot, uint32 maximumPoints)
{
    if (!bot || !maximumPoints)
        return 0u;

    WorldPosition origin(bot);
    std::vector<WorldPosition*> candidates;
    for (WorldPosition* point : points)
        if (point && point->GetMapId() == bot->GetMapId() && !point->getVisitors() && PointSpawned(point))
            candidates.push_back(point);
    std::sort(candidates.begin(), candidates.end(),
              [&origin](WorldPosition* left, WorldPosition* right)
              {
                  float const leftDistance = left->distance(&origin);
                  float const rightDistance = right->distance(&origin);
                  if (!std::isfinite(leftDistance))
                      return false;
                  return !std::isfinite(rightDistance) || leftDistance < rightDistance;
              });

    uint32 reachable = 0u;
    for (WorldPosition* point : candidates)
    {
        float const directDistance = origin.distance(point);
        if (!std::isfinite(directDistance) || directDistance < 0.0f)
            continue;
        bool const alreadyInRange = directDistance <= getRadiusMin();
        std::vector<WorldPosition> const route =
            alreadyInRange ? std::vector<WorldPosition>{} : origin.getPathTo(*point, bot);
        if (!alreadyInRange && !point->isPathTo(route))
            continue;
        if (++reachable >= maximumPoints)
            break;
    }
    return reachable;
}

uint32 GatheringTravelDestination::ConservativeYieldBasisPoints(uint32 itemId) const
{
    auto const found = conservativeItemYieldBasisPoints.find(itemId);
    return found == conservativeItemYieldBasisPoints.end() ? 0u : found->second;
}

WorldPosition* GatheringTravelDestination::NextUnvisitedPoint(WorldPosition& origin, uint32 mapId,
                                                              std::vector<WorldPosition*> const& visited) const
{
    WorldPosition* nearest = nullptr;
    for (WorldPosition* point : points)
    {
        if (!point || point->GetMapId() != mapId || point->getVisitors() || !PointSpawned(point) ||
            std::find(visited.begin(), visited.end(), point) != visited.end())
            continue;
        if (!nearest || point->distance(&origin) < nearest->distance(&origin))
            nearest = point;
    }
    return nearest;
}

TravelDestination* GatheringTravelDestination::PointDestination(WorldPosition* point)
{
    auto const found = std::find(points.begin(), points.end(), point);
    if (!point || found == points.end())
        return nullptr;

    size_t const index = static_cast<size_t>(std::distance(points.begin(), found));
    if (pointDestinations.size() < points.size())
        pointDestinations.resize(points.size());
    std::unique_ptr<TravelDestination>& pointDestination = pointDestinations[index];
    if (!pointDestination)
        pointDestination = std::make_unique<GatheringPointTravelDestination>(this, point);
    return pointDestination.get();
}

GatheringDestinationBlocker GatheringTravelDestination::GetBlocker(Player* bot, bool full)
{
    GatheringDestinationFacts facts;
    facts.hasPoints = !points.empty();
    facts.full = full;
    facts.requiredSkillId = skillId;
    if (!bot)
        return Evaluate(facts);

    facts.learnedSkillId = bot->HasSkill(skillId) ? skillId : 0u;
    facts.skillValue = bot->GetSkillValue(skillId);
    facts.requiredSkillValue = requiredSkill;
    facts.sameMap = HasPointOnMap(bot->GetMapId());
    facts.levelAppropriate =
        source != GatheringTravelSource::SkinningCreature ||
        PlayerbotEconomy::PlayerbotEconomyGathering::IsSkinningTargetLevelSafe(
            bot->GetLevel(), maximumLevel, static_cast<uint8>(sPlayerbotAIConfig.randomBotTeleLowerLevel));
    // TravelTarget owns long range routing and path failure. A direct navmesh preflight here treats
    // unloaded remote tiles as unreachable and prevents the travel system from constructing a route.
    facts.accessible = facts.sameMap;
    return Evaluate(facts);
}

bool GatheringTravelDestination::isActive(Player* bot) { return GetBlocker(bot) == GatheringDestinationBlocker::None; }

std::string const GatheringTravelDestination::getTitle()
{
    std::string title;
    switch (source)
    {
        case GatheringTravelSource::HerbalismNode:
            title = "herbalism node ";
            break;
        case GatheringTravelSource::MiningNode:
            title = "mining node ";
            break;
        case GatheringTravelSource::SkinningCreature:
            title = "skinning population ";
            break;
    }
    int32 const displayEntry =
        source == GatheringTravelSource::SkinningCreature ? static_cast<int32>(entry) : -static_cast<int32>(entry);
    return title + ChatHelper::FormatWorldEntry(displayEntry);
}

PlayerbotEconomyTravelCatalog& PlayerbotEconomyTravelCatalog::instance()
{
    static PlayerbotEconomyTravelCatalog catalog;
    return catalog;
}

void PlayerbotEconomyTravelCatalog::EnsureBuilt()
{
    if (built)
        return;
    built = true;

    ConservativeLootYields const gameObjectLoot =
        LoadConservativeLootYields("gameobject_loot_template", CONDITION_SOURCE_TYPE_GAMEOBJECT_LOOT_TEMPLATE);
    ConservativeLootYields const skinningLoot =
        LoadConservativeLootYields("skinning_loot_template", CONDITION_SOURCE_TYPE_SKINNING_LOOT_TEMPLATE);
    using GatheringCacheKey = std::tuple<GatheringTravelSource, uint16, uint32, uint32, uint8, uint8>;
    std::map<GatheringCacheKey, std::vector<WorldPosition>> gatheringPoints;
    std::map<GatheringCacheKey, std::vector<uint32>> gatheringSpawnIds;
    std::map<GatheringCacheKey, std::map<uint32, uint32>> gatheringYields;
    for (auto const& [guid, creatureData] : sObjectMgr->GetAllCreatureData())
    {
        (void)guid;
        CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(creatureData.id);
        if (!creatureTemplate || !IsConfiguredMap(creatureData.mapid))
            continue;

        uint16 const mapId = creatureData.mapid;
        uint32 const entry = creatureData.id;
        float const orientation = creatureData.orientation;
        if (creatureData.spawnMask && creatureData.phaseMask && creatureTemplate->SkinLootId &&
            creatureTemplate->GetRequiredLootSkill() == SKILL_SKINNING)
        {
            uint32 const requiredSkill = creatureTemplate->maxlevel < 10u   ? 1u
                                         : creatureTemplate->maxlevel < 20u ? (creatureTemplate->maxlevel - 10u) * 10u
                                                                            : creatureTemplate->maxlevel * 5u;
            GatheringCacheKey const key = {GatheringTravelSource::SkinningCreature,
                                           mapId,
                                           entry,
                                           requiredSkill,
                                           creatureTemplate->minlevel,
                                           creatureTemplate->maxlevel};
            gatheringPoints[key].emplace_back(mapId, creatureData.posX, creatureData.posY, creatureData.posZ,
                                              orientation);
            if (auto const found = skinningLoot.find(creatureTemplate->SkinLootId); found != skinningLoot.end())
                gatheringYields[key] = found->second;
        }

        if (creatureTemplate->npcflag & UNIT_NPC_FLAG_TRAINER)
        {
            Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
            if (trainer && trainer->GetTrainerType() == Trainer::Type::Tradeskill)
            {
                // FindMap only answers for a map that is already loaded, and this catalog is built once
                // on first use, while bots are still logging in. Asking it dropped every trainer on a
                // continent nobody had entered yet. MapMgr resolves the area from the base map instead.
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(sMapMgr->GetAreaId(
                    PHASEMASK_NORMAL, mapId, creatureData.posX, creatureData.posY, creatureData.posZ));
                if (area)
                {
                    uint32 const zoneId = area->zone ? area->zone : area->ID;
                    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
                    uint32 const minimumLevel = zone && zone->area_level > 0 ? zone->area_level : 0u;
                    WorldPosition const position(mapId, creatureData.posX, creatureData.posY, creatureData.posZ,
                                                 orientation);
                    trainersByMap[mapId].push_back(std::make_unique<TrainerDestination>(
                        position, entry, zoneId, minimumLevel, sPlayerbotAIConfig.tooCloseDistance,
                        sPlayerbotAIConfig.sightDistance));
                }
            }
        }

        if (!(creatureTemplate->npcflag & UNIT_NPC_FLAG_AUCTIONEER))
            continue;
        FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(creatureTemplate->faction);
        if (!factionEntry)
            continue;
        WorldPosition const position(mapId, creatureData.posX + std::cos(orientation) * 5.0f,
                                     creatureData.posY + std::sin(orientation) * 5.0f, creatureData.posZ + 0.5f,
                                     orientation + static_cast<float>(M_PI));
        if (!(factionEntry->hostileMask & 4))
            hordeAuctioneersByMap[mapId].push_back(std::make_unique<AuctioneerDestination>(
                position, entry, sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance));
        if (!(factionEntry->hostileMask & 2))
            allianceAuctioneersByMap[mapId].push_back(std::make_unique<AuctioneerDestination>(
                position, entry, sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance));
    }

    for (auto const& [guid, gameObjectData] : sObjectMgr->GetAllGOData())
    {
        (void)guid;
        GameObjectTemplate const* gameObjectTemplate = sObjectMgr->GetGameObjectTemplate(gameObjectData.id);
        if (!gameObjectTemplate || !IsConfiguredMap(gameObjectData.mapid))
            continue;
        uint16 const mapId = gameObjectData.mapid;
        WorldPosition const position(mapId, gameObjectData.posX, gameObjectData.posY, gameObjectData.posZ,
                                     gameObjectData.orientation);
        if (gameObjectTemplate->type == GAMEOBJECT_TYPE_MAILBOX)
            mailboxesByMap[mapId].push_back(std::make_unique<MailboxDestination>(
                position, sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance));
        if (gameObjectTemplate->type == GAMEOBJECT_TYPE_SPELL_FOCUS && gameObjectData.spawnMask &&
            gameObjectTemplate->spellFocus.focusId)
        {
            spellFocusByMap[gameObjectTemplate->spellFocus.focusId][mapId].push_back(
                std::make_unique<SpellFocusDestination>(position, sPlayerbotAIConfig.tooCloseDistance,
                                                        sPlayerbotAIConfig.sightDistance));
        }

        if (!gameObjectData.spawnMask || !gameObjectData.phaseMask ||
            gameObjectTemplate->type != GAMEOBJECT_TYPE_CHEST || !gameObjectTemplate->GetLootId())
            continue;
        LockEntry const* lockInfo = sLockStore.LookupEntry(gameObjectTemplate->GetLockId());
        if (!lockInfo)
            continue;
        for (uint8 index = 0u; index < 8u; ++index)
        {
            if (lockInfo->Type[index] != LOCK_KEY_SKILL)
                continue;
            uint32 const skillId = SkillByLockType(static_cast<LockType>(lockInfo->Index[index]));
            GatheringTravelSource source;
            if (skillId == SKILL_HERBALISM)
                source = GatheringTravelSource::HerbalismNode;
            else if (skillId == SKILL_MINING)
                source = GatheringTravelSource::MiningNode;
            else
                continue;
            GatheringCacheKey const key = {source, mapId, gameObjectData.id, std::max(1u, lockInfo->Skill[index]),
                                           0u,     0u};
            gatheringPoints[key].push_back(position);
            gatheringSpawnIds[key].push_back(guid);
            if (auto const found = gameObjectLoot.find(gameObjectTemplate->GetLootId()); found != gameObjectLoot.end())
                gatheringYields[key] = found->second;
            break;
        }
    }

    std::size_t trainerCount = 0u;
    for (auto const& [mapId, destinations] : trainersByMap)
    {
        (void)mapId;
        trainerCount += destinations.size();
    }
    LOG_INFO("playerbots.economy", "Economy travel catalog holds {} tradeskill trainers across {} maps.", trainerCount,
             trainersByMap.size());

    for (auto& [key, points] : gatheringPoints)
    {
        auto const& [source, mapId, entry, requiredSkill, minimumLevel, maximumLevel] = key;
        (void)mapId;
        uint32 const skillId = source == GatheringTravelSource::HerbalismNode ? SKILL_HERBALISM
                               : source == GatheringTravelSource::MiningNode  ? SKILL_MINING
                                                                              : SKILL_SKINNING;
        gatheringDestinations.push_back(std::make_unique<GatheringTravelDestination>(
            source, entry, skillId, requiredSkill, minimumLevel, maximumLevel, std::move(points),
            std::move(gatheringYields[key]), std::move(gatheringSpawnIds[key])));
    }
}

std::vector<GatheringTravelDestination*> PlayerbotEconomyTravelCatalog::GatheringDestinations(
    Player* bot, uint32 skillId, GatheringDestinationBlocker* blocker, bool ignoreFull, float maxDistance,
    uint32 itemId)
{
    EnsureBuilt();
    std::vector<GatheringTravelDestination*> destinations;
    GatheringDestinationBlocker selectedBlocker = GatheringDestinationBlocker::Empty;
    if (!bot)
    {
        if (blocker)
            *blocker = selectedBlocker;
        return destinations;
    }

    WorldPosition botLocation(bot);
    bool matchingSkill = false;
    for (auto const& destination : gatheringDestinations)
    {
        if (destination->getSkillId() != skillId)
            continue;
        matchingSkill = true;
        if (itemId && !destination->ConservativeYieldBasisPoints(itemId))
            continue;
        if (maxDistance > 0.0f && destination->HasPointOnMap(bot->GetMapId()) &&
            destination->distanceTo(&botLocation) > maxDistance)
        {
            selectedBlocker = GatheringDestinationBlocker::Inaccessible;
            continue;
        }
        GatheringDestinationBlocker const candidateBlocker =
            destination->GetBlocker(bot, destination->isFull(ignoreFull));
        if (candidateBlocker != GatheringDestinationBlocker::None)
        {
            // Destinations on other continents come last and would otherwise relabel every real
            // reason as wrong_map. Keep the verdict from the bot's own map once there is one.
            if (candidateBlocker != GatheringDestinationBlocker::WrongMap ||
                selectedBlocker == GatheringDestinationBlocker::Empty)
            {
                selectedBlocker = candidateBlocker;
            }
            continue;
        }
        destinations.push_back(destination.get());
    }
    if (!matchingSkill)
        selectedBlocker = GatheringDestinationBlocker::WrongSkill;
    else if (!destinations.empty())
        selectedBlocker = GatheringDestinationBlocker::None;
    if (blocker)
        *blocker = selectedBlocker;
    return destinations;
}

TravelDestination* PlayerbotEconomyTravelCatalog::SelectAuctioneer(Player* bot)
{
    EnsureBuilt();
    if (!bot)
        return nullptr;
    auto& byMap = bot->GetTeamId() == TEAM_ALLIANCE ? allianceAuctioneersByMap : hordeAuctioneersByMap;
    auto found = byMap.find(bot->GetMapId());
    if (found == byMap.end() || found->second.empty())
        return nullptr;
    auto nearest =
        std::min_element(found->second.begin(), found->second.end(), [bot](auto const& left, auto const& right)
                         { return bot->GetExactDist2dSq(left->position) < bot->GetExactDist2dSq(right->position); });
    return &(*nearest)->destination;
}

TravelDestination* PlayerbotEconomyTravelCatalog::SelectMailbox(Player* bot)
{
    EnsureBuilt();
    if (!bot)
        return nullptr;
    auto found = mailboxesByMap.find(bot->GetMapId());
    if (found == mailboxesByMap.end() || found->second.empty())
        return nullptr;
    auto nearest =
        std::min_element(found->second.begin(), found->second.end(), [bot](auto const& left, auto const& right)
                         { return bot->GetExactDist2dSq(left->position) < bot->GetExactDist2dSq(right->position); });
    return &(*nearest)->destination;
}

TravelDestination* PlayerbotEconomyTravelCatalog::SelectSpellFocus(Player* bot, uint32 spellFocusId)
{
    EnsureBuilt();
    if (!bot)
        return nullptr;
    auto const byFocus = spellFocusByMap.find(spellFocusId);
    if (byFocus == spellFocusByMap.end())
        return nullptr;
    auto const found = byFocus->second.find(bot->GetMapId());
    if (found == byFocus->second.end() || found->second.empty())
        return nullptr;
    auto nearest =
        std::min_element(found->second.begin(), found->second.end(), [bot](auto const& left, auto const& right)
                         { return bot->GetExactDist2dSq(left->position) < bot->GetExactDist2dSq(right->position); });
    return &(*nearest)->destination;
}

bool PlayerbotEconomyTravelCatalog::IsTrainerRouteReachable(PlayerbotTrainerRouteFacts const& facts)
{
    return (facts.sameMap && facts.withinLocalRange) || facts.travelNodePath;
}

PlayerbotTrainerTravelSelection PlayerbotEconomyTravelCatalog::SelectTrainer(
    Player* bot, PlayerbotCareerTrainerObjective const& objective, uint32 availableMoney)
{
    EnsureBuilt();
    if (!bot)
        return {};

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!botFaction)
        return {};

    std::vector<TrainerDestination*> candidates;
    bool foundIneligible = false;
    bool foundUnsafe = false;
    bool foundUnaffordable = false;
    for (auto const& [mapId, destinations] : trainersByMap)
    {
        (void)mapId;
        for (auto const& candidate : destinations)
        {
            CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(candidate->entry);
            FactionTemplateEntry const* trainerFaction =
                creatureTemplate ? sFactionTemplateStore.LookupEntry(creatureTemplate->faction) : nullptr;
            Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(candidate->entry);
            if (!trainerFaction || !trainer)
                continue;

            float const discount = bot->GetReputationPriceDiscount(trainerFaction);
            if (trainerFaction->IsHostileTo(*botFaction) ||
                !PlayerbotCareer::TrainerOffersCareerLesson(objective, bot, trainer, discount,
                                                            std::numeric_limits<uint32>::max()))
            {
                foundIneligible = true;
                continue;
            }
            if (!PlayerbotCareer::IsTrainerDestinationSafe(bot->GetLevel(), bot->GetZoneId(), candidate->zoneId,
                                                           candidate->minimumLevel))
            {
                foundUnsafe = true;
                continue;
            }
            if (!PlayerbotCareer::TrainerOffersCareerLesson(objective, bot, trainer, discount, availableMoney))
            {
                foundUnaffordable = true;
                continue;
            }
            candidates.push_back(candidate.get());
        }
    }

    if (!candidates.empty())
    {
        std::sort(candidates.begin(), candidates.end(),
                  [bot](auto const* left, auto const* right)
                  {
                      bool const leftOnMap = left->position.GetMapId() == bot->GetMapId();
                      bool const rightOnMap = right->position.GetMapId() == bot->GetMapId();
                      if (leftOnMap != rightOnMap)
                          return leftOnMap;
                      if (leftOnMap)
                          return bot->GetExactDist2dSq(left->position) < bot->GetExactDist2dSq(right->position);
                      return std::make_tuple(left->position.GetMapId(), left->entry) <
                             std::make_tuple(right->position.GetMapId(), right->entry);
                  });
        for (TrainerDestination* candidate : candidates)
        {
            WorldPosition botPosition(bot);
            PlayerbotTrainerRouteFacts route = {
                .sameMap = candidate->position.GetMapId() == bot->GetMapId(),
            };
            route.withinLocalRange =
                route.sameMap && botPosition.distance(candidate->position) <= MAX_LOCAL_TRAINER_ROUTE_DISTANCE;
            if (!route.withinLocalRange)
                route.travelNodePath = !TravelNodeMap::getFullPath(botPosition, candidate->position, bot).empty();
            if (IsTrainerRouteReachable(route))
                return {&candidate->destination, candidate->entry, PlayerbotCareerAcquisitionBlocker::None};
        }
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::UnsafeRoute};
    }

    if (foundUnaffordable)
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::InsufficientProtectedMoney};
    if (foundUnsafe)
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::UnsafeRoute};
    if (foundIneligible)
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::TrainerIneligible};
    return {};
}
