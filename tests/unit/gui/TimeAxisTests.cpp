// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the mapping between simulated time and pixels.
//
// This is where the waveform view's arithmetic can be wrong without anyone noticing by eye:
// squeezing a 64-bit time range into a few hundred pixels, zooming about a point without the
// design sliding under the pointer, and panning to either end without wrapping.

#include "gui/TimeAxis.h"

#include "runtime/Time.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using enzan::gui::TimeAxis;
using enzan::runtime::NoTime;
using enzan::runtime::SimTime;

TimeAxis axisOf(int width, SimTime start, SimTime span) {
    TimeAxis axis;
    axis.setWidth(width);
    axis.setRange(start, span);
    return axis;
}

TEST(TimeAxisTests, TimeAndPixelsRoundTrip) {
    const TimeAxis axis = axisOf(1000, 0, 1000);

    EXPECT_DOUBLE_EQ(axis.xOf(0), 0.0);
    EXPECT_DOUBLE_EQ(axis.xOf(500), 500.0);
    EXPECT_EQ(axis.timeAt(250.0), 250U);
    EXPECT_EQ(axis.timeAt(0.0), 0U);
}

TEST(TimeAxisTests, APositionOutsideTheViewIsNotClamped) {
    // Clamping would bend a trace that runs off the side instead of letting the caller clip it
    // straight, which shows up as a kink at the edge of the window.
    const TimeAxis axis = axisOf(100, 1000, 100);

    EXPECT_LT(axis.xOf(500), 0.0);
    EXPECT_GT(axis.xOf(2000), 100.0);
}

TEST(TimeAxisTests, ATimeOutsideTheViewIsClamped) {
    // The other direction is clamped, because a coordinate comes from a mouse and a mouse can
    // be outside the widget.
    const TimeAxis axis = axisOf(100, 1000, 100);

    EXPECT_EQ(axis.timeAt(-50.0), 1000U);
    EXPECT_EQ(axis.timeAt(1e9), 1100U);
}

TEST(TimeAxisTests, ZoomingKeepsTheTimeUnderTheAnchorInPlace) {
    // What makes a wheel zoom feel like it moves the design rather than the window.
    TimeAxis axis = axisOf(800, 1000, 800);

    const double anchorX = 200.0;
    const SimTime before = axis.timeAt(anchorX);

    axis.zoom(0.5, anchorX);
    const SimTime after = axis.timeAt(anchorX);

    EXPECT_EQ(axis.span(), 400U);
    EXPECT_LE(before > after ? before - after : after - before, 2U)
        << "the instant under the pointer moved when zooming";
}

TEST(TimeAxisTests, ZoomingInStopsAtOneUnit) {
    TimeAxis axis = axisOf(800, 0, 1000);

    for (int round = 0; round < 100; ++round) axis.zoom(0.5, 400.0);

    EXPECT_EQ(axis.span(), TimeAxis::MinimumSpan)
        << "zooming past a unit per pixel would be zooming into the gaps between instants";
}

TEST(TimeAxisTests, ZoomingOutDoesNotWrapPastTheEndOfTime) {
    TimeAxis axis = axisOf(800, 0, 1000);

    for (int round = 0; round < 200; ++round) axis.zoom(4.0, 400.0);

    EXPECT_GT(axis.span(), 0U);
    EXPECT_GE(axis.end(), axis.start()) << "the right-hand edge ended up before the left one";
}

TEST(TimeAxisTests, PanningStopsAtZero) {
    TimeAxis axis = axisOf(800, 100, 800);

    axis.pan(-10'000.0);
    EXPECT_EQ(axis.start(), 0U) << "panning before time zero has nowhere to go";
}

TEST(TimeAxisTests, PanningDoesNotWrapAtTheEndOfTime) {
    TimeAxis axis = axisOf(800, NoTime - 2000, 800);

    axis.pan(1e12);
    EXPECT_GE(axis.end(), axis.start());
    EXPECT_LE(axis.start(), NoTime - axis.span());
}

TEST(TimeAxisTests, PanningIsReversible) {
    TimeAxis axis = axisOf(800, 10'000, 800);

    axis.pan(120.0);
    axis.pan(-120.0);
    EXPECT_EQ(axis.start(), 10'000U) << "a pan and its opposite must not drift";
}

TEST(TimeAxisTests, FitShowsEverythingWithRoomAtTheEnd) {
    TimeAxis axis = axisOf(800, 5000, 100);
    axis.setExtent(10'000);
    axis.fit();

    EXPECT_EQ(axis.start(), 0U);
    EXPECT_GT(axis.end(), 10'000U) << "the last transition would sit under the right-hand edge";
}

TEST(TimeAxisTests, FitOnADesignThatHasNotRunStillGivesAUsableAxis) {
    // A span of zero would divide by zero in every conversion, so an unstarted view gets a
    // nominal window rather than a degenerate one.
    TimeAxis axis;
    axis.setWidth(800);
    axis.setExtent(0);
    axis.fit();

    EXPECT_GT(axis.span(), 0U);
}

TEST(TimeAxisTests, FollowingTheTailKeepsTheZoomAndTheRightEdge) {
    TimeAxis axis = axisOf(800, 0, 1000);
    axis.setExtent(50'000);
    axis.followTail();

    EXPECT_EQ(axis.span(), 1000U) << "following must not change the zoom";
    EXPECT_EQ(axis.end(), 50'000U);
}

TEST(TimeAxisTests, FollowingATailShorterThanTheWindowShowsFromZero) {
    TimeAxis axis = axisOf(800, 0, 1000);
    axis.setExtent(300);
    axis.followTail();

    EXPECT_EQ(axis.start(), 0U) << "scrolling before time zero to follow a short run is wrong";
}

TEST(TimeAxisTests, ShowRangeAcceptsItsBoundsEitherWayRound) {
    TimeAxis axis = axisOf(800, 0, 1000);

    axis.showRange(900, 400);
    EXPECT_EQ(axis.start(), 400U);
    EXPECT_EQ(axis.span(), 500U);
}

TEST(TimeAxisTests, GridStepsAreNumbersAReaderCanDivideBy) {
    // A ruler stepping by 3 or 7 is technically correct and useless.
    TimeAxis axis = axisOf(1000, 0, 1000);

    for (const SimTime span : {SimTime{100}, SimTime{1000}, SimTime{7777}, SimTime{123'456}}) {
        axis.setRange(0, span);
        const SimTime step = axis.gridStep(90.0);
        ASSERT_GT(step, 0U);

        SimTime mantissa = step;
        while (mantissa % 10 == 0 && mantissa > 10) mantissa /= 10;
        EXPECT_TRUE(mantissa == 1 || mantissa == 2 || mantissa == 5 || mantissa == 10)
            << "step " << step << " for a span of " << span;
    }
}

TEST(TimeAxisTests, AGridStepIsWideEnoughToLabel) {
    const TimeAxis axis = axisOf(1000, 0, 100'000);

    const SimTime step = axis.gridStep(90.0);
    const double pixels = axis.xOf(step) - axis.xOf(0);
    EXPECT_GE(pixels, 90.0) << "the labels would collide and the ruler would be unreadable";
}

TEST(TimeAxisTests, AWidthOfZeroDoesNotDivideByZero) {
    TimeAxis axis;
    axis.setWidth(0);
    axis.setRange(0, 1000);

    EXPECT_TRUE(std::isfinite(axis.unitsPerPixel()));
    EXPECT_TRUE(std::isfinite(axis.xOf(500)));
}

} // namespace
