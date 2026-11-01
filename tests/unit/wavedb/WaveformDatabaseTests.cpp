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
    EXPECT_EQ(columns[0].changeIndex, enzan::wavedb::NoChange);

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

// ---------------------------------------------------------------------------
// Sampling one bit range of a signal
// ---------------------------------------------------------------------------
//
// A row of the wave window showing one bit of a bus asks about that bit. Summarising the whole
// signal for it reported a transition in every column in which *any* bit moved, so a constant
// bit of a busy bus was drawn as a solid busy bar across the width of the window and only became
// the flat line it always was once the zoom put each column inside a single value.

/// A four-bit counter that steps every five time units, so bit 0 moves every step, bit 3 every
/// eight, and bit 2 is constant across the first half of the range.
void appendCountingBus(WaveformDatabase& database, std::uint32_t signal) {
    for (std::uint64_t step = 0; step < 16; ++step) {
        appendLiteral(database, signal, 5 + (step * 5),
                      "4'd" + std::to_string(static_cast<unsigned>(step)));
    }
}

TEST(WaveformDatabaseTests, SamplingOneBitCountsThatBitsTransitions) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 4);
    appendCountingBus(database, signal);

    // One column over the whole run. Sixteen changes fall inside it, and the first of them is the
    // record's own: it has no predecessor to differ from, so it counts for whatever bits it
    // covers — which is what the whole-signal pass does too, and the two must agree.
    std::vector<Sample> whole(1);
    database.sample(signal, 0, 100, whole);
    EXPECT_EQ(whole[0].transitions, 16U) << "the bus itself";

    // Bit 3 moves once in a count of sixteen, when it reaches eight; plus the value appearing.
    std::vector<Sample> bit3(1);
    database.sample(signal, 0, 100, 3, 3, bit3);
    EXPECT_EQ(bit3[0].transitions, 2U);

    std::vector<Sample> bit0(1);
    database.sample(signal, 0, 100, 0, 0, bit0);
    EXPECT_EQ(bit0[0].transitions, 16U) << "the bit that moves on every count";
}

TEST(WaveformDatabaseTests, ABitThatNeverChangesSummarisesToNoTransitions) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 4);

    // The low two bits count and the high two never move, which is the shape that made this worth
    // fixing: the bus is busy in every column and half of it is standing still. The first value
    // lands before the sampled range, so that what is measured is the standing still rather than
    // the value appearing.
    appendLiteral(database, signal, 1, "4'd0");
    for (std::uint64_t step = 0; step < 40; ++step) {
        appendLiteral(database, signal, 5 + (step * 5),
                      "4'd" + std::to_string(static_cast<unsigned>(step % 4)));
    }

    std::vector<Sample> columns(4);
    database.sample(signal, 5, 205, 3, 2, columns);

    for (const Sample& column : columns) {
        EXPECT_EQ(column.transitions, 0U) << "a constant bit range is drawn as a level";
        EXPECT_NE(column.changeIndex, enzan::wavedb::NoChange);
    }

    // And the bus itself is busy across the same columns, which is the contrast the row was
    // being drawn with before.
    std::vector<Sample> busy(4);
    database.sample(signal, 5, 205, busy);
    for (const Sample& column : busy) EXPECT_GT(column.transitions, 0U);
}

TEST(WaveformDatabaseTests, SamplingTheWholeWidthAgreesWithTheCheaperPass) {
    // Asking for every bit must give exactly what asking for the signal gives, or the two paths
    // would disagree about a row depending on whether the reader had sliced it.
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 4);
    appendCountingBus(database, signal);

    std::vector<Sample> whole(8);
    std::vector<Sample> ranged(8);
    database.sample(signal, 0, 100, whole);
    database.sample(signal, 0, 100, 3, 0, ranged);

    for (std::size_t column = 0; column < whole.size(); ++column) {
        EXPECT_EQ(ranged[column].transitions, whole[column].transitions) << column;
        EXPECT_EQ(ranged[column].changeIndex, whole[column].changeIndex) << column;
        EXPECT_EQ(ranged[column].startTime, whole[column].startTime) << column;
        EXPECT_EQ(ranged[column].anyUnknown, whole[column].anyUnknown) << column;
    }
}

