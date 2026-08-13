/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYTRAVEL_H
#define PLAYERBOTS_PLAYERBOTECONOMYTRAVEL_H

#include <memory>
#include <unordered_map>
#include <vector>

#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "TravelMgr.h"

struct PlayerbotTrainerTravelSelection
{
    TravelDestination* destination = nullptr;
    uint32 entry = 0;
    PlayerbotCareerAcquisitionBlocker blocker = PlayerbotCareerAcquisitionBlocker::TrainerUnavailable;
};

enum class GatheringTravelSource : uint8
{
    HerbalismNode,
    MiningNode,
    SkinningCreature
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
    Inaccessible
};

struct GatheringDestinationFacts
{
    bool hasPoints = false;
    bool full = false;
    bool expired = false;
    bool coolingDown = false;
    bool sameMap = false;
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
    GatheringTravelDestination(GatheringTravelSource source, uint32 entry, uint32 skillId, uint32 requiredSkill,
                               uint8 minimumLevel, uint8 maximumLevel, std::vector<WorldPosition> points);

    bool isActive(Player* bot) override;
    std::string const getName() override { return "GatheringTravelDestination"; }
    int32 getEntry() override { return static_cast<int32>(entry); }
    std::string const getTitle() override;
    [[nodiscard]] uint32 getSkillId() const { return skillId; }
    [[nodiscard]] GatheringTravelSource getSource() const { return source; }
    [[nodiscard]] bool HasPointOnMap(uint32 mapId) const;
    [[nodiscard]] WorldPosition* NextUnvisitedPoint(WorldPosition& origin, uint32 mapId,
                                                    std::vector<WorldPosition*> const& visited) const;
    [[nodiscard]] std::unique_ptr<TravelDestination> MakePointDestination(WorldPosition* point);
    [[nodiscard]] GatheringDestinationBlocker GetBlocker(Player* bot, bool full = false);
    [[nodiscard]] static GatheringDestinationBlocker Evaluate(GatheringDestinationFacts const& facts);

private:
    GatheringTravelSource source;
    uint32 entry;
    uint32 skillId;
    uint32 requiredSkill;
    uint8 minimumLevel;
    uint8 maximumLevel;
    std::vector<WorldPosition> ownedPoints;
};

class PlayerbotEconomyTravelCatalog
{
public:
    static PlayerbotEconomyTravelCatalog& instance();
    std::vector<GatheringTravelDestination*> GatheringDestinations(Player* bot, uint32 skillId,
                                                                   GatheringDestinationBlocker* blocker = nullptr,
                                                                   bool ignoreFull = false,
                                                                   float maxDistance = 5000.0f);
    TravelDestination* SelectAuctioneer(Player* bot);
    TravelDestination* SelectMailbox(Player* bot);
    PlayerbotTrainerTravelSelection SelectTrainer(Player* bot, PlayerbotCareerTrainerObjective const& objective,
                                                  uint32 availableMoney);

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

    void EnsureBuilt();
    bool built = false;
    std::vector<std::unique_ptr<GatheringTravelDestination>> gatheringDestinations;
    std::unordered_map<uint32, std::vector<std::unique_ptr<AuctioneerDestination>>> allianceAuctioneersByMap;
    std::unordered_map<uint32, std::vector<std::unique_ptr<AuctioneerDestination>>> hordeAuctioneersByMap;
    std::unordered_map<uint32, std::vector<std::unique_ptr<MailboxDestination>>> mailboxesByMap;
    std::unordered_map<uint32, std::vector<std::unique_ptr<TrainerDestination>>> trainersByMap;
};

#define sPlayerbotEconomyTravelCatalog PlayerbotEconomyTravelCatalog::instance()

#endif
