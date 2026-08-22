/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYTRAVEL_H
#define PLAYERBOTS_PLAYERBOTECONOMYTRAVEL_H

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "TravelMgr.h"

struct PlayerbotTrainerTravelSelection
{
    TravelDestination* destination = nullptr;
    uint32 entry = 0;
    PlayerbotCareerAcquisitionBlocker blocker = PlayerbotCareerAcquisitionBlocker::TrainerUnavailable;
};

struct PlayerbotTrainerRouteFacts
{
    bool sameMap = false;
    bool withinLocalRange = false;
    bool travelNodePath = false;
};

struct ConservativeLootYieldRow
{
    uint32 lootId = 0u;
    uint32 itemId = 0u;
    int32 referenceId = 0;
    uint32 chanceBasisPoints = 0u;
    uint32 groupId = 0u;
    uint32 minimum = 0u;
    uint32 maximum = 0u;
};

[[nodiscard]] std::unordered_map<uint32, std::map<uint32, uint32>> ResolveConservativeLootYields(
    std::span<ConservativeLootYieldRow const> sourceRows, std::span<ConservativeLootYieldRow const> referenceRows);

// How close a navmesh route must end to a spawn point for the point to count as reachable. The
// Playerbot default (AiPlayerbot.TargetPosRecalcDistance) can be set far below a yard, at which
// every creature spawn looks unreachable because the route ends on the navmesh surface beside it.
inline constexpr float REACHABLE_POINT_TOLERANCE = 5.0f;

enum class GatheringTravelSource : uint8
{
    HerbalismNode,
    MiningNode,
    SkinningCreature,
    // A creature population whose ordinary loot yields the item; the bot kills and loots, no skill involved.
    LootCreature
};

enum class GatheringDestinationBlocker : uint8
{
    None,
    Empty,
    Full,
    Expired,
    Cooldown,
    WrongMap,
    WrongSkill,
    InsufficientSkill,
    WrongLevel,
    Inaccessible,
    // A loot population the bot's faction cannot attack.
    NotAttackable
};

struct GatheringDestinationFacts
{
    bool hasPoints = false;
    bool full = false;
    bool expired = false;
    bool coolingDown = false;
    bool sameMap = false;
    // Herb, ore and skinning sources need a learned skill; a loot population needs none.
    bool skillRequired = true;
    bool attackable = true;
    uint32 requiredSkillId = 0;
    uint32 learnedSkillId = 0;
    uint32 skillValue = 0;
    uint32 requiredSkillValue = 0;
    bool levelAppropriate = false;
    bool accessible = false;
};

class GatheringTravelDestination : public TravelDestination
{
public:
    // Answers whether the game object spawned at a point is currently up. Herb and ore nodes are pool
    // members, so most catalog points are empty at any moment; the probe keeps bots off those.
    using SpawnProbe = std::function<bool(uint32 spawnId)>;

    // pointSpawnIds runs parallel to points (empty means unknown, treated as spawned). A default-constructed
    // spawnProbe consults the pool manager through IsGameObjectSpawned. factionTemplateId is the creature
    // faction of a LootCreature population; the bot's reaction to it decides whether the population is
    // attackable.
    GatheringTravelDestination(GatheringTravelSource source, uint32 entry, uint32 skillId, uint32 requiredSkill,
                               uint8 minimumLevel, uint8 maximumLevel, std::vector<WorldPosition> points,
                               std::map<uint32, uint32> conservativeItemYieldBasisPoints = {},
                               std::vector<uint32> pointSpawnIds = {}, SpawnProbe spawnProbe = {},
                               uint32 factionTemplateId = 0u);

    // True when the spawn is not a pool member, or is the pool member currently spawned.
    [[nodiscard]] static bool IsGameObjectSpawned(uint32 spawnId);

