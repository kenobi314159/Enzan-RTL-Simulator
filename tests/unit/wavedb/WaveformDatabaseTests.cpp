// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the live waveform database.
//
// Two properties carry most of the weight here. The first is that a query answers the same way
// whether the data it touches is in memory or has been spilled to the scratch file, because a
// budget that changed the answer would be worse than no budget at all. The second is that
// `sample` partitions a range exactly — every column adjacent to the next, the last ending on
// the requested end — since an off-by-one there shifts a trace on screen and looks like a design
// bug rather than a viewer bug.

#include "wavedb/WaveformDatabase.h"

#include "eir/BitVector.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using enzan::eir::BitVector;
using enzan::test::literal;
using enzan::wavedb::Change;
using enzan::wavedb::DatabaseOptions;
using enzan::wavedb::Sample;
using enzan::wavedb::WaveformDatabase;

/// Appends a change written as a Verilog literal, which keeps the fixtures readable.
void appendLiteral(WaveformDatabase& database, std::uint32_t recordIndex, std::uint64_t time,
                   std::string_view text) {
    database.append(recordIndex, time, literal(text));
}

TEST(WaveformDatabaseTests, AssignsDenseIndicesAndRemembersSignals) {
    WaveformDatabase database;

    const std::uint32_t clock = database.addSignal(7, "/tb/clk", 1);
    const std::uint32_t count = database.addSignal(3, "/tb/count", 8);

    EXPECT_EQ(clock, 0U);
    EXPECT_EQ(count, 1U);
    EXPECT_EQ(database.signalCount(), 2U);

    EXPECT_EQ(database.signal(clock).path, "/tb/clk");
    EXPECT_EQ(database.signal(count).width, 8U);

    EXPECT_EQ(database.recordIndexOf(3), count);
    EXPECT_EQ(database.recordIndexOf(99), WaveformDatabase::NotRecorded);
}

TEST(WaveformDatabaseTests, DeclaringTheSameSignalTwiceIsIdempotent) {
    WaveformDatabase database;

    const std::uint32_t first = database.addSignal(4, "/tb/x", 8);
    const std::uint32_t again = database.addSignal(4, "/tb/x", 8);

    EXPECT_EQ(first, again);
    EXPECT_EQ(database.signalCount(), 1U);
}

TEST(WaveformDatabaseTests, ValueAtReadsTheChangeInEffect) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);

    appendLiteral(database, signal, 10, "8'h01");
    appendLiteral(database, signal, 20, "8'h02");
    appendLiteral(database, signal, 30, "8'h03");

    EXPECT_FALSE(database.valueAt(signal, 9).has_value());

    // The change at a timestamp is in effect from that timestamp on, not from the next one.
    EXPECT_EQ(database.valueAt(signal, 10), std::optional(literal("8'h01")));
    EXPECT_EQ(database.valueAt(signal, 19), std::optional(literal("8'h01")));
    EXPECT_EQ(database.valueAt(signal, 20), std::optional(literal("8'h02")));
    EXPECT_EQ(database.valueAt(signal, 1000), std::optional(literal("8'h03")));
}

TEST(WaveformDatabaseTests, ChangesInReturnsAHalfOpenRange) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 4);

    for (std::uint64_t time = 10; time <= 50; time += 10) {
        appendLiteral(database, signal, time, "4'h5");
    }

    std::vector<Change> changes;
    database.changesIn(signal, 20, 40, changes);

    ASSERT_EQ(changes.size(), 2U);
    EXPECT_EQ(changes[0].time, 20U);
    EXPECT_EQ(changes[1].time, 30U);

    changes.clear();
    database.changesIn(signal, 40, 40, changes);
    EXPECT_TRUE(changes.empty());
}

TEST(WaveformDatabaseTests, LatestTimeTracksBothChangesAndIdleRuns) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 1);

    appendLiteral(database, signal, 40, "1'b1");
    EXPECT_EQ(database.latestTime(), 40U);

    // A run that settles produces no changes but still extends the time axis.
    database.setLatestTime(100);
    EXPECT_EQ(database.latestTime(), 100U);

    // It never moves backwards, so a restart cannot make the axis shrink under the view.
    database.setLatestTime(50);
    EXPECT_EQ(database.latestTime(), 100U);
}

