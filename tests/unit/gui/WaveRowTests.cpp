// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for what a row of the wave window shows, and for the clipboard.
//
// The rule being pinned here is that a row with children shows what its children show, joined
// together. It looks like a small thing and it is doing three jobs at once: a bus that has had
// some of its bits deleted, a group of one-bit signals standing in for a vector, and the
// nesting of those two that gives multi-dimensional arrays. Written as three features it would
// have been three sets of edge cases; written as one rule it needs one.

#include "gui/WaveformModel.h"

#include "gui/MainWindow.h"
#include "gui/Theme.h"
#include "gui/ValueFinder.h"
#include "gui/WaveformView.h"

#include "SessionFixture.h"

#include "support/Literals.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QDockWidget>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QStringList>
#include <QTableView>
#include <QTest>
#include <QTreeView>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string_view>
#include <tuple>
#include <vector>

namespace enzan::wavedb {

/// How gtest shows a search's outcome when a test of one fails.
///
/// gtest finds it by this name, through the namespace of the type, which neither a name of this
/// project's style nor an anonymous namespace would let it do.
// NOLINTNEXTLINE(readability-identifier-naming,misc-use-internal-linkage)
void PrintTo(const SearchResult& match, std::ostream* out) {
    switch (match.outcome) {
    case SearchResult::Outcome::Found: *out << "found at " << match.time; break;
    case SearchResult::Outcome::Absent: *out << "absent"; break;
    case SearchResult::Outcome::Unfinished: *out << "unfinished at " << match.time; break;
    }
}

} // namespace enzan::wavedb

namespace {

using enzan::eir::Radix;
using enzan::gui::MainWindow;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveRow;
using enzan::test::SessionFixture;

class WaveRowTest : public SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();

        theme.loadBuiltIn();
        model = std::make_unique<WaveformModel>(*session, theme);

        // A run behind the window, so that the rows have values to join.
        clock = infoFor(QStringLiteral("clk"));
        counter = infoFor(QStringLiteral("count"));
        for (const SignalInfo& info : objectsOf(QStringLiteral("/tb/u_dut"))) {
            if (info.name == QStringLiteral("q")) quiet = info;
        }
        session->subscribe({clock.signalIndex, counter.signalIndex, quiet.signalIndex});
        session->run(400);
        pumpUntilCaughtUp(400);
    }

    void TearDown() override {
        model = nullptr;
        SessionFixture::TearDown();
    }

    SignalInfo infoFor(const QString& name) {
        for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
            if (info.name == name) return info;
        }
        ADD_FAILURE() << "no signal " << name.toStdString();
        return {};
    }

    /// The row at `position` under `parent`.
    [[nodiscard]] int childOf(int parent, int position) const {
        return model->rowIdOf(model->index(position, 0, model->indexOfRow(parent)));
    }

    Theme theme;
    std::unique_ptr<WaveformModel> model;
    SignalInfo clock;
    SignalInfo counter;

    /// The child's `q`, which nothing drives: four unknown bits at every time.
    SignalInfo quiet;
};

// ---------------------------------------------------------------------------
// A row with children shows what its children show
// ---------------------------------------------------------------------------

TEST_F(WaveRowTest, AnUnpackedBusStillShowsItsOwnValue) {
    // With every bit present, joining them gives the value the signal already had. Nothing
    // about the row changes when it is opened, which is what makes unpacking free to try.
    const int bus = model->addSignal(counter);
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28"));

    model->unpack(bus);

    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28"));
    EXPECT_EQ(model->displayedWidth(bus), 8U);
    EXPECT_TRUE(model->childrenReconstituteParent(bus))
        << "an untouched unpacking must be recognised, or the row pays to be drawn twice";
}

TEST_F(WaveRowTest, RemovingABitNarrowsWhatTheBusShows) {
    // The parent shows what is left of it. The counter holds 40 at 400ns —
    // 0b00101000 — so dropping bit 3, the set bit, leaves 0b0010000, which is 0x10.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int bit3 = childOf(bus, 4);
    ASSERT_NE(model->row(bit3), nullptr);
    ASSERT_EQ(model->row(bit3)->label, QStringLiteral("count[3]"));

    model->removeRow(bit3);

    EXPECT_EQ(model->displayedWidth(bus), 7U);
    EXPECT_FALSE(model->childrenReconstituteParent(bus));
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("10"));
}

TEST_F(WaveRowTest, RemovingEveryBitLeavesTheBusWithNothingToShow) {
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    while (model->rowCount(model->indexOfRow(bus)) > 0) model->removeRow(childOf(bus, 0));

    EXPECT_EQ(model->displayedWidth(bus), 0U);
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("—"));
}

TEST_F(WaveRowTest, AGroupOfOneBitSignalsBehavesAsAVector) {
    // Four bits of the counter, collected by hand into a group, read as a four-bit number.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int group = model->addGroup(QStringLiteral("nibble"));
    for (int bit = 0; bit < 4; ++bit) {
        ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, group, bit));
    }

    // Bits 7 down to 4 of 40 are 0b0010, which is 2.
    EXPECT_EQ(model->displayedWidth(group), 4U);
    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("2"));

    model->setRadix(group, Radix::Binary);
    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("0010"));
}

TEST_F(WaveRowTest, AGroupWiderThanAWordJoinsAcrossIt) {
    // A group's bits are joined a word at a time, so the bits worth checking are the ones that
    // straddle a word: nine copies of the counter's bits are seventy-two, and 40 is 0x28.
    const int group = model->addGroup(QStringLiteral("wide"));
    for (int copy = 0; copy < 9; ++copy) {
        const int bus = model->addSignal(counter);
        model->unpack(bus);
        while (model->rowCount(model->indexOfRow(bus)) > 0) {
            ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, group,
                                          model->rowCount(model->indexOfRow(group))));
        }
    }

    EXPECT_EQ(model->displayedWidth(group), 72U);
    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("282828282828282828"));
}

TEST_F(WaveRowTest, AGroupOfDifferingWidthsReadsAsAList) {
    // Joining unlike signals into one number would file off the boundaries that say which bits
    // belong to what. The list keeps them, so a group of a clock and a counter is readable
    // rather than being either noise or nothing.
    const int group = model->addGroup(QStringLiteral("odds and ends"));
    model->addSignal(clock, group);
    model->addSignal(counter, group);

    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("{0}{28}"));
    EXPECT_EQ(model->displayedWidth(group), 9U);
}

TEST_F(WaveRowTest, AnEmptyGroupShowsNothing) {
    const int group = model->addGroup(QStringLiteral("empty"));
    EXPECT_TRUE(model->renderValue(group, 400).isEmpty());
    EXPECT_EQ(model->displayedWidth(group), 0U);
}

TEST_F(WaveRowTest, GroupsNestIntoSomethingWider) {
    // The multi-dimensional case: two four-bit groups inside one, which is eight bits. It
    // works because the rule is recursive, not because anything here knows about arrays.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int outer = model->addGroup(QStringLiteral("word"));
    const int high = model->addGroup(QStringLiteral("high"), outer);
    const int low = model->addGroup(QStringLiteral("low"), outer);

    for (int bit = 0; bit < 4; ++bit) {
        ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, high, bit));
    }
    for (int bit = 0; bit < 4; ++bit) {
        ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, low, bit));
    }

    EXPECT_EQ(model->displayedWidth(high), 4U);
    EXPECT_EQ(model->displayedWidth(low), 4U);
    EXPECT_EQ(model->displayedWidth(outer), 8U);

    // Each half is a group of bits, so each reads as a number; the group of those two reads as
    // the list of them. Joining them back into `28` would lose the shape that is the only
    // reason for arranging them this way.
    EXPECT_EQ(model->renderValue(high, 400), QStringLiteral("2"));
    EXPECT_EQ(model->renderValue(low, 400), QStringLiteral("8"));
    EXPECT_EQ(model->renderValue(outer, 400), QStringLiteral("{2}{8}"));
}

TEST_F(WaveRowTest, TheFirstChildIsTheMostSignificant) {
    // Which end is which is not a detail: reversed, every joined value would be wrong and
    // would still look plausible.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int group = model->addGroup(QStringLiteral("two"));
    ASSERT_TRUE(model->moveRowsTo({childOf(bus, 2)}, group, 0)); // count[5], which is 1
    ASSERT_TRUE(model->moveRowsTo({childOf(bus, 2)}, group, 1)); // count[4], which is 0

    model->setRadix(group, Radix::Binary);
    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("10"));
}

TEST_F(WaveRowTest, AJoinedValueTracksTheCursorLikeAnyOther) {
    const int bus = model->addSignal(counter);
    model->unpack(bus);
    model->removeRow(childOf(bus, 0)); // drop bit 7, which is zero throughout

    // The counter reaches 20 at 200ns and 40 at 400ns; dropping the top bit changes neither.
    EXPECT_EQ(model->renderValue(bus, 200), QStringLiteral("14"));
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28"));
}

TEST_F(WaveRowTest, AVectorOfVectorsReadsAsNestedLists) {
    // The shape the notation exists for: a group of groups of vectors reads
    // `{{11}{22}}{{22}{33}}` rather than as one long number.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int outer = model->addGroup(QStringLiteral("array"));
    for (int half = 0; half < 2; ++half) {
        const int inner = model->addGroup(QStringLiteral("pair %1").arg(half), outer);
        for (int pair = 0; pair < 2; ++pair) {
            const int slot = model->addGroup(QStringLiteral("slot"), inner);
            for (int bit = 0; bit < 2; ++bit) {
                ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, slot, bit));
            }
        }
    }

    // 40 is 0b00101000, taken two bits at a time: 00 10 10 00.
    EXPECT_EQ(model->renderValue(outer, 400), QStringLiteral("{{0}{2}}{{2}{0}}"));
}

