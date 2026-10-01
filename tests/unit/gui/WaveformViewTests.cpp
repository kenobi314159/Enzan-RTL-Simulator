// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the wave window: the model that decides what is shown, and the canvas that paints
// it.
//
// The canvas is tested by rendering it offscreen and looking at the pixels. That is worth the
// awkwardness: the thing most likely to be wrong in a waveform renderer is what it draws, and a
// test that only exercised the code paths would pass with a blank widget.

#include "gui/WaveformView.h"

#include "gui/DesignTreeModel.h"
#include "gui/Theme.h"
#include "gui/WaveformModel.h"
#include "runtime/Time.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QImage>
#include <QPixmap>
#include <QScrollBar>
#include <QTreeView>
#include <cstdio>

#include <map>
#include <optional>

#include "eir/BitVector.h"

namespace {

using enzan::eir::Radix;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveformView;
using enzan::gui::WaveRow;
using enzan::test::SessionFixture;

/// How often each colour appears in an image, so that a test can ask what was drawn.
std::map<QRgb, int> colourHistogram(const QImage& image) {
    std::map<QRgb, int> counts;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) counts[image.pixel(x, y)] += 1;
    }
    return counts;
}

int countOf(const std::map<QRgb, int>& counts, const QColor& colour) {
    const auto found = counts.find(colour.rgb());
    return found == counts.end() ? 0 : found->second;
}

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------

class WaveformModelTest : public SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();
        theme.loadBuiltIn();
        model = std::make_unique<WaveformModel>(*session, theme);
    }

    SignalInfo infoFor(const QString& scope, const QString& name) {
        for (const SignalInfo& info : objectsOf(scope)) {
            if (info.name == name) return info;
        }
        ADD_FAILURE() << "no signal " << name.toStdString() << " in " << scope.toStdString();
        return {};
    }

    Theme theme;
    std::unique_ptr<WaveformModel> model;
};

TEST_F(WaveformModelTest, AddedSignalsBecomeRowsAndAreSubscribedTo) {
    const SignalInfo clock = infoFor(QStringLiteral("/tb"), QStringLiteral("clk"));
    const int rowId = model->addSignal(clock);

    ASSERT_NE(model->row(rowId), nullptr);
    EXPECT_EQ(model->row(rowId)->label, QStringLiteral("clk"));
    EXPECT_EQ(model->rowCount(), 1);
    EXPECT_EQ(model->displayedSignals(), QVector<std::uint32_t>{clock.signalIndex});
}

TEST_F(WaveformModelTest, GroupingNestsRowsAndKeepsThemDisplayed) {
    const SignalInfo clock = infoFor(QStringLiteral("/tb"), QStringLiteral("clk"));
    const SignalInfo count = infoFor(QStringLiteral("/tb"), QStringLiteral("count"));

    const int group = model->addGroup(QStringLiteral("Bus"));
    const int first = model->addSignal(clock);
    const int second = model->addSignal(count);
    model->moveToGroup(first, group);
    model->moveToGroup(second, group);

    EXPECT_EQ(model->rowCount(), 1) << "the group is the only top-level row";
    EXPECT_EQ(model->rowCount(model->indexOfRow(group)), 2);
    EXPECT_EQ(model->displayedSignals().size(), 2);
}

TEST_F(WaveformModelTest, AGroupKeepsItsChildrenAsTheRowListGrows) {
    // The rows live in one vector, and adding to it reallocates. A parent's child list has to
    // be reached after the new row is stored, not through a reference taken before it: the
    // reference is dangling by then, and the children go into freed memory.
    const SignalInfo clock = infoFor(QStringLiteral("/tb"), QStringLiteral("clk"));
    const int group = model->addGroup(QStringLiteral("Bus"));

    constexpr int Enough = 64;
    for (int added = 0; added < Enough; ++added) model->addSignal(clock, group);

    EXPECT_EQ(model->rowCount(model->indexOfRow(group)), Enough);
    EXPECT_EQ(model->rowCount(), 1) << "every row went into the group, not beside it";

    for (int child = 0; child < Enough; ++child) {
        const int rowId = model->rowIdOf(model->index(child, 0, model->indexOfRow(group)));
        ASSERT_NE(model->row(rowId), nullptr) << "child " << child;
        EXPECT_EQ(model->row(rowId)->parent, group) << "child " << child;
    }
}

