// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the wave window's mouse and keyboard model.
//
// Every gesture is arbitrary in the sense that the other way round would also have worked — a
// bare wheel could have zoomed, a right-drag could have framed the other direction. That is
// exactly why they are tested: an arbitrary choice with nothing holding it in place is one that
// gets quietly swapped by whoever next finds the opposite more natural, and the reader who had
// learned the first one pays for it.
//
// The events are synthesised and posted rather than driven through QTest's helpers, because the
// gestures here are drags and modified wheels, which are clearer written out than assembled from
// press/move/release calls.

#include "gui/WaveformView.h"

#include "gui/Theme.h"
#include "gui/TimeAxis.h"
#include "gui/ValueFinder.h"
#include "gui/WaveformModel.h"
#include "wavedb/WaveformDatabase.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTreeView>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <memory>

namespace {

using enzan::eir::Radix;
using enzan::gui::HighlightSlots;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::TimeAxis;
using enzan::gui::ValueFinder;
using enzan::gui::WaveformModel;
using enzan::gui::WaveformView;
using enzan::runtime::SimTime;
using enzan::test::SessionFixture;

/// A wave window with the design's signals in it and a run behind it.
class InteractionTest : public SessionFixture {
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

        treeView->show();
        view->show();
        QCoreApplication::processEvents();
    }

    void TearDown() override {
        view = nullptr;
        treeView = nullptr;
        model = nullptr;
        SessionFixture::TearDown();
    }

    /// Adds `/tb`'s signals, subscribes to them and runs, so that there are edges to snap to.
    void populate(std::uint64_t duration = 400) {
        QVector<std::uint32_t> wanted;
        for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
            const int rowId = model->addSignal(info);
            rowOf[info.name] = rowId;
            wanted.push_back(info.signalIndex);
        }
        session->subscribe(wanted);

        session->run(duration);
        ASSERT_TRUE(pumpUntilCaughtUp(duration));

        view->axis().setExtent(session->latestTime());
        view->axis().setRange(0, duration);
        QCoreApplication::processEvents();
    }

    /// The y coordinate in the canvas of the row `label` is drawn on.
    [[nodiscard]] int yOfRow(const QString& label) const {
        const QModelIndex index = model->indexOfRow(rowOf.value(label));
        EXPECT_TRUE(index.isValid()) << label.toStdString();

        const QRect rect = treeView->visualRect(index);
        return treeView->header()->height() + rect.center().y();
    }

    void press(Qt::MouseButton button, const QPoint& at,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(QEvent::MouseButtonPress, at, view->mapToGlobal(at), button, button,
                          modifiers);
        QCoreApplication::sendEvent(view.get(), &event);
    }

    void move(Qt::MouseButton held, const QPoint& to) {
        QMouseEvent event(QEvent::MouseMove, to, view->mapToGlobal(to), Qt::NoButton, held,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(view.get(), &event);
    }

    void release(Qt::MouseButton button, const QPoint& at) {
        QMouseEvent event(QEvent::MouseButtonRelease, at, view->mapToGlobal(at), button,
                          Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view.get(), &event);
    }

    void drag(Qt::MouseButton button, const QPoint& from, const QPoint& to) {
        press(button, from);
        move(button, to);
        release(button, to);
    }

    /// Sends a key the way the window system would, so that Tab goes through Qt's focus
    /// machinery rather than round it — which is the only path the view actually sees.
    void key(Qt::Key which, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, which, modifiers);
        QCoreApplication::sendEvent(view.get(), &event);
    }

    void wheel(const QPoint& at, int notches, Qt::KeyboardModifiers modifiers) {
        QWheelEvent event(QPointF(at), view->mapToGlobal(at).toPointF(), QPoint(),
                          QPoint(0, notches * 120), Qt::NoButton, modifiers, Qt::NoScrollPhase,
                          false);
        QCoreApplication::sendEvent(view.get(), &event);
    }

    /// A point inside the trace area, on the row `label`.
    [[nodiscard]] QPoint pointOn(const QString& label, int x) const { return {x, yOfRow(label)}; }

    Theme theme;
    std::unique_ptr<WaveformModel> model;
    std::unique_ptr<QTreeView> treeView;
    std::unique_ptr<WaveformView> view;
    QHash<QString, int> rowOf;
};

// ---------------------------------------------------------------------------
// The wheel
// ---------------------------------------------------------------------------

TEST_F(InteractionTest, ABareWheelScrollsTheListRatherThanZooming) {
    // One wheel, one meaning, across the whole window: the names pane and the trace pane are an
    // internal boundary the reader has no reason to think about.
    populate();
    for (int extra = 0; extra < 40; ++extra)
        model->addGroup(QStringLiteral("filler %1").arg(extra));
    QCoreApplication::processEvents();

    const QScrollBar* bar = treeView->verticalScrollBar();
    ASSERT_GT(bar->maximum(), 0) << "the list is not long enough to scroll";
    const int before = bar->value();
    const SimTime spanBefore = view->axis().span();

    wheel(QPoint(400, 200), -3, Qt::NoModifier);
    QCoreApplication::processEvents();

    EXPECT_GT(bar->value(), before) << "a bare wheel must scroll the signal list";
    EXPECT_EQ(view->axis().span(), spanBefore) << "and must not zoom";
}

