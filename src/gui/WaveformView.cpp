// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/WaveformView.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

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

    // Anything that changes which rows are on screen, or where they are, changes what the
    // canvas should be drawing. Expanding a bus, pasting, deleting a bit: each of these used
    // to leave the traces as they were until something else happened to force a repaint.
    connect(rows, &QTreeView::expanded, this, [this](const QModelIndex&) { update(); });
    connect(rows, &QTreeView::collapsed, this, [this](const QModelIndex&) { update(); });

    if (const QAbstractItemModel* model = rows->model(); model != nullptr) {
        const auto repaint = [this] { update(); };

        // A structural change can change what a row summarises to — a bit deleted from a bus,
        // a group gaining a member — so these throw the cached summaries away as well as
        // asking for a repaint. `dataChanged` deliberately does not: it fires on every cursor
        // move, and the cursor does not change the shape of a trace.
        const auto restructured = [this] {
            invalidateSamples();
            update();
        };

        connect(model, &QAbstractItemModel::rowsInserted, this, restructured);
        connect(model, &QAbstractItemModel::rowsRemoved, this, restructured);
        connect(model, &QAbstractItemModel::rowsMoved, this, restructured);
        connect(model, &QAbstractItemModel::modelReset, this, restructured);
        connect(model, &QAbstractItemModel::layoutChanged, this, restructured);
        connect(model, &QAbstractItemModel::dataChanged, this, repaint);
    }

    // Selecting a row in the list means the same thing as clicking its trace: it is the row
    // Tab steps along, and the cursor goes to its nearest edge. Without this, picking a signal
    // by name left the cursor behind and Tab appeared to do nothing.
    if (const QItemSelectionModel* selection = rows->selectionModel(); selection != nullptr) {
        connect(selection, &QItemSelectionModel::currentRowChanged, this,
                [this](const QModelIndex&, const QModelIndex&) { onRowSelected(); });
        connect(selection, &QItemSelectionModel::selectionChanged, this,
                [this](const QItemSelection&, const QItemSelection&) { onRowSelected(); });
    }

    // Tab belongs to the cursor wherever the reader is looking. The list would otherwise spend
    // it on its own focus navigation, and a reader who had just picked a signal by name would
    // find the key did nothing.
    rows->installEventFilter(this);
}

bool WaveformView::eventFilter(QObject* watched, QEvent* event) {
    if (watched != rows || event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }

    const auto* key = dynamic_cast<QKeyEvent*>(event);
    if (key == nullptr || (key->key() != Qt::Key_Tab && key->key() != Qt::Key_Backtab)) {
        return QWidget::eventFilter(watched, event);
    }

    stepActiveCursorToEdge(key->key() == Qt::Key_Tab);
    return true;
}

void WaveformView::onRowSelected() {
    if (applyingSelection) return;

    const int rowId = selectedRow();
    if (rowId < 0) return;

    // Exactly what a click on that row's trace would do, with the cursor's own time standing
    // in for where the click landed.
    if (!cursorList.isEmpty()) {
        const int index = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
        if (!cursorList[index].locked) {
            const runtime::SimTime snapped = snapToEdge(rowId, cursorList[index].time);
            if (snapped != cursorList[index].time) {
                cursorList[index].time = snapped;
                Q_EMIT cursorsChanged();
            }
        }
    }

    update();
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

    const int current = waveModel->rowIdOf(rows->currentIndex());
    if (current >= 0) return current;

    // A row can be selected without becoming current — a Ctrl+click, or a selection restored
    // after the model was reset. Falling back keeps "the selected signal" meaning what the
    // reader sees highlighted rather than an index that happens to be stale.
    if (rows->selectionModel() == nullptr) return -1;

    const QModelIndexList selected = rows->selectionModel()->selectedRows();
    return selected.isEmpty() ? -1 : waveModel->rowIdOf(selected.front());
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
    if (waveModel->row(rowId) == nullptr) return time;

    const runtime::SimTime edge = waveModel->nearestEdge(rowId, time);
    if (edge == runtime::NoTime) return time;

    // The radius is a pixel distance, so it is compared as one. A window of time units would
    // snap from further and further away as the view zoomed out.
    if (std::abs(timeAxis.xOf(edge) - timeAxis.xOf(time)) > SnapPixels) return time;

    return edge;
}

