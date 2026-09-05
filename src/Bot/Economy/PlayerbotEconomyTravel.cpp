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
#include "Bot/Economy/PlayerbotEconomyPolicy.h"
#include "Bot/Economy/PlayerbotEconomyTravelPlan.h"
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

// An ordinary attackable creature: no vendor or quest giver, no elite, no critter, no trigger or
// civilian, and a loot table to empty. Faction is judged per bot when the destination is evaluated.
bool IsHuntableCreature(CreatureTemplate const& creatureTemplate)
{
    constexpr uint32 blockingUnitFlags = UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NOT_SELECTABLE;
    constexpr uint32 blockingExtraFlags = CREATURE_FLAG_EXTRA_TRIGGER | CREATURE_FLAG_EXTRA_CIVILIAN;
    return creatureTemplate.lootid && creatureTemplate.npcflag == UNIT_NPC_FLAG_NONE &&
           creatureTemplate.rank == CREATURE_ELITE_NORMAL && creatureTemplate.type != CREATURE_TYPE_CRITTER &&
           !(creatureTemplate.unit_flags & blockingUnitFlags) && !(creatureTemplate.flags_extra & blockingExtraFlags);
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

uint32 PlayerbotEconomyTravelLandmass(uint32 mapId, float x, float y)
{
    if (mapId != 530u)
        return 0u;
    // Outland proper sits entirely above y -5000 (its southernmost spawns are near y +800);
    // both isle groups sit below y -6000, split by the x axis: Quel'Thalas at positive x,
    // Azuremyst at negative x. Verified against live spawn coordinates on 2026-08-23.
    if (y >= -5000.0f)
        return 1u;
    return x >= 0.0f ? 2u : 3u;
}

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
                                                       std::vector<uint32> pointSpawnIds, SpawnProbe spawnProbe,
                                                       uint32 factionTemplateId)
    : TravelDestination(sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance),
      source(source),
      entry(entry),
      skillId(skillId),
      requiredSkill(requiredSkill),
      minimumLevel(minimumLevel),
      maximumLevel(maximumLevel),
      factionTemplateId(factionTemplateId),
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

uint64 EconomyTripDeadlineSeconds(uint32 budgetSeconds)
{
    uint64 const scaled = static_cast<uint64>(budgetSeconds) * ECONOMY_TRIP_BUDGET_MULTIPLIER;
    return std::max<uint64>(scaled, ECONOMY_TRIP_MINIMUM_SECONDS);
}

EconomyTripState EvaluateEconomyTrip(EconomyTripFacts const& facts)
{
    if (!facts.owned || !facts.forced)
        return EconomyTripState::NotOwned;
    if (!facts.travelling)
        return EconomyTripState::Arrived;
    // A lost destination is checked before the deadline so a stranded bot reports the node that went
    // away rather than sitting out a deadline that has not arrived yet.
    if (!facts.destinationActive)
        return EconomyTripState::DestinationLost;
    if (facts.elapsedSeconds > EconomyTripDeadlineSeconds(facts.budgetSeconds))
        return EconomyTripState::DeadlineExceeded;
    return EconomyTripState::InFlight;
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
    if (facts.skillRequired && (!facts.requiredSkillId || facts.requiredSkillId != facts.learnedSkillId))
        return GatheringDestinationBlocker::WrongSkill;
    if (facts.skillRequired && facts.skillValue < facts.requiredSkillValue)
        return GatheringDestinationBlocker::InsufficientSkill;
    if (!facts.levelAppropriate)
        return GatheringDestinationBlocker::WrongLevel;
    if (!facts.attackable)
        return GatheringDestinationBlocker::NotAttackable;
    if (!facts.accessible)
        return GatheringDestinationBlocker::Inaccessible;
    return GatheringDestinationBlocker::None;
}

