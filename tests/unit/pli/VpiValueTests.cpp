// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conversions between Enzan's vectors and the value formats of `s_vpi_value`. An
// application reads every signal through these, so a mistake here is a wrong value in every
// testbench that uses VPI.

#include "pli/VpiValue.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
#include <string>

namespace {

using enzan::eir::BitVector;
using enzan::pli::parseValue;
using enzan::pli::readValue;
using enzan::pli::ValueStorage;
using enzan::test::literal;

/// Reads `bits` as `format`, failing the test if the conversion is refused.
s_vpi_value read(const BitVector& bits, PLI_INT32 format, ValueStorage& storage,
                 bool isSigned = false) {
    s_vpi_value value{};
    value.format = format;
    std::string error;
    EXPECT_TRUE(readValue(bits.ref(), isSigned, value, storage, error)) << error;
    return value;
}

std::string readString(const BitVector& bits, PLI_INT32 format, bool isSigned = false) {
    ValueStorage storage;
    const s_vpi_value value = read(bits, format, storage, isSigned);
    return value.value.str != nullptr ? std::string(value.value.str) : "<null>";
}

/// Parses `value` to `width` bits and renders the result in binary, or "<refused>".
std::string written(s_vpi_value value, std::uint32_t width) {
    std::string error;
    const std::optional<BitVector> parsed = parseValue(value, width, error);
    if (!parsed.has_value()) return "<refused>";
    return enzan::eir::toString(parsed->ref(), enzan::eir::Radix::Binary);
}

s_vpi_value stringValue(PLI_INT32 format, const char* text) {
    s_vpi_value value{};
    value.format = format;
    value.value.str = const_cast<PLI_BYTE8*>(text);
    return value;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

TEST(VpiValue, StringsHaveADigitPerDigitOfTheWidth) {
    const BitVector value = literal("12'hA5x");
    EXPECT_EQ(readString(value, vpiBinStrVal), "10100101xxxx");
    EXPECT_EQ(readString(value, vpiHexStrVal), "a5x");
    EXPECT_EQ(readString(value, vpiOctStrVal), "51Xx") << "a partly unknown digit is X";
}

TEST(VpiValue, DecimalIsUnknownAsAWholeAndHonoursSign) {
    EXPECT_EQ(readString(literal("8'd200"), vpiDecStrVal), "200");
    EXPECT_EQ(readString(literal("8'd200"), vpiDecStrVal, true), "-56");
    EXPECT_EQ(readString(literal("8'bxxxxxxxx"), vpiDecStrVal), "x");
    EXPECT_EQ(readString(literal("8'b0000000x"), vpiDecStrVal), "X");
}

TEST(VpiValue, IntegersReadUnknownBitsAsZeroAndSignExtend) {
    ValueStorage storage;
    EXPECT_EQ(read(literal("8'hF0"), vpiIntVal, storage).value.integer, 240);
    EXPECT_EQ(read(literal("8'hF0"), vpiIntVal, storage, true).value.integer, -16);
    EXPECT_EQ(read(literal("8'b1111000x"), vpiIntVal, storage).value.integer, 240);
    EXPECT_EQ(read(literal("40'hFF_0000_0001"), vpiIntVal, storage).value.integer, 1)
        << "the low 32 bits of something wider";
}

TEST(VpiValue, AScalarIsTheStandardsEncodingOfOneBit) {
    ValueStorage storage;
    EXPECT_EQ(read(literal("1'b0"), vpiScalarVal, storage).value.scalar, vpi0);
    EXPECT_EQ(read(literal("1'b1"), vpiScalarVal, storage).value.scalar, vpi1);
    EXPECT_EQ(read(literal("1'bz"), vpiScalarVal, storage).value.scalar, vpiZ);
    EXPECT_EQ(read(literal("1'bx"), vpiScalarVal, storage).value.scalar, vpiX);
}

TEST(VpiValue, AVectorIsTheTwoPlanesInThirtyTwoBitSlices) {
    ValueStorage storage;
    const s_vpi_value value = read(literal("40'hzz_1234_5678"), vpiVectorVal, storage);
    EXPECT_EQ(static_cast<std::uint32_t>(value.value.vector[0].aval), 0x1234'5678U);
    EXPECT_EQ(value.value.vector[0].bval, 0);
    EXPECT_EQ(value.value.vector[1].aval, 0) << "Z is 0 in aval";
    EXPECT_EQ(value.value.vector[1].bval, 0xFF) << "and 1 in bval";
}

TEST(VpiValue, AStringReadsEightBitsToACharacterAndSkipsPadding) {
    EXPECT_EQ(readString(literal("32'h00_48_69_21"), vpiStringVal), "Hi!");
}

TEST(VpiValue, AWideValueReadsAsAReal) {
    ValueStorage storage;
    EXPECT_DOUBLE_EQ(read(literal("8'hFF"), vpiRealVal, storage, true).value.real, -1.0);
    EXPECT_DOUBLE_EQ(read(literal("72'h1_0000_0000_0000_0000"), vpiRealVal, storage).value.real,
                     18446744073709551616.0);
}

TEST(VpiValue, ObjectTypeChoosesScalarOrVectorAndSaysWhich) {
    ValueStorage storage;
    EXPECT_EQ(read(literal("1'b1"), vpiObjTypeVal, storage).format, vpiScalarVal);
    EXPECT_EQ(read(literal("4'b1010"), vpiObjTypeVal, storage).format, vpiVectorVal);
}

TEST(VpiValue, TwoStringsCanBeHeldAtOnce) {
    // An application that formats an old and a new value into one message holds two results;
    // with a single buffer the first would be overwritten by the second.
    ValueStorage storage;
    const s_vpi_value first = read(literal("4'h1"), vpiBinStrVal, storage);
    const s_vpi_value second = read(literal("4'h2"), vpiBinStrVal, storage);
    EXPECT_STREQ(first.value.str, "0001");
    EXPECT_STREQ(second.value.str, "0010");
}

TEST(VpiValue, AFormatWithNoMeaningForAVectorIsRefused) {
    s_vpi_value value{};
    value.format = vpiStrengthVal;
    ValueStorage storage;
    std::string error;
    EXPECT_FALSE(readValue(literal("1'b1").ref(), false, value, storage, error));
    EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

TEST(VpiValue, DigitStringsAreZeroExtendedAndTruncatedLikeASizedLiteral) {
    EXPECT_EQ(written(stringValue(vpiBinStrVal, "101"), 6), "000101");
    EXPECT_EQ(written(stringValue(vpiBinStrVal, "1x0z"), 4), "1x0z");
    EXPECT_EQ(written(stringValue(vpiHexStrVal, "1F3"), 8), "11110011");
    EXPECT_EQ(written(stringValue(vpiOctStrVal, "17"), 6), "001111");
}

TEST(VpiValue, DecimalStringsMayBeNegative) {
    EXPECT_EQ(written(stringValue(vpiDecStrVal, "5"), 4), "0101");
    EXPECT_EQ(written(stringValue(vpiDecStrVal, "-1"), 4), "1111");
    EXPECT_EQ(written(stringValue(vpiDecStrVal, "x"), 4), "xxxx");
}

TEST(VpiValue, AnIntegerIsSignExtended) {
    s_vpi_value value{};
    value.format = vpiIntVal;
    value.value.integer = -2;
    EXPECT_EQ(written(value, 40), std::string(39, '1') + "0");

    value.value.integer = 0x1234;
    EXPECT_EQ(written(value, 8), "00110100") << "and truncated to the width";
}

TEST(VpiValue, AScalarSetsTheLowBit) {
    s_vpi_value value{};
    value.format = vpiScalarVal;
    value.value.scalar = vpiH;
    EXPECT_EQ(written(value, 1), "1") << "a weak one is a one";
    value.value.scalar = vpiZ;
    EXPECT_EQ(written(value, 1), "z");
    value.value.scalar = 42;
    EXPECT_EQ(written(value, 1), "<refused>");
}

TEST(VpiValue, AVectorWritesBothPlanes) {
    std::array<s_vpi_vecval, 2> slices{};
    slices[0] = {.aval = 0x0F, .bval = 0x03};
    slices[1] = {.aval = 1, .bval = 0};
    s_vpi_value value{};
    value.format = vpiVectorVal;
    value.value.vector = slices.data();
    EXPECT_EQ(written(value, 33), "1" + std::string(28, '0') + "11xx");
}

TEST(VpiValue, ARealIsRoundedToTheNearestInteger) {
    s_vpi_value value{};
    value.format = vpiRealVal;
    value.value.real = 2.6;
    EXPECT_EQ(written(value, 4), "0011");
    value.value.real = std::numeric_limits<double>::infinity();
    EXPECT_EQ(written(value, 4), "<refused>");
}

TEST(VpiValue, AStringIsRightAligned) {
    EXPECT_EQ(written(stringValue(vpiStringVal, "A"), 16), "0000000001000001");
}

TEST(VpiValue, NonsenseIsRefusedRatherThanGuessed) {
    EXPECT_EQ(written(stringValue(vpiBinStrVal, "12"), 4), "<refused>");
    EXPECT_EQ(written(stringValue(vpiHexStrVal, ""), 4), "<refused>");
    EXPECT_EQ(written(stringValue(vpiBinStrVal, nullptr), 4), "<refused>");

    s_vpi_value value{};
    value.format = vpiTimeVal;
    EXPECT_EQ(written(value, 4), "<refused>");
}

TEST(VpiValue, WhatIsReadCanBeWrittenBack) {
    const BitVector original = literal("37'h1_2x4z_6789");
    for (const PLI_INT32 format : {vpiBinStrVal, vpiVectorVal}) {
        ValueStorage storage;
        const s_vpi_value value = read(original, format, storage);
        std::string error;
        const std::optional<BitVector> back = parseValue(value, original.width(), error);
        if (!back.has_value()) {
            ADD_FAILURE() << "format " << format << ": " << error;
            continue;
        }
        EXPECT_EQ(*back, original) << "format " << format;
    }
}

} // namespace
