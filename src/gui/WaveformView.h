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

Q_SIGNALS:
    /// A cursor moved, or the active one changed.
    void cursorsChanged();

    /// The visible time range changed.
    void rangeChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

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
    void paintOneBitTrace(QPainter& painter, const QRect& area, const WaveRow& waveRow,
                          std::uint32_t record);
    void paintBusTrace(QPainter& painter, const QRect& area, const WaveRow& waveRow,
                       std::uint32_t record);
    void paintCursors(QPainter& painter);
    void paintBeyondExtent(QPainter& painter);

    /// Fills `samples` with one entry per column of `area`. Returns false when there is
    /// nothing to draw.
    [[nodiscard]] bool sampleRow(const QRect& area, std::uint32_t record);

    void syncScrollBar();
    void onScrollBarMoved(int value);

    /// Moves the active cursor to the time under `x`, if it is not locked.
    void moveActiveCursorTo(double x);

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
};

} // namespace enzan::gui
