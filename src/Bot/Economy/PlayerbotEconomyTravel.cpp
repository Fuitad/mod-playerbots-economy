/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotEconomyTravel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Trainer.h"

namespace
{
bool IsConfiguredMap(uint32 mapId)
{
    return std::find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(), mapId) !=
           sPlayerbotAIConfig.randomBotMaps.end();
}

bool IsLevelRangeSafe(uint8 botLevel, uint8 minimumCreatureLevel, uint8 maximumCreatureLevel)
{
    return static_cast<uint16>(botLevel) + sPlayerbotAIConfig.randomBotTeleLowerLevel >= maximumCreatureLevel &&
           botLevel <= static_cast<uint16>(minimumCreatureLevel) + sPlayerbotAIConfig.randomBotTeleHigherLevel;
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

GatheringTravelDestination::GatheringTravelDestination(GatheringTravelSource source, uint32 entry, uint32 skillId,
                                                       uint32 requiredSkill, uint8 minimumLevel, uint8 maximumLevel,
                                                       std::vector<WorldPosition> points)
    : TravelDestination(sPlayerbotAIConfig.tooCloseDistance, sPlayerbotAIConfig.sightDistance),
      source(source),
      entry(entry),
      skillId(skillId),
      requiredSkill(requiredSkill),
      minimumLevel(minimumLevel),
      maximumLevel(maximumLevel),
      ownedPoints(std::move(points))
{
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

bool GatheringTravelDestination::HasPointOnMap(uint32 mapId) const
{
    return std::any_of(points.begin(), points.end(),
                       [mapId](WorldPosition const* point) { return point && point->GetMapId() == mapId; });
}

WorldPosition* GatheringTravelDestination::NextUnvisitedPoint(WorldPosition& origin, uint32 mapId,
                                                              std::vector<WorldPosition*> const& visited) const
{
    WorldPosition* nearest = nullptr;
    for (WorldPosition* point : points)
    {
        if (!point || point->GetMapId() != mapId || std::find(visited.begin(), visited.end(), point) != visited.end())
            continue;
        if (!nearest || point->distance(&origin) < nearest->distance(&origin))
            nearest = point;
    }
    return nearest;
}

std::unique_ptr<TravelDestination> GatheringTravelDestination::MakePointDestination(WorldPosition* point)
{
    if (!point || std::find(points.begin(), points.end(), point) == points.end())
        return nullptr;
    return std::make_unique<GatheringPointTravelDestination>(this, point);
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
    facts.levelAppropriate = source != GatheringTravelSource::SkinningCreature ||
                             IsLevelRangeSafe(bot->GetLevel(), minimumLevel, maximumLevel);
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

    using GatheringCacheKey = std::tuple<GatheringTravelSource, uint16, uint32, uint32, uint8, uint8>;
    std::map<GatheringCacheKey, std::vector<WorldPosition>> gatheringPoints;
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
            gatheringPoints[{GatheringTravelSource::SkinningCreature, mapId, entry, requiredSkill,
                             creatureTemplate->minlevel, creatureTemplate->maxlevel}]
                .emplace_back(mapId, creatureData.posX, creatureData.posY, creatureData.posZ, orientation);
        }

        if (creatureTemplate->npcflag & UNIT_NPC_FLAG_TRAINER)
        {
            Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
            Map* map = sMapMgr->FindMap(mapId, 0u);
            if (trainer && trainer->GetTrainerType() == Trainer::Type::Tradeskill && map)
            {
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(
                    map->GetAreaId(PHASEMASK_NORMAL, creatureData.posX, creatureData.posY, creatureData.posZ));
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
            gatheringPoints[{source, mapId, gameObjectData.id, std::max(1u, lockInfo->Skill[index]), 0u, 0u}].push_back(
                position);
            break;
        }
    }

    for (auto& [key, points] : gatheringPoints)
    {
        auto const& [source, mapId, entry, requiredSkill, minimumLevel, maximumLevel] = key;
        (void)mapId;
        uint32 const skillId = source == GatheringTravelSource::HerbalismNode ? SKILL_HERBALISM
                               : source == GatheringTravelSource::MiningNode  ? SKILL_MINING
                                                                              : SKILL_SKINNING;
        gatheringDestinations.push_back(std::make_unique<GatheringTravelDestination>(
            source, entry, skillId, requiredSkill, minimumLevel, maximumLevel, std::move(points)));
    }
}

std::vector<GatheringTravelDestination*> PlayerbotEconomyTravelCatalog::GatheringDestinations(
    Player* bot, uint32 skillId, GatheringDestinationBlocker* blocker, bool ignoreFull, float maxDistance)
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
            selectedBlocker = candidateBlocker;
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

PlayerbotTrainerTravelSelection PlayerbotEconomyTravelCatalog::SelectTrainer(Player* bot,
                                                                             PlayerbotCareerPlan const& plan,
                                                                             uint32 availableMoney)
{
    EnsureBuilt();
    if (!bot)
        return {};

    auto found = trainersByMap.find(bot->GetMapId());
    if (found == trainersByMap.end())
        return {};

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!botFaction)
        return {};

    constexpr float SEARCH_RADIUS_SQUARED = 2500.0f * 2500.0f;
    std::vector<TrainerDestination*> candidates;
    for (auto const& candidate : found->second)
    {
        if (bot->GetExactDist2dSq(candidate->position) > SEARCH_RADIUS_SQUARED ||
            !PlayerbotCareer::IsTrainerDestinationSafe(bot->GetLevel(), bot->GetZoneId(), candidate->zoneId,
                                                       candidate->minimumLevel))
        {
            continue;
        }

        CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(candidate->entry);
        FactionTemplateEntry const* trainerFaction =
            creatureTemplate ? sFactionTemplateStore.LookupEntry(creatureTemplate->faction) : nullptr;
        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(candidate->entry);
        if (!trainerFaction || trainerFaction->IsHostileTo(*botFaction) || !trainer ||
            !PlayerbotCareer::TrainerOffersCareerLesson(
                plan, bot, trainer, bot->GetReputationPriceDiscount(trainerFaction), availableMoney))
        {
            continue;
        }

        candidates.push_back(candidate.get());
    }

    if (candidates.empty())
        return {};

    auto const nearest =
        std::min_element(candidates.begin(), candidates.end(), [bot](auto const* left, auto const* right)
                         { return bot->GetExactDist2dSq(left->position) < bot->GetExactDist2dSq(right->position); });
    return {&(*nearest)->destination, (*nearest)->entry};
}
