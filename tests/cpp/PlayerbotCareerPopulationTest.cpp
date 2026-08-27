/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <vector>

#include "Bot/Economy/PlayerbotEconomyTelemetry.h"
#include "Bot/Personality/PlayerbotCareerPlan.h"
#include "Bot/Personality/PlayerbotCareerPopulation.h"
#include "Bot/Personality/PlayerbotCareerSeeds.h"
#include "SharedDefines.h"
#include "gtest/gtest.h"

namespace
{
using PlayerbotCareerPopulation::BIAS_NEUTRAL_PERMILLE;
using PlayerbotCareerPopulation::CandidateBiasPermille;
// The core already has a global Targets, so the population targets need their own name here.
using PopulationTargets = PlayerbotCareerPopulation::Targets;

bool IsGathering(uint16 skillId) { return PlayerbotCareerSeeds::IsGatheringSkill(skillId); }

PlayerbotProfessionCensus Census(std::map<uint16, uint32> const& counts)
{
    PlayerbotProfessionCensus census;
    for (auto const& [skillId, careers] : counts)
    {
        if (!careers)
            continue;
        census.primaries.push_back({skillId, careers});
        census.primarySlots += careers;
    }
    census.careers = static_cast<uint32>(counts.size());
    return census;
}

// A census with a deliberate shape: 600 permille gathering (exactly on the default target) so the share
// term contributes nothing, leaving the floor as the only live term.
PlayerbotProfessionCensus BalancedCensus(uint32 jewelcrafting)
{
    return Census({{SKILL_MINING, 60},
                   {SKILL_HERBALISM, 60},
                   {SKILL_SKINNING, 60},
                   {SKILL_TAILORING, 40 - jewelcrafting},
                   {SKILL_ENCHANTING, 40},
                   {SKILL_ALCHEMY, 40},
                   {SKILL_JEWELCRAFTING, jewelcrafting}});
}

PopulationTargets FloorOnly() { return {60, 40, 400, 0}; }
PopulationTargets ShareOnly() { return {60, 0, 0, 600}; }
PopulationTargets NoPopulationTerms() { return {60, 0, 0, 0}; }

PlayerbotPersonalityProfile Profile(uint8 crafting, uint8 gathering)
{
    PlayerbotPersonalityProfile profile;
    profile.craftingAffinity = crafting;
    profile.gatheringAffinity = gathering;
    profile.economyAffinity = 50;
    profile.explorationAffinity = 50;
    profile.sociability = 50;
    profile.voice = PlayerbotVoice::Pragmatic;
    return profile;
}

// Every primary profession slot a population would hold, so the simulation measures the same thing an
// operator counts off the realm.
struct PopulationOutcome
{
    std::map<uint16, uint32> counts;
    uint32 primarySlots = 0;
    uint32 gatheringSlots = 0;

    [[nodiscard]] uint32 GatheringPermille() const { return primarySlots ? gatheringSlots * 1000u / primarySlots : 0u; }