// ---------------------------------------------------------------------------
// Reading a value as a row shows it
// ---------------------------------------------------------------------------

TEST_F(WaveRowTest, ARowReadsAValueInTheRadixItShowsItIn) {
    const int bus = model->addSignal(counter);
    const std::optional<enzan::eir::BitVector> forty = enzan::eir::BitVector::parse("8'd40");

    EXPECT_EQ(model->readValue(bus, QStringLiteral("28")).value, forty);
    model->setRadix(bus, Radix::Decimal);
    EXPECT_EQ(model->readValue(bus, QStringLiteral("40")).value, forty);
    EXPECT_EQ(model->readValue(bus, QStringLiteral("8'h28")).value, forty) << "a literal anywhere";

    // A slice reads at its own width, so a value the whole signal could hold may not fit it.
    const int low = model->addSlice(bus, 3, 0);
    model->setRadix(low, Radix::Hexadecimal);
    EXPECT_EQ(model->readValue(low, QStringLiteral("f")).value,
              enzan::eir::BitVector::parse("4'hf"));
    EXPECT_EQ(model->readValue(low, QStringLiteral("1f")).problem,
              QStringLiteral("'1f' does not fit in 4 bits"));
}

TEST_F(WaveRowTest, WhatEveryShapeOfRowShowsReadsBackAsWhatItHolds) {
    // The rows `renderValue` writes differently, each typed back from what it wrote at a few
    // times, including before the counter has moved.
    QVector<int> shapes;

    shapes.push_back(model->addSignal(counter));
    shapes.push_back(model->addSignal(clock));

    const int narrowed = model->addSignal(counter);
    model->unpack(narrowed);
    model->removeRow(childOf(narrowed, 4));
    shapes.push_back(narrowed);

    const int source = model->addSignal(counter);
    model->unpack(source);
    const int nibble = model->addGroup(QStringLiteral("nibble"));
    for (int bit = 0; bit < 4; ++bit)
        ASSERT_TRUE(model->moveRowsTo({childOf(source, 0)}, nibble, bit));
    shapes.push_back(nibble);

    const int mixed = model->addGroup(QStringLiteral("mixed"));
    model->addSignal(clock, mixed);
    model->addSignal(counter, mixed);
    shapes.push_back(mixed);

    const int outer = model->addGroup(QStringLiteral("outer"));
    for (int half = 0; half < 2; ++half) {
        const int inner = model->addGroup(QStringLiteral("half"), outer);
        for (int bit = 0; bit < 2; ++bit) {
            ASSERT_TRUE(model->moveRowsTo({childOf(source, 0)}, inner, bit));
        }
    }
    shapes.push_back(outer);

    SignalInfo nibbles = counter;
    nibbles.dimensions = {{.left = 1, .right = 0}, {.left = 3, .right = 0}};
    shapes.push_back(model->addSignal(nibbles));

    for (const int rowId : shapes) {
        for (const std::uint64_t time : {0U, 5U, 200U, 395U}) {
            if (!model->valueAt(rowId, time).has_value()) continue;

            const QString shown = model->renderValue(rowId, time);
            const enzan::gui::ValueReading reading = model->readValue(rowId, shown);
            ASSERT_TRUE(reading.value.has_value())
                << model->row(rowId)->label.toStdString() << " shows " << shown.toStdString()
                << ": " << reading.problem.toStdString();
            EXPECT_EQ(reading.value, model->valueAt(rowId, time))
                << model->row(rowId)->label.toStdString() << " shows " << shown.toStdString();
        }
    }
}

TEST_F(WaveRowTest, AListForARowOfOtherRowsNamesHowManyItHolds) {
    const int group = model->addGroup(QStringLiteral("odds and ends"));
    model->addSignal(clock, group);
    model->addSignal(counter, group);

    EXPECT_EQ(model->readValue(group, QStringLiteral("{1}")).problem,
              QStringLiteral("'{1}' lists 1, and odds and ends holds 2 rows"));
    EXPECT_EQ(model->readValue(group, QStringLiteral("{1}{1ff}")).problem,
              QStringLiteral("'1ff' does not fit in 8 bits"))
        << "a word is read as its own row reads it";

    // A number the width of the whole group is read too.
    EXPECT_EQ(model->readValue(group, QStringLiteral("128")).value,
              enzan::eir::BitVector::parse("9'h128"));
}

