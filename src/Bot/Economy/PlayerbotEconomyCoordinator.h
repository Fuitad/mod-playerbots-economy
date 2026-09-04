/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYCOORDINATOR_H
#define PLAYERBOTS_PLAYERBOTECONOMYCOORDINATOR_H

#include <compare>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "Bot/Economy/PlayerbotEconomyPolicy.h"
#include "Bot/Economy/PlayerbotProfessionCapability.h"

namespace PlayerbotEconomy
{
inline constexpr std::size_t PLAYERBOT_ECONOMY_CHAIN_CAPACITY = 256;
inline constexpr std::size_t PLAYERBOT_ECONOMY_CHAIN_HISTORY_CAPACITY = 64;
inline constexpr uint32 PLAYERBOT_ECONOMY_CLAIM_RETENTION_SECONDS = 300;
inline constexpr uint8 PLAYERBOT_ECONOMY_CAPABILITY_AFFINITY_MINIMUM = 25;
inline constexpr uint32 PLAYERBOT_ECONOMY_CAPABILITY_PERSISTENCE_THRESHOLD = 2;
inline constexpr uint64 PLAYERBOT_ECONOMY_AFFINITY_RELAXATION_SECONDS = 600;

enum class EconomySubstitutionKind : uint8
{
    ExactReagent,
    Equipment,
    Bag,
    Ammunition,
    Consumable,
    Enhancement,
    Glyph,
    Gem
};

enum class ConsumableCapability : uint32
{
    Food,
    Drink,
    HealthRestoration,
    ManaRestoration,
    Bandage
};

struct EconomySubstitutionGroup
{
    EconomySubstitutionKind kind = EconomySubstitutionKind::ExactReagent;
    uint32 exactItemId = 0;
    uint8 equipmentSlot = 0;
    uint32 roleMask = 0;
    uint16 bagCapacity = 0;
    uint32 ammunitionType = 0;
    uint8 tier = 0;
    uint32 effectFamily = 0;
    uint32 enhancementTarget = 0;
    uint8 enhancementSlot = 0;
    uint32 glyphSpellId = 0;
    uint32 glyphSlotType = 0;
    uint32 gemColor = 0;
    uint32 valueBand = 0;
    // A representative glyph item, carried so a reader can be told the glyph's name. A glyph is
    // identified by the spell it grants, and nothing outside the client's DBC files maps that
    // spell back to an item, so the name has to travel with the demand or it cannot be shown at
    // all. It is display data and deliberately NOT identity: see the comparison below.
    uint32 glyphItemId = 0;

private:
    /*
     * Identity excludes glyphItemId. Every other member describes what would satisfy the demand,
     * so it belongs in the key; the representative item only says what to call it. Including it
     * would split one glyph's demand into a group per item that happens to grant it, and demand
     * that does not pool is demand no crafter can see: that is the defect the potion utility
     * floor caused before it was dropped to one. PlayerbotEconomyConsumptionTest pins this.
     *
     * Defined before the operators that call it: a deduced return type cannot be used before the
     * function is defined.
     */
    [[nodiscard]] auto IdentityTuple() const
    {
        return std::tie(kind, exactItemId, equipmentSlot, roleMask, bagCapacity, ammunitionType, tier, effectFamily,
                        enhancementTarget, enhancementSlot, glyphSpellId, glyphSlotType, gemColor, valueBand);
    }

public:
    friend auto operator<=>(EconomySubstitutionGroup const& left, EconomySubstitutionGroup const& right)
    {
        return left.IdentityTuple() <=> right.IdentityTuple();
    }

    friend bool operator==(EconomySubstitutionGroup const& left, EconomySubstitutionGroup const& right)
    {
        return left.IdentityTuple() == right.IdentityTuple();
    }

    static EconomySubstitutionGroup ExactReagent(uint32 itemId);
    static EconomySubstitutionGroup Equipment(uint8 slot, uint32 roles, uint8 itemTier);
    static EconomySubstitutionGroup Bag(uint16 capacity);
    static EconomySubstitutionGroup Ammunition(uint32 type, uint8 itemTier);
    static EconomySubstitutionGroup Consumable(uint32 family, uint8 itemTier);
    static EconomySubstitutionGroup Consumable(ConsumableCapability capability, uint32 minimumUtility);
    static EconomySubstitutionGroup Enhancement(uint32 target, uint32 band);
    static EconomySubstitutionGroup Enhancement(uint32 target, uint8 enchantmentSlot, uint32 band);
    static EconomySubstitutionGroup Glyph(uint32 spellId, uint32 slotType, uint32 itemId = 0);
    static EconomySubstitutionGroup Gem(uint32 color);
};

enum class EconomySupplySource : uint8
{
    Inventory,
    Mail,
    ActiveAuction,
    CommittedPurchase,
    CommittedProduction
};

struct EconomyDemandFact
{
    EconomySubstitutionGroup group;
    uint32 quantity = 0;

