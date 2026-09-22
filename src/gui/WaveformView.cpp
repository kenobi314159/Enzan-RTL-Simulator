// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/WaveformView.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <optional>

namespace enzan::gui {

namespace {

/// Vertical inset of a trace inside its row, so that neighbouring rows do not touch.
constexpr int TraceMargin = 3;

/// How wide a constant run has to be before its value is written on it.
constexpr int MinimumLabelPixels = 28;

/// Width of the slanted shoulder at each end of a bus segment.
constexpr int BusShoulder = 3;

/// How wide a bus segment has to be before it is drawn with shoulders rather than square ends.
constexpr int MinimumShoulderedPixels = 4 * BusShoulder;

/// Narrowest gap between ruler labels. Below this they collide and the ruler stops being
/// readable, which is the only thing it is for.
constexpr double MinimumGridPixels = 90.0;

/// How much one wheel notch zooms.
constexpr double ZoomStep = 1.25;

/// Positions the scroll bar can take. The time range is 64-bit and a scroll bar is not, so the
/// bar works in fractions of the scrollable span rather than in time units.
constexpr int ScrollResolution = 10'000;

} // namespace

WaveformView::WaveformView(const KernelSession& session, const WaveformModel& model,
                           const Theme& themeToUse, QWidget* parent)
    : QWidget(parent), kernelSession(&session), waveModel(&model), theme(&themeToUse) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAutoFillBackground(false);

    cursorList.push_back(WaveCursor{.name = tr("Cursor 1"), .time = 0, .locked = false});

    timeScrollBar = new QScrollBar(Qt::Horizontal, this);
    timeScrollBar->setRange(0, ScrollResolution);
    connect(timeScrollBar, &QScrollBar::valueChanged, this, &WaveformView::onScrollBarMoved);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch(1);
    layout->addWidget(timeScrollBar);

    timeAxis.setRange(0, 1000);
}

void WaveformView::setRowView(QTreeView* rowView) {
    rows = rowView;
    if (rows == nullptr) return;

    // The canvas has no scroll state of its own vertically; it mirrors the tree's, which is
    // what keeps a name and its trace on the same line through every kind of scrolling.
    connect(rows->verticalScrollBar(), &QScrollBar::valueChanged, this,
            qOverload<>(&QWidget::update));
    connect(rows->header(), &QHeaderView::geometriesChanged, this, qOverload<>(&QWidget::update));
}

int WaveformView::rulerHeight() const {
    // Matched to the tree's header so that the first trace lines up with the first name.
    if (rows != nullptr && rows->header() != nullptr) return rows->header()->height();
    return QFontMetrics(font()).height() + 8;
}

QRect WaveformView::traceArea() const {
    const int top = rulerHeight();
    const int bottom = height() - (timeScrollBar != nullptr ? timeScrollBar->height() : 0);
    return {0, top, width(), std::max(0, bottom - top)};
}

std::vector<WaveformView::RowGeometry> WaveformView::visibleRows() const {
    std::vector<RowGeometry> geometry;
    if (rows == nullptr) return geometry;

    const QRect area = traceArea();
    const int viewportHeight = rows->viewport()->height();

    int y = 0;
    while (y < viewportHeight) {
        const QModelIndex index = rows->indexAt(QPoint(1, y));
        if (!index.isValid()) break;

        const QRect rect = rows->visualRect(index);
        if (rect.height() <= 0) break;

        geometry.push_back(RowGeometry{.rowId = waveModel->rowIdOf(index),
                                       .top = area.top() + rect.top(),
                                       .height = rect.height()});
        y = rect.bottom() + 1;
    }

    return geometry;
}

// ---------------------------------------------------------------------------
// The view range
// ---------------------------------------------------------------------------

void WaveformView::extentChanged() {
    timeAxis.setExtent(kernelSession->latestTime());
    if (following) timeAxis.followTail();
    syncScrollBar();
    update();
}

