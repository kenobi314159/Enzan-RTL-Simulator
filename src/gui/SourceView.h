// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/Theme.h"

#include <QHash>
#include <QString>
#include <QWidget>

#include <cstdint>

class QComboBox;

namespace enzan::gui {

/// The Source window: HDL source with the current execution line and breakpoints.
///
/// Section 10.2 puts it in the layout and section 10.4 says what makes it a window rather than
/// a file viewer: a breakpoint on a line, the line the design is stopped on, and the controls
/// that step. Without those it would be a worse answer than the editor the reader already has,
/// which is why it did not exist before M3.
///
/// **The text comes from the kernel, never from the path.** The kernel is the end that knows
/// what it analysed, and it may be on another machine (section 2.2); a window that opened the
/// file itself would show whatever is at that path here, which is either the same file or a
/// quietly different one — and the second is a debugger pointing confidently at the wrong line.
/// So this widget is told its text and never reads a file.
///
/// **The gutter is a separate widget beside the text, not a margin inside it.** A
/// `QPlainTextEdit` scrolls its viewport by moving pixels, and a margin drawn inside it would
/// be repainted on every scrolled line; a sibling that is given the same scroll offset is
/// painted only when it changes. That is the same rule the wave window's row list follows
/// (section 10.3).
class SourceView : public QWidget {
    Q_OBJECT

public:
    /// `theme` must outlive the view.
    explicit SourceView(const Theme& theme, QWidget* parent = nullptr);

    /// Replaces the list of files the window offers, keeping the one on show if it is still
    /// there.
    void setFiles(const QStringList& files);

    /// Shows `path`, asking for its text through `fileRequested` when it is not held yet.
    void showFile(const QString& path);

    /// Supplies a file's text. `found` false leaves a note in place of the source.
    void setFileText(const QString& path, const QString& contents, bool found);

    [[nodiscard]] QString currentFile() const { return shownFile; }

    /// Puts the current-execution marker on `line` of `file`, or clears it when `line` is zero.
    ///
    /// Shows the file if it is not the one displayed, because the point of the marker is to be
    /// looked at: a reader who stepped into another module means to see that module.
    void setCurrentLine(const QString& file, std::uint32_t line);

    /// One breakpoint, as the window draws it.
    struct Marker {
        std::uint32_t id = 0;
        bool enabled = true;
    };

    /// Replaces every marker. The list is sent whole by the kernel, so it is taken whole.
    void setBreakpoints(const QHash<QString, QHash<std::uint32_t, Marker>>& byFile);

    /// Width the gutter needs for the file on show, so that the layout can be asked for it.
    [[nodiscard]] int gutterWidth() const;

Q_SIGNALS:
    /// The window needs a file's text and does not have it.
    void fileRequested(const QString& path);

    /// The reader clicked the gutter on a line with no breakpoint.
    void breakpointRequested(const QString& file, std::uint32_t line);

    /// The reader clicked the gutter on a line that has one.
    void breakpointRemoved(std::uint32_t id);

private:
    class Gutter;

    /// The text area, with the block geometry a gutter needs made reachable.
    ///
    /// `QPlainTextEdit` keeps `firstVisibleBlock`, `contentOffset` and the block rectangles
    /// protected, on the assumption that whoever needs them is drawing inside the widget. A
    /// gutter beside it needs the same numbers and must not be inside it (see the class
    /// comment), so they are published here rather than the layout being changed to suit Qt.
    class TextArea;

    /// Repaints the gutter and keeps it the width the line numbers need.
    void refreshGutter();

    void paintGutter(QPaintEvent* event);

    /// Turns a click in the gutter into a line number, or zero.
    [[nodiscard]] std::uint32_t lineAt(int y) const;

    /// The marker on `line` of the file on show, or nothing.
    [[nodiscard]] const Marker* markerAt(std::uint32_t line) const;

    /// Draws the wash behind the line the design is stopped on.
    void refreshCurrentLine();

    const Theme* theme;
    QComboBox* chooser = nullptr;
    TextArea* text = nullptr;
    Gutter* gutter = nullptr;

    QString shownFile;

    /// Text by path, so that stepping back and forth between two modules does not ask the
    /// kernel again. A design's sources are small next to a waveform and the kernel cannot
    /// change them while it is running.
    QHash<QString, QString> cachedText;

    QHash<QString, QHash<std::uint32_t, Marker>> markers;

    QString stoppedInFile;
    std::uint32_t currentLine = 0;

    /// True while `showFile` is changing the chooser, so that the change does not read as the
    /// reader picking something.
    bool updatingChooser = false;
};

} // namespace enzan::gui
