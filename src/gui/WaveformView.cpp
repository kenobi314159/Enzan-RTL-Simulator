// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/WaveformView.h"

#include <QFontMetrics>
#include <QHeaderView>
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

    painter.save();
    painter.setClipRect(drawable);
    for (const RowGeometry& geometry : visibleRows()) paintRow(painter, geometry);
    painter.restore();

    paintCursors(painter);
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

        if (sample.changeIndex == wavedb::Sample::NoChange) {
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

        if (sample.changeIndex == wavedb::Sample::NoChange) {
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

void WaveformView::paintCursors(QPainter& painter) {
    const QRect area = traceArea();
    const QFontMetrics metrics(font());

    for (int index = 0; index < cursorList.size(); ++index) {
        const WaveCursor& cursor = cursorList[index];
        const double x = timeAxis.xOf(cursor.time);
        if (x < 0.0 || x > width()) continue;

        const QColor colour = cursor.locked ? theme->color(ThemeColor::WaveCursorLocked)
                                            : theme->color(ThemeColor::WaveCursor);

        // The active cursor is the thick one. Which cursor a click moves has to be visible, or
        // the user finds out by moving the wrong one.
        painter.setPen(QPen(colour, index == active ? 2 : 1));
        painter.drawLine(static_cast<int>(x), 0, static_cast<int>(x), area.bottom());

        const QString label = cursor.name;
        const int textWidth = metrics.horizontalAdvance(label) + 8;
        const QRect tag(static_cast<int>(x) + 1, 0, textWidth, rulerHeight());
        painter.fillRect(tag, colour);
        painter.setPen(theme->color(ThemeColor::WaveCursorText));
        painter.drawText(tag, Qt::AlignCenter, label);
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void WaveformView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);

    // Panning on the middle button and on Ctrl+drag, so that a plain drag stays available for
    // scrubbing the cursor, which is what a reader does far more often.
    if (event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton
            && event->modifiers().testFlag(Qt::ControlModifier))) {
        panning = true;
        panFrom = event->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        following = false;
        moveActiveCursorTo(event->position().x());
    }
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    if (panning) {
        const int delta = panFrom.x() - event->pos().x();
        timeAxis.pan(delta);
        panFrom = event->pos();
        syncScrollBar();
        update();
        Q_EMIT rangeChanged();
        return;
    }

    if (event->buttons().testFlag(Qt::LeftButton)) moveActiveCursorTo(event->position().x());
}

void WaveformView::mouseReleaseEvent(QMouseEvent* event) {
    if (panning && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        panning = false;
        unsetCursor();
    }
}

void WaveformView::wheelEvent(QWheelEvent* event) {
    const int notches = event->angleDelta().y();
    if (notches == 0) {
        event->ignore();
        return;
    }

    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        timeAxis.pan(notches > 0 ? -60.0 : 60.0);
    } else {
        timeAxis.zoom(notches > 0 ? 1.0 / ZoomStep : ZoomStep, event->position().x());
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

} // namespace enzan::gui
