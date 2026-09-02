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

// The core mills five herbs per cast; the pigment reference loot tables yield two to four, and the
// bill counts on the floor so a short milling never leaves the craft one pigment shy.
inline constexpr std::uint32_t PROFESSION_MILLING_HERBS_PER_CAST = 5u;
inline constexpr std::uint32_t PROFESSION_MILLING_PIGMENTS_PER_CAST = 2u;

struct ProfessionProgressionReagentFact
{
    std::uint32_t itemId = 0u;
    std::uint32_t perCraftQuantity = 0u;
    bool ordinaryVendorAvailable = false;
    // The herb the bot would mill this reagent out of. When set, the scarce bill names the herb, which
    // a herbalist can gather and the market can list, instead of the pigment nobody can source.
    std::uint32_t millingInputItemId = 0u;
    // What the bot already holds of the reagent; the bill asks only for the shortfall.
    std::uint32_t ownedCount = 0u;
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
    // SameActorGathering requires a gathering skill id; SameActorHunting requires skill id 0.
    MaterialSourceKind kind = MaterialSourceKind::SameActorGathering;
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
// The bot behind a profession progression origin identity, or nothing for any other owner shape.
[[nodiscard]] std::optional<std::uint32_t> ProfessionProgressionOriginGuid(std::string const& originIdentity);
// The compaction the book needs now, or nothing: intents of bots in absentGuids (with every
// commitment they own), intents unobserved for staleAfterSeconds with no active commitment, terminal
// commitments whose horizon passed that long ago, and the operation log trimmed to
// retainedOperations when it grew past that.
[[nodiscard]] std::optional<MaterialCommitmentCommand> BuildCompaction(MaterialCommitmentSnapshot const& snapshot,
                                                                       std::uint64_t now,
                                                                       std::vector<std::uint32_t> const& absentGuids,
                                                                       std::uint64_t staleAfterSeconds,
                                                                       std::uint32_t retainedOperations);
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