TEST_F(InteractionTest, CtrlWheelZoomsAboutThePointer) {
    populate();

    const SimTime spanBefore = view->axis().span();
    const SimTime under = view->axis().timeAt(200.0);

    wheel(QPoint(200, 200), -1, Qt::ControlModifier);

    EXPECT_LT(view->axis().span(), spanBefore);

    // The instant under the pointer stays put, which is what makes a wheel zoom feel like it
    // moves the design rather than the window.
    const SimTime after = view->axis().timeAt(200.0);
    EXPECT_LE(under > after ? under - after : after - under, 2U);
}

TEST_F(InteractionTest, CtrlWheelTheOtherWayZoomsOut) {
    populate();

    const SimTime spanBefore = view->axis().span();
    wheel(QPoint(400, 200), 1, Qt::ControlModifier);
    EXPECT_GT(view->axis().span(), spanBefore);
}

TEST_F(InteractionTest, ShiftWheelPansWithoutChangingTheZoom) {
    populate();
    view->axis().setRange(1000, 400);

    const SimTime spanBefore = view->axis().span();
    wheel(QPoint(400, 200), -1, Qt::ShiftModifier);

    EXPECT_EQ(view->axis().span(), spanBefore);
    EXPECT_GT(view->axis().start(), 1000U);
}

// ---------------------------------------------------------------------------
// The right button
// ---------------------------------------------------------------------------

TEST_F(InteractionTest, DraggingRightToLeftZoomsToTheRange) {
    populate();
    view->axis().setRange(0, 400);

    const SimTime from = view->axis().timeAt(200.0);
    const SimTime to = view->axis().timeAt(600.0);

    drag(Qt::RightButton, QPoint(600, 200), QPoint(200, 200));

    EXPECT_EQ(view->axis().start(), from);
    EXPECT_EQ(view->axis().span(), to - from);
}

TEST_F(InteractionTest, DraggingLeftToRightZoomsToFit) {
    populate();
    view->axis().setRange(100, 50);

    drag(Qt::RightButton, QPoint(200, 200), QPoint(600, 200));

    EXPECT_EQ(view->axis().start(), 0U);
    EXPECT_GT(view->axis().end(), session->latestTime());
}

TEST_F(InteractionTest, ARightClickIsNotAGesture) {
    // Every mis-click while reaching for a context menu would otherwise throw away the range
    // the reader had zoomed to.
    populate();
    view->axis().setRange(100, 50);

    drag(Qt::RightButton, QPoint(300, 200), QPoint(302, 200));

    EXPECT_EQ(view->axis().start(), 100U);
    EXPECT_EQ(view->axis().span(), 50U);
}

// ---------------------------------------------------------------------------
// The left button
// ---------------------------------------------------------------------------

TEST_F(InteractionTest, ClickingATraceSelectsItsRow) {
    populate();

    press(Qt::LeftButton, pointOn(QStringLiteral("count"), 300));
    release(Qt::LeftButton, pointOn(QStringLiteral("count"), 300));

    EXPECT_EQ(view->selectedRow(), rowOf.value(QStringLiteral("count")));
    EXPECT_EQ(model->rowIdOf(treeView->currentIndex()), rowOf.value(QStringLiteral("count")))
        << "the tree and the canvas must agree about which row is current";
}

TEST_F(InteractionTest, ClickingNearAnEdgeSnapsTheCursorToIt) {
    // The clock toggles every five units; over four hundred units in eight hundred pixels, an
    // edge is ten pixels from its neighbour, so a click a few pixels off one is unambiguous.
    populate();
    view->axis().setRange(0, 400);

    const double atEdge = view->axis().xOf(200);
    press(Qt::LeftButton, QPoint(static_cast<int>(atEdge) + 2, yOfRow(QStringLiteral("clk"))));
    release(Qt::LeftButton, QPoint(static_cast<int>(atEdge) + 2, yOfRow(QStringLiteral("clk"))));

    EXPECT_EQ(view->activeCursorTime(), 200U);
}

TEST_F(InteractionTest, AClickWithNoEdgeInRangeLandsWhereItWasPut) {
    // A cursor that jumped to a distant edge would be worse than one that did not snap.
    populate();

    // Zoomed far out, every edge is sub-pixel, so a click in the middle of a busy stretch has
    // nothing within the snap radius and must be left alone.
    view->axis().setRange(0, 400);
    const int rowY = yOfRow(QStringLiteral("count"));

    press(Qt::LeftButton, QPoint(401, rowY));
    release(Qt::LeftButton, QPoint(401, rowY));

    const SimTime clicked = view->axis().timeAt(401.0);
    const SimTime landed = view->activeCursorTime();
    EXPECT_LE(clicked > landed ? clicked - landed : landed - clicked, 3U);
}

TEST_F(InteractionTest, ALockedCursorDoesNotFollowAClick) {
    populate();
    view->setCursorTime(0, 100);
    view->setCursorLocked(0, true);

    press(Qt::LeftButton, pointOn(QStringLiteral("clk"), 500));
    release(Qt::LeftButton, pointOn(QStringLiteral("clk"), 500));

    EXPECT_EQ(view->activeCursorTime(), 100U);

    // The click still selects the row, because selecting is not moving.
    EXPECT_EQ(view->selectedRow(), rowOf.value(QStringLiteral("clk")));
}

