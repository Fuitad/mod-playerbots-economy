/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <utility>

#include "Bot/Economy/PlayerbotEconomyCoordinator.h"
#include "Bot/Economy/PlayerbotEconomyGathering.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
EconomyActorFacts Actor(uint32 characterGuid, uint32 accountId, uint32 marketId, uint8 affinity = 100)
{
    EconomyActorFacts actor;
    actor.characterGuid = characterGuid;
    actor.accountId = accountId;
    actor.marketId = marketId;
    actor.online = true;
    actor.autonomous = true;
    actor.economyAffinity = affinity;
    return actor;
}

EconomyAssignmentRequest Request(uint32 characterGuid, uint32 marketId, EconomySubstitutionGroup group, uint32 quantity)
{
    EconomyAssignmentRequest request;
    request.characterGuid = characterGuid;
    request.marketId = marketId;
    request.group = group;
    request.quantity = quantity;
    request.kind = EconomyClaimKind::Production;
    request.priority = EconomyClaimPriority::Producer;
    request.workKind = EconomyWorkKind::Craft;
    request.workIdentity = "craft:100";
    request.expiresAt = 200;
    return request;
}

EconomyCapabilityRequirement CraftingRequirement(uint32 marketId, EconomySubstitutionGroup group, uint16 skillId,
                                                 uint32 recipeSpellId, uint32 outputItemId)
{
    return {
        .marketId = marketId,
        .group = group,
        .capability =
            {
                .outputItemId = outputItemId,
                .recipeSpellId = recipeSpellId,
                .professionSkillId = skillId,
                .kind = ProfessionCapabilityKind::Crafting,
                .primaryProfession = true,
            },
    };
}

EconomyCapabilityRequirement GatheringRequirement(uint32 marketId, EconomySubstitutionGroup group, uint16 skillId,
                                                  uint32 outputItemId)
{
    return {
        .marketId = marketId,
        .group = group,
        .capability =
            {
                .outputItemId = outputItemId,
                .professionSkillId = skillId,
                .kind = ProfessionCapabilityKind::Gathering,
                .primaryProfession = true,
            },
    };
}
}  // namespace

TEST(PlayerbotEconomyCoordinatorTest, CapabilityCountsOnlyExplicitEligibleCyclesAndReadCopiesStayStable)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const bags = EconomySubstitutionGroup::Bag(12u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({bags, 4u});
    coordinator.RefreshActor(consumer, 100u);

    EconomyActorFacts candidate = Actor(20u, 20u, 2u);
    candidate.craftingAffinity = 75u;
    candidate.freePrimaryProfessionSlots = 1u;
    coordinator.RefreshActor(candidate, 100u);

    EconomyCapabilityRequirement const tailoring = CraftingRequirement(2u, bags, 197u, 12044u, 10045u);
    coordinator.RevalidateCapability({tailoring, false}, 101u);
    EXPECT_TRUE(coordinator.Snapshot(101u).capabilityBlockers.empty());

    coordinator.RevalidateCapability({tailoring, true}, 102u);
    EconomyCoordinatorSnapshot first = coordinator.Snapshot(102u);
    ASSERT_EQ(first.actors.size(), 2u);
    auto const copiedCandidate =
        std::find_if(first.actors.begin(), first.actors.end(),
                     [](EconomyActorFacts const& actor) { return actor.characterGuid == 20u; });
    ASSERT_NE(copiedCandidate, first.actors.end());
    EXPECT_EQ(copiedCandidate->craftingAffinity, 75u);
    EXPECT_EQ(copiedCandidate->freePrimaryProfessionSlots, 1u);
    ASSERT_EQ(first.capabilityBlockers.size(), 1u);
    EXPECT_EQ(first.capabilityBlockers.front().state, EconomyCapabilityBlockerState::Observing);
    EXPECT_EQ(first.capabilityBlockers.front().consecutiveEligibleCycles, 1u);
    EXPECT_EQ(first.capabilityBlockers.front().assignedActorGuid, 0u);

    coordinator.RevalidateCapability({tailoring, true}, 102u);
    EconomyCoordinatorSnapshot const duplicate = coordinator.Snapshot(102u);
    ASSERT_EQ(duplicate.capabilityBlockers.size(), 1u);
    EXPECT_EQ(duplicate.capabilityBlockers.front().consecutiveEligibleCycles, 1u);

    coordinator.RevalidateCapability({tailoring, true}, 103u);
    EconomyCoordinatorSnapshot const persistent = coordinator.Snapshot(103u);
    ASSERT_EQ(persistent.capabilityBlockers.size(), 1u);
    EXPECT_EQ(persistent.capabilityBlockers.front().state, EconomyCapabilityBlockerState::Persistent);
    EXPECT_EQ(persistent.capabilityBlockers.front().consecutiveEligibleCycles,
              PLAYERBOT_ECONOMY_CAPABILITY_PERSISTENCE_THRESHOLD);
    EXPECT_EQ(persistent.capabilityBlockers.front().assignedActorGuid, 20u);
    EXPECT_EQ(persistent.capabilityBlockers.front().assignedWorkKind,
              std::optional<EconomyWorkKind>(EconomyWorkKind::Trainer));

    EconomyActorChainObservation const observed = coordinator.ObserveActor(20u, 500u);
    ASSERT_TRUE(observed.capabilityBlocker.has_value());
    EXPECT_EQ(observed.capabilityBlocker, persistent.capabilityBlockers.front());
    EXPECT_EQ(coordinator.Snapshot(500u).capabilityBlockers, persistent.capabilityBlockers);

    coordinator.RevalidateCapability({tailoring, true}, 104u);
    EXPECT_EQ(persistent.capabilityBlockers.front().consecutiveEligibleCycles, 2u);
    EconomyCoordinatorSnapshot const advanced = coordinator.Snapshot(104u);
    ASSERT_EQ(advanced.capabilityBlockers.size(), 1u);
    EXPECT_EQ(advanced.capabilityBlockers.front().consecutiveEligibleCycles, 3u);
}

