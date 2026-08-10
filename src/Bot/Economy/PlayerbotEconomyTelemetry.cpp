/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotEconomyTelemetry.h"

#include <algorithm>
#include <utility>

void PlayerbotEconomyFailureTracker::RecordFailure(std::string nextFingerprint)
{
    if (nextFingerprint != fingerprint)
    {
        fingerprint = std::move(nextFingerprint);
        count = 0;
    }

    count =
        std::min<std::uint8_t>(static_cast<std::uint8_t>(count + 1u), PLAYERBOT_ECONOMY_FAILURE_QUARANTINE_THRESHOLD);
}

void PlayerbotEconomyFailureTracker::Clear()
{
    fingerprint.clear();
    count = 0;
}

std::uint8_t PlayerbotEconomyFailureTracker::Count() const { return count; }

bool PlayerbotEconomyFailureTracker::IsQuarantined() const
{
    return count >= PLAYERBOT_ECONOMY_FAILURE_QUARANTINE_THRESHOLD;
}

void PlayerbotEconomyTelemetry::Publish(std::uint32_t characterGuid, PlayerbotEconomyObservation observation)
{
    std::scoped_lock lock(mutex);
    observation.sequence = observations[characterGuid].sequence + 1u;
    observations[characterGuid] = std::move(observation);
}

std::optional<PlayerbotEconomyObservation> PlayerbotEconomyTelemetry::Find(std::uint32_t characterGuid) const
{
    std::scoped_lock lock(mutex);
    auto const found = observations.find(characterGuid);
    return found == observations.end() ? std::nullopt : std::optional<PlayerbotEconomyObservation>(found->second);
}

void PlayerbotEconomyTelemetry::PublishCareerPending(std::uint32_t characterGuid) { PublishCareer(characterGuid, {}); }

void PlayerbotEconomyTelemetry::PublishCareer(std::uint32_t characterGuid, PlayerbotCareerObservation observation)
{
    std::scoped_lock lock(mutex);
    careerObservations[characterGuid] = std::move(observation);
}

std::optional<PlayerbotCareerObservation> PlayerbotEconomyTelemetry::FindCareer(std::uint32_t characterGuid) const
{
    std::scoped_lock lock(mutex);
    auto const found = careerObservations.find(characterGuid);
    return found == careerObservations.end() ? std::nullopt : std::optional<PlayerbotCareerObservation>(found->second);
}

PlayerbotEconomyTelemetry& GetPlayerbotEconomyTelemetry()
{
    static PlayerbotEconomyTelemetry telemetry;
    return telemetry;
}
