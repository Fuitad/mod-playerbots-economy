/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTCAREERPOPULATION_H
#define PLAYERBOTS_PLAYERBOTCAREERPOPULATION_H

#include <cstdint>
#include <functional>
#include <vector>

#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "PlayerbotCareerPlan.h"

// Population aware career selection. A bot still draws its career from its own affinity weighted pool;
// this only rewrites the weights so a profession the population is short of becomes more likely and one
// it already has plenty of does not. It never adds, removes or reorders candidates, so class legality,
// race legality and reachability are all decided before anything here runs, and affinity remains the
// tiebreak between two professions the population is equally short of.
namespace PlayerbotCareerPopulation
{
inline constexpr std::uint32_t BIAS_NEUTRAL_PERMILLE = 1000;

struct Targets
{
    // Share of assigned primary profession slots that should be gathering professions.
    std::uint32_t gatheringSharePercent = 60;
    // Minimum share of assigned primary profession slots any one profession should hold. With eleven
    // primary professions an even split is 91 permille, and a 60/40 gathering split puts the natural
    // share near 200 permille per gathering profession and 50 per crafting one.
    std::uint32_t professionFloorPermille = 40;
    // Extra weight, in percent, a profession sitting at zero against a nonzero floor receives. The
    // boost scales with the deficit and reaches zero exactly at the floor, so it cannot overshoot.
    std::uint32_t floorBoostPercent = 400;
    // Extra weight, in percent, a profession on the short side of the gathering share receives when the
    // realised share misses the target completely. It scales with the size of the miss.
    std::uint32_t shareBoostPercent = 600;
};

using SkillPredicate = std::function<bool(std::uint16_t)>;

// Weight multiplier in permille for one candidate's primary professions. BIAS_NEUTRAL_PERMILLE leaves
// the candidate exactly as affinity ranked it, and the value never falls below neutral: an
// over-represented profession is left alone rather than penalised, so no legal career can be weighted
// out of the pool. A candidate with no primary profession (no professions at all, or secondary skills
// only) is always neutral, and so is every candidate while the census is still empty.
[[nodiscard]] std::uint32_t CandidateBiasPermille(std::vector<std::uint16_t> const& primarySkills,
                                                  PlayerbotProfessionCensus const& census, Targets const& targets,
                                                  SkillPredicate const& isGathering);

// True when at least one primary profession sits below the configured floor. The full primary
// profession list is required rather than the census alone, because a profession nobody has taken is
// absent from the census and is exactly the starved case the floor exists to catch. False while the
// census is empty (no evidence yet) and false when the floor is disabled.
[[nodiscard]] bool PopulationNeedsCoverage(PlayerbotProfessionCensus const& census, Targets const& targets,
                                           std::vector<std::uint16_t> const& primaryProfessionSkillIds);

// affinityWeight scaled by biasPermille, saturating rather than wrapping.
[[nodiscard]] std::uint64_t BiasedWeight(std::uint64_t affinityWeight, std::uint32_t biasPermille);

void ApplyPopulationBias(std::vector<PlayerbotCareerCandidate>& candidates, PlayerbotProfessionCensus const& census,
                         Targets const& targets, SkillPredicate const& isGathering);
}  // namespace PlayerbotCareerPopulation

#endif  // PLAYERBOTS_PLAYERBOTCAREERPOPULATION_H
