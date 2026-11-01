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

#include <algorithm>
#include <array>
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
// Finding when bits start to hold a value
// ---------------------------------------------------------------------------

using enzan::eir::Logic;
using enzan::eir::Radix;
using enzan::runtime::SimTime;
using enzan::wavedb::BitPattern;
using enzan::wavedb::DigitTerm;
using enzan::wavedb::SearchResult;
using enzan::wavedb::SearchTerm;

/// Far past every time a fixture records, for a search meant to look everywhere.
constexpr SimTime Everywhere = 1'000'000;

/// A term asking bits `[highBit, lowBit]` of `record` to hold the literal `value`.
SearchTerm termFor(const WaveformDatabase& database, std::uint32_t record, std::uint32_t highBit,
                   std::uint32_t lowBit, std::string_view value) {
    SearchTerm term{.recordIndex = record, .pattern = BitPattern(database.signal(record).width)};
    EXPECT_TRUE(term.pattern.require(highBit, lowBit, literal(value)));
    return term;
}

SearchResult foundAt(SimTime time) {
    return {.outcome = SearchResult::Outcome::Found, .time = time};
}

const SearchResult Absent{};

TEST(WaveformDatabaseTests, APatternAsksOnlyTheBitsItWasGiven) {
    BitPattern pattern(8);
    EXPECT_TRUE(pattern.matches(literal("8'h00").ref().aval())) << "a pattern asking nothing";

    ASSERT_TRUE(pattern.require(7, 4, literal("4'h3")));
    ASSERT_TRUE(pattern.require(0, 0, literal("1'b1")));
    EXPECT_TRUE(pattern.matches(literal("8'h31").ref().aval()));
    EXPECT_TRUE(pattern.matches(literal("8'h3f").ref().aval()));
    EXPECT_FALSE(pattern.matches(literal("8'h30").ref().aval()));
    EXPECT_FALSE(pattern.matches(literal("8'h21").ref().aval()));
    EXPECT_FALSE(pattern.matches(literal("8'hx1").ref().aval()));
}

TEST(WaveformDatabaseTests, APatternRefusesABitAskedToHoldTwoThings) {
    BitPattern pattern(8);
    ASSERT_TRUE(pattern.require(7, 0, literal("8'h05")));
    EXPECT_TRUE(pattern.require(3, 0, literal("4'h5"))) << "asked again, the same";
    EXPECT_FALSE(pattern.require(3, 0, literal("4'h6")));
    EXPECT_FALSE(pattern.require(1, 0, literal("2'bx1")));

    // A refusal asks nothing, so what was asked before still decides.
    EXPECT_TRUE(pattern.matches(literal("8'h05").ref().aval()));
}

TEST(WaveformDatabaseTests, AValueIsFoundWhereTheBitsStartToHoldIt) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);

    appendLiteral(database, signal, 10, "8'h01");
    appendLiteral(database, signal, 20, "8'h05");
    appendLiteral(database, signal, 30, "8'h05"); // written again, held all along
    appendLiteral(database, signal, 40, "8'h07");
    appendLiteral(database, signal, 50, "8'h05");

    const std::vector<SearchTerm> five{termFor(database, signal, 7, 0, "8'h05")};
    EXPECT_EQ(database.nextTimeHolding(five, 0, Everywhere, 100), foundAt(20));
    EXPECT_EQ(database.nextTimeHolding(five, 20, Everywhere, 100), foundAt(50))
        << "a value held already is found where it comes back";
    EXPECT_EQ(database.previousTimeHolding(five, 60, Everywhere, 100), foundAt(50));
    EXPECT_EQ(database.previousTimeHolding(five, 50, Everywhere, 100), foundAt(20))
        << "a change writing the value again is no start of it";

    // The first change is the value appearing, so it starts whatever it holds.
    const std::vector<SearchTerm> one{termFor(database, signal, 7, 0, "8'h01")};
    EXPECT_EQ(database.nextTimeHolding(one, 0, Everywhere, 100), foundAt(10));
    EXPECT_EQ(database.previousTimeHolding(one, 60, Everywhere, 100), foundAt(10));

    const std::vector<SearchTerm> never{termFor(database, signal, 7, 0, "8'hff")};
    EXPECT_EQ(database.nextTimeHolding(never, 0, Everywhere, 100), Absent);
    EXPECT_EQ(database.previousTimeHolding(never, 60, Everywhere, 100), Absent);
    EXPECT_EQ(database.nextTimeHolding({}, 0, Everywhere, 100), Absent) << "no terms";
}