TEST_F(WaveRowTest, ARowWithNoBitsHasNoValueToRead) {
    const int group = model->addGroup(QStringLiteral("empty"));
    EXPECT_EQ(model->readValue(group, QStringLiteral("0")).problem,
              QStringLiteral("empty shows no bits to hold a value"));
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

TEST_F(WaveRowTest, AGroupHasTheEdgesOfEverythingInIt) {
    // A group shows a value on its line, so it has edges to step between. They
    // are its children's put together.
    const int group = model->addGroup(QStringLiteral("both"));
    model->addSignal(clock, group);
    model->addSignal(counter, group);

    // The clock toggles every five units and the counter every ten, so the group changes every
    // five — and the counter's changes coincide with the clock's rising edges.
    EXPECT_EQ(model->nextEdgeAfter(group, 100), 105U);
    EXPECT_EQ(model->nextEdgeAfter(group, 105), 110U);
    EXPECT_EQ(model->previousEdgeBefore(group, 105), 100U);
}

TEST_F(WaveRowTest, SteppingBackwardsLandsOnTheEdgeJustPassed) {
    // The cursor sits at 101, one unit after the clock changed at 100. Stepping back must find
    // 100, not skip it because the value there is the value the cursor already has.
    const int row = model->addSignal(clock);

    EXPECT_EQ(model->previousEdgeBefore(row, 101), 100U);
    EXPECT_EQ(model->previousEdgeBefore(row, 100), 95U);
}

TEST_F(WaveRowTest, AnUnpackedBitStepsOnlyWhereThatBitChanges) {
    // The counter changes every ten units, but its top bits do not. Stepping along bit 4 must
    // walk the sixteen-count boundaries rather than stopping at every increment.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int bit4 = childOf(bus, 3);
    ASSERT_NE(model->row(bit4), nullptr);
    ASSERT_EQ(model->row(bit4)->label, QStringLiteral("count[4]"));

    // The clock's first rising edge is at 5 and they are ten apart, so the counter reaches n
    // at 10n-5: bit 4 is first set at 155, when the count reaches 16, and clears at 315.
    EXPECT_EQ(model->nextEdgeAfter(bit4, 0), 155U);
    EXPECT_EQ(model->nextEdgeAfter(bit4, 155), 315U);
    EXPECT_EQ(model->previousEdgeBefore(bit4, 300), 155U);
}

TEST_F(WaveRowTest, EdgesRunOutRatherThanWrapping) {
    const int row = model->addSignal(clock);

    EXPECT_EQ(model->nextEdgeAfter(row, 100'000), enzan::runtime::NoTime);
    EXPECT_EQ(model->previousEdgeBefore(row, 0), enzan::runtime::NoTime);
}

TEST_F(WaveRowTest, TheNearestEdgeIsFoundFromEitherSide) {
    const int row = model->addSignal(clock);

    EXPECT_EQ(model->nearestEdge(row, 101), 100U);
    EXPECT_EQ(model->nearestEdge(row, 104), 105U);
}

// ---------------------------------------------------------------------------
// Finding a value
// ---------------------------------------------------------------------------
//
// The counter takes each value v at 10v - 5, on the clock's rising edge, and the clock is low from
// 0 and toggles every 5; the run stops at 400.

class FindValueTest : public WaveRowTest {
protected:
    using Match = WaveformModel::Match;

    /// Far more edges than the run has, for a search meant to finish.
    static constexpr int Unbounded = 1 << 20;

    [[nodiscard]] Match next(int rowId, enzan::runtime::SimTime from, std::string_view value,
                             enzan::runtime::SimTime limit = 400, int budget = Unbounded) const {
        return model->nextMatchAfter(rowId, from, enzan::test::literal(value), limit, budget);
    }

    [[nodiscard]] Match previous(int rowId, enzan::runtime::SimTime from, std::string_view value,
                                 enzan::runtime::SimTime limit = 400,
                                 int budget = Unbounded) const {
        return model->previousMatchBefore(rowId, from, enzan::test::literal(value), limit, budget);
    }

    static Match found(enzan::runtime::SimTime time) {
        return {.outcome = Match::Outcome::Found, .time = time};
    }
};

const WaveformModel::Match Absent{};

TEST_F(FindValueTest, AValueIsFoundWhereTheRowStartsToShowIt) {
    const int count = model->addSignal(counter);

    EXPECT_EQ(next(count, 0, "8'd5"), found(45));
    EXPECT_EQ(previous(count, 400, "8'd5"), found(45));
    EXPECT_EQ(next(count, 0, "8'd200"), Absent);
}

TEST_F(FindValueTest, FromInsideAStretchNextFindsTheNextOneAndPreviousItsStart) {
    // The clock is high over [5, 10) and again from 15: the stretch the cursor is in is not one
    // to find again, and its start is the one just passed.
    const int clk = model->addSignal(clock);

    EXPECT_EQ(next(clk, 7, "1'b1"), found(15));
    EXPECT_EQ(previous(clk, 7, "1'b1"), found(5));

    // From the start of a stretch, as a cursor just put there by a search is.
    EXPECT_EQ(next(clk, 5, "1'b1"), found(15));
    EXPECT_EQ(previous(clk, 5, "1'b1"), Absent) << "nothing before 5 starts to be high";
    EXPECT_EQ(previous(clk, 15, "1'b1"), found(5));
}

TEST_F(FindValueTest, AValueShownFromTheStartIsFoundAtZero) {
    const int clk = model->addSignal(clock);
    EXPECT_EQ(previous(clk, 7, "1'b0"), found(0));

    const int count = model->addSignal(counter);
    EXPECT_EQ(previous(count, 400, "8'd0"), found(0));
    EXPECT_EQ(next(count, 0, "8'd0"), Absent) << "a search after zero does not find zero";
}

TEST_F(FindValueTest, ASliceIsFoundWhereItsBitsChangeRatherThanWhereItsSignalDoes) {
    // Bit 3 of the counter is set from 8 to 15, from 75, and again from 24, at 235. The counter
    // changes eight times between, each an edge of the signal and none a start of the bit.
    const int count = model->addSignal(counter);
    const int bit3 = model->addSlice(count, 3, 3);

    EXPECT_EQ(next(bit3, 80, "1'b1"), found(235));
    EXPECT_EQ(previous(bit3, 230, "1'b1"), found(75));
}

TEST_F(FindValueTest, AGroupIsFoundByWhatItShows) {
    // The clock beside the counter: high, and 40, from 395.
    const int group = model->addGroup(QStringLiteral("mixed"));
    model->addSignal(clock, group);
    model->addSignal(counter, group);

    EXPECT_EQ(next(group, 0, "9'h128"), found(395));
    EXPECT_EQ(previous(group, 400, "9'h028"), Absent)
        << "the counter is 40 only once the clock has risen";
}

TEST_F(FindValueTest, NothingAtOrPastTheLimitIsFound) {
    const int count = model->addSignal(counter);

    EXPECT_EQ(next(count, 0, "8'd40", 395), Absent);
    EXPECT_EQ(next(count, 0, "8'd40", 396), found(395));

    // A cursor past the limit searches back from it.
    EXPECT_EQ(previous(count, 500, "8'd40", 395), Absent);
    EXPECT_EQ(previous(count, 500, "8'd39", 395), found(385));
}

TEST_F(FindValueTest, ASearchThatRunsOutSaysHowFarItGotAndGoesOnFromThere) {
    const int count = model->addSignal(counter);

    // Four edges, 5 to 35, and none of them a 30.
    EXPECT_EQ(next(count, 0, "8'd30", 400, 4),
              (Match{.outcome = Match::Outcome::Unfinished, .time = 35}));

    Match match = next(count, 0, "8'd30", 400, 4);
    int searches = 1;
    while (match.outcome == Match::Outcome::Unfinished) {
        const enzan::runtime::SimTime from = match.time;
        match = next(count, from, "8'd30", 400, 4);
        ASSERT_TRUE(match.outcome != Match::Outcome::Unfinished || match.time > from);
        ++searches;
    }
    EXPECT_EQ(match, found(295));
    EXPECT_EQ(searches, 8) << "thirty edges, four at a time";

    // Backwards the same: 395 down to 365, then on.
    EXPECT_EQ(previous(count, 400, "8'd10", 400, 4),
              (Match{.outcome = Match::Outcome::Unfinished, .time = 365}));
    match = previous(count, 400, "8'd10", 400, 4);
    while (match.outcome == Match::Outcome::Unfinished) {
        const enzan::runtime::SimTime from = match.time;
        match = previous(count, from, "8'd10", 400, 4);
        ASSERT_TRUE(match.outcome != Match::Outcome::Unfinished || match.time < from);
    }
    EXPECT_EQ(match, found(95));
}

TEST_F(FindValueTest, EveryShapeOfRowIsFoundWhereItsOwnValueStartsToBeTheOneAskedFor) {
    // A search takes a row apart into the bits of the signals it shows and asks the database
    // where those start to hold their parts together. What it must answer is where the row's own
    // value starts to be the one asked for, which the row says directly: its value is `valueAt`,
    // and a value starts only where the row has an edge. So every row is held to a walk over its
    // edges asking `valueAt` at each, for every value it shows, from either side and in steps of
    // any size.
    QVector<int> shapes;
    shapes.push_back(model->addSignal(counter));
    shapes.push_back(model->addSignal(clock));
    shapes.push_back(model->addSlice(model->addSignal(counter), 2, 0));

    // A bus with a bit taken out, joined from what is left of it.
    const int narrowed = model->addSignal(counter);
    model->unpack(narrowed);
    model->removeRow(childOf(narrowed, 4));
    shapes.push_back(narrowed);

    // Two signals that change apart as often as together.
    const int mixed = model->addGroup(QStringLiteral("clock and count"));
    model->addSignal(clock, mixed);
    model->addSignal(counter, mixed);
    shapes.push_back(mixed);

    // Bits of a bus regrouped in an order of the reader's own, in groups inside a group.
    const int source = model->addSignal(counter);
    model->unpack(source);
    const int outer = model->addGroup(QStringLiteral("regrouped"));
    const int first = model->addGroup(QStringLiteral("first"), outer);
    const int second = model->addGroup(QStringLiteral("second"), outer);
    ASSERT_TRUE(model->moveRowsTo({childOf(source, 7)}, first, 0));  // count[0]
    ASSERT_TRUE(model->moveRowsTo({childOf(source, 1)}, first, 1));  // count[6]
    ASSERT_TRUE(model->moveRowsTo({childOf(source, 2)}, second, 0)); // count[4]
    ASSERT_TRUE(model->moveRowsTo({childOf(source, 1)}, second, 1)); // count[5]
    shapes.push_back(outer);

    // One signal shown twice, and a parameter beside a signal.
    const int twice = model->addGroup(QStringLiteral("twice"));
    model->addSignal(counter, twice);
    model->addSlice(model->addSignal(counter, twice), 3, 0);
    shapes.push_back(twice);

    SignalInfo width;
    for (const SignalInfo& info : objectsOf(QStringLiteral("/tb/u_dut"))) {
        if (info.name == QStringLiteral("WIDTH")) width = info;
    }
    ASSERT_TRUE(width.isParameter());
    const int constant = model->addGroup(QStringLiteral("with a parameter"));
    model->addSignal(width, constant);
    model->addSignal(clock, constant);
    shapes.push_back(constant);

    const auto walk = [this](int rowId, enzan::runtime::SimTime from,
                             const enzan::eir::BitVector& value, bool forward) {
        enzan::runtime::SimTime edge =
            forward
                ? model->nextEdgeAfter(rowId, from)
                : model->previousEdgeBefore(rowId, std::min<enzan::runtime::SimTime>(from, 400));
        // NoTime, which there is past the last edge, is no time below the limit either.
        while (edge < 400) {
            if (model->valueAt(rowId, edge) == value) return found(edge);
            edge = forward ? model->nextEdgeAfter(rowId, edge)
                           : model->previousEdgeBefore(rowId, edge);
        }
        return Match{};
    };

    const auto settle = [this](int rowId, enzan::runtime::SimTime from,
                               const enzan::eir::BitVector& value, bool forward, int budget) {
        Match match;
        do {
            match = forward ? model->nextMatchAfter(rowId, from, value, 400, budget)
                            : model->previousMatchBefore(rowId, from, value, 400, budget);
            from = match.time;
        } while (match.outcome == Match::Outcome::Unfinished);
        return match;
    };

    for (const int rowId : shapes) {
        // The values the row shows, a few of them, at its edges.
        std::vector<enzan::eir::BitVector> values;
        for (enzan::runtime::SimTime edge = 0; edge < 400 && values.size() < 6;
             edge = model->nextEdgeAfter(rowId, edge)) {
            const std::optional<enzan::eir::BitVector> value = model->valueAt(rowId, edge);
            if (value.has_value() && std::ranges::find(values, *value) == values.end())
                values.push_back(*value);
            if (edge == enzan::runtime::NoTime) break;
        }
        ASSERT_FALSE(values.empty()) << model->row(rowId)->label.toStdString();

        for (const enzan::eir::BitVector& value : values) {
            for (const enzan::runtime::SimTime from : {0U, 7U, 45U, 200U, 399U, 500U}) {
                for (const bool forward : {true, false}) {
                    const Match expected = walk(rowId, from, value, forward);
                    for (const int budget : {1, 3, Unbounded}) {
                        EXPECT_EQ(settle(rowId, from, value, forward, budget), expected)
                            << model->row(rowId)->label.toStdString() << " for "
                            << enzan::eir::toString(value.ref(), Radix::Binary)
                            << (forward ? " after " : " before ") << from << ", budget " << budget;
                    }
                }
            }
        }
    }
}

TEST_F(FindValueTest, WhatARowShowsIsFoundWhereItStartsToShowItPartlyUnknownDigitsAndAll) {
    // Whatever a row shows can be typed back, and a search for it finds where the row starts to
    // show it. A digit the column writes in capitals is partly unknown, which no one value of its
    // bits is, and here such digits cover bits of several signals at once: the quiet `q`, which is
    // unknown, beside the clock and the counter, which are not. So each row is held to a walk over
    // every time comparing what it shows there with the text.
    const int counted = model->addSignal(counter);
    model->unpack(counted); // count[7] first
    const int quieted = model->addSignal(quiet);
    model->unpack(quieted); // q[3] first
    ASSERT_TRUE(model->valueAt(childOf(quieted, 0), 0).has_value())
        << "q is recorded from the start";

    // The counter's high nibble as one digit, and q[3] with the clock and count[1:0] as another.
    QVector<int> shapes;
    const int nibbles = model->addGroup(QStringLiteral("nibbles"));
    for (int bit = 0; bit < 4; ++bit)
        ASSERT_TRUE(model->moveRowsTo({childOf(counted, 0)}, nibbles, bit));
    ASSERT_TRUE(model->moveRowsTo({childOf(quieted, 0)}, nibbles, 4));
    model->addSignal(clock, nibbles);
    ASSERT_TRUE(model->moveRowsTo({childOf(counted, 2)}, nibbles, 6)); // count[1]
    ASSERT_TRUE(model->moveRowsTo({childOf(counted, 2)}, nibbles, 7)); // count[0]
    shapes.push_back(nibbles);

    // In decimal, which writes one digit for the whole of a partly unknown value.
    const int decimal = model->addGroup(QStringLiteral("decimal"));
    ASSERT_TRUE(model->moveRowsTo({childOf(quieted, 0)}, decimal, 0)); // q[2]
    model->addSignal(clock, decimal);
    model->setRadix(decimal, Radix::Decimal);
    shapes.push_back(decimal);

    // A list beside it, whose digits are wholly unknown or not at all.
    const int listed = model->addGroup(QStringLiteral("listed"));
    model->addSignal(quiet, listed);
    model->addSignal(counter, listed);
    shapes.push_back(listed);

    for (const int rowId : shapes) {
        std::vector<QString> shown(400);
        for (enzan::runtime::SimTime time = 0; time < 400; ++time) {
            if (model->valueAt(rowId, time).has_value())
                shown[time] = model->renderValue(rowId, time);
        }

        std::vector<QString> texts;
        for (enzan::runtime::SimTime time = 0; time < 400 && texts.size() < 5; time += 37) {
            if (!shown[time].isEmpty() && std::ranges::find(texts, shown[time]) == texts.end())
                texts.push_back(shown[time]);
        }
        ASSERT_FALSE(texts.empty()) << model->row(rowId)->label.toStdString();
        const std::string label = model->row(rowId)->label.toStdString();
        if (rowId != listed) {
            EXPECT_TRUE(
                std::ranges::any_of(texts, [](const QString& text) { return text.contains(u'X'); }))
                << label << " shows a partly unknown digit";
        }

        for (const QString& text : texts) {
            const enzan::gui::ValueReading reading = model->readValue(rowId, text);
            ASSERT_TRUE(reading.value.has_value())
                << label << ": " << reading.problem.toStdString();

            const auto walk = [&](enzan::runtime::SimTime from, bool forward) {
                Match first;
                for (enzan::runtime::SimTime time = 0; time < 400; ++time) {
                    if (forward ? time <= from : time >= from) continue;
                    if (shown[time] == text && (time == 0 || shown[time - 1] != text)) {
                        first = found(time);
                        if (forward) break;
                    }
                }
                return first;
            };

            for (const enzan::runtime::SimTime from : {0U, 7U, 156U, 399U, 500U}) {
                for (const bool forward : {true, false}) {
                    const Match expected = walk(from, forward);
                    for (const int budget : {1, 3, Unbounded}) {
                        Match match;
                        enzan::runtime::SimTime at = from;
                        do {
                            match = forward
                                        ? model->nextMatchAfter(rowId, at, *reading.value, 400,
                                                                budget, reading.partial)
                                        : model->previousMatchBefore(rowId, at, *reading.value, 400,
                                                                     budget, reading.partial);
                            at = match.time;
                        } while (match.outcome == Match::Outcome::Unfinished);
                        EXPECT_EQ(match, expected)
                            << label << " for " << text.toStdString()
                            << (forward ? " after " : " before ") << from << ", budget " << budget;
                    }
                }
            }
        }
    }
}

TEST_F(FindValueTest, ADigitAcrossAParameterAndASignalCountsTheBitsOfBoth) {
    // WIDTH, 4, beside q[1:0]: the lowest hexadecimal digit of the whole is WIDTH's two low bits,
    // both 0, and q's two unknown ones, so it is partly unknown and never wholly.
    SignalInfo width;
    for (const SignalInfo& info : objectsOf(QStringLiteral("/tb/u_dut"))) {
        if (info.name == QStringLiteral("WIDTH")) width = info;
    }
    ASSERT_TRUE(width.isParameter());
    const int group = model->addGroup(QStringLiteral("beside a parameter"));
    model->addSignal(width, group);
    const int whole = model->addSignal(quiet, group);
    model->addSlice(whole, 1, 0);
    model->removeRow(whole);
    ASSERT_EQ(model->displayedWidth(group), 34U);

    const auto search = [&](const QString& text) {
        const enzan::gui::ValueReading reading = model->readValue(group, text);
        EXPECT_TRUE(reading.value.has_value()) << reading.problem.toStdString();
        return model->previousMatchBefore(group, 400, *reading.value, 400, Unbounded,
                                          reading.partial);
    };
    EXPECT_EQ(search(QStringLiteral("00000001X")), found(0));
    EXPECT_EQ(search(QStringLiteral("00000001x")), Absent) << "WIDTH's bits are known";
    EXPECT_EQ(search(QStringLiteral("00000002X")), Absent) << "WIDTH is 4, not 8";
}

TEST_F(FindValueTest, AValueARowCanNeverShowIsFoundNowhere) {
    // A signal shown twice cannot show two values at once, and a parameter shows only its own.
    const int twice = model->addGroup(QStringLiteral("twice"));
    model->addSignal(counter, twice);
    model->addSignal(counter, twice);
    EXPECT_EQ(next(twice, 0, "16'h0505"), found(45));
    EXPECT_EQ(next(twice, 0, "16'h0506"), Absent);

    SignalInfo width;
    for (const SignalInfo& info : objectsOf(QStringLiteral("/tb/u_dut"))) {
        if (info.name == QStringLiteral("WIDTH")) width = info;
    }
    const int constant = model->addGroup(QStringLiteral("with a parameter"));
    model->addSignal(width, constant);
    model->addSignal(counter, constant);
    EXPECT_EQ(next(constant, 0, "40'h00000004_05"), found(45));
    EXPECT_EQ(next(constant, 0, "40'h00000005_05"), Absent);

    EXPECT_EQ(next(model->addGroup(QStringLiteral("empty")), 0, "1'b0"), Absent);
    EXPECT_EQ(next(twice, 0, "8'h05"), Absent) << "a value of another width";
}

TEST_F(FindValueTest, ARemovedRowHasNothingToFind) {
    const int count = model->addSignal(counter);
    model->removeRow(count);

    EXPECT_EQ(next(count, 0, "8'd5"), Absent);
    EXPECT_EQ(previous(count, 400, "8'd5"), Absent);
}

// ---------------------------------------------------------------------------
// An unpacked vector is a group
// ---------------------------------------------------------------------------

TEST_F(WaveRowTest, AnUnpackedVectorAcceptsRowsLikeAGroup) {
    // A multi-bit signal is a group whose children happen to have been made for it. A container
    // that let rows be dragged out but not back in would not be the same kind of thing at all, and
    // a bit taken out by mistake could never be put back.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int bit3 = childOf(bus, 4);
    ASSERT_EQ(model->row(bit3)->label, QStringLiteral("count[3]"));

    ASSERT_TRUE(model->moveRowsTo({bit3}, -1, 0));
    EXPECT_EQ(model->row(bit3)->parent, -1);
    EXPECT_EQ(model->displayedWidth(bus), 7U);

    ASSERT_TRUE(model->moveRowsTo({bit3}, bus, 4)) << "a bit must be able to go back where it was";
    EXPECT_EQ(model->row(bit3)->parent, bus);
    EXPECT_EQ(model->displayedWidth(bus), 8U);
    EXPECT_TRUE(model->childrenReconstituteParent(bus))
        << "put back in order, the vector is itself again";
}

TEST_F(WaveRowTest, APackedRowHasNothingToDropInto) {
    // A row that shows no children has nowhere to put one, so it refuses rather than silently
    // swallowing them.
    const int packed = model->addSignal(counter);
    EXPECT_FALSE(model->flags(model->indexOfRow(packed)).testFlag(Qt::ItemIsDropEnabled));

    model->unpack(packed);
    EXPECT_TRUE(model->flags(model->indexOfRow(packed)).testFlag(Qt::ItemIsDropEnabled));

    const int group = model->addGroup(QStringLiteral("Bus"));
    EXPECT_TRUE(model->flags(model->indexOfRow(group)).testFlag(Qt::ItemIsDropEnabled));
}

TEST_F(WaveRowTest, AVectorWithSomethingElseInItShowsWhatIsActuallyThere) {
    // The cost of treating a vector as a group: the row still says `count` while showing a bit
    // that is not count's. The value tells the truth even when the label no longer does, which
    // is the same bargain deleting a bit already made.
    const int bus = model->addSignal(counter);
    model->unpack(bus);
    ASSERT_TRUE(model->childrenReconstituteParent(bus));

    const int stray = model->addSignal(clock);
    ASSERT_TRUE(model->moveRowsTo({stray}, bus, 0));

    EXPECT_FALSE(model->childrenReconstituteParent(bus));
    EXPECT_EQ(model->displayedWidth(bus), 9U);

    // The clock is low at 400ns and the counter holds 40, so the nine bits read 0 0010 1000.
    model->setRadix(bus, Radix::Binary);
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("000101000"));
}

TEST_F(WaveRowTest, MovingARowDoesNotResetTheModel) {
    // `moveToGroup` used to reset, which made the view forget every expansion in the window:
    // collecting two signals into a group packed every other bus on screen.
    const int first = model->addSignal(clock);
    const int group = model->addGroup(QStringLiteral("Bus"));

    int resets = 0;
    QObject::connect(model.get(), &QAbstractItemModel::modelReset, [&resets] { ++resets; });

    model->moveToGroup(first, group);

    EXPECT_EQ(resets, 0);
    EXPECT_EQ(model->row(first)->parent, group);
}

TEST_F(WaveRowTest, AGroupIsAddedWhereItIsAskedFor) {
    const int outer = model->addGroup(QStringLiteral("outer"));
    const int first = model->addSignal(clock, outer);
    const int second = model->addSignal(counter, outer);

    // The view is told the same position the list takes, or it would draw the group beside the
    // wrong row until something made it ask again.
    int announced = -1;
    QObject::connect(model.get(), &QAbstractItemModel::rowsInserted,
                     [&announced](const QModelIndex& /*parent*/, int firstRow, int /*last*/) {
                         announced = firstRow;
                     });
    const int between = model->addGroup(QStringLiteral("between"), outer, 1);
    EXPECT_EQ(announced, 1);

    const int last = model->addGroup(QStringLiteral("last"), outer);
    const int past = model->addGroup(QStringLiteral("past the end"), outer, 99);

    EXPECT_EQ(model->row(outer)->children, (QVector<int>{first, between, second, last, past}));
}

// ---------------------------------------------------------------------------
// An array unpacks by its dimensions
// ---------------------------------------------------------------------------

// The counter holds 40, 0b0010'1000, at 400ns. Given dimensions, as the kernel lists a signal
// declared as an array, it reads and unpacks as that array.

TEST_F(WaveRowTest, AnArrayReadsAsTheListOfItsElements) {
    SignalInfo nibbles = counter;
    nibbles.dimensions = {{.left = 1, .right = 0}, {.left = 3, .right = 0}};
    const int array = model->addSignal(nibbles);
    EXPECT_EQ(model->renderValue(array, 400), QStringLiteral("{2}{8}"));

    model->unpack(array);
    ASSERT_EQ(model->rowCount(model->indexOfRow(array)), 2);
    const WaveRow* high = model->row(childOf(array, 0));
    const WaveRow* low = model->row(childOf(array, 1));
    EXPECT_EQ(high->label, QStringLiteral("count[1]"));
    EXPECT_EQ(low->label, QStringLiteral("count[0]"));
    EXPECT_EQ(model->displayedWidth(childOf(array, 0)), 4U);
    EXPECT_EQ(model->renderValue(childOf(array, 0), 400), QStringLiteral("2"));
    EXPECT_EQ(model->renderValue(childOf(array, 1), 400), QStringLiteral("8"));

    // Unpacking changes nothing about what the array's own line says.
    EXPECT_EQ(model->renderValue(array, 400), QStringLiteral("{2}{8}"));
    EXPECT_TRUE(model->childrenReconstituteParent(array));
}

TEST_F(WaveRowTest, AnElementIsNamedAndUnpackedByTheIndicesItIsDeclaredWith) {
    // bit [0:1][0:3]: element 0 is the left one, the highest, and bit 0 of it its highest bit.
    SignalInfo ascending = counter;
    ascending.dimensions = {{.left = 0, .right = 1}, {.left = 0, .right = 3}};
    const int array = model->addSignal(ascending);
    model->unpack(array);

    const int first = childOf(array, 0);
    EXPECT_EQ(model->row(first)->label, QStringLiteral("count[0]"));
    EXPECT_EQ(model->renderValue(first, 400), QStringLiteral("2"));
    ASSERT_TRUE(model->canUnpack(first));

    model->unpack(first);
    ASSERT_EQ(model->rowCount(model->indexOfRow(first)), 4);
    QStringList labels;
    QStringList values;
    for (int position = 0; position < 4; ++position) {
        labels << model->row(childOf(first, position))->label;
        values << model->renderValue(childOf(first, position), 400);
    }
    EXPECT_EQ(labels, (QStringList{QStringLiteral("count[0][0]"), QStringLiteral("count[0][1]"),
                                   QStringLiteral("count[0][2]"), QStringLiteral("count[0][3]")}));
    EXPECT_EQ(values, (QStringList{QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("1"),
                                   QStringLiteral("0")}));
    EXPECT_FALSE(model->canUnpack(childOf(first, 0))) << "a bit has nothing left to unpack";

    // An element taken apart is still the element, and the array still itself.
    EXPECT_TRUE(model->childrenReconstituteParent(first));
    EXPECT_TRUE(model->childrenReconstituteParent(array));
    EXPECT_EQ(model->renderValue(array, 400), QStringLiteral("{2}{8}"));
}

TEST_F(WaveRowTest, AnArrayOfThreeDimensionsNestsItsLists) {
    SignalInfo cube = counter;
    cube.dimensions = {{.left = 1, .right = 0}, {.left = 1, .right = 0}, {.left = 1, .right = 0}};
    const int array = model->addSignal(cube);
    EXPECT_EQ(model->renderValue(array, 400), QStringLiteral("{{0}{2}}{{2}{0}}"));

    model->unpack(array);
    EXPECT_EQ(model->renderValue(childOf(array, 0), 400), QStringLiteral("{0}{2}"));
}

TEST_F(WaveRowTest, AVectorDeclaredAscendingUnpacksFromItsLeftIndex) {
    // logic [0:7]: the bit called 0 is the most significant, so it is the first row.
    SignalInfo ascending = counter;
    ascending.dimensions = {{.left = 0, .right = 7}};
    const int bus = model->addSignal(ascending);
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28")) << "a vector is one number";

    model->unpack(bus);
    EXPECT_EQ(model->row(childOf(bus, 0))->label, QStringLiteral("count[0]"));
    EXPECT_EQ(model->row(childOf(bus, 2))->label, QStringLiteral("count[2]"));
    EXPECT_EQ(model->renderValue(childOf(bus, 2), 400), QStringLiteral("1"));
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28"));
}

TEST_F(WaveRowTest, RemovingAnElementLeavesTheListOfTheRest) {
    SignalInfo nibbles = counter;
    nibbles.dimensions = {{.left = 1, .right = 0}, {.left = 3, .right = 0}};
    const int array = model->addSignal(nibbles);
    model->unpack(array);
    model->removeRow(childOf(array, 0));

    EXPECT_FALSE(model->childrenReconstituteParent(array));
    EXPECT_EQ(model->displayedWidth(array), 4U);
    EXPECT_EQ(model->renderValue(array, 400), QStringLiteral("{8}"));
}

TEST_F(WaveRowTest, ARowIsNamedByTheSelectsTheDesignWouldWrite) {
    SignalInfo nibbles = counter;
    nibbles.dimensions = {{.left = 1, .right = 0}, {.left = 0, .right = 3}};
    const WaveRow whole = WaveformModel::rowFor(nibbles);

    EXPECT_EQ(WaveformModel::selectsOf(whole), QString());
    EXPECT_EQ(WaveformModel::selectsOf(WaveformModel::elementOf(whole, 0)), QStringLiteral("[1]"));
    EXPECT_EQ(
        WaveformModel::selectsOf(WaveformModel::elementOf(WaveformModel::elementOf(whole, 1), 0)),
        QStringLiteral("[0][0]"));
    EXPECT_EQ(WaveformModel::selectsOf(WaveformModel::sliceOf(whole, 7, 0)),
              QStringLiteral("[1:0]"));
    EXPECT_EQ(WaveformModel::selectsOf(WaveformModel::sliceOf(whole, 2, 1)),
              QStringLiteral("[0][1:2]"));
    EXPECT_EQ(WaveformModel::selectsOf(WaveformModel::sliceOf(whole, 5, 2)), std::nullopt)
        << "part of one element and part of the next is no select";

    // And what they name, `selectOf` makes again.
    const std::optional<WaveRow> selected = WaveformModel::selectOf(
        whole, {enzan::gui::IndexSelect{.left = 0},
                enzan::gui::IndexSelect{.left = 1, .right = 2, .range = true}});
    ASSERT_TRUE(selected.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — checked by the assertion above
    const WaveRow& picked = *selected;
    EXPECT_EQ(picked.highBit, 2U);
    EXPECT_EQ(picked.lowBit, 1U);
    EXPECT_EQ(WaveformModel::selectsOf(picked), QStringLiteral("[0][1:2]"));

    // A bit taken as a slice of a signal of one bit is the signal.
    EXPECT_EQ(WaveformModel::selectsOf(WaveformModel::sliceOf(WaveformModel::rowFor(clock), 0, 0)),
              QString());
}

TEST_F(WaveRowTest, AnArrayCopiesWithItsDimensions) {
    SignalInfo nibbles = counter;
    nibbles.dimensions = {{.left = 1, .right = 0}, {.left = 3, .right = 0}};
    const int array = model->addSignal(nibbles);
    model->unpack(array);

    model->copySignalRows({array});
    const QVector<int> pasted = model->pasteSignalRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);
    EXPECT_EQ(model->row(pasted[0])->dimensions, nibbles.dimensions);
    EXPECT_EQ(model->renderValue(pasted[0], 400), QStringLiteral("{2}{8}"));

    // Its elements came with their own, and unpack into bits as they did before the copy.
    const int element = childOf(pasted[0], 1);
    EXPECT_EQ(model->row(element)->dimensions,
              (std::vector<enzan::eir::ArrayDimension>{{.left = 3, .right = 0}}));
    model->unpack(element);
    EXPECT_EQ(model->rowCount(model->indexOfRow(element)), 4);
}

#ifdef ENZAN_WITH_SYSTEMVERILOG
/// The window over a SystemVerilog testbench whose counters are declared as large_design_00's
/// are, run for twenty nanoseconds: every element holds its index plus one, and at 10ns
/// element 2 of each changes.
class SourceArrayTest : public SessionFixture {
protected:
    bool loadDesign(enzan::session::Session& into) override {
        workspace.write("tb.sv", R"(
            module tb;
                logic [3:0][7:0] count_rec;
                logic [7:0] count_rec0 [3:0];
                logic count_rec1 [3:0][7:0];
                initial begin
                    for (int i = 0; i < 4; i++) begin
                        count_rec[i] = 8'(i + 1);
                        count_rec0[i] = 8'(i + 1);
                        for (int j = 0; j < 8; j++) count_rec1[i][j] = count_rec0[i][j];
                    end
                    #10;
                    count_rec[2] = 8'hff;
                    count_rec0[2] = 8'hff;
                    count_rec1[2][1] = 1'b0;
                    #10;
                end
            endmodule
        )");
        enzan::session::Session::AnalysisRequest request;
        request.files.push_back(workspace / "tb.sv");
        return into.analyse(request);
    }

    void SetUp() override {
        SessionFixture::SetUp();
        theme.loadBuiltIn();
        model = std::make_unique<WaveformModel>(*session, theme);

        QVector<std::uint32_t> indices;
        for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
            signals.insert(info.name, info);
            indices.push_back(info.signalIndex);
        }
        session->subscribe(indices);
        session->run(20);
        pumpUntilCaughtUp(20);
    }

    void TearDown() override {
        model = nullptr;
        SessionFixture::TearDown();
    }

    [[nodiscard]] int childOf(int parent, int position) const {
        return model->rowIdOf(model->index(position, 0, model->indexOfRow(parent)));
    }

    enzan::test::Workspace workspace;
    Theme theme;
    std::unique_ptr<WaveformModel> model;
    QHash<QString, SignalInfo> signals;
};

TEST_F(SourceArrayTest, EachCounterReadsAsItsElementsHoweverItIsDeclared) {
    // Packed, unpacked and an unpacked array of bits read alike, element by element, rather
    // than as one wide number.
    const int packed = model->addSignal(signals.value(QStringLiteral("count_rec")));
    const int unpacked = model->addSignal(signals.value(QStringLiteral("count_rec0")));
    const int bits = model->addSignal(signals.value(QStringLiteral("count_rec1")));

    for (const int row : {packed, unpacked, bits})
        EXPECT_EQ(model->renderValue(row, 5), QStringLiteral("{04}{03}{02}{01}"))
            << model->row(row)->label.toStdString();
    EXPECT_EQ(model->renderValue(packed, 10), QStringLiteral("{04}{ff}{02}{01}"));
    EXPECT_EQ(model->renderValue(unpacked, 10), QStringLiteral("{04}{ff}{02}{01}"));
    EXPECT_EQ(model->renderValue(bits, 10), QStringLiteral("{04}{01}{02}{01}"));
}

TEST_F(SourceArrayTest, EachCounterReadsBackTheListItShows) {
    for (const QString& name : {QStringLiteral("count_rec"), QStringLiteral("count_rec0"),
                                QStringLiteral("count_rec1")}) {
        const int row = model->addSignal(signals.value(name));
        for (const std::uint64_t time : {5U, 10U}) {
            const enzan::gui::ValueReading reading =
                model->readValue(row, model->renderValue(row, time));
            ASSERT_TRUE(reading.value.has_value()) << name.toStdString();
            EXPECT_EQ(reading.value, model->valueAt(row, time)) << name.toStdString();
        }
    }
}

TEST_F(SourceArrayTest, EachCounterUnpacksIntoItsElementsAndThenItsBits) {
    for (const QString& name : {QStringLiteral("count_rec"), QStringLiteral("count_rec0"),
                                QStringLiteral("count_rec1")}) {
        const int row = model->addSignal(signals.value(name));
        model->unpack(row);
        ASSERT_EQ(model->rowCount(model->indexOfRow(row)), 4) << name.toStdString();

        const int element = childOf(row, 1);
        EXPECT_EQ(model->row(element)->label, name + QStringLiteral("[2]"));
        EXPECT_EQ(model->displayedWidth(element), 8U);
        EXPECT_EQ(model->renderValue(element, 5), QStringLiteral("03"));

        model->unpack(element);
        ASSERT_EQ(model->rowCount(model->indexOfRow(element)), 8) << name.toStdString();
        EXPECT_EQ(model->row(childOf(element, 6))->label, name + QStringLiteral("[2][1]"));
        EXPECT_EQ(model->renderValue(childOf(element, 6), 5), QStringLiteral("1"));

        // Its edges are its own: element 2 changes at 10, and only the bits that moved do.
        EXPECT_EQ(model->nextEdgeAfter(element, 5), 10U) << name.toStdString();
        EXPECT_EQ(model->nextEdgeAfter(childOf(row, 0), 5), enzan::runtime::NoTime)
            << name.toStdString();
    }
}
#endif

// ---------------------------------------------------------------------------
// The clipboard
// ---------------------------------------------------------------------------

TEST_F(WaveRowTest, ARowCopiesAndPastes) {
    const int original = model->addSignal(counter);
    model->setRadix(original, Radix::Binary);
    model->setHighlight(original, 3);

    model->copySignalRows({original});
    EXPECT_TRUE(WaveformModel::clipboardHasRows());

    const QVector<int> pasted = model->pasteSignalRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    const WaveRow* copy = model->row(pasted.front());
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->label, model->row(original)->label);
    EXPECT_EQ(copy->signalIndex, counter.signalIndex);
    EXPECT_EQ(copy->radix, Radix::Binary) << "a copy keeps how it was set up";
    EXPECT_EQ(copy->highlight, 3);
    EXPECT_EQ(model->rowCount(), 2);
}