void WaveformView::zoomFit() {
    timeAxis.setExtent(kernelSession->latestTime());
    timeAxis.fit();
    following = false;
    syncScrollBar();
    update();
    Q_EMIT rangeChanged();
}

void WaveformView::zoomIn() {
    timeAxis.zoom(1.0 / ZoomStep, traceArea().width() / 2.0);
    syncScrollBar();
    update();
    Q_EMIT rangeChanged();
}

void WaveformView::zoomOut() {
    timeAxis.zoom(ZoomStep, traceArea().width() / 2.0);
    syncScrollBar();
    update();
    Q_EMIT rangeChanged();
}

void WaveformView::zoomToCursors() {
    if (cursorList.size() < 2) return;

    const int first = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
    const int second = std::clamp(previouslyActive, 0, static_cast<int>(cursorList.size()) - 1);
    if (first == second) return;

    timeAxis.showRange(cursorList[first].time, cursorList[second].time);
    following = false;
    syncScrollBar();
    update();
    Q_EMIT rangeChanged();
}

void WaveformView::setFollowTail(bool follow) {
    following = follow;
    if (following) {
        timeAxis.followTail();
        syncScrollBar();
        update();
        Q_EMIT rangeChanged();
    }
}

void WaveformView::syncScrollBar() {
    if (timeScrollBar == nullptr) return;

    const runtime::SimTime extent = std::max(timeAxis.extent(), timeAxis.span());
    const runtime::SimTime scrollable = extent > timeAxis.span() ? extent - timeAxis.span() : 0;

    const QSignalBlocker blocker(timeScrollBar);
    timeScrollBar->setEnabled(scrollable > 0);
    if (scrollable == 0) {
        timeScrollBar->setValue(0);
        return;
    }

    // In fractions of the scrollable span, because a 64-bit time range does not fit in the
    // integer a scroll bar works in.
    const double fraction = static_cast<double>(timeAxis.start()) / static_cast<double>(scrollable);
    timeScrollBar->setValue(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * ScrollResolution));
}

void WaveformView::onScrollBarMoved(int value) {
    const runtime::SimTime extent = std::max(timeAxis.extent(), timeAxis.span());
    const runtime::SimTime scrollable = extent > timeAxis.span() ? extent - timeAxis.span() : 0;
    if (scrollable == 0) return;

    const double fraction = static_cast<double>(value) / ScrollResolution;
    timeAxis.setRange(static_cast<runtime::SimTime>(fraction * static_cast<double>(scrollable)),
                      timeAxis.span());
    following = false;
    update();
    Q_EMIT rangeChanged();
}

// ---------------------------------------------------------------------------
// Cursors
// ---------------------------------------------------------------------------

int WaveformView::addCursor(const QString& name, runtime::SimTime time) {
    cursorList.push_back(WaveCursor{.name = name, .time = time, .locked = false});
    setActiveCursor(static_cast<int>(cursorList.size()) - 1);
    return static_cast<int>(cursorList.size()) - 1;
}

void WaveformView::removeCursor(int index) {
    // The last cursor stays. A wave window with no cursor cannot answer "what is this signal
    // here", which is most of what the window is for.
    if (index < 0 || index >= cursorList.size() || cursorList.size() == 1) return;

    cursorList.remove(index);
    active = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
    previouslyActive = std::clamp(previouslyActive, 0, static_cast<int>(cursorList.size()) - 1);
    update();
    Q_EMIT cursorsChanged();
}

void WaveformView::setActiveCursor(int index) {
    if (index < 0 || index >= cursorList.size() || index == active) return;

    previouslyActive = active;
    active = index;
    update();
    Q_EMIT cursorsChanged();
}

void WaveformView::setCursorTime(int index, runtime::SimTime time) {
    if (index < 0 || index >= cursorList.size()) return;

    cursorList[index].time = time;
    update();
    Q_EMIT cursorsChanged();
}