    [[nodiscard]] uint32 Careers(uint16 skillId) const
    {
        auto const found = counts.find(skillId);
        return found == counts.end() ? 0u : found->second;
    }
};

// A fresh population, assigned one bot at a time exactly as the adapter does: build the class pool,
// weight it by affinity, bias it by what the population already holds, then draw. The affinities stand
// in for PlayerbotPersonalityMgr::Generate, which draws urand(0, 100) from an unseeded source.
PopulationOutcome SimulatePopulation(uint32 bots, PopulationTargets const& targets)
{
    constexpr std::array<uint8, 10> CLASSES = {CLASS_WARRIOR, CLASS_PALADIN,      CLASS_HUNTER, CLASS_ROGUE,
                                               CLASS_PRIEST,  CLASS_DEATH_KNIGHT, CLASS_SHAMAN, CLASS_MAGE,
                                               CLASS_WARLOCK, CLASS_DRUID};
    PopulationOutcome outcome;
    for (uint32 bot = 1; bot <= bots; ++bot)
    {
        uint64 const draw = PlayerbotPersonality::SplitMix64(bot ^ 0x504F50554C4154ULL);
        PlayerbotPersonalityProfile const profile =
            Profile(static_cast<uint8>(draw % 101u), static_cast<uint8>((draw >> 21u) % 101u));

        std::vector<PlayerbotCareerCandidateSeed> const seeds =
            PlayerbotCareerSeeds::Build(CLASSES[bot % CLASSES.size()], {}, 2u, 30u);
        std::vector<PlayerbotCareerCandidate> candidates = PlayerbotCareer::BuildCandidates(profile, seeds, 2u);
        PlayerbotCareerPopulation::ApplyPopulationBias(candidates, Census(outcome.counts), targets, IsGathering);

        PlayerbotCareerCandidate const chosen = PlayerbotCareer::SelectFallback(candidates, bot);
        for (uint16 skillId : chosen.primarySkills)
        {
            ++outcome.counts[skillId];
            ++outcome.primarySlots;
            if (IsGathering(skillId))
                ++outcome.gatheringSlots;
        }
    }
    return outcome;
}

// A provider that always answers with the same candidate, standing in for the LLM career lane, which
// is told tokens, summaries, engagement and spending styles and nothing about the population.
class FixedCareerProvider : public PlayerbotCareerPlanProvider
{
public:
    explicit FixedCareerProvider(std::string answerToken) : answer(std::move(answerToken)) {}

    bool TrySubmit(PlayerbotCareerPlanRequest const& request) override
    {
        submissions++;
        response = {request.requestId,     request.botGuid, request.personalityVersion,
                    request.careerVersion, answer,          PlayerbotRecipeSpendingStyle::Minimal};
        return true;
    }

    std::optional<PlayerbotCareerPlanResponse> Poll(uint64 requestId) override
    {
        return response && response->requestId == requestId ? response : std::nullopt;
    }

    uint64 ResponseDeadlineMs() const override { return 10000u; }

    std::string answer;
    std::optional<PlayerbotCareerPlanResponse> response;
    uint32 submissions = 0;
};

std::vector<uint16> AllPrimaryProfessions()
{
    return {SKILL_ALCHEMY,   SKILL_BLACKSMITHING, SKILL_ENCHANTING,    SKILL_ENGINEERING,
            SKILL_HERBALISM, SKILL_INSCRIPTION,   SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
            SKILL_MINING,    SKILL_SKINNING,      SKILL_TAILORING};
}

void ReportPopulation(char const* label, PopulationOutcome const& outcome)
{
    std::cout << "[population] " << label << " slots=" << outcome.primarySlots
              << " gathering=" << outcome.GatheringPermille() << "permille";
    for (auto const& [skillId, careers] : outcome.counts)
        std::cout << ' ' << skillId << '=' << careers;
    std::cout << '\n';
}
}  // namespace

// Breaks if the empty-census guard is dropped: the floor and share terms would divide by a zero slot
// count and boost a population nothing is known about.
TEST(PlayerbotCareerPopulationTest, AnEmptyCensusLeavesEveryCandidateAtItsAffinityWeight)
{
    PlayerbotProfessionCensus const empty;
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE,
              CandidateBiasPermille({SKILL_JEWELCRAFTING}, empty, PopulationTargets{}, IsGathering));
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, CandidateBiasPermille({SKILL_MINING}, empty, PopulationTargets{}, IsGathering));

    std::vector<PlayerbotCareerCandidate> candidates = {{"a", "", {SKILL_JEWELCRAFTING}, {}, {}, false, 0u, 7777u}};
    PlayerbotCareerPopulation::ApplyPopulationBias(candidates, empty, PopulationTargets{}, IsGathering);
    EXPECT_EQ(7777u, candidates.front().weight);
}

// Breaks if the floor term is removed, or reads the population total instead of the profession's own
// count: Jewelcrafting at zero and Alchemy well above the floor would then weigh the same.
TEST(PlayerbotCareerPopulationTest, AProfessionBelowItsFloorOutdrawsOneAboveIt)
{
    PlayerbotProfessionCensus const census = BalancedCensus(0);
    ASSERT_EQ(0u, CensusCareers(census, SKILL_JEWELCRAFTING));

    uint32 const starved = CandidateBiasPermille({SKILL_JEWELCRAFTING}, census, FloorOnly(), IsGathering);
    uint32 const served = CandidateBiasPermille({SKILL_ALCHEMY}, census, FloorOnly(), IsGathering);

    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, served);
    // The floor is 40 permille of 300 slots, so a profession at zero owes the whole deficit and draws
    // at the configured 400 percent extra.
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE + 4000u, starved);
    EXPECT_GT(starved, served);
}

