/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyGathering.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "Bot/Economy/PlayerbotEconomyCoordinator.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr uint64 YIELD_BASIS_POINTS = 10'000u;
}

GatheringClaimResult PlayerbotEconomyGathering::ClaimGrouped(GatheringResource const& resource,
                                                             std::span<GatheringCandidate const> candidates, uint64 now,
                                                             uint32 leaseSeconds)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);

    if (!resource.resourceGuid || !resource.spawned || !resource.phaseMask || !leaseSeconds)
        return {std::nullopt, GatheringBlocker::InvalidResource};

    if (std::any_of(
            claims.begin(), claims.end(), [&resource](GatheringClaim const& claim)
            { return claim.resourceGuid == resource.resourceGuid && claim.state == GatheringClaimState::Leased; }))
    {
        return {std::nullopt, GatheringBlocker::AlreadyClaimed};
    }

    GatheringCandidate const* selected = nullptr;
    GatheringBlocker blocker = GatheringBlocker::NoCandidate;
    for (GatheringCandidate const& candidate : candidates)
    {
        GatheringBlocker const candidateBlocker = Evaluate(resource, candidate);
        if (candidateBlocker != GatheringBlocker::None)
        {
            if (blocker == GatheringBlocker::NoCandidate)
                blocker = candidateBlocker;
            continue;
        }

        bool const actorBusy =
            std::any_of(claims.begin(), claims.end(),
                        [&candidate, now](GatheringClaim const& claim)
                        {
                            return claim.characterGuid == candidate.characterGuid &&
                                   (claim.state == GatheringClaimState::Leased ||
                                    (claim.state == GatheringClaimState::Completed && claim.expiresAt > now));
                        });
        if (actorBusy)
        {
            blocker = GatheringBlocker::AlreadyClaimed;
            continue;
        }

        if (!selected || candidate.botDistance < selected->botDistance ||
            (candidate.botDistance == selected->botDistance &&
             candidate.formationDistance < selected->formationDistance) ||
            (candidate.botDistance == selected->botDistance &&
             candidate.formationDistance == selected->formationDistance &&
             candidate.characterGuid < selected->characterGuid))
        {
            selected = &candidate;
        }
    }

    if (!selected)
        return {std::nullopt, blocker};

    GatheringClaim claim;
    claim.leaseId = nextLeaseId++;
    claim.resourceGuid = resource.resourceGuid;
    claim.characterGuid = selected->characterGuid;
    claim.profession = resource.profession;
    claim.mapId = resource.mapId;
    claim.phaseMask = resource.phaseMask;
    claim.requiredSkill = resource.requiredSkill;
    claim.expiresAt = now + leaseSeconds;
    claim.directCommand = selected->directCommand;
    claims.push_back(claim);
    ++generation;
    return {claim, GatheringBlocker::None};
}

GatheringClaimResult PlayerbotEconomyGathering::ClaimNearby(GatheringResource const& resource,
                                                            GatheringCandidate const& candidate, uint64 now,
                                                            uint32 leaseSeconds)
{
    GatheringCandidate nearby = candidate;
    nearby.grouped = true;
    return ClaimGrouped(resource, {nearby}, now, leaseSeconds);
}

bool PlayerbotEconomyGathering::Release(uint64 leaseId, GatheringReleaseCause cause)
{
    std::scoped_lock lock(mutex);
    auto const claim = std::find_if(claims.begin(), claims.end(), [leaseId](GatheringClaim const& candidate)
                                    { return candidate.leaseId == leaseId; });
    if (claim == claims.end() || claim->state != GatheringClaimState::Leased || cause == GatheringReleaseCause::None ||
        cause == GatheringReleaseCause::Expired)
    {
        return false;
    }

    claim->state =
        cause == GatheringReleaseCause::Success ? GatheringClaimState::Completed : GatheringClaimState::Released;
    claim->releaseCause = cause;
    std::erase_if(observations,
                  [leaseId](Observation const& observation) { return observation.claim.leaseId == leaseId; });
    ++generation;
    return true;
}

void PlayerbotEconomyGathering::SetActiveTrip(uint32 characterGuid, uint32 skillId)
{
    std::scoped_lock lock(mutex);
    if (skillId)
        activeTrips[characterGuid] = skillId;
    else
        activeTrips.erase(characterGuid);
}

void PlayerbotEconomyGathering::ClearActiveTrip(uint32 characterGuid)
{
    std::scoped_lock lock(mutex);
    activeTrips.erase(characterGuid);
}

uint32 PlayerbotEconomyGathering::ActiveTripSkill(uint32 characterGuid)
{
    std::scoped_lock lock(mutex);
    auto const found = activeTrips.find(characterGuid);
    return found == activeTrips.end() ? 0u : found->second;
}