void WaveformView::stepActiveCursorToEdge(bool forward) {
    const int rowId = selectedRow();
    if (waveModel->row(rowId) == nullptr) return;
    if (cursorList.isEmpty()) return;

    const int index = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
    if (cursorList[index].locked) return;

    const runtime::SimTime from = cursorList[index].time;
    const runtime::SimTime edge = forward ? waveModel->nextEdgeAfter(rowId, from)
                                          : waveModel->previousEdgeBefore(rowId, from);
    if (edge == runtime::NoTime) return;

    cursorList[index].time = edge;

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
        if (index.isValid()) {
            // Guarded, because setting the current row sends the selection back here and the
            // cursor would be snapped twice — once to where the click landed and once to
            // wherever the row's nearest edge happened to be.
            applyingSelection = true;
            rows->setCurrentIndex(index);
            applyingSelection = false;
        }
    }

    if (cursorList.isEmpty()) return;

    const int index = std::clamp(active, 0, static_cast<int>(cursorList.size()) - 1);
    if (cursorList[index].locked) return;

    cursorList[index].time = snapToEdge(rowId, timeAxis.timeAt(point.x()));
    update();
    Q_EMIT cursorsChanged();
}

void WaveformView::showCursorMenu(int index, const QPoint& where) {
    if (index < 0 || index >= cursorList.size()) return;

    // Shown rather than run: `exec` would sit in its own event loop inside a mouse handler,
    // which stops the socket being serviced and would freeze a live waveform for as long as
    // the menu was open.
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    connect(menu->addAction(tr("Rename…")), &QAction::triggered, this,
            [this, index] { renameCursor(index); });

    // One entry that says what it will do, rather than a checkbox whose current state the
    // reader would have to work out from the colour of the line.
    const bool locked = cursorList[index].locked;
    connect(menu->addAction(locked ? tr("Unlock") : tr("Lock")), &QAction::triggered, this,
            [this, index, locked] { setCursorLocked(index, !locked); });

    menu->addSeparator();

    QAction* remove = menu->addAction(tr("Remove"));
    remove->setEnabled(cursorList.size() > 1);
    connect(remove, &QAction::triggered, this, [this, index] { removeCursor(index); });

    menu->popup(where);
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

    // A summary is only reusable for the range, the width and the amount of recorded history
    // it was made against. Checking all four here, rather than asking every place that could
    // change one of them to remember to say so, is what keeps the cache from ever being
    // subtly stale: zooming, scrolling sideways, resizing and a kernel that has run all land
    // in exactly one of these four.
    const int columns = traceArea().width();
    const runtime::SimTime extent = kernelSession->latestTime();
    if (timeAxis.start() != cachedStart || timeAxis.end() != cachedEnd || columns != cachedColumns
        || extent != cachedExtent) {
        invalidateSamples();
        cachedStart = timeAxis.start();
        cachedEnd = timeAxis.end();
        cachedColumns = columns;
        cachedExtent = extent;
    }

    ++paintCounter;

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

    // Worked out once. The tree is asked where each of its rows is, and asking it twice over
    // for the same answer is work for nothing.
    const std::vector<RowGeometry> visible = visibleRows();

    // Backgrounds first and unclipped: a highlight marks the whole row, including the part of
    // it the design has not been simulated to yet.
    const QRect area = traceArea();
    painter.save();
    painter.setClipRect(area);
    for (const RowGeometry& geometry : visible) paintRowBackground(painter, geometry, area);
    painter.restore();

    // A parameter is not clipped at the recorded extent the way a trace is. A trace stopping
    // there says "nothing has happened here yet", which is true of a signal and false of a
    // parameter: its value is settled by elaboration and holds at every time, including before
    // the kernel has run at all — when the extent is zero and the clip would hide it entirely.
    painter.save();
    painter.setClipRect(area);
    for (const RowGeometry& geometry : visible) {
        if (isParameterRow(geometry.rowId)) paintRow(painter, geometry);
    }
    painter.restore();

    painter.save();
    painter.setClipRect(drawable);
    for (const RowGeometry& geometry : visible) {
        if (!isParameterRow(geometry.rowId)) paintRow(painter, geometry);
    }
    painter.restore();

    paintZoomBand(painter);
    paintCursors(painter);

    // Rows scrolled off the window keep their summary for a repaint or two and are then let
    // go, so a window showing forty rows of a list of two thousand holds forty summaries.
    for (auto entry = sampleCache.begin(); entry != sampleCache.end();) {
        entry = entry.value().lastPaint < paintCounter - SampleCacheGrace ? sampleCache.erase(entry)
                                                                          : std::next(entry);
    }
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
        painter.fillRect(QRect(column, ruler.bottom() - 4, 1, 5),
                         theme->color(ThemeColor::WaveRulerForeground));
        painter.drawText(column + 3, metrics.ascent() + 2,
                         TimeAxis::format(time, kernelSession->timeScale()));

        // Guards against a step small enough that the loop would not terminate on overflow.
        if (time > runtime::NoTime - step) break;
    }

    // Filled rather than stroked. A one-pixel vertical line is the same pixels either way, and
    // the fill path writes them directly where the stroker sets up a pen, a dash pattern and a
    // clip per call — which on a window three thousand pixels wide, with a grid line every
    // ninety, was a third of what the ruler cost and it was paid on every repaint.
    const QColor grid = theme->color(ThemeColor::WaveGrid);
    const QRect area = traceArea();
    for (runtime::SimTime time = first; time <= timeAxis.end(); time += step) {
        const double x = timeAxis.xOf(time);
        if (x < 0.0 || x > width()) continue;
        painter.fillRect(QRect(static_cast<int>(x), area.top(), 1, area.height()), grid);
        if (time > runtime::NoTime - step) break;
    }
}

