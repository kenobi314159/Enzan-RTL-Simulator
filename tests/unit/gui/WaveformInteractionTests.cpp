// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the mouse and keyboard model of DESIGN_GUIDE.md section 10.7.
//
// Every gesture there is arbitrary in the sense that the other way round would also have worked
// — a bare wheel could have zoomed, a right-drag could have framed the other direction. That is
// exactly why they are tested: an arbitrary choice with nothing holding it in place is one that
// gets quietly swapped by whoever next finds the opposite more natural, and the reader who had
// learned the first one pays for it.
//
// The events are synthesised and posted rather than driven through QTest's helpers, because the
// gestures here are drags and modified wheels, which are clearer written out than assembled from
// press/move/release calls.

#include "gui/WaveformView.h"

#include "gui/Theme.h"
#include "gui/WaveformModel.h"
#include "wavedb/WaveformDatabase.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QHeaderView>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTreeView>
#include <QWheelEvent>

#include <memory>

namespace {

using enzan::gui::HighlightSlots;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
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
        ASSERT_TRUE(pumpUntil([this, duration] { return session->currentTime() == duration; }));

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

    wheel(QPoint(200, 200), 1, Qt::ControlModifier);

    EXPECT_LT(view->axis().span(), spanBefore);

    // The instant under the pointer stays put, which is what makes a wheel zoom feel like it
    // moves the design rather than the window.
    const SimTime after = view->axis().timeAt(200.0);
    EXPECT_LE(under > after ? under - after : after - under, 2U);
}

TEST_F(InteractionTest, CtrlWheelTheOtherWayZoomsOut) {
    populate();

    const SimTime spanBefore = view->axis().span();
    wheel(QPoint(400, 200), -1, Qt::ControlModifier);
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

    EXPECT_EQ(view->activeCursor(), 0);
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

    ASSERT_TRUE(model->moveRowsTo({clock}, -1, 2));

    EXPECT_EQ(model->indexOfRow(count).row(), 0);
    EXPECT_EQ(model->indexOfRow(clock).row(), 1);
    EXPECT_EQ(model->indexOfRow(third).row(), 2);
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

    EXPECT_EQ(model->displayedSignals().size(), 2)
        << "unpacking must not subscribe to anything new";
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

TEST_F(InteractionTest, HighlightingARowComesFromTheThemeAndCanBeCleared) {
    populate();

    const int clock = rowOf.value(QStringLiteral("clk"));
    model->setHighlight(clock, 2);

    EXPECT_EQ(model->row(clock)->highlight, 2);

    // Section 10.5: the colour on screen is the theme's, not a literal in widget code.
    const QVariant background = model->data(model->indexOfRow(clock), Qt::BackgroundRole);
    ASSERT_TRUE(background.isValid());
    EXPECT_EQ(background.value<QColor>(), theme.color(enzan::gui::highlightColor(2)));

    model->setHighlight(clock, 0);
    EXPECT_FALSE(model->data(model->indexOfRow(clock), Qt::BackgroundRole).isValid());
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

} // namespace
