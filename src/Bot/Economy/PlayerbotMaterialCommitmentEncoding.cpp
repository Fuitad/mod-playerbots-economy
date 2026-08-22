/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotMaterialCommitmentEncoding.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace PlayerbotEconomy::MaterialCommitmentEncoding
{
namespace
{
void AppendCapacity(std::ostringstream& stream, MaterialCapacityKey const& capacity)
{
    stream << static_cast<unsigned>(capacity.kind) << ':' << capacity.authorityIdentity.size() << ':'
           << capacity.authorityIdentity << ';';
}

std::uint64_t Fnv1a(std::string const& value)
{
    std::uint64_t hash = 14'695'981'039'346'656'037ull;
    for (unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1'099'511'628'211ull;
    }
    return hash;
}

void AppendSourcePath(std::ostringstream& stream, MaterialSourcePath const& path)
{
    stream << static_cast<unsigned>(path.kind) << ':' << static_cast<unsigned>(path.phase) << ':' << path.actorGuid
           << ':' << path.materialItemId << ':' << path.selectedQuantity << ':' << path.gatheringSkillId << ':'
           << path.sourceEntry << ':' << path.sourceMapId << ':' << path.routeIdentity.size() << ':'
           << path.routeIdentity << ':' << path.capacityIdentity.size() << ':' << path.capacityIdentity << ':'
           << path.sourceRevision << ':' << path.selectedAt << ':' << path.sourceTravelBudgetSeconds << ':'
           << path.sourceActionBudgetSeconds << ':' << path.deliveryTravelBudgetSeconds << ':'
           << path.completionObservationBudgetSeconds << ':' << path.destinationYieldBasisPoints << ':'
           << path.conservativeYieldBasisPoints << ':' << path.observedGatheredQuantity << ':'
           << path.observedResourceAttempts << ':' << path.observedResourceSeconds << ':'
           << path.authoritativeInteractionSeconds << ':' << path.remainingDedicatedActivitySeconds << ':'
           << path.requiredResourceCount << ':' << path.secondsPerResource << ':' << path.startingInventoryQuantity
           << ':' << path.availableResourceCount << ':' << path.neededBy << ';';
}

bool CheckedAdd(std::uint64_t& total, std::uint64_t value)
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
        return false;
    total += value;
    return true;
}
}  // namespace

std::string Fingerprint(MaterialCommitmentCommand const& command)
{
    std::ostringstream stream;
    stream << command.expectedBookRevision << '|' << static_cast<unsigned>(command.kind) << '|';
    for (MaterialIntent const& intent : command.intents)
    {
        stream << intent.originIdentity.size() << ':' << intent.originIdentity << ':'
               << static_cast<unsigned>(intent.ownerKind) << ':' << intent.ownerRevision << ':' << intent.marketId
               << ':' << intent.boundedQuantity << ':';
        if (intent.neededBy.has_value())
            stream << *intent.neededBy;
        stream << '|';
        for (MaterialRequirement const& requirement : intent.requirements)
            stream << requirement.itemId << ':' << requirement.quantity << ',';
        stream << '|';
    }
    for (MaterialAdmissionCandidate const& candidate : command.candidates)
    {
        stream << candidate.originIdentity.size() << ':' << candidate.originIdentity << ':' << candidate.ownerRevision
               << '|';
        for (MaterialReservationRequest const& reservation : candidate.reservations)
        {
            stream << reservation.materialItemId << ':';
            AppendCapacity(stream, reservation.capacity);
            stream << reservation.authorityRevision << ':' << reservation.backedMaterialQuantity << ':'
                   << reservation.capacityQuantity << ',';
        }
        for (MaterialSourcePath const& path : candidate.sourcePaths)
            AppendSourcePath(stream, path);
        stream << '|';
    }
    for (MaterialCapacityObservation const& observation : command.capacityObservations)
    {
        AppendCapacity(stream, observation.capacity);
        stream << static_cast<unsigned>(observation.unit) << ':' << observation.materialItemId << ':'
               << observation.authorityRevision << ':' << observation.availableQuantity << '|';
    }
    for (MaterialFulfillment const& fulfillment : command.fulfillments)
    {
        stream << fulfillment.commitmentIdentity.size() << ':' << fulfillment.commitmentIdentity << ':'
               << fulfillment.quantity << '|';
        if (fulfillment.observedInventoryQuantity.has_value())
            stream << *fulfillment.observedInventoryQuantity;
        stream << '|';
        for (MaterialReservationSettlement const& settlement : fulfillment.reservationSettlements)
        {
            AppendCapacity(stream, settlement.capacity);
            stream << settlement.backedMaterialQuantity << ':' << settlement.capacityQuantity << ',';
        }
        stream << '|';
    }
    for (MaterialSourceStart const& start : command.sourceStarts)
    {
        stream << start.commitmentIdentity.size() << ':' << start.commitmentIdentity << ':'
               << start.expectedSourceRevision << ':' << start.startingInventoryQuantity << '|';
    }
    for (std::string const& identity : command.commitmentIdentities)
        stream << identity.size() << ':' << identity << '|';
    return stream.str();
}