TEST(PlayerbotEconomyCoordinatorTest, CapabilityIdentityChangeDoesNotInheritConsecutiveCycles)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const bags = EconomySubstitutionGroup::Bag(12u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({bags, 4u});
    coordinator.RefreshActor(consumer, 100u);

    EconomyCapabilityRequirement const shirts = CraftingRequirement(2u, bags, 197u, 12044u, 10045u);
    EconomyCapabilityRequirement const bolts = CraftingRequirement(2u, bags, 197u, 2964u, 2997u);
    coordinator.RevalidateCapability({shirts, true}, 101u);
    coordinator.RevalidateCapability({shirts, true}, 102u);
    EconomyCoordinatorSnapshot const shirtsPersistent = coordinator.Snapshot(102u);
    ASSERT_EQ(shirtsPersistent.capabilityBlockers.size(), 1u);
    EXPECT_EQ(shirtsPersistent.capabilityBlockers.front().consecutiveEligibleCycles, 2u);

    coordinator.RevalidateCapability({bolts, true}, 103u);
    EconomyCoordinatorSnapshot const reset = coordinator.Snapshot(103u);
    ASSERT_EQ(reset.capabilityBlockers.size(), 1u);
    EXPECT_EQ(reset.capabilityBlockers.front().requirement, bolts);
    EXPECT_EQ(reset.capabilityBlockers.front().consecutiveEligibleCycles, 1u);
    EXPECT_EQ(reset.capabilityBlockers.front().state, EconomyCapabilityBlockerState::Observing);
    EXPECT_EQ(reset.capabilityBlockers.front().assignedActorGuid, 0u);
}

TEST(PlayerbotEconomyCoordinatorTest, CapabilitySelectsOneRecipeOwnerBeforeAnyTrainerCandidate)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const bags = EconomySubstitutionGroup::Bag(12u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({bags, 4u});
    coordinator.RefreshActor(consumer, 100u);

    EconomyActorFacts trainerCandidate = Actor(2u, 12u, 2u);
    trainerCandidate.craftingAffinity = 100u;
    trainerCandidate.freePrimaryProfessionSlots = 1u;
    coordinator.RefreshActor(trainerCandidate, 100u);

    EconomyActorFacts lowAffinityRecipeCandidate = Actor(5u, 15u, 2u);
    lowAffinityRecipeCandidate.craftingAffinity = PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM - 1u;
    lowAffinityRecipeCandidate.professionSkillIds = {197u};
    coordinator.RefreshActor(lowAffinityRecipeCandidate, 100u);

    EconomyActorFacts lowerGuidRecipeCandidate = Actor(10u, 20u, 2u);
    lowerGuidRecipeCandidate.craftingAffinity = 75u;
    lowerGuidRecipeCandidate.professionSkillIds = {197u};
    coordinator.RefreshActor(lowerGuidRecipeCandidate, 100u);

    EconomyActorFacts higherGuidRecipeCandidate = Actor(20u, 30u, 2u);
    higherGuidRecipeCandidate.craftingAffinity = 75u;
    higherGuidRecipeCandidate.professionSkillIds = {197u};
    coordinator.RefreshActor(higherGuidRecipeCandidate, 100u);

    EconomyCapabilityRequirement const tailoring = CraftingRequirement(2u, bags, 197u, 12044u, 10045u);
    coordinator.RevalidateCapability({tailoring, true}, 101u);
    coordinator.RevalidateCapability({tailoring, true}, 102u);

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(102u);
    ASSERT_EQ(snapshot.capabilityBlockers.size(), 1u);
    EXPECT_EQ(snapshot.capabilityBlockers.front().assignedActorGuid, 10u);
    EXPECT_EQ(snapshot.capabilityBlockers.front().assignedWorkKind,
              std::optional<EconomyWorkKind>(EconomyWorkKind::Recipe));

    lowerGuidRecipeCandidate.recipeSpellIds = {tailoring.capability.recipeSpellId};
    coordinator.RefreshActor(lowerGuidRecipeCandidate, 103u);
    EXPECT_TRUE(coordinator.Snapshot(103u).capabilityBlockers.empty());
}

