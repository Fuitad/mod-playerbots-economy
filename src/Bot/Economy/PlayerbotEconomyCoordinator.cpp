/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyCoordinator.h"

#include <algorithm>
#include <limits>
#include <utility>

using namespace PlayerbotEconomy;

namespace
{
uint32 BoundedQuantity(uint64 quantity)
{
    return static_cast<uint32>(std::min<uint64>(quantity, std::numeric_limits<uint32>::max()));
}

bool Contains(std::vector<uint16> const& values, uint16 value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool Contains(std::vector<uint32> const& values, uint32 value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

uint8 RelevantAffinity(EconomyActorFacts const& actor, ProfessionCapabilityKind kind)
{
    return kind == ProfessionCapabilityKind::Crafting ? actor.craftingAffinity : actor.gatheringAffinity;
}

// Profession work is gated on the affinity that put the actor on it: the runtime admits a crafter
// or gatherer on crafting or gathering affinity alone, so the coordinator must not turn the same
// work (or the reagent purchases feeding a recipe the actor knows) away on economy affinity.
// Market work keeps the economy figure.
uint8 WorkAffinity(EconomyActorFacts const& actor, EconomyAssignmentRequest const& request)
{
    switch (request.workKind)
    {
        case EconomyWorkKind::Gather:
            return std::max(actor.economyAffinity, actor.gatheringAffinity);
        case EconomyWorkKind::Craft:
        case EconomyWorkKind::Recipe:
        case EconomyWorkKind::Trainer:
            return std::max(actor.economyAffinity, actor.craftingAffinity);
        case EconomyWorkKind::Buy:
            return request.recipeSpellId && Contains(actor.recipeSpellIds, request.recipeSpellId)
                       ? std::max(actor.economyAffinity, actor.craftingAffinity)
                       : actor.economyAffinity;
        default:
            return actor.economyAffinity;
    }
}

bool IsValidRequirement(EconomyCapabilityRequirement const& requirement)
{
    ProfessionCapability const& capability = requirement.capability;
    if (!requirement.marketId || !capability.outputItemId || !capability.professionSkillId ||
        !capability.primaryProfession)
    {
        return false;
    }
    switch (capability.kind)
    {
        case ProfessionCapabilityKind::Crafting:
            if (!capability.recipeSpellId)
                return false;
            break;
        case ProfessionCapabilityKind::Gathering:
            if (capability.recipeSpellId)
                return false;
            break;
        default:
            return false;
    }
    return requirement.group.kind != EconomySubstitutionKind::ExactReagent ||
           requirement.group.exactItemId == capability.outputItemId;
}
}  // namespace

EconomySubstitutionGroup EconomySubstitutionGroup::ExactReagent(uint32 itemId)
{
    EconomySubstitutionGroup group;
    group.kind = EconomySubstitutionKind::ExactReagent;
    group.exactItemId = itemId;
    return group;
}

EconomySubstitutionGroup EconomySubstitutionGroup::Equipment(uint8 slot, uint32 roles, uint8 itemTier)
{
    EconomySubstitutionGroup group;
    group.kind = EconomySubstitutionKind::Equipment;
    group.equipmentSlot = slot;
    group.roleMask = roles;
    group.tier = itemTier;
    return group;
}

EconomySubstitutionGroup EconomySubstitutionGroup::Bag(uint16 capacity)
{
    EconomySubstitutionGroup group;
    group.kind = EconomySubstitutionKind::Bag;
    group.bagCapacity = capacity;
    return group;
}

EconomySubstitutionGroup EconomySubstitutionGroup::Ammunition(uint32 type, uint8 itemTier)
{
    EconomySubstitutionGroup group;
    group.kind = EconomySubstitutionKind::Ammunition;
    group.ammunitionType = type;
    group.tier = itemTier;
    return group;
}

EconomySubstitutionGroup EconomySubstitutionGroup::Consumable(uint32 family, uint8 itemTier)
{
    EconomySubstitutionGroup group;
    group.kind = EconomySubstitutionKind::Consumable;
    group.effectFamily = family;
    group.tier = itemTier;
    return group;
}

EconomySubstitutionGroup EconomySubstitutionGroup::Consumable(ConsumableCapability capability, uint32 minimumUtility)
{
    EconomySubstitutionGroup group;
    group.kind = EconomySubstitutionKind::Consumable;
    group.effectFamily = static_cast<uint32>(capability);
    group.valueBand = minimumUtility;
    return group;
}

EconomySubstitutionGroup EconomySubstitutionGroup::Enhancement(uint32 target, uint32 band)
{
    EconomySubstitutionGroup group;
    group.kind = EconomySubstitutionKind::Enhancement;
    group.enhancementTarget = target;
    group.valueBand = band;
    return group;
}

void PlayerbotEconomyCoordinator::RefreshActor(EconomyActorFacts facts, uint64 now)
{
    std::scoped_lock lock(mutex);
    if (!facts.characterGuid)
        return;

    auto const existing = actors.find(facts.characterGuid);
    if (existing != actors.end())
    {
        bool const actorInvalidated = existing->second.marketId != facts.marketId ||
                                      (existing->second.online && !facts.online) ||
                                      (existing->second.autonomous && !facts.autonomous);
        if (actorInvalidated)
            InvalidateActorLocked(facts.characterGuid, EconomyAssignmentOutcome::CapabilityLost, now);
    }

    // The refreshed facts are authoritative for goods this actor holds: consume any released
    // claim bridges the reported supply now covers, so the same items are never counted twice.
    for (EconomyAssignment& claim : claims)
    {
        if (claim.characterGuid != facts.characterGuid || claim.marketId != facts.marketId ||
            claim.state != EconomyClaimState::Released || !claim.bridgedQuantity)
        {
            continue;
        }

        uint64 reported = 0u;
        for (EconomySupplyFact const& supply : facts.supplies)
        {
            if (supply.group == claim.group &&
                (supply.source == EconomySupplySource::Inventory || supply.source == EconomySupplySource::Mail))
            {
                reported += supply.quantity;
            }
        }
        claim.bridgedQuantity -= static_cast<uint32>(std::min<uint64>(claim.bridgedQuantity, reported));
    }

    actors[facts.characterGuid] = std::move(facts);
    ExpireLocked(now);
    ReleaseExcessClaimsLocked(now);
    SyncChainsLocked(now);
    ReconcileCapabilityBlockersLocked();
    ++generation;
}

void PlayerbotEconomyCoordinator::RefreshMarket(EconomyMarketFacts facts, uint64 now)
{
    std::scoped_lock lock(mutex);
    if (!facts.marketId)
        return;

    markets[facts.marketId] = std::move(facts);
    ReleaseExcessClaimsLocked(now);
    SyncChainsLocked(now);
    ReconcileCapabilityBlockersLocked();
    ++generation;
}

void PlayerbotEconomyCoordinator::RevalidateCapability(EconomyCapabilityObservation observation, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    SyncChainsLocked(now);

    EconomyCapabilityRequirement const& requirement = observation.requirement;
    if (!observation.eligibleCycle || !IsValidRequirement(requirement))
        return;

    GapKey const key{requirement.marketId, requirement.group};
    std::map<GapKey, GapTotals> const gaps = CalculateGapsLocked();
    auto const gap = gaps.find(key);
    bool const unmetDemand = gap != gaps.end() && gap->second.demand > gap->second.supply + gap->second.claimed;
    if (!unmetDemand)
    {
        if (capabilityBlockers.erase(key))
            ++generation;
        return;
    }

    if (HasCapabilityProviderLocked(requirement))
    {
        if (capabilityBlockers.erase(key))
            ++generation;
        return;
    }

    auto existing = capabilityBlockers.find(key);
    if (existing != capabilityBlockers.end() && existing->second.requirement == requirement &&
        existing->second.lastObservedAt == now)
    {
        return;
    }

    EconomyCapabilityBlocker& blocker = capabilityBlockers[key];
    if (existing == capabilityBlockers.end() || blocker.requirement != requirement)
    {
        blocker = {
            .requirement = requirement,
            .consecutiveEligibleCycles = 1u,
            .firstObservedAt = now,
            .lastObservedAt = now,
        };
    }
    else
    {
        if (blocker.consecutiveEligibleCycles < std::numeric_limits<uint32>::max())
            ++blocker.consecutiveEligibleCycles;
        blocker.lastObservedAt = now;
    }

    blocker.state = blocker.consecutiveEligibleCycles >= PLAYERBOT_ECONOMY_CAPABILITY_PERSISTENCE_THRESHOLD
                        ? EconomyCapabilityBlockerState::Persistent
                        : EconomyCapabilityBlockerState::Observing;
    AssignCapabilityOwnerLocked(blocker);
    ++generation;
}

EconomyAssignmentLease PlayerbotEconomyCoordinator::Lease(EconomyAssignmentRequest request, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    SyncChainsLocked(now);

    auto const actor = actors.find(request.characterGuid);
    if (actor == actors.end())
        return RejectLocked(EconomyWorkBlocker::UnknownActor, &request, now);
    if (!actor->second.online)
        return RejectLocked(EconomyWorkBlocker::Offline, &request, now);
    if (!actor->second.autonomous && !request.directCommand)
        return RejectLocked(EconomyWorkBlocker::NotAutonomous, &request, now);
    if (!request.marketId || actor->second.marketId != request.marketId)
        return RejectLocked(EconomyWorkBlocker::WrongMarket, &request, now);
    if (!request.quantity || request.workIdentity.empty() || !request.expiresAt || request.expiresAt <= now)
        return RejectLocked(EconomyWorkBlocker::Illegal, &request, now);
    if (request.kind == EconomyClaimKind::Purchase && (!actor->second.accountId || !request.sellerAccountId))
        return RejectLocked(EconomyWorkBlocker::AccountIdentityUnavailable, &request, now);

    GapKey const key{request.marketId, request.group};
    EconomyWorkPolicyInput policy = request.safeguards;
    policy.kind = request.workKind;
    policy.economyAffinity = WorkAffinity(actor->second, request);
    policy.directCommand = request.directCommand;
    policy.sameAccountPurchase =
        request.kind == EconomyClaimKind::Purchase && request.sellerAccountId == actor->second.accountId;
    // A gap that has been affinity-starved for the whole relaxation window takes the
    // best remaining candidate instead of leaving its demand stuck forever.
    auto const standing = gapBlockers.find(key);
    policy.affinityRelaxed = standing != gapBlockers.end() &&
                             standing->second.blocker == EconomyWorkBlocker::AffinityTooLow &&
                             now >= standing->second.firstObservedAt + PLAYERBOT_ECONOMY_AFFINITY_RELAXATION_SECONDS;
    EconomyWorkBlocker const policyBlocker = PlayerbotEconomyPolicy::EvaluateWork(policy);
    if (policyBlocker != EconomyWorkBlocker::None)
        return RejectLocked(policyBlocker, &request, now);

    auto gaps = CalculateGapsLocked();
    auto gap = gaps.find(key);
    EconomyChain* chain = EnsureChainLocked(key, now, gap != gaps.end() && gap->second.demand != 0u);
    if (!chain && request.priority != EconomyClaimPriority::Speculation)
        return RejectLocked(EconomyWorkBlocker::Capacity, &request, now);
    if (request.kind == EconomyClaimKind::Purchase)
    {
        GapTotals totals = gap == gaps.end() ? GapTotals{} : gap->second;
        uint64 const purchaseNeed =
            totals.demand > totals.nonAuctionSupply ? totals.demand - totals.nonAuctionSupply : 0u;
        uint64 available = 0u;
        if (request.priority == EconomyClaimPriority::Speculation)
        {
            uint64 const protectedSupply = std::max(purchaseNeed, totals.purchaseClaimed);
            available = totals.auctionSupply > protectedSupply + totals.speculationClaimed
                            ? totals.auctionSupply - protectedSupply - totals.speculationClaimed
                            : 0u;
        }
        else
        {
            uint64 const unclaimedNeed =
                purchaseNeed > totals.purchaseClaimed ? purchaseNeed - totals.purchaseClaimed : 0u;
            uint64 freeSupply = totals.auctionSupply > totals.purchaseClaimed + totals.speculationClaimed
                                    ? totals.auctionSupply - totals.purchaseClaimed - totals.speculationClaimed
                                    : 0u;
            if (freeSupply < unclaimedNeed && totals.speculationClaimed)
            {
                ReleaseSpeculationLocked(key, now);
                gaps = CalculateGapsLocked();
                gap = gaps.find(key);
                totals = gap == gaps.end() ? GapTotals{} : gap->second;
                freeSupply =
                    totals.auctionSupply > totals.purchaseClaimed ? totals.auctionSupply - totals.purchaseClaimed : 0u;
            }
            available = std::min(unclaimedNeed, freeSupply);
        }

        if (!available)
            return RejectLocked(EconomyWorkBlocker::NoDemand, &request, now);

        EconomyAssignment assignment;
        assignment.leaseId = nextLeaseId++;
        if (chain)
            assignment.chainPublicId = chain->publicId;
        assignment.characterGuid = request.characterGuid;
        assignment.marketId = request.marketId;
        assignment.group = request.group;
        assignment.quantity = BoundedQuantity(std::min<uint64>(request.quantity, available));
        assignment.kind = request.kind;
        assignment.priority = request.priority;
        assignment.directCommand = request.directCommand;
        assignment.workIdentity = std::move(request.workIdentity);
        assignment.createdAt = now;
        assignment.expiresAt = request.expiresAt;
        // The request's recipe only identifies the crafting work a purchase feeds; a recipe on
        // the claim itself means production, and telemetry consumers reject it elsewhere.
        claims.push_back(assignment);
        gapBlockers.erase(key);
        AppendClaimEventLocked(assignment, EconomyChainStage::Claim, EconomyChainOutcome::Progress,
                               EconomyWorkBlocker::None, now);
        SyncChainsLocked(now);
        ++generation;
        return {assignment, EconomyWorkBlocker::None};
    }

    uint64 remaining = gap == gaps.end() || gap->second.demand <= gap->second.supply + gap->second.claimed
                           ? 0u
                           : gap->second.demand - gap->second.supply - gap->second.claimed;

    if (!remaining && request.priority != EconomyClaimPriority::Speculation)
    {
        ReleaseSpeculationLocked(key, now);
        gaps = CalculateGapsLocked();
        gap = gaps.find(key);
        remaining = gap == gaps.end() || gap->second.demand <= gap->second.supply + gap->second.claimed
                        ? 0u
                        : gap->second.demand - gap->second.supply - gap->second.claimed;
    }

    if (!remaining)
        return RejectLocked(EconomyWorkBlocker::NoDemand, &request, now);

    EconomyAssignment assignment;
    assignment.leaseId = nextLeaseId++;
    if (chain)
        assignment.chainPublicId = chain->publicId;
    assignment.characterGuid = request.characterGuid;
    assignment.marketId = request.marketId;
    assignment.group = request.group;
    assignment.quantity = BoundedQuantity(std::min<uint64>(request.quantity, remaining));
    assignment.kind = request.kind;
    assignment.priority = request.priority;
    assignment.directCommand = request.directCommand;
    assignment.workIdentity = std::move(request.workIdentity);
    assignment.createdAt = now;
    assignment.expiresAt = request.expiresAt;
    assignment.recipeSpellId = request.recipeSpellId;
    assignment.outputItemId = request.outputItemId;
    claims.push_back(assignment);
    gapBlockers.erase(key);
    AppendClaimEventLocked(assignment, EconomyChainStage::Claim, EconomyChainOutcome::Progress,
                           EconomyWorkBlocker::None, now);
    SyncChainsLocked(now);
    ++generation;
    return {assignment, EconomyWorkBlocker::None};
}

EconomyAssignmentLease PlayerbotEconomyCoordinator::AssignProduction(EconomyProductionRequest request, uint64 now)
{
    std::erase_if(request.recipes,
                  [](EconomyProductionRecipe const& recipe) { return !recipe.recipeSpellId || !recipe.outputItemId; });
    if (!request.characterGuid || !request.marketId || request.expiresAt <= now)
        return {std::nullopt, EconomyWorkBlocker::Illegal};

    {
        std::scoped_lock lock(mutex);
        ExpireLocked(now);
        SyncChainsLocked(now);

        EconomyActorFacts const* actor = nullptr;
        if (auto const existing = actors.find(request.characterGuid); existing != actors.end())
            actor = &existing->second;

        EconomyAssignment* retained = nullptr;
        for (EconomyAssignment& claim : claims)
        {
            if (claim.characterGuid != request.characterGuid || claim.kind != EconomyClaimKind::Production ||
                claim.state != EconomyClaimState::Leased)
            {
                continue;
            }

            bool const stillAvailable =
                actor && actor->online && actor->autonomous && actor->marketId == request.marketId &&
                claim.marketId == request.marketId && Contains(actor->recipeSpellIds, claim.recipeSpellId) &&
                std::any_of(request.recipes.begin(), request.recipes.end(),
                            [&claim](EconomyProductionRecipe const& recipe)
                            {
                                return recipe.group == claim.group && recipe.recipeSpellId == claim.recipeSpellId &&
                                       recipe.outputItemId == claim.outputItemId;
                            });
            if (!retained && stillAvailable)
            {
                retained = &claim;
                continue;
            }
            ApplyOutcomeLocked(
                claim,
                stillAvailable ? EconomyAssignmentOutcome::NeedChanged : EconomyAssignmentOutcome::CapabilityLost,
                claim.committedQuantity, now);
        }

        if (retained)
        {
            retained->expiresAt = request.expiresAt;
            SyncChainsLocked(now);
            ++generation;
            return {*retained, EconomyWorkBlocker::None};
        }
    }

    if (request.recipes.empty())
        return {std::nullopt, EconomyWorkBlocker::NoDemand};

    EconomyCoordinatorSnapshot const snapshot = Snapshot(now);
    auto const actor =
        std::find_if(snapshot.actors.begin(), snapshot.actors.end(), [&request](EconomyActorFacts const& value)
                     { return value.characterGuid == request.characterGuid; });
    if (actor == snapshot.actors.end())
        return {std::nullopt, EconomyWorkBlocker::UnknownActor};

    for (EconomyDemandGap const& gap : snapshot.gaps)
    {
        if (gap.marketId != request.marketId || !gap.remainingQuantity)
            continue;

        auto const recipe = std::find_if(
            request.recipes.begin(), request.recipes.end(), [&gap, &actor](EconomyProductionRecipe const& candidate)
            { return candidate.group == gap.group && Contains(actor->recipeSpellIds, candidate.recipeSpellId); });
        if (recipe == request.recipes.end())
            continue;

        EconomyAssignmentRequest assignment;
        assignment.characterGuid = request.characterGuid;
        assignment.marketId = request.marketId;
        assignment.group = recipe->group;
        assignment.quantity =
            recipe->maxQuantity ? std::min(gap.remainingQuantity, recipe->maxQuantity) : gap.remainingQuantity;
        assignment.kind = EconomyClaimKind::Production;
        assignment.priority = EconomyClaimPriority::Producer;
        assignment.workKind = EconomyWorkKind::Craft;
        assignment.workIdentity =
            "craft:" + std::to_string(recipe->recipeSpellId) + ":" + std::to_string(recipe->outputItemId);
        assignment.expiresAt = request.expiresAt;
        assignment.recipeSpellId = recipe->recipeSpellId;
        assignment.outputItemId = recipe->outputItemId;
        assignment.safeguards = request.safeguards;
        return Lease(std::move(assignment), now);
    }

    return {std::nullopt, EconomyWorkBlocker::NoDemand};
}

EconomyProductionOutput PlayerbotEconomyCoordinator::RecordProductionInventory(uint64 leaseId, uint32 startingQuantity,
                                                                               uint32 currentQuantity, uint64 now)
{
    if (currentQuantity <= startingQuantity)
        return {};

    return RecordProductionOutput(leaseId, currentQuantity - startingQuantity, now);
}

EconomyProductionOutput PlayerbotEconomyCoordinator::RecordProductionOutput(uint64 leaseId, uint32 producedQuantity,
                                                                            uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    auto const claim = std::find_if(claims.begin(), claims.end(), [leaseId](EconomyAssignment const& candidate)
                                    { return candidate.leaseId == leaseId; });
    if (!producedQuantity || claim == claims.end() || claim->kind != EconomyClaimKind::Production ||
        claim->state != EconomyClaimState::Leased)
    {
        return {};
    }

    uint32 const committedQuantity = BoundedQuantity(
        std::min<uint64>(claim->quantity, static_cast<uint64>(claim->committedQuantity) + producedQuantity));
    bool const completed = committedQuantity >= claim->quantity;
    ApplyOutcomeLocked(*claim, completed ? EconomyAssignmentOutcome::Completed : EconomyAssignmentOutcome::Committed,
                       committedQuantity, now);
    SyncChainsLocked(now);
    ReconcileCapabilityBlockersLocked();
    ++generation;
    return {
        .recorded = true,
        .completed = completed,
        .producedQuantity = producedQuantity,
        .committedQuantity = committedQuantity,
    };
}

bool PlayerbotEconomyCoordinator::RecordOutcome(uint64 leaseId, EconomyAssignmentOutcome outcome,
                                                uint32 committedQuantity, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    auto const claim = std::find_if(claims.begin(), claims.end(), [leaseId](EconomyAssignment const& candidate)
                                    { return candidate.leaseId == leaseId; });
    if (claim == claims.end() || claim->state != EconomyClaimState::Leased)
        return false;

    ApplyOutcomeLocked(*claim, outcome, committedQuantity, now);
    SyncChainsLocked(now);
    ReconcileCapabilityBlockersLocked();
    ++generation;
    return true;
}

void PlayerbotEconomyCoordinator::InvalidateActor(uint32 characterGuid, EconomyAssignmentOutcome outcome, uint64 now)
{
    std::scoped_lock lock(mutex);
    InvalidateActorLocked(characterGuid, outcome, now);
    SyncChainsLocked(now);
    ReconcileCapabilityBlockersLocked();
    ++generation;
}

void PlayerbotEconomyCoordinator::Expire(uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    SyncChainsLocked(now);
    ReconcileCapabilityBlockersLocked();
}

EconomyCoordinatorSnapshot PlayerbotEconomyCoordinator::Snapshot(uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    SyncChainsLocked(now);

    EconomyCoordinatorSnapshot snapshot;
    snapshot.generation = generation;
    snapshot.actors.reserve(actors.size());
    for (auto const& [characterGuid, actor] : actors)
    {
        (void)characterGuid;
        snapshot.actors.push_back(actor);
    }
    for (auto const& [key, totals] : CalculateGapsLocked())
    {
        if (!totals.demand)
            continue;

        EconomyDemandGap gap;
        gap.marketId = key.first;
        gap.group = key.second;
        gap.demandQuantity = BoundedQuantity(totals.demand);
        gap.supplyQuantity = BoundedQuantity(totals.supply);
        gap.claimedQuantity = BoundedQuantity(totals.claimed);
        gap.remainingQuantity = totals.demand > totals.supply + totals.claimed
                                    ? BoundedQuantity(totals.demand - totals.supply - totals.claimed)
                                    : 0u;
        snapshot.gaps.push_back(gap);
    }
    snapshot.claims = claims;
    std::map<EconomyWorkBlocker, uint32> blockerCounts;
    for (auto const& [key, condition] : gapBlockers)
    {
        (void)key;
        ++blockerCounts[condition.blocker];
    }
    for (auto const& [blocker, count] : blockerCounts)
        snapshot.blockers.push_back({blocker, count});
    for (auto const& [key, blocker] : capabilityBlockers)
    {
        (void)key;
        snapshot.capabilityBlockers.push_back(blocker);
    }
    snapshot.chains = chains;
    return snapshot;
}

bool PlayerbotEconomyCoordinator::HasCapabilityProviderLocked(EconomyCapabilityRequirement const& requirement) const
{
    ProfessionCapability const& capability = requirement.capability;
    return std::any_of(actors.begin(), actors.end(),
                       [&requirement, &capability](auto const& entry)
                       {
                           EconomyActorFacts const& actor = entry.second;
                           if (!actor.online || !actor.autonomous || actor.marketId != requirement.marketId ||
                               !Contains(actor.professionSkillIds, capability.professionSkillId))
                           {
                               return false;
                           }
                           return capability.kind == ProfessionCapabilityKind::Gathering ||
                                  Contains(actor.recipeSpellIds, capability.recipeSpellId);
                       });
}

void PlayerbotEconomyCoordinator::AssignCapabilityOwnerLocked(EconomyCapabilityBlocker& blocker) const
{
    blocker.assignedActorGuid = 0u;
    blocker.assignedWorkKind.reset();
    if (blocker.state != EconomyCapabilityBlockerState::Persistent)
        return;

    ProfessionCapability const& capability = blocker.requirement.capability;
    auto const bestCandidate = [this, &blocker, &capability](EconomyWorkKind workKind) -> std::optional<uint32>
    {
        uint32 selectedGuid = 0u;
        uint8 selectedAffinity = 0u;
        for (auto const& [characterGuid, actor] : actors)
        {
            uint8 const affinity = RelevantAffinity(actor, capability.kind);
            if (!actor.online || !actor.autonomous || actor.marketId != blocker.requirement.marketId ||
                affinity < PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM)
            {
                continue;
            }

            bool const hasSkill = Contains(actor.professionSkillIds, capability.professionSkillId);
            bool eligible = false;
            if (workKind == EconomyWorkKind::Recipe)
            {
                eligible = capability.kind == ProfessionCapabilityKind::Crafting && hasSkill &&
                           !Contains(actor.recipeSpellIds, capability.recipeSpellId);
            }
            else if (workKind == EconomyWorkKind::Trainer)
                eligible = !hasSkill && actor.freePrimaryProfessionSlots != 0u;
            if (!eligible)
                continue;

            if (!selectedGuid || affinity > selectedAffinity ||
                (affinity == selectedAffinity && characterGuid < selectedGuid))
            {
                selectedGuid = characterGuid;
                selectedAffinity = affinity;
            }
        }
        return selectedGuid ? std::optional<uint32>(selectedGuid) : std::nullopt;
    };

    if (std::optional<uint32> const recipeOwner = bestCandidate(EconomyWorkKind::Recipe))
    {
        blocker.assignedActorGuid = *recipeOwner;
        blocker.assignedWorkKind = EconomyWorkKind::Recipe;
        return;
    }
    if (std::optional<uint32> const trainerOwner = bestCandidate(EconomyWorkKind::Trainer))
    {
        blocker.assignedActorGuid = *trainerOwner;
        blocker.assignedWorkKind = EconomyWorkKind::Trainer;
    }
}

void PlayerbotEconomyCoordinator::ReconcileCapabilityBlockersLocked()
{
    std::map<GapKey, GapTotals> const gaps = CalculateGapsLocked();
    for (auto blocker = capabilityBlockers.begin(); blocker != capabilityBlockers.end();)
    {
        auto const gap = gaps.find(blocker->first);
        bool const unmetDemand = gap != gaps.end() && gap->second.demand > gap->second.supply + gap->second.claimed;
        if (!unmetDemand || HasCapabilityProviderLocked(blocker->second.requirement))
        {
            blocker = capabilityBlockers.erase(blocker);
            continue;
        }

        AssignCapabilityOwnerLocked(blocker->second);
        ++blocker;
    }
}

void PlayerbotEconomyCoordinator::ExpireLocked(uint64 now)
{
    bool changed = false;
    for (EconomyAssignment& claim : claims)
    {
        if (claim.state != EconomyClaimState::Leased || !claim.expiresAt || claim.expiresAt > now)
            continue;

        ApplyOutcomeLocked(claim, EconomyAssignmentOutcome::NeedChanged, claim.committedQuantity, now);
        changed = true;
    }
    if (std::erase_if(claims,
                      [now](EconomyAssignment const& claim)
                      {
                          return claim.state != EconomyClaimState::Leased && claim.settledAt &&
                                 now >= claim.settledAt + PLAYERBOT_ECONOMY_CLAIM_RETENTION_SECONDS;
                      }))
    {
        changed = true;
    }
    if (changed)
        ++generation;
}

void PlayerbotEconomyCoordinator::InvalidateActorLocked(uint32 characterGuid, EconomyAssignmentOutcome outcome,
                                                        uint64 now)
{
    for (EconomyAssignment& claim : claims)
    {
        if (claim.characterGuid != characterGuid || claim.state != EconomyClaimState::Leased)
            continue;

        ApplyOutcomeLocked(claim, outcome, claim.committedQuantity, now);
    }
}

void PlayerbotEconomyCoordinator::ReleaseSpeculationLocked(GapKey const& key, uint64 now)
{
    for (EconomyAssignment& claim : claims)
    {
        if (claim.marketId != key.first || claim.group != key.second ||
            claim.priority != EconomyClaimPriority::Speculation || claim.state != EconomyClaimState::Leased)
        {
            continue;
        }

        ApplyOutcomeLocked(claim, EconomyAssignmentOutcome::NeedChanged, claim.committedQuantity, now);
    }
}

void PlayerbotEconomyCoordinator::ReleaseExcessClaimsLocked(uint64 now)
{
    auto gaps = CalculateGapsLocked();
    for (auto const& [key, totals] : gaps)
    {
        uint64 const purchaseNeed =
            totals.demand > totals.nonAuctionSupply ? totals.demand - totals.nonAuctionSupply : 0u;
        uint64 const speculationCapacity =
            totals.auctionSupply > purchaseNeed ? totals.auctionSupply - purchaseNeed : 0u;
        if (totals.speculationClaimed > speculationCapacity)
        {
            uint64 excess = totals.speculationClaimed - speculationCapacity;
            for (auto claim = claims.rbegin(); claim != claims.rend() && excess; ++claim)
            {
                if (claim->marketId != key.first || claim->group != key.second ||
                    claim->kind != EconomyClaimKind::Purchase || claim->priority != EconomyClaimPriority::Speculation ||
                    claim->state != EconomyClaimState::Leased)
                {
                    continue;
                }
                ApplyOutcomeLocked(*claim, EconomyAssignmentOutcome::NeedChanged, claim->committedQuantity, now);
                excess = excess > claim->quantity ? excess - claim->quantity : 0u;
            }
        }

        uint64 const purchaseCapacity = std::min(purchaseNeed, totals.auctionSupply);
        if (totals.purchaseClaimed > purchaseCapacity)
        {
            uint64 excess = totals.purchaseClaimed - purchaseCapacity;
            for (auto claim = claims.rbegin(); claim != claims.rend() && excess; ++claim)
            {
                if (claim->marketId != key.first || claim->group != key.second ||
                    claim->kind != EconomyClaimKind::Purchase || claim->priority == EconomyClaimPriority::Speculation ||
                    claim->state != EconomyClaimState::Leased)
                {
                    continue;
                }
                ApplyOutcomeLocked(*claim, EconomyAssignmentOutcome::NeedChanged, claim->committedQuantity, now);
                excess = excess > claim->quantity ? excess - claim->quantity : 0u;
            }
        }

        uint64 const needed = totals.demand > totals.supply ? totals.demand - totals.supply : 0u;
        if (totals.claimed <= needed)
            continue;

        // Trim the claims carrying the least committed progress first, so nearly finished
        // work survives a shrinking gap.
        std::vector<EconomyAssignment*> trimmable;
        for (EconomyAssignment& claim : claims)
        {
            if (claim.marketId != key.first || claim.group != key.second || claim.kind == EconomyClaimKind::Purchase ||
                claim.state != EconomyClaimState::Leased)
            {
                continue;
            }
            trimmable.push_back(&claim);
        }
        std::stable_sort(trimmable.begin(), trimmable.end(),
                         [](EconomyAssignment const* left, EconomyAssignment const* right)
                         {
                             if (left->committedQuantity != right->committedQuantity)
                                 return left->committedQuantity < right->committedQuantity;
                             return left->createdAt > right->createdAt;
                         });

        // Shrink claims by only their uncommitted excess instead of releasing them whole:
        // a small demand dip must not kill an active order that is still mostly needed.
        uint64 excess = totals.claimed - needed;
        for (EconomyAssignment* claim : trimmable)
        {
            if (!excess)
                break;

            uint32 const transferable = claim->quantity - claim->committedQuantity;
            if (!transferable)
                continue;

            uint32 const reduction = static_cast<uint32>(std::min<uint64>(transferable, excess));
            excess -= reduction;
            if (reduction < transferable)
            {
                claim->quantity -= reduction;
                continue;
            }

            // Nothing uncommitted remains wanted: settle the claim. Quantity stays at the
            // committed amount, or untouched when nothing was committed, so a claim record
            // never carries a zero quantity (the telemetry contract requires positive).
            if (claim->committedQuantity)
                claim->quantity = claim->committedQuantity;
            ApplyOutcomeLocked(*claim, EconomyAssignmentOutcome::NeedChanged, claim->committedQuantity, now);
        }
    }
}

void PlayerbotEconomyCoordinator::ApplyOutcomeLocked(EconomyAssignment& claim, EconomyAssignmentOutcome outcome,
                                                     uint32 committedQuantity, uint64 now)
{
    uint32 const priorCommitted = claim.committedQuantity;
    EconomyClaimState const priorState = claim.state;
    EconomyAssignmentOutcome const priorOutcome = claim.lastOutcome;
    claim.committedQuantity = std::max(claim.committedQuantity, std::min(committedQuantity, claim.quantity));
    claim.lastOutcome = outcome;
    if (priorState == EconomyClaimState::Leased && outcome != EconomyAssignmentOutcome::Committed)
    {
        claim.settledAt = now;
        // Committed output still exists in the world after a release, so it keeps suppressing
        // demand until the producer's refreshed facts carry the items (RefreshActor consumes
        // the bridge) or the retention window erases the claim.
        bool const bridgesSupply = claim.kind != EconomyClaimKind::Purchase &&
                                   outcome != EconomyAssignmentOutcome::Completed &&
                                   outcome != EconomyAssignmentOutcome::InventoryReceived;
        claim.bridgedQuantity = bridgesSupply ? claim.committedQuantity : 0u;
    }

    EconomyChainStage stage = EconomyChainStage::Commit;
    EconomyChainOutcome chainOutcome = EconomyChainOutcome::Progress;
    switch (outcome)
    {
        case EconomyAssignmentOutcome::Committed:
            break;
        case EconomyAssignmentOutcome::Completed:
            claim.committedQuantity = claim.quantity;
            claim.state = EconomyClaimState::Completed;
            stage = EconomyChainStage::Deliver;
            break;
        case EconomyAssignmentOutcome::InventoryReceived:
            claim.state = EconomyClaimState::Released;
            stage = EconomyChainStage::Deliver;
            break;
        case EconomyAssignmentOutcome::FailedTravel:
        case EconomyAssignmentOutcome::FailedPurchase:
        case EconomyAssignmentOutcome::CapabilityLost:
            claim.state = EconomyClaimState::Released;
            stage = EconomyChainStage::Release;
            chainOutcome = EconomyChainOutcome::Failed;
            break;
        case EconomyAssignmentOutcome::NeedChanged:
        case EconomyAssignmentOutcome::LoggedOut:
        case EconomyAssignmentOutcome::Disabled:
            claim.state = EconomyClaimState::Released;
            stage = EconomyChainStage::Release;
            chainOutcome = EconomyChainOutcome::Released;
            break;
    }

    if (priorCommitted == claim.committedQuantity && priorState == claim.state && priorOutcome == claim.lastOutcome)
        return;

    AppendClaimEventLocked(claim, stage, chainOutcome, EconomyWorkBlocker::None, now);
}

std::map<PlayerbotEconomyCoordinator::GapKey, PlayerbotEconomyCoordinator::GapTotals>
PlayerbotEconomyCoordinator::CalculateGapsLocked() const
{
    std::map<GapKey, GapTotals> gaps;
    for (auto const& [characterGuid, actor] : actors)
    {
        (void)characterGuid;
        if (actor.online && actor.autonomous)
        {
            for (EconomyDemandFact const& demand : actor.demands)
                gaps[{actor.marketId, demand.group}].demand += demand.quantity;
        }

        for (EconomySupplyFact const& supply : actor.supplies)
        {
            gaps[{actor.marketId, supply.group}].supply += supply.quantity;
            gaps[{actor.marketId, supply.group}].nonAuctionSupply += supply.quantity;
        }
    }

    for (auto const& [marketId, market] : markets)
    {
        for (EconomySupplyFact const& supply : market.supplies)
        {
            gaps[{marketId, supply.group}].supply += supply.quantity;
            gaps[{marketId, supply.group}].auctionSupply += supply.quantity;
        }
    }

    for (EconomyAssignment const& claim : claims)
    {
        GapTotals& gap = gaps[{claim.marketId, claim.group}];
        if (claim.kind == EconomyClaimKind::Purchase)
        {
            if (claim.state == EconomyClaimState::Leased)
            {
                if (claim.priority == EconomyClaimPriority::Speculation)
                    gap.speculationClaimed += claim.quantity;
                else
                    gap.purchaseClaimed += claim.quantity;
            }
            continue;
        }
        if (claim.state == EconomyClaimState::Leased)
        {
            gap.supply += claim.committedQuantity;
            gap.claimed += claim.quantity - claim.committedQuantity;
            gap.nonAuctionSupply += claim.quantity;
        }
        else if (claim.state == EconomyClaimState::Released && claim.bridgedQuantity)
        {
            gap.supply += claim.bridgedQuantity;
            gap.nonAuctionSupply += claim.bridgedQuantity;
        }
    }
    return gaps;
}

EconomyAssignmentLease PlayerbotEconomyCoordinator::RejectLocked(EconomyWorkBlocker blocker,
                                                                 EconomyAssignmentRequest const* request, uint64 now)
{
    // A satisfied gap turning work away is the system operating, not a circulation blocker,
    // so routine NoDemand rejections stay out of both the blocker map and the chain history.
    if (request && blocker != EconomyWorkBlocker::NoDemand)
    {
        GapBlockerCondition& condition = gapBlockers[{request->marketId, request->group}];
        if (condition.blocker != blocker)
            condition = {blocker, now};
        auto const active = activeChainIds.find({request->marketId, request->group});
        if (active != activeChainIds.end())
        {
            EconomyChain* chain = FindChainLocked(active->second);
            if (chain)
            {
                AppendChainEventLocked(*chain, {
                                                   .occurredAt = now,
                                                   .actorGuid = request->characterGuid,
                                                   .stage = EconomyChainStage::Blocked,
                                                   .outcome = EconomyChainOutcome::Blocked,
                                                   .claimKind = request->kind,
                                                   .blocker = blocker,
                                                   .quantity = request->quantity,
                                                   .remainingQuantity = chain->remainingQuantity,
                                                   .workIdentity = request->workIdentity,
                                               });
            }
        }
    }
    ++generation;
    return {std::nullopt, blocker};
}

PlayerbotEconomyCoordinator& PlayerbotEconomy::GetPlayerbotEconomyCoordinator()
{
    static PlayerbotEconomyCoordinator coordinator;
    return coordinator;
}