TEST_F(WaveformModelTest, AGroupCannotBeMovedInsideItself) {
    // Which would make the tree a cycle and the view an infinite recursion.
    const int outer = model->addGroup(QStringLiteral("Outer"));
    const int inner = model->addGroup(QStringLiteral("Inner"));
    model->moveToGroup(inner, outer);

    model->moveToGroup(outer, inner);

    EXPECT_EQ(model->row(outer)->parent, -1);
    EXPECT_EQ(model->row(inner)->parent, outer);
}

TEST_F(WaveformModelTest, RemovingAGroupRemovesWhatIsInIt) {
    const SignalInfo clock = infoFor(QStringLiteral("/tb"), QStringLiteral("clk"));
    const int group = model->addGroup(QStringLiteral("Bus"));
    const int child = model->addSignal(clock);
    model->moveToGroup(child, group);

    model->removeRow(group);

    EXPECT_EQ(model->rowCount(), 0);
    EXPECT_EQ(model->row(child), nullptr);
    EXPECT_TRUE(model->displayedSignals().isEmpty());
}

TEST_F(WaveformModelTest, ABitRangeBecomesItsOwnRowOfTheSameSignal) {
    // Section 10.3 requires extracting a bit range into a separately displayed signal. It is a
    // property of the row, so the kernel still streams the signal once.
    const SignalInfo count = infoFor(QStringLiteral("/tb"), QStringLiteral("count"));
    const int whole = model->addSignal(count);

    const int slice = model->addSlice(whole, 3, 0);
    ASSERT_GE(slice, 0);

    EXPECT_EQ(model->row(slice)->label, QStringLiteral("count[3:0]"));
    EXPECT_TRUE(model->row(slice)->sliced);
    EXPECT_EQ(model->displayedSignals().size(), 1) << "one signal, two rows";

    // Placed beside the row it came from, which is where the user is looking.
    EXPECT_EQ(model->indexOfRow(slice).row(), model->indexOfRow(whole).row() + 1);
}

TEST_F(WaveformModelTest, ABitRangeOutsideTheSignalIsRefused) {
    const SignalInfo count = infoFor(QStringLiteral("/tb"), QStringLiteral("count"));
    const int whole = model->addSignal(count);

    EXPECT_LT(model->addSlice(whole, 99, 0), 0);
    EXPECT_EQ(model->rowCount(), 1);
}

TEST_F(WaveformModelTest, ASlicedRowShowsOnlyItsBits) {
    const SignalInfo count = infoFor(QStringLiteral("/tb"), QStringLiteral("count"));
    const int whole = model->addSignal(count);
    const int high = model->addSlice(whole, 7, 4);
    ASSERT_GE(high, 0);

    session->subscribe({count.signalIndex});
    session->run(400);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 400; }));

    // Forty rising edges have passed, so the counter holds 40 — 0x28. The sliced row shows
    // only its top four bits, which is 2.
    EXPECT_EQ(model->valueAt(whole, 400), std::optional(enzan::eir::BitVector::fromUInt64(40, 8)));
    EXPECT_EQ(model->valueAt(high, 400), std::optional(enzan::eir::BitVector::fromUInt64(2, 4)));
}

TEST_F(WaveformModelTest, RadixIsPerRowAndDefaultsToHexadecimal) {
    // Section 10.3: multi-bit values display in hexadecimal by default.
    const SignalInfo count = infoFor(QStringLiteral("/tb"), QStringLiteral("count"));
    const int rowId = model->addSignal(count);

    EXPECT_EQ(model->row(rowId)->radix, Radix::Hexadecimal);

    session->subscribe({count.signalIndex});
    session->run(100);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 100; }));

    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("0a"));

    model->setRadix(rowId, Radix::Decimal);
    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("10"));

    model->setRadix(rowId, Radix::Binary);
    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("00001010"));
}

TEST_F(WaveformModelTest, SettingAGroupsRadixSetsItsContents) {
    const SignalInfo count = infoFor(QStringLiteral("/tb"), QStringLiteral("count"));
    const int group = model->addGroup(QStringLiteral("Bus"));
    const int child = model->addSignal(count);
    model->moveToGroup(child, group);

    model->setRadixRecursive(group, Radix::Binary);
    EXPECT_EQ(model->row(child)->radix, Radix::Binary);
}

