// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the Keyboard Shortcuts window reads keys from actions. The window as a whole, opened from
// the Help menu, is MainWindowTests.

#include "gui/ShortcutSheet.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QKeySequence>
#include <QMenu>

namespace {

using enzan::gui::keysOf;
using enzan::gui::keyText;
using enzan::gui::sectionOf;
using enzan::gui::ShortcutSection;

TEST(ShortcutSheetTest, AKeyIsWrittenOutRatherThanAbbreviated) {
    // Qt's "Ins", beside a command in a tool that measures time, reads as a nanosecond.
    EXPECT_EQ(keyText(QKeySequence(Qt::Key_Insert)), QStringLiteral("Insert"));
    EXPECT_EQ(keyText(QKeySequence(Qt::CTRL | Qt::Key_Delete)), QStringLiteral("Ctrl+Delete"));
    EXPECT_EQ(keyText(QKeySequence(Qt::Key_PageDown)), QStringLiteral("Page Down"));

    // And nothing else is touched, a name that merely contains one of them included.
    EXPECT_EQ(keyText(QKeySequence(Qt::CTRL | Qt::Key_Plus)), QStringLiteral("Ctrl++"));
    EXPECT_EQ(keyText(QKeySequence(Qt::CTRL | Qt::Key_Home)), QStringLiteral("Ctrl+Home"));
}

TEST(ShortcutSheetTest, AnActionsKeysAreEveryKeyItIsBoundTo) {
    QAction zoom(QStringLiteral("Zoom In"));
    zoom.setShortcuts(
        {QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal)});

    EXPECT_EQ(keysOf(zoom), QStringLiteral("Ctrl++ or Ctrl+="));
}

TEST(ShortcutSheetTest, AKeyTheActionDoesNotBindIsReadFromItsText) {
    // How a menu is told a key it does not bind, such as Tab, which the wave window handles itself.
    const QAction step(QStringLiteral("Next Edge\tTab"));
    EXPECT_EQ(keysOf(step), QStringLiteral("Tab"));

    const QAction plain(QStringLiteral("Release"));
    EXPECT_TRUE(keysOf(plain).isEmpty());
}

TEST(ShortcutSheetTest, ASectionListsWhatHasAKeyOnceAndInOrder) {
    QAction rename(QStringLiteral("Rename…"));
    rename.setShortcut(QKeySequence(Qt::Key_F2));
    QAction separator;
    separator.setSeparator(true);
    QAction release(QStringLiteral("Release"));
    QAction copy(QStringLiteral("&Copy"));
    copy.setShortcut(QKeySequence::Copy);

    // Entries in a submenu are listed with the section their menu is in.
    QMenu radix(QStringLiteral("Radix"));
    QAction* binary = radix.addAction(QStringLiteral("Binary"));
    binary->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));

    QSet<const QAction*> seen;
    const ShortcutSection edit = sectionOf(
        QStringLiteral("Edit"), {&rename, &separator, &release, radix.menuAction(), &copy}, seen);
    ASSERT_EQ(edit.entries.size(), 3);
    EXPECT_EQ(edit.entries[0].keys, QStringLiteral("F2"));
    EXPECT_EQ(edit.entries[0].meaning, QStringLiteral("Rename"))
        << "the ellipsis promises a dialog, which the sheet does not need to";
    EXPECT_EQ(edit.entries[1].meaning, QStringLiteral("Binary"));
    EXPECT_EQ(edit.entries[2].meaning, QStringLiteral("Copy")) << "the mnemonic is not part of it";

    // A command offered in two menus is listed under the first.
    const ShortcutSection again = sectionOf(QStringLiteral("Wave"), {&copy}, seen);
    EXPECT_TRUE(again.entries.isEmpty());
}

} // namespace
