/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Personality/PlayerbotPersonality.h"
#include "Random.h"
#include "StringFormat.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr uint64 CHAIN_ID_NAMESPACE = 0xa21f745b61c8d309ULL;

uint32 BoundedQuantity(uint64 quantity)
{
    return static_cast<uint32>(std::min<uint64>(quantity, std::numeric_limits<uint32>::max()));
}

std::string ChainPublicId(uint64 sequence, uint64 now)
{
    uint64 const entropy = (static_cast<uint64>(rand32()) << 32u) | rand32();
    uint64 const opaque = PlayerbotPersonality::SplitMix64(entropy ^ sequence ^ now ^ CHAIN_ID_NAMESPACE);
    return Acore::StringFormat("chn_{:016x}", opaque);
}
}  // namespace

EconomyActorChainObservation PlayerbotEconomyCoordinator::ObserveActor(uint32 characterGuid, uint64 now)
{
    std::scoped_lock lock(mutex);
    ExpireLocked(now);
    SyncChainsLocked(now);

    auto const actor = actors.find(characterGuid);
    if (actor == actors.end())
        return {};

    std::optional<EconomyCapabilityBlocker> assignedCapability;
    for (auto const& [key, blocker] : capabilityBlockers)
    {
        (void)key;
        if (blocker.assignedActorGuid == characterGuid)
        {
            assignedCapability = blocker;
            break;
        }
    }
    auto attachCapability = [this, &assignedCapability](EconomyActorChainObservation& observation)
    {
        if (assignedCapability)
        {
            observation.capabilityBlocker = assignedCapability;
            return;
        }
        auto const blocker = capabilityBlockers.find({observation.marketId, observation.group});
        if (blocker != capabilityBlockers.end())
            observation.capabilityBlocker = blocker->second;
    };

    auto observeClaim = [this, characterGuid, now](bool leasedOnly) -> std::optional<EconomyActorChainObservation>
    {
        auto const claim = std::find_if(claims.rbegin(), claims.rend(),
                                        [characterGuid, leasedOnly](EconomyAssignment const& candidate)
                                        {
                                            return candidate.characterGuid == characterGuid &&
                                                   !candidate.chainPublicId.empty() &&
                                                   (!leasedOnly || candidate.state == EconomyClaimState::Leased);
                                        });
        if (claim == claims.rend())
            return std::nullopt;

        EconomyChain* chain = FindChainLocked(claim->chainPublicId);
        if (!chain)
            return std::nullopt;

        return EconomyActorChainObservation{
            .available = true,
            .chainPublicId = chain->publicId,
            .workIdentity = claim->workIdentity,
            .marketId = chain->marketId,
            .group = chain->group,
            .remainingQuantity = chain->remainingQuantity,
            .claimAgeSeconds = now > claim->createdAt ? now - claim->createdAt : 0u,
            .claimState = claim->state,
            .assignmentOutcome = claim->lastOutcome,
        };
    };

    if (auto const leasedClaim = observeClaim(true))
    {
        EconomyActorChainObservation observation = *leasedClaim;
        attachCapability(observation);
        return observation;
    }

    auto const activeChain = std::find_if(
        chains.rbegin(), chains.rend(),
        [characterGuid](EconomyChain const& candidate)
        {
            return candidate.active && std::find(candidate.consumerGuids.begin(), candidate.consumerGuids.end(),
                                                 characterGuid) != candidate.consumerGuids.end();
        });
    if (activeChain != chains.rend())
    {
        EconomyActorChainObservation observation = {
            .available = true,
            .chainPublicId = activeChain->publicId,
            .marketId = activeChain->marketId,
            .group = activeChain->group,
            .remainingQuantity = activeChain->remainingQuantity,
            .claimAgeSeconds = now > activeChain->createdAt ? now - activeChain->createdAt : 0u,
        };
        attachCapability(observation);
        return observation;
    }

    if (auto const historicalClaim = observeClaim(false))
    {
        EconomyActorChainObservation observation = *historicalClaim;
        attachCapability(observation);
        return observation;
    }
    if (assignedCapability)
    {
        return {
            .available = true,
            .marketId = assignedCapability->requirement.marketId,
            .group = assignedCapability->requirement.group,
            .capabilityBlocker = assignedCapability,
        };
    }
    return {};
}