bool PlayerbotEconomyGathering::ReleaseForActorResource(uint32 characterGuid, uint64 resourceGuid,
                                                        GatheringReleaseCause cause, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    auto const claim = std::find_if(claims.begin(), claims.end(),
                                    [characterGuid, resourceGuid](GatheringClaim const& candidate)
                                    {
                                        return candidate.characterGuid == characterGuid &&
                                               candidate.resourceGuid == resourceGuid &&
                                               candidate.state == GatheringClaimState::Leased;
                                    });
    if (claim == claims.end() || cause == GatheringReleaseCause::None || cause == GatheringReleaseCause::Expired)
        return false;

    claim->state =
        cause == GatheringReleaseCause::Success ? GatheringClaimState::Completed : GatheringClaimState::Released;
    claim->releaseCause = cause;
    std::erase_if(
        observations, [characterGuid, resourceGuid](Observation const& observation)
        { return observation.claim.characterGuid == characterGuid && observation.claim.resourceGuid == resourceGuid; });
    ++generation;
    return true;
}

bool PlayerbotEconomyGathering::Observe(GatheringClaim const& claim, std::map<uint32, uint32> startingItemCounts)
{
    std::scoped_lock lock(mutex);
    auto const leased = std::find_if(claims.begin(), claims.end(),
                                     [&claim](GatheringClaim const& candidate)
                                     {
                                         return candidate.leaseId == claim.leaseId &&
                                                candidate.characterGuid == claim.characterGuid &&
                                                candidate.resourceGuid == claim.resourceGuid &&
                                                candidate.state == GatheringClaimState::Leased;
                                     });
    if (leased == claims.end())
        return false;

    std::erase_if(observations, [&claim](Observation const& observation)
                  { return observation.claim.characterGuid == claim.characterGuid; });
    observations.push_back({*leased, std::move(startingItemCounts)});
    return true;
}

std::optional<GatheringObservedSuccess> PlayerbotEconomyGathering::ConfirmLoot(uint32 characterGuid, uint32 itemId,
                                                                               uint32 currentItemCount, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    auto const observation =
        std::find_if(observations.begin(), observations.end(), [characterGuid](Observation const& candidate)
                     { return candidate.claim.characterGuid == characterGuid; });
    if (observation == observations.end() || !itemId)
        return std::nullopt;

    uint32 const startingItemCount =
        observation->startingItemCounts.contains(itemId) ? observation->startingItemCounts.at(itemId) : 0u;
    if (currentItemCount <= startingItemCount)
        return std::nullopt;

    auto const claim = std::find_if(
        claims.begin(), claims.end(), [&observation](GatheringClaim const& candidate)
        { return candidate.leaseId == observation->claim.leaseId && candidate.state == GatheringClaimState::Leased; });
    if (claim == claims.end())
        return std::nullopt;

    GatheringObservedSuccess success{
        .leaseId = claim->leaseId,
        .resourceGuid = claim->resourceGuid,
        .characterGuid = claim->characterGuid,
        .itemId = itemId,
        .quantity = currentItemCount - startingItemCount,
    };
    claim->state = GatheringClaimState::Completed;
    claim->releaseCause = GatheringReleaseCause::Success;
    observations.erase(observation);
    ++generation;
    return success;
}

void PlayerbotEconomyGathering::RemoveActor(uint32 characterGuid)
{
    std::scoped_lock lock(mutex);
    for (GatheringClaim& claim : claims)
    {
        if (claim.characterGuid != characterGuid || claim.state != GatheringClaimState::Leased)
            continue;
        claim.state = GatheringClaimState::Released;
        claim.releaseCause = GatheringReleaseCause::Disabled;
        ++generation;
    }
    std::erase_if(observations, [characterGuid](Observation const& observation)
                  { return observation.claim.characterGuid == characterGuid; });
    activeTrips.erase(characterGuid);
}

std::optional<GatheringClaim> PlayerbotEconomyGathering::FindLeasedByActor(uint32 characterGuid, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    auto const claim = std::find_if(
        claims.begin(), claims.end(), [characterGuid](GatheringClaim const& candidate)
        { return candidate.characterGuid == characterGuid && candidate.state == GatheringClaimState::Leased; });
    return claim == claims.end() ? std::nullopt : std::optional<GatheringClaim>(*claim);
}

std::optional<GatheringClaim> PlayerbotEconomyGathering::FindClaim(uint64 leaseId, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    auto const claim = std::find_if(claims.begin(), claims.end(), [leaseId](GatheringClaim const& candidate)
                                    { return candidate.leaseId == leaseId; });
    return claim == claims.end() ? std::nullopt : std::optional<GatheringClaim>(*claim);
}

std::optional<GatheringClaim> PlayerbotEconomyGathering::FindLeasedByResource(uint64 resourceGuid, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    auto const claim = std::find_if(
        claims.begin(), claims.end(), [resourceGuid](GatheringClaim const& candidate)
        { return candidate.resourceGuid == resourceGuid && candidate.state == GatheringClaimState::Leased; });
    return claim == claims.end() ? std::nullopt : std::optional<GatheringClaim>(*claim);
}