TEST(WaveformDatabaseTests, OnlyTheBitsAskedAboutDecideWhetherAColumnIsUnknown) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/bus", 4);
    appendLiteral(database, signal, 10, "4'b001x");

    std::vector<Sample> columns(1);

    // The bus has an X, so the bus is unknown.
    database.sample(signal, 0, 100, columns);
    EXPECT_TRUE(columns[0].anyUnknown);

    // Bit 0 is the X. The bits above it are not, and a row showing one of them must not be
    // painted as unknown because a neighbour is.
    database.sample(signal, 0, 100, 0, 0, columns);
    EXPECT_TRUE(columns[0].anyUnknown);

    database.sample(signal, 0, 100, 3, 1, columns);
    EXPECT_FALSE(columns[0].anyUnknown);
}

TEST(WaveformDatabaseTests, ABitRangeBeforeTheFirstChangeShowsNothing) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/bus", 4);
    appendLiteral(database, signal, 500, "4'h5");

    std::vector<Sample> columns(2);
    database.sample(signal, 0, 1000, 2, 2, columns);

    // Nothing is in effect in the first column, exactly as for the whole signal: there is no
    // value yet, rather than a value of zero.
    EXPECT_EQ(columns[0].changeIndex, enzan::wavedb::NoChange);
    EXPECT_EQ(columns[0].transitions, 0U);
    EXPECT_FALSE(columns[0].anyUnknown);
}

TEST(WaveformDatabaseTests, TheFirstChangeOfARecordIsATransitionOfTheBitsItCovers) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/bus", 4);

    // Both changes land inside the single column, and the first of them has no predecessor to
    // differ from: the value appearing is a transition of everything it covers.
    appendLiteral(database, signal, 10, "4'h4");
    appendLiteral(database, signal, 20, "4'h0");

    std::vector<Sample> columns(1);
    database.sample(signal, 0, 100, 2, 2, columns);
    EXPECT_EQ(columns[0].transitions, 2U);
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

TEST(WaveformDatabaseTests, EdgeNavigationFindsTheNeighbouringChanges) {
    // What a cursor snaps to and what Tab steps along. Both happen per click and per
    // keystroke, so both are one query rather than a scan.
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/clk", 1);

    for (std::uint64_t time = 10; time <= 50; time += 10) {
        appendLiteral(database, signal, time, "1'b1");
    }

    EXPECT_EQ(database.nextChangeAfter(signal, 0), 0U);
    EXPECT_EQ(database.nextChangeAfter(signal, 10), 1U) << "strictly after, so 10 is behind us";
    EXPECT_EQ(database.nextChangeAfter(signal, 45), 4U);
    EXPECT_EQ(database.nextChangeAfter(signal, 50), enzan::wavedb::NoChange)
        << "stepping past the last edge must stop rather than wrap";

    EXPECT_EQ(database.previousChangeBefore(signal, 50), 3U);
    EXPECT_EQ(database.previousChangeBefore(signal, 11), 0U);
    EXPECT_EQ(database.previousChangeBefore(signal, 10), enzan::wavedb::NoChange)
        << "strictly before, so 10 is ahead of us";
    EXPECT_EQ(database.previousChangeBefore(signal, 0), enzan::wavedb::NoChange);
}

TEST(WaveformDatabaseTests, TheNearestChangeIsFoundFromEitherSide) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/clk", 1);

    for (std::uint64_t time = 10; time <= 50; time += 10) {
        appendLiteral(database, signal, time, "1'b1");
    }

    EXPECT_EQ(database.nearestChange(signal, 21), 1U);
    EXPECT_EQ(database.nearestChange(signal, 29), 2U);
    EXPECT_EQ(database.nearestChange(signal, 30), 2U) << "a change exactly at the time is it";
    EXPECT_EQ(database.nearestChange(signal, 0), 0U);
    EXPECT_EQ(database.nearestChange(signal, 1'000), 4U);

    // A tie goes to the earlier change, so that which side of an edge a click rounded to does
    // not decide the answer.
    EXPECT_EQ(database.nearestChange(signal, 25), 1U);
}

TEST(WaveformDatabaseTests, EdgeNavigationOnATraceWithNothingInItSaysSo) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 1);

    EXPECT_EQ(database.nearestChange(signal, 10), enzan::wavedb::NoChange);
    EXPECT_EQ(database.nextChangeAfter(signal, 10), enzan::wavedb::NoChange);
    EXPECT_EQ(database.previousChangeBefore(signal, 10), enzan::wavedb::NoChange);
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