void WaveformView::setCursorLocked(int index, bool locked) {
    if (index < 0 || index >= cursorList.size()) return;

    cursorList[index].locked = locked;
    update();
    Q_EMIT cursorsChanged();
}

int WaveformView::selectedRow() const {
    if (rows == nullptr) return -1;
    return waveModel->rowIdOf(rows->currentIndex());
}

int WaveformView::rowAt(int y) const {
    for (const RowGeometry& geometry : visibleRows()) {
        if (y >= geometry.top && y < geometry.top + geometry.height) return geometry.rowId;
    }
    return -1;
}

int WaveformView::cursorLabelAt(const QPoint& point) const {
    // Walked backwards so that the label drawn last — and so on top — is the one a click on
    // two overlapping labels picks.
    for (int index = static_cast<int>(cursorList.size()); index-- > 0;) {
        if (cursorList[index].labelRect.contains(point)) return index;
    }
    return -1;
}

runtime::SimTime WaveformView::snapToEdge(int rowId, runtime::SimTime time) const {
    const WaveRow* waveRow = waveModel->row(rowId);
    if (waveRow == nullptr || waveRow->kind != WaveRow::Kind::Signal) return time;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();
    const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) return time;

    const std::uint64_t nearest = database.nearestChange(record, time);
    if (nearest == wavedb::NoChange) return time;

    // The radius is a pixel distance, so it is compared as one. A window of time units would
    // snap from further and further away as the view zoomed out.
    const runtime::SimTime edge = database.timeOfChange(record, nearest);
    if (std::abs(timeAxis.xOf(edge) - timeAxis.xOf(time)) > SnapPixels) return time;

    return edge;
}

void WaveformView::stepActiveCursorToEdge(bool forward) {
    const int rowId = selectedRow();
    const WaveRow* waveRow = waveModel->row(rowId);
    if (waveRow == nullptr || waveRow->kind != WaveRow::Kind::Signal) return;
    if (cursorList.isEmpty()) return;

    const int index = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
    if (cursorList[index].locked) return;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();
    const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) return;

    const runtime::SimTime from = cursorList[index].time;
    const std::uint64_t change = forward ? database.nextChangeAfter(record, from)
                                         : database.previousChangeBefore(record, from);
    if (change == wavedb::NoChange) return;

    cursorList[index].time = database.timeOfChange(record, change);

    // The reader is stepping towards something they want to look at, so the view goes with
    // them rather than leaving the cursor off the side of the window.
    if (cursorList[index].time < timeAxis.start() || cursorList[index].time > timeAxis.end()) {
        timeAxis.setRange(cursorList[index].time > timeAxis.span() / 2
                              ? cursorList[index].time - (timeAxis.span() / 2)
                              : 0,
                          timeAxis.span());
        following = false;
        syncScrollBar();
        Q_EMIT rangeChanged();
    }

    update();
    Q_EMIT cursorsChanged();
}

void WaveformView::pickAt(const QPoint& point) {
    const int rowId = rowAt(point.y());

    // Selecting through the tree rather than keeping a selection of our own: the names and the
    // traces are one row to the reader, and two ideas of which row is current would drift.
    if (rowId >= 0 && rows != nullptr) {
        const QModelIndex index = waveModel->indexOfRow(rowId);
        if (index.isValid()) rows->setCurrentIndex(index);
    }

    if (cursorList.isEmpty()) return;

    const int index = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
    if (cursorList[index].locked) return;

    cursorList[index].time = snapToEdge(rowId, timeAxis.timeAt(point.x()));
    update();
    Q_EMIT cursorsChanged();
}

void WaveformView::renameCursor(int index) {
    if (index < 0 || index >= cursorList.size()) return;

    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, tr("Rename Cursor"), tr("Cursor name:"), QLineEdit::Normal,
                              cursorList[index].name, &accepted);
    if (!accepted || name.isEmpty()) return;

    cursorList[index].name = name;
    update();
    Q_EMIT cursorsChanged();
}