bool WaveformView::sampleInto(int rowId, int columns, std::vector<wavedb::Sample>& out) const {
    const WaveRow* waveRow = waveModel->row(rowId);
    if (waveRow == nullptr || columns <= 0) return false;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();

    const bool merged =
        !waveRow->children.isEmpty() && !waveModel->childrenReconstituteParent(rowId);

    if (!merged) {
        // A parameter is one value from the beginning of time to the end of it, so every
        // column shows the same change and the bus painter draws the lot as one segment with
        // the value written on it. There is no trace to summarise and nothing to look up.
        if (waveRow->kind == WaveRow::Kind::Parameter) {
            if (!waveRow->constantValue.has_value()) return false;

            out.assign(static_cast<std::size_t>(columns),
                       wavedb::Sample{.startTime = timeAxis.start(),
                                      .transitions = 0,
                                      .changeIndex = 0,
                                      .anyUnknown = waveRow->constantValue->ref().hasUnknown()});
            return true;
        }

        if (waveRow->kind != WaveRow::Kind::Signal) return false;

        const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
        if (record == wavedb::WaveformDatabase::NotRecorded) return false;

        out.assign(static_cast<std::size_t>(columns), wavedb::Sample{});
        database.sample(record, timeAxis.start(), timeAxis.end(), out);
        return true;
    }

    if (waveRow->children.isEmpty()) return false;

    // Allocated per level rather than shared, because this recurses for a group of groups.
    // Section 13.4's rule about allocation is about the simulation's hot path; a repaint is
    // not one, and a buffer shared between levels would be overwritten by the level below.
    std::vector<wavedb::Sample> child;
    std::vector<std::uint8_t> boundary(static_cast<std::size_t>(columns), 0);
    std::vector<std::uint8_t> missing(static_cast<std::size_t>(columns), 0);

    out.assign(static_cast<std::size_t>(columns), wavedb::Sample{});
    bool started = false;

    for (const int childRow : waveRow->children) {
        if (!sampleInto(childRow, columns, child)) return false;

        for (std::size_t column = 0; column < out.size(); ++column) {
            if (!started) out[column].startTime = child[column].startTime;

            out[column].transitions += child[column].transitions;
            out[column].anyUnknown = out[column].anyUnknown || child[column].anyUnknown;

            if (child[column].changeIndex == wavedb::NoChange) missing[column] = 1;
            if (column > 0 && child[column].changeIndex != child[column - 1].changeIndex) {
                boundary[column] = 1;
            }
        }
        started = true;
    }

    // The change index of a merged row is a run number rather than an index into anything: it
    // only has to differ between columns that show different values, which is what the painter
    // uses it for.
    std::uint64_t run = 0;
    for (std::size_t column = 0; column < out.size(); ++column) {
        if (column > 0 && boundary[column] != 0) ++run;
        out[column].changeIndex = missing[column] != 0 ? wavedb::NoChange : run;
    }

    return true;
}

bool WaveformView::isParameterRow(int rowId) const {
    const WaveRow* waveRow = waveModel->row(rowId);
    return waveRow != nullptr && waveRow->kind == WaveRow::Kind::Parameter;
}

void WaveformView::invalidateSamples() {
    sampleCache.clear();
}

std::span<const wavedb::Sample> WaveformView::sampleRow(int rowId, const QRect& area) {
    const int columns = area.width();
    if (columns <= 0) return {};
    if (timeAxis.span() == 0) return {};

    CachedSamples& entry = sampleCache[rowId];
    entry.lastPaint = paintCounter;
    if (!entry.samples.empty()) return entry.samples;

    ++summaryCount;
    if (!sampleInto(rowId, columns, entry.samples)) {
        entry.samples.clear();
        return {};
    }
    return entry.samples;
}