TEST(PlayerbotEconomyCoordinatorTest, CapabilitySelectsHighestAffinityTrainerAndProviderReleasesWithoutClosingChain)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const ore = EconomySubstitutionGroup::ExactReagent(2770u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({ore, 4u});
    coordinator.RefreshActor(consumer, 100u);

    EconomyActorFacts lowerAffinity = Actor(8u, 18u, 2u);
    lowerAffinity.gatheringAffinity = 50u;
    lowerAffinity.freePrimaryProfessionSlots = 1u;
    coordinator.RefreshActor(lowerAffinity, 100u);
    EconomyActorFacts occupied = Actor(5u, 15u, 2u);
    occupied.gatheringAffinity = 100u;
    coordinator.RefreshActor(occupied, 100u);
    EconomyActorFacts higherGuid = Actor(20u, 30u, 2u);
    higherGuid.gatheringAffinity = 75u;
    higherGuid.freePrimaryProfessionSlots = 1u;
    coordinator.RefreshActor(higherGuid, 100u);
    EconomyActorFacts lowerGuid = Actor(10u, 20u, 2u);
    lowerGuid.gatheringAffinity = 75u;
    lowerGuid.freePrimaryProfessionSlots = 1u;
    coordinator.RefreshActor(lowerGuid, 100u);

    EconomyCapabilityRequirement const mining = GatheringRequirement(2u, ore, 186u, 2770u);
    coordinator.RevalidateCapability({mining, true}, 101u);
    coordinator.RevalidateCapability({mining, true}, 102u);
    EconomyCoordinatorSnapshot const blocked = coordinator.Snapshot(102u);
    ASSERT_EQ(blocked.capabilityBlockers.size(), 1u);
    EXPECT_EQ(blocked.capabilityBlockers.front().assignedActorGuid, 10u);
    EXPECT_EQ(blocked.capabilityBlockers.front().assignedWorkKind,
              std::optional<EconomyWorkKind>(EconomyWorkKind::Trainer));
    ASSERT_EQ(blocked.chains.size(), 1u);
    std::string const chainPublicId = blocked.chains.front().publicId;
    uint64 const chainHistoryCount = blocked.chains.front().totalHistoryCount;

    EconomyActorFacts provider = Actor(30u, 40u, 2u);
    provider.gatheringAffinity = 25u;
    provider.professionSkillIds = {186u};
    coordinator.RefreshActor(provider, 103u);

    EconomyCoordinatorSnapshot const released = coordinator.Snapshot(103u);
    EXPECT_TRUE(released.capabilityBlockers.empty());
    ASSERT_EQ(released.chains.size(), 1u);
    EXPECT_EQ(released.chains.front().publicId, chainPublicId);
    EXPECT_TRUE(released.chains.front().active);
    EXPECT_EQ(released.chains.front().completedAt, 0u);
    EXPECT_EQ(released.chains.front().totalHistoryCount, chainHistoryCount);
    EXPECT_EQ(released.chains.front().remainingQuantity, 4u);
}

TEST(PlayerbotEconomyCoordinatorTest, CapabilityTrainerAssignmentPreservesZeroOneAndTwoOccupiedSkillFixtures)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const ore = EconomySubstitutionGroup::ExactReagent(2770u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({ore, 4u});
    coordinator.RefreshActor(consumer, 100u);

    EconomyActorFacts zeroOccupied = Actor(10u, 20u, 2u);
    zeroOccupied.gatheringAffinity = 50u;
    zeroOccupied.freePrimaryProfessionSlots = 2u;
    coordinator.RefreshActor(zeroOccupied, 100u);

    EconomyActorFacts oneOccupied = Actor(20u, 30u, 2u);
    oneOccupied.gatheringAffinity = 75u;
    oneOccupied.freePrimaryProfessionSlots = 1u;
    oneOccupied.professionSkillIds = {164u};
    coordinator.RefreshActor(oneOccupied, 100u);

    EconomyActorFacts twoOccupied = Actor(30u, 40u, 2u);
    twoOccupied.gatheringAffinity = 100u;
    twoOccupied.freePrimaryProfessionSlots = 0u;
    twoOccupied.professionSkillIds = {164u, 165u};
    coordinator.RefreshActor(twoOccupied, 100u);

    EconomyCapabilityRequirement const mining = GatheringRequirement(2u, ore, 186u, 2770u);
    coordinator.RevalidateCapability({mining, true}, 101u);
    coordinator.RevalidateCapability({mining, true}, 102u);

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(102u);
    ASSERT_EQ(snapshot.capabilityBlockers.size(), 1u);
    EXPECT_EQ(snapshot.capabilityBlockers.front().assignedActorGuid, oneOccupied.characterGuid);
    EXPECT_EQ(snapshot.capabilityBlockers.front().assignedWorkKind,
              std::optional<EconomyWorkKind>(EconomyWorkKind::Trainer));

    auto const preservedSkills = [&snapshot](uint32 characterGuid)
    {
        auto const actor = std::find_if(snapshot.actors.begin(), snapshot.actors.end(),
                                        [characterGuid](EconomyActorFacts const& candidate)
                                        { return candidate.characterGuid == characterGuid; });
        return actor == snapshot.actors.end() ? std::vector<uint16>{} : actor->professionSkillIds;
    };
    EXPECT_EQ(preservedSkills(zeroOccupied.characterGuid), zeroOccupied.professionSkillIds);
    EXPECT_EQ(preservedSkills(oneOccupied.characterGuid), oneOccupied.professionSkillIds);
    EXPECT_EQ(preservedSkills(twoOccupied.characterGuid), twoOccupied.professionSkillIds);
}