TEST_F(WaveRowTest, CopyingAGroupCopiesWhatIsInIt) {
    const int group = model->addGroup(QStringLiteral("Bus"));
    model->addSignal(clock, group);
    model->addSignal(counter, group);

    model->copySignalRows({group});
    const QVector<int> pasted = model->pasteSignalRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    const int copiedGroup = pasted.front();
    ASSERT_NE(model->row(copiedGroup), nullptr);
    EXPECT_EQ(model->row(copiedGroup)->kind, WaveRow::Kind::Group);
    EXPECT_EQ(model->rowCount(model->indexOfRow(copiedGroup)), 2);
    EXPECT_EQ(model->rowCount(), 2) << "the copy sits beside the original, not inside it";
}

TEST_F(WaveRowTest, AnUnpackedBusCopiesWithTheBitsItHasLeft) {
    const int bus = model->addSignal(counter);
    model->unpack(bus);
    model->removeRow(childOf(bus, 4));

    model->copySignalRows({bus});
    const QVector<int> pasted = model->pasteSignalRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    EXPECT_EQ(model->rowCount(model->indexOfRow(pasted.front())), 7);
    EXPECT_EQ(model->renderValue(pasted.front(), 400), QStringLiteral("10"))
        << "a pasted row shows what the one it came from showed";
}

