// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/TranscriptView.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

namespace enzan::gui {

namespace {

/// Lines kept before the oldest are dropped.
///
/// A design with a `$display` in a clocked block produces output faster than anyone reads it,
/// and an unbounded transcript turns into the GUI's largest allocation by a wide margin.
constexpr int MaximumLines = 20'000;

/// Maps a `DiagnosticSeverity` to the theme colour it is shown in. Taken as an int because the
/// severity crosses the socket as a number and the GUI does not link the diagnostic engine.
ThemeColor colorForSeverity(int severity) {
    // DiagnosticSeverity: Ignored, Note, Warning, Error, Fatal.
    switch (severity) {
    case 1: return ThemeColor::TranscriptNote;
    case 2: return ThemeColor::TranscriptWarning;
    default: return ThemeColor::TranscriptError;
    }
}

} // namespace

TranscriptView::TranscriptView(const Theme& themeToUse, QWidget* parent)
    : QWidget(parent), theme(&themeToUse) {
    output = new QPlainTextEdit(this);
    output->setReadOnly(true);
    output->setUndoRedoEnabled(false);
    output->setMaximumBlockCount(MaximumLines);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    entry = new QLineEdit(this);
    entry->setEnabled(false);
    entry->setPlaceholderText(tr("Waiting for a kernel"));
    entry->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(entry, &QLineEdit::returnPressed, this, &TranscriptView::submitCommand);

    // Up and Down walk what was typed. An event filter rather than a subclass of QLineEdit,
    // because the two keys are the whole of the behaviour and a class would be more to read
    // than it saves.
    entry->installEventFilter(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(output, 1);
    layout->addWidget(entry);
}

void TranscriptView::setCommandsAvailable(bool available, const QString& reason) {
    entry->setEnabled(available);
    entry->setPlaceholderText(available ? tr("Enter a command, for example: run 100ns") : reason);
}

void TranscriptView::submitCommand() {
    const QString script = entry->text().trimmed();
    if (script.isEmpty()) return;

    // Cleared before the command runs rather than after. A command may take a while — `run
    // -all` on a real design does — and a line still sitting in the box looks as though
    // nothing happened and invites a second Return.
    entry->clear();

    // Not recorded twice in a row: a reader pressing Return on the same command repeatedly
    // wants one entry to walk back to, not ten.
    if (history.isEmpty() || history.constLast() != script) history.append(script);
    historyPosition = static_cast<int>(history.size());

    Q_EMIT commandEntered(script);
}

void TranscriptView::recallCommand(int offset) {
    if (history.isEmpty()) return;

    const int wanted = historyPosition + offset;
    if (wanted < 0) return;

    // Past the newest entry is the empty line the reader started from, which is where Down
    // should end up rather than refusing to move.
    if (wanted >= static_cast<int>(history.size())) {
        historyPosition = static_cast<int>(history.size());
        entry->clear();
        return;
    }

    historyPosition = wanted;
    entry->setText(history.at(wanted));
}

bool TranscriptView::eventFilter(QObject* watched, QEvent* event) {
    if (watched != entry || event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);

    // A key press is a QKeyEvent, and the type test above is how Qt says so. `dynamic_cast`
    // would ask the run time to confirm what the event system has already guaranteed, on a
    // path that runs for every key the reader types.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    const auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Up) {
        recallCommand(-1);
        return true;
    }
    if (key->key() == Qt::Key_Down) {
        recallCommand(1);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void TranscriptView::append(const QString& text, const QColor& colour, bool ownLine) {
    if (text.isEmpty()) return;

    // Only follow the end when the reader is already there. Yanking the view back while
    // someone is reading what scrolled past is the single most irritating thing a log pane
    // can do.
    QScrollBar* bar = output->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2;

    QTextCursor cursor(output->document());
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat format;
    format.setForeground(colour);

    if (ownLine && !output->document()->isEmpty()) cursor.insertBlock();
    cursor.insertText(text, format);

    if (atEnd) bar->setValue(bar->maximum());
}

void TranscriptView::appendOutput(const QString& text) {
    // The kernel's output arrives in whatever chunks the socket delivered, which need not be
    // whole lines, so a partial line is held back rather than being shown as a line of its own
    // and then duplicated when the rest turns up.
    partialLine += text;

    const qsizetype lastBreak = partialLine.lastIndexOf(u'\n');
    if (lastBreak < 0) return;

    const QString complete = partialLine.left(lastBreak);
    partialLine = partialLine.mid(lastBreak + 1);

    const QColor colour = theme->color(ThemeColor::TranscriptForeground);
    for (const QString& line : complete.split(u'\n')) append(line, colour, true);
}

void TranscriptView::appendNote(const QString& text) {
    append(text, theme->color(ThemeColor::TranscriptNote), true);
}

void TranscriptView::appendDiagnostic(int severity, const QString& text, const QString& file,
                                      int line) {
    QString rendered;
    if (!file.isEmpty()) {
        rendered = line > 0 ? QStringLiteral("%1:%2: ").arg(file).arg(line)
                            : QStringLiteral("%1: ").arg(file);
    }
    rendered += text;

    append(rendered, theme->color(colorForSeverity(severity)), true);
}

void TranscriptView::clearTranscript() {
    output->clear();
    partialLine.clear();
}

} // namespace enzan::gui