// ---------------------------------------------------------------------------
// Cursors
// ---------------------------------------------------------------------------

TEST_F(InteractionTest, TabStepsTheActiveCursorAlongTheSelectedSignal) {
    populate();
    view->axis().setRange(0, 400);

    press(Qt::LeftButton, pointOn(QStringLiteral("clk"), 0));
    release(Qt::LeftButton, pointOn(QStringLiteral("clk"), 0));
    ASSERT_EQ(view->activeCursorTime(), 0U);

    // The clock toggles every five units, so Tab walks 5, then 10, and Shift+Tab comes back.
    key(Qt::Key_Tab);
    EXPECT_EQ(view->activeCursorTime(), 5U);
    key(Qt::Key_Tab);
    EXPECT_EQ(view->activeCursorTime(), 10U);

    key(Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(view->activeCursorTime(), 5U);
}

TEST_F(InteractionTest, TabDoesNothingWithoutASelectedSignal) {
    populate();
    treeView->setCurrentIndex(QModelIndex());
    view->setCursorTime(0, 100);

    key(Qt::Key_Tab);
    EXPECT_EQ(view->activeCursorTime(), 100U);
}

TEST_F(InteractionTest, TabStopsAtTheLastEdgeRatherThanWrapping) {
    populate(40);
    view->axis().setRange(0, 40);

    press(Qt::LeftButton, pointOn(QStringLiteral("clk"), 0));
    release(Qt::LeftButton, pointOn(QStringLiteral("clk"), 0));

    for (int step = 0; step < 50; ++step) key(Qt::Key_Tab);
    const SimTime settled = view->activeCursorTime();

    key(Qt::Key_Tab);
    EXPECT_EQ(view->activeCursorTime(), settled) << "stepping past the end must not wrap round";
    EXPECT_LE(settled, 40U);
}

TEST_F(InteractionTest, ClickingACursorsLabelMakesItActive) {
    populate();
    view->axis().setRange(0, 400);

    view->setCursorTime(0, 100);
    const int second = view->addCursor(QStringLiteral("B"), 300);
    ASSERT_EQ(view->activeCursor(), second);

    // The label rectangles are filled in as the view paints, so it has to have painted once.
    view->grab();

    const QRect firstLabel = view->cursors()[0].labelRect;
    ASSERT_FALSE(firstLabel.isEmpty()) << "the first cursor's label was not drawn";

    press(Qt::LeftButton, firstLabel.center());
    release(Qt::LeftButton, firstLabel.center());

    EXPECT_EQ(view->activeCursor(), 1);
    EXPECT_EQ(view->activeCursorTime(), 100U)
        << "clicking a label must pick that cursor, not drag the active one to it";
}

TEST_F(InteractionTest, ACursorsLabelCarriesItsTime) {
    // The exact text is the renderer's business, but a label whose width does not change with
    // the time it sits at is a label with no time on it. Both positions are inside the view, so
    // both are drawn.
    populate();
    view->axis().setRange(0, 400);

    view->setCursorTime(0, 5);
    view->grab();
    const int shortTime = view->cursors()[0].labelRect.width();
    ASSERT_GT(shortTime, 0) << "the cursor's label was not drawn at all";

    view->setCursorTime(0, 300);
    view->grab();

    EXPECT_GT(view->cursors()[0].labelRect.width(), shortTime);
}

TEST_F(InteractionTest, SelectingARowInTheListIsTheSameAsClickingItsTrace) {
    // Picking a signal by name has to mean what picking it by trace means, or Tab appears to
    // do nothing to a reader who chose their signal from the list.
    populate();
    view->axis().setRange(0, 400);
    view->setCursorTime(0, 201);

    const QModelIndex clockRow = model->indexOfRow(rowOf.value(QStringLiteral("clk")));
    treeView->setCurrentIndex(clockRow);
    QCoreApplication::processEvents();

    EXPECT_EQ(view->selectedRow(), rowOf.value(QStringLiteral("clk")));
    EXPECT_EQ(view->activeCursorTime(), 200U)
        << "the cursor should have snapped to that signal's nearest edge";
}

TEST_F(InteractionTest, ARowSelectedWithoutBecomingCurrentIsStillTheSelectedSignal) {
    // A Ctrl+click, or a selection left over after the model changed, leaves the current index
    // somewhere else. "The selected signal" has to mean what the reader sees highlighted.
    populate();

    const QModelIndex counterRow = model->indexOfRow(rowOf.value(QStringLiteral("count")));
    treeView->selectionModel()->clearCurrentIndex();
    treeView->selectionModel()->select(counterRow,
                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCoreApplication::processEvents();

    EXPECT_EQ(view->selectedRow(), rowOf.value(QStringLiteral("count")));
}

TEST_F(InteractionTest, TabWorksWhileTheListHasFocus) {
    // Tab means the cursor wherever the reader is looking. Left to the tree it would be spent
    // on focus navigation, and a reader who had just picked a signal by name would lose it.
    populate();
    view->axis().setRange(0, 400);

    treeView->setCurrentIndex(model->indexOfRow(rowOf.value(QStringLiteral("clk"))));
    QCoreApplication::processEvents();
    view->setCursorTime(0, 100);

    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QCoreApplication::sendEvent(treeView.get(), &tab);
    EXPECT_EQ(view->activeCursorTime(), 105U);

    QKeyEvent back(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
    QCoreApplication::sendEvent(treeView.get(), &back);
    EXPECT_EQ(view->activeCursorTime(), 100U);
}

TEST_F(InteractionTest, AGroupCanBeSteppedAlongLikeASignal) {
    // A group shows a value on its line, so it has edges.
    populate();

    const int group = model->addGroup(QStringLiteral("both"));
    ASSERT_TRUE(model->moveRowsTo({rowOf.value(QStringLiteral("clk"))}, group, 0));

    treeView->setCurrentIndex(model->indexOfRow(group));
    QCoreApplication::processEvents();
    view->setCursorTime(0, 100);

    key(Qt::Key_Tab);
    EXPECT_EQ(view->activeCursorTime(), 105U);
}

// ---------------------------------------------------------------------------
// Finding a value
// ---------------------------------------------------------------------------
//
// The counter takes each value v at 10v - 5 and the run stops at 400.

class FindValueInteractionTest : public InteractionTest {
protected:
    void SetUp() override {
        InteractionTest::SetUp();
        populate();

        finder = std::make_unique<ValueFinder>(*session, *model, *view);
        reports = std::make_unique<QSignalSpy>(finder.get(), &ValueFinder::reported);

        select(QStringLiteral("count"));
        view->setCursorTime(0, 0);
    }

    void TearDown() override {
        reports = nullptr;
        finder = nullptr;
        InteractionTest::TearDown();
    }

    void select(const QString& label) {
        treeView->setCurrentIndex(model->indexOfRow(rowOf.value(label)));
    }

    /// What the finder said last.
    [[nodiscard]] QString lastReport() const {
        return reports->isEmpty() ? QString() : reports->last().front().toString();
    }

    [[nodiscard]] QString at(SimTime time) const {
        return TimeAxis::format(time, session->timeScale());
    }

    /// Lets the event loop run the slices of a search until it finishes.
    void finishSearching() {
        for (int turn = 0; turn < 1000 && finder->searching(); ++turn) {
            QCoreApplication::processEvents();
        }
    }

    std::unique_ptr<ValueFinder> finder;
    std::unique_ptr<QSignalSpy> reports;
};

TEST_F(FindValueInteractionTest, FindingMovesTheActiveCursorToWhereTheRowStartsToShowTheValue) {
    finder->find(QStringLiteral("05"), true);
    EXPECT_EQ(view->activeCursorTime(), 45U);
    EXPECT_EQ(lastReport(), QStringLiteral("count shows 05 from %1").arg(at(45)));

    view->setCursorTime(0, 100);
    finder->find(QStringLiteral("05"), false);
    EXPECT_EQ(view->activeCursorTime(), 45U);
}

TEST_F(FindValueInteractionTest, FindingAgainGoesOnToTheNextTimeAndNotTheOneTheCursorIsAt) {
    select(QStringLiteral("clk"));
    view->setCursorTime(0, 0);

    for (const SimTime expected : {5U, 15U, 25U}) {
        finder->find(QStringLiteral("1"), true);
        EXPECT_EQ(view->activeCursorTime(), expected);
    }
    finder->find(QStringLiteral("1"), false);
    EXPECT_EQ(view->activeCursorTime(), 15U);
}

TEST_F(FindValueInteractionTest, NothingFoundLeavesTheCursorAndSaysHowFarTheSearchLooked) {
    finder->find(QStringLiteral("ff"), true);
    EXPECT_EQ(view->activeCursorTime(), 0U);
    EXPECT_EQ(lastReport(), QStringLiteral("No later ff in count, up to %1").arg(at(400)));

    view->setCursorTime(0, 40);
    finder->find(QStringLiteral("05"), false);
    EXPECT_EQ(view->activeCursorTime(), 40U) << "a search does not wrap round";
    EXPECT_EQ(lastReport(), QStringLiteral("No earlier 05 in count"));
}

TEST_F(FindValueInteractionTest, ALockedCursorStaysWhereItIs) {
    view->setCursorTime(0, 100);
    view->setCursorLocked(0, true);

    finder->find(QStringLiteral("05"), false);
    EXPECT_EQ(view->activeCursorTime(), 100U);
    EXPECT_EQ(lastReport(), QStringLiteral("The active cursor is locked"));
}

TEST_F(FindValueInteractionTest, WithNoRowSelectedThereIsNothingToSearch) {
    treeView->setCurrentIndex(QModelIndex());
    treeView->selectionModel()->clearSelection();

    finder->find(QStringLiteral("05"), true);
    EXPECT_EQ(view->activeCursorTime(), 0U);
    EXPECT_EQ(lastReport(), QStringLiteral("Select a row to find a value in"));
}

TEST_F(FindValueInteractionTest, TextThatIsNoValueOfTheRowSaysWhyAndMovesNothing) {
    finder->find(QStringLiteral("1ff"), true);
    EXPECT_EQ(view->activeCursorTime(), 0U);
    EXPECT_EQ(lastReport(), QStringLiteral("'1ff' does not fit in 8 bits"));
    EXPECT_FALSE(finder->searching());
}

TEST_F(FindValueInteractionTest, ALongSearchGoesOnWhileTheWindowAnswersAndThenMovesTheCursor) {
    // One edge a slice: thirty-nine slices to the counter's 39, at 385.
    finder->setSlice(1, std::chrono::milliseconds(0));
    const QSignalSpy started(finder.get(), &ValueFinder::searchStarted);

    finder->find(QStringLiteral("27"), true);
    EXPECT_TRUE(finder->searching()) << "find returned before the search was done";
    EXPECT_EQ(started.count(), 1);
    EXPECT_EQ(view->activeCursorTime(), 0U);

    finishSearching();
    EXPECT_FALSE(finder->searching());
    EXPECT_EQ(view->activeCursorTime(), 385U);
    EXPECT_EQ(lastReport(), QStringLiteral("count shows 27 from %1").arg(at(385)));
}

TEST_F(FindValueInteractionTest, MovingTheCursorAbandonsASearchStillRunning) {
    finder->setSlice(1, std::chrono::milliseconds(0));
    const QSignalSpy abandoned(finder.get(), &ValueFinder::searchAbandoned);

    finder->find(QStringLiteral("27"), true);
    ASSERT_TRUE(finder->searching());
    view->setCursorTime(0, 200);

    finishSearching();
    EXPECT_FALSE(finder->searching());
    EXPECT_EQ(abandoned.count(), 1);
    EXPECT_EQ(view->activeCursorTime(), 200U) << "the reader's cursor, not the search's";
    EXPECT_TRUE(reports->isEmpty());
}

TEST_F(FindValueInteractionTest, SelectingAnotherRowAbandonsASearchStillRunning) {
    finder->setSlice(1, std::chrono::milliseconds(0));
    const QSignalSpy abandoned(finder.get(), &ValueFinder::searchAbandoned);

    finder->find(QStringLiteral("27"), true);
    ASSERT_TRUE(finder->searching());
    select(QStringLiteral("clk"));
    const SimTime placed = view->activeCursorTime();

    finishSearching();
    EXPECT_EQ(abandoned.count(), 1);
    EXPECT_EQ(view->activeCursorTime(), placed);
}

TEST_F(FindValueInteractionTest, ANewSearchReplacesOneStillRunning) {
    finder->setSlice(1, std::chrono::milliseconds(0));
    const QSignalSpy abandoned(finder.get(), &ValueFinder::searchAbandoned);

    finder->find(QStringLiteral("27"), true);
    ASSERT_TRUE(finder->searching());
    finder->find(QStringLiteral("03"), true);

    finishSearching();
    EXPECT_EQ(abandoned.count(), 1);
    EXPECT_EQ(view->activeCursorTime(), 25U);
}

// ---------------------------------------------------------------------------
// Keeping the canvas up to date
// ---------------------------------------------------------------------------

/// Counts the paint events a widget is asked for.
class PaintCounter : public QObject {
public:
    int painted = 0;

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Paint) ++painted;
        return QObject::eventFilter(watched, event);
    }
};

TEST_F(InteractionTest, UnpackingAndPackingRedrawTheCanvas) {
    // Expanding a bus changes which rows are on screen. Without the canvas being told, the
    // traces stayed as they were until something else happened to force a repaint.
    populate();

    PaintCounter counter;
    view->installEventFilter(&counter);

    const QModelIndex bus = model->indexOfRow(rowOf.value(QStringLiteral("count")));
    treeView->expand(bus);
    QCoreApplication::processEvents();
    EXPECT_GT(counter.painted, 0) << "unpacking did not ask the canvas to redraw";

    counter.painted = 0;
    treeView->collapse(bus);
    QCoreApplication::processEvents();
    EXPECT_GT(counter.painted, 0) << "packing did not ask the canvas to redraw";

    view->removeEventFilter(&counter);
}

TEST_F(InteractionTest, PastingRedrawsTheCanvasAndKeepsWhatWasOpen) {
    populate();

    const int bus = rowOf.value(QStringLiteral("count"));
    treeView->expand(model->indexOfRow(bus));
    QCoreApplication::processEvents();
    ASSERT_TRUE(treeView->isExpanded(model->indexOfRow(bus)));

    PaintCounter counter;
    view->installEventFilter(&counter);

    model->copySignalRows({rowOf.value(QStringLiteral("clk"))});
    const QVector<int> pasted = model->pasteSignalRows(-1, -1);
    QCoreApplication::processEvents();

    ASSERT_EQ(pasted.size(), 1);
    EXPECT_GT(counter.painted, 0) << "pasting did not ask the canvas to redraw";
    EXPECT_TRUE(treeView->isExpanded(model->indexOfRow(bus)))
        << "pasting must not pack what the reader had open";

    view->removeEventFilter(&counter);
}

// ---------------------------------------------------------------------------
// The signal list
// ---------------------------------------------------------------------------

TEST_F(InteractionTest, RowsCanBeReordered) {
    populate();

    const int clock = rowOf.value(QStringLiteral("clk"));
    const int count = rowOf.value(QStringLiteral("count"));
    ASSERT_EQ(model->indexOfRow(clock).row(), 0);

    ASSERT_TRUE(model->moveRowsTo({clock}, -1, 2));

    EXPECT_EQ(model->indexOfRow(count).row(), 0);
    EXPECT_EQ(model->indexOfRow(clock).row(), 1);
}

TEST_F(InteractionTest, RowsCanBeDraggedIntoAndOutOfAGroup) {
    populate();

    const int clock = rowOf.value(QStringLiteral("clk"));
    const int group = model->addGroup(QStringLiteral("Bus"));

    ASSERT_TRUE(model->moveRowsTo({clock}, group, 0));
    EXPECT_EQ(model->row(clock)->parent, group);
    EXPECT_EQ(model->rowCount(model->indexOfRow(group)), 1);

    ASSERT_TRUE(model->moveRowsTo({clock}, -1, 0));
    EXPECT_EQ(model->row(clock)->parent, -1);
    EXPECT_EQ(model->indexOfRow(clock).row(), 0);
}

TEST_F(InteractionTest, AGroupCannotBeDroppedInsideItself) {
    const int outer = model->addGroup(QStringLiteral("Outer"));
    const int inner = model->addGroup(QStringLiteral("Inner"));
    ASSERT_TRUE(model->moveRowsTo({inner}, outer, 0));

    EXPECT_FALSE(model->moveRowsTo({outer}, inner, 0));
    EXPECT_EQ(model->row(outer)->parent, -1);
}

TEST_F(InteractionTest, MovingARowDownWithinItsParentLandsWhereItWasDropped) {
    // Taking a row out from before the drop point moves that point up by one. Without
    // accounting for it, every downward drag lands one row further than it was dropped.
    populate();

    const int clock = rowOf.value(QStringLiteral("clk"));
    const int count = rowOf.value(QStringLiteral("count"));
    const int third = model->addGroup(QStringLiteral("Third"));

    const int before = model->indexOfRow(clock).row();
    ASSERT_TRUE(model->moveRowsTo({clock}, -1, before + 2));

    EXPECT_EQ(model->indexOfRow(count).row(), before)
        << "what was below the moved row moves up into its place";
    EXPECT_EQ(model->indexOfRow(clock).row(), before + 1);
    EXPECT_EQ(model->indexOfRow(third).row(), model->rowCount() - 1);
}

TEST_F(InteractionTest, AMultiBitSignalUnpacksIntoItsBits) {
    populate();

    const int count = rowOf.value(QStringLiteral("count"));
    ASSERT_TRUE(model->canUnpack(count));
    EXPECT_TRUE(model->hasChildren(model->indexOfRow(count)))
        << "an unpackable row must offer its expander before anyone clicks it";

    model->unpack(count);

    ASSERT_EQ(model->rowCount(model->indexOfRow(count)), 8);
    EXPECT_FALSE(model->canUnpack(count)) << "unpacking twice would double the rows";

    // Most significant first, and each bit is a row over the same signal rather than a new one.
    const int top = model->rowIdOf(model->index(0, 0, model->indexOfRow(count)));
    ASSERT_NE(model->row(top), nullptr);
    EXPECT_EQ(model->row(top)->label, QStringLiteral("count[7]"));
    EXPECT_TRUE(model->row(top)->sliced);
    EXPECT_EQ(model->row(top)->highBit, 7U);
    EXPECT_EQ(model->row(top)->lowBit, 7U);
    EXPECT_FALSE(model->canUnpack(top)) << "a single bit has nothing left to unpack";

    const int signalsBefore = static_cast<int>(model->displayedSignals().size());
    EXPECT_EQ(signalsBefore, static_cast<int>(objectsOf(QStringLiteral("/tb")).size()))
        << "unpacking must not subscribe to anything new";
}

TEST_F(InteractionTest, MovingTheCursorRefreshesRowsInsideGroupsAndUnpackedSignals) {
    // A range of changed rows names rows under one parent. One range over the top-level rows
    // left a bit of an unpacked signal, and a signal in a group, showing the value at the old
    // cursor until something else made the list redraw them.
    populate();

    const int count = rowOf.value(QStringLiteral("count"));
    model->unpack(count);
    const int group = model->addGroup(QStringLiteral("clocks"));
    ASSERT_TRUE(model->moveRowsTo({rowOf.value(QStringLiteral("clk"))}, group, 0));

    const QModelIndex bit = model->index(0, WaveformModel::ValueColumn, model->indexOfRow(count));
    const QModelIndex member =
        model->index(0, WaveformModel::ValueColumn, model->indexOfRow(group));
    ASSERT_TRUE(bit.isValid());
    ASSERT_TRUE(member.isValid());

    QSignalSpy changes(model.get(), &QAbstractItemModel::dataChanged);
    model->setCursorTime(123);

    const auto covered = [&changes](const QModelIndex& wanted) {
        return std::ranges::any_of(changes, [&wanted](const QList<QVariant>& change) {
            const auto first = change.at(0).value<QModelIndex>();
            const auto last = change.at(1).value<QModelIndex>();
            return first.parent() == wanted.parent() && first.row() <= wanted.row()
                   && wanted.row() <= last.row() && first.column() <= wanted.column()
                   && wanted.column() <= last.column();
        });
    };
    EXPECT_TRUE(covered(bit)) << "a bit of an unpacked signal was not refreshed";
    EXPECT_TRUE(covered(member)) << "a signal inside a group was not refreshed";
}

TEST_F(InteractionTest, AnAsciiRowShowsItsBytesAsCharacters) {
    populate(400);

    // Forty rising edges have passed, so the counter holds 40, the code of '('.
    const int count = rowOf.value(QStringLiteral("count"));
    model->setRadix(count, Radix::Ascii);
    EXPECT_EQ(model->renderValue(count, 400), QStringLiteral("("));
}

TEST_F(InteractionTest, AnUnpackedBitShowsThatBitsValue) {
    populate(400);

    const int count = rowOf.value(QStringLiteral("count"));
    model->unpack(count);

    // Forty rising edges have passed, so the counter holds 40 — 0b00101000.
    const QModelIndex parent = model->indexOfRow(count);
    const int bit5 = model->rowIdOf(model->index(2, 0, parent));
    const int bit4 = model->rowIdOf(model->index(3, 0, parent));

    ASSERT_NE(model->row(bit5), nullptr);
    EXPECT_EQ(model->row(bit5)->label, QStringLiteral("count[5]"));
    EXPECT_EQ(model->renderValue(bit5, 400), QStringLiteral("1"));
    EXPECT_EQ(model->renderValue(bit4, 400), QStringLiteral("0"));
}

TEST_F(InteractionTest, ASingleBitSignalDoesNotOfferToUnpack) {
    populate();

    const int clock = rowOf.value(QStringLiteral("clk"));
    EXPECT_FALSE(model->canUnpack(clock));
    EXPECT_FALSE(model->hasChildren(model->indexOfRow(clock)));
}

TEST_F(InteractionTest, AGroupOfOneBitSignalsDrawsAsABus) {
    // The other half of "a group of one-bit signals behaves as a vector": it has to be drawn as
    // one too, or the reader gets the number on the line and a blank beside it.
    populate();

    const int bus = rowOf.value(QStringLiteral("count"));
    model->unpack(bus);

    const int group = model->addGroup(QStringLiteral("nibble"));
    for (int bit = 0; bit < 4; ++bit) {
        const QModelIndex child = model->index(0, 0, model->indexOfRow(bus));
        ASSERT_TRUE(model->moveRowsTo({model->rowIdOf(child)}, group, bit));
    }

    // Collapsed, so only the group's own line is drawn and anything painted on it came from
    // the joined value rather than from its children being visible.
    treeView->collapseAll();
    QCoreApplication::processEvents();

    const QImage rendered = view->grab().toImage().convertToFormat(QImage::Format_RGB32);
    ASSERT_FALSE(rendered.isNull());

    const QRgb trace = theme.color(enzan::gui::ThemeColor::WaveTrace).rgb();
    int found = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            if (rendered.pixel(x, y) == trace) ++found;
        }
    }
    EXPECT_GT(found, 100) << "the group's own line was left blank";
}