TEST_F(WaveRowTest, PastingIntoAGroupPutsTheRowsInside) {
    const int original = model->addSignal(clock);
    const int group = model->addGroup(QStringLiteral("Bus"));

    model->copySignalRows({original});
    const QVector<int> pasted = model->pasteSignalRows(group, -1);

    ASSERT_EQ(pasted.size(), 1);
    EXPECT_EQ(model->row(pasted.front())->parent, group);
}

TEST_F(WaveRowTest, PastingAtAPositionLandsThere) {
    const int first = model->addSignal(clock);
    model->addSignal(counter);

    model->copySignalRows({first});
    const QVector<int> pasted = model->pasteSignalRows(-1, 1);

    ASSERT_EQ(pasted.size(), 1);
    EXPECT_EQ(model->indexOfRow(pasted.front()).row(), 1)
        << "pasting beside the row you copied must not send it to the end";
}

TEST_F(WaveRowTest, PastingSubscribesToWhatArrived) {
    const int original = model->addSignal(counter);
    model->copySignalRows({original});
    model->removeRow(original);
    ASSERT_TRUE(model->displayedSignals().isEmpty());

    model->pasteSignalRows(-1, -1);
    EXPECT_EQ(model->displayedSignals(), QVector<std::uint32_t>{counter.signalIndex});
}

