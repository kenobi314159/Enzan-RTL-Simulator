// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/KernelSession.h"
#include "gui/ShortcutSheet.h"
#include "gui/Theme.h"
#include "gui/TimeAxis.h"
#include "gui/WaveformModel.h"
#include "runtime/Time.h"
#include "wavedb/WaveformDatabase.h"

#include <QHash>
#include <QPixmap>
#include <QPoint>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <span>
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

    /// Moves the view one step along the time axis, towards later times when `later` is set.
    void panStep(bool later);

    /// Keeps the right-hand edge at the latest simulated time while a run is in progress.
    void setFollowTail(bool follow);

    [[nodiscard]] bool followsTail() const noexcept { return following; }

    // --- cursors -----------------------------------------------------------

    [[nodiscard]] const QVector<WaveCursor>& cursors() const noexcept { return cursorList; }

    void updateActive();

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

    /// Moves the active cursor to `time` and brings it into view. Does nothing to a locked
    /// cursor, which stays where it was pinned whatever moves it.
    void placeActiveCursor(runtime::SimTime time);

    /// Makes the cursor to the right of the active one active, or the one to its left, and
    /// brings it into view.
    ///
    /// By time rather than by the order the cursors were made in, because the reader sees them
    /// on an axis and the keys are named for directions on it. Nothing happens past the last
    /// one either way: going round would make the key that reaches the leftmost cursor also the
    /// one that jumps to the rightmost.
    void activateNeighbourCursor(bool later);

    /// The wave row the signal list has selected, or -1.
    [[nodiscard]] int selectedRow() const;

    /// What the mouse does in the wave window, and the keys the signal list answers to there.
    ///
    /// The shortcut sheet lists them beside the window's actions. Neither is an action it could
    /// read them from, so they are written here, beside the handlers that do them, and follow
    /// section 10.7.
    [[nodiscard]] static QVector<ShortcutEntry> gestures();

    /// Pixels a click may miss an edge by and still snap to it.
    ///
    /// In pixels rather than time units so that the gesture behaves the same at every zoom:
    /// close enough to click is close enough to snap (section 10.7).
    static constexpr double SnapPixels = 6.0;

    /// Pixels a right-button drag must cover before it counts as a gesture at all. Below it a
    /// right-click is a right-click, and must not throw away the reader's zoom.
    static constexpr int MinimumDragPixels = 10;

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

    /// Hands a key to the signal list beside it.
    ///
    /// The names and the traces are one window, so a key means on the traces what it means on
    /// the names: moving between rows, packing and unpacking, and finding a row by typing the
    /// start of its name (section 10.7). The window's own keys are actions of the whole Wave
    /// tab and never arrive here.
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

    /// How many row summaries have been computed since the view was made.
    ///
    /// A row's summary is one pass over the width of the window, and it is the work the cache
    /// exists to avoid; this is what lets a test say "scrolling one line summarised one row
    /// rather than forty" exactly. The alternative is to time a repaint, and a timing test is
    /// one that fails on a loaded build machine for reasons that have nothing to do with the
    /// change it is meant to be guarding.
    [[nodiscard]] std::uint64_t summariesComputed() const noexcept { return summaryCount; }

    /// How many row traces have been drawn since the view was made, rather than blitted from the
    /// picture drawn before.
    ///
    /// Drawing is the other half of a repaint's cost, and the half that reads a value for every
    /// segment it labels; this is what lets a test say a click drew no trace again.
    [[nodiscard]] std::uint64_t tracesDrawn() const noexcept { return traceCount; }

    /// How far the last scroll moved what was already drawn, in pixels.
    ///
    /// Exposed because this is the one thing here that can be *wrong* rather than merely slow:
    /// a blit by the wrong amount leaves traces against the wrong names and the window looks
    /// plausible while lying. Reading the moved pixels back is not possible — asking a widget
    /// for its picture re-renders it — so what a test can check is that the distance moved was
    /// the distance the list moved.
    [[nodiscard]] int lastScrollShift() const noexcept { return lastShift; }

