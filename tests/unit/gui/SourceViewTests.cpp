// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the Source window.
//
// What is worth pinning is the behaviour that makes it a debugger rather than a file viewer:
// the text comes from the kernel and never from the path, a click in the gutter toggles a
// breakpoint, and the current-execution marker follows the design into whichever file it
// stopped in.

#include "gui/SourceView.h"

#include "gui/Theme.h"

#include <gtest/gtest.h>

#include <QComboBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QStringList>
#include <QWidget>

#include <memory>

namespace {

using enzan::gui::SourceView;
using enzan::gui::Theme;

/// A view with the shipped theme, plus the two child widgets the tests have to reach.
class SourceViewTest : public ::testing::Test {
protected:
    SourceViewTest() {
        // Loaded before the view is built, because the view reads its colours in its
        // constructor and an empty theme would give it none.
        theme.loadBuiltIn();

        view = std::make_unique<SourceView>(theme);
        view->resize(600, 400);

        // Found by type rather than exposed by the view: a test reaching into the widget tree
        // is better than an accessor that exists only for the test and that production code
        // could then start using.
        chooser = view->findChild<QComboBox*>();
        text = view->findChild<QPlainTextEdit*>();
    }

    /// The gutter, which is the one child widget that is neither the chooser nor the text.
    [[nodiscard]] QWidget* gutter() const {
        for (QWidget* child : view->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            if (child != chooser && child != text) return child;
        }
        return nullptr;
    }

    /// Clicks the gutter at the vertical middle of 1-based `line`.
    void clickGutter(std::uint32_t line) {
        QWidget* strip = gutter();
        ASSERT_NE(strip, nullptr);

        // The line height from the text's own metrics, which is what the gutter lays out by.
        const int height = text->fontMetrics().height();
        const int y = (static_cast<int>(line - 1) * height) + (height / 2);

        QMouseEvent press(QEvent::MouseButtonPress, QPointF(4, y), QPointF(4, y), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(strip, &press);
    }

    Theme theme;
    std::unique_ptr<SourceView> view;
    QComboBox* chooser = nullptr;
    QPlainTextEdit* text = nullptr;
};

// ---------------------------------------------------------------------------
// Where the text comes from
// ---------------------------------------------------------------------------

TEST_F(SourceViewTest, AFileIsAskedForRatherThanRead) {
    // The kernel holds the bytes it analysed and may be on another machine (section 2.2). A
    // window that opened the path would show whatever is at that path here.
    const QSignalSpy requested(view.get(), &SourceView::fileRequested);

    view->setFiles({QStringLiteral("/rtl/counter.sv")});

    ASSERT_EQ(requested.count(), 1);
    EXPECT_EQ(requested.front().front().toString(), QStringLiteral("/rtl/counter.sv"));
    EXPECT_EQ(view->currentFile(), QStringLiteral("/rtl/counter.sv"));
}

TEST_F(SourceViewTest, TextThatArrivesIsShown) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("module a;\nendmodule\n"), true);

    EXPECT_TRUE(text->toPlainText().contains(QStringLiteral("endmodule")));
}

TEST_F(SourceViewTest, AFileTheKernelDoesNotHaveSaysSoInsteadOfBeingEmpty) {
    view->setFiles({QStringLiteral("gone.sv")});
    view->setFileText(QStringLiteral("gone.sv"), QString(), false);

    EXPECT_TRUE(text->toPlainText().contains(QStringLiteral("gone.sv")));
    EXPECT_TRUE(text->toPlainText().contains(QStringLiteral("no source")));
}

