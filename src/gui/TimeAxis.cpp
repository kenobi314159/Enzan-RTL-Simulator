// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/TimeAxis.h"

#include <algorithm>
#include <cmath>

namespace enzan::gui {

namespace {

/// The largest time the axis will position its start at.
///
/// Leaving room for the span means `start + span` never wraps, and a wrapped end would put the
/// right-hand edge of the view before its left-hand edge — which draws as every trace being
/// simultaneously everywhere.
runtime::SimTime maximumStart(runtime::SimTime span) noexcept {
    return runtime::NoTime - span;
}

} // namespace

void TimeAxis::setWidth(int pixels) noexcept {
    viewportWidth = std::max(1, pixels);
}

void TimeAxis::setRange(runtime::SimTime start, runtime::SimTime span) noexcept {
    rangeSpan = std::max(span, MinimumSpan);
    rangeStart = std::min(start, maximumStart(rangeSpan));
}

double TimeAxis::unitsPerPixel() const noexcept {
    return static_cast<double>(rangeSpan) / static_cast<double>(viewportWidth);
}

double TimeAxis::xOf(runtime::SimTime time) const noexcept {
    // Signed, and deliberately not clamped: a coordinate pinned to the edge would bend a trace
    // that runs off the side rather than letting the caller clip it straight.
    const double offset = static_cast<double>(time) - static_cast<double>(rangeStart);
    return offset / unitsPerPixel();
}

runtime::SimTime TimeAxis::timeAt(double x) const noexcept {
    if (x <= 0.0) return rangeStart;

    const double units = x * unitsPerPixel();
    if (units >= static_cast<double>(rangeSpan)) return end();

    return rangeStart + static_cast<runtime::SimTime>(units);
}

void TimeAxis::zoom(double factor, double anchorX) noexcept {
    if (!(factor > 0.0)) return;

    const runtime::SimTime anchorTime = timeAt(anchorX);

    const double wanted = static_cast<double>(rangeSpan) * factor;
    const auto limit = static_cast<double>(runtime::NoTime);
    const auto newSpan = static_cast<runtime::SimTime>(
        std::clamp(wanted, static_cast<double>(MinimumSpan), limit / 2.0));

    // The anchor keeps the instant under the pointer where it was, which is what makes a wheel
    // zoom feel like it moves the design rather than the window.
    const double anchorFraction =
        viewportWidth > 0 ? std::clamp(anchorX / viewportWidth, 0.0, 1.0) : 0.5;
    const auto offset =
        static_cast<runtime::SimTime>(static_cast<double>(newSpan) * anchorFraction);

    setRange(anchorTime > offset ? anchorTime - offset : 0, newSpan);
}

void TimeAxis::pan(double deltaPixels) noexcept {
    const double shift = deltaPixels * unitsPerPixel();
    if (shift >= 0.0) {
        const auto forward = static_cast<runtime::SimTime>(shift);
        setRange(rangeStart > maximumStart(rangeSpan) - forward ? maximumStart(rangeSpan)
                                                                : rangeStart + forward,
                 rangeSpan);
        return;
    }

    const auto backward = static_cast<runtime::SimTime>(-shift);
    setRange(rangeStart > backward ? rangeStart - backward : 0, rangeSpan);
}

void TimeAxis::fit() noexcept {
    // A design that has not run yet still needs an axis to draw, and a span of zero would
    // divide by zero everywhere below, so an unstarted view shows a nominal window.
    if (latest == 0) {
        setRange(0, 1000);
        return;
    }

    // A tenth past the end, so that the last transition is not painted under the right-hand
    // edge where it cannot be seen.
    const runtime::SimTime margin = std::max<runtime::SimTime>(latest / 10, 1);
    setRange(0, latest + margin);
}

void TimeAxis::showRange(runtime::SimTime from, runtime::SimTime to) noexcept {
    const runtime::SimTime low = std::min(from, to);
    const runtime::SimTime high = std::max(from, to);
    setRange(low, std::max(high - low, MinimumSpan));
}

void TimeAxis::followTail() noexcept {
    if (latest <= rangeSpan) {
        setRange(0, rangeSpan);
        return;
    }
    setRange(latest - rangeSpan, rangeSpan);
}

runtime::SimTime TimeAxis::gridStep(double minimumPixels) const noexcept {
    const double wanted = std::max(minimumPixels, 1.0) * unitsPerPixel();
    if (wanted <= 1.0) return 1;

    // 1, 2, 5, 10, 20, 50 ... because those are the intervals a reader can divide by without
    // thinking about it. A step of 3 or 7 makes a ruler that is technically correct and
    // useless.
    runtime::SimTime step = 1;
    while (static_cast<double>(step) < wanted) {
        const runtime::SimTime doubled = step * 2;
        if (static_cast<double>(doubled) >= wanted) return doubled;

        const runtime::SimTime fived = step * 5;
        if (static_cast<double>(fived) >= wanted) return fived;

        const runtime::SimTime tenfold = step * 10;
        if (tenfold / 10 != step) return step; // would overflow; the range is already absurd
        step = tenfold;
    }
    return step;
}

QString TimeAxis::format(runtime::SimTime time, runtime::TimeScale scale) {
    return QString::fromStdString(scale.format(time));
}

QString TimeAxis::formatTick(runtime::SimTime time, runtime::SimTime step,
                             runtime::TimeScale scale) {
    const runtime::TimeExponent precision = scale.precisionExponent();

    // Only the named units are candidates, which is why the walk starts at seconds and goes
    // down in thousands. A precision with no name of its own, such as 100ps, is still labelled
    // in a named unit whenever a tenth of that unit is a whole number of time units.
    for (int unit = 0; unit >= precision; unit -= 3) {
        const auto unitScale = runtime::TimeScale(static_cast<runtime::TimeExponent>(unit));
        if (unit == precision) return format(time, unitScale);

        runtime::SimTime tenth = 1;
        for (int i = precision + 1; i < unit; ++i) tenth *= 10;
        if (step % tenth != 0) continue;

        const runtime::SimTime tenths = time / tenth;
        QString label = QString::number(tenths / 10);
        if (tenths % 10 != 0) label += QLatin1Char('.') + QString::number(tenths % 10);
        return label + QString::fromStdString(unitScale.unitName());
    }

    return format(time, scale);
}

} // namespace enzan::gui
