// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/BitVector.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace {

using enzan::eir::BitVector;
using enzan::eir::ConstBitRef;
using enzan::eir::Logic;
using enzan::eir::Radix;
using enzan::eir::Word;
using enzan::test::literal;

std::string binary(ConstBitRef value) {
    return enzan::eir::toString(value, Radix::Binary);
}

// ---------------------------------------------------------------------------
// Storage arithmetic
// ---------------------------------------------------------------------------

TEST(BitStorage, WordCounts) {
    EXPECT_EQ(enzan::eir::wordsFor(1), 1U);
    EXPECT_EQ(enzan::eir::wordsFor(64), 1U);
    EXPECT_EQ(enzan::eir::wordsFor(65), 2U);
    EXPECT_EQ(enzan::eir::wordsFor(128), 2U);
    EXPECT_EQ(enzan::eir::wordsFor(129), 3U);

    // Two planes, always.
    EXPECT_EQ(enzan::eir::storageWordsFor(8), 2U);
    EXPECT_EQ(enzan::eir::storageWordsFor(65), 4U);
}

TEST(BitStorage, TopWordMask) {
    EXPECT_EQ(enzan::eir::topWordMask(1), 0x1U);
    EXPECT_EQ(enzan::eir::topWordMask(8), 0xFFU);
    EXPECT_EQ(enzan::eir::topWordMask(63), (Word(1) << 63) - 1);

    // A width that exactly fills the last Word uses all of it; the modulo would
    // otherwise produce a zero mask and erase the value.
    EXPECT_EQ(enzan::eir::topWordMask(64), ~Word(0));
    EXPECT_EQ(enzan::eir::topWordMask(128), ~Word(0));
}

TEST(LogicValues, EncodingMatchesTheTwoPlaneTable) {
    // aval | bval << 1, which is also VPI's s_vpi_vecval encoding.
    EXPECT_EQ(static_cast<int>(Logic::Zero), 0);
    EXPECT_EQ(static_cast<int>(Logic::One), 1);
    EXPECT_EQ(static_cast<int>(Logic::HighZ), 2);
    EXPECT_EQ(static_cast<int>(Logic::Unknown), 3);
}

