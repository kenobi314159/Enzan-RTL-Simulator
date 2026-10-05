// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/ReadMem.h"

#include "eir/BitVector.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::eir::BitVector;
using enzan::runtime::loadMemoryImage;
using enzan::runtime::MemoryShape;

/// A memory of `left`..`right` elements of `width` bits, all X, loaded from `text`, as its
/// elements' values from the left one, and what went wrong.
struct Loaded {
    std::vector<std::string> elements;
    std::vector<std::string> problems;
};

Loaded load(std::string_view text, bool hex, std::int64_t left, std::int64_t right,
            std::uint32_t width, std::optional<std::int64_t> start = std::nullopt,
            std::optional<std::int64_t> end = std::nullopt) {
    const MemoryShape shape{.left = left, .right = right, .elementWidth = width};
    const auto count = static_cast<std::uint32_t>((left > right ? left - right : right - left) + 1);
    BitVector memory(count * width);
    Loaded loaded;
    loaded.problems = loadMemoryImage(text, hex, shape, start, end, memory.mutableRef());
    for (std::uint32_t element = 0; element < count; ++element) {
        std::string bits;
        for (std::uint32_t bit = width; bit-- > 0;)
            bits += enzan::eir::toChar(memory.ref().get(((count - 1 - element) * width) + bit));
        loaded.elements.push_back(bits);
    }
    return loaded;
}

TEST(ReadMem, WordsLoadFromTheLowestAddressWhicheverWayTheMemoryIsDeclared) {
    // Declared [3:0], element 0 is the rightmost, and it gets the first word.
    const Loaded loaded = load("1 2 3 4", true, 3, 0, 4);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"0100", "0011", "0010", "0001"}));
    EXPECT_TRUE(loaded.problems.empty());
}

TEST(ReadMem, CommentsUnderscoresAndUnknownDigitsAreRead) {
    const Loaded loaded = load("// a comment\n1_0 /* and\n another */ xz\n", true, 0, 1, 8);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"00010000", "xxxxzzzz"}));
    EXPECT_TRUE(loaded.problems.empty());
}

TEST(ReadMem, AnAddressMovesTheNextWordAndWhatIsNotReachedKeepsItsValue) {
    const Loaded loaded = load("@2 1 1", false, 0, 3, 1);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"x", "x", "1", "1"}));
    EXPECT_TRUE(loaded.problems.empty());
}

TEST(ReadMem, ARangeLoadsFromItsStartTowardsItsEnd) {
    const Loaded loaded = load("1 2 3", true, 0, 3, 4, 3, 1);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"xxxx", "0011", "0010", "0001"}));
}

TEST(ReadMem, AWordPastTheRangeStopsTheLoad) {
    const Loaded loaded = load("1 1 1", false, 0, 3, 1, 0, 1);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"1", "1", "x", "x"}));
    ASSERT_EQ(loaded.problems.size(), 1U);
    EXPECT_NE(loaded.problems.front().find("more words"), std::string::npos);
}

TEST(ReadMem, AnAddressOutsideTheRangeStopsTheLoad) {
    const Loaded loaded = load("1 @5 1", false, 0, 3, 1);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"1", "x", "x", "x"}));
    ASSERT_EQ(loaded.problems.size(), 1U);
    EXPECT_NE(loaded.problems.front().find("address 5"), std::string::npos);
}

TEST(ReadMem, ACharacterThatIsNoDigitIsReported) {
    // A 2 is no binary digit.
    const Loaded loaded = load("1 2", false, 0, 1, 1);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"1", "x"}));
    ASSERT_EQ(loaded.problems.size(), 1U);
    EXPECT_NE(loaded.problems.front().find("line 1"), std::string::npos);
}

TEST(ReadMem, TooFewWordsAreReportedButLoaded) {
    const Loaded loaded = load("1", false, 0, 1, 1);
    EXPECT_EQ(loaded.elements, (std::vector<std::string>{"1", "x"}));
    EXPECT_EQ(loaded.problems.size(), 1U);
}

} // namespace