TEST_F(SourceViewTest, TextIsKeptSoTheSameFileIsNotAskedForTwice) {
    view->setFiles({QStringLiteral("a.sv"), QStringLiteral("b.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("first\n"), true);
    view->setFileText(QStringLiteral("b.sv"), QStringLiteral("second\n"), true);

    const QSignalSpy requested(view.get(), &SourceView::fileRequested);
    view->showFile(QStringLiteral("b.sv"));
    view->showFile(QStringLiteral("a.sv"));

    EXPECT_EQ(requested.count(), 0) << "a file already in hand was asked for again";
    EXPECT_TRUE(text->toPlainText().contains(QStringLiteral("first")));
}

TEST_F(SourceViewTest, TheFileOnShowStaysOnShowWhenTheListIsRebuilt) {
    view->setFiles({QStringLiteral("a.sv"), QStringLiteral("b.sv")});
    view->setFileText(QStringLiteral("b.sv"), QStringLiteral("second\n"), true);
    view->showFile(QStringLiteral("b.sv"));

    // A reader who was looking at a line did not ask to be moved.
    view->setFiles({QStringLiteral("a.sv"), QStringLiteral("b.sv"), QStringLiteral("c.sv")});
    EXPECT_EQ(view->currentFile(), QStringLiteral("b.sv"));
}

TEST_F(SourceViewTest, AnEmptyFileListLeavesTheWindowAlone) {
    view->setFiles({});
    EXPECT_TRUE(view->currentFile().isEmpty());
}

// ---------------------------------------------------------------------------
// Breakpoints
// ---------------------------------------------------------------------------

TEST_F(SourceViewTest, ClickingTheGutterAsksForABreakpoint) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\ntwo\nthree\nfour\n"), true);

    const QSignalSpy requested(view.get(), &SourceView::breakpointRequested);
    clickGutter(3);

    ASSERT_EQ(requested.count(), 1);
    EXPECT_EQ(requested.front().at(0).toString(), QStringLiteral("a.sv"));
    EXPECT_EQ(requested.front().at(1).toUInt(), 3U);
}

TEST_F(SourceViewTest, ClickingALineThatHasOneRemovesIt) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\ntwo\nthree\nfour\n"), true);

    QHash<QString, QHash<std::uint32_t, SourceView::Marker>> markers;
    markers[QStringLiteral("a.sv")].insert(3, SourceView::Marker{.id = 42, .enabled = true});
    view->setBreakpoints(markers);

    const QSignalSpy removed(view.get(), &SourceView::breakpointRemoved);
    const QSignalSpy requested(view.get(), &SourceView::breakpointRequested);
    clickGutter(3);

    // One gesture, two meanings, decided by what is already there — which is what every editor
    // does and what saves the reader a modifier to remember.
    ASSERT_EQ(removed.count(), 1);
    EXPECT_EQ(removed.front().front().toUInt(), 42U);
    EXPECT_EQ(requested.count(), 0);
}

TEST_F(SourceViewTest, AMarkerInAnotherFileDoesNotAffectThisOne) {
    view->setFiles({QStringLiteral("a.sv"), QStringLiteral("b.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\ntwo\nthree\n"), true);

    QHash<QString, QHash<std::uint32_t, SourceView::Marker>> markers;
    markers[QStringLiteral("b.sv")].insert(2, SourceView::Marker{.id = 7, .enabled = true});
    view->setBreakpoints(markers);

    const QSignalSpy requested(view.get(), &SourceView::breakpointRequested);
    const QSignalSpy removed(view.get(), &SourceView::breakpointRemoved);
    clickGutter(2);

    EXPECT_EQ(requested.count(), 1) << "another file's breakpoint was taken for this one";
    EXPECT_EQ(removed.count(), 0);
}

TEST_F(SourceViewTest, ClickingPastTheEndOfTheTextDoesNothing) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\n"), true);

    const QSignalSpy requested(view.get(), &SourceView::breakpointRequested);
    clickGutter(40);

    EXPECT_EQ(requested.count(), 0);
}

// ---------------------------------------------------------------------------
// The current line
// ---------------------------------------------------------------------------

TEST_F(SourceViewTest, TheCurrentLineFollowsTheDesignIntoAnotherFile) {
    view->setFiles({QStringLiteral("a.sv"), QStringLiteral("b.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\n"), true);
    view->setFileText(QStringLiteral("b.sv"), QStringLiteral("one\ntwo\n"), true);
    view->showFile(QStringLiteral("a.sv"));

    // A reader who stepped into another module means to see that module.
    view->setCurrentLine(QStringLiteral("b.sv"), 2);
    EXPECT_EQ(view->currentFile(), QStringLiteral("b.sv"));
}

TEST_F(SourceViewTest, TheCurrentLineIsWashedBehindTheTextRatherThanRecolouringIt) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\ntwo\nthree\n"), true);

    view->setCurrentLine(QStringLiteral("a.sv"), 2);

    const QList<QTextEdit::ExtraSelection> selections = text->extraSelections();
    ASSERT_EQ(selections.size(), 1);
    EXPECT_EQ(selections.front().cursor.blockNumber(), 1);
    EXPECT_EQ(selections.front().format.background().color(),
              theme.color(enzan::gui::ThemeColor::SourceCurrentLine));

    // The text keeps its own colour: it is what is being read, and recolouring it would make
    // the line the design stopped on the hardest one on screen to read.
    EXPECT_FALSE(selections.front().format.hasProperty(QTextFormat::ForegroundBrush));
}

TEST_F(SourceViewTest, AZeroLineClearsTheMarker) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\ntwo\n"), true);
    view->setCurrentLine(QStringLiteral("a.sv"), 2);
    ASSERT_EQ(text->extraSelections().size(), 1);

    view->setCurrentLine(QString(), 0);
    EXPECT_TRUE(text->extraSelections().isEmpty());
}

TEST_F(SourceViewTest, ALineBeyondTheTextIsIgnoredRatherThanCrashing) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\n"), true);

    view->setCurrentLine(QStringLiteral("a.sv"), 99);
    EXPECT_TRUE(text->extraSelections().isEmpty());
}

// ---------------------------------------------------------------------------
// The gutter
// ---------------------------------------------------------------------------

TEST_F(SourceViewTest, TheGutterWidensForMoreLineNumbers) {
    view->setFiles({QStringLiteral("a.sv")});
    view->setFileText(QStringLiteral("a.sv"), QStringLiteral("one\ntwo\n"), true);
    const int narrow = view->gutterWidth();

    QString many;
    for (int line = 0; line < 2000; ++line) many += QStringLiteral("x\n");
    view->setFileText(QStringLiteral("a.sv"), many, true);

    EXPECT_GT(view->gutterWidth(), narrow);
}

} // namespace