TEST_F(InteractionTest, HighlightingARowComesFromTheThemeAndCanBeCleared) {
    populate();

    const int clock = rowOf.value(QStringLiteral("clk"));
    model->setHighlight(clock, 2);

    EXPECT_EQ(model->row(clock)->highlight, 2);

    // The colour on screen is the theme's, not a literal in widget code.
    const QVariant background = model->data(model->indexOfRow(clock), Qt::BackgroundRole);
    ASSERT_TRUE(background.isValid());
    EXPECT_EQ(background.value<QColor>(), theme.color(enzan::gui::highlightColor(2)));

    // And so is its text, which has a colour of its own for every highlight.
    const QVariant text = model->data(model->indexOfRow(clock), Qt::ForegroundRole);
    ASSERT_TRUE(text.isValid());
    EXPECT_EQ(text.value<QColor>(), theme.color(enzan::gui::ThemeColor::WaveHighlightText));
    EXPECT_EQ(model->data(model->indexOfRow(clock).siblingAtColumn(WaveformModel::ValueColumn),
                          Qt::ForegroundRole),
              text)
        << "the value is on the highlighted row too";

    model->setHighlight(clock, 0);
    EXPECT_FALSE(model->data(model->indexOfRow(clock), Qt::BackgroundRole).isValid());
    EXPECT_FALSE(model->data(model->indexOfRow(clock), Qt::ForegroundRole).isValid())
        << "a row with no highlight keeps the panel's text";
}

