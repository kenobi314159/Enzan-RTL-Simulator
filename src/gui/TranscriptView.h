// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/Theme.h"

#include <QString>
#include <QWidget>

class QAction;
class QPlainTextEdit;

namespace enzan::gui {

/// The Transcript window: the kernel's output and its diagnostics.
class TranscriptView : public QWidget {
    Q_OBJECT

public:
    /// `theme` must outlive the view. When it changes, `applyTheme` shows the change.
    explicit TranscriptView(const Theme& theme, QWidget* parent = nullptr);

    /// Appends kernel output exactly as it was written, without adding a line ending.
    void appendOutput(const QString& text);

    /// Appends a line of Enzan's own, in the theme's colour for notes.
    void appendNote(const QString& text);

    /// Appends a diagnostic, coloured by `severity` as `DiagnosticSeverity` numbers it.
    void appendDiagnostic(int severity, const QString& text, const QString& file, int line);

    void clearTranscript();

    /// Takes the theme's font and colours again, recolouring every line already shown.
    ///
    /// Each line remembers which of the theme's colours it is in rather than the colour itself,
    /// so a new theme reaches what was written under the old one: a transcript half in one
    /// theme's colours and half in another's would read as two kinds of message.
    void applyTheme();

    /// The command that clears the transcript, for the window's menu as well as the
    /// transcript's own, so that the two are one command rather than two kept in step.
    [[nodiscard]] QAction* clearAction() const noexcept { return clear; }

private:
    /// Appends `text` on a line of its own in `colour`, scrolling to the end only when the view
    /// was already there.
    void append(const QString& text, ThemeColor colour);

    const Theme* theme;
    QPlainTextEdit* output = nullptr;
    QAction* clear = nullptr;

    /// Whatever the kernel wrote without a trailing newline, waiting for the rest of its line.
    QString partialLine;
};

} // namespace enzan::gui