GatheringClaimSnapshot PlayerbotEconomyGathering::Snapshot(uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    return {generation, claims};
}

std::optional<GatheringReleaseCause> PlayerbotEconomyGathering::ReleaseCause(GatheringContinuationFacts const& facts)
{
    if (facts.inCombat)
        return GatheringReleaseCause::Combat;
    if (facts.onTransport)
        return GatheringReleaseCause::Transport;
    if (facts.commandReplaced)
        return GatheringReleaseCause::CommandReplacement;
    if (facts.pathFailed)
        return GatheringReleaseCause::PathFailure;
    if (facts.mapChanged)
        return GatheringReleaseCause::MapChanged;
    if (facts.phaseChanged)
        return GatheringReleaseCause::PhaseChanged;
    if (facts.formationMoved)
        return GatheringReleaseCause::FormationMoved;
    if (facts.despawned)
        return GatheringReleaseCause::Despawned;
    if (facts.higherPriorityBehavior)
        return GatheringReleaseCause::HigherPriorityBehavior;
    if (facts.succeeded)
        return GatheringReleaseCause::Success;
    return std::nullopt;
}

AutonomousGatheringDecision PlayerbotEconomyGathering::DecideAutonomous(AutonomousGatheringPlan const& plan,
                                                                        AutonomousGatheringFacts const& facts)
{
    AutonomousGatheringDecision decision;
    decision.gatheredQuantity =
        facts.currentItemCount > plan.startingItemCount ? facts.currentItemCount - plan.startingItemCount : 0u;
    decision.remainingQuantity =
        decision.gatheredQuantity >= plan.requestedQuantity ? 0u : plan.requestedQuantity - decision.gatheredQuantity;

    bool const deficitSatisfied = plan.itemId && plan.requestedQuantity && !decision.remainingQuantity;
    bool const progressionAdvanced = !plan.itemId && facts.currentSkillValue > plan.startingSkillValue;
    if (deficitSatisfied || progressionAdvanced)
    {
        decision.action = AutonomousGatheringAction::Complete;
        return decision;
    }

    if (plan.itemId && !facts.demandStillExists)
    {
        decision.blocker = AutonomousGatheringBlocker::DemandGone;
        return decision;
    }
    if (!plan.expiresAt || facts.now >= plan.expiresAt)
    {
        decision.blocker = AutonomousGatheringBlocker::DestinationExpired;
        return decision;
    }
    if (!facts.destinationAvailable)
    {
        decision.blocker = AutonomousGatheringBlocker::DestinationUnavailable;
        return decision;
    }
    if (!facts.safe)
    {
        // Combat, flight, or a teleport is momentary; pausing keeps the trip alive
        // while the destination expiry above still bounds a permanently unsafe bot.
        decision.action = AutonomousGatheringAction::Wait;
        return decision;
    }
    if (!facts.inventoryCapacity)
    {
        decision.blocker = AutonomousGatheringBlocker::InventoryFull;
        return decision;
    }
    if (!facts.atDestination)
    {
        decision.action = AutonomousGatheringAction::Travel;
        return decision;
    }
    if (plan.profession == GatheringProfession::Hunting)
    {
        // A hunt kills until the drop lands or the trip clock runs out; each corpse is emptied by the
        // loot strategy before the next creature is engaged.
        decision.action = facts.creatureKillActive || (facts.creatureKillStarted && facts.corpseLootPending)
                              ? AutonomousGatheringAction::Wait
                              : AutonomousGatheringAction::GrindOneCreature;
        return decision;
    }
    if (plan.profession != GatheringProfession::Skinning)
    {
        decision.action =
            facts.resourceAvailable ? AutonomousGatheringAction::Gather : AutonomousGatheringAction::Travel;
        return decision;
    }
    if (facts.existingSkinningCorpse)
    {
        decision.action = AutonomousGatheringAction::Gather;
        return decision;
    }
    if (!facts.creatureKillStarted)
    {
        decision.action = AutonomousGatheringAction::GrindOneCreature;
        return decision;
    }
    if (facts.creatureKillActive)
    {
        decision.action = AutonomousGatheringAction::Wait;
        return decision;
    }

    decision.action = AutonomousGatheringAction::Release;
    decision.blocker = AutonomousGatheringBlocker::OneKillBoundReached;
    return decision;
}

bool PlayerbotEconomyGathering::ReleaseCountsAsProgress(AutonomousGatheringDecision const& decision)
{
    return decision.gatheredQuantity != 0u;
}

bool PlayerbotEconomyGathering::SettleUnavailableDestination(PlayerbotEconomyCoordinator& coordinator, uint64 leaseId,
                                                             uint32 committedQuantity, uint64 now)
{
    if (!leaseId)
        return false;
    EconomyAssignmentOutcome const outcome =
        committedQuantity ? EconomyAssignmentOutcome::InventoryReceived : EconomyAssignmentOutcome::FailedTravel;
    return coordinator.RecordOutcome(leaseId, outcome, committedQuantity, now);
}

