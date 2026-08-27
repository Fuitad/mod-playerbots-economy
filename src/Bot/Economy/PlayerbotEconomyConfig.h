/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYCONFIG_H
#define PLAYERBOTS_PLAYERBOTECONOMYCONFIG_H

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct PlayerbotEconomySettings
{
    bool lifecycleEnabled = true;
    std::uint32_t classMatchingProfessionChance = 30;
    std::uint32_t careerGatheringSharePercent = 60;
    std::uint32_t careerProfessionFloorPermille = 40;
    std::uint32_t careerFloorBoostPercent = 400;
    std::uint32_t careerShareBoostPercent = 600;
    std::uint32_t professionReserveStacks = 2;
    std::uint32_t productionMaxBatchStacks = 1;
    bool marketMakingEnabled = false;
    std::uint32_t marketMakingPerGroupExposurePercent = 0;
    std::uint32_t marketMakingTotalExposurePercent = 0;
    std::uint32_t marketMakingMinimumEvidence = 0;
    std::uint32_t marketMakingHoldingHorizonSeconds = 0;
    std::uint32_t marketMakingMaximumRelistAttempts = 0;
    std::uint32_t marketMakingCooldownSeconds = 0;
};

namespace PlayerbotEconomyConfigDetail
{
inline bool ReadBool(std::optional<std::string> const& value, bool fallback)
{
    if (!value.has_value())
        return fallback;
    if (*value == "1" || *value == "true" || *value == "yes" || *value == "on")
        return true;
    if (*value == "0" || *value == "false" || *value == "no" || *value == "off")
        return false;
    return fallback;
}

inline std::uint32_t ReadUnsigned(std::optional<std::string> const& value, std::uint32_t fallback)
{
    if (!value.has_value() || value->empty())
        return fallback;

    std::uint32_t parsed = 0;
    auto const result = std::from_chars(value->data(), value->data() + value->size(), parsed);
    return result.ec == std::errc() && result.ptr == value->data() + value->size() ? parsed : fallback;
}
}  // namespace PlayerbotEconomyConfigDetail

template <class Lookup>
PlayerbotEconomySettings LoadPlayerbotEconomySettings(Lookup&& lookup)
{
    PlayerbotEconomySettings settings;
    settings.lifecycleEnabled =
        PlayerbotEconomyConfigDetail::ReadBool(lookup("PlayerbotsEconomy.LifecycleEnabled"), settings.lifecycleEnabled);
    settings.classMatchingProfessionChance = std::min<std::uint32_t>(
        100, PlayerbotEconomyConfigDetail::ReadUnsigned(lookup("PlayerbotsEconomy.ClassMatchingProfessionChance"),
                                                        settings.classMatchingProfessionChance));
    settings.careerGatheringSharePercent = std::min<std::uint32_t>(
        100, PlayerbotEconomyConfigDetail::ReadUnsigned(lookup("PlayerbotsEconomy.Careers.GatheringSharePercent"),
                                                        settings.careerGatheringSharePercent));
    settings.careerProfessionFloorPermille = std::min<std::uint32_t>(
        1000, PlayerbotEconomyConfigDetail::ReadUnsigned(lookup("PlayerbotsEconomy.Careers.ProfessionFloorPermille"),
                                                         settings.careerProfessionFloorPermille));
    settings.careerFloorBoostPercent = std::min<std::uint32_t>(
        10000, PlayerbotEconomyConfigDetail::ReadUnsigned(lookup("PlayerbotsEconomy.Careers.FloorBoostPercent"),
                                                          settings.careerFloorBoostPercent));
    settings.careerShareBoostPercent = std::min<std::uint32_t>(
        10000, PlayerbotEconomyConfigDetail::ReadUnsigned(lookup("PlayerbotsEconomy.Careers.ShareBoostPercent"),
                                                          settings.careerShareBoostPercent));
    settings.professionReserveStacks = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.ProfessionReserveStacks"), settings.professionReserveStacks);
    settings.productionMaxBatchStacks = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.Production.MaxBatchStacks"), settings.productionMaxBatchStacks);
    settings.marketMakingEnabled = PlayerbotEconomyConfigDetail::ReadBool(
        lookup("PlayerbotsEconomy.MarketMakingEnabled"), settings.marketMakingEnabled);
    settings.marketMakingPerGroupExposurePercent = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.MarketMakingPerGroupExposurePercent"), settings.marketMakingPerGroupExposurePercent);
    settings.marketMakingTotalExposurePercent = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.MarketMakingTotalExposurePercent"), settings.marketMakingTotalExposurePercent);
    settings.marketMakingMinimumEvidence = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.MarketMakingMinimumEvidence"), settings.marketMakingMinimumEvidence);
    settings.marketMakingHoldingHorizonSeconds = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.MarketMakingHoldingHorizonSeconds"), settings.marketMakingHoldingHorizonSeconds);
    settings.marketMakingMaximumRelistAttempts = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.MarketMakingMaximumRelistAttempts"), settings.marketMakingMaximumRelistAttempts);
    settings.marketMakingCooldownSeconds = PlayerbotEconomyConfigDetail::ReadUnsigned(
        lookup("PlayerbotsEconomy.MarketMakingCooldownSeconds"), settings.marketMakingCooldownSeconds);
    return settings;
}

extern PlayerbotEconomySettings sPlayerbotEconomyConfig;
void ReloadPlayerbotEconomyConfig();

#endif