runtime::SimTime WaveformView::activeCursorTime() const {
    if (cursorList.isEmpty()) return 0;
    return cursorList[std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1)].time;
}

void WaveformView::moveActiveCursorTo(double x) {
    if (cursorList.isEmpty()) return;

    const int index = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
    if (cursorList[index].locked) return;

    cursorList[index].time = timeAxis.timeAt(x);
    update();
    Q_EMIT cursorsChanged();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void WaveformView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    timeAxis.setWidth(traceArea().width());
    if (following) timeAxis.followTail();
    syncScrollBar();
    Q_EMIT rangeChanged();
}

void WaveformView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), theme->color(ThemeColor::WaveBackground));

    timeAxis.setWidth(traceArea().width());

    paintBeyondExtent(painter);
    paintRuler(painter);

    // Traces are clipped at the point the kernel has simulated to. The value in effect at the
    // end of a run would otherwise be drawn straight across the part of the window the design
    // has not reached, which reads as "this signal is flat here" rather than "nothing has
    // happened here yet" — a distinction that matters while a run is in progress.
    QRect drawable = traceArea();
    const double extentX = timeAxis.xOf(kernelSession->latestTime());
    if (extentX < drawable.right()) {
        drawable.setRight(std::max(drawable.left(), static_cast<int>(std::ceil(extentX))));
    }

    // Backgrounds first and unclipped: a highlight marks the whole row, including the part of
    // it the design has not been simulated to yet.
    const QRect area = traceArea();
    painter.save();
    painter.setClipRect(area);
    for (const RowGeometry& geometry : visibleRows()) paintRowBackground(painter, geometry, area);
    painter.restore();

    painter.save();
    painter.setClipRect(drawable);
    for (const RowGeometry& geometry : visibleRows()) paintRow(painter, geometry);
    painter.restore();

    paintZoomBand(painter);
    paintCursors(painter);
}

void WaveformView::paintRowBackground(QPainter& painter, const RowGeometry& geometry,
                                      const QRect& area) {
    const WaveRow* waveRow = waveModel->row(geometry.rowId);
    if (waveRow == nullptr) return;

    const QRect rowRect(area.left(), geometry.top, area.width(), geometry.height);
    if (rowRect.bottom() < area.top() || rowRect.top() > area.bottom()) return;

    if (waveRow->highlight > 0) {
        painter.fillRect(rowRect, theme->color(highlightColor(waveRow->highlight)));
    }

    // The selected row is tinted too, because it is what Tab steps along and what a click just
    // picked: a reader has to be able to see which one that is.
    if (geometry.rowId == selectedRow()) {
        painter.fillRect(rowRect, theme->color(ThemeColor::WaveSelectedRow));
    }
}

void WaveformView::paintZoomBand(QPainter& painter) {
    if (!zoomDragging) return;

    const int travelled = zoomToX - zoomFromX;
    if (std::abs(travelled) < MinimumDragPixels) return;

    const QRect area = traceArea();

    // Only a backwards drag has a range to show. A forwards one is about to zoom to fit, and
    // shading the part of the window it swept over would suggest it was going to frame that.
    if (travelled < 0) {
        const QRect band(zoomToX, area.top(), -travelled, area.height());
        painter.fillRect(band, theme->color(ThemeColor::WaveZoomBand));

        const runtime::SimTime from = timeAxis.timeAt(zoomToX);
        const runtime::SimTime to = timeAxis.timeAt(zoomFromX);
        painter.setPen(theme->color(ThemeColor::WaveRulerForeground));
        painter.drawText(band, Qt::AlignCenter,
                         TimeAxis::format(to - from, kernelSession->timeScale()));
        return;
    }

    painter.setPen(theme->color(ThemeColor::WaveRulerForeground));
    painter.drawText(QRect(zoomFromX, area.top(), travelled, area.height()), Qt::AlignCenter,
                     tr("zoom to fit"));
}