void WaveformView::paintRow(QPainter& painter, const RowGeometry& geometry) {
    const WaveRow* waveRow = waveModel->row(geometry.rowId);
    if (waveRow == nullptr) return;

    const QRect area = traceArea();
    const QRect rowRect(area.left(), geometry.top, area.width(), geometry.height);
    if (rowRect.bottom() < area.top() || rowRect.top() > area.bottom()) return;

    // Filled rather than stroked, for the reason the grid is: one of these per row, the full
    // width of the window, on every repaint.
    painter.fillRect(QRect(rowRect.left(), rowRect.bottom(), rowRect.width(), 1),
                     theme->color(ThemeColor::WaveRowSeparator));

    // A group draws too, when its children make a value between them: section 10.7 has a row
    // with children show what they show joined, and a vector assembled out of one-bit signals
    // is worth as much on screen as one the design declared that way.
    const std::uint32_t displayedWidth = waveModel->displayedWidth(geometry.rowId);
    if (displayedWidth == 0) return;

    const QRect traceRect = rowRect.adjusted(0, TraceMargin, 0, -TraceMargin);
    if (traceRect.height() <= 2) return;

    const std::span<const wavedb::Sample> samples = sampleRow(geometry.rowId, traceRect);
    if (samples.empty()) return;

    if (displayedWidth == 1) {
        paintOneBitTrace(painter, traceRect, geometry.rowId, samples);
    } else {
        paintBusTrace(painter, traceRect, geometry.rowId, samples);
    }
}

void WaveformView::paintOneBitTrace(QPainter& painter, const QRect& area, int rowId,
                                    std::span<const wavedb::Sample> samples) {
    const int top = area.top();
    const int bottom = area.bottom();
    const int middle = (top + bottom) / 2;

    painter.setPen(theme->color(ThemeColor::WaveTrace));

    // Columns are walked in runs rather than one at a time, for the same reason the bus
    // painter does it: within a run of columns showing the same change, the value is the same,
    // and asking for it again per column is a binary search through the trace and a value
    // allocation for an answer already in hand. On a flat one-bit trace that was one lookup
    // per pixel of window width, per row, per repaint — which is what made scrolling cost
    // what it did.
    int previousY = -1;
    std::size_t column = 0;
    while (column < samples.size()) {
        const wavedb::Sample& sample = samples[column];
        const int x = area.left() + static_cast<int>(column);

        if (sample.transitions > 0) {
            // Busy: more than one transition inside a single pixel column. Drawn as a solid
            // bar rather than by iterating the transitions, which is the whole point — a
            // column covering a million edges costs exactly what this one does. The run stops
            // where the unknown flag changes, so that a busy stretch which is unknown in part
            // is still drawn in two colours.
            std::size_t run = column;
            while (run < samples.size() && samples[run].transitions > 0
                   && samples[run].anyUnknown == sample.anyUnknown) {
                ++run;
            }

            painter.fillRect(QRect(x, top, static_cast<int>(run - column), bottom - top + 1),
                             sample.anyUnknown ? theme->color(ThemeColor::WaveUnknown)
                                               : theme->color(ThemeColor::WaveTrace));
            previousY = -1;
            column = run;
            continue;
        }

        if (sample.changeIndex == wavedb::NoChange) {
            previousY = -1;
            ++column;
            continue;
        }

        std::size_t run = column;
        while (run < samples.size() && samples[run].transitions == 0
               && samples[run].changeIndex == sample.changeIndex) {
            ++run;
        }

        // Asked of the model by time rather than of the database by change index, because a
        // row may be showing a slice of a signal, or a value joined out of its children, and
        // only the model knows which.
        const std::optional<eir::BitVector> value = waveModel->valueAt(rowId, sample.startTime);
        if (!value.has_value()) {
            previousY = -1;
            column = run;
            continue;
        }

        const eir::Logic bit = value.value().get(0);

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
        painter.drawLine(x, y, x + static_cast<int>(run - column) - 1, y);
        previousY = y;
        column = run;
    }
}

void WaveformView::paintBusTrace(QPainter& painter, const QRect& area, int rowId,
                                 std::span<const wavedb::Sample> samples) {
    const WaveRow* waveRow = waveModel->row(rowId);
    if (waveRow == nullptr) return;

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
        const std::optional<eir::BitVector> value = waveModel->valueAt(rowId, sample.startTime);
        if (!value.has_value()) {
            column = run;
            continue;
        }

        const QColor pen = value.value().ref().hasUnknown() ? theme->color(ThemeColor::WaveUnknown)
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
            // The model renders it, not this code: a row whose children are vectors reads as
            // the list of them, and a trace that printed the joined number while the value
            // column beside it printed the list would be two answers to one question.
            const QString text = waveModel->renderValue(rowId, sample.startTime);
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
        // A right-click on a cursor's label is about that cursor, not about the time axis, so
        // it opens the cursor's menu instead of starting a zoom gesture.
        const int label = cursorLabelAt(event->pos());
        if (label >= 0) {
            setActiveCursor(label);
            showCursorMenu(label, event->globalPosition().toPoint());
            return;
        }

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