TEST_F(WaveformModelTest, ARowWithNoDataShowsADashRatherThanAWrongValue) {
    const SignalInfo count = infoFor(QStringLiteral("/tb"), QStringLiteral("count"));
    const int rowId = model->addSignal(count);

    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("—"));
}

// ---------------------------------------------------------------------------
// Enumerated signals
// ---------------------------------------------------------------------------
//
// Section 10.7: a row shows the name its type gives a value, because `S_SEND` is what the
// reader wrote and `3'h2` is what the compiler made of it. The fixture's `state` sits at
// `S_SEND` for the whole run, so every time reads the same and the tests are about the
// rendering rather than about when it was sampled.

class EnumeratedRowTest : public WaveformModelTest {
protected:
    /// Adds `state` to the window and runs far enough for it to have a value.
    int addState() {
        const SignalInfo state = infoFor(QStringLiteral("/tb"), QStringLiteral("state"));
        const int rowId = model->addSignal(state);
        session->subscribe({state.signalIndex});
        session->run(200);
        EXPECT_TRUE(pumpUntil([this] { return session->currentTime() == 200; }));
        return rowId;
    }
};

TEST_F(EnumeratedRowTest, ShowsTheNameOfTheValueRatherThanTheBits) {
    const int rowId = addState();

    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("S_SEND"));
    EXPECT_NE(model->enumerationOf(rowId), nullptr);
}

TEST_F(EnumeratedRowTest, AskingForARadixShowsTheNumber) {
    const int rowId = addState();
    ASSERT_EQ(model->renderValue(rowId, 100), QStringLiteral("S_SEND"));

    // A radix menu that silently did nothing on a state register would read as a fault rather
    // than as a choice, so choosing one turns the naming off.
    model->setRadixRecursive(rowId, Radix::Binary);
    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("010"));
    EXPECT_EQ(model->enumerationOf(rowId), nullptr);

    model->setSymbolicRecursive(rowId, true);
    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("S_SEND"))
        << "and there is a way back";
}

TEST_F(EnumeratedRowTest, OneBitOfAnEnumeratedSignalIsANumber) {
    const int rowId = addState();
    model->unpack(rowId);

    // The bus still shows its name: unpacking does not change what it holds, only what is
    // listed under it.
    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("S_SEND"));

    // Its bits do not. An enumeration names whole values, and bit 1 of `S_SEND` is not a state.
    const WaveRow* row = model->row(rowId);
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->children.size(), 3);
    EXPECT_EQ(model->enumerationOf(row->children.front()), nullptr);
    EXPECT_EQ(model->renderValue(row->children.front(), 100), QStringLiteral("0"))
        << "bit 2 of 3'b010";
    EXPECT_EQ(model->renderValue(row->children[1], 100), QStringLiteral("1"));
}

TEST_F(EnumeratedRowTest, ABusThatHasLostABitIsNoLongerNamed) {
    const int rowId = addState();
    model->unpack(rowId);

    const WaveRow* row = model->row(rowId);
    ASSERT_NE(row, nullptr);
    model->removeRow(row->children.front());

    // What is left is two of the three bits, which no enumeration describes. Naming it would
    // be naming a different value than the one on screen.
    EXPECT_EQ(model->enumerationOf(rowId), nullptr);
    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("2")) << "2'b10 in hexadecimal";
}

TEST_F(EnumeratedRowTest, AGroupHoldingItIsNotNamed) {
    const int rowId = addState();
    const int group = model->addGroup(QStringLiteral("Machine"));
    ASSERT_TRUE(model->moveRowsTo({rowId}, group, -1));

    // The row inside keeps its name; the group is a join of whatever is in it, which is not
    // the enumerated type.
    EXPECT_EQ(model->renderValue(rowId, 100), QStringLiteral("S_SEND"));
    EXPECT_EQ(model->enumerationOf(group), nullptr);
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
//
// A parameter is a constant the elaborated design settled on, not a piece of its state. It has
// no slot, nothing to subscribe to and the same value at every time, and section 10.7 has it
// show that value across the whole window — including the part the kernel has not reached,
// where a trace would stop.

class ParameterRowTest : public WaveformModelTest {
protected:
    /// The child scope's `WIDTH` parameter, which is 4.
    SignalInfo width() { return infoFor(QStringLiteral("/tb/u_dut"), QStringLiteral("WIDTH")); }
};

TEST_F(ParameterRowTest, ShowsItsConstantValueAtEveryTime) {
    const int rowId = model->addSignal(width());

    const WaveRow* row = model->row(rowId);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->kind, WaveRow::Kind::Parameter);

    // Before any run at all, and at any time after it. A signal would show a dash here.
    EXPECT_EQ(model->renderValue(rowId, 0), QStringLiteral("00000004"));
    EXPECT_EQ(model->renderValue(rowId, 1'000'000), QStringLiteral("00000004"));

    model->setRadixRecursive(rowId, Radix::Decimal);
    EXPECT_EQ(model->renderValue(rowId, 0), QStringLiteral("4"));
}