void WaveformView::paintBeyondExtent(QPainter& painter) {
    const runtime::SimTime extent = kernelSession->latestTime();
    if (extent >= timeAxis.end()) return;

    // Where the design has not been simulated to yet, shaded so that "nothing here" does not
    // read as "this signal is flat here".
    const double x = std::max(0.0, timeAxis.xOf(extent));
    const QRect area(static_cast<int>(x), 0, width() - static_cast<int>(x), height());
    painter.fillRect(area, theme->color(ThemeColor::WaveBeyondRecorded));
}

void WaveformView::paintRuler(QPainter& painter) {
    const QRect ruler(0, 0, width(), rulerHeight());
    painter.fillRect(ruler, theme->color(ThemeColor::WaveRulerBackground));

    const runtime::SimTime step = timeAxis.gridStep(MinimumGridPixels);
    if (step == 0) return;

    painter.setPen(theme->color(ThemeColor::WaveRulerForeground));
    const QFontMetrics metrics(font());

    // Started at the first multiple of the step at or after the left edge, so that the labels
    // are round numbers rather than an arbitrary offset from wherever the view happens to be.
    const runtime::SimTime first = (timeAxis.start() / step) * step;
    for (runtime::SimTime time = first; time <= timeAxis.end(); time += step) {
        const double x = timeAxis.xOf(time);
        if (x < -1.0 || x > width()) continue;

        const int column = static_cast<int>(x);
        painter.drawLine(column, ruler.bottom() - 4, column, ruler.bottom());
        painter.drawText(column + 3, metrics.ascent() + 2,
                         TimeAxis::format(time, kernelSession->timeScale()));

        // Guards against a step small enough that the loop would not terminate on overflow.
        if (time > runtime::NoTime - step) break;
    }

    painter.setPen(theme->color(ThemeColor::WaveGrid));
    const QRect area = traceArea();
    for (runtime::SimTime time = first; time <= timeAxis.end(); time += step) {
        const double x = timeAxis.xOf(time);
        if (x < 0.0 || x > width()) continue;
        painter.drawLine(static_cast<int>(x), area.top(), static_cast<int>(x), area.bottom());
        if (time > runtime::NoTime - step) break;
    }
}

bool WaveformView::sampleRow(const QRect& area, std::uint32_t record) {
    const int columns = area.width();
    if (columns <= 0) return false;
    if (timeAxis.span() == 0) return false;

    samples.assign(static_cast<std::size_t>(columns), wavedb::Sample{});
    kernelSession->waveforms().sample(record, timeAxis.start(), timeAxis.end(), samples);
    return true;
}

void WaveformView::paintRow(QPainter& painter, const RowGeometry& geometry) {
    const WaveRow* waveRow = waveModel->row(geometry.rowId);
    if (waveRow == nullptr) return;

    const QRect area = traceArea();
    const QRect rowRect(area.left(), geometry.top, area.width(), geometry.height);
    if (rowRect.bottom() < area.top() || rowRect.top() > area.bottom()) return;

    painter.setPen(theme->color(ThemeColor::WaveRowSeparator));
    painter.drawLine(rowRect.left(), rowRect.bottom(), rowRect.right(), rowRect.bottom());

    if (waveRow->kind == WaveRow::Kind::Group) return;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();
    const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) return;

    const QRect traceRect = rowRect.adjusted(0, TraceMargin, 0, -TraceMargin);
    if (traceRect.height() <= 2 || !sampleRow(traceRect, record)) return;

    const std::uint32_t displayedWidth =
        waveRow->sliced ? waveRow->highBit - waveRow->lowBit + 1 : waveRow->signalWidth;

    if (displayedWidth == 1) {
        paintOneBitTrace(painter, traceRect, *waveRow, record);
    } else {
        paintBusTrace(painter, traceRect, *waveRow, record);
    }
}