    bool isActive(Player* bot) override;
    std::string const getName() override { return "GatheringTravelDestination"; }
    int32 getEntry() override { return static_cast<int32>(entry); }
    std::string const getTitle() override;
    [[nodiscard]] uint32 getSkillId() const { return skillId; }
    [[nodiscard]] GatheringTravelSource getSource() const { return source; }
    [[nodiscard]] bool HasPointOnMap(uint32 mapId) const;
    [[nodiscard]] uint32 CountAvailablePointsOnMap(uint32 mapId) const;
    // Points reachable from origin by a direct navmesh route, up to maximumPoints. origin is the point
    // the bot will arrive at, not where it stands now: the long walk there is the travel target's job.
    [[nodiscard]] uint32 CountReachablePointsOnMap(Player* bot, WorldPosition origin, uint32 maximumPoints);
    [[nodiscard]] uint32 ConservativeYieldBasisPoints(uint32 itemId) const;
    // Item ids this node type can yield (keys of the conservative loot yields).
    [[nodiscard]] std::vector<uint32> YieldItemIds() const;
    [[nodiscard]] WorldPosition* NextUnvisitedPoint(WorldPosition& origin, uint32 mapId,
                                                    std::vector<WorldPosition*> const& visited) const;
    // One-point view of this destination, owned here for the catalog's lifetime. Group members copy a groupmate's
    // raw TravelDestination pointer (ChooseTravelTargetAction::SetGroupTarget), so a per-trip owner would leave
    // their TravelTarget dangling once the trip ends. Returns nullptr for a point this destination does not own.
    [[nodiscard]] TravelDestination* PointDestination(WorldPosition* point);
    [[nodiscard]] GatheringDestinationBlocker GetBlocker(Player* bot, bool full = false);
    [[nodiscard]] static GatheringDestinationBlocker Evaluate(GatheringDestinationFacts const& facts);

private:
    GatheringTravelSource source;
    uint32 entry;
    uint32 skillId;
    uint32 requiredSkill;
    uint8 minimumLevel;
    uint8 maximumLevel;
    uint32 factionTemplateId;
    [[nodiscard]] bool PointSpawned(WorldPosition const* point) const;

    std::vector<WorldPosition> ownedPoints;
    std::vector<uint32> pointSpawnIds;
    SpawnProbe spawnProbe;
    std::vector<std::unique_ptr<TravelDestination>> pointDestinations;
    std::map<uint32, uint32> conservativeItemYieldBasisPoints;
};

class PlayerbotEconomyTravelCatalog
{
public:
    static PlayerbotEconomyTravelCatalog& instance();
    std::vector<GatheringTravelDestination*> GatheringDestinations(Player* bot, uint32 skillId,
                                                                   GatheringDestinationBlocker* blocker = nullptr,
                                                                   bool ignoreFull = false, float maxDistance = 5000.0f,
                                                                   uint32 itemId = 0u);
    // The herb, ore, skinning or loot destination for a game object or creature entry on a map, or nullptr.
    // Loot populations carry HUNTING_SKILL_ID.
    GatheringTravelDestination* FindGatheringDestination(uint32 skillId, uint32 entry, uint32 mapId);
    // True when some mining node in the catalog yields itemId (ore, stone, gems). Copper Bar shares the
    // ore item subclass but is never mined, so it fails this test.
    bool MiningNodeYieldsItem(uint32 itemId);
    TravelDestination* SelectAuctioneer(Player* bot);
    TravelDestination* SelectMailbox(Player* bot);
    // Nearest spell focus object (forge, anvil, cooking fire, ...) of the given SpellFocusObject.dbc id
    // on the bot's map, or nullptr when the map has none.
    class SpellFocusTravelDestination : public TravelDestination
    {
    public:
        SpellFocusTravelDestination(float radiusMin, float radiusMax) : TravelDestination(radiusMin, radiusMax) {}
        bool isActive([[maybe_unused]] Player* bot) override { return true; }
        std::string const getName() override { return "SpellFocusTravelDestination"; }
        std::string const getTitle() override { return "spell focus"; }
    };

    struct SpellFocusDestination
    {
        SpellFocusDestination(WorldPosition const& position, uint32 focusRange, float radiusMin, float radiusMax)
            : position(position), focusRange(focusRange), destination(radiusMin, radiusMax)
        {
            destination.addPoint(&this->position);
        }
        WorldPosition position;
        // The template's listed focus range in yards; the core accepts a caster within half of it.
        uint32 focusRange;
        SpellFocusTravelDestination destination;
    };