TEST(WaveformDatabaseTests, NothingAtOrPastTheLimitIsFound) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);
    appendLiteral(database, signal, 10, "8'h01");
    appendLiteral(database, signal, 20, "8'h05");
    appendLiteral(database, signal, 30, "8'h07");

    const std::vector<SearchTerm> five{termFor(database, signal, 7, 0, "8'h05")};
    EXPECT_EQ(database.nextTimeHolding(five, 0, 20, 100), Absent);
    EXPECT_EQ(database.nextTimeHolding(five, 0, 21, 100), foundAt(20));
    EXPECT_EQ(database.previousTimeHolding(five, 100, 20, 100), Absent);
    EXPECT_EQ(database.previousTimeHolding(five, 25, Everywhere, 100), foundAt(20));
}

TEST(WaveformDatabaseTests, OnlyTheBitsAskedAboutAreCompared) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/count", 8);
    appendLiteral(database, signal, 10, "8'h15");
    appendLiteral(database, signal, 20, "8'h25"); // the low nibble stays 5
    appendLiteral(database, signal, 30, "8'h26");
    appendLiteral(database, signal, 40, "8'h35");

    const std::vector<SearchTerm> low{termFor(database, signal, 3, 0, "4'h5")};
    EXPECT_EQ(database.nextTimeHolding(low, 0, Everywhere, 100), foundAt(10));
    EXPECT_EQ(database.nextTimeHolding(low, 10, Everywhere, 100), foundAt(40));
    EXPECT_EQ(database.previousTimeHolding(low, 40, Everywhere, 100), foundAt(10));

    const std::vector<SearchTerm> high{termFor(database, signal, 7, 4, "4'h3")};
    EXPECT_EQ(database.nextTimeHolding(high, 0, Everywhere, 100), foundAt(40));
}

TEST(WaveformDatabaseTests, BitsAcrossAWordBoundaryAreComparedAsOne) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/wide", 96);
    appendLiteral(database, signal, 10, "96'h0");
    appendLiteral(database, signal, 20, "96'h00000001_80000000_00000000"); // bits 63 and 64
    appendLiteral(database, signal, 30, "96'h00000001_00000000_00000000");

    const auto search = [&](std::string_view value) {
        return database.nextTimeHolding(std::vector{termFor(database, signal, 64, 63, value)}, 0,
                                        Everywhere, 100);
    };
    EXPECT_EQ(search("2'b11"), foundAt(20));
    EXPECT_EQ(search("2'b10"), foundAt(30));
    EXPECT_EQ(search("2'b00"), foundAt(10));
}

TEST(WaveformDatabaseTests, AnUnknownBitMatchesOnlyTheSameUnknown) {
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/x", 4);
    appendLiteral(database, signal, 10, "4'b10x1");
    appendLiteral(database, signal, 20, "4'b10z1");
    appendLiteral(database, signal, 30, "4'b10x1");

    const auto search = [&](SimTime after, std::string_view value) {
        return database.nextTimeHolding(std::vector{termFor(database, signal, 3, 0, value)}, after,
                                        Everywhere, 100);
    };
    EXPECT_EQ(search(10, "4'b10x1"), foundAt(30));
    EXPECT_EQ(search(0, "4'b10z1"), foundAt(20));
    EXPECT_EQ(search(0, "4'b1001"), Absent);
    EXPECT_EQ(search(0, "4'b1011"), Absent);
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

    const std::vector<SearchTerm> five{termFor(database, signal, 7, 0, "8'h05")};
    const std::vector<SearchTerm> six{termFor(database, signal, 7, 0, "8'h06")};
    EXPECT_EQ(database.nextTimeHolding(five, 0, Everywhere, 100), foundAt(30));
    EXPECT_EQ(database.previousTimeHolding(five, 40, Everywhere, 100), foundAt(30));
    EXPECT_EQ(database.nextTimeHolding(six, 0, Everywhere, 100), foundAt(20));
    EXPECT_EQ(database.previousTimeHolding(six, 30, Everywhere, 100), foundAt(20));

    // A search stopping at that time, a change at a time, goes on past the whole of it.
    const SearchResult stopped = database.nextTimeHolding(five, 10, Everywhere, 1);
    EXPECT_EQ(stopped, (SearchResult{.outcome = SearchResult::Outcome::Unfinished, .time = 20}));
    EXPECT_EQ(database.nextTimeHolding(five, stopped.time, Everywhere, 1), foundAt(30));
}