TEST_F(InteractionTest, AHighlightIsPaintedOnTheTraceToo) {
    // A highlight that stopped at the splitter would read as a rendering fault: the name and
    // the trace are one row to the reader.
    populate();
    model->setHighlight(rowOf.value(QStringLiteral("clk")), 3);

    const QImage rendered = view->grab().toImage().convertToFormat(QImage::Format_RGB32);
    ASSERT_FALSE(rendered.isNull());

    const QRgb wanted = theme.color(enzan::gui::highlightColor(3)).rgb();
    int found = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            if (rendered.pixel(x, y) == wanted) ++found;
        }
    }
    EXPECT_GT(found, 100);
}

TEST_F(InteractionTest, EveryHighlightSlotHasAColour) {
    for (int slot = 1; slot <= HighlightSlots; ++slot) {
        EXPECT_TRUE(theme.color(enzan::gui::highlightColor(slot)).isValid()) << slot;
    }
}

// ---------------------------------------------------------------------------
// The keyboard
// ---------------------------------------------------------------------------

TEST_F(InteractionTest, TheTracesHandTheirKeysToTheList) {
    // A key means one thing across the Wave tab, and the bare keys are the list's: `+` and `-`
    // unpack and pack, and typing the start of a name finds its row.
    populate();
    const int count = rowOf.value(QStringLiteral("count"));
    treeView->setCurrentIndex(model->indexOfRow(count));

    key(Qt::Key_Plus);
    EXPECT_TRUE(treeView->isExpanded(model->indexOfRow(count))) << "+ on the traces unpacks";
    key(Qt::Key_Minus);
    EXPECT_FALSE(treeView->isExpanded(model->indexOfRow(count))) << "- on the traces packs";

    treeView->setCurrentIndex(model->indexOfRow(rowOf.value(QStringLiteral("clk"))));
    for (const QString& letter : {QStringLiteral("c"), QStringLiteral("o")}) {
        QKeyEvent typed(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, letter);
        QCoreApplication::sendEvent(view.get(), &typed);
    }
    EXPECT_EQ(view->selectedRow(), count) << "typing on the traces finds a row by its name";
}

