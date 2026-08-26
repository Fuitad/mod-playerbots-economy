/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYTRACE_H
#define PLAYERBOTS_PLAYERBOTECONOMYTRACE_H

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Define.h"

namespace PlayerbotEconomy
{
inline constexpr std::size_t PLAYERBOT_ECONOMY_TRACE_GLOBAL_CAPACITY = 512;
inline constexpr std::size_t PLAYERBOT_ECONOMY_TRACE_CHAIN_CAPACITY = 64;

enum class EconomyTraceKind : uint8
{
    Gathered,
    Crafted,
    Listed,
    Purchased,
    Delivered,
    SaleSettled,
    Expired,
    FinalUse
};

// A counterparty is not always another bot. A vendor purchase names a creature spawn, whose GUID
// counter comes from a different namespace than a character GUID and can collide with one. Consumers
// map the GUID to an identity, so they must be told which namespace to look in rather than guess.
enum class EconomyCounterpartyKind : uint8
{
    Bot,
    Creature
};

enum class EconomyFinalUseKind : uint8
{
    None,
    Equipped,
    AmmunitionSet,
    Consumed,
    Applied,
    Transformed,
    Vendored,
    Learned,
    Recovered,
    Lost
};

struct EconomyTraceRecord
{
    std::string deduplicationKey;
    std::string chainPublicId;
    uint32 actorGuid = 0;
    uint32 counterpartyGuid = 0;
    EconomyCounterpartyKind counterpartyKind = EconomyCounterpartyKind::Bot;
    uint32 itemId = 0;
    uint32 recipeSpellId = 0;
    uint32 quantity = 0;
    uint64 unitPriceCopper = 0;
    uint64 depositCopper = 0;
    uint64 auctionCutCopper = 0;
    uint64 proceedsCopper = 0;
    uint64 referenceUnitPriceCopper = 0;
    uint64 competingUnitPriceCopper = 0;
    uint64 occurredAt = 0;
    uint32 correlationAuctionId = 0;
    uint32 correlationMailId = 0;
    EconomyTraceKind kind = EconomyTraceKind::Crafted;
    EconomyFinalUseKind finalUse = EconomyFinalUseKind::None;
};

struct EconomyTraceEvent
{
    std::string publicId;
    std::string chainPublicId;
    uint64 sequence = 0;
    uint32 actorGuid = 0;
    uint32 counterpartyGuid = 0;
    EconomyCounterpartyKind counterpartyKind = EconomyCounterpartyKind::Bot;
    uint32 itemId = 0;
    uint32 recipeSpellId = 0;
    uint32 quantity = 0;
    uint64 unitPriceCopper = 0;
    uint64 depositCopper = 0;
    uint64 auctionCutCopper = 0;
    uint64 proceedsCopper = 0;
    uint64 referenceUnitPriceCopper = 0;
    uint64 competingUnitPriceCopper = 0;
    uint64 occurredAt = 0;
    uint32 correlationAuctionId = 0;
    uint32 correlationMailId = 0;
    EconomyTraceKind kind = EconomyTraceKind::Crafted;
    EconomyFinalUseKind finalUse = EconomyFinalUseKind::None;

    bool operator==(EconomyTraceEvent const&) const = default;
};

struct EconomyTraceSnapshot
{
    uint64 generation = 0;
    uint64 totalCount = 0;
    uint64 truncatedCount = 0;
    std::vector<EconomyTraceEvent> events;

    bool operator==(EconomyTraceSnapshot const&) const = default;
};

class PlayerbotEconomyTrace
{
public:
    [[nodiscard]] bool Record(EconomyTraceRecord record);
    [[nodiscard]] EconomyTraceSnapshot Snapshot() const;

private:
    mutable std::mutex mutex;
    uint64 generation = 0;
    uint64 nextSequence = 1;
    uint64 totalCount = 0;
    std::vector<EconomyTraceEvent> events;
    std::vector<std::pair<std::string, uint64>> deduplicationKeys;
};

class PlayerbotEconomyTraceRuntime
{
public:
    explicit PlayerbotEconomyTraceRuntime(PlayerbotEconomyTrace& trace) : trace(trace) {}

    [[nodiscard]] bool Complete(bool coreOperationSucceeded, EconomyTraceRecord record);
    [[nodiscard]] std::size_t CompleteMailScan(bool coreOperationSucceeded, std::vector<EconomyTraceRecord> records);

private:
    PlayerbotEconomyTrace& trace;
};

PlayerbotEconomyTrace& GetPlayerbotEconomyTrace();
}  // namespace PlayerbotEconomy

#endif