TEST(PlayerbotEconomyCoordinatorTest, EquivalentSupplyClosesOnlyItsMarketAndSubstitutionGap)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    EconomySubstitutionGroup const tin = EconomySubstitutionGroup::ExactReagent(101u);

    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 10u});
    consumer.supplies.push_back({copper, 3u, EconomySupplySource::Inventory});
    consumer.supplies.push_back({copper, 2u, EconomySupplySource::Mail});
    consumer.supplies.push_back({tin, 50u, EconomySupplySource::Inventory});
    coordinator.RefreshActor(consumer, 100u);

    EconomyActorFacts offline = Actor(2u, 12u, 2u);
    offline.online = false;
    offline.demands.push_back({copper, 100u});
    coordinator.RefreshActor(offline, 100u);
    EconomyActorFacts controlled = Actor(3u, 13u, 2u);
    controlled.autonomous = false;
    controlled.demands.push_back({copper, 100u});
    coordinator.RefreshActor(controlled, 100u);

    EconomyMarketFacts market;
    market.marketId = 2u;
    market.supplies.push_back({copper, 2u, EconomySupplySource::ActiveAuction});
    coordinator.RefreshMarket(market, 100u);

    EconomyMarketFacts otherMarket;
    otherMarket.marketId = 6u;
    otherMarket.supplies.push_back({copper, 100u, EconomySupplySource::ActiveAuction});
    coordinator.RefreshMarket(otherMarket, 100u);

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(100u);
    ASSERT_EQ(snapshot.gaps.size(), 1u);
    EXPECT_EQ(snapshot.gaps.front().marketId, 2u);
    EXPECT_EQ(snapshot.gaps.front().group, copper);
    EXPECT_EQ(snapshot.gaps.front().demandQuantity, 10u);
    EXPECT_EQ(snapshot.gaps.front().supplyQuantity, 7u);
    EXPECT_EQ(snapshot.gaps.front().remainingQuantity, 3u);
}

TEST(PlayerbotEconomyCoordinatorTest, FailedAndExpiredClaimsTransferOnlyTheirUncommittedRemainder)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 10u});
    coordinator.RefreshActor(consumer, 100u);
    coordinator.RefreshActor(Actor(2u, 12u, 2u), 100u);
    coordinator.RefreshActor(Actor(3u, 13u, 2u), 100u);

    EconomyAssignmentLease const first = coordinator.Lease(Request(2u, 2u, copper, 10u), 100u);
    ASSERT_TRUE(first.assignment.has_value());
    EXPECT_EQ(first.assignment->quantity, 10u);
    EXPECT_TRUE(
        coordinator.RecordOutcome(first.assignment->leaseId, EconomyAssignmentOutcome::FailedPurchase, 4u, 110u));

    EconomyAssignmentRequest secondRequest = Request(3u, 2u, copper, 10u);
    secondRequest.expiresAt = 120u;
    EconomyAssignmentLease const second = coordinator.Lease(secondRequest, 111u);
    ASSERT_TRUE(second.assignment.has_value());
    EXPECT_EQ(second.assignment->quantity, 6u);

    coordinator.Expire(121u);
    EconomyAssignmentLease const third = coordinator.Lease(Request(2u, 2u, copper, 10u), 122u);
    ASSERT_TRUE(third.assignment.has_value());
    EXPECT_EQ(third.assignment->quantity, 6u);
}

TEST(PlayerbotEconomyCoordinatorTest, ReleasedGatheringInventoryReplacesClaimBackingWithoutDoubleSupply)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 5u});
    consumer.supplies.push_back({copper, 2u, EconomySupplySource::Inventory});
    coordinator.RefreshActor(consumer, 100u);
    coordinator.RefreshActor(Actor(2u, 12u, 2u), 100u);

    EconomyAssignmentRequest request = Request(2u, 2u, copper, 3u);
    request.kind = EconomyClaimKind::Resource;
    request.workKind = EconomyWorkKind::Gather;
    EconomyAssignmentLease const lease = coordinator.Lease(request, 100u);
    ASSERT_TRUE(lease.assignment.has_value());
    ASSERT_TRUE(
        coordinator.RecordOutcome(lease.assignment->leaseId, EconomyAssignmentOutcome::InventoryReceived, 1u, 101u));

    EconomyActorFacts gatherer = Actor(2u, 12u, 2u);
    gatherer.supplies.push_back({copper, 1u, EconomySupplySource::Inventory});
    coordinator.RefreshActor(std::move(gatherer), 101u);

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(101u);
    ASSERT_EQ(snapshot.gaps.size(), 1u);
    EXPECT_EQ(snapshot.gaps.front().supplyQuantity, 3u);
    EXPECT_EQ(snapshot.gaps.front().remainingQuantity, 2u);
}