TEST(LogicValues, CharactersAndUnknownPredicate) {
    EXPECT_EQ(enzan::eir::toChar(Logic::Zero), '0');
    EXPECT_EQ(enzan::eir::toChar(Logic::One), '1');
    EXPECT_EQ(enzan::eir::toChar(Logic::HighZ), 'z');
    EXPECT_EQ(enzan::eir::toChar(Logic::Unknown), 'x');

    EXPECT_FALSE(enzan::eir::isUnknown(Logic::Zero));
    EXPECT_FALSE(enzan::eir::isUnknown(Logic::One));
    EXPECT_TRUE(enzan::eir::isUnknown(Logic::HighZ));
    EXPECT_TRUE(enzan::eir::isUnknown(Logic::Unknown));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(BitVector, DefaultsToUnknown) {
    // Not zero: an uninitialised four-state variable is X at time zero
    // (DESIGN_GUIDE.md section 3.9), and defaulting to zero would hide exactly the
    // bugs X exists to reveal.
    const BitVector value(8);

    EXPECT_EQ(value.width(), 8U);
    EXPECT_TRUE(value.ref().isAll(Logic::Unknown));
    EXPECT_TRUE(value.ref().hasUnknown());
    EXPECT_FALSE(value.ref().isTwoState());
}

TEST(BitVector, FilledSetsEveryBit) {
    for (const Logic fill : {Logic::Zero, Logic::One, Logic::HighZ, Logic::Unknown}) {
        const BitVector value = BitVector::filled(13, fill);
        EXPECT_TRUE(value.ref().isAll(fill)) << static_cast<int>(fill);
        for (std::uint32_t index = 0; index < 13; ++index) EXPECT_EQ(value.get(index), fill);
    }
}

TEST(BitVector, FromUInt64Truncates) {
    EXPECT_EQ(binary(literal("8'hff").ref()), "11111111");
    EXPECT_EQ(BitVector::fromUInt64(0xFFFF, 8).ref().toUInt64(), 0xFFU);
    EXPECT_EQ(BitVector::fromUInt64(5, 4).ref().toUInt64(), 5U);
}

TEST(BitVector, FromUInt64KeepsTheTopBitsCanonical) {
    // A stray bit above the width would make two equal values compare unequal.
    const BitVector value = BitVector::fromUInt64(0xFFFF'FFFF'FFFF'FFFFULL, 3);
    EXPECT_EQ(value.ref().toUInt64(), 7U);
    EXPECT_EQ(binary(value.ref()), "111");
}

TEST(BitVector, FromInt64SignExtends) {
    const BitVector narrow = BitVector::fromInt64(-1, 4);
    EXPECT_EQ(binary(narrow.ref()), "1111");

    const BitVector wide = BitVector::fromInt64(-2, 96);
    EXPECT_EQ(wide.get(95), Logic::One) << "the sign must reach the top limb";
    EXPECT_EQ(wide.get(0), Logic::Zero);
    EXPECT_EQ(wide.get(1), Logic::One);
}

TEST(BitVector, FromInt64OfPositiveValuesDoesNotSetHighBits) {
    const BitVector value = BitVector::fromInt64(3, 96);
    EXPECT_EQ(value.get(95), Logic::Zero);
    EXPECT_EQ(value.ref().toUInt64(), 3U);
}

TEST(BitVector, CopiesPreserveBothPlanes) {
    const BitVector source = literal("4'b10xz");
    const BitVector copy = BitVector::from(source.ref());

    EXPECT_EQ(source, copy);
    EXPECT_TRUE(source.ref().identicalTo(copy.ref()));
}

// ---------------------------------------------------------------------------
// Bit access and predicates
// ---------------------------------------------------------------------------

TEST(BitVector, SetAndGetAcrossALimbBoundary) {
    BitVector value = BitVector::filled(130, Logic::Zero);

    value.set(0, Logic::One);
    value.set(63, Logic::Unknown);
    value.set(64, Logic::HighZ);
    value.set(129, Logic::One);

    EXPECT_EQ(value.get(0), Logic::One);
    EXPECT_EQ(value.get(63), Logic::Unknown);
    EXPECT_EQ(value.get(64), Logic::HighZ);
    EXPECT_EQ(value.get(129), Logic::One);
    EXPECT_EQ(value.get(1), Logic::Zero);
    EXPECT_EQ(value.get(128), Logic::Zero);
}

TEST(BitVector, TwoStateDetection) {
    EXPECT_TRUE(literal("4'b1010").ref().isTwoState());
    EXPECT_FALSE(literal("4'b101x").ref().isTwoState());
    EXPECT_FALSE(literal("4'b101z").ref().isTwoState());

    // Z counts as unknown for the two-state fast path: the fast path exists to
    // skip plane bookkeeping, and Z needs the second plane just as X does.
    EXPECT_TRUE(literal("4'b101z").ref().hasUnknown());
}

TEST(BitVector, IsAllChecksTheWholeWidth) {
    EXPECT_TRUE(BitVector::filled(70, Logic::One).ref().isAll(Logic::One));
    EXPECT_TRUE(BitVector::filled(70, Logic::HighZ).ref().isAll(Logic::HighZ));

    BitVector almost = BitVector::filled(70, Logic::One);
    almost.set(69, Logic::Zero);
    EXPECT_FALSE(almost.ref().isAll(Logic::One)) << "the top limb must be checked too";
}

TEST(BitVector, IsZeroRequiresBothPlanesClear) {
    EXPECT_TRUE(BitVector::filled(8, Logic::Zero).ref().isZero());
    EXPECT_FALSE(BitVector::filled(8, Logic::One).ref().isZero());

    // Z has a zero aval plane but is not zero.
    EXPECT_FALSE(BitVector::filled(8, Logic::HighZ).ref().isZero());
    EXPECT_FALSE(BitVector::filled(8, Logic::Unknown).ref().isZero());
}

TEST(BitVector, ToInt64SignExtendsFromTheDeclaredWidth) {
    EXPECT_EQ(literal("4'b1111").ref().toInt64(), -1);
    EXPECT_EQ(literal("4'b1000").ref().toInt64(), -8);
    EXPECT_EQ(literal("4'b0111").ref().toInt64(), 7);
    EXPECT_EQ(literal("8'hff").ref().toInt64(), -1);
    EXPECT_EQ(BitVector::fromUInt64(0, 64).ref().toInt64(), 0);
    EXPECT_EQ(BitVector::fromInt64(std::numeric_limits<std::int64_t>::min(), 64).ref().toInt64(),
              std::numeric_limits<std::int64_t>::min());
}

TEST(BitVector, IdenticalToDistinguishesXFromZ) {
    // This is what makes `===` different from `==`, so it must not collapse the
    // two unknown values.
    EXPECT_FALSE(literal("1'bx").ref().identicalTo(literal("1'bz").ref()));
    EXPECT_TRUE(literal("1'bx").ref().identicalTo(literal("1'bx").ref()));
}

// ---------------------------------------------------------------------------
// Resizing
// ---------------------------------------------------------------------------

TEST(BitVector, ResizeZeroExtends) {
    EXPECT_EQ(binary(literal("4'b1011").resized(8, false).ref()), "00001011");
}

TEST(BitVector, ResizeSignExtends) {
    EXPECT_EQ(binary(literal("4'b1011").resized(8, true).ref()), "11111011");
    EXPECT_EQ(binary(literal("4'b0011").resized(8, true).ref()), "00000011");
}

TEST(BitVector, ResizeSignExtendingAnUnknownSignSpreadsIt) {
    // A sign that is unknown makes every extended bit unknown. Anything less
    // pessimistic would invent information.
    EXPECT_EQ(binary(literal("4'bx011").resized(8, true).ref()), "xxxxx011");
    EXPECT_EQ(binary(literal("4'bz011").resized(8, true).ref()), "zzzzz011");
}

TEST(BitVector, ResizeTruncates) {
    EXPECT_EQ(binary(literal("8'b1010_1100").resized(4, false).ref()), "1100");
    EXPECT_EQ(binary(literal("8'b1010_1100").resized(4, true).ref()), "1100");
}

TEST(BitVector, ResizeAcrossALimbBoundary) {
    const BitVector narrow = literal("4'b1111");
    const BitVector wide = narrow.resized(70, true);

    EXPECT_EQ(wide.width(), 70U);
    EXPECT_TRUE(wide.ref().isAll(Logic::One));
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(BitVectorParse, SizedBasedLiterals) {
    EXPECT_EQ(binary(literal("4'b1010").ref()), "1010");
    EXPECT_EQ(binary(literal("8'hA5").ref()), "10100101");
    EXPECT_EQ(binary(literal("6'o52").ref()), "101010");
    EXPECT_EQ(literal("16'd1234").ref().toUInt64(), 1234U);
}

TEST(BitVectorParse, UnknownDigitsExpandToTheWholeDigit) {
    EXPECT_EQ(binary(literal("4'b10xz").ref()), "10xz");

    // One hex x covers four bits.
    EXPECT_EQ(binary(literal("8'hx5").ref()), "xxxx0101");
    EXPECT_EQ(binary(literal("8'hzF").ref()), "zzzz1111");
}

TEST(BitVectorParse, UnderscoresAreSeparators) {
    EXPECT_EQ(binary(literal("8'b1010_0101").ref()), "10100101");
    EXPECT_EQ(literal("16'd1_000").ref().toUInt64(), 1000U);
}

TEST(BitVectorParse, SignedMarkerIsAccepted) {
    // Signedness belongs to the type, not to the storage, so it is accepted and
    // dropped here.
    EXPECT_EQ(binary(literal("4'sb1011").ref()), "1011");
    EXPECT_EQ(binary(literal("8'sh7F").ref()), "01111111");
}

TEST(BitVectorParse, OversizedLiteralsTruncateAtTheTop) {
    EXPECT_EQ(binary(literal("4'hFF").ref()), "1111");
    EXPECT_EQ(binary(literal("2'b1011").ref()), "11");
}

TEST(BitVectorParse, PlainDecimalIsThirtyTwoBits) {
    const BitVector value = literal("42");
    EXPECT_EQ(value.width(), 32U);
    EXPECT_EQ(value.ref().toUInt64(), 42U);
}

TEST(BitVectorParse, UnsizedBasedLiteralsAreThirtyTwoBits) {
    const BitVector value = literal("'hFF");
    EXPECT_EQ(value.width(), 32U);
    EXPECT_EQ(value.ref().toUInt64(), 0xFFU);
}

TEST(BitVectorParse, UnsizedFillLiteralsAreOneBit) {
    // A fill literal has no width of its own; the caller extends it in context.
    EXPECT_EQ(literal("'0").width(), 1U);
    EXPECT_EQ(literal("'0").get(0), Logic::Zero);
    EXPECT_EQ(literal("'1").get(0), Logic::One);
    EXPECT_EQ(literal("'x").get(0), Logic::Unknown);
    EXPECT_EQ(literal("'z").get(0), Logic::HighZ);
}

TEST(BitVectorParse, DecimalUnknownLiterals) {
    // Decimal cannot localise an unknown to a digit, so the whole value is X or Z.
    EXPECT_TRUE(literal("8'dx").ref().isAll(Logic::Unknown));
    EXPECT_TRUE(literal("8'dz").ref().isAll(Logic::HighZ));
}

TEST(BitVectorParse, WideLiterals) {
    const BitVector value = literal("96'hFFFF_FFFF_0000_0000_1234_5678");

    EXPECT_EQ(value.width(), 96U);
    EXPECT_EQ(value.ref().toUInt64(), 0x0000'0000'1234'5678ULL);
    EXPECT_EQ(value.get(95), Logic::One);
    EXPECT_EQ(value.get(64), Logic::One);
    EXPECT_EQ(value.get(63), Logic::Zero);
}

TEST(BitVectorParse, WideDecimalLiterals) {
    // 2^64, which does not fit in a single limb.
    const BitVector value = literal("96'd18446744073709551616");

    EXPECT_EQ(value.get(64), Logic::One);
    EXPECT_EQ(value.ref().toUInt64(), 0U);
}

TEST(BitVectorParse, RejectsMalformedInput) {
    // Returning nothing rather than guessing: a misparsed literal is a wrong
    // simulation result, and the caller has a source location to report against.
    EXPECT_FALSE(BitVector::parse("").has_value());
    EXPECT_FALSE(BitVector::parse("'").has_value());
    EXPECT_FALSE(BitVector::parse("4'").has_value());
    EXPECT_FALSE(BitVector::parse("4'q11").has_value());
    EXPECT_FALSE(BitVector::parse("4'b").has_value());
    EXPECT_FALSE(BitVector::parse("4'b12").has_value()) << "2 is not a binary digit";
    EXPECT_FALSE(BitVector::parse("4'o8").has_value()) << "8 is not an octal digit";
    EXPECT_FALSE(BitVector::parse("4'hG").has_value());
    EXPECT_FALSE(BitVector::parse("0'b1").has_value()) << "a zero width is not representable";
    EXPECT_FALSE(BitVector::parse("x'b1").has_value());
    EXPECT_FALSE(BitVector::parse("hello").has_value());
    EXPECT_FALSE(BitVector::parse("4'd1x").has_value()) << "x mixed with decimal digits";
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

TEST(BitVectorToString, BinaryRendersEveryBit) {
    EXPECT_EQ(enzan::eir::toString(literal("8'h0F").ref(), Radix::Binary), "00001111");
    EXPECT_EQ(enzan::eir::toString(literal("3'b10x").ref(), Radix::Binary), "10x");
}

TEST(BitVectorToString, HexadecimalRendersEveryDigit) {
    EXPECT_EQ(enzan::eir::toString(literal("8'h0F").ref(), Radix::Hexadecimal), "0f");
    EXPECT_EQ(enzan::eir::toString(literal("16'hDEAD").ref(), Radix::Hexadecimal), "dead");
}

TEST(BitVectorToString, PartialTopDigit) {
    // A 6-bit value needs two hex digits, the upper covering only two bits.
    EXPECT_EQ(enzan::eir::toString(literal("6'b10_1010").ref(), Radix::Hexadecimal), "2a");
    EXPECT_EQ(enzan::eir::toString(literal("6'b10_1010").ref(), Radix::Octal), "52");
}

TEST(BitVectorToString, UnknownDigitCase) {
    // Lowercase when the digit is entirely unknown, uppercase when only partly:
    // the distinction tells a reader "unknown" from "contaminated".
    EXPECT_EQ(enzan::eir::toString(literal("4'bxxxx").ref(), Radix::Hexadecimal), "x");
    EXPECT_EQ(enzan::eir::toString(literal("4'bzzzz").ref(), Radix::Hexadecimal), "z");
    EXPECT_EQ(enzan::eir::toString(literal("4'b1x01").ref(), Radix::Hexadecimal), "X");
    EXPECT_EQ(enzan::eir::toString(literal("4'b1z01").ref(), Radix::Hexadecimal), "Z");

    // X beats Z when a digit carries both, so that the more alarming value wins.
    EXPECT_EQ(enzan::eir::toString(literal("4'b1xz1").ref(), Radix::Hexadecimal), "X");
}

TEST(BitVectorToString, DecimalUnsigned) {
    EXPECT_EQ(enzan::eir::toString(literal("8'd0").ref(), Radix::Decimal), "0");
    EXPECT_EQ(enzan::eir::toString(literal("8'd42").ref(), Radix::Decimal), "42");
    EXPECT_EQ(enzan::eir::toString(literal("8'hFF").ref(), Radix::Decimal), "255");
}

TEST(BitVectorToString, DecimalSigned) {
    EXPECT_EQ(enzan::eir::toString(literal("8'hFF").ref(), Radix::Decimal, true), "-1");
    EXPECT_EQ(enzan::eir::toString(literal("8'h80").ref(), Radix::Decimal, true), "-128");
    EXPECT_EQ(enzan::eir::toString(literal("8'h7F").ref(), Radix::Decimal, true), "127");
    EXPECT_EQ(enzan::eir::toString(literal("4'b1111").ref(), Radix::Decimal, true), "-1");
}

TEST(BitVectorToString, DecimalUnknownIsNotLocalised) {
    EXPECT_EQ(enzan::eir::toString(literal("8'bxxxxxxxx").ref(), Radix::Decimal), "x");
    EXPECT_EQ(enzan::eir::toString(literal("8'bzzzzzzzz").ref(), Radix::Decimal), "z");
    EXPECT_EQ(enzan::eir::toString(literal("8'b1010101x").ref(), Radix::Decimal), "X");
}

TEST(BitVectorToString, WideDecimal) {
    // 2^64 and 2^64 - 1, which exercise the multi-limb long division.
    EXPECT_EQ(enzan::eir::toString(literal("96'd18446744073709551616").ref(), Radix::Decimal),
              "18446744073709551616");
    EXPECT_EQ(enzan::eir::toString(literal("64'hFFFF_FFFF_FFFF_FFFF").ref(), Radix::Decimal),
              "18446744073709551615");
}

} // namespace
