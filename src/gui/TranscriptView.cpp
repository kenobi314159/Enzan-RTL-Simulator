// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/TranscriptView.h"

#include <QAction>
#include <QFontDatabase>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
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

    // The text edit's own menu, copying and selecting, with clearing added to it: what a reader
    // right-clicking a log expects to find there.
    clear = new QAction(tr("Clear Transcript"), this);
    connect(clear, &QAction::triggered, this, &TranscriptView::clearTranscript);
    output->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(output, &QWidget::customContextMenuRequested, this, [this](const QPoint& where) {
        QMenu* menu = output->createStandardContextMenu();
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addSeparator();
        menu->addAction(clear);
        menu->popup(output->viewport()->mapToGlobal(where));
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(output, 1);

    applyTheme();
}

void TranscriptView::append(const QString& text, ThemeColor colour) {
    if (text.isEmpty()) return;

    // Only follow the end when the reader is already there. Yanking the view back while
    // someone is reading what scrolled past is the single most irritating thing a log pane
    // can do.
    QScrollBar* bar = output->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2;

    QTextCursor cursor(output->document());
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat format;
    format.setForeground(theme->color(colour));

    if (!output->document()->isEmpty()) cursor.insertBlock();
    QTextBlock block = cursor.block();
    cursor.insertText(text, format);

    // Every line the text made, which is more than one when it had line breaks in it, records
    // which of the theme's colours it is in, for `applyTheme`.
    for (const QTextBlock last = cursor.block(); block.isValid(); block = block.next()) {
        block.setUserState(static_cast<int>(colour));
        if (block == last) break;
    }

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

    for (const QString& line : complete.split(u'\n'))
        append(line, ThemeColor::TranscriptForeground);
}

void TranscriptView::appendNote(const QString& text) {
    append(text, ThemeColor::TranscriptNote);
}

void TranscriptView::appendDiagnostic(int severity, const QString& text, const QString& file,
                                      int line) {
    QString rendered;
    if (!file.isEmpty()) {
        rendered = line > 0 ? QStringLiteral("%1:%2: ").arg(file).arg(line)
                            : QStringLiteral("%1: ").arg(file);
    }
    rendered += text;

    append(rendered, colorForSeverity(severity));
}

void TranscriptView::clearTranscript() {
    output->clear();
    partialLine.clear();
}

void TranscriptView::applyTheme() {
    output->setFont(
        theme->font(ThemeFont::Transcript, QFontDatabase::systemFont(QFontDatabase::FixedFont)));

    // One edit for the whole document, so that it is laid out once rather than once a line.
    QTextCursor cursor(output->document());
    cursor.beginEditBlock();
    for (QTextBlock block = output->document()->begin(); block.isValid(); block = block.next()) {
        if (block.userState() < 0) continue;

        QTextCharFormat format;
        format.setForeground(theme->color(static_cast<ThemeColor>(block.userState())));
        cursor.setPosition(block.position());
        cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
}

} // namespace enzan::gui