// ---------------------------------------------------------------------------
// Finding where bits start to hold a value
// ---------------------------------------------------------------------------

using enzan::wavedb::NoChange;

TEST(WaveformDatabaseTests, AValueIsFoundWhereTheBitsStartToHoldIt) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);

    appendLiteral(database, signal, 10, "8'h01");
    appendLiteral(database, signal, 20, "8'h05");
    appendLiteral(database, signal, 30, "8'h05"); // written again, held all along
    appendLiteral(database, signal, 40, "8'h07");
    appendLiteral(database, signal, 50, "8'h05");

    const BitVector five = literal("8'h05");
    EXPECT_EQ(database.firstStartIn(signal, 0, 5, 7, 0, five), 1U);
    EXPECT_EQ(database.firstStartIn(signal, 2, 5, 7, 0, five), 4U)
        << "the value already held before the range is not found again inside it";
    EXPECT_EQ(database.lastStartIn(signal, 0, 5, 7, 0, five), 4U);
    EXPECT_EQ(database.lastStartIn(signal, 0, 4, 7, 0, five), 1U)
        << "a change writing the value again is no start of it";

    // The first change is the value appearing, so it starts whatever it holds.
    EXPECT_EQ(database.firstStartIn(signal, 0, 5, 7, 0, literal("8'h01")), 0U);
    EXPECT_EQ(database.lastStartIn(signal, 0, 5, 7, 0, literal("8'h01")), 0U);

    EXPECT_EQ(database.firstStartIn(signal, 0, 5, 7, 0, literal("8'hff")), NoChange);
    EXPECT_EQ(database.lastStartIn(signal, 0, 5, 7, 0, literal("8'hff")), NoChange);
}

TEST(WaveformDatabaseTests, ASearchLooksOnlyInsideItsRange) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);
    appendLiteral(database, signal, 10, "8'h01");
    appendLiteral(database, signal, 20, "8'h05");
    appendLiteral(database, signal, 30, "8'h07");

    const BitVector five = literal("8'h05");
    EXPECT_EQ(database.firstStartIn(signal, 0, 1, 7, 0, five), NoChange);
    EXPECT_EQ(database.lastStartIn(signal, 2, 3, 7, 0, five), NoChange);
    EXPECT_EQ(database.firstStartIn(signal, 1, 1, 7, 0, five), NoChange) << "an empty range";
    EXPECT_EQ(database.lastStartIn(signal, 1, 2, 7, 0, five), 1U);
}

TEST(WaveformDatabaseTests, OnlyTheBitsAskedAboutAreCompared) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);
    appendLiteral(database, signal, 10, "8'h15");
    appendLiteral(database, signal, 20, "8'h25"); // the low nibble stays 5
    appendLiteral(database, signal, 30, "8'h26");
    appendLiteral(database, signal, 40, "8'h35");

    const BitVector five = literal("4'h5");
    EXPECT_EQ(database.firstStartIn(signal, 0, 4, 3, 0, five), 0U);
    EXPECT_EQ(database.firstStartIn(signal, 1, 4, 3, 0, five), 3U);
    EXPECT_EQ(database.lastStartIn(signal, 0, 3, 3, 0, five), 0U);
    EXPECT_EQ(database.firstStartIn(signal, 0, 4, 7, 4, literal("4'h3")), 3U);
}

TEST(WaveformDatabaseTests, BitsAcrossAWordBoundaryAreComparedAsOne) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/wide", 96);
    appendLiteral(database, signal, 10, "96'h0");
    appendLiteral(database, signal, 20, "96'h00000001_80000000_00000000"); // bits 63 and 64
    appendLiteral(database, signal, 30, "96'h00000001_00000000_00000000");

    EXPECT_EQ(database.firstStartIn(signal, 0, 3, 64, 63, literal("2'b11")), 1U);
    EXPECT_EQ(database.firstStartIn(signal, 0, 3, 64, 63, literal("2'b10")), 2U);
    EXPECT_EQ(database.lastStartIn(signal, 0, 3, 64, 63, literal("2'b00")), 0U);
}