TEST_F(ParameterRowTest, IsSubscribedToNothingAndCannotBeTakenApart) {
    const int rowId = model->addSignal(width());

    EXPECT_TRUE(model->displayedSignals().isEmpty())
        << "a parameter has no signal index, so there is nothing to ask the kernel for";

    // Immutable: there is nothing behind it to unpack into, and a `+` that did nothing would
    // be worse than no `+`.
    EXPECT_FALSE(model->canUnpack(rowId));
    EXPECT_FALSE(model->hasChildren(model->indexOfRow(rowId)));
    EXPECT_FALSE(model->showsChildren(rowId));
}

TEST_F(ParameterRowTest, NeverHasAnEdgeToStepTo) {
    const int rowId = model->addSignal(width());

    // Tab walks the active cursor from edge to edge, and a constant has none. Answering with a
    // time anyway would move the cursor somewhere arbitrary.
    EXPECT_EQ(model->nextEdgeAfter(rowId, 0), enzan::runtime::NoTime);
    EXPECT_EQ(model->previousEdgeBefore(rowId, 1000), enzan::runtime::NoTime);
    EXPECT_EQ(model->nearestEdge(rowId, 500), enzan::runtime::NoTime);
}

TEST_F(ParameterRowTest, SurvivesTheClipboardWithItsValue) {
    const int rowId = model->addSignal(width());
    model->copyRows({rowId});

    const QVector<int> pasted = model->pasteRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    // The value travels with it: there is nowhere for the other end to look it up, since a
    // parameter has no signal index.
    const WaveRow* copy = model->row(pasted.front());
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->kind, WaveRow::Kind::Parameter);
    EXPECT_EQ(model->renderValue(pasted.front(), 0), QStringLiteral("00000004"));
}

TEST_F(ParameterRowTest, IsDrawnBeforeTheKernelHasRunAtAll) {
    // The clip that stops a trace at the recorded extent would hide a parameter completely at
    // time zero, and "nothing has happened here yet" is not true of one.
    const int rowId = model->addSignal(width());
    ASSERT_GE(rowId, 0);

    WaveformView view(*session, *model, theme);
    QTreeView tree;
    tree.setModel(model.get());
    tree.setUniformRowHeights(true);
    tree.resize(200, 200);
    view.setRowView(&tree);
    view.resize(600, 200);
    tree.show();
    view.show();
    QCoreApplication::processEvents();

    ASSERT_EQ(session->latestTime(), 0U) << "nothing has been run";

    const std::map<QRgb, int> counts =
        colourHistogram(view.grab().toImage().convertToFormat(QImage::Format_RGB32));
    EXPECT_GT(countOf(counts, theme.color(enzan::gui::ThemeColor::WaveTrace)), 20)
        << "the parameter has to be drawn across a window the design has not been run in";
}

// ---------------------------------------------------------------------------
// The canvas
// ---------------------------------------------------------------------------