bool GatheringTravelDestination::IsGameObjectSpawned(uint32 spawnId)
{
    if (!spawnId || !sPoolMgr->IsPartOfAPool<GameObject>(spawnId))
        return true;

    // Creature and gameobject pool spawn state lives on the owning Map's SpawnedPoolData; only quest
    // pool state is still global on PoolMgr. Gathering nodes are continent spawns, so the base
    // non-instance map of the spawn's own map id is the owner.
    GameObjectData const* data = sObjectMgr->GetGameObjectData(spawnId);
    if (!data)
        return true;

    // A continent with nobody on it is not resident. An absent map means unknown, not despawned, and
    // this class already treats an unknown spawn as spawned (see pointSpawnIds), so answering false
    // here would hide every catalog point on an empty continent from travel planning.
    Map const* map = sMapMgr->FindBaseNonInstanceMap(data->mapid);
    if (!map)
        return true;

    return map->GetPoolData().IsSpawnedObject<GameObject>(spawnId);
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

uint32 GatheringTravelDestination::CountReachablePointsOnMap(Player* bot, WorldPosition origin, uint32 maximumPoints)
{
    if (!bot || !maximumPoints)
        return 0u;

    std::vector<WorldPosition*> candidates;
    for (WorldPosition* point : points)
        if (point && point->GetMapId() == origin.GetMapId() && !point->getVisitors() && PointSpawned(point))
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
        if (!alreadyInRange && !point->isPathTo(route, REACHABLE_POINT_TOLERANCE))
            continue;
        if (++reachable >= maximumPoints)
            break;
    }
    return reachable;
}