TEST(WaveformDatabaseTests, SampleGivesOneConstantColumnPerChange) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);

    appendLiteral(database, signal, 10, "8'h01");
    appendLiteral(database, signal, 20, "8'h02");
    appendLiteral(database, signal, 30, "8'h03");

    std::vector<Sample> columns(4);
    database.sample(signal, 0, 40, columns);

    EXPECT_EQ(columns[0].startTime, 0U);
    EXPECT_EQ(columns[0].transitions, 0U);
    EXPECT_EQ(columns[0].changeIndex, Sample::NoChange);

    EXPECT_EQ(columns[1].startTime, 10U);
    EXPECT_EQ(columns[1].transitions, 0U);
    EXPECT_EQ(columns[1].changeIndex, 0U);
    EXPECT_EQ(database.valueOfChange(signal, columns[1].changeIndex), literal("8'h01"));

    EXPECT_EQ(columns[3].startTime, 30U);
    EXPECT_EQ(columns[3].changeIndex, 2U);
}

TEST(WaveformDatabaseTests, SampleCountsTransitionsInsideABusyColumn) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/clk", 1);

    for (std::uint64_t time = 5; time < 1000; time += 5) {
        appendLiteral(database, signal, time, (time / 5) % 2 == 0 ? "1'b0" : "1'b1");
    }

    std::vector<Sample> columns(2);
    database.sample(signal, 0, 1000, columns);

    // 5, 10 ... 495 is ninety-nine changes strictly inside the first column, and the change at
    // 500 opens the second one rather than closing the first.
    EXPECT_EQ(columns[0].transitions, 99U);
    EXPECT_EQ(columns[1].transitions, 99U);
    EXPECT_FALSE(columns[0].anyUnknown);
}

TEST(WaveformDatabaseTests, SampleColumnsTileTheRangeExactly) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 1);
    appendLiteral(database, signal, 50, "1'b1");

    // A range that does not divide evenly by the column count is the case the rounding has to
    // get right: the columns must stay adjacent and must still finish at the requested end.
    std::vector<Sample> columns(7);
    database.sample(signal, 3, 100, columns);

    EXPECT_EQ(columns.front().startTime, 3U);
    for (std::size_t column = 1; column < columns.size(); ++column) {
        EXPECT_GT(columns[column].startTime, columns[column - 1].startTime);
    }
    EXPECT_LT(columns.back().startTime, 100U);

    // Every change is accounted for exactly once across the columns.
    std::uint64_t total = 0;
    for (const Sample& column : columns) total += column.transitions;
    EXPECT_EQ(total, 1U);
}

TEST(WaveformDatabaseTests, SampleReportsUnknownValues) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 4);

    appendLiteral(database, signal, 10, "4'b10x1");
    appendLiteral(database, signal, 20, "4'h5");

    std::vector<Sample> columns(4);
    database.sample(signal, 0, 40, columns);

    EXPECT_FALSE(columns[0].anyUnknown);
    EXPECT_TRUE(columns[1].anyUnknown);

    // The known value that arrives at 20 is what the third column opens with, so neither it
    // nor anything after it is unknown.
    EXPECT_FALSE(columns[2].anyUnknown);
    EXPECT_FALSE(columns[3].anyUnknown);
}

TEST(WaveformDatabaseTests, SampleSeesAnUnknownValueCarriedIntoAColumn) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 4);

    appendLiteral(database, signal, 5, "4'bxxxx");

    std::vector<Sample> columns(4);
    database.sample(signal, 0, 40, columns);

    EXPECT_TRUE(columns[0].anyUnknown);
    EXPECT_TRUE(columns[1].anyUnknown);
    EXPECT_TRUE(columns[3].anyUnknown);
}

TEST(WaveformDatabaseTests, SpilledValuesReadBackUnchanged) {
    // A budget far below what the trace needs, so that almost everything ends up on disk and
    // every read below has to bring a chunk back.
    DatabaseOptions options;
    options.chunkCapacity = 16;
    options.memoryBudgetBytes = 512;

    WaveformDatabase database(options);
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 16);

    constexpr std::uint64_t Changes = 2000;
    for (std::uint64_t index = 0; index < Changes; ++index) {
        database.append(signal, (index + 1) * 10, BitVector::fromUInt64(index, 16));
    }

    EXPECT_GT(database.spilledValueBytes(), 0U)
        << "the budget was too small for this trace to have stayed in memory";

    // Read in an order that defeats any sequential prefetch the chunk cache might have.
    for (std::uint64_t index = Changes; index-- > 0;) {
        EXPECT_EQ(database.valueOfChange(signal, index), BitVector::fromUInt64(index, 16))
            << "change " << index << " did not survive the round trip through the scratch file";
    }

    EXPECT_EQ(database.valueAt(signal, 10U * Changes),
              std::optional(BitVector::fromUInt64(Changes - 1, 16)));
}