class WaveformViewTest : public SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();

        theme.loadBuiltIn();
        model = std::make_unique<WaveformModel>(*session, theme);

        treeView = std::make_unique<QTreeView>();
        treeView->setModel(model.get());
        treeView->setUniformRowHeights(true);
        treeView->resize(200, 400);

        view = std::make_unique<WaveformView>(*session, *model, theme);
        view->setRowView(treeView.get());
        view->resize(800, 400);

        // Offscreen widgets still have to be shown for their layouts to settle, which is what
        // gives the tree the row geometry the canvas reads.
        treeView->show();
        view->show();
        QCoreApplication::processEvents();
    }

    void TearDown() override {
        // Assigned rather than reset: `QTreeView` has a `reset()` of its own, and
        // `treeView.reset()` reads as though it might be calling that one.
        view = nullptr;
        treeView = nullptr;
        model = nullptr;
        SessionFixture::TearDown();
    }

    /// Adds every signal of `/tb` to the wave window and runs for `duration`.
    void showEverythingAndRun(std::uint64_t duration) {
        QVector<std::uint32_t> wanted;
        for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
            model->addSignal(info);
            wanted.push_back(info.signalIndex);
        }
        session->subscribe(wanted);

        session->run(duration);
        ASSERT_TRUE(pumpUntil([this, duration] { return session->currentTime() == duration; }));

        view->zoomFit();
        treeView->expandAll();
        QCoreApplication::processEvents();
    }

    [[nodiscard]] QImage render() {
        QCoreApplication::processEvents();
        return view->grab().toImage().convertToFormat(QImage::Format_RGB32);
    }

    Theme theme;
    std::unique_ptr<WaveformModel> model;
    std::unique_ptr<QTreeView> treeView;
    std::unique_ptr<WaveformView> view;
};

TEST_F(WaveformViewTest, AnEmptyWindowIsThemeBackgroundAndNothingElse) {
    const QImage rendered = render();
    ASSERT_FALSE(rendered.isNull());

    const std::map<QRgb, int> counts = colourHistogram(rendered);
    EXPECT_EQ(countOf(counts, theme.color(enzan::gui::ThemeColor::WaveTrace)), 0)
        << "a trace was drawn with nothing in the window";
}

TEST_F(WaveformViewTest, TracesArePaintedInTheThemeColour) {
    showEverythingAndRun(200);

    const QImage rendered = render();
    ASSERT_FALSE(rendered.isNull());

    const std::map<QRgb, int> counts = colourHistogram(rendered);

    // Section 10.5 forbids colour literals in widget code, so the colour on screen has to be
    // the one the theme file names — which is exactly what this asserts.
    EXPECT_GT(countOf(counts, theme.color(enzan::gui::ThemeColor::WaveTrace)), 200)
        << "the traces were not drawn in the theme's trace colour";
    EXPECT_GT(countOf(counts, theme.color(enzan::gui::ThemeColor::WaveRulerBackground)), 100)
        << "the ruler was not drawn";
}

TEST_F(WaveformViewTest, AConstantBitOfABusyBusIsDrawnAsALevelAndNotAsABusyBar) {
    // The complaint this pins: zoomed out far enough that a pixel column spans several changes
    // of an unpacked bus, every one of its bits was filled as busy — because the row was
    // summarised over the whole signal rather than over the bit it shows. A bit that never moved
    // was a solid bar across the window, and only zooming in until each column held one value
    // resolved it into the flat line it always was.
    //
    // A narrow canvas, so that a run short enough to keep the counter's top bit at zero still
    // puts several changes in every column. The artifact needs columns to be busy; that is the
    // whole of what "zoomed out" means here.
    view->resize(100, 400);

    QVector<std::uint32_t> wanted;
    SignalInfo count;
    for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
        if (info.name == QStringLiteral("count")) count = info;
        wanted.push_back(info.signalIndex);
    }
    ASSERT_EQ(count.width, 8U) << "the fixture's counter is not the width this test assumes";
    session->subscribe(wanted);

    // 120 increments: the counter reaches 120, so bit 7 never leaves zero while bit 0 moves on
    // every one of them.
    constexpr std::uint64_t Duration = 1200;
    session->run(Duration);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == Duration; }));

    /// Draws one row showing bits [high:low] of the counter, and counts the trace-coloured
    /// pixels. One row at a time so that the two answers are measured at the same geometry.
    const auto tracePixelsOfBit = [&](std::uint32_t bit) {
        model->clear();
        const int bus = model->addSignal(count);
        const int slice = model->addSlice(bus, bit, bit);
        EXPECT_NE(slice, -1);
        model->removeRow(bus);

        view->zoomFit();
        treeView->expandAll();
        QCoreApplication::processEvents();

        return countOf(colourHistogram(render()), theme.color(enzan::gui::ThemeColor::WaveTrace));
    };

    const int busy = tracePixelsOfBit(0);
    const int constant = tracePixelsOfBit(7);

    // A level is a line one pixel high; a busy column is filled for the row's whole height. The
    // two differ by the row height, so an order of magnitude is a safe way to say "a line, not a
    // block" without depending on the font the row height comes from.
    EXPECT_GT(busy, 0) << "the bit that changes every count should be busy";
    EXPECT_LT(constant * 4, busy) << "a constant bit was filled like a busy one: " << constant
                                  << " against " << busy;
}