// Breaks if the boost becomes a flat multiplier while below the floor instead of scaling with the
// deficit: the halfway census would then read the same as the empty one, and the profession would keep
// its boost right up to the floor and overshoot past it.
TEST(PlayerbotCareerPopulationTest, TheFloorBoostFadesWithTheDeficitAndStopsAtTheFloor)
{
    uint32 const atZero = CandidateBiasPermille({SKILL_JEWELCRAFTING}, BalancedCensus(0), FloorOnly(), IsGathering);
    uint32 const halfway = CandidateBiasPermille({SKILL_JEWELCRAFTING}, BalancedCensus(6), FloorOnly(), IsGathering);
    uint32 const atFloor = CandidateBiasPermille({SKILL_JEWELCRAFTING}, BalancedCensus(12), FloorOnly(), IsGathering);
    uint32 const above = CandidateBiasPermille({SKILL_JEWELCRAFTING}, BalancedCensus(30), FloorOnly(), IsGathering);

    EXPECT_GT(atZero, halfway);
    EXPECT_GT(halfway, atFloor);
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, atFloor);
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, above);
}

// Breaks if the direction test on the share term is dropped or inverted: gathering would keep its boost
// once the population had already reached the target, and would overshoot it.
TEST(PlayerbotCareerPopulationTest, GatheringIsBoostedOnlyWhileTheRealisedShareIsBelowTarget)
{
    PlayerbotProfessionCensus const shortOfTarget = Census({{SKILL_MINING, 30}, {SKILL_ALCHEMY, 70}});
    PlayerbotProfessionCensus const onTarget = Census({{SKILL_MINING, 60}, {SKILL_ALCHEMY, 40}});

    uint32 const boosted = CandidateBiasPermille({SKILL_MINING}, shortOfTarget, ShareOnly(), IsGathering);
    uint32 const settled = CandidateBiasPermille({SKILL_MINING}, onTarget, ShareOnly(), IsGathering);

    // 300 permille realised against a 600 permille target is half the way to a complete miss, so half
    // of the configured 600 percent lands.
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE + 3000u, boosted);
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, settled);
}

// Breaks if only the gathering side of the share term is implemented: a population that overshot
// gathering would have nothing pulling it back toward crafting.
TEST(PlayerbotCareerPopulationTest, CraftingIsBoostedWhenGatheringOvershootsItsTarget)
{
    PlayerbotProfessionCensus const overshot = Census({{SKILL_MINING, 80}, {SKILL_ALCHEMY, 20}});

    EXPECT_GT(CandidateBiasPermille({SKILL_ALCHEMY}, overshot, ShareOnly(), IsGathering), BIAS_NEUTRAL_PERMILLE);
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, CandidateBiasPermille({SKILL_MINING}, overshot, ShareOnly(), IsGathering));
}

// Breaks if the per-skill biases are summed or maxed instead of averaged: a mixed pair would then match
// or beat the pure gathering pair it is half of, and a two-skill career would always outdraw a
// one-skill career regardless of what the population needs.
TEST(PlayerbotCareerPopulationTest, AMixedPairSitsBetweenThePureCareersItIsMadeOf)
{
    PlayerbotProfessionCensus const shortOfTarget = Census({{SKILL_MINING, 30}, {SKILL_ALCHEMY, 70}});

    uint32 const pureGathering =
        CandidateBiasPermille({SKILL_MINING, SKILL_HERBALISM}, shortOfTarget, ShareOnly(), IsGathering);
    uint32 const mixed = CandidateBiasPermille({SKILL_MINING, SKILL_ALCHEMY}, shortOfTarget, ShareOnly(), IsGathering);
    uint32 const pureCrafting =
        CandidateBiasPermille({SKILL_ALCHEMY, SKILL_TAILORING}, shortOfTarget, ShareOnly(), IsGathering);

    EXPECT_GT(pureGathering, mixed);
    EXPECT_GT(mixed, pureCrafting);
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, pureCrafting);
}

