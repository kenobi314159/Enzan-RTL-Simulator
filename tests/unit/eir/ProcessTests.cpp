// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Process.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using enzan::eir::normaliseWriteSet;
using enzan::eir::WriteExtent;
using enzan::eir::writesObject;

WriteExtent bits(std::uint32_t objectIndex, std::uint32_t lowBit, std::uint32_t width) {
    return {.objectIndex = objectIndex, .lowBit = lowBit, .width = width};
}

TEST(WriteSet, NormalisingOrdersByObjectAndThenByLowBit) {
    std::vector<WriteExtent> extents = {bits(2, 0, 1), bits(0, 6, 1), bits(0, 1, 1)};
    normaliseWriteSet(extents);

    EXPECT_EQ(extents, (std::vector<WriteExtent>{bits(0, 1, 1), bits(0, 6, 1), bits(2, 0, 1)}));
}

TEST(WriteSet, OverlappingAndTouchingExtentsOfOneObjectMerge) {
    std::vector<WriteExtent> extents = {bits(0, 0, 4), bits(0, 2, 4), bits(0, 6, 2),
                                        bits(0, 10, 1)};
    normaliseWriteSet(extents);

    EXPECT_EQ(extents, (std::vector<WriteExtent>{bits(0, 0, 8), bits(0, 10, 1)}));
}

TEST(WriteSet, AnExtentInsideAnotherIsAbsorbed) {
    std::vector<WriteExtent> extents = {bits(0, 0, 8), bits(0, 3, 2), bits(0, 0, 8)};
    normaliseWriteSet(extents);

    EXPECT_EQ(extents, (std::vector<WriteExtent>{bits(0, 0, 8)}));
}

TEST(WriteSet, ExtentsOfDifferentObjectsNeverMerge) {
    // Bit 7 of one object and bit 0 of the next are not neighbours.
    std::vector<WriteExtent> extents = {bits(0, 7, 1), bits(1, 0, 1)};
    normaliseWriteSet(extents);

    EXPECT_EQ(extents, (std::vector<WriteExtent>{bits(0, 7, 1), bits(1, 0, 1)}));
}

TEST(WriteSet, WritesObjectFindsAnyExtentOfIt) {
    const std::vector<WriteExtent> extents = {bits(0, 0, 1), bits(3, 2, 1), bits(3, 6, 1)};

    EXPECT_TRUE(writesObject(extents, 0));
    EXPECT_TRUE(writesObject(extents, 3));
    EXPECT_FALSE(writesObject(extents, 1));
    EXPECT_FALSE(writesObject(extents, 4));
    EXPECT_FALSE(writesObject({}, 0));
}

} // namespace