    bool operator==(EconomyDemandFact const&) const = default;
};

struct EconomySupplyFact
{
    EconomySubstitutionGroup group;
    uint32 quantity = 0;
    EconomySupplySource source = EconomySupplySource::Inventory;
    uint32 itemId = 0;

    bool operator==(EconomySupplyFact const&) const = default;
};

struct EconomyActorFacts
{
    uint32 characterGuid = 0;
    uint32 accountId = 0;
    uint32 marketId = 0;
    bool online = false;
    bool autonomous = false;
    uint8 craftingAffinity = 0;
    uint8 gatheringAffinity = 0;
    uint8 economyAffinity = 0;
    uint8 freePrimaryProfessionSlots = 0;
    std::vector<uint16> professionSkillIds;
    std::vector<uint32> recipeSpellIds;
    std::vector<EconomyDemandFact> demands;
    std::vector<EconomySupplyFact> supplies;

    bool operator==(EconomyActorFacts const&) const = default;
};

struct EconomyMarketFacts
{
    uint32 marketId = 0;
    std::vector<EconomySupplyFact> supplies;

    bool operator==(EconomyMarketFacts const&) const = default;
};

enum class EconomyClaimKind : uint8
{
    Production,
    Purchase,
    Resource
};

enum class EconomyClaimPriority : uint8
{
    Speculation,
    Producer,
    Consumer
};

enum class EconomyClaimState : uint8
{
    Leased,
    Released,
    Completed
};

enum class EconomyAssignmentOutcome : uint8
{
    Committed,
    Completed,
    InventoryReceived,
    FailedTravel,
    FailedPurchase,
    CapabilityLost,
    NeedChanged,
    LoggedOut,
    Disabled
};

struct EconomyAssignmentRequest
{
    uint32 characterGuid = 0;
    uint32 marketId = 0;
    EconomySubstitutionGroup group;
    uint32 quantity = 0;
    EconomyClaimKind kind = EconomyClaimKind::Production;
    EconomyClaimPriority priority = EconomyClaimPriority::Producer;
    EconomyWorkKind workKind = EconomyWorkKind::Craft;
    std::string workIdentity;
    uint32 sellerAccountId = 0;
    bool directCommand = false;
    uint64 expiresAt = 0;
    uint32 recipeSpellId = 0;
    uint32 outputItemId = 0;
    EconomyWorkPolicyInput safeguards;
};

struct EconomyAssignment
{
    uint64 leaseId = 0;
    std::string chainPublicId;
    uint32 characterGuid = 0;
    uint32 marketId = 0;
    EconomySubstitutionGroup group;
    uint32 quantity = 0;
    uint32 committedQuantity = 0;
    EconomyClaimKind kind = EconomyClaimKind::Production;
    EconomyClaimPriority priority = EconomyClaimPriority::Producer;
    EconomyClaimState state = EconomyClaimState::Leased;
    bool directCommand = false;
    std::string workIdentity;
    uint64 createdAt = 0;
    uint64 expiresAt = 0;
    uint64 settledAt = 0;
    uint32 bridgedQuantity = 0;
    uint32 recipeSpellId = 0;
    uint32 outputItemId = 0;
    EconomyAssignmentOutcome lastOutcome = EconomyAssignmentOutcome::Committed;
};

struct EconomyAssignmentLease
{
    std::optional<EconomyAssignment> assignment;
    EconomyWorkBlocker blocker = EconomyWorkBlocker::None;
};

struct EconomyProductionRecipe
{
    EconomySubstitutionGroup group;
    uint32 recipeSpellId = 0;
    uint32 outputItemId = 0;
    uint32 maxQuantity = 0;

    bool operator==(EconomyProductionRecipe const&) const = default;
};

struct EconomyProductionRequest
{
    uint32 characterGuid = 0;
    uint32 marketId = 0;
    std::vector<EconomyProductionRecipe> recipes;
    uint64 expiresAt = 0;
    EconomyWorkPolicyInput safeguards;
};

struct EconomyProductionOutput
{
    bool recorded = false;
    bool completed = false;
    uint32 producedQuantity = 0;
    uint32 committedQuantity = 0;
};

struct EconomyDemandGap
{
    uint32 marketId = 0;
    EconomySubstitutionGroup group;
    uint32 demandQuantity = 0;
    uint32 supplyQuantity = 0;
    uint32 claimedQuantity = 0;
    uint32 remainingQuantity = 0;

