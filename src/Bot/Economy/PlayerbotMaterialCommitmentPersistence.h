/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTPERSISTENCE_H
#define PLAYERBOTS_PLAYERBOTMATERIALCOMMITMENTPERSISTENCE_H

#include <cstdint>
#include <functional>

#include "AsyncCallbackProcessor.h"
#include "Bot/Economy/PlayerbotMaterialCommitmentAuthority.h"
#include "DatabaseEnv.h"

namespace PlayerbotEconomy
{
class PlayerbotMaterialCommitmentPersistence
{
public:
    using Completion = std::function<void(std::uint64_t, bool)>;

    explicit PlayerbotMaterialCommitmentPersistence(DatabaseWorkerPool<PlayerbotsDatabaseConnection>& database);

    void QueueWrite(std::uint64_t token, MaterialCommitmentWrite const& write, Completion completion);
    [[nodiscard]] MaterialCommitmentStartup Load();
    void ProcessCallbacks();

private:
    DatabaseWorkerPool<PlayerbotsDatabaseConnection>& database;
    AsyncCallbackProcessor<TransactionCallback> transactionProcessor;
};
}  // namespace PlayerbotEconomy

#endif
