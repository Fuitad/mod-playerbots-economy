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

// Generic success does not prove that the recorded failed precondition changed.
void PlayerbotEconomyFailureTracker::RecordUnrelatedSuccess() const {}

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

std::uint32_t CensusCareers(PlayerbotProfessionCensus const& census, std::uint16_t skillId)
{
    auto const found = std::lower_bound(census.primaries.begin(), census.primaries.end(), skillId,
                                        [](PlayerbotProfessionCount const& count, std::uint16_t wanted)
                                        { return count.skillId < wanted; });
    return found != census.primaries.end() && found->skillId == skillId ? found->careers : 0u;
}

void PlayerbotEconomyTelemetry::PublishCareerPending(std::uint32_t characterGuid) { PublishCareer(characterGuid, {}); }

void PlayerbotEconomyTelemetry::PublishCareer(std::uint32_t characterGuid, PlayerbotCareerObservation observation)
{
    std::scoped_lock lock(mutex);
    PlayerbotCareerObservation& stored = careerObservations[characterGuid];
    ApplyCensusDelta(stored, false);
    stored = std::move(observation);
    ApplyCensusDelta(stored, true);
}

void PlayerbotEconomyTelemetry::ApplyCensusDelta(PlayerbotCareerObservation const& observation, bool added)
{
    if (observation.status != PlayerbotCareerTelemetryStatus::Valid)
        return;

    // Counted with unsigned arithmetic throughout. Every removal subtracts exactly the observation
    // that was stored, so the guards below can only fire if that invariant were ever broken.
    if (added)
        ++observedCareers;
    else if (observedCareers)
        --observedCareers;

    auto const apply = [this, added](std::vector<std::uint16_t> const& skills)
    {
        for (std::uint16_t skillId : skills)
        {
            if (!skillId)
                continue;

            if (added)
            {
                ++primaryCareerCounts[skillId];
                ++primaryCareerSlots;
                continue;
            }

            auto const found = primaryCareerCounts.find(skillId);
            if (found == primaryCareerCounts.end())
                continue;
            if (!--found->second)
                primaryCareerCounts.erase(found);
            if (primaryCareerSlots)
                --primaryCareerSlots;
        }
    };
    // A career the bot already holds beyond its plan is still a profession the population carries, so
    // the amendments count exactly like the planned primaries (EffectivePrimarySkills).
    apply(observation.primarySkills);
    apply(observation.primarySkillAmendments);
}

PlayerbotProfessionCensus PlayerbotEconomyTelemetry::SnapshotProfessionCensus() const
{
    std::scoped_lock lock(mutex);
    PlayerbotProfessionCensus census;
    census.primarySlots = primaryCareerSlots;
    census.careers = observedCareers;
    census.primaries.reserve(primaryCareerCounts.size());
    for (auto const& [skillId, careers] : primaryCareerCounts)
        census.primaries.push_back({skillId, careers});
    std::sort(census.primaries.begin(), census.primaries.end(),
              [](PlayerbotProfessionCount const& left, PlayerbotProfessionCount const& right)
              { return left.skillId < right.skillId; });
    return census;
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