// Breaks if the empty-primaries guard is dropped: averaging over no skills divides by zero, and the
// no-professions candidate and the secondary-only variants would be dragged around by a population
// term that says nothing about them.
TEST(PlayerbotCareerPopulationTest, ACandidateWithNoPrimaryProfessionKeepsItsWeight)
{
    PlayerbotProfessionCensus const census = BalancedCensus(0);
    EXPECT_EQ(BIAS_NEUTRAL_PERMILLE, CandidateBiasPermille({}, census, PopulationTargets{}, IsGathering));

    std::vector<PlayerbotCareerCandidate> candidates = {
        {"career-none", "no professions", {}, {}, {}, false, 0u, 909u},
        {"cooking", "secondary", {}, {SKILL_COOKING}, {}, false, 0u, 51u}};
    PlayerbotCareerPopulation::ApplyPopulationBias(candidates, census, PopulationTargets{}, IsGathering);
    EXPECT_EQ(909u, candidates[0].weight);
    EXPECT_EQ(51u, candidates[1].weight);
}

// Breaks if either population term is allowed to subtract: an over-represented but legal career would
// lose weight, and with enough over-representation could be weighted out of a pool it belongs in.
TEST(PlayerbotCareerPopulationTest, AnOverRepresentedProfessionIsNeverWeightedBelowItsAffinityWeight)
{
    PlayerbotProfessionCensus const gatheringHeavy =
        Census({{SKILL_MINING, 500}, {SKILL_HERBALISM, 400}, {SKILL_ALCHEMY, 1}});

    std::vector<PlayerbotCareerCandidate> candidates = {{"m", "", {SKILL_MINING}, {}, {}, false, 0u, 1000u},
                                                        {"h", "", {SKILL_HERBALISM}, {}, {}, false, 0u, 1000u}};
    PlayerbotCareerPopulation::ApplyPopulationBias(candidates, gatheringHeavy, PopulationTargets{}, IsGathering);
    EXPECT_GE(candidates[0].weight, 1000u);
    EXPECT_GE(candidates[1].weight, 1000u);
}

// Breaks if the overflow guard on the multiply is removed: the product wraps and the heaviest candidate
// in the pool turns into the lightest.
TEST(PlayerbotCareerPopulationTest, BiasSaturatesInsteadOfWrappingOnAnExtremeWeight)
{
    constexpr uint64 huge = std::numeric_limits<uint64>::max() - 1u;
    EXPECT_EQ(std::numeric_limits<uint64>::max(), PlayerbotCareerPopulation::BiasedWeight(huge, 5000u));
    EXPECT_EQ(2000u, PlayerbotCareerPopulation::BiasedWeight(1000u, 2000u));
    EXPECT_EQ(1000u, PlayerbotCareerPopulation::BiasedWeight(1000u, BIAS_NEUTRAL_PERMILLE));
}

// Breaks if the class pool stops being blended with the class agnostic pool, or if a lone crafting
// profession stops carrying the gathering skill that feeds it: a Priest would then never see Mining,
// and Jewelcrafting would never appear without its Mining pair.
TEST(PlayerbotCareerPopulationTest, SeedsBlendClassPairsWithTheRandomPoolAndCarryFeeders)
{
    std::vector<PlayerbotCareerCandidateSeed> const seeds = PlayerbotCareerSeeds::Build(CLASS_PRIEST, {}, 2u, 30u);

    auto const hasPrimaryPair = [&seeds](uint16 first, uint16 second)
    {
        return std::any_of(seeds.begin(), seeds.end(),
                           [first, second](PlayerbotCareerCandidateSeed const& seed)
                           {
                               return seed.primarySkills.size() == 2u &&
                                      std::find(seed.primarySkills.begin(), seed.primarySkills.end(), first) !=
                                          seed.primarySkills.end() &&
                                      std::find(seed.primarySkills.begin(), seed.primarySkills.end(), second) !=
                                          seed.primarySkills.end();
                           });
    };

    EXPECT_TRUE(hasPrimaryPair(SKILL_TAILORING, SKILL_ENCHANTING));
    // Mining reaches a Priest only through the class agnostic pool.
    EXPECT_TRUE(hasPrimaryPair(SKILL_MINING, SKILL_BLACKSMITHING));
    EXPECT_TRUE(hasPrimaryPair(SKILL_JEWELCRAFTING, SKILL_MINING));
    EXPECT_TRUE(std::any_of(seeds.begin(), seeds.end(), [](PlayerbotCareerCandidateSeed const& seed)
                            { return seed.feeder && seed.primarySkills.front() == SKILL_JEWELCRAFTING; }));
}

