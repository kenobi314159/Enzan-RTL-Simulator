// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/Theme.h"

#include <QString>
#include <QWidget>

class QLineEdit;
class QPlainTextEdit;

namespace enzan::gui {

/// The Transcript window: the kernel's output, its diagnostics, and the command entry.
///
/// **The command entry is present and disabled**, and that is a milestone decision rather than
/// an unfinished one. Section 8.1 commits to Tcl with a Questa-style vocabulary as *the*
/// control language and section 18 puts it in M3; a handful of commands invented here to fill
/// the gap would be a second control surface that M3 then has to stay compatible with. Showing
/// the entry with the reason in it is more honest than hiding it and more useful than a set of
/// commands nobody will keep.
class TranscriptView : public QWidget {
    Q_OBJECT

public:
    /// `theme` must outlive the view.
    explicit TranscriptView(const Theme& theme, QWidget* parent = nullptr);

    /// Appends kernel output exactly as it was written, without adding a line ending.
    void appendOutput(const QString& text);

    /// Appends a line of Enzan's own, in the plain foreground colour.
    void appendNote(const QString& text);

    /// Appends a diagnostic, coloured by `severity` as `DiagnosticSeverity` numbers it.
    void appendDiagnostic(int severity, const QString& text, const QString& file, int line);

    void clearTranscript();

private:
    /// Appends `text` in `colour`, scrolling to the end only when the view was already there.
    void append(const QString& text, const QColor& colour, bool ownLine);

    const Theme* theme;
    QPlainTextEdit* output = nullptr;
    QLineEdit* entry = nullptr;

    /// Whatever the kernel wrote without a trailing newline, waiting for the rest of its line.
    QString partialLine;
};

} // namespace enzan::gui