    [[nodiscard]] bool HasResidualDemand() const { return remainingQuantity != 0u; }
    // Demand that goods in hand or on the auction house do not cover. Claims are promises, not
    // supply: a producer judging whether its output is wanted must not see its own claim as the
    // answer, or it drops the recipe, loses the claim, and takes it again next cycle.
    [[nodiscard]] bool HasUnsuppliedDemand() const { return demandQuantity > supplyQuantity; }
};

struct EconomyBlockerCount
{
    EconomyWorkBlocker blocker = EconomyWorkBlocker::None;
    uint32 count = 0;
};

struct EconomyCapabilityRequirement
{
    uint32 marketId = 0;
    EconomySubstitutionGroup group;
    ProfessionCapability capability;

    bool operator==(EconomyCapabilityRequirement const&) const = default;
};

struct EconomyCapabilityObservation
{
    EconomyCapabilityRequirement requirement;
    bool eligibleCycle = false;
};

enum class EconomyCapabilityBlockerState : uint8
{
    Observing,
    Persistent
};

struct EconomyCapabilityBlocker
{
    EconomyCapabilityRequirement requirement;
    EconomyCapabilityBlockerState state = EconomyCapabilityBlockerState::Observing;
    uint32 consecutiveEligibleCycles = 0;
    uint32 assignedActorGuid = 0;
    std::optional<EconomyWorkKind> assignedWorkKind;
    uint64 firstObservedAt = 0;
    uint64 lastObservedAt = 0;

    bool operator==(EconomyCapabilityBlocker const&) const = default;
};

enum class EconomyChainStage : uint8
{
    Demand,
    Claim,
    Commit,
    Deliver,
    Release,
    Blocked,
    Complete
};

enum class EconomyChainOutcome : uint8
{
    Progress,
    Released,
    Failed,
    Blocked,
    Completed
};

struct EconomyChainEvent
{
    uint64 sequence = 0;
    uint64 occurredAt = 0;
    uint32 actorGuid = 0;
    EconomyChainStage stage = EconomyChainStage::Demand;
    EconomyChainOutcome outcome = EconomyChainOutcome::Progress;
    EconomyClaimKind claimKind = EconomyClaimKind::Production;
    EconomyAssignmentOutcome assignmentOutcome = EconomyAssignmentOutcome::Committed;
    EconomyWorkBlocker blocker = EconomyWorkBlocker::None;
    uint32 quantity = 0;
    uint32 remainingQuantity = 0;
    std::string workIdentity;

    bool operator==(EconomyChainEvent const&) const = default;
};

struct EconomyChain
{
    std::string publicId;
    uint32 marketId = 0;
    EconomySubstitutionGroup group;
    uint64 createdAt = 0;
    uint64 updatedAt = 0;
    uint64 completedAt = 0;
    uint32 demandQuantity = 0;
    uint32 supplyQuantity = 0;
    uint32 claimedQuantity = 0;
    uint32 remainingQuantity = 0;
    bool active = true;
    std::vector<uint32> consumerGuids;
    std::vector<EconomyChainEvent> history;
    uint64 totalHistoryCount = 0;
    bool historyTruncated = false;

    bool operator==(EconomyChain const&) const = default;
};

struct EconomyActorChainObservation
{
    bool available = false;
    std::string chainPublicId;
    std::string workIdentity;
    uint32 marketId = 0;
    EconomySubstitutionGroup group;
    uint32 remainingQuantity = 0;
    uint64 claimAgeSeconds = 0;
    EconomyClaimState claimState = EconomyClaimState::Released;
    EconomyAssignmentOutcome assignmentOutcome = EconomyAssignmentOutcome::NeedChanged;
    std::optional<EconomyCapabilityBlocker> capabilityBlocker;
};

struct EconomyCoordinatorSnapshot
{
    uint64 generation = 0;
    std::vector<EconomyActorFacts> actors;
    std::vector<EconomyDemandGap> gaps;
    std::vector<EconomyAssignment> claims;
    std::vector<EconomyBlockerCount> blockers;
    std::vector<EconomyCapabilityBlocker> capabilityBlockers;
    std::vector<EconomyChain> chains;
};

struct EconomyCoordinatorWorkStats
{
    uint64 lockAcquisitions = 0;
    uint64 gapRebuilds = 0;
    uint64 chainSyncExecutions = 0;
    uint64 capabilityReconciliations = 0;
};

class PlayerbotEconomyCoordinator
{
public:
    void RefreshActor(EconomyActorFacts facts, uint64 now);
    void RefreshMarket(EconomyMarketFacts facts, uint64 now);
    void RevalidateCapability(EconomyCapabilityObservation observation, uint64 now);
    void RevalidateCapabilities(std::vector<EconomyCapabilityObservation> observations, uint64 now);
    [[nodiscard]] EconomyAssignmentLease Lease(EconomyAssignmentRequest request, uint64 now);
    [[nodiscard]] EconomyAssignmentLease AssignProduction(EconomyProductionRequest request, uint64 now);
    [[nodiscard]] EconomyProductionOutput RecordProductionInventory(uint64 leaseId, uint32 startingQuantity,
                                                                    uint32 currentQuantity, uint64 now);
    [[nodiscard]] EconomyProductionOutput RecordProductionOutput(uint64 leaseId, uint32 producedQuantity, uint64 now);
    // A confirmed progression craft can consume its input before the actor publishes fresh facts. Credit only the
    // matching demand those facts leave uncovered; the next RefreshActor removes the transient credit.
    [[nodiscard]] uint32 RecordConsumedInputs(uint32 characterGuid, std::vector<EconomyDemandFact> const& inputs,
                                              uint64 now);
    [[nodiscard]] bool RecordOutcome(uint64 leaseId, EconomyAssignmentOutcome outcome, uint32 committedQuantity,
                                     uint64 now);
    void InvalidateActor(uint32 characterGuid, EconomyAssignmentOutcome outcome, uint64 now);
    void Expire(uint64 now);
    [[nodiscard]] EconomyCoordinatorSnapshot Snapshot(uint64 now);
    [[nodiscard]] EconomyActorChainObservation ObserveActor(uint32 characterGuid, uint64 now);
    [[nodiscard]] EconomyCoordinatorWorkStats WorkStats() const;

private:
    struct GapTotals
    {
        uint64 demand = 0;
        uint64 supply = 0;
        uint64 claimed = 0;
        uint64 nonAuctionSupply = 0;
        uint64 auctionSupply = 0;
        uint64 purchaseClaimed = 0;
        uint64 speculationClaimed = 0;
    };

