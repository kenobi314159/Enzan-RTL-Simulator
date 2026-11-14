// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Names.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

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

TEST(Names, ListsNamesAsADiagnosticDoes) {
    EXPECT_EQ(enzan::quotedNames(std::vector<std::string>{"counter", "counter_tb"}),
              "'counter', 'counter_tb'");
    EXPECT_EQ(enzan::quotedNames(std::vector<std::string_view>{"tb"}), "'tb'");
    EXPECT_EQ(enzan::quotedNames(std::vector<std::string>{}), "(none)");
}

} // namespace