// The behaviour Pierre asked for, measured end to end: a fresh population assigned one bot at a time
// must land near a 60 percent gathering share and must not leave any profession starved. Breaks if
// either population term stops steering, which the no-terms run in the same test proves by carrying
// the old, luck-driven distribution.
TEST(PlayerbotCareerPopulationTest, AFreshPopulationClearsTheJewelcraftingFloorAndReachesTheGatheringTarget)
{
    constexpr uint32 BOTS = 200;
    PopulationTargets const defaults;

    PopulationOutcome const luck = SimulatePopulation(BOTS, NoPopulationTerms());
    PopulationOutcome const steered = SimulatePopulation(BOTS, defaults);
    ReportPopulation("unbiased", luck);
    ReportPopulation("biased", steered);

    uint32 const floorCareers = steered.primarySlots * defaults.professionFloorPermille / 1000u;
    ASSERT_GT(floorCareers, 0u);
    constexpr std::array<uint16, 11> PRIMARY_PROFESSIONS = {
        SKILL_ALCHEMY,   SKILL_BLACKSMITHING, SKILL_ENCHANTING,    SKILL_ENGINEERING,
        SKILL_HERBALISM, SKILL_INSCRIPTION,   SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING,
        SKILL_MINING,    SKILL_SKINNING,      SKILL_TAILORING};
    for (uint16 skillId : PRIMARY_PROFESSIONS)
    {
        EXPECT_GE(steered.Careers(skillId), floorCareers)
            << "profession " << skillId << " is below the population floor";
    }

    EXPECT_GT(steered.Careers(SKILL_JEWELCRAFTING), luck.Careers(SKILL_JEWELCRAFTING));

    uint32 const target = defaults.gatheringSharePercent * 10u;
    uint32 const steeredMiss = steered.GatheringPermille() > target ? steered.GatheringPermille() - target
                                                                    : target - steered.GatheringPermille();
    uint32 const luckMiss =
        luck.GatheringPermille() > target ? luck.GatheringPermille() - target : target - luck.GatheringPermille();
    EXPECT_LT(steeredMiss, luckMiss);
    EXPECT_LE(steeredMiss, 30u) << "realised gathering share " << steered.GatheringPermille() << " permille";
}

// Breaks if a profession absent from the census is treated as satisfied, which is exactly the starved
// case: a profession nobody has taken has no census row at all, so reading the census alone would
// report the population healthy while Jewelcrafting sat at zero.
TEST(PlayerbotCareerPopulationTest, APopulationMissingAProfessionEntirelyStillReportsAGap)
{
    PopulationTargets const defaults;
    PlayerbotProfessionCensus const missingJewelcrafting = Census(
        {{SKILL_MINING, 60}, {SKILL_HERBALISM, 60}, {SKILL_SKINNING, 60}, {SKILL_ALCHEMY, 60}, {SKILL_TAILORING, 60}});
    EXPECT_TRUE(
        PlayerbotCareerPopulation::PopulationNeedsCoverage(missingJewelcrafting, defaults, AllPrimaryProfessions()));

    // An empty census carries no evidence, and a disabled floor asks for none.
    EXPECT_FALSE(PlayerbotCareerPopulation::PopulationNeedsCoverage({}, defaults, AllPrimaryProfessions()));
    EXPECT_FALSE(PlayerbotCareerPopulation::PopulationNeedsCoverage(missingJewelcrafting, NoPopulationTerms(),
                                                                    AllPrimaryProfessions()));
}

