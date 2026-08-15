/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTENCODING_H
#define PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTENCODING_H

#include <cstddef>
#include <string>

#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"

namespace PlayerbotEconomy::MaterialCommitmentEncoding
{
constexpr std::size_t MAX_IDENTITY_BYTES = 191u;
constexpr std::size_t MAX_FINGERPRINT_BYTES = 16'777'215u;

[[nodiscard]] std::string Fingerprint(MaterialCommitmentCommand const& command);
[[nodiscard]] std::string CommitmentIdentity(std::string const& operationIdentity, std::size_t ordinal);
}  // namespace PlayerbotEconomy::MaterialCommitmentEncoding

#endif
