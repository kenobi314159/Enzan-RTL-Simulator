// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/KernelSession.h"
#include "gui/Theme.h"
#include "gui/TimeAxis.h"
#include "gui/WaveformModel.h"
#include "runtime/Time.h"
#include "wavedb/WaveformDatabase.h"

#include <QPoint>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <vector>

class QScrollBar;
class QTreeView;

namespace enzan::gui {

/// One named cursor in the wave window.
struct WaveCursor {
    QString name;
    runtime::SimTime time = 0;

    /// A locked cursor stays where it is when the user clicks elsewhere.
    ///
    /// Section 10.3 requires cursor locking, and this is what it is for: a reader pins a cursor
    /// to the edge they are measuring from and then moves another one around without having to
    /// be careful where they click.
    bool locked = false;

    /// Where the cursor's label was last drawn, for hit testing. Filled by painting rather
    /// than computed twice: the label's width depends on the font and on the time in it, and
    /// two answers to where it is would eventually disagree.
    QRect labelRect;
};

/// The waveform canvas.
///
/// **Rendering cost is a function of visible pixel columns, not of the number of value changes
/// in the visible range** (DESIGN_GUIDE.md section 10.3). Each trace is drawn by asking the
/// waveform database for one summary per pixel column, which it answers from its resident time
/// index by binary search; a column spanning a million transitions costs what an empty one
/// costs. Values are read only for columns quiet enough to print one, which bounds those reads
/// by the pixel count too. Getting this wrong makes the viewer unusable on exactly the large
/// designs Enzan is for, and it is very hard to retrofit, which is why the database was built
/// for it before a single pixel was drawn.
///
/// Row geometry comes from the tree view beside it rather than being computed here. That is
/// what keeps the names and the traces aligned through scrolling, collapsing a group and a
/// change of font, none of which this widget then has to know about.
class WaveformView : public QWidget {
    Q_OBJECT

public:
    /// All three must outlive the view.
    WaveformView(const KernelSession& session, const WaveformModel& model, const Theme& theme,
                 QWidget* parent = nullptr);

    /// The tree view whose rows this canvas lines up with.
    void setRowView(QTreeView* rowView);

    [[nodiscard]] TimeAxis& axis() noexcept { return timeAxis; }

    [[nodiscard]] const TimeAxis& axis() const noexcept { return timeAxis; }

    /// Brings the axis up to date with how far the kernel has simulated, and follows the tail
    /// when following is on.
    void extentChanged();

    void zoomFit();
    void zoomIn();
    void zoomOut();

    /// Shows the span between the two most recently used cursors.
    void zoomToCursors();

    /// Keeps the right-hand edge at the latest simulated time while a run is in progress.
    void setFollowTail(bool follow);

    [[nodiscard]] bool followsTail() const noexcept { return following; }

    // --- cursors -----------------------------------------------------------

    [[nodiscard]] const QVector<WaveCursor>& cursors() const noexcept { return cursorList; }

    int addCursor(const QString& name, runtime::SimTime time);

    void removeCursor(int index);

    void setActiveCursor(int index);

    [[nodiscard]] int activeCursor() const noexcept { return active; }

    void setCursorTime(int index, runtime::SimTime time);

    void setCursorLocked(int index, bool locked);

    /// The active cursor's time, which is what the value columns read at.
    [[nodiscard]] runtime::SimTime activeCursorTime() const;

    /// Moves the active cursor to the next transition of the selected signal, or to the
    /// previous one. Does nothing without a selected signal, or past its last edge.
    void stepActiveCursorToEdge(bool forward);

    /// The wave row the signal list has selected, or -1.
    [[nodiscard]] int selectedRow() const;

    /// Pixels a click may miss an edge by and still snap to it.
    ///
    /// In pixels rather than time units so that the gesture behaves the same at every zoom:
    /// close enough to click is close enough to snap (section 10.7).
    static constexpr double SnapPixels = 6.0;

    /// Pixels a right-button drag must cover before it counts as a gesture at all. Below it a
    /// right-click is a right-click, and must not throw away the reader's zoom.
    static constexpr int MinimumDragPixels = 5;

Q_SIGNALS:
    /// A cursor moved, or the active one changed.
    void cursorsChanged();

