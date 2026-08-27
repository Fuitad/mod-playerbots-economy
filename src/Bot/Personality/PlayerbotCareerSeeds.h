/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTCAREERSEEDS_H
#define PLAYERBOTS_PLAYERBOTCAREERSEEDS_H

#include <cstdint>
#include <vector>

#include "PlayerbotCareerPlan.h"

// The career candidate pool, assembled from facts alone: the bot's class, the primary professions it
// has already learned, the primary profession slot limit, and the configured class matching share.
// Nothing here reads a Player, a DBC row or a database, so the pool a population would draw from can
// be built and measured without a running world.
namespace PlayerbotCareerSeeds
{
struct WeightedProfessionPair
{
    std::uint16_t firstSkill = 0;
    std::uint16_t secondSkill = 0;
    std::uint32_t weight = 0;
};

// The eleven primary professions of 3.3.5a. Naming them here is what keeps seed assembly free of
// SkillLine.dbc; the authoritative DBC derived list is still what validates a persisted plan.
[[nodiscard]] bool IsGatheringSkill(std::uint16_t skillId);
[[nodiscard]] bool IsCraftingSkill(std::uint16_t skillId);

// Profession pairs a class is steered toward, and the class agnostic pool it is blended with.
[[nodiscard]] std::vector<WeightedProfessionPair> ClassProfessionPairs(std::uint8_t classId);
[[nodiscard]] std::vector<WeightedProfessionPair> RandomProfessionPairs();

[[nodiscard]] std::vector<PlayerbotCareerCandidateSeed> Build(std::uint8_t classId,
                                                              std::vector<std::uint16_t> const& learnedPrimarySkills,
                                                              std::uint32_t maxPrimarySkills,
                                                              std::uint32_t classMatchingProfessionChance);
}  // namespace PlayerbotCareerSeeds

#endif  // PLAYERBOTS_PLAYERBOTCAREERSEEDS_H