TEST_F(WaveformViewTest, TheActiveCursorIsPainted) {
    showEverythingAndRun(200);

    view->setCursorTime(0, 100);
    const QImage rendered = render();

    const std::map<QRgb, int> counts = colourHistogram(rendered);
    EXPECT_GT(countOf(counts, theme.color(enzan::gui::ThemeColor::WaveCursor)), 20);
}

TEST_F(WaveformViewTest, ALockedCursorIsPaintedDifferently) {
    // A reader has to be able to tell which cursor a click will move, or they find out by
    // moving the wrong one.
    showEverythingAndRun(200);

    view->addCursor(QStringLiteral("B"), 150);
    view->setCursorLocked(1, true);

    const std::map<QRgb, int> counts = colourHistogram(render());
    EXPECT_GT(countOf(counts, theme.color(enzan::gui::ThemeColor::WaveCursorLocked)), 20);
}

TEST_F(WaveformViewTest, ALockedCursorDoesNotFollowAClick) {
    showEverythingAndRun(200);

    view->setCursorLocked(0, true);
    const enzan::runtime::SimTime before = view->activeCursorTime();

    view->setCursorTime(0, 77);
    EXPECT_EQ(view->activeCursorTime(), 77U) << "locking must not stop the program moving it";

    // Locking stops the *click*, not the API: the view's own handler is what honours it, and
    // that is what a user's click goes through.
    view->setCursorLocked(0, false);
    view->setCursorTime(0, before);
    EXPECT_EQ(view->activeCursorTime(), before);
}

TEST_F(WaveformViewTest, RenderingCostDoesNotGrowWithTheTransitionsInView) {
    // The requirement of section 10.3. A window showing two hundred time units and a window
    // showing twenty thousand of the same trace differ by a hundredfold in transitions and must
    // not differ meaningfully in the work done to draw them.
    showEverythingAndRun(20'000);

    view->axis().setRange(0, 200);
    const QImage zoomedIn = render();

    view->axis().setRange(0, 20'000);
    const QImage zoomedOut = render();

    ASSERT_FALSE(zoomedIn.isNull());
    ASSERT_FALSE(zoomedOut.isNull());

    // Both drew something: the zoomed-out view collapses into busy bands rather than going
    // blank, which is the behaviour that makes a hundred-thousand-transition trace readable.
    const std::map<QRgb, int> wide = colourHistogram(zoomedOut);
    EXPECT_GT(countOf(wide, theme.color(enzan::gui::ThemeColor::WaveTrace)), 200);
}

TEST_F(WaveformViewTest, ZoomingToFitShowsTheWholeRun) {
    showEverythingAndRun(500);

    view->zoomFit();
    EXPECT_EQ(view->axis().start(), 0U);
    EXPECT_GT(view->axis().end(), 500U);
}

TEST_F(WaveformViewTest, FollowingTheTailKeepsTheEndInView) {
    showEverythingAndRun(200);

    view->axis().setRange(0, 50);
    view->setFollowTail(true);

    session->run(300);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 500; }));
    view->extentChanged();

    EXPECT_EQ(view->axis().span(), 50U) << "following must not change the zoom";
    EXPECT_GE(view->axis().end(), 500U);
}

// ---------------------------------------------------------------------------
// The summary cache
// ---------------------------------------------------------------------------
//
// Summarising a row is a pass over the width of the window, and it used to happen for every row
// on screen on every repaint — which is why the window slowed down in proportion to its own
// height and why scrolling it was the worst case. The tests below are in two halves: one says
// the cache is used, the rest say it is thrown away whenever it could be stale, which is the
// only way a cache can be wrong.
//
// The second half counts summaries rather than comparing pixels. A trace is drawn partly from
// its summary and partly from values read live, so a stale summary still moves some pixels and
// a picture that differs proves nothing about which half did it; the count is about the cache
// and nothing else.