TEST_F(InteractionTest, APanStepMovesTheViewAndKeepsTheZoom) {
    populate();
    view->axis().setRange(100, 100);

    view->panStep(true);
    EXPECT_GT(view->axis().start(), 100U);
    EXPECT_EQ(view->axis().span(), 100U);

    view->panStep(false);
    EXPECT_EQ(view->axis().start(), 100U);
}

TEST_F(InteractionTest, TinyUnapplicablePanStepsAccumulateToAnActualStep) {
    populate();
    view->axis().setRange(0, 8);

    press(Qt::MiddleButton, QPoint(300, 200));

    EXPECT_EQ(view->axis().start(), 0U);
    EXPECT_EQ(view->axis().span(), 8U);
    move(Qt::MiddleButton, QPoint(266, 200));
    EXPECT_EQ(view->axis().start(), 0U);
    EXPECT_EQ(view->axis().span(), 8U);
    move(Qt::MiddleButton, QPoint(233, 200));
    EXPECT_EQ(view->axis().start(), 0U);
    EXPECT_EQ(view->axis().span(), 8U);
    move(Qt::MiddleButton, QPoint(199, 200));
    EXPECT_EQ(view->axis().start(), 1U);
    EXPECT_EQ(view->axis().span(), 8U);

    release(Qt::MiddleButton, QPoint(199, 200));
}