void PlayerbotEconomyCoordinator::SyncChainsLocked(uint64 now)
{
    std::map<GapKey, GapTotals> const gaps = CalculateGapsLocked();
    // A blocker row describes work that is currently stuck: once its gap is gone or fully
    // covered by supply and claims, the condition no longer holds and the row goes with it.
    for (auto blocker = gapBlockers.begin(); blocker != gapBlockers.end();)
    {
        auto const gap = gaps.find(blocker->first);
        bool const unmetDemand = gap != gaps.end() && gap->second.demand > gap->second.supply + gap->second.claimed;
        blocker = unmetDemand ? std::next(blocker) : gapBlockers.erase(blocker);
    }
    std::set<GapKey> demandKeys;
    for (auto const& [key, totals] : gaps)
    {
        if (!totals.demand)
            continue;

        demandKeys.insert(key);
        EconomyChain* chain = EnsureChainLocked(key, now, true);
        if (!chain)
            continue;

        uint32 const demandQuantity = BoundedQuantity(totals.demand);
        uint32 const supplyQuantity = BoundedQuantity(totals.supply);
        uint32 const claimedQuantity = BoundedQuantity(totals.claimed + totals.purchaseClaimed);
        uint32 const remainingQuantity = totals.demand > totals.supply + totals.claimed
                                             ? BoundedQuantity(totals.demand - totals.supply - totals.claimed)
                                             : 0u;
        std::vector<uint32> consumerGuids;
        for (auto const& [characterGuid, actor] : actors)
        {
            if (!actor.online || !actor.autonomous || actor.marketId != key.first)
                continue;
            if (std::any_of(actor.demands.begin(), actor.demands.end(), [&key](EconomyDemandFact const& demand)
                            { return demand.group == key.second && demand.quantity != 0u; }))
            {
                consumerGuids.push_back(characterGuid);
            }
        }

        bool const changed = chain->demandQuantity != demandQuantity || chain->supplyQuantity != supplyQuantity ||
                             chain->claimedQuantity != claimedQuantity ||
                             chain->remainingQuantity != remainingQuantity || chain->consumerGuids != consumerGuids;
        chain->demandQuantity = demandQuantity;
        chain->supplyQuantity = supplyQuantity;
        chain->claimedQuantity = claimedQuantity;
        chain->remainingQuantity = remainingQuantity;
        chain->consumerGuids = std::move(consumerGuids);
        if (changed)
            chain->updatedAt = now;
        if (chain->totalHistoryCount == 1u && !chain->history.empty() &&
            chain->history.back().stage == EconomyChainStage::Demand)
        {
            chain->history.back().quantity = chain->demandQuantity;
            chain->history.back().remainingQuantity = chain->remainingQuantity;
        }
    }

    for (auto chainId = activeChainIds.begin(); chainId != activeChainIds.end();)
    {
        if (demandKeys.contains(chainId->first))
        {
            ++chainId;
            continue;
        }

        EconomyChain* chain = FindChainLocked(chainId->second);
        if (chain)
        {
            EconomyChainOutcome const outcome =
                chain->remainingQuantity == 0u ? EconomyChainOutcome::Completed : EconomyChainOutcome::Released;
            chain->active = false;
            chain->updatedAt = now;
            chain->completedAt = now;
            chain->remainingQuantity = 0u;
            AppendChainEventLocked(*chain, {
                                               .occurredAt = now,
                                               .stage = EconomyChainStage::Complete,
                                               .outcome = outcome,
                                           });
        }
        chainId = activeChainIds.erase(chainId);
    }
}

EconomyChain* PlayerbotEconomyCoordinator::EnsureChainLocked(GapKey const& key, uint64 now, bool hasDemand)
{
    auto const active = activeChainIds.find(key);
    if (active != activeChainIds.end())
        return FindChainLocked(active->second);

    if (!hasDemand)
        return nullptr;

    if (chains.size() >= PLAYERBOT_ECONOMY_CHAIN_CAPACITY)
    {
        auto const removable =
            std::find_if(chains.begin(), chains.end(), [](EconomyChain const& chain) { return !chain.active; });
        if (removable == chains.end())
            return nullptr;
        chains.erase(removable);
    }

    std::string publicId;
    do
    {
        publicId = ChainPublicId(nextChainSequence++, now);
    } while (FindChainLocked(publicId));

    EconomyChain chain;
    chain.publicId = std::move(publicId);
    chain.marketId = key.first;
    chain.group = key.second;
    chain.createdAt = now;
    chain.updatedAt = now;
    chains.push_back(std::move(chain));
    EconomyChain& created = chains.back();
    activeChainIds[key] = created.publicId;
    AppendChainEventLocked(created, {
                                        .occurredAt = now,
                                        .stage = EconomyChainStage::Demand,
                                        .outcome = EconomyChainOutcome::Progress,
                                    });
    return &created;
}

EconomyChain* PlayerbotEconomyCoordinator::FindChainLocked(std::string const& publicId)
{
    auto const chain = std::find_if(chains.begin(), chains.end(), [&publicId](EconomyChain const& candidate)
                                    { return candidate.publicId == publicId; });
    return chain == chains.end() ? nullptr : &*chain;
}

void PlayerbotEconomyCoordinator::AppendChainEventLocked(EconomyChain& chain, EconomyChainEvent event)
{
    chain.updatedAt = std::max(chain.updatedAt, event.occurredAt);
    event.sequence = ++chain.totalHistoryCount;
    if (chain.history.size() == PLAYERBOT_ECONOMY_CHAIN_HISTORY_CAPACITY)
    {
        chain.history.erase(chain.history.begin());
        chain.historyTruncated = true;
    }
    chain.history.push_back(std::move(event));
}

void PlayerbotEconomyCoordinator::AppendClaimEventLocked(EconomyAssignment const& claim, EconomyChainStage stage,
                                                         EconomyChainOutcome outcome, EconomyWorkBlocker blocker,
                                                         uint64 now)
{
    if (claim.chainPublicId.empty())
        return;

    EconomyChain* chain = FindChainLocked(claim.chainPublicId);
    if (!chain)
        return;

    std::map<GapKey, GapTotals> const gaps = CalculateGapsLocked();
    auto const gap = gaps.find({claim.marketId, claim.group});
    uint32 const remaining = gap == gaps.end() || gap->second.demand <= gap->second.supply + gap->second.claimed
                                 ? 0u
                                 : BoundedQuantity(gap->second.demand - gap->second.supply - gap->second.claimed);
    AppendChainEventLocked(*chain, {
                                       .occurredAt = now,
                                       .actorGuid = claim.characterGuid,
                                       .stage = stage,
                                       .outcome = outcome,
                                       .claimKind = claim.kind,
                                       .assignmentOutcome = claim.lastOutcome,
                                       .blocker = blocker,
                                       .quantity = claim.quantity,
                                       .remainingQuantity = remaining,
                                       .workIdentity = claim.workIdentity,
                                   });
}
