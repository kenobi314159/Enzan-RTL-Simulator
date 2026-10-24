// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Module.h"

#include "eir/BitVector.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using enzan::eir::ArrayDimension;
using enzan::eir::BitVector;
using enzan::eir::declaredDimensions;
using enzan::eir::Encoding;
using enzan::eir::Logic;
using enzan::eir::ModuleDefinition;
using enzan::eir::NetKind;
using enzan::eir::Object;
using enzan::eir::resolvesSingleDrivers;
using enzan::eir::Type;
using enzan::eir::undrivenValue;
using enzan::eir::VhdlType;

TEST(NetKind, AnUndrivenBitIsZOrWhatTheNetIsPulledOrTiedTo) {
    for (const NetKind kind : {NetKind::Wire, NetKind::Tri, NetKind::WiredAnd, NetKind::WiredOr,
                               NetKind::TriAnd, NetKind::TriOr, NetKind::UWire}) {
        EXPECT_EQ(undrivenValue(kind), Logic::HighZ) << enzan::eir::toString(kind);
    }
    EXPECT_EQ(undrivenValue(NetKind::Tri0), Logic::Zero);
    EXPECT_EQ(undrivenValue(NetKind::Supply0), Logic::Zero);
    EXPECT_EQ(undrivenValue(NetKind::Tri1), Logic::One);
    EXPECT_EQ(undrivenValue(NetKind::Supply1), Logic::One);
}

TEST(NetKind, OnlyPulledAndSupplyNetsResolveASingleDriver) {
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Tri0));
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Tri1));
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Supply0));
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Supply1));
    EXPECT_FALSE(resolvesSingleDrivers(NetKind::Wire));
    EXPECT_FALSE(resolvesSingleDrivers(NetKind::WiredAnd));
    EXPECT_FALSE(resolvesSingleDrivers(NetKind::UWire));
}

TEST(NetKind, IsNamedByItsKeyword) {
    EXPECT_EQ(enzan::eir::toString(NetKind::WiredAnd), "wand");
    EXPECT_EQ(enzan::eir::toString(NetKind::TriOr), "trior");
    EXPECT_EQ(enzan::eir::toString(NetKind::UWire), "uwire");
}

TEST(Encoding, VhdlLogicShowsAsTheFourStateBitEachCodeStandsFor) {
    // U X 0 1 Z W L H -, a byte each, the left element highest (DESIGN_GUIDE.md section 3.6).
    BitVector codes = BitVector::filled(72, Logic::Zero);
    for (std::uint32_t position = 0; position < 9; ++position) {
        const std::uint32_t element = 8 - position;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            if (((position >> bit) & 1U) != 0) codes.set((element * 8) + bit, Logic::One);
        }
    }
    ASSERT_EQ(enzan::eir::presentedWidth(Encoding::VhdlLogic, 72), 9U);
    BitVector shown(9);
    enzan::eir::present(Encoding::VhdlLogic, codes.ref(), shown.mutableRef());
    EXPECT_EQ(enzan::eir::toString(shown.ref(), enzan::eir::Radix::Binary), "xx01zx01x");
}

TEST(Encoding, AShownValueIsStoredAsTheCodesItStandsFor) {
    // 1 0 z x, which a reader writes and which show again as themselves.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const BitVector shown = *BitVector::parse("4'b10zx");
    BitVector codes = BitVector::filled(32, Logic::Zero);
    ASSERT_TRUE(enzan::eir::storeShown(Encoding::VhdlLogic, shown.ref(), codes.mutableRef()));
    EXPECT_EQ(codes.ref().toUInt64(), 0x03020401U) << "'1', '0', 'Z' and 'X', left highest";
    BitVector again(4);
    enzan::eir::present(Encoding::VhdlLogic, codes.ref(), again.mutableRef());
    EXPECT_EQ(enzan::eir::toString(again.ref(), enzan::eir::Radix::Binary), "10zx");
}

TEST(Encoding, ABitTakesOnlyZeroAndOne) {
    BitVector codes = BitVector::filled(16, Logic::Zero);
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    ASSERT_TRUE(enzan::eir::storeShown(Encoding::VhdlBit, BitVector::parse("2'b10")->ref(),
                                       codes.mutableRef()));
    EXPECT_EQ(codes.ref().toUInt64(), 0x0100U);
    EXPECT_FALSE(enzan::eir::storeShown(Encoding::VhdlBit, BitVector::parse("2'b1x")->ref(),
                                        codes.mutableRef()));
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ(codes.ref().toUInt64(), 0x0100U) << "left as it was";
}

TEST(Encoding, VhdlBitShowsAsTwoStateBitsAndBitsAsThemselves) {
    const BitVector codes = BitVector::fromUInt64(0x0100, 16);
    BitVector shown(2);
    enzan::eir::present(Encoding::VhdlBit, codes.ref(), shown.mutableRef());
    EXPECT_EQ(enzan::eir::toString(shown.ref(), enzan::eir::Radix::Binary), "10");
    EXPECT_EQ(enzan::eir::presentedWidth(Encoding::Bits, 16), 16U);
}

TEST(Encoding, EncodingGivesAnUnknownBitOfABitOrABooleanItsZero) {
    // A value crossing into VHDL arrives whatever it holds (DESIGN_GUIDE.md section 3.5), as
    // assigning an unknown bit to a two-state variable makes it 0.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const BitVector shown = *BitVector::parse("4'b10zx");
    BitVector bits = BitVector::filled(32, Logic::One);
    enzan::eir::encode(Encoding::VhdlBit, shown.ref(), bits.mutableRef());
    EXPECT_EQ(bits.ref().toUInt64(), 0x01000000U);

    BitVector logic = BitVector::filled(32, Logic::Zero);
    enzan::eir::encode(Encoding::VhdlLogic, shown.ref(), logic.mutableRef());
    EXPECT_EQ(logic.ref().toUInt64(), 0x03020401U) << "as storeShown stores them";
}