protected:
private:
    /// One row's column summary and the picture drawn from it, with the repaint each was last
    /// wanted for.
    struct CachedSamples {
        std::vector<wavedb::Sample> samples;

        /// The row's trace, drawn once and then blitted.
        ///
        /// Summarising a row was only half of what it cost; the other half is the drawing,
        /// which for a bus is a segment, a value measured against the font and a string laid
        /// out for every run of columns across the window. None of that depends on where the
        /// row sits, so scrolling can put the same picture one line further up instead of
        /// drawing it again.
        QPixmap trace;

        /// The height `trace` was drawn at, since a row that changed height has to be redrawn.
        int traceHeight = 0;

        int lastPaint = 0;
    };

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
    void paintOneBitTrace(QPainter& painter, const QRect& area, int rowId,
                          std::span<const wavedb::Sample> samples);
    void paintBusTrace(QPainter& painter, const QRect& area, int rowId,
                       std::span<const wavedb::Sample> samples);
    void paintCursors(QPainter& painter);
    void paintBeyondExtent(QPainter& painter);
    void paintRowBackground(QPainter& painter, const RowGeometry& geometry, const QRect& area);
    void paintZoomBand(QPainter& painter);

    /// The cache entry for `rowId`, with its summary filled in, or nullptr when there is
    /// nothing to draw. Valid until the next call.
    [[nodiscard]] CachedSamples* sampleRow(int rowId, const QRect& area);

    /// Moves what is already drawn instead of drawing it again, when the list scrolls.
    ///
    /// A vertical scroll does not change a single trace; it changes where each one sits. The
    /// pixels are already correct one line further down, so they are blitted and only the
    /// strip that has come into view is painted — which is what `QTreeView` does for its own
    /// rows and the reason the list always felt cheaper than the canvas beside it.
    void onRowsScrolled();

    /// How far the list has moved since the canvas last agreed with it, or nothing when that
    /// cannot be told and the canvas must be repainted instead.
    ///
    /// Measured from the tree rather than worked out from the scroll bar: the shift is asked
    /// of a row that was on screen, by comparing where it is now with where the canvas last
    /// drew it. Deriving it from the bar's value instead would mean knowing whether it counts
    /// items or pixels and whether every row is the same height — three assumptions, each of
    /// which would put the traces against the wrong names if it were ever wrong.
    [[nodiscard]] std::optional<int> listShift() const;

    /// True for a row whose value is a constant of the elaborated design rather than a trace.
    [[nodiscard]] bool isParameterRow(int rowId) const;

    /// Throws away every cached summary and every drawn trace.
    ///
    /// Called when the rows themselves change: a bus that has lost a bit summarises to a
    /// different shape, not merely to a different picture.
    void invalidateSamples();

    /// Throws away the drawn traces, keeping the summaries.
    ///
    /// For a change to what a row *shows* rather than to what it is — a radix, a highlight,
    /// the value written on a segment. The shape is still right, so the expensive half of the
    /// work is kept and only the drawing is done again.
    void invalidateTraces();

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

    /// Scrolls the view so that `time` is in it, centred, unless it already is.
    void bringIntoView(runtime::SimTime time);

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

    /// What each row summarises to, kept between repaints.
    ///
    /// Summarising a row costs one pass over the width of the window, and the answer depends
    /// only on the row, the time range and that width — none of which a vertical scroll
    /// changes. Without this, scrolling one line recomputed every row on screen to redraw the
    /// same traces one line up, which is why the window slowed down in proportion to its own
    /// height. Entries are dropped once a repaint goes by without wanting them, so what is
    /// held is proportional to what is on screen rather than to the length of the list.
    QHash<int, CachedSamples> sampleCache;

    /// What the cache was computed against. A repaint whose range, width or recorded extent
    /// differs from these throws the whole cache away, which is what makes zooming, scrolling
    /// sideways and a running kernel correct without any of them having to remember to say so.
    runtime::SimTime cachedStart = 0;
    runtime::SimTime cachedEnd = 0;
    int cachedColumns = 0;
    runtime::SimTime cachedExtent = 0;

    /// Counts summaries computed, for `summariesComputed`.
    std::uint64_t summaryCount = 0;

    /// Counts traces drawn, for `tracesDrawn`.
    std::uint64_t traceCount = 0;

    /// A row the canvas has drawn, and where it drew it, so that the next scroll can measure
    /// how far the list has moved by asking where that row is now.
    int anchorRowId = -1;
    int anchorTop = 0;

    /// How far the last scroll moved the contents, for the test that says it matched the list.
    int lastShift = 0;

    /// Counts repaints, so an entry knows how long it has been since it was wanted.
    int paintCounter = 0;

    /// How many repaints an unwanted summary survives. Two rather than one so that a repaint
    /// which happens to draw nothing — a window momentarily too short, a model mid-edit — does
    /// not throw away everything the next one is about to ask for.
    static constexpr int SampleCacheGrace = 2;

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