AutonomousSupplierListing PlayerbotEconomyGathering::BoundSupplierListing(uint32 availableQuantity,
                                                                          uint32 committedQuantity,
                                                                          uint32 remainingDeficit,
                                                                          uint64 availableStartBid,
                                                                          uint64 availableBuyout)
{
    uint32 const count = std::min({availableQuantity, committedQuantity, remainingDeficit});
    if (!count || !availableQuantity)
        return {};

    auto const scaled = [availableQuantity, count](uint64 value)
    { return (value * count + availableQuantity - 1u) / availableQuantity; };
    uint64 const startBid = scaled(availableStartBid);
    return {count, startBid, std::max(startBid, scaled(availableBuyout))};
}

AcceptedExternalGatheringSlice PlayerbotEconomyGathering::ReconcileAcceptedExternalSlice(
    AcceptedExternalGatheringSliceFacts const& facts)
{
    uint32 const postTripInventoryDelta = facts.currentInventoryQuantity > facts.preTripInventoryQuantity
                                              ? facts.currentInventoryQuantity - facts.preTripInventoryQuantity
                                              : 0u;
    uint32 const protectedQuantity = facts.retained ? std::min(postTripInventoryDelta, facts.acceptedQuantity) : 0u;
    return {protectedQuantity, facts.currentInventoryQuantity - protectedQuantity};
}

bool PlayerbotEconomyGathering::OutboundFitsTripBudget(uint32 outboundSeconds, uint32 tripBudgetSeconds)
{
    return tripBudgetSeconds && static_cast<uint64>(outboundSeconds) * 2u < tripBudgetSeconds;
}

bool PlayerbotEconomyGathering::LootGuardAllowsNewStack(uint8 bagSpacePercent, uint8 guardPercent, bool hasPartialStack)
{
    return bagSpacePercent <= guardPercent || hasPartialStack;
}

std::optional<uint32> PlayerbotEconomyGathering::GatheringSkillForTradeGood(uint32 itemClass, uint32 itemSubClass,
                                                                            bool yieldedByMiningNode)
{
    if (itemClass != ITEM_CLASS_TRADE_GOODS)
        return std::nullopt;
    if (itemSubClass == ITEM_SUBCLASS_HERB)
        return SKILL_HERBALISM;
    if (itemSubClass == ITEM_SUBCLASS_METAL_STONE)
        return yieldedByMiningNode ? std::optional<uint32>(SKILL_MINING) : std::nullopt;
    if (itemSubClass == ITEM_SUBCLASS_LEATHER)
        return SKILL_SKINNING;
    return std::nullopt;
}

bool PlayerbotEconomyGathering::IsDisenchantYieldMaterial(uint32 itemClass, uint32 itemSubClass)
{
    return itemClass == ITEM_CLASS_TRADE_GOODS && itemSubClass == ITEM_SUBCLASS_ENCHANTING;
}

std::vector<std::string> PlayerbotEconomyGathering::IdleStrategiesToSuspend(
    std::vector<std::string> const& activeStrategies)
{
    // Grind is idle work too: its "attack anything" picks a fight with every mob on the way to the
    // node, and hunting trips open their own fights through the pull target.
    static constexpr std::array<char const*, 4> idle{"rpg", "new rpg", "move random", "grind"};
    std::vector<std::string> suspended;
    for (char const* strategy : idle)
    {
        if (std::find(activeStrategies.begin(), activeStrategies.end(), strategy) != activeStrategies.end())
            suspended.emplace_back(strategy);
    }
    return suspended;
}

LostTravelTargetDecision PlayerbotEconomyGathering::DecideLostTravelTarget(LostTravelTargetFacts const& facts)
{
    if (!facts.alive)
        return {LostTravelTargetAction::Release, "actor_dead"};
    if (!facts.sameMap)
        return {LostTravelTargetAction::Release, "actor_relocated"};
    if (!facts.deadlineAhead)
        return {LostTravelTargetAction::Release, "deadline_passed"};
    if (facts.retravelAttempts >= MAX_TRIP_RETRAVELS)
        return {LostTravelTargetAction::Release, "retravel_exhausted"};
    return {LostTravelTargetAction::Retravel, "travel_target_lost"};
}

uint32 PlayerbotEconomyGathering::GatheringSkillTargetForLevel(uint8 level, uint32 maxRank)
{
    return std::min<uint32>(maxRank, static_cast<uint32>(level) * 5u);
}