TEST_F(WaveformViewTest, ScrollingReusesTheSummariesAlreadyComputed) {
    showEverythingAndRun(20'000);
    static_cast<void>(render());

    const std::uint64_t afterFirstPaint = view->summariesComputed();
    ASSERT_GT(afterFirstPaint, 0U) << "the first repaint has to summarise something";

    // Nothing changed, so nothing has to be summarised again. This is the repaint a moving
    // cursor, a highlight or a change of radix asks for.
    static_cast<void>(render());
    EXPECT_EQ(view->summariesComputed(), afterFirstPaint);

    // And scrolling moves the rows without changing what any of them shows.
    QScrollBar* bar = treeView->verticalScrollBar();
    bar->setValue(bar->value() + 1);
    static_cast<void>(render());

    EXPECT_LE(view->summariesComputed() - afterFirstPaint, 2U)
        << "scrolling one line must summarise the row that came into view, not every row";
}

TEST_F(WaveformViewTest, ZoomingSummarisesAgain) {
    showEverythingAndRun(20'000);
    static_cast<void>(render());

    const std::uint64_t afterFirstPaint = view->summariesComputed();

    view->axis().setRange(0, 200);
    static_cast<void>(render());

    EXPECT_GT(view->summariesComputed(), afterFirstPaint)
        << "a summary is per pixel column of a time range, so a new range needs new summaries";
}

TEST_F(WaveformViewTest, RunningFurtherSummarisesAgain) {
    showEverythingAndRun(200);

    // A window wider than the run and pinned, so that the only thing the run changes is how
    // much history there is — not the range, which would invalidate the cache the other way
    // and hide it if the recorded extent were not a key of its own.
    view->setFollowTail(false);
    view->axis().setRange(0, 2000);
    static_cast<void>(render());

    const std::uint64_t afterFirstPaint = view->summariesComputed();
    const auto rangeStart = view->axis().start();
    const auto rangeEnd = view->axis().end();

    session->run(1800);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 2000; }));
    view->extentChanged();
    static_cast<void>(render());

    ASSERT_EQ(view->axis().start(), rangeStart) << "the range must not have moved";
    ASSERT_EQ(view->axis().end(), rangeEnd);

    EXPECT_GT(view->summariesComputed(), afterFirstPaint)
        << "the trace the kernel has since recorded has to be summarised, not drawn from the "
           "summary of the shorter one";
}

TEST_F(WaveformViewTest, RemovingABitSummarisesItsBusAgain) {
    SignalInfo count;
    for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
        if (info.name == QStringLiteral("count")) count = info;
    }
    ASSERT_GT(count.width, 1U) << "the counter is the design's only bus";

    const int bus = model->addSignal(count);
    session->subscribe({count.signalIndex});

    session->run(2000);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 2000; }));

    model->unpack(bus);
    treeView->expandAll();
    view->zoomFit();
    static_cast<void>(render());

    const std::uint64_t afterFirstPaint = view->summariesComputed();

    // Section 10.7: a row with children shows what its children show. A bus whose bits are all
    // still there is drawn from its own trace; take one away and it has to be drawn by merging
    // the rest, which is a different summary of a different shape.
    const WaveRow* row = model->row(bus);
    ASSERT_NE(row, nullptr);
    ASSERT_FALSE(row->children.isEmpty());
    model->removeRow(row->children.front());
    static_cast<void>(render());

    EXPECT_GT(view->summariesComputed(), afterFirstPaint)
        << "the bus has to be summarised again without the bit that was removed";
}

// ---------------------------------------------------------------------------
// Scrolling moves what is drawn
// ---------------------------------------------------------------------------
//
// A vertical scroll does not change a single trace, only where each one sits, so the canvas
// blits what it has and paints the strip that came into view. That is worth having and it is
// the one optimisation here that can be *wrong* rather than merely slow: a blit by the wrong
// amount leaves traces against the wrong names, and the window looks plausible while lying.
//
// So the test is the only one that settles it — what the window shows after scrolling has to
// be pixel for pixel what it shows when made to draw the same thing from scratch.