std::vector<uint32> GatheringTravelDestination::YieldItemIds() const
{
    std::vector<uint32> items;
    items.reserve(conservativeItemYieldBasisPoints.size());
    for (auto const& [itemId, basisPoints] : conservativeItemYieldBasisPoints)
    {
        (void)basisPoints;
        items.push_back(itemId);
    }
    return items;
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

    facts.skillRequired = source != GatheringTravelSource::LootCreature;
    facts.learnedSkillId = bot->HasSkill(skillId) ? skillId : 0u;
    facts.skillValue = bot->GetSkillValue(skillId);
    facts.requiredSkillValue = requiredSkill;
    facts.sameMap = HasPointOnMap(bot->GetMapId());
    switch (source)
    {
        case GatheringTravelSource::SkinningCreature:
            facts.levelAppropriate = PlayerbotEconomy::PlayerbotEconomyGathering::IsSkinningTargetLevelSafe(
                bot->GetLevel(), maximumLevel, static_cast<uint8>(sPlayerbotAIConfig.randomBotTeleLowerLevel));
            break;
        case GatheringTravelSource::LootCreature:
        {
            facts.levelAppropriate =
                PlayerbotEconomy::PlayerbotEconomyGathering::IsHuntingTargetLevelSafe(bot->GetLevel(), maximumLevel);
            FactionTemplateEntry const* const creatureFaction = sFactionTemplateStore.LookupEntry(factionTemplateId);
            facts.attackable = creatureFaction && Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(),
                                                                             creatureFaction) < REP_FRIENDLY;
            break;
        }
        case GatheringTravelSource::HerbalismNode:
        case GatheringTravelSource::MiningNode:
            facts.levelAppropriate = true;
            break;
    }
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
        case GatheringTravelSource::LootCreature:
            title = "loot population ";
            break;
    }
    bool const creaturePopulation =
        source == GatheringTravelSource::SkinningCreature || source == GatheringTravelSource::LootCreature;
    int32 const displayEntry = creaturePopulation ? static_cast<int32>(entry) : -static_cast<int32>(entry);
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
    ConservativeLootYields const creatureLoot =
        LoadConservativeLootYields("creature_loot_template", CONDITION_SOURCE_TYPE_CREATURE_LOOT_TEMPLATE);
    using GatheringCacheKey = std::tuple<GatheringTravelSource, uint16, uint32, uint32, uint8, uint8>;
    std::map<GatheringCacheKey, std::vector<WorldPosition>> gatheringPoints;
    std::map<GatheringCacheKey, std::vector<uint32>> gatheringSpawnIds;
    std::map<GatheringCacheKey, std::map<uint32, uint32>> gatheringYields;
    std::map<GatheringCacheKey, uint32> gatheringFactions;
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

        if (creatureData.spawnMask && creatureData.phaseMask && IsHuntableCreature(*creatureTemplate))
        {
            if (auto const found = creatureLoot.find(creatureTemplate->lootid); found != creatureLoot.end())
            {
                GatheringCacheKey const key = {
                    GatheringTravelSource::LootCreature, mapId, entry, 0u, creatureTemplate->minlevel,
                    creatureTemplate->maxlevel};
                gatheringPoints[key].emplace_back(mapId, creatureData.posX, creatureData.posY, creatureData.posZ,
                                                  orientation);
                gatheringYields[key] = found->second;
                gatheringFactions[key] = creatureTemplate->faction;
            }
        }

        if (creatureTemplate->npcflag & UNIT_NPC_FLAG_TRAINER)
        {
            Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
            if (trainer && CatalogsTrainerType(trainer->GetTrainerType()))
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
                    bool const capital = zone && (zone->flags & AREA_FLAG_CAPITAL) != 0;
                    WorldPosition const position(mapId, creatureData.posX, creatureData.posY, creatureData.posZ,
                                                 orientation);
                    trainersByMap[mapId].push_back(std::make_unique<TrainerDestination>(
                        position, entry, trainer->GetTrainerType(), zoneId, minimumLevel, capital,
                        sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance));
                }
            }
        }

        if ((creatureTemplate->npcflag & UNIT_NPC_FLAG_VENDOR) && creatureData.spawnMask &&
            sObjectMgr->GetNpcVendorItemList(entry))
        {
            vendorsByMap[mapId].push_back(std::make_unique<VendorDestination>(
                WorldPosition(mapId, creatureData.posX, creatureData.posY, creatureData.posZ, orientation), entry,
                sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance));
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
                std::make_unique<SpellFocusDestination>(position, gameObjectTemplate->spellFocus.dist,
                                                        sPlayerbotAIConfig.tooCloseDistance,
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
    std::size_t mountTrainerCount = 0u;
    for (auto const& [mapId, destinations] : trainersByMap)
    {
        (void)mapId;
        trainerCount += destinations.size();
        mountTrainerCount +=
            static_cast<std::size_t>(std::count_if(destinations.begin(), destinations.end(), [](auto const& destination)
                                                   { return destination->type == Trainer::Type::Mount; }));
    }
    // A vendor standing within the hub radius of an auctioneer is a hub vendor for that faction.
    // Computed once every auctioneer of the map is known, so it runs after the spawn loop.
    // WorldPosition::distance takes a mutable pointer, hence the copy.
    auto const nearAnyAuctioneer =
        [](std::vector<std::unique_ptr<AuctioneerDestination>> const* auctioneers, WorldPosition position)
    {
        if (!auctioneers)
            return false;
        return std::any_of(auctioneers->begin(), auctioneers->end(),
                           [&position](std::unique_ptr<AuctioneerDestination> const& auctioneer)
                           { return auctioneer->position.distance(&position) <= ECONOMY_HUB_VENDOR_RADIUS_YARDS; });
    };
    std::size_t vendorCount = 0u;
    std::size_t hubVendorCount = 0u;
    for (auto& [mapId, destinations] : vendorsByMap)
    {
        auto const alliance = allianceAuctioneersByMap.find(mapId);
        auto const horde = hordeAuctioneersByMap.find(mapId);
        for (std::unique_ptr<VendorDestination>& vendor : destinations)
        {
            vendor->allianceHub = nearAnyAuctioneer(
                alliance == allianceAuctioneersByMap.end() ? nullptr : &alliance->second, vendor->position);
            vendor->hordeHub =
                nearAnyAuctioneer(horde == hordeAuctioneersByMap.end() ? nullptr : &horde->second, vendor->position);
            hubVendorCount += vendor->allianceHub || vendor->hordeHub ? 1u : 0u;
        }
        vendorCount += destinations.size();
    }
    LOG_INFO("playerbots.economy",
             "Economy travel catalog holds {} trainers ({} mount) across {} maps and {} vendors ({} in hubs) across "
             "{} maps.",
             trainerCount, mountTrainerCount, trainersByMap.size(), vendorCount, hubVendorCount, vendorsByMap.size());

    for (auto& [key, points] : gatheringPoints)
    {
        auto const& [source, mapId, entry, requiredSkill, minimumLevel, maximumLevel] = key;
        (void)mapId;
        uint32 const skillId = source == GatheringTravelSource::HerbalismNode      ? SKILL_HERBALISM
                               : source == GatheringTravelSource::MiningNode       ? SKILL_MINING
                               : source == GatheringTravelSource::SkinningCreature ? SKILL_SKINNING
                                                                                   : PlayerbotEconomy::HUNTING_SKILL_ID;
        gatheringDestinations.push_back(std::make_unique<GatheringTravelDestination>(
            source, entry, skillId, requiredSkill, minimumLevel, maximumLevel, std::move(points),
            std::move(gatheringYields[key]), std::move(gatheringSpawnIds[key]),
            GatheringTravelDestination::SpawnProbe{}, gatheringFactions[key]));
    }
}

bool PlayerbotEconomyTravelCatalog::MiningNodeYieldsItem(uint32 itemId)
{
    EnsureBuilt();
    if (miningNodeYieldItemIds.empty())
    {
        for (auto const& destination : gatheringDestinations)
        {
            if (!destination || destination->getSource() != GatheringTravelSource::MiningNode)
                continue;
            for (uint32 const yieldItemId : destination->YieldItemIds())
                miningNodeYieldItemIds.insert(yieldItemId);
        }
    }
    return miningNodeYieldItemIds.contains(itemId);
}

GatheringTravelDestination* PlayerbotEconomyTravelCatalog::FindGatheringDestination(uint32 skillId, uint32 entry,
                                                                                    uint32 mapId)
{
    EnsureBuilt();
    for (auto const& destination : gatheringDestinations)
    {
        if (destination && destination->getSkillId() == skillId &&
            destination->getEntry() == static_cast<int32>(entry) && destination->HasPointOnMap(mapId))
            return destination.get();
    }
    return nullptr;
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

namespace
{
// Nearest destination sharing the bot's landmass. On every map but 530 the landmass is
// uniform, so this degrades to plain nearest; on 530 it refuses a facility on a landmass
// the bot cannot reach, returning null so the caller reports a clean blocker instead of
// scheduling a trip no route can complete.
template <typename Destination>
Destination* NearestOnSameLandmass(std::vector<std::unique_ptr<Destination>> const& destinations, Player* bot)
{
    uint32 const botLandmass =
        PlayerbotEconomyTravelLandmass(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
    Destination* nearest = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();
    for (std::unique_ptr<Destination> const& candidate : destinations)
    {
        if (PlayerbotEconomyTravelLandmass(candidate->position.GetMapId(), candidate->position.GetPositionX(),
                                           candidate->position.GetPositionY()) != botLandmass)
        {
            continue;
        }
        float const distance = bot->GetExactDist2dSq(candidate->position);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = candidate.get();
        }
    }
    return nearest;
}
}  // namespace

TravelDestination* PlayerbotEconomyTravelCatalog::SelectAuctioneer(Player* bot)
{
    EnsureBuilt();
    if (!bot)
        return nullptr;
    auto& byMap = bot->GetTeamId() == TEAM_ALLIANCE ? allianceAuctioneersByMap : hordeAuctioneersByMap;
    auto found = byMap.find(bot->GetMapId());
    if (found == byMap.end() || found->second.empty())
        return nullptr;
    AuctioneerDestination* const nearest = NearestOnSameLandmass(found->second, bot);
    return nearest ? &nearest->destination : nullptr;
}

TravelDestination* PlayerbotEconomyTravelCatalog::SelectMailbox(Player* bot)
{
    EnsureBuilt();
    if (!bot)
        return nullptr;
    auto found = mailboxesByMap.find(bot->GetMapId());
    if (found == mailboxesByMap.end() || found->second.empty())
        return nullptr;
    MailboxDestination* const nearest = NearestOnSameLandmass(found->second, bot);
    return nearest ? &nearest->destination : nullptr;
}

PlayerbotEconomyTravelCatalog::SpellFocusDestination* PlayerbotEconomyTravelCatalog::SelectSpellFocus(
    Player* bot, uint32 spellFocusId)
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
    return NearestOnSameLandmass(found->second, bot);
}

bool PlayerbotEconomyTravelCatalog::IsTrainerRouteReachable(PlayerbotTrainerRouteFacts const& facts)
{
    return (facts.sameMap && facts.withinLocalRange) || facts.travelNodePath;
}

bool PlayerbotEconomyTravelCatalog::CatalogsTrainerType(Trainer::Type type)
{
    return type == Trainer::Type::Tradeskill || type == Trainer::Type::Mount;
}

bool PlayerbotEconomyTravelCatalog::TrainerServesObjective(Trainer::Type trainerType,
                                                           PlayerbotCareerTrainerObjectiveKind kind)
{
    return trainerType == PlayerbotCareer::ObjectiveTrainerType(kind);
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
            // The two pools share this map, so the wrong one is dropped before anything is looked up.
            if (!TrainerServesObjective(candidate->type, objective.kind))
                continue;

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
                                                           candidate->minimumLevel, candidate->capital,
                                                           candidate->position.GetMapId() == bot->GetMapId()))
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
        // Safe, eligible, affordable trainers exist but no travel path reaches any of them. This is
        // a different fact from a zone-gate refusal, and sharing the unsafe_route label with one is
        // what sent every diagnosis of the permanently unroutable professions down the wrong trail.
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::NoRoute};
    }

    // Read the blocker this returns with care: it is the reason SOME trainer was rejected, not
    // necessarily the reason the bot has none. foundUnsafe is set by ANY trainer on ANY map
    // failing IsTrainerDestinationSafe, and there are 311 mining trainer spawns across Outland
    // and Northrend, so a low level bot sets it on every cycle. It therefore wins this ordering
    // almost always and hides an ineligible or unaffordable nearby trainer behind
    // "unsafe_route". Verified 2026-08-26: bot 809 stood 75 yards from a valid Tradeskill mining
    // trainer, far inside MAX_LOCAL_TRAINER_ROUTE_DISTANCE, and still logged unsafe_route.
    //
    // Jewelcrafting is the case this misleads people into chasing, and it is NOT a defect: every
    // JC trainer spawns on map 530 or 571 and none on 0 or 1, so a pre-Outland bot genuinely has
    // no reachable JC trainer and correctly logs unsafe_route forever. See ERRORS.md.
    //
    // When judging whether trainer acquisition works, count SUCCESSES in character_skills rather
    // than refusals here: refusals are emitted every cycle and never stop, so they make a working
    // change look blocked.
    if (foundUnaffordable)
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::InsufficientProtectedMoney};
    if (foundUnsafe)
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::UnsafeRoute};
    if (foundIneligible)
        return {nullptr, 0u, PlayerbotCareerAcquisitionBlocker::TrainerIneligible};
    return {};
}

