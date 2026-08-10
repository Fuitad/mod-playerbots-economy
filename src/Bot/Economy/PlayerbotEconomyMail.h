/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYMAIL_H
#define PLAYERBOTS_PLAYERBOTECONOMYMAIL_H

#include <cstddef>
#include <cstdint>

[[nodiscard]] constexpr bool PlayerbotEconomyMailIsFullyCollected(std::uint32_t money, std::size_t attachmentCount)
{
    return money == 0 && attachmentCount == 0;
}

#endif