void WaveformView::paintOneBitTrace(QPainter& painter, const QRect& area, const WaveRow& waveRow,
                                    std::uint32_t record) {
    const wavedb::WaveformDatabase& database = kernelSession->waveforms();

    const int top = area.top();
    const int bottom = area.bottom();
    const int middle = (top + bottom) / 2;

    painter.setPen(theme->color(ThemeColor::WaveTrace));

    int previousY = -1;
    for (std::size_t column = 0; column < samples.size(); ++column) {
        const wavedb::Sample& sample = samples[column];
        const int x = area.left() + static_cast<int>(column);

        if (sample.transitions > 0) {
            // Busy: more than one transition inside this single pixel column. Drawn as a solid
            // bar rather than by iterating the transitions, which is the whole point — a column
            // covering a million edges costs exactly what this one does.
            painter.fillRect(QRect(x, top, 1, bottom - top + 1),
                             sample.anyUnknown ? theme->color(ThemeColor::WaveUnknown)
                                               : theme->color(ThemeColor::WaveTrace));
            previousY = -1;
            continue;
        }

        if (sample.changeIndex == wavedb::NoChange) {
            previousY = -1;
            continue;
        }

        const eir::BitVector whole = database.valueOfChange(record, sample.changeIndex);
        const eir::BitVector value = WaveformModel::slice(waveRow, whole);
        const eir::Logic bit = value.get(0);

        // An X or a Z sits on the midline in its own colour rather than being drawn as a
        // level. A value that is not zero or one must not read as either (section 3.9).
        int y = 0;
        QColor pen;
        switch (bit) {
        case eir::Logic::Zero:
            y = bottom;
            pen = theme->color(ThemeColor::WaveTrace);
            break;
        case eir::Logic::One:
            y = top;
            pen = theme->color(ThemeColor::WaveTrace);
            break;
        case eir::Logic::HighZ:
            y = middle;
            pen = theme->color(ThemeColor::WaveHighImpedance);
            break;
        case eir::Logic::Unknown:
            y = middle;
            pen = theme->color(ThemeColor::WaveUnknown);
            break;
        }

        painter.setPen(pen);
        if (previousY >= 0 && previousY != y) painter.drawLine(x, previousY, x, y);
        painter.drawPoint(x, y);
        previousY = y;
    }
}

void WaveformView::paintBusTrace(QPainter& painter, const QRect& area, const WaveRow& waveRow,
                                 std::uint32_t record) {
    const wavedb::WaveformDatabase& database = kernelSession->waveforms();

    const int top = area.top();
    const int bottom = area.bottom();
    const QFontMetrics metrics(font());

    // Constant columns are coalesced into runs so that a bus segment is drawn once with its
    // value on it, rather than as a row of identical one-pixel slivers.
    std::size_t column = 0;
    while (column < samples.size()) {
        const wavedb::Sample& sample = samples[column];
        const int x = area.left() + static_cast<int>(column);

        if (sample.transitions > 0) {
            std::size_t run = column;
            while (run < samples.size() && samples[run].transitions > 0) ++run;

            const int runWidth = static_cast<int>(run - column);
            painter.fillRect(QRect(x, top, runWidth, bottom - top + 1),
                             sample.anyUnknown ? theme->color(ThemeColor::WaveUnknown)
                                               : theme->color(ThemeColor::WaveTraceFill));
            column = run;
            continue;
        }

        if (sample.changeIndex == wavedb::NoChange) {
            ++column;
            continue;
        }

        std::size_t run = column;
        while (run < samples.size() && samples[run].transitions == 0
               && samples[run].changeIndex == sample.changeIndex) {
            ++run;
        }

        const int runWidth = static_cast<int>(run - column);
        const eir::BitVector whole = database.valueOfChange(record, sample.changeIndex);
        const eir::BitVector value = WaveformModel::slice(waveRow, whole);

        const QColor pen = value.ref().hasUnknown() ? theme->color(ThemeColor::WaveUnknown)
                                                    : theme->color(ThemeColor::WaveTrace);
        painter.setPen(pen);

        // A bus segment: two horizontal rails with slanted shoulders where the value changes,
        // which is the shape every waveform viewer draws and therefore the one users read
        // without having to learn anything.
        //
        // The shoulders are dropped once a segment is too narrow for them to be legible. Kept,
        // they would consume the whole segment and a bus changing every few pixels would draw
        // as a row of crosses — technically the right shape and unreadable as a trace.
        const int shoulder = runWidth >= MinimumShoulderedPixels ? BusShoulder : 0;
        const int left = x;
        const int right = x + runWidth - 1;
        painter.drawLine(left + shoulder, top, right - shoulder, top);
        painter.drawLine(left + shoulder, bottom, right - shoulder, bottom);
        if (shoulder > 0) {
            const int middle = (top + bottom) / 2;
            painter.drawLine(left, middle, left + shoulder, top);
            painter.drawLine(left, middle, left + shoulder, bottom);
            painter.drawLine(right, middle, right - shoulder, top);
            painter.drawLine(right, middle, right - shoulder, bottom);
        } else {
            painter.drawLine(left, top, left, bottom);
        }

        if (runWidth >= MinimumLabelPixels) {
            const QString text = QString::fromStdString(eir::toString(value.ref(), waveRow.radix));
            const QRect textRect(left + shoulder + 2, top, runWidth - (2 * shoulder) - 4,
                                 bottom - top + 1);
            painter.setPen(theme->color(ThemeColor::WaveText));
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                             metrics.elidedText(text, Qt::ElideRight, textRect.width()));
        }

        column = run;
    }
}

