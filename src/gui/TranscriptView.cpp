// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/TranscriptView.h"

#include <QAction>
#include <QFont>
#include <QFontDatabase>
#include <QList>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextFragment>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <utility>
#include <vector>

namespace enzan::gui {

namespace {

/// Lines kept before the oldest are dropped.
///
/// A design with a `$display` in a clocked block produces output faster than anyone reads it,
/// and an unbounded transcript turns into the GUI's largest allocation by a wide margin.
constexpr int MaximumLines = 20'000;

/// The property of a run's character format that holds which of the theme's colours it is in.
constexpr int ColourProperty = QTextFormat::UserProperty;

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
    appendLine({Piece{.text = text, .colour = colour}});
}

void TranscriptView::appendLine(const std::vector<Piece>& pieces) {
    if (std::ranges::all_of(pieces, [](const Piece& piece) { return piece.text.isEmpty(); }))
        return;

    // Only follow the end when the reader is already there. Yanking the view back while
    // someone is reading what scrolled past is the single most irritating thing a log pane
    // can do.
    QScrollBar* bar = output->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2;

    QTextCursor cursor(output->document());
    cursor.movePosition(QTextCursor::End);
    if (!output->document()->isEmpty()) cursor.insertBlock();

    // Each run records which of the theme's colours it is in, for `applyTheme`.
    for (const Piece& piece : pieces) {
        QTextCharFormat format;
        format.setForeground(theme->color(piece.colour));
        format.setProperty(ColourProperty, static_cast<int>(piece.colour));
        if (piece.bold) format.setFontWeight(QFont::Bold);
        format.setFontItalic(piece.italic);
        format.setFontUnderline(piece.underline);
        cursor.insertText(piece.text, format);
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

    const QString complete = partialLine.left(lastBreak + 1);
    partialLine = partialLine.mid(lastBreak + 1);

    // A run may hold line breaks, and a line several runs. `complete` ends with its break, so
    // nothing is left over once the last line is appended.
    std::vector<Piece> line;
    for (const TerminalRun& run : decoder.decode(complete)) {
        const ThemeColor colour = run.style.color == TerminalStyle::NoColor
                                      ? ThemeColor::TranscriptForeground
                                      : terminalColor(run.style.color);
        const QList<QStringView> parts = QStringView(run.text).split(u'\n');
        for (qsizetype index = 0; index < parts.size(); ++index) {
            if (index > 0) appendLine(std::exchange(line, {}));
            line.push_back(Piece{.text = parts[index].toString(),
                                 .colour = colour,
                                 .bold = run.style.bold,
                                 .italic = run.style.italic,
                                 .underline = run.style.underline});
        }
    }
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
    decoder.reset();
}

void TranscriptView::applyTheme() {
    output->setFont(
        theme->font(ThemeFont::Transcript, QFontDatabase::systemFont(QFontDatabase::FixedFont)));

    // Where each run is and which colour it is in, read before anything is recoloured, since
    // recolouring a run can change how the document divides into fragments.
    struct Run {
        int position;
        int length;
        ThemeColor colour;
    };

    std::vector<Run> runs;
    for (QTextBlock block = output->document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator at = block.begin(); !at.atEnd(); ++at) {
            const QTextFragment fragment = at.fragment();
            const QVariant colour = fragment.charFormat().property(ColourProperty);
            if (!colour.isValid()) continue;
            runs.push_back(Run{.position = fragment.position(),
                               .length = fragment.length(),
                               .colour = static_cast<ThemeColor>(colour.toInt())});
        }
    }

    // One edit for the whole document, so that it is laid out once rather than once a run.
    QTextCursor cursor(output->document());
    cursor.beginEditBlock();
    for (const Run& run : runs) {
        QTextCharFormat format;
        format.setForeground(theme->color(run.colour));
        cursor.setPosition(run.position);
        cursor.setPosition(run.position + run.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
}

} // namespace enzan::gui
