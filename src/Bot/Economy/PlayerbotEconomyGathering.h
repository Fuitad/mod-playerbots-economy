/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYGATHERING_H
#define PLAYERBOTS_PLAYERBOTECONOMYGATHERING_H

#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "Define.h"

namespace PlayerbotEconomy
{
class PlayerbotEconomyCoordinator;

enum class GatheringProfession : uint8
{
    Herbalism,
    Mining,
    Skinning
};

enum class GatheringBlocker : uint8
{
    None,
    InvalidResource,
    AlreadyClaimed,
    MissingCareer,
    MissingSkill,
    WrongProfession,
    InsufficientSkill,
    AffinityTooLow,
    NotGrouped,
    WrongMap,
    WrongPhase,
    MissingPath,
    OutOfRange,
    Unsafe,
    NoCandidate
};

enum class GatheringReleaseCause : uint8
{
    None,
    Combat,
    Transport,
    CommandReplacement,
    PathFailure,
    MapChanged,
    PhaseChanged,
    FormationMoved,
    Despawned,
    HigherPriorityBehavior,
    Success,
    Expired,
    Disabled
};

enum class GatheringClaimState : uint8
{
    Leased,
    Released,
    Completed
};

enum class AutonomousGatheringAction : uint8
{
    Travel,
    Gather,
    GrindOneCreature,
    Wait,
    Complete,
    Release
};

enum class AutonomousGatheringBlocker : uint8
{
    None,
    DemandGone,
    DestinationUnavailable,
    DestinationExpired,
    InventoryFull,
    Unsafe,
    OneKillBoundReached
};

struct GatheringResource
{
    uint64 resourceGuid = 0;
    GatheringProfession profession = GatheringProfession::Herbalism;
    uint32 mapId = 0;
    uint32 phaseMask = 0;
    uint32 requiredSkill = 0;
    bool spawned = false;
};

struct GatheringCandidate
{
    uint32 characterGuid = 0;
    GatheringProfession profession = GatheringProfession::Herbalism;
    uint32 skillValue = 0;
    uint8 economyAffinity = 0;
    float botDistance = 0.0f;
    float formationDistance = 0.0f;
    float lootDistance = 0.0f;
    float discoveryDistance = 0.0f;
    bool hasCareer = false;
    bool hasLearnedSkill = false;
    bool grouped = false;
    bool directCommand = false;
    bool sameMap = false;
    bool samePhase = false;
    bool pathAvailable = false;
    bool safe = false;
};

struct GatheringClaim
{
    uint64 leaseId = 0;
    uint64 resourceGuid = 0;
    uint32 characterGuid = 0;
    GatheringProfession profession = GatheringProfession::Herbalism;
    uint32 mapId = 0;
    uint32 phaseMask = 0;
    uint32 requiredSkill = 0;
    uint64 expiresAt = 0;
    bool directCommand = false;
    GatheringClaimState state = GatheringClaimState::Leased;
    GatheringReleaseCause releaseCause = GatheringReleaseCause::None;
};

struct GatheringClaimResult
{
    std::optional<GatheringClaim> claim;
    GatheringBlocker blocker = GatheringBlocker::NoCandidate;
};

struct GatheringClaimSnapshot
{
    uint64 generation = 0;
    std::vector<GatheringClaim> claims;
};

struct GatheringObservedSuccess
{
    uint64 leaseId = 0;
    uint64 resourceGuid = 0;
    uint32 characterGuid = 0;
    uint32 itemId = 0;
    uint32 quantity = 0;
};

struct GatheringContinuationFacts
{
    bool inCombat = false;
    bool onTransport = false;
    bool commandReplaced = false;
    bool pathFailed = false;
    bool mapChanged = false;
    bool phaseChanged = false;
    bool formationMoved = false;
    bool despawned = false;
    bool higherPriorityBehavior = false;
    bool succeeded = false;
};

struct AutonomousGatheringPlan
{
    GatheringProfession profession = GatheringProfession::Herbalism;
    uint32 itemId = 0;
    uint32 requestedQuantity = 0;
    uint32 startingItemCount = 0;
    uint32 startingSkillValue = 0;
    uint64 expiresAt = 0;
};

struct AutonomousGatheringFacts
{
    uint64 now = 0;
    uint32 currentItemCount = 0;
    uint32 currentSkillValue = 0;
    bool demandStillExists = false;
    bool destinationAvailable = false;
    bool inventoryCapacity = false;
    bool safe = false;
    bool atDestination = false;
    bool resourceAvailable = false;
    bool existingSkinningCorpse = false;
    bool creatureKillStarted = false;
    bool creatureKillActive = false;
};

struct AutonomousGatheringDecision
{
    AutonomousGatheringAction action = AutonomousGatheringAction::Release;
    AutonomousGatheringBlocker blocker = AutonomousGatheringBlocker::None;
    uint32 gatheredQuantity = 0;
    uint32 remainingQuantity = 0;
};

struct AutonomousSupplierListing
{
    uint32 count = 0;
    uint64 startBid = 0;
    uint64 buyout = 0;
};

class PlayerbotEconomyGathering
{
public:
    [[nodiscard]] GatheringClaimResult ClaimGrouped(GatheringResource const& resource,
                                                    std::span<GatheringCandidate const> candidates, uint64 now,
                                                    uint32 leaseSeconds);
    [[nodiscard]] GatheringClaimResult ClaimGrouped(GatheringResource const& resource,
                                                    std::initializer_list<GatheringCandidate> candidates, uint64 now,
                                                    uint32 leaseSeconds)
    {
        return ClaimGrouped(resource, std::span<GatheringCandidate const>(candidates), now, leaseSeconds);
    }
    [[nodiscard]] GatheringClaimResult ClaimNearby(GatheringResource const& resource,
                                                   GatheringCandidate const& candidate, uint64 now,
                                                   uint32 leaseSeconds);
    [[nodiscard]] bool Release(uint64 leaseId, GatheringReleaseCause cause);
    [[nodiscard]] bool ReleaseForActorResource(uint32 characterGuid, uint64 resourceGuid, GatheringReleaseCause cause,
                                               uint64 now);
    [[nodiscard]] bool Observe(GatheringClaim const& claim, std::map<uint32, uint32> startingItemCounts);
    [[nodiscard]] std::optional<GatheringObservedSuccess> ConfirmLoot(uint32 characterGuid, uint32 itemId,
                                                                      uint32 currentItemCount, uint64 now);
    void RemoveActor(uint32 characterGuid);
    [[nodiscard]] std::optional<GatheringClaim> FindLeasedByActor(uint32 characterGuid, uint64 now);
    [[nodiscard]] std::optional<GatheringClaim> FindLeasedByResource(uint64 resourceGuid, uint64 now);
    [[nodiscard]] GatheringClaimSnapshot Snapshot(uint64 now);
    [[nodiscard]] static std::optional<GatheringReleaseCause> ReleaseCause(GatheringContinuationFacts const& facts);
    [[nodiscard]] static AutonomousGatheringDecision DecideAutonomous(AutonomousGatheringPlan const& plan,
                                                                      AutonomousGatheringFacts const& facts);
    [[nodiscard]] static bool ReleaseCountsAsProgress(AutonomousGatheringDecision const& decision);
    [[nodiscard]] static bool SettleUnavailableDestination(PlayerbotEconomyCoordinator& coordinator, uint64 leaseId,
                                                           uint32 committedQuantity, uint64 now);
    [[nodiscard]] static AutonomousSupplierListing BoundSupplierListing(uint32 availableQuantity,
                                                                        uint32 committedQuantity,
                                                                        uint32 remainingDeficit,
                                                                        uint64 availableStartBid,
                                                                        uint64 availableBuyout);

private:
    [[nodiscard]] static GatheringBlocker Evaluate(GatheringResource const& resource,
                                                   GatheringCandidate const& candidate);
    void ExpireLocked(uint64 now);

    std::mutex mutex;
    std::vector<GatheringClaim> claims;
    struct Observation
    {
        GatheringClaim claim;
        std::map<uint32, uint32> startingItemCounts;
    };
    std::vector<Observation> observations;
    uint64 nextLeaseId = 1u;
    uint64 generation = 0u;
};

PlayerbotEconomyGathering& GetPlayerbotEconomyGathering();
}  // namespace PlayerbotEconomy

#endif