    SpellFocusDestination* SelectSpellFocus(Player* bot, uint32 spellFocusId);
    PlayerbotTrainerTravelSelection SelectTrainer(Player* bot, PlayerbotCareerTrainerObjective const& objective,
                                                  uint32 availableMoney);
    [[nodiscard]] static bool IsTrainerRouteReachable(PlayerbotTrainerRouteFacts const& facts);
    // Every item some vendor on the bot's map sells it for gold without a stock limit: faction, level and
    // reputation gates applied for this bot. Built from creature spawns, not from TravelMgr's RPG table,
    // which this playerbots fork never loads.
    std::unordered_set<uint32> ApplicableUnlimitedGoldVendorItems(Player* bot);
    // Nearest vendor on the bot's map that sells itemId to it for gold without a stock limit, or nullptr.
    TravelDestination* SelectVendor(Player* bot, uint32 itemId);

private:
    class MailboxTravelDestination : public TravelDestination
    {
    public:
        MailboxTravelDestination(float radiusMin, float radiusMax) : TravelDestination(radiusMin, radiusMax) {}
        bool isActive([[maybe_unused]] Player* bot) override { return true; }
        std::string const getName() override { return "MailboxTravelDestination"; }
        std::string const getTitle() override { return "mailbox"; }
    };

    struct AuctioneerDestination
    {
        AuctioneerDestination(WorldPosition const& position, uint32 entry, float radiusMin, float radiusMax)
            : position(position), destination(entry, radiusMin, radiusMax)
        {
            destination.addPoint(&this->position);
        }
        WorldPosition position;
        RpgTravelDestination destination;
    };

    struct MailboxDestination
    {
        MailboxDestination(WorldPosition const& position, float radiusMin, float radiusMax)
            : position(position), destination(radiusMin, radiusMax)
        {
            destination.addPoint(&this->position);
        }
        WorldPosition position;
        MailboxTravelDestination destination;
    };

    struct TrainerDestination
    {
        TrainerDestination(WorldPosition const& position, uint32 entry, uint32 zoneId, uint32 minimumLevel,
                           float radiusMin, float radiusMax)
            : position(position),
              entry(entry),
              zoneId(zoneId),
              minimumLevel(minimumLevel),
              destination(entry, radiusMin, radiusMax)
        {
            destination.addPoint(&this->position);
        }
        WorldPosition position;
        uint32 entry;
        uint32 zoneId;
        uint32 minimumLevel;
        RpgTravelDestination destination;
    };

    struct VendorDestination
    {
        VendorDestination(WorldPosition const& position, uint32 entry, float radiusMin, float radiusMax)
            : position(position), entry(entry), destination(entry, radiusMin, radiusMax)
        {
            destination.addPoint(&this->position);
        }
        WorldPosition position;
        uint32 entry;
        RpgTravelDestination destination;
    };

    void EnsureBuilt();
    // Item ids the vendor template sells to this bot for gold without a stock limit.
    static std::vector<uint32> ApplicableOffers(Player* bot, uint32 entry);
    bool built = false;
    std::vector<std::unique_ptr<GatheringTravelDestination>> gatheringDestinations;
    std::unordered_set<uint32> miningNodeYieldItemIds;
    std::unordered_map<uint32, std::vector<std::unique_ptr<AuctioneerDestination>>> allianceAuctioneersByMap;
    std::unordered_map<uint32, std::vector<std::unique_ptr<AuctioneerDestination>>> hordeAuctioneersByMap;
    std::unordered_map<uint32, std::vector<std::unique_ptr<MailboxDestination>>> mailboxesByMap;
    // spell focus id -> map id -> objects
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<std::unique_ptr<SpellFocusDestination>>>>
        spellFocusByMap;
    std::unordered_map<uint32, std::vector<std::unique_ptr<TrainerDestination>>> trainersByMap;
    std::unordered_map<uint32, std::vector<std::unique_ptr<VendorDestination>>> vendorsByMap;
};

#define sPlayerbotEconomyTravelCatalog PlayerbotEconomyTravelCatalog::instance()

#endif
