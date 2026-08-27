/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotCareerSeeds.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <utility>

#include "SharedDefines.h"

namespace
{
constexpr std::array<std::uint16_t, 11> PRIMARY_PROFESSION_SKILLS = {
    SKILL_HERBALISM,     SKILL_MINING,         SKILL_SKINNING,    SKILL_ALCHEMY,
    SKILL_BLACKSMITHING, SKILL_ENCHANTING,     SKILL_ENGINEERING, SKILL_INSCRIPTION,
    SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_TAILORING};
}  // namespace

bool PlayerbotCareerSeeds::IsGatheringSkill(std::uint16_t skillId)
{
    return skillId == SKILL_HERBALISM || skillId == SKILL_MINING || skillId == SKILL_SKINNING;
}

bool PlayerbotCareerSeeds::IsCraftingSkill(std::uint16_t skillId)
{
    return !IsGatheringSkill(skillId) && std::find(PRIMARY_PROFESSION_SKILLS.begin(), PRIMARY_PROFESSION_SKILLS.end(),
                                                   skillId) != PRIMARY_PROFESSION_SKILLS.end();
}

std::vector<PlayerbotCareerSeeds::WeightedProfessionPair> PlayerbotCareerSeeds::ClassProfessionPairs(
    std::uint8_t classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
            return {{SKILL_MINING, SKILL_BLACKSMITHING, 45},
                    {SKILL_MINING, SKILL_ENGINEERING, 30},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 15},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 10}};
        case CLASS_PALADIN:
            return {{SKILL_MINING, SKILL_BLACKSMITHING, 45},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 30},
                    {SKILL_MINING, SKILL_ENGINEERING, 15},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 10}};
        case CLASS_DEATH_KNIGHT:
            return {{SKILL_MINING, SKILL_BLACKSMITHING, 45},
                    {SKILL_MINING, SKILL_ENGINEERING, 35},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 20}};
        case CLASS_HUNTER:
            return {{SKILL_SKINNING, SKILL_LEATHERWORKING, 45},
                    {SKILL_MINING, SKILL_ENGINEERING, 35},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 10},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 10}};
        case CLASS_ROGUE:
            return {{SKILL_SKINNING, SKILL_LEATHERWORKING, 35},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25},
                    {SKILL_MINING, SKILL_ENGINEERING, 25},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 10},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 5}};
        case CLASS_DRUID:
            return {{SKILL_SKINNING, SKILL_LEATHERWORKING, 35},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 35},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 20},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 10}};
        case CLASS_SHAMAN:
            return {{SKILL_HERBALISM, SKILL_ALCHEMY, 35},
                    {SKILL_SKINNING, SKILL_LEATHERWORKING, 25},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 25},
                    {SKILL_MINING, SKILL_JEWELCRAFTING, 15}};
        case CLASS_PRIEST:
            return {{SKILL_TAILORING, SKILL_ENCHANTING, 45},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 30},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25}};
        case CLASS_MAGE:
            return {{SKILL_TAILORING, SKILL_ENCHANTING, 50},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 25}};
        case CLASS_WARLOCK:
        default:
            return {{SKILL_TAILORING, SKILL_ENCHANTING, 50},
                    {SKILL_HERBALISM, SKILL_ALCHEMY, 25},
                    {SKILL_HERBALISM, SKILL_INSCRIPTION, 25}};
    }
}

std::vector<PlayerbotCareerSeeds::WeightedProfessionPair> PlayerbotCareerSeeds::RandomProfessionPairs()
{
    return {{SKILL_MINING, SKILL_BLACKSMITHING, 20}, {SKILL_MINING, SKILL_ENGINEERING, 18},
            {SKILL_MINING, SKILL_JEWELCRAFTING, 16}, {SKILL_SKINNING, SKILL_LEATHERWORKING, 18},
            {SKILL_HERBALISM, SKILL_ALCHEMY, 18},    {SKILL_HERBALISM, SKILL_INSCRIPTION, 14},
            {SKILL_TAILORING, SKILL_ENCHANTING, 10}, {SKILL_HERBALISM, SKILL_MINING, 6},
            {SKILL_HERBALISM, SKILL_SKINNING, 5},    {SKILL_MINING, SKILL_SKINNING, 5}};
}

