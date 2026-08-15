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
        for (MaterialReservationSettlement const& settlement : fulfillment.reservationSettlements)
        {
            AppendCapacity(stream, settlement.capacity);
            stream << settlement.backedMaterialQuantity << ':' << settlement.capacityQuantity << ',';
        }
        stream << '|';
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
}  // namespace PlayerbotEconomy::MaterialCommitmentEncoding