    using GapKey = std::pair<uint32, EconomySubstitutionGroup>;

    struct GapBlockerCondition
    {
        EconomyWorkBlocker blocker = EconomyWorkBlocker::None;
        uint64 firstObservedAt = 0;
    };

    bool ExpireLocked(uint64 now, bool advanceGeneration = true);
    bool InvalidateActorLocked(uint32 characterGuid, EconomyAssignmentOutcome outcome, uint64 now);
    bool ReleaseSpeculationLocked(GapKey const& key, uint64 now);
    void ReleaseExcessClaimsLocked(uint64 now);
    bool ApplyOutcomeLocked(EconomyAssignment& claim, EconomyAssignmentOutcome outcome, uint32 committedQuantity,
                            uint64 now);
    [[nodiscard]] bool RevalidateCapabilityLocked(EconomyCapabilityObservation const& observation, uint64 now);
    void ReconcileCapabilityBlockersLocked();
    [[nodiscard]] bool HasCapabilityProviderLocked(EconomyCapabilityRequirement const& requirement) const;
    void AssignCapabilityOwnerLocked(EconomyCapabilityBlocker& blocker) const;
    void SyncChainsLocked(uint64 now);
    [[nodiscard]] EconomyChain* EnsureChainLocked(GapKey const& key, uint64 now, bool hasDemand);
    [[nodiscard]] EconomyChain* FindChainLocked(std::string const& publicId);
    void AppendChainEventLocked(EconomyChain& chain, EconomyChainEvent event);
    void AppendClaimEventLocked(EconomyAssignment const& claim, EconomyChainStage stage, EconomyChainOutcome outcome,
                                EconomyWorkBlocker blocker, uint64 now);
    void InvalidateGapCacheLocked();
    [[nodiscard]] std::map<GapKey, GapTotals> const& CalculateGapsLocked() const;
    [[nodiscard]] EconomyAssignmentLease RejectLocked(EconomyWorkBlocker blocker,
                                                      EconomyAssignmentRequest const* request = nullptr,
                                                      uint64 now = 0);

    mutable std::mutex mutex;
    std::map<uint32, EconomyActorFacts> actors;
    std::map<uint32, EconomyMarketFacts> markets;
    // Confirmed craft inputs consumed since each actor's last authoritative refresh.
    std::map<uint32, std::map<EconomySubstitutionGroup, uint32>> consumedInputCredits;
    std::vector<EconomyAssignment> claims;
    std::map<GapKey, EconomyCapabilityBlocker> capabilityBlockers;
    std::vector<EconomyChain> chains;
    std::map<GapKey, std::string> activeChainIds;
    std::map<GapKey, GapBlockerCondition> gapBlockers;
    mutable std::map<GapKey, GapTotals> cachedGaps;
    mutable std::map<GapKey, std::vector<uint32>> cachedConsumers;
    mutable bool gapCacheDirty = true;
    bool chainsDirty = true;
    mutable EconomyCoordinatorWorkStats workStats;
    uint64 nextLeaseId = 1;
    uint64 nextChainSequence = 1;
    uint64 generation = 0;
};

PlayerbotEconomyCoordinator& GetPlayerbotEconomyCoordinator();
}  // namespace PlayerbotEconomy

#endif
