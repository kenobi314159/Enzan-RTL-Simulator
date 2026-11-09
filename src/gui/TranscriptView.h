// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/TerminalDecoder.h"
#include "gui/Theme.h"

#include <QString>
#include <QWidget>

#include <vector>

class QAction;
class QPlainTextEdit;

namespace enzan::gui {

/// The Transcript window: the kernel's output and its diagnostics.
class TranscriptView : public QWidget {
    Q_OBJECT

public:
    /// `theme` must outlive the view. When it changes, `applyTheme` shows the change.
    explicit TranscriptView(const Theme& theme, QWidget* parent = nullptr);

    /// Appends kernel output as it was written, without adding a line ending, in the colours its
    /// terminal codes give it (`TerminalDecoder`).
    void appendOutput(const QString& text);

    /// Appends a line of Enzan's own, in the theme's colour for notes.
    void appendNote(const QString& text);

    /// Appends a diagnostic, coloured by `severity` as `DiagnosticSeverity` numbers it.
    void appendDiagnostic(int severity, const QString& text, const QString& file, int line);

    void clearTranscript();

    /// Takes the theme's font and colours again, recolouring everything already shown.
    ///
    /// Each run of text remembers which of the theme's colours it is in rather than the colour
    /// itself, so a new theme reaches what was written under the old one: a transcript half in
    /// one theme's colours and half in another's would read as two kinds of message.
    void applyTheme();

    /// The command that clears the transcript, for the window's menu as well as the
    /// transcript's own, so that the two are one command rather than two kept in step.
    [[nodiscard]] QAction* clearAction() const noexcept { return clear; }

private:
    /// A run of a line's text, in one of the theme's colours.
    struct Piece {
        QString text;
        ThemeColor colour = ThemeColor::TranscriptForeground;
        bool bold = false;
        bool italic = false;
        bool underline = false;
    };

    /// Appends `text` on a line of its own in `colour`.
    void append(const QString& text, ThemeColor colour);

    /// Appends `pieces` as a line of their own, scrolling to the end only when the view was
    /// already there. A line of nothing is not shown.
    void appendLine(const std::vector<Piece>& pieces);

    const Theme* theme;
    QPlainTextEdit* output = nullptr;
    QAction* clear = nullptr;

    /// Whatever the kernel wrote without a trailing newline, waiting for the rest of its line.
    QString partialLine;

    /// Reads the kernel's output a line at a time, keeping a colour from one to the next.
    TerminalDecoder decoder;
};

} // namespace enzan::gui
