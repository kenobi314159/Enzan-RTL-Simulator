// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/Theme.h"

#include <QString>
#include <QStringList>
#include <QWidget>

class QEvent;
class QLineEdit;
class QPlainTextEdit;

namespace enzan::gui {

/// The Transcript window: the kernel's output, its diagnostics, and the command entry.
///
/// **The entry sends a line of Tcl to the kernel and nothing else.** Section 8.1 commits to Tcl
/// with a Questa-style vocabulary as *the* control language, so the front end does not name
/// commands: it passes text to the interpreter the kernel already has, acting on the session
/// the window is showing. A set of commands invented here would be a second vocabulary to keep
/// in step with the first.
///
/// It keeps a history, walked with Up and Down, because a reader typing `run 100ns` does it
/// more than once and retyping it is the sort of friction a prompt exists to remove.
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

    /// Turns the entry on or off, with the reason shown in its placeholder when off.
    void setCommandsAvailable(bool available, const QString& reason = QString());

    // Public, matching `QObject`. Narrowing an override's visibility is legal and confusing:
    // Qt calls it through the base class either way. It also has to sit outside the signal
    // block below, or moc takes it for a signal and generates a definition of its own.
    /// Watches the entry for Up and Down, which walk the history.
    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    /// The reader typed a line and pressed Return.
    void commandEntered(const QString& script);

private:
    /// Appends `text` in `colour`, scrolling to the end only when the view was already there.
    void append(const QString& text, const QColor& colour, bool ownLine);

    /// Sends what was typed and remembers it.
    void submitCommand();

    /// Moves `offset` through the history: -1 is the previous line, +1 the next.
    void recallCommand(int offset);

    const Theme* theme;
    QPlainTextEdit* output = nullptr;
    QLineEdit* entry = nullptr;

    /// Lines typed, oldest first. Kept whole rather than trimmed: a session's worth of
    /// commands is a few dozen short strings.
    QStringList history;

    /// Where Up and Down are in the history, or `history.size()` when at the fresh line.
    int historyPosition = 0;

    /// Whatever the kernel wrote without a trailing newline, waiting for the rest of its line.
    QString partialLine;
};

} // namespace enzan::gui
