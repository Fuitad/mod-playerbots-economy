/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Bot/Economy/PlayerbotEconomyTrace.h"

#include <algorithm>

#include "Bot/Personality/PlayerbotPersonality.h"
#include "Random.h"
#include "StringFormat.h"

using namespace PlayerbotEconomy;

namespace
{
constexpr uint64 TRACE_ID_NAMESPACE = 0x74b6d3a10f29c85eULL;

std::string TracePublicId(uint64 sequence, uint64 occurredAt)
{
    uint64 const entropy = (static_cast<uint64>(rand32()) << 32u) | rand32();
    uint64 const opaque = PlayerbotPersonality::SplitMix64(entropy ^ sequence ^ occurredAt ^ TRACE_ID_NAMESPACE);
    return Acore::StringFormat("evt_{:016x}", opaque);
}

bool IsValid(EconomyTraceRecord const& record)
{
    if (record.deduplicationKey.empty() || record.chainPublicId.empty() || !record.actorGuid || !record.itemId ||
        !record.quantity || !record.occurredAt)
    {
        return false;
    }
    return record.kind == EconomyTraceKind::FinalUse ? record.finalUse != EconomyFinalUseKind::None
                                                     : record.finalUse == EconomyFinalUseKind::None;
}
}  // namespace

bool PlayerbotEconomyTrace::Record(EconomyTraceRecord record)
{
    if (!IsValid(record))
        return false;

    std::scoped_lock lock(mutex);
    if (std::any_of(deduplicationKeys.begin(), deduplicationKeys.end(),
                    [&record](auto const& entry) { return entry.first == record.deduplicationKey; }))
    {
        return false;
    }

    EconomyTraceEvent event;
    event.sequence = nextSequence++;
    do
    {
        event.publicId = TracePublicId(event.sequence, record.occurredAt);
    } while (std::any_of(events.begin(), events.end(),
                         [&event](EconomyTraceEvent const& existing) { return existing.publicId == event.publicId; }));
    event.chainPublicId = std::move(record.chainPublicId);
    event.actorGuid = record.actorGuid;
    event.counterpartyGuid = record.counterpartyGuid;
    event.itemId = record.itemId;
    event.recipeSpellId = record.recipeSpellId;
    event.quantity = record.quantity;
    event.unitPriceCopper = record.unitPriceCopper;
    event.depositCopper = record.depositCopper;
    event.auctionCutCopper = record.auctionCutCopper;
    event.proceedsCopper = record.proceedsCopper;
    event.referenceUnitPriceCopper = record.referenceUnitPriceCopper;
    event.competingUnitPriceCopper = record.competingUnitPriceCopper;
    event.occurredAt = record.occurredAt;
    event.correlationAuctionId = record.correlationAuctionId;
    event.correlationMailId = record.correlationMailId;
    event.kind = record.kind;
    event.finalUse = record.finalUse;
    deduplicationKeys.emplace_back(std::move(record.deduplicationKey), event.sequence);
    events.push_back(std::move(event));
    ++totalCount;
    ++generation;

    auto const removeEvent = [this](auto event)
    {
        uint64 const sequence = event->sequence;
        events.erase(event);
        std::erase_if(deduplicationKeys, [sequence](auto const& entry) { return entry.second == sequence; });
    };

    std::string const chainPublicId = events.back().chainPublicId;
    std::size_t const chainCount =
        std::count_if(events.begin(), events.end(), [&chainPublicId](EconomyTraceEvent const& candidate)
                      { return candidate.chainPublicId == chainPublicId; });
    if (chainCount > PLAYERBOT_ECONOMY_TRACE_CHAIN_CAPACITY)
    {
        auto const oldest =
            std::find_if(events.begin(), events.end(), [&chainPublicId](EconomyTraceEvent const& candidate)
                         { return candidate.chainPublicId == chainPublicId; });
        removeEvent(oldest);
    }
    while (events.size() > PLAYERBOT_ECONOMY_TRACE_GLOBAL_CAPACITY)
        removeEvent(events.begin());
    return true;
}

EconomyTraceSnapshot PlayerbotEconomyTrace::Snapshot() const
{
    std::scoped_lock lock(mutex);
    return {
        .generation = generation,
        .totalCount = totalCount,
        .truncatedCount = totalCount - events.size(),
        .events = events,
    };
}

bool PlayerbotEconomyTraceRuntime::Complete(bool coreOperationSucceeded, EconomyTraceRecord record)
{
    return coreOperationSucceeded && trace.Record(std::move(record));
}

std::size_t PlayerbotEconomyTraceRuntime::CompleteMailScan(bool coreOperationSucceeded,
                                                           std::vector<EconomyTraceRecord> records)
{
    if (!coreOperationSucceeded)
        return 0u;

    std::size_t completed = 0u;
    for (EconomyTraceRecord& record : records)
        completed += trace.Record(std::move(record)) ? 1u : 0u;
    return completed;
}

PlayerbotEconomyTrace& PlayerbotEconomy::GetPlayerbotEconomyTrace()
{
    static PlayerbotEconomyTrace trace;
    return trace;
}