std::vector<uint32> PlayerbotEconomyTravelCatalog::ApplicableOffers(Player* bot, uint32 entry)
{
    std::vector<uint32> offers;
    CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(entry);
    VendorItemData const* items = creatureTemplate ? sObjectMgr->GetNpcVendorItemList(entry) : nullptr;
    FactionTemplateEntry const* vendorFaction =
        creatureTemplate ? sFactionTemplateStore.LookupEntry(creatureTemplate->faction) : nullptr;
    if (!bot || !items || !vendorFaction)
        return offers;

    bool const factionAllowed =
        Unit::GetFactionReactionTo(bot->GetFactionTemplateEntry(), vendorFaction) >= REP_NEUTRAL;
    for (VendorItem const* item : items->m_items)
    {
        ItemTemplate const* itemTemplate = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!itemTemplate)
            continue;
        PlayerbotEconomy::VendorOfferPolicyInput const input{
            .maximumCount = item->maxcount,
            .extendedCost = item->ExtendedCost,
            .factionAllowed = factionAllowed,
            .levelAllowed =
                bot->GetLevel() >= itemTemplate->RequiredLevel && bot->CanUseItem(itemTemplate) == EQUIP_ERR_OK,
            .reputationAllowed = !itemTemplate->RequiredReputationFaction ||
                                 static_cast<uint32>(bot->GetReputationRank(itemTemplate->RequiredReputationFaction)) >=
                                     itemTemplate->RequiredReputationRank,
            .sameMap = true,
            .routeAvailable = true,
        };
        if (PlayerbotEconomy::PlayerbotEconomyPolicy::IsApplicableUnlimitedGoldVendorOffer(input))
            offers.push_back(item->item);
    }
    return offers;
}