TEST(PlayerbotEconomyCoordinatorTest, ConcurrentLeasesNeverExceedTheRemainingDemand)
{
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    for (EconomyClaimKind kind : {EconomyClaimKind::Production, EconomyClaimKind::Purchase, EconomyClaimKind::Resource})
    {
        PlayerbotEconomyCoordinator coordinator;
        EconomyActorFacts consumer = Actor(1u, 11u, 2u);
        consumer.demands.push_back({copper, 8u});
        coordinator.RefreshActor(consumer, 100u);
        coordinator.RefreshActor(Actor(2u, 12u, 2u), 100u);
        coordinator.RefreshActor(Actor(3u, 13u, 2u), 100u);
        if (kind == EconomyClaimKind::Purchase)
        {
            EconomyMarketFacts market;
            market.marketId = 2u;
            market.supplies.push_back({copper, 8u, EconomySupplySource::ActiveAuction});
            coordinator.RefreshMarket(std::move(market), 100u);
        }

        std::atomic<uint32> leased = 0u;
        auto lease = [&coordinator, &copper, &leased, kind](uint32 characterGuid)
        {
            EconomyAssignmentRequest request = Request(characterGuid, 2u, copper, 8u);
            request.kind = kind;
            request.sellerAccountId = kind == EconomyClaimKind::Purchase ? 99u : 0u;
            request.workKind = kind == EconomyClaimKind::Resource   ? EconomyWorkKind::Gather
                               : kind == EconomyClaimKind::Purchase ? EconomyWorkKind::Buy
                                                                    : EconomyWorkKind::Craft;
            EconomyAssignmentLease const result = coordinator.Lease(std::move(request), 100u);
            if (result.assignment.has_value())
                leased.fetch_add(result.assignment->quantity);
        };
        std::thread first(lease, 2u);
        std::thread second(lease, 3u);
        first.join();
        second.join();

        EXPECT_EQ(leased.load(), 8u);
        EXPECT_EQ(coordinator.Snapshot(100u).gaps.front().remainingQuantity, 0u);
    }
}

TEST(PlayerbotEconomyCoordinatorTest, ConsumerOrProducerPreemptsSpeculationAndUnsafeAccountPurchasesAreRejected)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 5u});
    coordinator.RefreshActor(consumer, 100u);
    coordinator.RefreshActor(Actor(2u, 12u, 2u), 100u);
    coordinator.RefreshActor(Actor(3u, 13u, 2u), 100u);
    EconomyMarketFacts market;
    market.marketId = 2u;
    market.supplies.push_back({copper, 10u, EconomySupplySource::ActiveAuction});
    coordinator.RefreshMarket(std::move(market), 100u);

    EconomyAssignmentRequest speculative = Request(2u, 2u, copper, 5u);
    speculative.kind = EconomyClaimKind::Purchase;
    speculative.priority = EconomyClaimPriority::Speculation;
    speculative.workKind = EconomyWorkKind::MarketMaking;
    speculative.workIdentity = "position:test";
    speculative.sellerAccountId = 99u;
    ASSERT_TRUE(coordinator.Lease(speculative, 100u).assignment.has_value());

    consumer.demands.front().quantity = 10u;
    coordinator.RefreshActor(consumer, 101u);
    EconomyAssignmentRequest producer = Request(3u, 2u, copper, 5u);
    producer.kind = EconomyClaimKind::Purchase;
    producer.sellerAccountId = 99u;
    producer.workKind = EconomyWorkKind::Buy;
    EconomyAssignmentLease const winning = coordinator.Lease(producer, 101u);
    ASSERT_TRUE(winning.assignment.has_value());
    EXPECT_EQ(winning.assignment->quantity, 5u);

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(101u);
    ASSERT_EQ(snapshot.claims.size(), 2u);
    EXPECT_EQ(snapshot.claims[0].state, EconomyClaimState::Released);
    EXPECT_EQ(snapshot.claims[1].state, EconomyClaimState::Leased);

    EconomyAssignmentRequest selfPurchase = Request(2u, 2u, copper, 1u);
    selfPurchase.kind = EconomyClaimKind::Purchase;
    selfPurchase.sellerAccountId = 12u;
    EconomyAssignmentLease const rejected = coordinator.Lease(selfPurchase, 102u);
    EXPECT_FALSE(rejected.assignment.has_value());
    EXPECT_EQ(rejected.blocker, EconomyWorkBlocker::SameAccountPurchase);

    EconomyAssignmentRequest unknownSeller = Request(2u, 2u, copper, 1u);
    unknownSeller.kind = EconomyClaimKind::Purchase;
    EconomyAssignmentLease const rejectedUnknownSeller = coordinator.Lease(unknownSeller, 102u);
    EXPECT_FALSE(rejectedUnknownSeller.assignment.has_value());
    EXPECT_EQ(rejectedUnknownSeller.blocker, EconomyWorkBlocker::AccountIdentityUnavailable);

    coordinator.RefreshActor(Actor(4u, 0u, 2u), 102u);
    EconomyAssignmentRequest unknownBuyer = Request(4u, 2u, copper, 1u);
    unknownBuyer.kind = EconomyClaimKind::Purchase;
    unknownBuyer.sellerAccountId = 99u;
    EconomyAssignmentLease const rejectedUnknownBuyer = coordinator.Lease(unknownBuyer, 102u);
    EXPECT_FALSE(rejectedUnknownBuyer.assignment.has_value());
    EXPECT_EQ(rejectedUnknownBuyer.blocker, EconomyWorkBlocker::AccountIdentityUnavailable);
}

