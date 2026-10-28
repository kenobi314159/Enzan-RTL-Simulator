// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

class QAction;
class QKeySequence;
class QTextBrowser;

namespace enzan::gui {

/// One line of the sheet: what to press or do, and what it does.
struct ShortcutEntry {
    QString keys;
    QString meaning;
};

/// A heading of the sheet and the lines under it.
struct ShortcutSection {
    QString title;
    QVector<ShortcutEntry> entries;
};

/// `key` as a reader is shown it outside a menu: Qt's text for it, with the names it abbreviates
/// written out.
///
/// "Ins" beside a command, in a tool that measures time, reads as a nanosecond.
[[nodiscard]] QString keyText(const QKeySequence& key);

/// The key of `action` as a menu shows it, or empty when it has none.
///
/// Every key it is bound to, and otherwise what its text gives after a tab character: that is how
/// a menu is told the key of an entry the action itself does not bind, such as Tab, which the
/// wave window handles itself.
[[nodiscard]] QString keysOf(const QAction& action);

/// The section `title` lists: every action among `actions`, and in the submenus under them, that
/// has a key, in order. An action already in `seen` is left out and every one listed is added to
/// it, so that a command offered in two menus is listed once, under the first.
[[nodiscard]] ShortcutSection sectionOf(const QString& title, const QList<QAction*>& actions,
                                        QSet<const QAction*>& seen);

/// The Keyboard Shortcuts window.
///
/// Built from the actions themselves each time it is shown rather than written out, so that it
/// cannot come to disagree with them; what the mouse does is not an action, and comes from the
/// table beside the view's event handlers instead (`WaveformView::gestures`). Not modal, because
/// it is something to keep open beside the window while learning it.
class ShortcutSheet : public QDialog {
    Q_OBJECT

public:
    explicit ShortcutSheet(QWidget* parent = nullptr);

    void setSections(const QVector<ShortcutSection>& sections);

private:
    QTextBrowser* browser = nullptr;
};

} // namespace enzan::gui
