/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYMAIL_H
#define PLAYERBOTS_PLAYERBOTECONOMYMAIL_H

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

[[nodiscard]] inline std::optional<std::vector<std::uint64_t>> PlayerbotEconomyParseAuctionMailFields(
    std::string_view text, bool firstFieldHex)
{
    std::vector<std::uint64_t> fields;
    while (!text.empty())
    {
        std::size_t const separator = text.find(':');
        std::string_view field = text.substr(0, separator);
        // AuctionEntry::BuildAuctionMailBody right-aligns its 16-character hexadecimal GUID with spaces.
        while (!field.empty() && field.front() == ' ')
            field.remove_prefix(1);
        while (!field.empty() && field.back() == ' ')
            field.remove_suffix(1);
        if (field.empty())
            return std::nullopt;

        std::uint64_t value = 0;
        int const base = firstFieldHex && fields.empty() ? 16 : 10;
        auto const parsed = std::from_chars(field.data(), field.data() + field.size(), value, base);
        if (parsed.ec != std::errc() || parsed.ptr != field.data() + field.size())
            return std::nullopt;
        fields.push_back(value);
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1);
    }
    return fields;
}

[[nodiscard]] constexpr bool PlayerbotEconomyMailIsFullyCollected(std::uint32_t money, std::size_t attachmentCount)
{
    return money == 0 && attachmentCount == 0;
}

[[nodiscard]] constexpr bool PlayerbotEconomyMailCollectionMadeProgress(std::uint32_t moneyBefore,
                                                                        std::size_t attachmentsBefore,
                                                                        std::uint32_t moneyAfter,
                                                                        std::size_t attachmentsAfter)
{
    return moneyAfter < moneyBefore || attachmentsAfter < attachmentsBefore;
}

struct PlayerbotEconomyMailCollectionResult
{
    bool madeProgress = false;
    bool fullyCollected = false;
};

[[nodiscard]] constexpr PlayerbotEconomyMailCollectionResult PlayerbotEconomyMailCollectionOutcome(
    bool mailPresent, std::uint32_t moneyBefore, std::size_t attachmentsBefore, std::uint32_t moneyAfter,
    std::size_t attachmentsAfter)
{
    if (!mailPresent)
        return {};

    bool const progress =
        PlayerbotEconomyMailCollectionMadeProgress(moneyBefore, attachmentsBefore, moneyAfter, attachmentsAfter);
    return {progress, progress && PlayerbotEconomyMailIsFullyCollected(moneyAfter, attachmentsAfter)};
}

#endif