TEST(WaveformDatabaseTests, SamplingNeverTouchesSpilledValues) {
    // The point of keeping the time index resident: summarising a whole trace answers from it
    // alone, so a zoomed-out redraw of a spilled trace does no I/O and frees no cached chunk.
    DatabaseOptions options;
    options.chunkCapacity = 16;
    options.memoryBudgetBytes = 512;

    WaveformDatabase database(options);
    const std::uint32_t signal = database.addSignal(0, "/tb/clk", 1);

    for (std::uint64_t index = 0; index < 4000; ++index) {
        appendLiteral(database, signal, (index + 1) * 5, index % 2 == 0 ? "1'b1" : "1'b0");
    }

    const std::uint64_t residentBefore = database.residentValueBytes();

    // Five hundred columns over [1, 20001) puts every column edge one unit past a multiple of
    // forty, so no change lands exactly on one and every change is counted as a transition.
    std::vector<Sample> columns(500);
    database.sample(signal, 1, 20'001, columns);

    EXPECT_EQ(database.residentValueBytes(), residentBefore);

    std::uint64_t total = 0;
    for (const Sample& column : columns) total += column.transitions;
    EXPECT_EQ(total, 4000U);
}

TEST(WaveformDatabaseTests, TheBudgetBoundsTheResidentPayload) {
    DatabaseOptions options;
    options.chunkCapacity = 8;
    options.memoryBudgetBytes = 4096;

    WaveformDatabase database(options);
    const std::uint32_t signal = database.addSignal(0, "/tb/wide", 256);

    for (std::uint64_t index = 0; index < 500; ++index) {
        database.append(signal, index + 1, BitVector::fromUInt64(index, 256));
    }

    // The chunk still being filled is never spilled, so the ceiling is the budget plus one
    // chunk rather than the budget exactly.
    const std::uint64_t chunkBytes =
        8ULL * enzan::eir::storageWordsFor(256) * sizeof(enzan::eir::Word);
    EXPECT_LE(database.residentValueBytes(), options.memoryBudgetBytes + chunkBytes);
    EXPECT_GT(database.spilledValueBytes(), 0U);
}

TEST(WaveformDatabaseTests, AnUnwritableSpillDirectoryKeepsTheDataInMemory) {
    // Losing a waveform silently is worse than exceeding a budget, so a scratch file that
    // cannot be created must leave the database working rather than dropping values.
    DatabaseOptions options;
    options.chunkCapacity = 4;
    options.memoryBudgetBytes = 64;
    options.spillDirectory = "/proc/enzan-does-not-exist";

    WaveformDatabase database(options);
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 8);

    for (std::uint64_t index = 0; index < 100; ++index) {
        database.append(signal, index + 1, BitVector::fromUInt64(index, 8));
    }

    EXPECT_EQ(database.spilledValueBytes(), 0U);
    for (std::uint64_t index = 0; index < 100; ++index) {
        EXPECT_EQ(database.valueOfChange(signal, index), BitVector::fromUInt64(index, 8));
    }
}

TEST(WaveformDatabaseTests, IndexBytesGrowWithTheTrace) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 1);

    const std::uint64_t empty = database.indexBytes();
    for (std::uint64_t index = 0; index < 100; ++index) {
        appendLiteral(database, signal, index + 1, "1'b1");
    }

    EXPECT_GT(database.indexBytes(), empty);
}

TEST(WaveformDatabaseTests, ChangeCountAndTimeOfChangeAgreeWithWhatWasAppended) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 1);

    EXPECT_EQ(database.changeCount(signal), 0U);

    appendLiteral(database, signal, 7, "1'b1");
    appendLiteral(database, signal, 11, "1'b0");

    EXPECT_EQ(database.changeCount(signal), 2U);
    EXPECT_EQ(database.timeOfChange(signal, 0), 7U);
    EXPECT_EQ(database.timeOfChange(signal, 1), 11U);
}

} // namespace
