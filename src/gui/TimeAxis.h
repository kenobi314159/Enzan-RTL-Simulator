// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "runtime/Time.h"

#include <QString>

#include <cstdint>

namespace enzan::gui {

/// The mapping between simulated time and horizontal pixels in the waveform view.
///
/// Separated from the widget because it is where the arithmetic that can be wrong lives, and
/// because it is the only part of the view that can be tested without a display. Zooming about
/// a point, panning without drifting, and clamping a 64-bit time range to a few hundred pixels
/// are all easy to get subtly wrong and impossible to notice by eye.
///
/// Time is a 64-bit integer and pixels are not, so the conversion is the one place a
/// floating-point value is allowed to appear: it converts *for display*, and nothing here
/// feeds back into the simulation.
class TimeAxis {
public:
    /// The narrowest span the axis will show, in time units.
    ///
    /// One unit per pixel is as far in as anyone can usefully go: past that the view would be
    /// zooming into the gaps between representable instants.
    static constexpr runtime::SimTime MinimumSpan = 1;

    void setWidth(int pixels) noexcept;

    [[nodiscard]] int width() const noexcept { return viewportWidth; }

    /// Sets the visible range. `span` is clamped to at least `MinimumSpan`, and `start` to
    /// whatever keeps the end inside the 64-bit range.
    void setRange(runtime::SimTime start, runtime::SimTime span) noexcept;

    [[nodiscard]] runtime::SimTime start() const noexcept { return rangeStart; }

    [[nodiscard]] runtime::SimTime span() const noexcept { return rangeSpan; }

    [[nodiscard]] runtime::SimTime end() const noexcept { return rangeStart + rangeSpan; }

    /// Records how far the kernel has simulated, which is what `fit` and `followTail` use.
    void setExtent(runtime::SimTime latestTime) noexcept { latest = latestTime; }

    [[nodiscard]] runtime::SimTime extent() const noexcept { return latest; }

    /// The x coordinate of `time`. May be outside the viewport, and deliberately is not
    /// clamped: the caller clips, and a clamped coordinate would bend a trace at the edge.
    [[nodiscard]] double xOf(runtime::SimTime time) const noexcept;

    /// The time at an x coordinate, clamped to the visible range.
    [[nodiscard]] runtime::SimTime timeAt(double x) const noexcept;

    /// Time units per pixel, as a real number. Below one when zoomed in past a unit per pixel.
    [[nodiscard]] double unitsPerPixel() const noexcept;

    /// Zooms by `factor` — greater than one zooms out — keeping the time under `anchorX` fixed.
    ///
    /// Anchoring on the pointer rather than on the middle is what makes a wheel zoom feel like
    /// it is moving the design rather than the window.
    void zoom(double factor, double anchorX) noexcept;

    /// Scrolls by a number of pixels. Positive moves the view later in time.
    ///
    /// Returns the number of pixels actually shifted.
    double pan(double deltaPixels) noexcept;

    /// Shows everything simulated so far, with a little room past the end.
    void fit() noexcept;

    /// Shows exactly `[from, to]`, ordered either way round.
    void showRange(runtime::SimTime from, runtime::SimTime to) noexcept;

    /// Scrolls so that the latest simulated time sits at the right-hand edge, keeping the
    /// current zoom. This is what "follow the run" does while a simulation is in progress.
    void followTail() noexcept;

    /// Returns a step for grid lines that is a round number in the current units and at least
    /// `minimumPixels` wide. Steps go 1, 2, 5, 10, 20, 50 and so on, because those are the
    /// intervals a reader can divide by in their head.
    [[nodiscard]] runtime::SimTime gridStep(double minimumPixels) const noexcept;

    /// Formats `time` in `scale`'s unit, exactly. For a cursor or the status bar, where the
    /// reader wants the instant itself rather than a round number near it.
    [[nodiscard]] static QString format(runtime::SimTime time, runtime::TimeScale scale);

    /// Formats a ruler tick at `time`, a multiple of `step`, in the coarsest unit from seconds
    /// down to the precision in which `step` is at least a tenth of a unit.
    ///
    /// The unit follows the zoom so that a ruler zoomed out to microseconds reads `1.5us` rather
    /// than `1500000ps`. It is chosen from the step rather than from each tick so that every
    /// label on the ruler is in the same unit, and a tenth is the finest it goes so that a label
    /// needs at most one decimal place. The arithmetic is on integers, so a label is exact.
    [[nodiscard]] static QString formatTick(runtime::SimTime time, runtime::SimTime step,
                                            runtime::TimeScale scale);

private:
    int viewportWidth = 1;
    runtime::SimTime rangeStart = 0;
    runtime::SimTime rangeSpan = 1000;
    runtime::SimTime latest = 0;
};

} // namespace enzan::gui
