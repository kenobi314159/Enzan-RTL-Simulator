// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Type.h"

#include <gtest/gtest.h>

namespace {

using enzan::eir::Type;

TEST(Type, DefaultIsVoid) {
    EXPECT_EQ(Type().kind(), Type::Kind::Void);
    EXPECT_TRUE(Type().isVoid());
    EXPECT_EQ(Type().storageWords(), 0U);
}

TEST(Type, NamedShorthandsMatchTheLanguage) {
    EXPECT_EQ(Type::logic(), Type::bits(1, false, true));
    EXPECT_EQ(Type::bit(), Type::bits(1, false, false));
    EXPECT_EQ(Type::integer(), Type::bits(32, true, true));
    EXPECT_EQ(Type::intType(), Type::bits(32, true, false));

    // `integer` is four-state and `int` is not; conflating them would silently
    // change X handling on every assignment.
    EXPECT_TRUE(Type::integer().isFourState());
    EXPECT_FALSE(Type::intType().isFourState());
    EXPECT_TRUE(Type::integer().isSigned());
}

TEST(Type, TwoStateTypesStillCarryBothPlanes) {
    // Uniform storage keeps one value path rather than two, which is the standing
    // four-state-shape invariant. The second plane of a two-state object stays
    // zero, so the two-state fast path triggers on it for free.
    EXPECT_EQ(Type::bit().storageWords(), Type::logic().storageWords());
    EXPECT_EQ(Type::intType().storageWords(), Type::integer().storageWords());
}

TEST(Type, StorageWordsCountsBothPlanes) {
    EXPECT_EQ(Type::bits(1, false, true).storageWords(), 2U);
    EXPECT_EQ(Type::bits(64, false, true).storageWords(), 2U);
    EXPECT_EQ(Type::bits(65, false, true).storageWords(), 4U);
    EXPECT_EQ(Type::bits(128, false, true).storageWords(), 4U);
}

TEST(Type, RealAndStringStorage) {
    EXPECT_EQ(Type::real().storageWords(), 1U) << "one double";
    EXPECT_EQ(Type::real().bitWidth(), 64U);

    // Strings live in the constant pool in the M1 subset, not the state arena.
    EXPECT_EQ(Type::string().storageWords(), 0U);
}

TEST(Type, RenderedAsTheUserWouldWriteIt) {
    EXPECT_EQ(Type::logic().toString(), "logic");
    EXPECT_EQ(Type::bit().toString(), "bit");
    EXPECT_EQ(Type::bits(8, false, true).toString(), "logic [7:0]");
    EXPECT_EQ(Type::bits(32, true, true).toString(), "logic signed [31:0]");
    EXPECT_EQ(Type::bits(8, false, false).toString(), "bit [7:0]");
    EXPECT_EQ(Type::real().toString(), "real");
    EXPECT_EQ(Type::voidType().toString(), "void");
}

TEST(Type, EqualityComparesEveryProperty) {
    EXPECT_NE(Type::bits(8, false, true), Type::bits(8, true, true)) << "signedness";
    EXPECT_NE(Type::bits(8, false, true), Type::bits(8, false, false)) << "four-state-ness";
    EXPECT_NE(Type::bits(8, false, true), Type::bits(16, false, true)) << "width";
    EXPECT_EQ(Type::bits(8, false, true), Type::bits(8, false, true));
}

} // namespace