TEST(WaveformDatabaseTests, AnUnknownBitMatchesOnlyTheSameUnknown) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 4);
    appendLiteral(database, signal, 10, "4'b10x1");
    appendLiteral(database, signal, 20, "4'b10z1");
    appendLiteral(database, signal, 30, "4'b10x1");

    EXPECT_EQ(database.firstStartIn(signal, 1, 3, 3, 0, literal("4'b10x1")), 2U);
    EXPECT_EQ(database.firstStartIn(signal, 0, 3, 3, 0, literal("4'b10z1")), 1U);
    EXPECT_EQ(database.firstStartIn(signal, 0, 3, 3, 0, literal("4'b1001")), NoChange);
    EXPECT_EQ(database.firstStartIn(signal, 0, 3, 3, 0, literal("4'b1011")), NoChange);
}

TEST(WaveformDatabaseTests, OfTwoChangesAtOneTimeTheLastIsWhatWasHeld) {
    // A value written twice in one time step: the signal held the second at that time, as
    // `valueAt` says, so the first was never held and starts nothing.
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);
    appendLiteral(database, signal, 10, "8'h01");
    appendLiteral(database, signal, 20, "8'h05");
    appendLiteral(database, signal, 20, "8'h06");
    appendLiteral(database, signal, 30, "8'h05");
    ASSERT_EQ(database.valueAt(signal, 20), std::optional(literal("8'h06")));

    EXPECT_EQ(database.firstStartIn(signal, 0, 4, 7, 0, literal("8'h05")), 3U);
    EXPECT_EQ(database.lastStartIn(signal, 0, 4, 7, 0, literal("8'h05")), 3U);
    EXPECT_EQ(database.firstStartIn(signal, 0, 4, 7, 0, literal("8'h06")), 2U);
    EXPECT_EQ(database.lastStartIn(signal, 0, 3, 7, 0, literal("8'h06")), 2U);

    // From the middle of the time, what was held an instant before is still the time before.
    EXPECT_EQ(database.firstStartIn(signal, 2, 4, 7, 0, literal("8'h06")), 2U);
    EXPECT_EQ(database.lastStartIn(signal, 3, 4, 7, 0, literal("8'h05")), 3U);
}

TEST(WaveformDatabaseTests, SpilledValuesAreSearchedAsThoughTheyWereResident) {
    // A small budget spills almost every chunk, so the searches read most values back from the
    // scratch file, in both directions and across chunk boundaries, and must answer what a walk
    // over the values one by one does.
    DatabaseOptions options;
    options.chunkCapacity = 4;
    options.memoryBudgetBytes = 64;
    WaveformDatabase database(options);
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);

    constexpr std::uint64_t Changes = 100;
    for (std::uint64_t index = 0; index < Changes; ++index) {
        database.append(signal, (index + 1) * 10, BitVector::fromUInt64(index % 37, 8));
    }
    ASSERT_GT(database.spilledValueBytes(), 0U);

    const auto walk = [&](std::uint64_t from, std::uint64_t to, const BitVector& value,
                          bool forward) {
        std::uint64_t found = NoChange;
        for (std::uint64_t change = from; change < to; ++change) {
            const bool holds = database.valueOfChange(signal, change) == value;
            const bool held = change > 0 && database.valueOfChange(signal, change - 1) == value;
            if (holds && !held) {
                found = change;
                if (forward) break;
            }
        }
        return found;
    };

    for (std::uint64_t number = 0; number < 40; ++number) {
        const BitVector value = BitVector::fromUInt64(number, 8);
        for (const std::uint64_t from : {0U, 3U, 37U, 61U}) {
            EXPECT_EQ(database.firstStartIn(signal, from, Changes, 7, 0, value),
                      walk(from, Changes, value, true))
                << number << " from " << from;
            EXPECT_EQ(database.lastStartIn(signal, 0, Changes - from, 7, 0, value),
                      walk(0, Changes - from, value, false))
                << number << " up to " << Changes - from;
        }
    }
}

} // namespace
