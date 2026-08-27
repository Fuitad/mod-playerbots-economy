/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotCareerPopulation.h"

#include <algorithm>
#include <limits>

namespace
{
constexpr std::uint32_t PERCENT_TO_PERMILLE = 10;
constexpr std::uint32_t FULL_PERMILLE = 1000;

std::uint32_t FloorCareers(PlayerbotProfessionCensus const& census, std::uint32_t professionFloorPermille)
{
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(census.primarySlots) *
                                      std::min<std::uint32_t>(FULL_PERMILLE, professionFloorPermille) / FULL_PERMILLE);
}

std::uint32_t GatheringSlots(PlayerbotProfessionCensus const& census,
                             PlayerbotCareerPopulation::SkillPredicate const& isGathering)
{
    std::uint32_t slots = 0;
    for (PlayerbotProfessionCount const& count : census.primaries)
    {
        if (isGathering(count.skillId))
            slots += count.careers;
    }
    return slots;
}
}  // namespace

std::uint32_t PlayerbotCareerPopulation::CandidateBiasPermille(std::vector<std::uint16_t> const& primarySkills,
                                                               PlayerbotProfessionCensus const& census,
                                                               Targets const& targets,
                                                               SkillPredicate const& isGathering)
{
    // Nothing to steer toward: a candidate that occupies no primary slot cannot move the population,
    // and an empty census carries no evidence, so both leave affinity alone.
    if (primarySkills.empty() || !census.primarySlots)
        return BIAS_NEUTRAL_PERMILLE;

    std::uint32_t const targetPermille =
        std::min<std::uint32_t>(100u, targets.gatheringSharePercent) * PERCENT_TO_PERMILLE;
    std::uint32_t const gatheringPermille = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(GatheringSlots(census, isGathering)) * FULL_PERMILLE / census.primarySlots);
    std::uint32_t const floorCareers = FloorCareers(census, targets.professionFloorPermille);

    std::uint64_t total = 0;
    for (std::uint16_t skillId : primarySkills)
    {
        std::uint64_t bias = BIAS_NEUTRAL_PERMILLE;

        // The floor is a deficit the population owes this profession. The boost is proportional to how
        // much of that deficit is still open, so it fades to nothing as the profession fills up and the
        // profession is never pushed past its floor.
        if (floorCareers)
        {
            std::uint32_t const careers = CensusCareers(census, skillId);
            if (careers < floorCareers)
                bias += static_cast<std::uint64_t>(targets.floorBoostPercent) * PERCENT_TO_PERMILLE *
                        (floorCareers - careers) / floorCareers;
        }

        // The share term only ever boosts the side that is short. The side that is long is left at its
        // affinity weight, so a bot that wants to craft still can while the realised share catches up.
        bool const gathering = isGathering(skillId);
        std::uint32_t const span = gathering ? targetPermille : FULL_PERMILLE - targetPermille;
        std::uint32_t gap = 0;
        if (gathering && gatheringPermille < targetPermille)
            gap = targetPermille - gatheringPermille;
        else if (!gathering && gatheringPermille > targetPermille)
            gap = gatheringPermille - targetPermille;
        if (span && gap)
            bias += static_cast<std::uint64_t>(targets.shareBoostPercent) * PERCENT_TO_PERMILLE * gap / span;

        total += bias;
    }

    // A career is judged by the mean of the professions it occupies, so a pair that is short on both
    // sides outranks one that is short on a single side, and a mixed pair sits between them.
    std::uint64_t const mean = total / primarySkills.size();
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(), mean));
}

bool PlayerbotCareerPopulation::PopulationNeedsCoverage(PlayerbotProfessionCensus const& census, Targets const& targets,
                                                        std::vector<std::uint16_t> const& primaryProfessionSkillIds)
{
    if (!census.primarySlots)
        return false;

    std::uint32_t const floorCareers = FloorCareers(census, targets.professionFloorPermille);
    if (!floorCareers)
        return false;

    return std::any_of(primaryProfessionSkillIds.begin(), primaryProfessionSkillIds.end(),
                       [&census, floorCareers](std::uint16_t skillId)
                       { return CensusCareers(census, skillId) < floorCareers; });
}

std::uint64_t PlayerbotCareerPopulation::BiasedWeight(std::uint64_t affinityWeight, std::uint32_t biasPermille)
{
    if (!affinityWeight || !biasPermille)
        return affinityWeight;

    if (affinityWeight <= std::numeric_limits<std::uint64_t>::max() / biasPermille)
        return std::max<std::uint64_t>(1u, affinityWeight * biasPermille / BIAS_NEUTRAL_PERMILLE);

    // The product does not fit, but the quotient still might, so scale down before multiplying. This
    // only discards the remainder of a weight already far larger than any candidate the pool builds.
    std::uint64_t const scaled = affinityWeight / BIAS_NEUTRAL_PERMILLE;
    if (scaled > std::numeric_limits<std::uint64_t>::max() / biasPermille)
        return std::numeric_limits<std::uint64_t>::max();

    return std::max<std::uint64_t>(1u, scaled * biasPermille);
}

void PlayerbotCareerPopulation::ApplyPopulationBias(std::vector<PlayerbotCareerCandidate>& candidates,
                                                    PlayerbotProfessionCensus const& census, Targets const& targets,
                                                    SkillPredicate const& isGathering)
{
    for (PlayerbotCareerCandidate& candidate : candidates)
    {
        std::uint32_t const bias = CandidateBiasPermille(candidate.primarySkills, census, targets, isGathering);
        candidate.weight = BiasedWeight(candidate.weight, bias);
    }
}
