/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTENCODING_H
#define PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTENCODING_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Bot/Economy/PlayerbotEconomyRuntime.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"

namespace PlayerbotEconomy::MaterialCommitmentEncoding
{
constexpr std::size_t MAX_IDENTITY_BYTES = 191u;
constexpr std::size_t MAX_FINGERPRINT_BYTES = 16'777'215u;

struct ProfessionProgressionIntentInput
{
    std::uint32_t characterGuid = 0u;
    std::uint32_t marketId = 0u;
    std::uint16_t professionSkillId = 0u;
    std::uint16_t targetSkill = 0u;
    std::uint32_t recipeSpellId = 0u;
    std::uint32_t outputItemId = 0u;
    std::uint32_t boundedBatch = 0u;
    std::vector<MaterialRequirement> scarceRequirements;
};

struct ProfessionProgressionReagentFact
{
    std::uint32_t itemId = 0u;
    std::uint32_t perCraftQuantity = 0u;
    bool ordinaryVendorAvailable = false;
};

enum class ProfessionProgressionObserveBuildStatus : std::uint8_t
{
    Command,
    NoChange,
    Invalid
};

struct ProfessionProgressionObserveBuildResult
{
    ProfessionProgressionObserveBuildStatus status = ProfessionProgressionObserveBuildStatus::Invalid;
    std::optional<MaterialCommitmentCommand> command;
};

enum class ProfessionProgressionObserveStatus : std::uint8_t
{
    PendingPersistence,
    NoChange,
    Busy,
    Stale,
    PersistenceUnavailable,
    Invalid
};

struct ProfessionProgressionObserveResult
{
    ProfessionProgressionObserveStatus status = ProfessionProgressionObserveStatus::Invalid;
};

struct ProfessionProgressionBlockedCycleInput
{
    std::optional<ProfessionProgressionIntentInput> intent;
    std::uint32_t recipeSpellId = 0u;
    std::uint32_t materialItemId = 0u;
    std::string blockerCode;
};

struct ProfessionProgressionBlockedCycleResult
{
    PlayerbotEconomyCycleResult cycleResult;
    ProfessionProgressionObserveStatus observationStatus = ProfessionProgressionObserveStatus::Invalid;
};

struct SameActorGatheringPathInput
{
    std::uint32_t actorGuid = 0u;
    std::uint32_t materialItemId = 0u;
    std::uint32_t selectedQuantity = 0u;
    std::uint32_t gatheringSkillId = 0u;
    std::uint32_t sourceEntry = 0u;
    std::uint32_t sourceMapId = 0u;
    std::string routeIdentity;
    std::string capacityIdentity;
    std::uint64_t selectedAt = 0u;
    std::uint32_t sourceTravelBudgetSeconds = 0u;
    std::uint32_t destinationConservativeYieldBasisPoints = 0u;
    std::uint32_t observedGatheredQuantity = 0u;
    std::uint32_t observedResourceAttempts = 0u;
    std::uint32_t observedResourceSeconds = 0u;
    std::uint32_t authoritativeInteractionSeconds = 0u;
    std::uint32_t remainingDedicatedActivitySeconds = 0u;
    std::uint32_t deliveryTravelBudgetSeconds = 0u;
    std::uint32_t completionObservationBudgetSeconds = 0u;
    std::uint32_t startingInventoryQuantity = 0u;
    std::uint32_t availableResourceCount = 0u;
};

enum class SameActorGatheringPathBuildStatus : std::uint8_t
{
    Path,
    Invalid
};

struct SameActorGatheringPathBuildResult
{
    SameActorGatheringPathBuildStatus status = SameActorGatheringPathBuildStatus::Invalid;
    std::optional<MaterialSourcePath> path;
};

[[nodiscard]] std::string Fingerprint(MaterialCommitmentCommand const& command);
[[nodiscard]] std::string CommitmentIdentity(std::string const& operationIdentity, std::size_t ordinal);
[[nodiscard]] std::string ProfessionProgressionOriginIdentity(ProfessionProgressionIntentInput const& input);
[[nodiscard]] std::string ProfessionProgressionObserveOperationIdentity(std::string const& originIdentity,
                                                                        std::uint64_t ownerRevision);
[[nodiscard]] std::optional<std::vector<MaterialRequirement>> ProfessionProgressionScarceRequirements(
    std::uint32_t boundedBatch, std::vector<ProfessionProgressionReagentFact> const& reagents);
[[nodiscard]] ProfessionProgressionObserveBuildResult BuildProfessionProgressionObserve(
    ProfessionProgressionIntentInput input, MaterialCommitmentSnapshot const& snapshot);
[[nodiscard]] ProfessionProgressionObserveResult ObserveProfessionProgression(
    ProfessionProgressionIntentInput input, MaterialCommitmentSnapshot const& snapshot,
    PlayerbotMaterialCommitmentAuthority& authority, std::uint64_t now);
[[nodiscard]] ProfessionProgressionBlockedCycleResult ObserveBlockedProfessionProgression(
    ProfessionProgressionBlockedCycleInput input, MaterialCommitmentSnapshot const& snapshot,
    PlayerbotMaterialCommitmentAuthority& authority, std::uint64_t now);
[[nodiscard]] SameActorGatheringPathBuildResult BuildSameActorGatheringPath(SameActorGatheringPathInput const& input);
}  // namespace PlayerbotEconomy::MaterialCommitmentEncoding

#endif