// Fills in each cursor's label rectangle as it draws, which is what `cursorLabelAt` then hits
// against; see the comment on `WaveCursor::labelRect`.
void WaveformView::paintCursors(QPainter& painter) {
    const QRect area = traceArea();
    const QFontMetrics metrics(font());

    for (int index = 0; index < cursorList.size(); ++index) {
        WaveCursor& cursor = cursorList[index];

        const double x = timeAxis.xOf(cursor.time);
        if (x < 0.0 || x > width()) {
            cursor.labelRect = QRect();
            continue;
        }

        const QColor colour = cursor.locked ? theme->color(ThemeColor::WaveCursorLocked)
                                            : theme->color(ThemeColor::WaveCursor);

        // The active cursor is the thick one. Which cursor a click moves has to be visible, or
        // the user finds out by moving the wrong one.
        painter.setPen(QPen(colour, index == active ? 2 : 1));
        painter.drawLine(static_cast<int>(x), 0, static_cast<int>(x), area.bottom());

        // The time goes on the label rather than in a status bar, where only the active
        // cursor's would fit. Reading two of them at once is the whole point of locking one.
        const QString label = QStringLiteral("%1  %2").arg(
            cursor.name, TimeAxis::format(cursor.time, kernelSession->timeScale()));

        // Tucked to the left of the line when a label at the right would run off the edge, so
        // that a cursor near the end of the window can still be read and clicked.
        const int textWidth = metrics.horizontalAdvance(label) + 10;
        const int left = static_cast<int>(x) + 1 + textWidth <= width()
                             ? static_cast<int>(x) + 1
                             : static_cast<int>(x) - textWidth;

        cursor.labelRect = QRect(std::max(0, left), 0, textWidth, rulerHeight());
        painter.fillRect(cursor.labelRect, colour);
        painter.setPen(theme->color(ThemeColor::WaveCursorText));
        painter.drawText(cursor.labelRect, Qt::AlignCenter, label);
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void WaveformView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);

    // Panning is the middle button's, and only the middle button's. Ctrl means *zoom* on the
    // wheel (section 10.7), and a modifier that meant zoom in one hand and pan in the other
    // would be a coin toss every time.
    if (event->button() == Qt::MiddleButton) {
        panning = true;
        panFrom = event->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (event->button() == Qt::RightButton) {
        zoomDragging = true;
        zoomFromX = event->pos().x();
        zoomToX = zoomFromX;
        return;
    }

    if (event->button() != Qt::LeftButton) return;

    // A click on a cursor's label picks that cursor rather than moving the current one, which
    // is how a reader swaps between them without reaching for a menu.
    const int label = cursorLabelAt(event->pos());
    if (label >= 0) {
        setActiveCursor(label);
        return;
    }

    following = false;
    pickAt(event->pos());
}

void WaveformView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    const int label = cursorLabelAt(event->pos());
    if (label < 0) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    setActiveCursor(label);
    renameCursor(label);
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    if (zoomDragging) {
        zoomToX = event->pos().x();
        update();
        return;
    }

    if (panning) {
        const int delta = panFrom.x() - event->pos().x();
        timeAxis.pan(delta);
        panFrom = event->pos();
        syncScrollBar();
        update();
        Q_EMIT rangeChanged();
        return;
    }

    // Scrubbing: the cursor follows the pointer, snapping as it goes, so that dragging along
    // a trace stops on its edges rather than between them.
    if (event->buttons().testFlag(Qt::LeftButton)) pickAt(event->pos());
}

