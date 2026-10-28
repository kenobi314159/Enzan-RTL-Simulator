// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the time a reader writes, as `--run` and the wave window's run length take it.

#include "runtime/Time.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using enzan::runtime::LengthFit;
using enzan::runtime::parseRunLength;
using enzan::runtime::parseTimeLength;
using enzan::runtime::RunLength;
using enzan::runtime::SimTime;
using enzan::runtime::TimeLength;
using enzan::runtime::TimeScale;

/// `text` counted in a scale of `exponent`, or nothing when it does not parse or does not fit.
std::optional<SimTime> countIn(std::string_view text, std::int8_t exponent) {
    const std::optional<TimeLength> length = parseTimeLength(text);
    if (!length) return std::nullopt;
    SimTime count = 0;
    if (TimeScale(exponent).countOf(*length, count) != LengthFit::Exact) return std::nullopt;
    return count;
}

TEST(TimeTests, ABareCountIsTheRunsUnit) {
    EXPECT_EQ(countIn("100", -12), 100U);
    EXPECT_EQ(countIn("100", -15), 100U) << "a count is of whatever unit the run is in";
    EXPECT_EQ(countIn("0", -9), 0U);
}

TEST(TimeTests, ATimeWithAUnitIsCountedInTheRunsUnit) {
    EXPECT_EQ(countIn("10ns", -12), 10'000U);
    EXPECT_EQ(countIn("1.5us", -12), 1'500'000U);
    EXPECT_EQ(countIn("1.5us", -15), 1'500'000'000U);
    EXPECT_EQ(countIn("0.5ns", -12), 500U);
    EXPECT_EQ(countIn("2s", -3), 2'000U);
    EXPECT_EQ(countIn("3000ps", -9), 3U) << "a finer unit that comes to whole units of the run's";
    EXPECT_EQ(countIn("1.000ns", -9), 1U) << "trailing zeroes of a fraction are not finer";
}

TEST(TimeTests, ATimeThatIsNoWholeNumberOfTheRunsUnitIsRefused) {
    // Rounded, the run would stop at another time than the one asked for.
    const std::optional<TimeLength> finer = parseTimeLength("10fs");
    ASSERT_TRUE(finer.has_value());
    SimTime count = 7;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the ASSERT above.
    EXPECT_EQ(TimeScale(-12).countOf(*finer, count), LengthFit::NotWhole);
    EXPECT_EQ(count, 7U) << "the count is left alone";
    EXPECT_FALSE(countIn("1.0005ns", -12).has_value());
}

TEST(TimeTests, ATimeLongerThanARunCountsIsRefused) {
    const std::optional<TimeLength> length = parseTimeLength("18446744073709551615s");
    ASSERT_TRUE(length.has_value());
    SimTime count = 0;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the ASSERT above.
    EXPECT_EQ(TimeScale(-15).countOf(*length, count), LengthFit::TooLong);
}

TEST(TimeTests, WhatIsNoLengthOfTimeDoesNotParse) {
    for (const std::string_view text : {"", "ns", "1.5", "5.", ".5ns", "1.ns", "10xs", "10 ns",
                                        "-1ns", "1e3ns", "99999999999999999999ns", "all"}) {
        EXPECT_FALSE(parseTimeLength(text).has_value()) << "'" << text << "'";
    }
}

TEST(TimeTests, ARunGoesOnUntilTheDesignFinishesOrForALengthOfTime) {
    const std::optional<RunLength> all = parseRunLength("all");
    ASSERT_TRUE(all.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the ASSERT above.
    EXPECT_TRUE(all->untilFinished);

    // Anything else is a length, read as parseTimeLength reads it.
    const std::optional<RunLength> length = parseRunLength("1.5us");
    ASSERT_TRUE(length.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): checked by the ASSERT above.
    EXPECT_FALSE(length->untilFinished);
    SimTime count = 0;
    EXPECT_EQ(TimeScale(-12).countOf(length->length, count), LengthFit::Exact);
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ(count, 1'500'000U);

    // Spelt as it is, as a unit is.
    for (const std::string_view text : {"ALL", "All", " all", "all ", "allns", "1.5", ""}) {
        EXPECT_FALSE(parseRunLength(text).has_value()) << "'" << text << "'";
    }
}

} // namespace
