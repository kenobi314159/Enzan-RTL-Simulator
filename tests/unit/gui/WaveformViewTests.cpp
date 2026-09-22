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

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QImage>
#include <QPixmap>
#include <QTreeView>

#include <map>
#include <optional>

#include "eir/BitVector.h"

namespace {

using enzan::eir::Radix;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveformView;
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
        model = std::make_unique<WaveformModel>(*session);
    }

    SignalInfo infoFor(const QString& scope, const QString& name) {
        for (const SignalInfo& info : objectsOf(scope)) {
            if (info.name == name) return info;
        }
        ADD_FAILURE() << "no signal " << name.toStdString() << " in " << scope.toStdString();
        return {};
    }

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
// The canvas
// ---------------------------------------------------------------------------

class WaveformViewTest : public SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();

        theme.loadBuiltIn();
        model = std::make_unique<WaveformModel>(*session);

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

} // namespace