std::string CommitmentIdentity(std::string const& operationIdentity, std::size_t ordinal)
{
    std::ostringstream stream;
    stream << "mc" << std::hex << std::setfill('0') << std::setw(16)
           << Fnv1a(operationIdentity + ":" + std::to_string(ordinal));
    return stream.str();
}

std::string ProfessionProgressionOriginIdentity(ProfessionProgressionIntentInput const& input)
{
    std::ostringstream stream;
    stream << "profession-progression:" << input.characterGuid << ':' << input.professionSkillId << ':'
           << input.targetSkill << ':' << input.recipeSpellId << ':' << input.outputItemId;
    return stream.str();
}

std::string ProfessionProgressionObserveOperationIdentity(std::string const& originIdentity,
                                                          std::uint64_t ownerRevision)
{
    return originIdentity + ":observe:" + std::to_string(ownerRevision);
}

std::optional<std::vector<MaterialRequirement>> ProfessionProgressionScarceRequirements(
    std::uint32_t boundedBatch, std::vector<ProfessionProgressionReagentFact> const& reagents)
{
    if (!boundedBatch)
        return std::nullopt;

    struct ReagentBill
    {
        std::uint64_t required = 0u;
    };
    std::map<std::uint32_t, ReagentBill> bill;
    for (ProfessionProgressionReagentFact const& reagent : reagents)
    {
        if (!reagent.itemId || !reagent.perCraftQuantity)
            return std::nullopt;
        if (reagent.ordinaryVendorAvailable)
            continue;
        std::uint64_t const required = static_cast<std::uint64_t>(reagent.perCraftQuantity) * boundedBatch;
        ReagentBill& item = bill[reagent.itemId];
        if (required > std::numeric_limits<std::uint64_t>::max() - item.required)
            return std::nullopt;
        item.required += required;
    }

    std::vector<MaterialRequirement> requirements;
    requirements.reserve(bill.size());
    for (auto const& [itemId, item] : bill)
    {
        if (item.required > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        requirements.push_back({.itemId = itemId, .quantity = static_cast<std::uint32_t>(item.required)});
    }
    if (requirements.empty())
        return std::nullopt;
    return requirements;
}

ProfessionProgressionObserveBuildResult BuildProfessionProgressionObserve(ProfessionProgressionIntentInput input,
                                                                          MaterialCommitmentSnapshot const& snapshot)
{
    if (!input.characterGuid || !input.marketId || !input.professionSkillId || !input.targetSkill ||
        !input.recipeSpellId || !input.outputItemId || !input.boundedBatch || input.scarceRequirements.empty())
    {
        return {.status = ProfessionProgressionObserveBuildStatus::Invalid};
    }
    std::ranges::sort(input.scarceRequirements, {}, &MaterialRequirement::itemId);
    if (std::ranges::any_of(input.scarceRequirements, [](MaterialRequirement const& requirement)
                            { return !requirement.itemId || !requirement.quantity; }) ||
        std::ranges::adjacent_find(input.scarceRequirements, {}, &MaterialRequirement::itemId) !=
            input.scarceRequirements.end())
    {
        return {.status = ProfessionProgressionObserveBuildStatus::Invalid};
    }

    std::string const originIdentity = ProfessionProgressionOriginIdentity(input);
    if (originIdentity.size() > MAX_IDENTITY_BYTES)
        return {.status = ProfessionProgressionObserveBuildStatus::Invalid};
    auto const existing = std::ranges::find(snapshot.intents, originIdentity, &MaterialIntent::originIdentity);
    if (existing != snapshot.intents.end() &&
        existing->ownerKind == MaterialCommitmentOwnerKind::ProfessionProgression &&
        existing->marketId == input.marketId && existing->boundedQuantity == input.boundedBatch &&
        !existing->neededBy.has_value() && existing->requirements == input.scarceRequirements)
    {
        return {.status = ProfessionProgressionObserveBuildStatus::NoChange};
    }
    if (existing != snapshot.intents.end() && existing->ownerRevision == std::numeric_limits<std::uint64_t>::max())
        return {.status = ProfessionProgressionObserveBuildStatus::Invalid};

    std::uint64_t const ownerRevision = existing == snapshot.intents.end() ? 1u : existing->ownerRevision + 1u;
    std::string const operationIdentity = ProfessionProgressionObserveOperationIdentity(originIdentity, ownerRevision);
    if (operationIdentity.size() > MAX_IDENTITY_BYTES)
        return {.status = ProfessionProgressionObserveBuildStatus::Invalid};
    return {
        .status = ProfessionProgressionObserveBuildStatus::Command,
        .command =
            MaterialCommitmentCommand{
                .operationIdentity = operationIdentity,
                .expectedBookRevision = snapshot.bookRevision,
                .kind = MaterialCommitmentCommandKind::Observe,
                .intents = {{
                    .originIdentity = originIdentity,
                    .ownerKind = MaterialCommitmentOwnerKind::ProfessionProgression,
                    .ownerRevision = ownerRevision,
                    .marketId = input.marketId,
                    .boundedQuantity = input.boundedBatch,
                    .neededBy = std::nullopt,
                    .requirements = std::move(input.scarceRequirements),
                }},
            },
    };
}

ProfessionProgressionObserveResult ObserveProfessionProgression(ProfessionProgressionIntentInput input,
                                                                MaterialCommitmentSnapshot const& snapshot,
                                                                PlayerbotMaterialCommitmentAuthority& authority,
                                                                std::uint64_t now)
{
    ProfessionProgressionObserveBuildResult build = BuildProfessionProgressionObserve(std::move(input), snapshot);
    if (build.status == ProfessionProgressionObserveBuildStatus::NoChange)
        return {.status = ProfessionProgressionObserveStatus::NoChange};
    if (build.status != ProfessionProgressionObserveBuildStatus::Command || !build.command)
        return {.status = ProfessionProgressionObserveStatus::Invalid};

    MaterialCommitmentApplyResult const applied = authority.Apply(std::move(*build.command), now);
    switch (applied.status)
    {
        case MaterialCommitmentApplyStatus::PendingPersistence:
            return {.status = ProfessionProgressionObserveStatus::PendingPersistence};
        case MaterialCommitmentApplyStatus::Idempotent:
            return {.status = ProfessionProgressionObserveStatus::NoChange};
        case MaterialCommitmentApplyStatus::Busy:
            return {.status = ProfessionProgressionObserveStatus::Busy};
        case MaterialCommitmentApplyStatus::StaleBookRevision:
        case MaterialCommitmentApplyStatus::StaleOwnerRevision:
            return {.status = ProfessionProgressionObserveStatus::Stale};
        case MaterialCommitmentApplyStatus::PersistenceUnavailable:
            return {.status = ProfessionProgressionObserveStatus::PersistenceUnavailable};
        default:
            return {.status = ProfessionProgressionObserveStatus::Invalid};
    }
}

ProfessionProgressionBlockedCycleResult ObserveBlockedProfessionProgression(
    ProfessionProgressionBlockedCycleInput input, MaterialCommitmentSnapshot const& snapshot,
    PlayerbotMaterialCommitmentAuthority& authority, std::uint64_t now)
{
    ProfessionProgressionObserveStatus status = ProfessionProgressionObserveStatus::Invalid;
    if (input.intent)
        status = ObserveProfessionProgression(std::move(*input.intent), snapshot, authority, now).status;

    PlayerbotEconomyCycleResult cycleResult;
    cycleResult.outcome = PlayerbotEconomyCycleOutcome::NoCandidate;
    cycleResult.phase = EconomyPhase::Craft;
    cycleResult.workIdentity = {input.recipeSpellId, input.materialItemId, 0u, 0u};
    cycleResult.blocker =
        input.blockerCode + ":item:" + std::to_string(input.materialItemId) + ":owned_or_ordinary_vendor";
    cycleResult.schedulingEffect = EconomyAttemptOutcome::NoCandidate;
    return {.cycleResult = std::move(cycleResult), .observationStatus = status};
}

SameActorGatheringPathBuildResult BuildSameActorGatheringPath(SameActorGatheringPathInput const& input)
{
    constexpr std::uint32_t herbalismSkillId = 182u;
    constexpr std::uint32_t miningSkillId = 186u;
    constexpr std::uint32_t skinningSkillId = 393u;
    bool const skillMatchesKind =
        input.kind == MaterialSourceKind::SameActorHunting
            ? input.gatheringSkillId == 0u
            : (input.gatheringSkillId == herbalismSkillId || input.gatheringSkillId == miningSkillId ||
               input.gatheringSkillId == skinningSkillId);
    if (!input.actorGuid || !input.materialItemId || !input.selectedQuantity || !input.sourceEntry ||
        !skillMatchesKind || input.routeIdentity.empty() || input.routeIdentity.size() > MAX_IDENTITY_BYTES ||
        input.capacityIdentity.empty() || input.capacityIdentity.size() > MAX_IDENTITY_BYTES || !input.selectedAt ||
        !input.destinationConservativeYieldBasisPoints || !input.authoritativeInteractionSeconds ||
        !input.remainingDedicatedActivitySeconds || input.deliveryTravelBudgetSeconds != 0u ||
        !input.completionObservationBudgetSeconds || !input.availableResourceCount)
    {
        return {};
    }

    bool const hasHistory = input.observedResourceAttempts != 0u;
    if ((!hasHistory && (input.observedGatheredQuantity || input.observedResourceSeconds)) ||
        (hasHistory && !input.observedResourceSeconds))
    {
        return {};
    }

    std::uint64_t const resourcesPerExpectedItem = (10'000ull + input.destinationConservativeYieldBasisPoints - 1u) /
                                                   input.destinationConservativeYieldBasisPoints;
    std::uint64_t const coldStartSecondsPerResource =
        input.remainingDedicatedActivitySeconds / resourcesPerExpectedItem;
    if (!coldStartSecondsPerResource)
        return {};

    std::uint64_t conservativeYieldBasisPoints = input.destinationConservativeYieldBasisPoints;
    std::uint64_t throughputSecondsPerResource = coldStartSecondsPerResource;
    if (hasHistory)
    {
        if (input.observedGatheredQuantity >
            (std::numeric_limits<std::uint64_t>::max() - input.destinationConservativeYieldBasisPoints) / 10'000u)
        {
            return {};
        }
        std::uint64_t const blendedYield = (input.destinationConservativeYieldBasisPoints +
                                            static_cast<std::uint64_t>(input.observedGatheredQuantity) * 10'000u) /
                                           (static_cast<std::uint64_t>(input.observedResourceAttempts) + 1u);
        conservativeYieldBasisPoints =
            std::min<std::uint64_t>(input.destinationConservativeYieldBasisPoints, blendedYield);
        throughputSecondsPerResource =
            (static_cast<std::uint64_t>(input.observedResourceSeconds) + input.observedResourceAttempts - 1u) /
            input.observedResourceAttempts;
    }
    if (!conservativeYieldBasisPoints)
        return {};

    std::uint64_t const requiredResourceCount =
        (static_cast<std::uint64_t>(input.selectedQuantity) * 10'000u + conservativeYieldBasisPoints - 1u) /
        conservativeYieldBasisPoints;
    std::uint64_t const secondsPerResource =
        std::max<std::uint64_t>(input.authoritativeInteractionSeconds, throughputSecondsPerResource);
    if (!requiredResourceCount || requiredResourceCount > input.availableResourceCount || !secondsPerResource ||
        requiredResourceCount > std::numeric_limits<std::uint32_t>::max() ||
        secondsPerResource > std::numeric_limits<std::uint32_t>::max() ||
        requiredResourceCount > std::numeric_limits<std::uint32_t>::max() / secondsPerResource)
    {
        return {};
    }

    std::uint32_t const sourceActionBudgetSeconds =
        static_cast<std::uint32_t>(requiredResourceCount * secondsPerResource);
    std::uint64_t neededBy = input.selectedAt;
    if (!CheckedAdd(neededBy, input.sourceTravelBudgetSeconds) || !CheckedAdd(neededBy, sourceActionBudgetSeconds) ||
        !CheckedAdd(neededBy, input.deliveryTravelBudgetSeconds) ||
        !CheckedAdd(neededBy, input.completionObservationBudgetSeconds))
    {
        return {};
    }

    std::ostringstream revisionFacts;
    revisionFacts << input.actorGuid << ':' << input.materialItemId << ':' << input.selectedQuantity << ':'
                  << input.gatheringSkillId << ':' << input.sourceEntry << ':' << input.sourceMapId << ':'
                  << input.routeIdentity << ':' << input.capacityIdentity << ':' << input.selectedAt << ':'
                  << input.sourceTravelBudgetSeconds << ':' << input.destinationConservativeYieldBasisPoints << ':'
                  << input.observedGatheredQuantity << ':' << input.observedResourceAttempts << ':'
                  << input.observedResourceSeconds << ':' << input.authoritativeInteractionSeconds << ':'
                  << input.remainingDedicatedActivitySeconds << ':' << input.deliveryTravelBudgetSeconds << ':'
                  << input.completionObservationBudgetSeconds << ':' << input.availableResourceCount;
    std::uint64_t const sourceRevision = Fnv1a(revisionFacts.str());
    if (!sourceRevision)
        return {};

    return {
        .status = SameActorGatheringPathBuildStatus::Path,
        .path =
            MaterialSourcePath{
                .kind = input.kind,
                .phase = MaterialSourcePhase::Selected,
                .actorGuid = input.actorGuid,
                .materialItemId = input.materialItemId,
                .selectedQuantity = input.selectedQuantity,
                .gatheringSkillId = input.gatheringSkillId,
                .sourceEntry = input.sourceEntry,
                .sourceMapId = input.sourceMapId,
                .routeIdentity = input.routeIdentity,
                .capacityIdentity = input.capacityIdentity,
                .sourceRevision = sourceRevision,
                .selectedAt = input.selectedAt,
                .sourceTravelBudgetSeconds = input.sourceTravelBudgetSeconds,
                .sourceActionBudgetSeconds = sourceActionBudgetSeconds,
                .deliveryTravelBudgetSeconds = input.deliveryTravelBudgetSeconds,
                .completionObservationBudgetSeconds = input.completionObservationBudgetSeconds,
                .destinationYieldBasisPoints = input.destinationConservativeYieldBasisPoints,
                .conservativeYieldBasisPoints = static_cast<std::uint32_t>(conservativeYieldBasisPoints),
                .observedGatheredQuantity = input.observedGatheredQuantity,
                .observedResourceAttempts = input.observedResourceAttempts,
                .observedResourceSeconds = input.observedResourceSeconds,
                .authoritativeInteractionSeconds = input.authoritativeInteractionSeconds,
                .remainingDedicatedActivitySeconds = input.remainingDedicatedActivitySeconds,
                .requiredResourceCount = static_cast<std::uint32_t>(requiredResourceCount),
                .secondsPerResource = static_cast<std::uint32_t>(secondsPerResource),
                .startingInventoryQuantity = input.startingInventoryQuantity,
                .availableResourceCount = input.availableResourceCount,
                .neededBy = neededBy,
            },
    };
}
}  // namespace PlayerbotEconomy::MaterialCommitmentEncoding