TEST(Encoding, IsNamedByWhatItHolds) {
    EXPECT_EQ(enzan::eir::toString(Encoding::VhdlLogic), "std_ulogic codes");
    EXPECT_EQ(enzan::eir::toString(Encoding::Bits), "four-state bits");
}

using Dimensions = std::vector<ArrayDimension>;

TEST(DeclaredDimensions, ASystemVerilogArrayIsItsUnpackedDimensionsThenItsPackedOnes) {
    const ModuleDefinition definition;

    // logic [3:0][1:0] grid [0:2]
    const Object grid{.name = "grid",
                      .type = Type::bits(24, false, true),
                      .unpackedDimensions = {ArrayDimension{.left = 0, .right = 2}},
                      .packedDimensions = {ArrayDimension{.left = 3, .right = 0},
                                           ArrayDimension{.left = 1, .right = 0}},
                      .elementWidth = 8};
    EXPECT_EQ(
        declaredDimensions(definition, grid),
        (Dimensions{ArrayDimension{.left = 0, .right = 2}, ArrayDimension{.left = 3, .right = 0},
                    ArrayDimension{.left = 1, .right = 0}}));

    // int words [4], whose element has no dimensions of its own
    const Object words{.name = "words",
                       .type = Type::bits(128, false, false),
                       .unpackedDimensions = {ArrayDimension{.left = 0, .right = 3}},
                       .elementWidth = 32};
    EXPECT_EQ(declaredDimensions(definition, words),
              (Dimensions{ArrayDimension{.left = 0, .right = 3}}));

    const Object flag{.name = "flag", .type = Type::logic()};
    EXPECT_TRUE(declaredDimensions(definition, flag).empty());
}

TEST(DeclaredDimensions, AVhdlArrayIsItsTypesDimensionsDividingTheBitsItShowsAs) {
    ModuleDefinition definition;
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Enumeration,
                                            .name = "STD_ULOGIC",
                                            .width = 8,
                                            .encoding = Encoding::VhdlLogic});
    // std_logic_vector(7 downto 0)
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                            .name = "STD_LOGIC_VECTOR",
                                            .width = 64,
                                            .ranges = {{.left = 7, .right = 0, .ascending = false}},
                                            .element = 0});
    // array (0 to 3) of std_logic_vector(7 downto 0)
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                            .name = "BYTES",
                                            .width = 256,
                                            .ranges = {{.left = 0, .right = 3, .ascending = true}},
                                            .element = 1});
    // array (1 to 2, 3 downto 0) of std_logic
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                            .name = "GRID",
                                            .width = 64,
                                            .ranges = {{.left = 1, .right = 2, .ascending = true},
                                                       {.left = 3, .right = 0, .ascending = false}},
                                            .element = 0});
    // array (1 to 0) of std_logic, which holds nothing
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                            .name = "STD_LOGIC_VECTOR",
                                            .width = 0,
                                            .ranges = {{.left = 1, .right = 0, .ascending = true}},
                                            .element = 0});

    const auto ofType = [](std::uint32_t width, std::uint32_t vhdlType) {
        return Object{.name = "s",
                      .type = Type::bits(width, false, false),
                      .encoding = Encoding::VhdlLogic,
                      .vhdlType = vhdlType};
    };

    EXPECT_EQ(declaredDimensions(definition, ofType(64, 1)),
              (Dimensions{ArrayDimension{.left = 7, .right = 0}}));
    EXPECT_EQ(
        declaredDimensions(definition, ofType(256, 2)),
        (Dimensions{ArrayDimension{.left = 0, .right = 3}, ArrayDimension{.left = 7, .right = 0}}))
        << "an array of arrays is the dimensions of both";
    EXPECT_EQ(
        declaredDimensions(definition, ofType(64, 3)),
        (Dimensions{ArrayDimension{.left = 1, .right = 2}, ArrayDimension{.left = 3, .right = 0}}))
        << "and so is an array of two dimensions";
    EXPECT_TRUE(declaredDimensions(definition, ofType(8, 0)).empty()) << "a scalar is no array";

    Object empty = ofType(8, 4);
    empty.holdsNothing = true;
    EXPECT_TRUE(declaredDimensions(definition, empty).empty());

    // An integer array's elements are as wide as the integer.
    definition.vhdlTypes.push_back(
        VhdlType{.kind = VhdlType::Kind::Integer,
                 .name = "INTEGER",
                 .width = 32,
                 .ranges = {{.left = -2147483648, .right = 2147483647}}});
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                            .name = "INTEGERS",
                                            .width = 96,
                                            .ranges = {{.left = 2, .right = 0, .ascending = false}},
                                            .element = 5});
    const Object integers{.name = "n",
                          .type = Type::bits(96, false, false),
                          .encoding = Encoding::Bits,
                          .vhdlType = 6};
    EXPECT_EQ(declaredDimensions(definition, integers),
              (Dimensions{ArrayDimension{.left = 2, .right = 0}}));
}

TEST(DeclaredDimensions, DimensionsThatDoNotDivideTheBitsAreNone) {
    const ModuleDefinition definition;
    const Object odd{.name = "odd",
                     .type = Type::bits(10, false, true),
                     .unpackedDimensions = {ArrayDimension{.left = 0, .right = 2}},
                     .elementWidth = 3};
    EXPECT_TRUE(declaredDimensions(definition, odd).empty());
}

} // namespace