std::unordered_set<uint32> PlayerbotEconomyTravelCatalog::ApplicableUnlimitedGoldVendorItems(Player* bot)
{
    EnsureBuilt();
    std::unordered_set<uint32> itemIds;
    if (!bot)
        return itemIds;
    auto const vendors = vendorsByMap.find(bot->GetMapId());
    if (vendors == vendorsByMap.end())
        return itemIds;

    uint32 const botLandmass =
        PlayerbotEconomyTravelLandmass(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
    // A template spawns many times; its offers only need evaluating once per bot.
    std::unordered_set<uint32> evaluated;
    for (std::unique_ptr<VendorDestination> const& vendor : vendors->second)
    {
        if (vendor->landmass != botLandmass || !evaluated.insert(vendor->entry).second)
            continue;
        for (uint32 const itemId : ApplicableOffers(bot, vendor->entry))
            itemIds.insert(itemId);
    }
    return itemIds;
}

TravelDestination* PlayerbotEconomyTravelCatalog::SelectVendor(Player* bot, uint32 itemId, bool preferHub)
{
    EnsureBuilt();
    if (!bot || !itemId)
        return nullptr;
    auto const vendors = vendorsByMap.find(bot->GetMapId());
    if (vendors == vendorsByMap.end())
        return nullptr;

    uint32 const botLandmass =
        PlayerbotEconomyTravelLandmass(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY());
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    std::unordered_map<uint32, bool> sellsByEntry;
    WorldPosition botPosition(bot);
    VendorDestination* selected = nullptr;
    bool selectedHub = false;
    float selectedDistance = std::numeric_limits<float>::max();
    for (std::unique_ptr<VendorDestination> const& vendor : vendors->second)
    {
        if (vendor->landmass != botLandmass)
            continue;
        auto sells = sellsByEntry.find(vendor->entry);
        if (sells == sellsByEntry.end())
        {
            std::vector<uint32> const offers = ApplicableOffers(bot, vendor->entry);
            sells = sellsByEntry.emplace(vendor->entry, std::find(offers.begin(), offers.end(), itemId) != offers.end())
                        .first;
        }
        if (!sells->second)
            continue;
        bool const hub = preferHub && (alliance ? vendor->allianceHub : vendor->hordeHub);
        float const distance = vendor->position.distance(&botPosition);
        if (!selected || PrefersVendor(hub, distance, selectedHub, selectedDistance))
        {
            selected = vendor.get();
            selectedHub = hub;
            selectedDistance = distance;
        }
    }
    return selected ? &selected->destination : nullptr;
}