TEST_F(WaveRowTest, PastingKeepsTheRowsThatWereAlreadyThere) {
    // A paste used to reset the model, which made the view forget every expansion in the
    // window. The rows themselves have to survive it untouched.
    const int first = model->addSignal(counter);
    model->unpack(first);
    model->setHighlight(first, 2);

    model->copySignalRows({first});
    const QVector<int> pasted = model->pasteSignalRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    ASSERT_NE(model->row(first), nullptr);
    EXPECT_TRUE(model->row(first)->unpacked);
    EXPECT_EQ(model->rowCount(model->indexOfRow(first)), 8);
    EXPECT_EQ(model->row(first)->highlight, 2);
    EXPECT_EQ(model->indexOfRow(first).row(), 0) << "and stays where it was";
}

TEST_F(WaveRowTest, PastingAnEmptyClipboardDoesNothing) {
    QApplication::clipboard()->clear();
    QApplication::clipboard()->setText(QStringLiteral("not a wave row"));

    EXPECT_FALSE(WaveformModel::clipboardHasRows());
    EXPECT_TRUE(model->pasteSignalRows(-1, -1).isEmpty());
    EXPECT_EQ(model->rowCount(), 0);
}

TEST_F(WaveRowTest, CopyingAlsoLeavesThePathsAsText) {
    // So that the clipboard is worth something outside this window too.
    const int original = model->addSignal(counter);
    model->copySignalRows({original});

    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("/tb/count"));
}