TEST(WaveformDatabaseTests, SeveralSignalsAreFoundWhereTheyStartToHoldTogether) {
    // `a` steps 01, 02, 03 at 10, 20, 30; `b` alternates aa and bb, and changes with `a` at 30.
    WaveformDatabase database;
    const std::uint32_t a = database.addSignal(0, "/tb/a", 8);
    const std::uint32_t b = database.addSignal(1, "/tb/b", 8);
    appendLiteral(database, a, 10, "8'h01");
    appendLiteral(database, a, 20, "8'h02");
    appendLiteral(database, a, 30, "8'h03");
    appendLiteral(database, b, 15, "8'haa");
    appendLiteral(database, b, 25, "8'hbb");
    appendLiteral(database, b, 30, "8'haa");
    appendLiteral(database, b, 40, "8'hbb");

    const auto both = [&](std::string_view first, std::string_view second) {
        return std::vector{termFor(database, a, 7, 0, first), termFor(database, b, 7, 0, second)};
    };

    // Each starts where the second of the two arrives, whichever of them that is.
    EXPECT_EQ(database.nextTimeHolding(both("8'h01", "8'haa"), 0, Everywhere, 100), foundAt(15));
    EXPECT_EQ(database.nextTimeHolding(both("8'h02", "8'haa"), 0, Everywhere, 100), foundAt(20));
    EXPECT_EQ(database.nextTimeHolding(both("8'h03", "8'hbb"), 0, Everywhere, 100), foundAt(40));

    // Two changes at one time are one step: both arrive at 30, together.
    EXPECT_EQ(database.nextTimeHolding(both("8'h03", "8'haa"), 0, Everywhere, 100), foundAt(30));
    EXPECT_EQ(database.previousTimeHolding(both("8'h03", "8'haa"), 100, Everywhere, 100),
              foundAt(30));
    EXPECT_EQ(database.nextTimeHolding(both("8'h02", "8'hbb"), 0, Everywhere, 100), foundAt(25));

    // Held already where the search starts, so not found again; and `b` before its first change
    // holds nothing, so nothing starts before 15.
    EXPECT_EQ(database.nextTimeHolding(both("8'h02", "8'haa"), 21, Everywhere, 100), Absent);
    EXPECT_EQ(database.previousTimeHolding(both("8'h01", "8'haa"), 15, Everywhere, 100), Absent);
    EXPECT_EQ(database.previousTimeHolding(both("8'h01", "8'haa"), 18, Everywhere, 100),
              foundAt(15));
}

/// Where the terms start to hold together, found by asking `valueAt` at every time any of their
/// signals changes: the definition, walked as slowly as it reads.
SearchResult walk(const WaveformDatabase& database, const std::vector<SearchTerm>& terms,
                  const std::vector<BitVector>& values, const std::vector<std::uint32_t>& lows,
                  SimTime from, bool forward) {
    std::vector<SimTime> times;
    for (const SearchTerm& term : terms) {
        for (std::uint64_t change = 0; change < database.changeCount(term.recordIndex); ++change)
            times.push_back(database.timeOfChange(term.recordIndex, change));
    }
    std::ranges::sort(times);
    const auto [duplicates, end] = std::ranges::unique(times);
    times.erase(duplicates, end);

    const auto holdAt = [&](SimTime time) {
        for (std::size_t index = 0; index < terms.size(); ++index) {
            const std::optional<BitVector> value = database.valueAt(terms[index].recordIndex, time);
            if (!value.has_value()) return false;
            for (std::uint32_t bit = 0; bit < values[index].width(); ++bit) {
                if (value->get(lows[index] + bit) != values[index].get(bit)) return false;
            }
        }
        return true;
    };

    SearchResult found;
    for (const SimTime time : times) {
        if (forward ? time <= from : time >= from) continue;
        if (holdAt(time) && (time == 0 || !holdAt(time - 1))) {
            found = foundAt(time);
            if (forward) break;
        }
    }
    return found;
}

