/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <algorithm>
#include <set>
#include <string>

#include "Bot/Economy/PlayerbotEconomyTrace.h"
#include "gtest/gtest.h"

using namespace PlayerbotEconomy;

namespace
{
EconomyTraceRecord Record(uint32 index, std::string chainPublicId)
{
    return {
        .deduplicationKey = "craft:" + std::to_string(index),
        .chainPublicId = std::move(chainPublicId),
        .actorGuid = 42u,
        .itemId = 1000u + index,
        .recipeSpellId = 2000u + index,
        .quantity = 1u,
        .occurredAt = 10'000u + index,
        .kind = EconomyTraceKind::Crafted,
    };
}

EconomyTraceRecord BoundaryRecord(std::string key, EconomyTraceKind kind,
                                  EconomyFinalUseKind finalUse = EconomyFinalUseKind::None)
{
    EconomyTraceRecord record = Record(1u, "chn_0123456789abcdef");
    record.deduplicationKey = std::move(key);
    record.kind = kind;
    record.finalUse = finalUse;
    return record;
}

bool IsOpaqueEventId(std::string const& value)
{
    return value.size() == 20u && value.starts_with("evt_") &&
           std::all_of(value.begin() + 4, value.end(), [](char character)
                       { return character >= '0' && character <= '9' || character >= 'a' && character <= 'f'; });
}
}  // namespace

