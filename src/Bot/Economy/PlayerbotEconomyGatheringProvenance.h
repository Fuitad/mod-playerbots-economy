/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYGATHERINGPROVENANCE_H
#define PLAYERBOTS_PLAYERBOTECONOMYGATHERINGPROVENANCE_H

#include <cstdint>
#include <string>
#include <vector>

namespace PlayerbotEconomy
{
enum class DedicatedGatheringOriginState : std::uint8_t
{
    Active,
    Latent
};

struct DedicatedGatheringOrigin
{
    std::string originIdentity;
    DedicatedGatheringOriginState state = DedicatedGatheringOriginState::Latent;
    std::uint32_t quantity = 0;
    std::uint64_t expiresAt = 0;

    bool operator==(DedicatedGatheringOrigin const&) const = default;
};

struct DedicatedGatheringTripOriginProvenance
{
    DedicatedGatheringOrigin origin;
    std::uint32_t allocatedQuantity = 0;

    bool operator==(DedicatedGatheringTripOriginProvenance const&) const = default;
};

struct DedicatedGatheringTripProvenance
{
    std::string tripIdentity;
    std::vector<DedicatedGatheringTripOriginProvenance> origins;

    bool operator==(DedicatedGatheringTripProvenance const&) const = default;
};
}  // namespace PlayerbotEconomy

#endif