    /// The visible time range changed.
    void rangeChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    /// Consumes Tab and Shift+Tab, which Qt would otherwise spend on focus navigation.
    ///
    /// Overridden here rather than handled in `keyPressEvent`, which never sees them: a widget
    /// that wants Tab for itself has to say so at this level.
    bool focusNextPrevChild(bool next) override;

public:
    /// Takes Tab from the signal list, so the key means the same thing on either side of the
    /// window. Public because `QObject` declares it so, and narrowing an override's visibility
    /// is legal and confusing.
    bool eventFilter(QObject* watched, QEvent* event) override;

protected:
private:
    /// Where one model row sits on this canvas.
    struct RowGeometry {
        int rowId = -1;
        int top = 0;
        int height = 0;
    };

    /// The rows the tree view is currently showing, in its order and at its positions.
    [[nodiscard]] std::vector<RowGeometry> visibleRows() const;

    [[nodiscard]] int rulerHeight() const;

    /// The area traces are drawn in, below the ruler and above the scroll bar.
    [[nodiscard]] QRect traceArea() const;

    void paintRuler(QPainter& painter);
    void paintRow(QPainter& painter, const RowGeometry& geometry);
    void paintOneBitTrace(QPainter& painter, const QRect& area, int rowId);
    void paintBusTrace(QPainter& painter, const QRect& area, int rowId);
    void paintCursors(QPainter& painter);
    void paintBeyondExtent(QPainter& painter);
    void paintRowBackground(QPainter& painter, const RowGeometry& geometry, const QRect& area);
    void paintZoomBand(QPainter& painter);

    /// Fills `samples` with one entry per column of `area`. Returns false when there is
    /// nothing to draw.
    [[nodiscard]] bool sampleRow(int rowId, const QRect& area);

    /// Summarises one row into `out`, one entry per column.
    ///
    /// A row with children is summarised by merging theirs: it changes wherever any of them
    /// does, and is constant only where all of them are. That costs what its children cost put
    /// together, which is the price of the value on its line and is why a row whose children
    /// are simply its own bits is read from its own trace instead (section 10.7).
    [[nodiscard]] bool sampleInto(int rowId, int columns, std::vector<wavedb::Sample>& out) const;

    void syncScrollBar();
    void onScrollBarMoved(int value);

    /// Moves the active cursor to the time under `x`, if it is not locked.
    void moveActiveCursorTo(double x);

    /// Applies a change of selection in the list, which means the same as a click on the row.
    void onRowSelected();

    /// Index of the cursor whose label covers `point`, or -1.
    [[nodiscard]] int cursorLabelAt(const QPoint& point) const;

    /// The model row drawn at `y`, or -1.
    [[nodiscard]] int rowAt(int y) const;

    /// Handles a click in the trace area: selects the row, moves the cursor and snaps it.
    void pickAt(const QPoint& point);

    /// Snaps `time` to a transition of `rowId` within `SnapPixels`, or returns it unchanged.
    [[nodiscard]] runtime::SimTime snapToEdge(int rowId, runtime::SimTime time) const;

    /// Asks the reader for a new name for a cursor.
    void renameCursor(int index);

    /// Opens a cursor's own menu — rename, lock or unlock, remove — at `where`.
    void showCursorMenu(int index, const QPoint& where);

    const KernelSession* kernelSession;
    const WaveformModel* waveModel;
    const Theme* theme;
    QTreeView* rows = nullptr;
    QScrollBar* timeScrollBar = nullptr;

    TimeAxis timeAxis;
    bool following = true;

    QVector<WaveCursor> cursorList;
    int active = 0;
    int previouslyActive = 0;

    /// Reused across rows and repaints, because a repaint allocating one of these per trace is
    /// how a view that is fast in principle becomes slow in practice.
    std::vector<wavedb::Sample> samples;

    bool panning = false;
    QPoint panFrom;

    /// Set while the right button is down. The two x coordinates are the whole gesture: their
    /// order decides whether the release zooms to a range or to fit.
    bool zoomDragging = false;
    int zoomFromX = 0;
    int zoomToX = 0;

    /// True while this view is the one changing the list's selection, so that the change does
    /// not come back round and act a second time.
    bool applyingSelection = false;
};

} // namespace enzan::gui
