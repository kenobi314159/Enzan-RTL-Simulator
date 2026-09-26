// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the Transcript window's command entry.
//
// The entry sends a line of Tcl and names no commands of its own (DESIGN_GUIDE.md section 8.1),
// so what is worth pinning is the prompt's own behaviour: when it is available, what it sends,
// and that the history it keeps is the one a reader expects from a prompt.

#include "gui/TranscriptView.h"

#include "gui/Theme.h"

#include <gtest/gtest.h>

#include <QKeyEvent>
#include <QLineEdit>
#include <QSignalSpy>
#include <QString>

#include <memory>

namespace {

using enzan::gui::Theme;
using enzan::gui::TranscriptView;

class TranscriptViewTest : public ::testing::Test {
protected:
    TranscriptViewTest() {
        theme.loadBuiltIn();
        view = std::make_unique<TranscriptView>(theme);
        entry = view->findChild<QLineEdit*>();
    }

    /// Types `text` and presses Return.
    ///
    /// A real key event rather than emitting `returnPressed`, so that the entry's own handling
    /// of the key is part of what is tested — including the event filter that watches for the
    /// arrows, which sits in front of it.
    void submit(const QString& text) {
        entry->setText(text);
        pressKey(Qt::Key_Return);
    }

    void pressKey(int key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(entry, &press);
    }

    Theme theme;
    std::unique_ptr<TranscriptView> view;
    QLineEdit* entry = nullptr;
};

TEST_F(TranscriptViewTest, TheEntryStartsUnavailableWithAReason) {
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->isEnabled());
    EXPECT_FALSE(entry->placeholderText().isEmpty()) << "a greyed box with no reason in it";
}

TEST_F(TranscriptViewTest, TheReasonIsShownWhileItIsUnavailable) {
    view->setCommandsAvailable(false, QStringLiteral("Waiting for the run to stop"));

    EXPECT_FALSE(entry->isEnabled());
    EXPECT_EQ(entry->placeholderText(), QStringLiteral("Waiting for the run to stop"));
}

TEST_F(TranscriptViewTest, BecomingAvailableSaysWhatToType) {
    view->setCommandsAvailable(true);

    EXPECT_TRUE(entry->isEnabled());
    EXPECT_TRUE(entry->placeholderText().contains(QStringLiteral("run")))
        << "a prompt with no example in it";
}

TEST_F(TranscriptViewTest, ALineIsSentAndTheBoxIsCleared) {
    view->setCommandsAvailable(true);
    const QSignalSpy entered(view.get(), &TranscriptView::commandEntered);

    submit(QStringLiteral("run 100ns"));

    ASSERT_EQ(entered.count(), 1);
    EXPECT_EQ(entered.front().front().toString(), QStringLiteral("run 100ns"));

    // Cleared before the command runs rather than after: a line still sitting in the box looks
    // as though nothing happened and invites a second Return.
    EXPECT_TRUE(entry->text().isEmpty());
}

TEST_F(TranscriptViewTest, SurroundingSpaceIsTrimmed) {
    view->setCommandsAvailable(true);
    const QSignalSpy entered(view.get(), &TranscriptView::commandEntered);

    submit(QStringLiteral("  run 100ns  "));

    ASSERT_EQ(entered.count(), 1);
    EXPECT_EQ(entered.front().front().toString(), QStringLiteral("run 100ns"));
}

TEST_F(TranscriptViewTest, AnEmptyLineSendsNothing) {
    view->setCommandsAvailable(true);
    const QSignalSpy entered(view.get(), &TranscriptView::commandEntered);

    submit(QStringLiteral("   "));
    EXPECT_EQ(entered.count(), 0);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

TEST_F(TranscriptViewTest, UpWalksBackThroughWhatWasTyped) {
    view->setCommandsAvailable(true);
    submit(QStringLiteral("vsim tb"));
    submit(QStringLiteral("run 100ns"));

    pressKey(Qt::Key_Up);
    EXPECT_EQ(entry->text(), QStringLiteral("run 100ns"));

    pressKey(Qt::Key_Up);
    EXPECT_EQ(entry->text(), QStringLiteral("vsim tb"));

    // Past the oldest entry it stays where it is rather than emptying the box.
    pressKey(Qt::Key_Up);
    EXPECT_EQ(entry->text(), QStringLiteral("vsim tb"));
}

TEST_F(TranscriptViewTest, DownComesBackToTheFreshLine) {
    view->setCommandsAvailable(true);
    submit(QStringLiteral("vsim tb"));
    submit(QStringLiteral("run 100ns"));

    pressKey(Qt::Key_Up);
    pressKey(Qt::Key_Up);
    pressKey(Qt::Key_Down);
    EXPECT_EQ(entry->text(), QStringLiteral("run 100ns"));

    pressKey(Qt::Key_Down);
    EXPECT_TRUE(entry->text().isEmpty()) << "Down past the newest entry is the empty line";
}

TEST_F(TranscriptViewTest, TheSameLineTwiceIsRememberedOnce) {
    view->setCommandsAvailable(true);
    submit(QStringLiteral("run 100ns"));
    submit(QStringLiteral("run 100ns"));
    submit(QStringLiteral("run 100ns"));

    pressKey(Qt::Key_Up);
    EXPECT_EQ(entry->text(), QStringLiteral("run 100ns"));

    // One entry to walk back to, not three.
    pressKey(Qt::Key_Up);
    EXPECT_EQ(entry->text(), QStringLiteral("run 100ns"));
    pressKey(Qt::Key_Down);
    EXPECT_TRUE(entry->text().isEmpty());
}

TEST_F(TranscriptViewTest, UpWithNoHistoryDoesNothing) {
    view->setCommandsAvailable(true);
    entry->setText(QStringLiteral("half a command"));

    pressKey(Qt::Key_Up);
    EXPECT_EQ(entry->text(), QStringLiteral("half a command"));
}

} // namespace