TEST(PlayerbotEconomyCoordinatorTest, LogoutReleasesOnlyTransferableWork)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 5u});
    coordinator.RefreshActor(consumer, 100u);
    coordinator.RefreshActor(Actor(2u, 12u, 2u), 100u);
    coordinator.RefreshActor(Actor(3u, 13u, 2u), 100u);

    EconomyAssignmentLease const original = coordinator.Lease(Request(2u, 2u, copper, 5u), 100u);
    ASSERT_TRUE(original.assignment.has_value());
    ASSERT_TRUE(coordinator.RecordOutcome(original.assignment->leaseId, EconomyAssignmentOutcome::Committed, 2u, 100u));
    coordinator.InvalidateActor(2u, EconomyAssignmentOutcome::LoggedOut, 100u);

    EconomyAssignmentLease const reassigned = coordinator.Lease(Request(3u, 2u, copper, 5u), 101u);
    ASSERT_TRUE(reassigned.assignment.has_value());
    EXPECT_EQ(reassigned.assignment->quantity, 3u);
}

TEST(PlayerbotEconomyCoordinatorTest, DirectCommandsBypassOnlyOrdinaryAffinityThresholds)
{
    std::array<EconomyWorkKind, 6> const ordinary = {EconomyWorkKind::Craft,  EconomyWorkKind::Gather,
                                                     EconomyWorkKind::Buy,    EconomyWorkKind::Sell,
                                                     EconomyWorkKind::Recipe, EconomyWorkKind::Trainer};

    for (EconomyWorkKind kind : ordinary)
    {
        for (uint8 affinity : {0u, 24u, 25u, 75u})
        {
            EconomyWorkPolicyInput input;
            input.kind = kind;
            input.economyAffinity = affinity;
            EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(input),
                      affinity < PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM ? EconomyWorkBlocker::AffinityTooLow
                                                                               : EconomyWorkBlocker::None);

            input.directCommand = true;
            EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(input), EconomyWorkBlocker::None);
        }
    }

    EconomyWorkPolicyInput marketMaking;
    marketMaking.kind = EconomyWorkKind::MarketMaking;
    marketMaking.economyAffinity = 75u;
    EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(marketMaking), EconomyWorkBlocker::None);
    marketMaking.directCommand = true;
    EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(marketMaking), EconomyWorkBlocker::AutonomousOnly);

    std::array<std::pair<bool EconomyWorkPolicyInput::*, EconomyWorkBlocker>, 7> const falseSafeguards = {
        std::pair{&EconomyWorkPolicyInput::legal, EconomyWorkBlocker::Illegal},
        std::pair{&EconomyWorkPolicyInput::withinBudget, EconomyWorkBlocker::Budget},
        std::pair{&EconomyWorkPolicyInput::liveObject, EconomyWorkBlocker::MissingLiveObject},
        std::pair{&EconomyWorkPolicyInput::pathAvailable, EconomyWorkBlocker::MissingPath},
        std::pair{&EconomyWorkPolicyInput::hasSkill, EconomyWorkBlocker::MissingSkill},
        std::pair{&EconomyWorkPolicyInput::phaseAllowed, EconomyWorkBlocker::WrongPhase},
        std::pair{&EconomyWorkPolicyInput::transactionSafe, EconomyWorkBlocker::UnsafeTransaction}};
    for (auto const& [member, blocker] : falseSafeguards)
    {
        EconomyWorkPolicyInput blocked;
        blocked.kind = EconomyWorkKind::Craft;
        blocked.economyAffinity = 0u;
        blocked.*member = false;
        EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(blocked), blocker);
        blocked.directCommand = true;
        EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(blocked), blocker);
        EXPECT_STRNE(PlayerbotEconomyPolicy::WorkBlockerName(blocker), "unknown");
    }

    EconomyWorkPolicyInput sameAccount;
    sameAccount.kind = EconomyWorkKind::Buy;
    sameAccount.economyAffinity = 0u;
    sameAccount.sameAccountPurchase = true;
    EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(sameAccount), EconomyWorkBlocker::SameAccountPurchase);
    sameAccount.directCommand = true;
    EXPECT_EQ(PlayerbotEconomyPolicy::EvaluateWork(sameAccount), EconomyWorkBlocker::SameAccountPurchase);
}

TEST(PlayerbotEconomyCoordinatorTest, SubstitutionGroupsMatchOnlyEquivalentUtility)
{
    EXPECT_EQ(EconomySubstitutionGroup::Equipment(4u, 3u, 2u), EconomySubstitutionGroup::Equipment(4u, 3u, 2u));
    EXPECT_NE(EconomySubstitutionGroup::Equipment(4u, 3u, 2u), EconomySubstitutionGroup::Equipment(4u, 1u, 2u));
    EXPECT_NE(EconomySubstitutionGroup::Bag(16u), EconomySubstitutionGroup::Bag(20u));
    EXPECT_NE(EconomySubstitutionGroup::Ammunition(2u, 3u), EconomySubstitutionGroup::Ammunition(2u, 4u));
    EXPECT_NE(EconomySubstitutionGroup::Consumable(7u, 2u), EconomySubstitutionGroup::Consumable(8u, 2u));
    EXPECT_NE(EconomySubstitutionGroup::Enhancement(5u, 100u), EconomySubstitutionGroup::Enhancement(5u, 200u));
    EXPECT_NE(EconomySubstitutionGroup::ExactReagent(100u), EconomySubstitutionGroup::ExactReagent(101u));
}