std::vector<PlayerbotCareerCandidateSeed> PlayerbotCareerSeeds::Build(
    std::uint8_t classId, std::vector<std::uint16_t> const& learnedPrimarySkills, std::uint32_t maxPrimarySkills,
    std::uint32_t classMatchingProfessionChance)
{
    std::uint32_t const classWeight = std::min<std::uint32_t>(100u, classMatchingProfessionChance);
    std::uint32_t const randomWeight = 100u - classWeight;

    std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint32_t> weightedPairs;
    auto const addPairs = [&weightedPairs](std::vector<WeightedProfessionPair> const& pairs, std::uint32_t poolWeight)
    {
        if (!poolWeight)
            return;
        for (WeightedProfessionPair const& pair : pairs)
            weightedPairs[{pair.firstSkill, pair.secondSkill}] += pair.weight * poolWeight;
    };
    addPairs(ClassProfessionPairs(classId), classWeight);
    addPairs(RandomProfessionPairs(), randomWeight);

    std::map<std::uint16_t, std::uint32_t> weightedSkills;
    std::vector<PlayerbotCareerCandidateSeed> primarySeeds;
    for (auto const& [skills, weight] : weightedPairs)
    {
        weightedSkills[skills.first] += weight;
        weightedSkills[skills.second] += weight;
        if (maxPrimarySkills < 2u)
            continue;

        bool const hasCrafting = IsCraftingSkill(skills.first) || IsCraftingSkill(skills.second);
        bool const hasGathering = IsGatheringSkill(skills.first) || IsGatheringSkill(skills.second);
        primarySeeds.push_back(
            {{skills.first, skills.second},
             {},
             hasCrafting,
             hasGathering,
             weight,
             hasCrafting && hasGathering ? "mixed primary professions" : "complementary primary professions"});
    }

    if (maxPrimarySkills)
    {
        for (auto const& [skill, weight] : weightedSkills)
        {
            bool const hasCrafting = IsCraftingSkill(skill);
            std::optional<std::uint16_t> const feeder =
                hasCrafting && maxPrimarySkills >= 2u ? PlayerbotCareer::FeederGatheringSkill(skill) : std::nullopt;
            if (feeder)
            {
                // A lone crafter gets the gathering skill that feeds it instead of buying every reagent.
                primarySeeds.push_back(PlayerbotCareer::FeederCraftingSeed(skill, *feeder, weight));
                continue;
            }
            if ((skill == SKILL_TAILORING || skill == SKILL_ENCHANTING) && maxPrimarySkills >= 2u &&
                PlayerbotCareer::FoldSingleSeedWeight(primarySeeds, SKILL_TAILORING, SKILL_ENCHANTING, weight))
            {
                // Tailoring and Enchanting have no feeder; they only come as the pair that supplies itself.
                continue;
            }
            primarySeeds.push_back({{skill},
                                    {},
                                    hasCrafting,
                                    !hasCrafting,
                                    weight,
                                    hasCrafting ? "single crafting profession" : "single gathering profession"});
        }
    }

    // A primary slot cannot be reclaimed, so a seed proposing professions the bot could never fit is
    // unreachable and would strand its career on a trainer objective it can never satisfy. Rewrite
    // every seed onto what the bot can actually reach, merging the duplicates that collapses.
    if (!learnedPrimarySkills.empty())
    {
        std::vector<PlayerbotCareerCandidateSeed> reachableSeeds;
        for (PlayerbotCareerCandidateSeed const& seed : primarySeeds)
        {
            PlayerbotCareerCandidateSeed const reachable = PlayerbotCareer::ReachableSeed(
                seed, learnedPrimarySkills, maxPrimarySkills, [](std::uint16_t skillId)
                { return IsCraftingSkill(skillId); }, [](std::uint16_t skillId) { return IsGatheringSkill(skillId); });
            auto const merged = std::find_if(reachableSeeds.begin(), reachableSeeds.end(),
                                             [&reachable](PlayerbotCareerCandidateSeed const& candidate)
                                             {
                                                 return candidate.primarySkills == reachable.primarySkills &&
                                                        candidate.hasCrafting == reachable.hasCrafting &&
                                                        candidate.hasGathering == reachable.hasGathering &&
                                                        candidate.feeder == reachable.feeder;
                                             });
            if (merged != reachableSeeds.end())
                merged->baseWeight += reachable.baseWeight;
            else
                reachableSeeds.push_back(reachable);
        }
        primarySeeds = std::move(reachableSeeds);
    }

    std::vector<PlayerbotCareerCandidateSeed> seeds = primarySeeds;
    std::array<std::pair<std::uint16_t, bool>, 3> const secondarySkills = {
        {{SKILL_COOKING, true}, {SKILL_FIRST_AID, true}, {SKILL_FISHING, false}}};
    for (auto const& [secondarySkill, isCrafting] : secondarySkills)
    {
        seeds.push_back({{},
                         {secondarySkill},
                         isCrafting,
                         !isCrafting,
                         100u,
                         isCrafting ? "secondary crafting profession" : "secondary gathering profession"});
        for (PlayerbotCareerCandidateSeed const& primary : primarySeeds)
        {
            PlayerbotCareerCandidateSeed variant = primary;
            variant.secondarySkills = {secondarySkill};
            variant.hasCrafting |= isCrafting;
            variant.hasGathering |= !isCrafting;
            variant.summary +=
                isCrafting ? " with an optional crafting secondary" : " with an optional gathering secondary";
            seeds.push_back(std::move(variant));
        }
    }

    return seeds;
}
