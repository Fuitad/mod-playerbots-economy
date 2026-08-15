/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotMaterialCommitmentEncoding.h"

#include <iomanip>
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
}  // namespace PlayerbotEconomy::MaterialCommitmentEncoding