TEST(PlayerbotEconomyCoordinatorTest, UnavailableGatheringDestinationSettlesTheLeaseExactlyOnce)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(2'770u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 4u});
    coordinator.RefreshActor(consumer, 100u);
    coordinator.RefreshActor(Actor(2u, 12u, 2u), 100u);
    coordinator.RefreshActor(Actor(3u, 13u, 2u), 100u);

    EconomyAssignmentLease const failed = coordinator.Lease(Request(2u, 2u, copper, 4u), 101u);
    ASSERT_TRUE(failed.assignment.has_value());
    EXPECT_TRUE(
        PlayerbotEconomyGathering::SettleUnavailableDestination(coordinator, failed.assignment->leaseId, 0u, 102u));
    EXPECT_FALSE(
        PlayerbotEconomyGathering::SettleUnavailableDestination(coordinator, failed.assignment->leaseId, 0u, 103u));

    EconomyAssignmentLease const partial = coordinator.Lease(Request(3u, 2u, copper, 4u), 104u);
    ASSERT_TRUE(partial.assignment.has_value());
    EXPECT_TRUE(
        PlayerbotEconomyGathering::SettleUnavailableDestination(coordinator, partial.assignment->leaseId, 2u, 105u));
    EXPECT_FALSE(
        PlayerbotEconomyGathering::SettleUnavailableDestination(coordinator, partial.assignment->leaseId, 2u, 106u));

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(106u);
    auto const failedClaim =
        std::find_if(snapshot.claims.begin(), snapshot.claims.end(),
                     [&failed](EconomyAssignment const& claim) { return claim.leaseId == failed.assignment->leaseId; });
    ASSERT_NE(failedClaim, snapshot.claims.end());
    EXPECT_EQ(failedClaim->state, EconomyClaimState::Released);
    EXPECT_EQ(failedClaim->lastOutcome, EconomyAssignmentOutcome::FailedTravel);
    EXPECT_EQ(failedClaim->committedQuantity, 0u);

    auto const partialClaim =
        std::find_if(snapshot.claims.begin(), snapshot.claims.end(), [&partial](EconomyAssignment const& claim)
                     { return claim.leaseId == partial.assignment->leaseId; });
    ASSERT_NE(partialClaim, snapshot.claims.end());
    EXPECT_EQ(partialClaim->state, EconomyClaimState::Released);
    EXPECT_EQ(partialClaim->lastOutcome, EconomyAssignmentOutcome::InventoryReceived);
    EXPECT_EQ(partialClaim->committedQuantity, 2u);
}

TEST(PlayerbotEconomyCoordinatorTest, DemandChainKeepsOpaqueIdentityAndBoundedChronology)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 4u});
    coordinator.RefreshActor(consumer, 100u);
    coordinator.RefreshActor(Actor(2u, 12u, 2u), 100u);
    coordinator.RefreshActor(Actor(3u, 13u, 2u), 100u);

    EconomyCoordinatorSnapshot snapshot = coordinator.Snapshot(100u);
    ASSERT_EQ(snapshot.chains.size(), 1u);
    EconomyChain const& opened = snapshot.chains.front();
    EXPECT_EQ(opened.publicId.size(), 20u);
    EXPECT_EQ(opened.publicId.substr(0u, 4u), "chn_");
    EXPECT_EQ(opened.createdAt, 100u);
    EXPECT_TRUE(opened.active);
    EXPECT_EQ(opened.marketId, 2u);
    EXPECT_EQ(opened.group, copper);
    EXPECT_EQ(opened.demandQuantity, 4u);
    EXPECT_EQ(opened.remainingQuantity, 4u);
    ASSERT_EQ(opened.consumerGuids.size(), 1u);
    EXPECT_EQ(opened.consumerGuids.front(), 1u);
    ASSERT_EQ(opened.history.size(), 1u);
    EXPECT_EQ(opened.history.front().stage, EconomyChainStage::Demand);
    EXPECT_EQ(opened.history.front().outcome, EconomyChainOutcome::Progress);

    EconomyAssignmentLease const failed = coordinator.Lease(Request(2u, 2u, copper, 4u), 101u);
    ASSERT_TRUE(failed.assignment.has_value());
    std::string const publicId = failed.assignment->chainPublicId;
    EXPECT_EQ(publicId, opened.publicId);
    EXPECT_EQ(failed.assignment->createdAt, 101u);
    ASSERT_TRUE(
        coordinator.RecordOutcome(failed.assignment->leaseId, EconomyAssignmentOutcome::FailedTravel, 1u, 102u));

    EconomyAssignmentLease const reassigned = coordinator.Lease(Request(3u, 2u, copper, 4u), 103u);
    ASSERT_TRUE(reassigned.assignment.has_value());
    EXPECT_EQ(reassigned.assignment->chainPublicId, publicId);
    EXPECT_EQ(reassigned.assignment->quantity, 3u);
    ASSERT_TRUE(
        coordinator.RecordOutcome(reassigned.assignment->leaseId, EconomyAssignmentOutcome::Completed, 3u, 104u));

    EconomyActorFacts supplier = Actor(3u, 13u, 2u);
    supplier.supplies.push_back({copper, 4u, EconomySupplySource::Inventory});
    coordinator.RefreshActor(std::move(supplier), 105u);
    snapshot = coordinator.Snapshot(105u);
    ASSERT_EQ(snapshot.chains.size(), 1u);
    EXPECT_TRUE(snapshot.chains.front().active);
    EXPECT_EQ(snapshot.chains.front().remainingQuantity, 0u);

    consumer.demands.clear();
    coordinator.RefreshActor(consumer, 106u);
    snapshot = coordinator.Snapshot(106u);
    ASSERT_EQ(snapshot.chains.size(), 1u);
    EconomyChain const& completed = snapshot.chains.front();
    EXPECT_EQ(completed.publicId, publicId);
    EXPECT_FALSE(completed.active);
    EXPECT_EQ(completed.completedAt, 106u);
    ASSERT_EQ(completed.history.size(), 6u);
    EXPECT_EQ(completed.totalHistoryCount, 6u);
    EXPECT_FALSE(completed.historyTruncated);
    EXPECT_EQ(completed.history[1].stage, EconomyChainStage::Claim);
    EXPECT_EQ(completed.history[2].stage, EconomyChainStage::Release);
    EXPECT_EQ(completed.history[2].outcome, EconomyChainOutcome::Failed);
    EXPECT_EQ(completed.history[3].stage, EconomyChainStage::Claim);
    EXPECT_EQ(completed.history[4].stage, EconomyChainStage::Deliver);
    EXPECT_EQ(completed.history[5].stage, EconomyChainStage::Complete);
    EXPECT_EQ(completed.history[5].outcome, EconomyChainOutcome::Completed);

    consumer.demands.push_back({copper, 2u});
    coordinator.RefreshActor(consumer, 107u);
    snapshot = coordinator.Snapshot(107u);
    ASSERT_EQ(snapshot.chains.size(), 2u);
    EXPECT_NE(snapshot.chains.back().publicId, publicId);

    EconomyActorFacts returningConsumer = Actor(3u, 13u, 2u);
    returningConsumer.demands.push_back({copper, 2u});
    coordinator.RefreshActor(std::move(returningConsumer), 108u);
    EconomyActorChainObservation const observation = coordinator.ObserveActor(3u, 108u);
    ASSERT_TRUE(observation.available);
    EXPECT_EQ(observation.chainPublicId, snapshot.chains.back().publicId);
}