// A career provider is told nothing about the population, so a successful provider answer would
// silently ignore the floor. Bypassing the provider while a floor is unmet is a design decision taken
// in this change and not yet reviewed by the repository owner; see the README. Breaks if ResolvePlan
// stops honouring the bypass, or if the bypass leaks into a healthy population and retires the
// provider altogether.
TEST(PlayerbotCareerPopulationTest, ASuccessfulProviderAnswerCannotOverrideAnUnmetProfessionFloor)
{
    std::vector<PlayerbotCareerCandidate> const candidates = {
        {"career-gathering", "gathering", {SKILL_MINING}, {}, PlayerbotRecipeSpendingStyle::Minimal, false, 50u, 1000u},
        {"career-starved",
         "starved",
         {SKILL_JEWELCRAFTING},
         {},
         PlayerbotRecipeSpendingStyle::Minimal,
         false,
         50u,
         1000u}};

    FixedCareerProvider provider("career-gathering");
    ASSERT_TRUE(PlayerbotCareer::RegisterProvider(&provider));

    // A healthy population leaves the provider in charge: its answer is the plan.
    PlayerbotCareerPlanResolution const healthy =
        PlayerbotCareer::ResolvePlan(7001u, Profile(80, 80), candidates, 0u, PlayerbotCareerProviderUse::Allowed);
    PlayerbotCareerPlanResolution const healthyResolved =
        PlayerbotCareer::ResolvePlan(7001u, Profile(80, 80), candidates, 1u, PlayerbotCareerProviderUse::Allowed);
    EXPECT_EQ(PlayerbotCareerPlanResolutionStatus::Pending, healthy.status);
    ASSERT_EQ(PlayerbotCareerPlanResolutionStatus::Resolved, healthyResolved.status);
    EXPECT_EQ("career-gathering", healthyResolved.plan.candidateToken);
    EXPECT_EQ(1u, provider.submissions);

    // An unmet floor takes the assignment back: the provider is never asked, and the weighted draw
    // decides. The starved candidate carries the whole weight here, so it must be the one chosen.
    std::vector<PlayerbotCareerCandidate> weighted = candidates;
    weighted[0].weight = 1u;
    weighted[1].weight = 100000u;
    PlayerbotCareerPlanResolution const covered = PlayerbotCareer::ResolvePlan(
        7002u, Profile(80, 80), weighted, 0u, PlayerbotCareerProviderUse::BypassedForPopulationCoverage);
    EXPECT_EQ(PlayerbotCareerPlanResolutionStatus::Resolved, covered.status);
    EXPECT_EQ("career-starved", covered.plan.candidateToken);
    EXPECT_EQ(1u, provider.submissions) << "the provider was consulted while a profession floor was unmet";

    // A request already in flight cannot land behind the bypass either.
    PlayerbotCareerPlanResolution const inFlight =
        PlayerbotCareer::ResolvePlan(7003u, Profile(80, 80), candidates, 0u, PlayerbotCareerProviderUse::Allowed);
    ASSERT_EQ(PlayerbotCareerPlanResolutionStatus::Pending, inFlight.status);
    PlayerbotCareerPlanResolution const dropped = PlayerbotCareer::ResolvePlan(
        7003u, Profile(80, 80), weighted, 1u, PlayerbotCareerProviderUse::BypassedForPopulationCoverage);
    EXPECT_EQ(PlayerbotCareerPlanResolutionStatus::Resolved, dropped.status);
    EXPECT_EQ("career-starved", dropped.plan.candidateToken);

    PlayerbotCareer::UnregisterProvider(&provider);
}

// Breaks if the overflow guard saturates on the intermediate product alone: a weight whose scaled
// result is perfectly representable would be flattened to the maximum and lose its ordering.
TEST(PlayerbotCareerPopulationTest, BiasSaturatesOnlyWhenTheScaledResultTrulyDoesNotFit)
{
    constexpr uint64 quarter = std::numeric_limits<uint64>::max() / 4u;
    uint64 const doubled = PlayerbotCareerPopulation::BiasedWeight(quarter, 2000u);
    EXPECT_LT(doubled, std::numeric_limits<uint64>::max());
    EXPECT_GT(doubled, quarter);
}