uint32 PlayerbotEconomyGathering::ReachableResourceCap(GatheringReachableResourceFacts const& facts)
{
    uint64 const windowThroughput =
        facts.conservativeSecondsPerResource ? facts.resourceTimeSeconds / facts.conservativeSecondsPerResource : 0u;
    uint64 const resourcesForDemand =
        facts.conservativeYieldBasisPoints
            ? (static_cast<uint64>(facts.activeUncoveredDemand) * 10'000u + facts.conservativeYieldBasisPoints - 1u) /
                  facts.conservativeYieldBasisPoints
            : 0u;

    // Never count fewer spawns than the requirement needs. The window throughput is an estimate and
    // a cold start one is worthless; the spawn map is the fact. Capping below the demand refuses the
    // trip before the map is read, and refusing the trip is what keeps the estimate at cold start.
    return static_cast<uint32>(
        std::min<uint64>(std::max(windowThroughput, resourcesForDemand), std::numeric_limits<uint32>::max()));
}

uint32 PlayerbotEconomyGathering::DedicatedWorkOrderCapacity(DedicatedGatheringCapacityFacts const& facts)
{
    if (!facts.skillEligible || !facts.routeAvailable || !facts.safe || !facts.deliveryAvailable ||
        !facts.activeUncoveredDemand || facts.selfReservedQuantity >= facts.activeUncoveredDemand ||
        !facts.reachableResourceCount || !facts.conservativeYieldBasisPoints || !facts.inventoryCapacity ||
        !facts.activityBudgetSeconds || !facts.conservativeSecondsPerResource)
    {
        return 0u;
    }

    uint64 const fixedTravelSeconds = static_cast<uint64>(facts.outboundSeconds) + facts.returnSeconds;
    if (fixedTravelSeconds >= facts.activityBudgetSeconds)
        return 0u;

    uint64 const resourceTimeSeconds = facts.activityBudgetSeconds - fixedTravelSeconds;
    uint64 const resourcesWithinTime = resourceTimeSeconds / facts.conservativeSecondsPerResource;
    uint64 const feasibleResources = facts.respawningPopulation
                                         ? resourcesWithinTime
                                         : std::min<uint64>(facts.reachableResourceCount, resourcesWithinTime);
    uint64 const expectedBasisPoints = feasibleResources * facts.conservativeYieldBasisPoints;
    // Nodes floor the expectation (a vein is one ore or none); a hunt rounds it, since three kills at a
    // 39 percent drop are one expected cloth, not zero.
    uint64 const resourceCapacity = facts.respawningPopulation
                                        ? (expectedBasisPoints + YIELD_BASIS_POINTS / 2u) / YIELD_BASIS_POINTS
                                        : expectedBasisPoints / YIELD_BASIS_POINTS;
    uint64 const physicalCapacity = std::min(resourceCapacity, static_cast<uint64>(facts.inventoryCapacity));
    uint64 const selfReservation = std::min<uint64>(facts.selfReservedQuantity, physicalCapacity);
    uint64 const externalCapacity = physicalCapacity - selfReservation;
    uint64 const externalDemand = facts.activeUncoveredDemand - facts.selfReservedQuantity;
    return static_cast<uint32>(std::min(externalDemand, externalCapacity));
}

DedicatedGatheringPlan PlayerbotEconomyGathering::PlanDedicatedWork(
    uint32 activeUncoveredDemand, std::span<DedicatedGatheringCandidate const> candidates)
{
    std::vector<DedicatedGatheringCandidate const*> ranked;
    ranked.reserve(candidates.size());
    for (DedicatedGatheringCandidate const& candidate : candidates)
    {
        if (candidate.characterGuid && candidate.capacity)
            ranked.push_back(&candidate);
    }

    std::sort(ranked.begin(), ranked.end(),
              [](DedicatedGatheringCandidate const* left, DedicatedGatheringCandidate const* right)
              {
                  if (left->capacity != right->capacity)
                      return left->capacity > right->capacity;
                  if (left->recentWorkSeconds != right->recentWorkSeconds)
                      return left->recentWorkSeconds < right->recentWorkSeconds;
                  if (left->routeSeconds != right->routeSeconds)
                      return left->routeSeconds < right->routeSeconds;
                  uint64 const leftReliability =
                      static_cast<uint64>(left->reliabilitySuccesses) * std::max(1u, right->reliabilityAttempts);
                  uint64 const rightReliability =
                      static_cast<uint64>(right->reliabilitySuccesses) * std::max(1u, left->reliabilityAttempts);
                  if (leftReliability != rightReliability)
                      return leftReliability > rightReliability;
                  if (left->skillValue != right->skillValue)
                      return left->skillValue > right->skillValue;
                  if (left->gatheringAffinity != right->gatheringAffinity)
                      return left->gatheringAffinity > right->gatheringAffinity;
                  return left->characterGuid < right->characterGuid;
              });

    DedicatedGatheringPlan plan;
    plan.unassignedQuantity = activeUncoveredDemand;
    for (DedicatedGatheringCandidate const* candidate : ranked)
    {
        if (!plan.unassignedQuantity)
            break;
        uint32 const quantity = std::min(candidate->capacity, plan.unassignedQuantity);
        plan.workOrders.push_back({candidate->characterGuid, quantity});
        plan.assignedQuantity += quantity;
        plan.unassignedQuantity -= quantity;
    }
    return plan;
}

std::optional<DedicatedGatheringProvenancePlan> PlayerbotEconomyGathering::PlanDedicatedWork(
    DedicatedGatheringPlanRequest const& request, std::span<DedicatedGatheringCandidate const> candidates)
{
    if (request.tripIdentity.empty() || !request.batchQuantity || request.origins.empty())
        return std::nullopt;

    std::unordered_set<uint32> candidateActors;
    for (DedicatedGatheringCandidate const& candidate : candidates)
    {
        if (candidate.characterGuid && candidate.capacity && !candidateActors.insert(candidate.characterGuid).second)
            return std::nullopt;
    }

    std::unordered_set<std::string> identities;
    uint64 activeQuantity = 0u;
    uint64 latentQuantity = 0u;
    for (DedicatedGatheringOrigin const& origin : request.origins)
    {
        if (origin.originIdentity.empty() || origin.originIdentity == request.tripIdentity || !origin.quantity ||
            origin.expiresAt <= request.observedAt || !identities.insert(origin.originIdentity).second ||
            (origin.state != DedicatedGatheringOriginState::Active &&
             origin.state != DedicatedGatheringOriginState::Latent))
        {
            return std::nullopt;
        }

        if (origin.state == DedicatedGatheringOriginState::Active)
            activeQuantity += origin.quantity;
        else
            latentQuantity += origin.quantity;
    }

    uint32 const executableQuantity = static_cast<uint32>(std::min<uint64>(request.batchQuantity, activeQuantity));
    DedicatedGatheringPlan basePlan = PlanDedicatedWork(executableQuantity, candidates);
    DedicatedGatheringProvenancePlan plan;
    plan.tripIdentity = request.tripIdentity;
    plan.observedAt = request.observedAt;
    plan.batchQuantity = request.batchQuantity;
    plan.origins = request.origins;
    plan.workOrders = std::move(basePlan.workOrders);
    plan.assignedQuantity = basePlan.assignedQuantity;
    plan.unassignedBatchQuantity = basePlan.unassignedQuantity;
    plan.deferredActiveQuantity = activeQuantity - executableQuantity;
    plan.latentQuantity = latentQuantity;

    std::size_t originIndex = 0u;
    uint32 originAllocatedQuantity = 0u;
    for (DedicatedGatheringWorkOrder& workOrder : plan.workOrders)
    {
        uint32 remaining = workOrder.quantity;
        while (remaining)
        {
            while (originIndex < plan.origins.size() &&
                   (plan.origins[originIndex].state == DedicatedGatheringOriginState::Latent ||
                    originAllocatedQuantity == plan.origins[originIndex].quantity))
            {
                ++originIndex;
                originAllocatedQuantity = 0u;
            }
            if (originIndex == plan.origins.size())
                return std::nullopt;

            DedicatedGatheringOrigin const& origin = plan.origins[originIndex];
            uint32 const available = origin.quantity - originAllocatedQuantity;
            uint32 const allocated = std::min(remaining, available);
            workOrder.allocations.push_back({origin.originIdentity, allocated});
            originAllocatedQuantity += allocated;
            remaining -= allocated;
        }
    }
    return plan;
}

std::optional<DedicatedGatheringTripProvenance> PlayerbotEconomyGathering::ProvenanceForWorkOrder(
    DedicatedGatheringProvenancePlan const& plan, DedicatedGatheringWorkOrder const& workOrder)
{
    if (plan.tripIdentity.empty() || !plan.batchQuantity || plan.origins.empty() || !workOrder.characterGuid ||
        !workOrder.quantity ||
        std::find(plan.workOrders.begin(), plan.workOrders.end(), workOrder) == plan.workOrders.end())
    {
        return std::nullopt;
    }

    std::unordered_map<std::string, DedicatedGatheringOrigin const*> activeOrigins;
    std::unordered_set<std::string> planOriginIdentities;
    uint64 activeQuantity = 0u;
    uint64 latentQuantity = 0u;
    for (DedicatedGatheringOrigin const& origin : plan.origins)
    {
        if (origin.originIdentity.empty() || origin.originIdentity == plan.tripIdentity || !origin.quantity ||
            origin.expiresAt <= plan.observedAt || !planOriginIdentities.insert(origin.originIdentity).second ||
            (origin.state != DedicatedGatheringOriginState::Active &&
             origin.state != DedicatedGatheringOriginState::Latent))
        {
            return std::nullopt;
        }
        if (origin.state == DedicatedGatheringOriginState::Active)
        {
            activeOrigins.emplace(origin.originIdentity, &origin);
            activeQuantity += origin.quantity;
        }
        else
            latentQuantity += origin.quantity;
    }

    std::unordered_set<uint32> actors;
    std::unordered_map<std::string, uint64> globallyAllocatedByOrigin;
    uint64 assignedQuantity = 0u;
    for (DedicatedGatheringWorkOrder const& plannedOrder : plan.workOrders)
    {
        if (!plannedOrder.characterGuid || !plannedOrder.quantity || !actors.insert(plannedOrder.characterGuid).second)
            return std::nullopt;

        std::unordered_set<std::string> orderOrigins;
        uint64 orderQuantity = 0u;
        for (DedicatedGatheringOriginAllocation const& allocation : plannedOrder.allocations)
        {
            auto const origin = activeOrigins.find(allocation.originIdentity);
            if (!allocation.quantity || origin == activeOrigins.end() ||
                !orderOrigins.insert(allocation.originIdentity).second)
            {
                return std::nullopt;
            }
            orderQuantity += allocation.quantity;
            uint64& globalQuantity = globallyAllocatedByOrigin[allocation.originIdentity];
            globalQuantity += allocation.quantity;
            if (globalQuantity > origin->second->quantity)
                return std::nullopt;
        }
        if (orderQuantity != plannedOrder.quantity)
            return std::nullopt;
        assignedQuantity += plannedOrder.quantity;
    }

    uint64 const executableQuantity = std::min<uint64>(plan.batchQuantity, activeQuantity);
    if (assignedQuantity != plan.assignedQuantity || assignedQuantity > executableQuantity ||
        plan.unassignedBatchQuantity != executableQuantity - assignedQuantity ||
        plan.deferredActiveQuantity != activeQuantity - executableQuantity || plan.latentQuantity != latentQuantity)
    {
        return std::nullopt;
    }

    DedicatedGatheringTripProvenance provenance;
    provenance.tripIdentity = plan.tripIdentity;
    provenance.origins.reserve(plan.origins.size());
    std::unordered_map<std::string, uint32> allocatedByOrigin;
    for (DedicatedGatheringOriginAllocation const& allocation : workOrder.allocations)
        allocatedByOrigin.emplace(allocation.originIdentity, allocation.quantity);
    for (DedicatedGatheringOrigin const& origin : plan.origins)
    {
        auto const allocated = allocatedByOrigin.find(origin.originIdentity);
        provenance.origins.push_back({origin, allocated == allocatedByOrigin.end() ? 0u : allocated->second});
    }
    return provenance;
}

void PlayerbotEconomyGathering::RecordDedicatedActivity(uint32 characterGuid, uint64 startedAt, uint64 finishedAt)
{
    if (!characterGuid || finishedAt <= startedAt)
        return;

    std::scoped_lock lock(mutex);
    DedicatedActivity& activity = dedicatedActivity[characterGuid];
    if (activity.lastUpdatedAt && startedAt > activity.lastUpdatedAt)
    {
        uint64 const recovered = startedAt - activity.lastUpdatedAt;
        activity.debtSeconds = recovered >= activity.debtSeconds ? 0u : activity.debtSeconds - recovered;
    }
    activity.debtSeconds += finishedAt - startedAt;
    activity.lastUpdatedAt = std::max(activity.lastUpdatedAt, finishedAt);
}

uint32 PlayerbotEconomyGathering::AvailableDedicatedActivityBudget(uint32 characterGuid, uint32 budgetSeconds,
                                                                   uint64 now)
{
    if (!characterGuid || !budgetSeconds)
        return 0u;

    std::scoped_lock lock(mutex);
    auto found = dedicatedActivity.find(characterGuid);
    if (found == dedicatedActivity.end())
        return budgetSeconds;

    DedicatedActivity& activity = found->second;
    if (now > activity.lastUpdatedAt)
    {
        uint64 const recovered = now - activity.lastUpdatedAt;
        activity.debtSeconds = recovered >= activity.debtSeconds ? 0u : activity.debtSeconds - recovered;
        activity.lastUpdatedAt = now;
    }
    return activity.debtSeconds >= budgetSeconds ? 0u : budgetSeconds - static_cast<uint32>(activity.debtSeconds);
}

void PlayerbotEconomyGathering::RecordDedicatedTrip(DedicatedGatheringTripObservation const& observation)
{
    RecordDedicatedActivity(observation.characterGuid, observation.startedAt, observation.finishedAt);
    if (!observation.characterGuid || observation.finishedAt <= observation.startedAt ||
        !observation.attemptedResources)
    {
        return;
    }

    uint64 const duration = observation.finishedAt - observation.startedAt;
    uint64 const resourceSeconds =
        duration > observation.outboundSeconds ? duration - observation.outboundSeconds : duration;
    std::scoped_lock lock(mutex);
    DedicatedHistory& history = dedicatedHistory[{observation.characterGuid, observation.itemId}];
    history.gatheredQuantity += observation.gatheredQuantity;
    history.resourceAttempts += observation.attemptedResources;
    history.resourceSeconds += resourceSeconds;
    history.successes += observation.gatheredQuantity ? 1u : 0u;
    ++history.attempts;
    history.skillPoints += observation.skillPoints;
}

DedicatedGatheringExperience PlayerbotEconomyGathering::DedicatedExperience(uint32 characterGuid, uint32 itemId)
{
    std::scoped_lock lock(mutex);
    auto const found = dedicatedHistory.find({characterGuid, itemId});
    if (found == dedicatedHistory.end() || !found->second.resourceAttempts)
        return {};

    DedicatedHistory const& history = found->second;
    uint64 const observedYield = history.gatheredQuantity * YIELD_BASIS_POINTS / history.resourceAttempts;
    uint64 const observedSeconds = (history.resourceSeconds + history.resourceAttempts - 1u) / history.resourceAttempts;
    uint64 constexpr maximum = std::numeric_limits<uint32>::max();
    return {
        .observedYieldBasisPoints = static_cast<uint32>(std::min(maximum, observedYield)),
        .conservativeSecondsPerResource = static_cast<uint32>(std::min(maximum, observedSeconds)),
        .successes = history.successes,
        .attempts = history.attempts,
        .gatheredQuantity = history.gatheredQuantity,
        .resourceAttempts = history.resourceAttempts,
        .resourceSeconds = history.resourceSeconds,
    };
}

bool PlayerbotEconomyGathering::IsSkinningTargetLevelSafe(uint8 botLevel, uint8 creatureMaximumLevel,
                                                          uint8 upperLevelMargin)
{
    return static_cast<uint16>(creatureMaximumLevel) <= static_cast<uint16>(botLevel) + upperLevelMargin;
}

bool PlayerbotEconomyGathering::IsHuntingTargetLevelSafe(uint8 botLevel, uint8 creatureMaximumLevel)
{
    return creatureMaximumLevel <= botLevel;
}

GatheringBlocker PlayerbotEconomyGathering::Evaluate(GatheringResource const& resource,
                                                     GatheringCandidate const& candidate)
{
    if (!candidate.characterGuid)
        return GatheringBlocker::NoCandidate;
    if (!candidate.hasCareer)
        return GatheringBlocker::MissingCareer;
    if (!candidate.hasLearnedSkill)
        return GatheringBlocker::MissingSkill;
    if (candidate.profession != resource.profession)
        return GatheringBlocker::WrongProfession;
    if (candidate.skillValue < resource.requiredSkill)
        return GatheringBlocker::InsufficientSkill;
    if (!candidate.directCommand && !candidate.activeTrip)
    {
        if (candidate.yieldsAtCeiling)
            return GatheringBlocker::InventoryFull;
        if (!candidate.skillUpPossible && !candidate.craftingUsesYield && !candidate.marketEligible)
            return GatheringBlocker::NotUseful;
    }
    if (candidate.committedElsewhere)
        return GatheringBlocker::Committed;
    if (!candidate.grouped)
        return GatheringBlocker::NotGrouped;
    if (!candidate.sameMap)
        return GatheringBlocker::WrongMap;
    if (!candidate.samePhase)
        return GatheringBlocker::WrongPhase;
    bool const inInteractionRange = std::isfinite(candidate.botDistance) && std::isfinite(candidate.lootDistance) &&
                                    candidate.botDistance >= 0.0f && candidate.lootDistance > 0.0f &&
                                    candidate.botDistance <= candidate.lootDistance;
    if (!candidate.pathAvailable && !inInteractionRange)
        return GatheringBlocker::MissingPath;
    if (!candidate.safe)
        return GatheringBlocker::Unsafe;
    float const claimDistance =
        candidate.discoveryDistance > 0.0f ? candidate.discoveryDistance : candidate.lootDistance;
    if (!std::isfinite(candidate.botDistance) || !std::isfinite(candidate.formationDistance) ||
        !std::isfinite(candidate.lootDistance) || !std::isfinite(candidate.discoveryDistance) ||
        candidate.botDistance < 0.0f || candidate.formationDistance < 0.0f || candidate.lootDistance <= 0.0f ||
        candidate.discoveryDistance < 0.0f || candidate.botDistance > claimDistance ||
        candidate.formationDistance > claimDistance)
    {
        return GatheringBlocker::OutOfRange;
    }
    return GatheringBlocker::None;
}

void PlayerbotEconomyGathering::ExpireLocked(uint64 now)
{
    bool changed = false;
    for (GatheringClaim& claim : claims)
    {
        if (claim.state != GatheringClaimState::Leased || claim.expiresAt > now)
            continue;
        claim.state = GatheringClaimState::Released;
        claim.releaseCause = GatheringReleaseCause::Expired;
        changed = true;
    }
    std::erase_if(observations,
                  [this](Observation const& observation)
                  {
                      auto const claim =
                          std::find_if(claims.begin(), claims.end(), [&observation](GatheringClaim const& candidate)
                                       { return candidate.leaseId == observation.claim.leaseId; });
                      return claim == claims.end() || claim->state != GatheringClaimState::Leased;
                  });
    if (changed)
        ++generation;
}

PlayerbotEconomyGathering& PlayerbotEconomy::GetPlayerbotEconomyGathering()
{
    static PlayerbotEconomyGathering gathering;
    return gathering;
}