TEST(PlayerbotEconomyCoordinatorTest, DemandChainHistoryRetainsTheNewestBoundedEvents)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomySubstitutionGroup const copper = EconomySubstitutionGroup::ExactReagent(100u);
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    consumer.demands.push_back({copper, 1u});
    coordinator.RefreshActor(std::move(consumer), 100u);

    std::size_t constexpr extraEvents = 5u;
    for (std::size_t attempt = 0; attempt < PLAYERBOT_ECONOMY_CHAIN_HISTORY_CAPACITY + extraEvents; ++attempt)
    {
        EconomyAssignmentLease const rejected = coordinator.Lease(Request(999u, 2u, copper, 1u), 101u + attempt);
        EXPECT_FALSE(rejected.assignment.has_value());
        EXPECT_EQ(rejected.blocker, EconomyWorkBlocker::UnknownActor);
    }

    EconomyCoordinatorSnapshot const snapshot = coordinator.Snapshot(200u);
    ASSERT_EQ(snapshot.chains.size(), 1u);
    EconomyChain const& chain = snapshot.chains.front();
    EXPECT_EQ(chain.history.size(), PLAYERBOT_ECONOMY_CHAIN_HISTORY_CAPACITY);
    EXPECT_EQ(chain.totalHistoryCount, 1u + PLAYERBOT_ECONOMY_CHAIN_HISTORY_CAPACITY + extraEvents);
    EXPECT_TRUE(chain.historyTruncated);
    EXPECT_EQ(chain.history.front().sequence, chain.totalHistoryCount - PLAYERBOT_ECONOMY_CHAIN_HISTORY_CAPACITY + 1u);
    EXPECT_EQ(chain.history.back().sequence, chain.totalHistoryCount);
    EXPECT_EQ(chain.history.back().stage, EconomyChainStage::Blocked);
    EXPECT_EQ(chain.history.back().blocker, EconomyWorkBlocker::UnknownActor);
    EXPECT_EQ(chain.updatedAt, 169u);
}

TEST(PlayerbotEconomyCoordinatorTest, SaturatedChainSnapshotsAreBoundedAndReadStable)
{
    PlayerbotEconomyCoordinator coordinator;
    EconomyActorFacts consumer = Actor(1u, 11u, 2u);
    for (std::size_t index = 0; index < PLAYERBOT_ECONOMY_CHAIN_CAPACITY + 1u; ++index)
    {
        consumer.demands.push_back({
            EconomySubstitutionGroup::ExactReagent(static_cast<uint32>(1000u + index)),
            1u,
        });
    }
    coordinator.RefreshActor(std::move(consumer), 100u);

    EconomyCoordinatorSnapshot const first = coordinator.Snapshot(100u);
    EconomyCoordinatorSnapshot const second = coordinator.Snapshot(200u);
    EXPECT_EQ(first.chains.size(), PLAYERBOT_ECONOMY_CHAIN_CAPACITY);
    EXPECT_EQ(second.generation, first.generation);
    EXPECT_EQ(second.chains, first.chains);
    ASSERT_EQ(first.blockers.size(), second.blockers.size());
    if (!first.blockers.empty())
        EXPECT_EQ(second.blockers.front().count, first.blockers.front().count);
}
