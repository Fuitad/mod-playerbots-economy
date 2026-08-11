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
        {"PlayerbotsEconomy.ProfessionReserveStacks", "3"},
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
    Require(settings.professionReserveStacks == 3, "profession reserve stacks were not loaded");
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
    return EXIT_SUCCESS;
}