TEST_F(InteractionTest, PlacingTheActiveCursorBringsItIntoView) {
    populate();
    view->axis().setRange(0, 100);

    view->placeActiveCursor(300);
    EXPECT_EQ(view->activeCursorTime(), 300U);
    EXPECT_LE(view->axis().start(), 300U);
    EXPECT_GE(view->axis().end(), 300U) << "a cursor sent off the side of the window is lost";

    // Locked means pinned, whichever key would move it.
    view->setCursorLocked(view->activeCursor(), true);
    view->placeActiveCursor(0);
    EXPECT_EQ(view->activeCursorTime(), 300U);
}

TEST_F(InteractionTest, TheNeighbourCursorIsTheNextOneInTime) {
    // By where the cursors are on the axis, not by when they were made, and with no going round
    // at either end.
    populate();
    view->axis().setRange(0, 400);
    view->setCursorTime(0, 100);
    view->addCursor(QStringLiteral("B"), 300);
    view->addCursor(QStringLiteral("C"), 200);

    view->activateNeighbourCursor(true);
    EXPECT_EQ(view->activeCursorTime(), 300U);
    view->activateNeighbourCursor(true);
    EXPECT_EQ(view->activeCursorTime(), 300U) << "past the last cursor nothing happens";

    view->activateNeighbourCursor(false);
    EXPECT_EQ(view->activeCursorTime(), 200U);
    view->activateNeighbourCursor(false);
    EXPECT_EQ(view->activeCursorTime(), 100U);
    view->activateNeighbourCursor(false);
    EXPECT_EQ(view->activeCursorTime(), 100U) << "past the first cursor nothing happens";
}

TEST_F(InteractionTest, CursorsAtOneTimeAreOrderedByName) {
    // A cursor is added at the active one's time, so two at one time is the usual case rather
    // than a curiosity. Making either active must not change which comes first, or the keys
    // would swap them back and forth.
    populate();
    view->setCursorTime(0, 100);
    ASSERT_EQ(view->cursors()[0].name, QStringLiteral("Cursor 1"));
    view->addCursor(QStringLiteral("Cursor 2"), 100);

    const auto activeName = [this] { return view->cursors()[view->activeCursor()].name; };
    view->activateNeighbourCursor(false);
    EXPECT_EQ(activeName(), QStringLiteral("Cursor 1"));
    view->activateNeighbourCursor(false);
    EXPECT_EQ(activeName(), QStringLiteral("Cursor 1"));
    view->activateNeighbourCursor(true);
    EXPECT_EQ(activeName(), QStringLiteral("Cursor 2"));
    view->activateNeighbourCursor(true);
    EXPECT_EQ(activeName(), QStringLiteral("Cursor 2"));
}

} // namespace