TEST(WaveformDatabaseTests, SearchesThroughSpilledChunksAnswerAsTheDefinitionDoes) {
    // A small budget spills almost every chunk, and several signals searched together read their
    // chunks in turn, so each read can evict the chunk another cursor is in the middle of. The
    // searches must still answer what asking `valueAt` at every time does, in both directions
    // and however small the steps they go in.
    DatabaseOptions options;
    options.chunkCapacity = 4;
    options.memoryBudgetBytes = 64;
    WaveformDatabase database(options);
    const std::uint32_t fast = database.addSignal(0, "/tb/fast", 8);
    const std::uint32_t slow = database.addSignal(1, "/tb/slow", 8);
    const std::uint32_t odd = database.addSignal(2, "/tb/odd", 8);

    for (std::uint64_t index = 0; index < 120; ++index) {
        database.append(fast, (index * 10) + 5, BitVector::fromUInt64(index % 7, 8));
        if (index % 3 == 0) database.append(slow, index * 10, BitVector::fromUInt64(index % 5, 8));
        if (index % 2 == 1)
            database.append(odd, (index * 10) + 5, BitVector::fromUInt64((index / 2) % 4, 8));
    }
    ASSERT_GT(database.spilledValueBytes(), 0U);

    const auto settle = [&](const std::vector<SearchTerm>& terms, SimTime from, bool forward,
                            std::uint64_t budget) {
        SearchResult result;
        do {
            result = forward ? database.nextTimeHolding(terms, from, Everywhere, budget)
                             : database.previousTimeHolding(terms, from, Everywhere, budget);
            from = result.time;
        } while (result.outcome == SearchResult::Outcome::Unfinished);
        return result;
    };

    for (std::uint64_t first = 0; first < 7; ++first) {
        for (std::uint64_t second = 0; second < 5; second += 2) {
            for (std::uint64_t third = 0; third < 4; third += 3) {
                const std::vector<BitVector> values{BitVector::fromUInt64(first, 8),
                                                    BitVector::fromUInt64(second, 4),
                                                    BitVector::fromUInt64(third, 8)};
                std::vector<SearchTerm> terms;
                terms.push_back({.recordIndex = fast, .pattern = BitPattern(8)});
                terms.push_back({.recordIndex = slow, .pattern = BitPattern(8)});
                terms.push_back({.recordIndex = odd, .pattern = BitPattern(8)});
                ASSERT_TRUE(terms[0].pattern.require(7, 0, values[0]));
                ASSERT_TRUE(terms[1].pattern.require(3, 0, values[1]));
                ASSERT_TRUE(terms[2].pattern.require(7, 0, values[2]));
                const std::vector<std::uint32_t> lows{0, 0, 0};

                for (const SimTime from : {SimTime{0}, SimTime{333}, SimTime{1205}}) {
                    for (const bool forward : {true, false}) {
                        const SearchResult expected =
                            walk(database, terms, values, lows, from, forward);
                        for (const std::uint64_t budget : {1U, 100U}) {
                            EXPECT_EQ(settle(terms, from, forward, budget), expected)
                                << first << second << third << (forward ? " after " : " before ")
                                << from << ", budget " << budget;
                        }
                        // One term alone, through the walk over a single signal.
                        const std::vector<SearchTerm> alone{terms[0]};
                        EXPECT_EQ(settle(alone, from, forward, 1),
                                  walk(database, alone, {values[0]}, {0}, from, forward));
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Digits shown partly unknown
// ---------------------------------------------------------------------------

/// A digit of `kind` over the bits `[highBit, lowBit]` of the signal of term `term`.
DigitTerm digitOver(enzan::eir::Logic kind, std::uint32_t term, std::uint32_t highBit,
                    std::uint32_t lowBit) {
    DigitTerm digit;
    digit.kind = kind;
    for (std::uint32_t bit = highBit + 1; bit-- > lowBit;) digit.cover(term, bit);
    return digit;
}

TEST(WaveformDatabaseTests, ADigitIsFoundWhereItsBitsStartToBePartlyUnknown) {
    // The high nibble stays 1 while the low one is known, partly unknown, wholly unknown, partly
    // again, partly high impedance, and partly unknown among high impedance: `1X` is shown at 20,
    // 40 and 60, which are where it starts, and `1Z` at 50.
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/nibbles", 8);
    appendLiteral(database, signal, 10, "8'b0001_0101");
    appendLiteral(database, signal, 20, "8'b0001_0x01");
    appendLiteral(database, signal, 25, "8'b0001_x001"); // still partly unknown
    appendLiteral(database, signal, 30, "8'b0001_xxxx");
    appendLiteral(database, signal, 40, "8'b0001_x0x0");
    appendLiteral(database, signal, 50, "8'b0001_0z01");
    appendLiteral(database, signal, 60, "8'b0001_zz0x");

    const std::vector<SearchTerm> high{termFor(database, signal, 7, 4, "4'h1")};
    const std::vector<DigitTerm> unknown{digitOver(Logic::Unknown, 0, 3, 0)};
    const std::vector<DigitTerm> highZ{digitOver(Logic::HighZ, 0, 3, 0)};

    EXPECT_EQ(database.nextTimeHolding(high, 0, Everywhere, 100, unknown), foundAt(20));
    EXPECT_EQ(database.nextTimeHolding(high, 20, Everywhere, 100, unknown), foundAt(40))
        << "25 goes on showing it, and 30 shows a lowercase x";
    EXPECT_EQ(database.nextTimeHolding(high, 40, Everywhere, 100, unknown), foundAt(60))
        << "50 shows a Z";
    EXPECT_EQ(database.previousTimeHolding(high, 100, Everywhere, 100, unknown), foundAt(60));
    EXPECT_EQ(database.previousTimeHolding(high, 60, Everywhere, 100, unknown), foundAt(40));
    EXPECT_EQ(database.previousTimeHolding(high, 40, Everywhere, 100, unknown), foundAt(20));
    EXPECT_EQ(database.previousTimeHolding(high, 20, Everywhere, 100, unknown), Absent);

    EXPECT_EQ(database.nextTimeHolding(high, 0, Everywhere, 100, highZ), foundAt(50));
    EXPECT_EQ(database.previousTimeHolding(high, 100, Everywhere, 100, highZ), foundAt(50));
    EXPECT_EQ(database.nextTimeHolding(high, 50, Everywhere, 100, highZ), Absent)
        << "60 has an x among its z";
}

TEST(WaveformDatabaseTests, ADigitsFixedBitsCountWithItsSignalsBits) {
    // Two bits of a signal and two of a parameter beside it, both unknown: the digit is partly
    // unknown wherever the signal's bits are known, and wholly so where they are not.
    WaveformDatabase database;
    const std::uint32_t signal = database.addSignal(0, "/tb/pair", 2);
    appendLiteral(database, signal, 10, "2'b01");
    appendLiteral(database, signal, 20, "2'bxx");
    appendLiteral(database, signal, 30, "2'b10");

    const std::vector<SearchTerm> terms{
        {.recordIndex = signal, .pattern = BitPattern(database.signal(signal).width)}};
    std::vector<DigitTerm> digits{digitOver(Logic::Unknown, 0, 1, 0)};
    digits.front().coverFixed(Logic::Unknown);
    digits.front().coverFixed(Logic::Unknown);

    EXPECT_EQ(database.nextTimeHolding(terms, 0, Everywhere, 100, digits), foundAt(10));
    EXPECT_EQ(database.nextTimeHolding(terms, 10, Everywhere, 100, digits), foundAt(30));
    EXPECT_EQ(database.previousTimeHolding(terms, 30, Everywhere, 100, digits), foundAt(10));
}

TEST(WaveformDatabaseTests, DigitsAcrossSignalsAnswerAsTheirRenderingDoes) {
    // Values of every kind of bit, spilled and searched in steps of every size, with digits that
    // cover bits of several signals and terms that ask bits of them besides. What the searches
    // must answer is where the digits start to be written in capitals, as the value column
    // writes them, while the terms hold.
    DatabaseOptions options;
    options.chunkCapacity = 4;
    options.memoryBudgetBytes = 64;
    WaveformDatabase database(options);
    const std::array<std::uint32_t, 3> records{database.addSignal(0, "/tb/a", 3),
                                               database.addSignal(1, "/tb/b", 3),
                                               database.addSignal(2, "/tb/c", 3)};

    // Each bit unknown about one change in four and high impedance about one in four.
    std::uint64_t seed = 12345;
    const auto next = [&seed] {
        seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(seed >> 33);
    };
    for (std::uint64_t step = 0; step < 150; ++step) {
        for (std::size_t signal = 0; signal < records.size(); ++signal) {
            if (next() % 3 == 0) continue;
            BitVector value = BitVector::filled(3, Logic::Zero);
            for (std::uint32_t bit = 0; bit < 3; ++bit)
                value.set(bit, static_cast<Logic>(next() % 4));
            database.append(records[signal], (step * 10) + (signal * 3), value);
        }
    }
    ASSERT_GT(database.spilledValueBytes(), 0U);

    // A hexadecimal digit over a[2:1] and b[1:0], and an octal one over c[1:0] and a fixed 0;
    // the terms ask b[2] and c[2], so that a digit is asked only where they hold.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> hexBits{
        {0, 2}, {0, 1}, {1, 1}, {1, 0}};
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> octalBits{{2, 1}, {2, 0}};

    const auto rendered = [&](SimTime time,
                              const std::vector<std::pair<std::uint32_t, std::uint32_t>>& bits,
                              bool fixedZero, Radix radix) -> std::optional<std::string> {
        BitVector digit = BitVector::filled(
            static_cast<std::uint32_t>(bits.size()) + (fixedZero ? 1 : 0), Logic::Zero);
        std::uint32_t position = digit.width();
        for (const auto& [term, bit] : bits) {
            const std::optional<BitVector> value = database.valueAt(records[term], time);
            if (!value.has_value()) return std::nullopt;
            digit.set(--position, value->get(bit));
        }
        return enzan::eir::toString(digit.ref(), radix);
    };

    for (const Logic kind : {Logic::Unknown, Logic::HighZ}) {
        const std::string capital = kind == Logic::Unknown ? "X" : "Z";
        for (const Logic b2 : {Logic::Zero, Logic::Unknown}) {
            std::vector<SearchTerm> terms;
            for (const std::uint32_t record : records)
                terms.push_back({.recordIndex = record, .pattern = BitPattern(3)});
            ASSERT_TRUE(terms[1].pattern.require(2, 2, BitVector::filled(1, b2).ref()));
            ASSERT_TRUE(terms[2].pattern.require(2, 2, BitVector::filled(1, Logic::One).ref()));

            std::vector<DigitTerm> digits(2);
            digits[0].kind = kind;
            for (const auto& [term, bit] : hexBits) digits[0].cover(term, bit);
            digits[1].kind = kind;
            for (const auto& [term, bit] : octalBits) digits[1].cover(term, bit);
            digits[1].coverFixed(Logic::Zero);

            const auto shows = [&](SimTime time) {
                for (const std::uint32_t record : records) {
                    if (!database.valueAt(record, time).has_value()) return false;
                }
                return database.valueAt(records[1], time)->get(2) == b2
                       && database.valueAt(records[2], time)->get(2) == Logic::One
                       && rendered(time, hexBits, false, Radix::Hexadecimal) == capital
                       && rendered(time, octalBits, true, Radix::Octal) == capital;
            };
            const auto walk = [&](SimTime from, bool forward) {
                SearchResult found;
                for (SimTime time = 0; time < 1500; ++time) {
                    if (forward ? time <= from : time >= from) continue;
                    if (shows(time) && (time == 0 || !shows(time - 1))) {
                        found = foundAt(time);
                        if (forward) break;
                    }
                }
                return found;
            };

            for (const SimTime from : {SimTime{0}, SimTime{333}, SimTime{1499}}) {
                for (const bool forward : {true, false}) {
                    const SearchResult expected = walk(from, forward);
                    for (const std::uint64_t budget : {1U, 100U}) {
                        SearchResult result;
                        SimTime at = from;
                        do {
                            result = forward ? database.nextTimeHolding(terms, at, Everywhere,
                                                                        budget, digits)
                                             : database.previousTimeHolding(terms, at, Everywhere,
                                                                            budget, digits);
                            at = result.time;
                        } while (result.outcome == SearchResult::Outcome::Unfinished);
                        EXPECT_EQ(result, expected) << capital << (forward ? " after " : " before ")
                                                    << from << ", budget " << budget;
                    }
                }
            }
        }
    }
}

} // namespace
