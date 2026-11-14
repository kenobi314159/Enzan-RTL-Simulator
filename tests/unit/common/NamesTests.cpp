// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Names.h"

#include <gtest/gtest.h>

namespace {

TEST(Names, ComparesWithoutCase) {
    EXPECT_TRUE(enzan::equalsIgnoringCase("COUNTER", "counter"));
    EXPECT_TRUE(enzan::equalsIgnoringCase("Std_Logic_1164", "std_logic_1164"));
    EXPECT_TRUE(enzan::equalsIgnoringCase("", ""));
}

TEST(Names, TellsDifferentNamesApart) {
    EXPECT_FALSE(enzan::equalsIgnoringCase("counter", "counters"));
    EXPECT_FALSE(enzan::equalsIgnoringCase("count_a", "count_b"));
    // '@' and '`' sit just before 'A' and 'a', and '[' and '{' just after 'Z' and 'z'.
    EXPECT_FALSE(enzan::equalsIgnoringCase("@", "`"));
    EXPECT_FALSE(enzan::equalsIgnoringCase("[", "{"));
}

} // namespace