// ---------------------------------------------------------------------------
// Adding a signal leaves the window as it was
// ---------------------------------------------------------------------------

/// The window itself, because this is about what the view does when rows arrive.
class WaveWindowTest : public SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();

        theme.loadBuiltIn();
        window = std::make_unique<MainWindow>(*session, theme);
        window->resize(1200, 700);
        window->show();
        pump(6);

        waveRows = window->findChildren<QTreeView*>().value(1);
        objects = window->findChild<QTableView*>();
        ASSERT_NE(waveRows, nullptr);
        ASSERT_NE(objects, nullptr);

        waveModel = qobject_cast<WaveformModel*>(waveRows->model());
        ASSERT_NE(waveModel, nullptr);
    }

    void TearDown() override {
        window = nullptr;
        SessionFixture::TearDown();
    }

    /// Selects one object by name in the Objects window and runs "Add to Wave".
    void addToWave(const QString& name) {
        for (int row = 0; row < objects->model()->rowCount(); ++row) {
            if (objects->model()->index(row, 0).data().toString() != name) continue;
            objects->selectionModel()->clearSelection();
            objects->selectRow(row);
        }

        for (QAction* action : objects->actions()) {
            if (action->text().contains(QStringLiteral("Add to Wave"))) action->trigger();
        }
        pump(2);
    }

    /// Triggers the wave list's action whose text contains `name`. Returns false when the
    /// window has no such action, which is how a test says a menu entry is missing.
    bool triggerWaveAction(const QString& name) {
        for (QAction* action : waveRows->actions()) {
            if (!action->text().contains(name)) continue;
            action->trigger();
            pump(2);
            return true;
        }
        return false;
    }

    Theme theme;
    std::unique_ptr<MainWindow> window;
    QTreeView* waveRows = nullptr;
    QTableView* objects = nullptr;
    WaveformModel* waveModel = nullptr;
};

// ---------------------------------------------------------------------------
// The row's own menu
// ---------------------------------------------------------------------------

TEST_F(WaveWindowTest, PackAndUnpackAreOneEntryThatToggles) {
    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 1);

    const int bus = waveModel->rowIdOf(waveModel->index(0, 0));
    waveRows->setCurrentIndex(waveModel->indexOfRow(bus));
    ASSERT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(bus)));

    // Unpacking through the menu is the same act as the disclosure arrow, so it is what
    // creates the bits the first time round.
    ASSERT_TRUE(triggerWaveAction(QStringLiteral("Pack/Unpack")));
    EXPECT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(bus)));
    EXPECT_EQ(waveModel->rowCount(waveModel->indexOfRow(bus)), 8);

    ASSERT_TRUE(triggerWaveAction(QStringLiteral("Pack/Unpack")));
    EXPECT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(bus)))
        << "one entry has to pack what it unpacked";
}

TEST_F(WaveWindowTest, RenameOpensTheEditorOnTheName) {
    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 1);

    // Deliberately on the value column, which is not editable: the action has to move to the
    // name whichever column the reader was on, rather than quietly doing nothing.
    const int bus = waveModel->rowIdOf(waveModel->index(0, 0));
    waveRows->setCurrentIndex(waveModel->index(0, WaveformModel::ValueColumn));

    ASSERT_TRUE(triggerWaveAction(QStringLiteral("Rename")));

    // An editor appearing at all is the assertion that it moved: the value column is not
    // editable, so an action that stayed where the reader was would have opened nothing.
    EXPECT_NE(waveRows->viewport()->findChild<QLineEdit*>(), nullptr)
        << "the name editor has to be open";

    // And the editor edits the label, which is what `setData` is for.
    const QModelIndex rowIndex = waveModel->indexOfRow(bus);
    ASSERT_TRUE(waveModel->setData(rowIndex, QStringLiteral("tally"), Qt::EditRole));
    EXPECT_EQ(waveModel->indexOfRow(bus).data().toString(), QStringLiteral("tally"));
}

TEST_F(WaveWindowTest, PasteGoesAfterTheCurrentRowAtItsLevel) {
    // After a group as after anything else, rather than into it: pasting into a group left no
    // way to paste after one, and dragging is what puts a row inside a group.
    addToWave(QStringLiteral("clk"));
    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 2);
    const int clock = waveModel->rowIdOf(waveModel->index(0, 0));
    const int counter = waveModel->rowIdOf(waveModel->index(1, 0));
    const int group = waveModel->addGroup(QStringLiteral("g"));
    ASSERT_TRUE(waveModel->moveRowsTo({counter}, group, 0));
    ASSERT_EQ(waveModel->rowCount(), 2) << "the clock, then the group holding the counter";

    waveModel->copySignalRows({clock});
    waveRows->setCurrentIndex(waveModel->indexOfRow(group));
    ASSERT_TRUE(triggerWaveAction(QStringLiteral("Paste")));

    ASSERT_EQ(waveModel->rowCount(), 3);
    EXPECT_EQ(waveModel->rowIdOf(waveModel->index(1, 0)), group);
    EXPECT_EQ(waveModel->index(2, 0).data().toString(), QStringLiteral("clk"))
        << "the copy belongs after the group";
    EXPECT_EQ(waveModel->rowCount(waveModel->indexOfRow(group)), 1) << "and not inside it";

    // Inside a group, after the row that is current there.
    waveRows->setCurrentIndex(waveModel->indexOfRow(counter));
    ASSERT_TRUE(triggerWaveAction(QStringLiteral("Paste")));
    const QModelIndex inside = waveModel->indexOfRow(group);
    ASSERT_EQ(waveModel->rowCount(inside), 2);
    EXPECT_EQ(waveModel->index(1, 0, inside).data().toString(), QStringLiteral("clk"));
}

TEST_F(WaveWindowTest, TheBitRangeDialogIsGone) {
    // A range is reached by unpacking and removing what is not wanted, not by
    // typing `high:low` into a box. The entry that did is not there to be found.
    addToWave(QStringLiteral("count"));
    EXPECT_FALSE(triggerWaveAction(QStringLiteral("Extract")));
}

TEST_F(WaveWindowTest, ANewSignalArrivesPackedAndLeavesTheOthersAlone) {
    // Adding a signal used to expand the whole window, which unpacked every bus in it and threw
    // away whatever the reader had chosen to have open.
    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 1);

    const int bus = waveModel->rowIdOf(waveModel->index(0, 0));
    EXPECT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(bus))) << "a signal arrives packed";

    waveRows->expand(waveModel->indexOfRow(bus));
    pump(2);
    ASSERT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(bus)));
    ASSERT_EQ(waveModel->rowCount(waveModel->indexOfRow(bus)), 8);

    addToWave(QStringLiteral("clk"));

    ASSERT_EQ(waveModel->rowCount(), 2);
    EXPECT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(bus)))
        << "what was already open must stay open";

    const int added = waveModel->rowIdOf(waveModel->index(1, 0));
    EXPECT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(added)));
}

TEST_F(WaveWindowTest, MakingAGroupLeavesTheRestOfTheWindowOpen) {
    // Creating a group used to reset the model, which packed every bus in the window.
    addToWave(QStringLiteral("count"));
    addToWave(QStringLiteral("clk"));
    ASSERT_EQ(waveModel->rowCount(), 2);

    const int bus = waveModel->rowIdOf(waveModel->index(0, 0));
    waveRows->expand(waveModel->indexOfRow(bus));
    pump(2);
    ASSERT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(bus)));

    // Group the clock, leaving the bus where it is.
    const int clockRow = waveModel->rowIdOf(waveModel->index(1, 0));
    const int group = waveModel->addGroup(QStringLiteral("Bus"));
    waveModel->moveToGroup(clockRow, group);
    pump(2);

    EXPECT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(bus)))
        << "making a group must not pack what was already open";
}