TEST(PlayerbotEconomyTraceTest, RetainsNewestEventsWithinGlobalAndPerChainBounds)
{
    PlayerbotEconomyTrace trace;
    for (uint32 index = 0; index < 65u; ++index)
        ASSERT_TRUE(trace.Record(Record(index, "chn_0123456789abcdef")));

    EconomyTraceSnapshot const perChain = trace.Snapshot();
    ASSERT_EQ(perChain.events.size(), PLAYERBOT_ECONOMY_TRACE_CHAIN_CAPACITY);
    EXPECT_EQ(perChain.totalCount, 65u);
    EXPECT_EQ(perChain.truncatedCount, 1u);
    EXPECT_EQ(perChain.events.front().sequence, 2u);
    EXPECT_EQ(perChain.events.back().sequence, 65u);

    for (uint32 chain = 1u; chain < 9u; ++chain)
    {
        for (uint32 index = 0; index < 64u; ++index)
        {
            EconomyTraceRecord record =
                Record(1'000u + chain * 100u + index, "chn_" + std::string(15u, '0') + std::to_string(chain));
            ASSERT_TRUE(trace.Record(std::move(record)));
        }
    }

    EconomyTraceSnapshot const global = trace.Snapshot();
    ASSERT_EQ(global.events.size(), PLAYERBOT_ECONOMY_TRACE_GLOBAL_CAPACITY);
    EXPECT_EQ(global.totalCount, 577u);
    EXPECT_EQ(global.truncatedCount, 65u);
    EXPECT_TRUE(std::is_sorted(global.events.begin(), global.events.end(),
                               [](EconomyTraceEvent const& left, EconomyTraceEvent const& right)
                               { return left.sequence < right.sequence; }));
}

TEST(PlayerbotEconomyTraceTest, DeduplicatesSuccessfulBoundariesAndReturnsStableOpaqueCopies)
{
    PlayerbotEconomyTrace trace;
    EconomyTraceRecord record = Record(1u, "chn_0123456789abcdef");
    ASSERT_TRUE(trace.Record(record));
    EXPECT_FALSE(trace.Record(record));

    EconomyTraceRecord invalid = record;
    invalid.deduplicationKey.clear();
    EXPECT_FALSE(trace.Record(invalid));
    invalid = record;
    invalid.chainPublicId.clear();
    EXPECT_FALSE(trace.Record(invalid));
    invalid = record;
    invalid.quantity = 0u;
    EXPECT_FALSE(trace.Record(invalid));

    EconomyTraceSnapshot const first = trace.Snapshot();
    EconomyTraceSnapshot const second = trace.Snapshot();
    ASSERT_EQ(first, second);
    ASSERT_EQ(first.events.size(), 1u);
    EXPECT_EQ(first.generation, 1u);
    EXPECT_EQ(first.totalCount, 1u);
    EXPECT_EQ(first.truncatedCount, 0u);
    EXPECT_TRUE(IsOpaqueEventId(first.events.front().publicId));

    EconomyTraceRecord next = Record(2u, "chn_0123456789abcdef");
    ASSERT_TRUE(trace.Record(next));
    EconomyTraceSnapshot const third = trace.Snapshot();
    ASSERT_EQ(third.events.size(), 2u);
    EXPECT_NE(third.events[0].publicId, third.events[1].publicId);
    EXPECT_EQ(third.events[1].sequence, third.events[0].sequence + 1u);
}

TEST(PlayerbotEconomyTraceRuntimeTest, FailedCoreOperationsEmitNoSuccessEvents)
{
    PlayerbotEconomyTrace trace;
    PlayerbotEconomyTraceRuntime runtime(trace);

    EXPECT_FALSE(runtime.Complete(false, BoundaryRecord("cast", EconomyTraceKind::Crafted)));
    EXPECT_FALSE(runtime.Complete(false, BoundaryRecord("bid", EconomyTraceKind::Purchased)));
    EXPECT_FALSE(runtime.Complete(false, BoundaryRecord("listing", EconomyTraceKind::Listed)));
    EXPECT_FALSE(
        runtime.Complete(false, BoundaryRecord("item-use", EconomyTraceKind::FinalUse, EconomyFinalUseKind::Consumed)));
    EXPECT_EQ(runtime.CompleteMailScan(false,
                                       {
                                           BoundaryRecord("delivery", EconomyTraceKind::Delivered),
                                           BoundaryRecord("settlement", EconomyTraceKind::SaleSettled),
                                       }),
              0u);

    EconomyTraceSnapshot const snapshot = trace.Snapshot();
    EXPECT_EQ(snapshot.generation, 0u);
    EXPECT_EQ(snapshot.totalCount, 0u);
    EXPECT_TRUE(snapshot.events.empty());
}

TEST(PlayerbotEconomyTraceRuntimeTest, SuccessfulBoundariesRetainChainAndFinalUseWithoutDuplicates)
{
    PlayerbotEconomyTrace trace;
    PlayerbotEconomyTraceRuntime runtime(trace);
    std::vector<EconomyFinalUseKind> const finalUses{
        EconomyFinalUseKind::Equipped,  EconomyFinalUseKind::AmmunitionSet, EconomyFinalUseKind::Consumed,
        EconomyFinalUseKind::Applied,   EconomyFinalUseKind::Transformed,   EconomyFinalUseKind::Vendored,
        EconomyFinalUseKind::Recovered, EconomyFinalUseKind::Lost,
    };

    ASSERT_TRUE(runtime.Complete(true, BoundaryRecord("cast", EconomyTraceKind::Crafted)));
    ASSERT_TRUE(runtime.Complete(true, BoundaryRecord("bid", EconomyTraceKind::Purchased)));
    ASSERT_TRUE(runtime.Complete(true, BoundaryRecord("listing", EconomyTraceKind::Listed)));
    EXPECT_EQ(runtime.CompleteMailScan(true,
                                       {
                                           BoundaryRecord("delivery", EconomyTraceKind::Delivered),
                                           BoundaryRecord("settlement", EconomyTraceKind::SaleSettled),
                                       }),
              2u);
    EXPECT_EQ(runtime.CompleteMailScan(true,
                                       {
                                           BoundaryRecord("delivery", EconomyTraceKind::Delivered),
                                           BoundaryRecord("settlement", EconomyTraceKind::SaleSettled),
                                       }),
              0u);

    for (std::size_t index = 0; index < finalUses.size(); ++index)
    {
        ASSERT_TRUE(runtime.Complete(
            true, BoundaryRecord("final-use:" + std::to_string(index), EconomyTraceKind::FinalUse, finalUses[index])));
    }

    EconomyTraceSnapshot const snapshot = trace.Snapshot();
    ASSERT_EQ(snapshot.events.size(), 13u);
    EXPECT_EQ(snapshot.totalCount, snapshot.events.size());
    EXPECT_TRUE(
        std::all_of(snapshot.events.begin(), snapshot.events.end(), [](EconomyTraceEvent const& event)
                    { return event.chainPublicId == "chn_0123456789abcdef" && IsOpaqueEventId(event.publicId); }));
    EXPECT_EQ(std::count_if(snapshot.events.begin(), snapshot.events.end(),
                            [](EconomyTraceEvent const& event) { return event.kind == EconomyTraceKind::FinalUse; }),
              finalUses.size());
}