class ScrollingCanvasTest : public WaveformViewTest {
protected:
    /// Fills the window with enough rows to scroll through, each with a trace of its own.
    void fillWithRows(int count) {
        const QVector<SignalInfo> available = objectsOf(QStringLiteral("/tb"));
        ASSERT_FALSE(available.isEmpty());

        QVector<std::uint32_t> wanted;
        for (int i = 0; i < count; ++i) {
            const SignalInfo& info = available[i % available.size()];
            model->addSignal(info);
            if (!info.isParameter()) wanted.push_back(info.signalIndex);
        }
        session->subscribe(wanted);

        session->run(2000);
        ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 2000; }));

        treeView->resize(200, 300);
        view->resize(600, 300);
        view->zoomFit();
        QCoreApplication::processEvents();
    }

    /// What the canvas shows, after letting every queued repaint happen.
    [[nodiscard]] QImage shown() {
        QCoreApplication::processEvents();
        return view->grab().toImage().convertToFormat(QImage::Format_RGB32);
    }

    /// What the canvas shows when made to draw all of it again.
    [[nodiscard]] QImage drawnAfresh() {
        view->update();
        return shown();
    }
};

TEST_F(ScrollingCanvasTest, TheBlitMovesTheContentsAsFarAsTheListMoved) {
    fillWithRows(40);
    QScrollBar* bar = treeView->verticalScrollBar();
    ASSERT_GT(bar->maximum(), 8) << "there has to be something to scroll through";

    static_cast<void>(shown());

    // A row that stays on screen throughout, so that how far *it* moved is how far everything
    // moved. Comparing pictures instead would prove nothing: asking a widget for its picture
    // re-renders it from scratch, which is exactly the path the blit is avoiding.
    const QModelIndex probe = model->index(3, 0);
    ASSERT_TRUE(probe.isValid());

    for (int step = 0; step < 5; ++step) {
        const int before = treeView->visualRect(probe).top();
        bar->setValue(bar->value() + 1);
        QCoreApplication::processEvents();

        const int after = treeView->visualRect(probe).top();
        ASSERT_NE(after, before) << "the list did not actually move on step " << step;
        EXPECT_EQ(view->lastScrollShift(), after - before)
            << "the canvas moved its contents a different distance than the list moved, on "
               "step "
            << step;
    }

    // And the other way, which is where a sign error hides.
    for (int step = 0; step < 4; ++step) {
        const int before = treeView->visualRect(probe).top();
        bar->setValue(bar->value() - 1);
        QCoreApplication::processEvents();

        EXPECT_EQ(view->lastScrollShift(), treeView->visualRect(probe).top() - before)
            << "going back up moved the contents the wrong way, on step " << step;
    }
}

TEST_F(ScrollingCanvasTest, StepsThatArriveFasterThanRepaintsEachMoveTheContentsOnce) {
    fillWithRows(40);
    QScrollBar* bar = treeView->verticalScrollBar();
    static_cast<void>(shown());

    const QModelIndex probe = model->index(3, 0);
    const int before = treeView->visualRect(probe).top();

    // No repaint in between, which is what a fast scroll delivers: several steps land before
    // the window has drawn any of them. Each has to move the contents by its own step, so the
    // canvas has to remember where it left them rather than where it last drew them.
    bar->setValue(bar->value() + 1);
    const int first = view->lastScrollShift();
    bar->setValue(bar->value() + 1);
    const int second = view->lastScrollShift();

    QCoreApplication::processEvents();

    EXPECT_EQ(first + second, treeView->visualRect(probe).top() - before)
        << "two steps moved the contents " << first + second << " where the list moved "
        << treeView->visualRect(probe).top() - before;
}

TEST_F(ScrollingCanvasTest, AJumpPastAWindowfulIsRepaintedRatherThanBlitted) {
    fillWithRows(40);
    QScrollBar* bar = treeView->verticalScrollBar();

    static_cast<void>(shown());

    // Nothing on screen survives a jump this far, so there is nothing worth moving and the
    // canvas is painted afresh instead.
    bar->setValue(bar->maximum());
    QCoreApplication::processEvents();
    EXPECT_EQ(view->lastScrollShift(), 0);

    // What it drew has to be right either way.
    EXPECT_EQ(shown(), drawnAfresh());
}

TEST_F(ScrollingCanvasTest, ScrollingDoesNotSummariseWhatItAlreadyHas) {
    fillWithRows(40);
    static_cast<void>(shown());

    const std::uint64_t before = view->summariesComputed();

    QScrollBar* bar = treeView->verticalScrollBar();
    bar->setValue(bar->value() + 1);
    static_cast<void>(shown());

    EXPECT_LE(view->summariesComputed() - before, 2U)
        << "one line down summarises the row that came into view, not the window";
}
} // namespace