TEST_F(WaveWindowTest, ASecondBusArrivesPackedWhileTheFirstStaysOpen) {
    addToWave(QStringLiteral("count"));
    const int first = waveModel->rowIdOf(waveModel->index(0, 0));
    waveRows->expand(waveModel->indexOfRow(first));
    pump(2);

    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 2);

    const int second = waveModel->rowIdOf(waveModel->index(1, 0));
    EXPECT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(first)));
    EXPECT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(second)));
    EXPECT_FALSE(waveModel->row(second)->unpacked)
        << "the second copy of a bus must not inherit the first one's unpacking";
}

// ---------------------------------------------------------------------------
// Whose keys they are
// ---------------------------------------------------------------------------

TEST_F(WaveWindowTest, TheWaveListsKeysActOnlyInTheWaveWindow) {
    // Delete in the Structure window removed whatever was selected in the wave list, and Ctrl+C in
    // the Objects window copied wave rows: a key given to one pane was a key of the whole window.
    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 1);
    waveRows->setCurrentIndex(waveModel->index(0, 0));

    window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));

    QTreeView* structure = nullptr;
    for (const QDockWidget* dock : window->findChildren<QDockWidget*>()) {
        if (dock->objectName() == QStringLiteral("structure"))
            structure = qobject_cast<QTreeView*>(dock->widget());
    }
    ASSERT_NE(structure, nullptr);

    structure->setFocus();
    QTest::keyClick(structure, Qt::Key_Delete);
    EXPECT_EQ(waveModel->rowCount(), 1) << "Delete in the Structure window removed a wave row";

    QApplication::clipboard()->clear();
    objects->setFocus();
    QTest::keyClick(objects, Qt::Key_C, Qt::ControlModifier);
    EXPECT_FALSE(WaveformModel::clipboardHasRows())
        << "Ctrl+C in the Objects window copied wave rows";

    // The traces are as much the wave window as the names are, so there the key does act.
    auto* canvas = window->findChild<enzan::gui::WaveformView*>();
    ASSERT_NE(canvas, nullptr);
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_Delete);
    EXPECT_EQ(waveModel->rowCount(), 0) << "Delete on the traces removes the selected row";
}

TEST_F(WaveWindowTest, TheWaveKeysLeaveTheListsTypingAlone) {
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDelete)
    // The wave window's own keys carry a modifier or are punctuation, so that a letter typed in
    // the list still finds a row by its name, and they act only in the Wave tab.
    addToWave(QStringLiteral("clk"));
    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 2);

    window->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));
    auto* canvas = window->findChild<enzan::gui::WaveformView*>();
    ASSERT_NE(canvas, nullptr);

    waveRows->setFocus();
    waveRows->setCurrentIndex(waveModel->index(0, 0));
    QTest::keyClicks(waveRows, QStringLiteral("co"));
    EXPECT_EQ(waveRows->currentIndex().data().toString(), QStringLiteral("count"));

    const qsizetype cursors = canvas->cursors().size();
    objects->setFocus();
    QTest::keyClick(objects, Qt::Key_Insert);
    EXPECT_EQ(canvas->cursors().size(), cursors) << "Insert outside the Wave tab added a cursor";

    waveRows->setFocus();
    QTest::keyClick(waveRows, Qt::Key_Insert);
    EXPECT_EQ(canvas->cursors().size(), cursors + 1) << "Insert in the wave list adds a cursor";
    // NOLINTEND(clang-analyzer-cplusplus.NewDelete)
}

TEST_F(WaveWindowTest, TheEditMenuIsTheWaveListsMenu) {
    // One set of actions in one order, so that the two menus cannot come to differ.
    const QMenu* edit = nullptr;
    for (const QAction* entry : window->menuBar()->actions()) {
        if (entry->text() == QStringLiteral("&Edit")) edit = entry->menu();
    }
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->actions(), waveRows->actions());
}

// ---------------------------------------------------------------------------
// Finding a value from the toolbar
// ---------------------------------------------------------------------------

class FindFieldTest : public WaveWindowTest {
protected:
    void SetUp() override {
        WaveWindowTest::SetUp();

        // The counter in the window, with a run behind it: it takes each value v at 10v - 5.
        addToWave(QStringLiteral("count"));
        ASSERT_EQ(waveModel->rowCount(), 1);
        session->run(400);
        ASSERT_TRUE(pumpUntilCaughtUp(400));
        waveRows->setCurrentIndex(waveModel->index(0, 0));

        canvas = window->findChild<enzan::gui::WaveformView*>();
        field = window->findChild<QLineEdit*>(QStringLiteral("findValue"));
        ASSERT_NE(canvas, nullptr);
        ASSERT_NE(field, nullptr);
        canvas->setCursorTime(0, 0);

        window->activateWindow();
        ASSERT_TRUE(QTest::qWaitForWindowActive(window.get()));
    }

    /// Presses and releases `key` on `widget`, and lets go of the modifiers afterwards.
    ///
    /// A key with a modifier leaves the modifier held in the application's state, whether QTest
    /// sends it or a test does, since nothing sends the modifier's own release as a keyboard
    /// would. Every selection the process made afterwards would then extend a range rather than
    /// select a row, so the release is sent here.
    static void press(QWidget* widget, Qt::Key key,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(widget, key, modifiers);
        if (modifiers.testFlag(Qt::ShiftModifier)) QTest::keyRelease(widget, Qt::Key_Shift);
    }

    enzan::gui::WaveformView* canvas = nullptr;
    QLineEdit* field = nullptr;
};

TEST_F(FindFieldTest, F3FindsTheTypedValueWhereverTheFocusIs) {
    field->setText(QStringLiteral("05"));

    // From the signal list, where a key without a modifier is the list's own: F3 types nothing.
    waveRows->setFocus();
    press(waveRows, Qt::Key_F3);
    EXPECT_EQ(canvas->activeCursorTime(), 45U);
    EXPECT_TRUE(window->statusBar()->currentMessage().startsWith(QStringLiteral("count shows 05")))
        << window->statusBar()->currentMessage().toStdString();

    canvas->setCursorTime(0, 100);
    objects->setFocus();
    press(objects, Qt::Key_F3, Qt::ShiftModifier);
    EXPECT_EQ(canvas->activeCursorTime(), 45U);
}

TEST_F(FindFieldTest, EnterInTheFieldFindsTheNextAndShiftEnterThePrevious) {
    field->setFocus();
    QTest::keyClicks(field, QStringLiteral("05"));

    press(field, Qt::Key_Return);
    EXPECT_EQ(canvas->activeCursorTime(), 45U);
    EXPECT_TRUE(field->hasFocus()) << "finding again is another Enter away";

    canvas->setCursorTime(0, 100);
    press(field, Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(canvas->activeCursorTime(), 45U);
}

TEST_F(FindFieldTest, F3WithNothingTypedGoesToTheField) {
    waveRows->setFocus();
    press(waveRows, Qt::Key_F3);

    EXPECT_TRUE(field->hasFocus());
    EXPECT_EQ(canvas->activeCursorTime(), 0U);
}

TEST_F(FindFieldTest, EscapeInTheFieldGoesBackToTheWaves) {
    field->setFocus();
    press(field, Qt::Key_Escape);
    EXPECT_TRUE(waveRows->hasFocus());
}

TEST_F(FindFieldTest, WhatCannotBeFoundIsSaidInTheStatusBar) {
    field->setText(QStringLiteral("1ff"));
    waveRows->setFocus();
    press(waveRows, Qt::Key_F3);

    EXPECT_EQ(canvas->activeCursorTime(), 0U);
    EXPECT_EQ(window->statusBar()->currentMessage(),
              QStringLiteral("'1ff' does not fit in 8 bits"));
}

TEST_F(FindFieldTest, ARestartAbandonsASearchStillRunning) {
    // The values it was searching are gone, and going on through what replaces them would answer
    // for a run that no longer exists.
    auto* finder = window->findChild<enzan::gui::ValueFinder*>();
    ASSERT_NE(finder, nullptr);
    finder->setSlice(1, std::chrono::milliseconds(0));

    field->setText(QStringLiteral("27"));
    waveRows->setFocus();
    press(waveRows, Qt::Key_F3);
    ASSERT_TRUE(finder->searching());
    EXPECT_TRUE(window->statusBar()->currentMessage().startsWith(QStringLiteral("Searching")))
        << window->statusBar()->currentMessage().toStdString();

    Q_EMIT session->wavesCleared();
    EXPECT_FALSE(finder->searching());
    EXPECT_TRUE(window->statusBar()->currentMessage().isEmpty())
        << window->statusBar()->currentMessage().toStdString();
    EXPECT_EQ(canvas->activeCursorTime(), 0U);
}

TEST_F(FindFieldTest, TheCursorMenuOffersFindingWithItsKeys) {
    const QMenu* cursor = nullptr;
    for (const QAction* entry : window->menuBar()->actions()) {
        if (entry->text() == QStringLiteral("&Cursor")) cursor = entry->menu();
    }
    ASSERT_NE(cursor, nullptr);

    QHash<QString, QKeySequence> keys;
    for (const QAction* action : cursor->actions()) keys.insert(action->text(), action->shortcut());
    EXPECT_EQ(keys.value(QStringLiteral("Find Next")), QKeySequence(Qt::Key_F3));
    EXPECT_EQ(keys.value(QStringLiteral("Find Previous")), QKeySequence(Qt::SHIFT | Qt::Key_F3));
}

} // namespace
