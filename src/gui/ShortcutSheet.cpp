// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/ShortcutSheet.h"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace enzan::gui {

namespace {

/// What an action is called, without the parts that belong to a menu: the key after a tab, the
/// mnemonic's ampersand and the ellipsis that promises a dialog.
QString meaningOf(const QAction& action) {
    QString text = action.text().section(u'\t', 0, 0);
    text.remove(u'&');
    if (text.endsWith(u'…')) text.chop(1);
    return text;
}

} // namespace

QString keysOf(const QAction& action) {
    if (!action.shortcuts().isEmpty()) {
        QStringList keys;
        for (const QKeySequence& key : action.shortcuts())
            keys << key.toString(QKeySequence::NativeText);
        return keys.join(QStringLiteral(" or "));
    }
    return action.text().section(u'\t', 1);
}

ShortcutSection sectionOf(const QString& title, const QList<QAction*>& actions,
                          QSet<const QAction*>& seen) {
    ShortcutSection section{.title = title, .entries = {}};
    for (const QAction* action : actions) {
        if (action->menu() != nullptr) {
            section.entries += sectionOf(title, action->menu()->actions(), seen).entries;
            continue;
        }
        if (action->isSeparator() || seen.contains(action)) continue;

        const QString keys = keysOf(*action);
        if (keys.isEmpty()) continue;

        seen.insert(action);
        section.entries.append({.keys = keys, .meaning = meaningOf(*action)});
    }
    return section;
}

ShortcutSheet::ShortcutSheet(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Keyboard Shortcuts"));

    browser = new QTextBrowser(this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(browser);
    resize(560, 640);
}

void ShortcutSheet::setSections(const QVector<ShortcutSection>& sections) {
    // No colour and no font in the markup: the browser draws it with the palette and the font the
    // theme gave the window (section 10.5).
    QString html;
    for (const ShortcutSection& section : sections) {
        if (section.entries.isEmpty()) continue;

        // One width for the key column in every section, so that the meanings line up down the
        // whole sheet rather than starting wherever each section's longest key ends.
        html += QStringLiteral("<h3>%1</h3><table width=\"100%\" cellspacing=\"0\" "
                               "cellpadding=\"3\">")
                    .arg(section.title.toHtmlEscaped());
        for (const ShortcutEntry& entry : section.entries) {
            html += QStringLiteral("<tr><td width=\"40%\"><b>%1</b></td><td>%2</td></tr>")
                        .arg(entry.keys.toHtmlEscaped(), entry.meaning.toHtmlEscaped());
        }
        html += QStringLiteral("</table>");
    }
    browser->setHtml(html);
}

} // namespace enzan::gui