void WaveformView::mouseReleaseEvent(QMouseEvent* event) {
    if (panning && event->button() == Qt::MiddleButton) {
        panning = false;
        unsetCursor();
        return;
    }

    if (!zoomDragging || event->button() != Qt::RightButton) return;

    zoomDragging = false;
    zoomToX = event->pos().x();
    const int travelled = zoomToX - zoomFromX;
    update();

    // A right-click is not a gesture. Without this, every mis-click while reaching for the
    // context menu would throw away the range the reader had zoomed to.
    if (std::abs(travelled) < MinimumDragPixels) return;

    if (travelled < 0) {
        // Swept back across what was just looked at: frame it.
        timeAxis.showRange(timeAxis.timeAt(zoomToX), timeAxis.timeAt(zoomFromX));
        following = false;
        syncScrollBar();
        update();
        Q_EMIT rangeChanged();
        return;
    }

    // Swept forward past the end: show everything.
    zoomFit();
}

void WaveformView::wheelEvent(QWheelEvent* event) {
    const int notches = event->angleDelta().y();
    if (notches == 0) {
        event->ignore();
        return;
    }

    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        timeAxis.zoom(notches > 0 ? 1.0 / ZoomStep : ZoomStep, event->position().x());
    } else if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        timeAxis.pan(notches > 0 ? -60.0 : 60.0);
    } else {
        // A bare wheel scrolls the list, wherever the pointer is. The names and the traces are
        // two widgets and one window, and a wheel that meant different things on either side
        // of the splitter would make an internal boundary decide what it did (section 10.7).
        // The event is handed to the tree so that it scrolls exactly as the tree does.
        if (rows != nullptr) QCoreApplication::sendEvent(rows->viewport(), event);
        return;
    }

    following = false;
    syncScrollBar();
    update();
    Q_EMIT rangeChanged();
    event->accept();
}

void WaveformView::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomIn(); return;
    case Qt::Key_Minus: zoomOut(); return;
    case Qt::Key_F: zoomFit(); return;
    case Qt::Key_Home: setCursorTime(active, 0); return;
    case Qt::Key_Left:
        timeAxis.pan(-60.0);
        following = false;
        syncScrollBar();
        update();
        Q_EMIT rangeChanged();
        return;
    case Qt::Key_Right:
        timeAxis.pan(60.0);
        following = false;
        syncScrollBar();
        update();
        Q_EMIT rangeChanged();
        return;
    default: QWidget::keyPressEvent(event); return;
    }
}

bool WaveformView::focusNextPrevChild(bool next) {
    // Tab measures rather than navigating. A wave window is read by pinning one cursor and
    // walking another along a signal's edges, and that is worth a key you can hold down.
    stepActiveCursorToEdge(next);
    return true;
}

} // namespace enzan::gui
