#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Bot/Economy/PlayerbotEconomyConfig.h"
#include "Bot/Economy/PlayerbotEconomyTelemetry.h"

namespace
{
void Require(bool condition, std::string_view message)
{
    if (condition)
        return;

    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}
}  // namespace

int main()
{
    std::unordered_map<std::string, std::string> const values = {
        {"PlayerbotsEconomy.LifecycleEnabled", "0"},
        {"PlayerbotsEconomy.ClassMatchingProfessionChance", "67"},
        {"PlayerbotsEconomy.Careers.GatheringSharePercent", "55"},
        {"PlayerbotsEconomy.Careers.ProfessionFloorPermille", "35"},
        {"PlayerbotsEconomy.Careers.FloorBoostPercent", "250"},
        {"PlayerbotsEconomy.Careers.ShareBoostPercent", "150"},
        {"PlayerbotsEconomy.ProfessionReserveStacks", "3"},
        {"PlayerbotsEconomy.Production.MaxBatchStacks", "2"},
        {"PlayerbotsEconomy.MarketMakingEnabled", "1"},
        {"PlayerbotsEconomy.MarketMakingPerGroupExposurePercent", "17"},
        {"PlayerbotsEconomy.MarketMakingTotalExposurePercent", "43"},
        {"PlayerbotsEconomy.MarketMakingMinimumEvidence", "9"},
        {"PlayerbotsEconomy.MarketMakingHoldingHorizonSeconds", "7200"},
        {"PlayerbotsEconomy.MarketMakingMaximumRelistAttempts", "4"},
        {"PlayerbotsEconomy.MarketMakingCooldownSeconds", "600"},
    };

    PlayerbotEconomySettings const settings = LoadPlayerbotEconomySettings(
        [&values](std::string_view key) -> std::optional<std::string>
        {
            auto const found = values.find(std::string(key));
            return found == values.end() ? std::nullopt : std::optional<std::string>(found->second);
        });

    Require(!settings.lifecycleEnabled, "nondefault lifecycle setting was not loaded");
    Require(settings.classMatchingProfessionChance == 67, "class matching chance was not loaded");
    Require(settings.careerGatheringSharePercent == 55, "career gathering share was not loaded");
    Require(settings.careerProfessionFloorPermille == 35, "career profession floor was not loaded");
    Require(settings.careerFloorBoostPercent == 250, "career floor boost was not loaded");
    Require(settings.careerShareBoostPercent == 150, "career share boost was not loaded");
    Require(settings.professionReserveStacks == 3, "profession reserve stacks were not loaded");
    Require(settings.productionMaxBatchStacks == 2, "production max batch stacks were not loaded");
    Require(settings.marketMakingEnabled, "nondefault market setting was not loaded");
    Require(settings.marketMakingPerGroupExposurePercent == 17, "per group exposure was not loaded");
    Require(settings.marketMakingTotalExposurePercent == 43, "total exposure was not loaded");
    Require(settings.marketMakingMinimumEvidence == 9, "minimum evidence was not loaded");
    Require(settings.marketMakingHoldingHorizonSeconds == 7200, "holding horizon was not loaded");
    Require(settings.marketMakingMaximumRelistAttempts == 4, "relist attempts were not loaded");
    Require(settings.marketMakingCooldownSeconds == 600, "cooldown was not loaded");

    PlayerbotEconomyTelemetry telemetry;
    PlayerbotEconomyObservation observation;
    observation.outcome = PlayerbotEconomyOutcome::Operation;
    observation.operationIdentity = "auction:42";
    telemetry.Publish(9001, observation);
    auto const stored = telemetry.Find(9001);
    Require(stored.has_value(), "published economy telemetry is missing");
    Require(stored->outcome == PlayerbotEconomyOutcome::Operation, "economy outcome changed");
    Require(stored->operationIdentity == "auction:42", "economy operation identity changed");
    Require(!telemetry.Find(9002).has_value(), "telemetry leaked across bot identities");

    PlayerbotEconomySettings const clamped = LoadPlayerbotEconomySettings(
        [](std::string_view key) -> std::optional<std::string>
        {
            if (key == "PlayerbotsEconomy.Careers.GatheringSharePercent")
                return std::optional<std::string>("400");
            if (key == "PlayerbotsEconomy.Careers.ProfessionFloorPermille")
                return std::optional<std::string>("9000");
            return std::nullopt;
        });
    Require(clamped.careerGatheringSharePercent == 100, "gathering share was not clamped to a percentage");
    Require(clamped.careerProfessionFloorPermille == 1000, "profession floor was not clamped to a permille");

    PlayerbotCareerObservation career;
    career.status = PlayerbotCareerTelemetryStatus::Valid;
    career.primarySkills = {186, 164};
    telemetry.PublishCareer(9001, career);
    career.primarySkills = {182};
    career.primarySkillAmendments = {171};
    telemetry.PublishCareer(9002, career);

    PlayerbotProfessionCensus census = telemetry.SnapshotProfessionCensus();
    Require(census.careers == 2, "census did not count both observed careers");
    Require(census.primarySlots == 4, "census did not count every primary profession slot");
    Require(CensusCareers(census, 186) == 1, "census lost a planned primary profession");
    Require(CensusCareers(census, 171) == 1, "census ignored a primary profession amendment");
    Require(CensusCareers(census, 755) == 0, "census invented a profession nobody plans");

    // Republishing the same bot replaces its contribution rather than adding a second one.
    career.primarySkills = {186};
    career.primarySkillAmendments.clear();
    telemetry.PublishCareer(9002, career);
    census = telemetry.SnapshotProfessionCensus();
    Require(census.careers == 2, "republishing a career changed the observed career count");
    Require(census.primarySlots == 3, "republishing a career did not release its old primary slots");
    Require(CensusCareers(census, 186) == 2, "republishing a career did not record its new profession");
    Require(CensusCareers(census, 182) == 0, "republishing a career kept a profession it no longer plans");

    telemetry.PublishCareerPending(9001);
    census = telemetry.SnapshotProfessionCensus();
    Require(census.careers == 1, "a pending career still counted toward the census");
    Require(census.primarySlots == 1, "a pending career still held its primary slots");
    Require(CensusCareers(census, 164) == 0, "a pending career left a profession behind");

    return EXIT_SUCCESS;
}
