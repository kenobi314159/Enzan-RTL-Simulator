// SPDX-License-Identifier: GPL-3.0-or-later
//
// These tests are the specification of Enzan's four-state semantics as much as
// BitOps.cpp is. Where a case looks surprising, the comment says which IEEE 1800
// rule produces it, because the surprising cases are exactly the ones a future
// change is likely to break (DESIGN_GUIDE.md section 3).

#include "eir/BitOps.h"

#include "eir/BitVector.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

using enzan::eir::BitVector;
using enzan::eir::ConstBitRef;
using enzan::eir::Logic;
using enzan::eir::Radix;
using enzan::test::literal;
namespace bitops = enzan::eir::bitops;

std::string binary(ConstBitRef value) {
    return enzan::eir::toString(value, Radix::Binary);
}

/// Applies a two-operand function to parsed literals and renders the result in
/// binary, which keeps the tests readable as truth tables.
template<typename Operation>
std::string applyBinary(Operation operation, std::string_view left, std::string_view right) {
    const BitVector a = literal(left);
    const BitVector b = literal(right);
    BitVector result(a.width());
    operation(result.mutableRef(), a.ref(), b.ref());
    return binary(result.ref());
}

template<typename Operation>
std::string applyUnary(Operation operation, std::string_view operand) {
    const BitVector a = literal(operand);
    BitVector result(a.width());
    operation(result.mutableRef(), a.ref());
    return binary(result.ref());
}

/// Applies a predicate and renders its single-bit result.
template<typename Operation>
char applyPredicate(Operation operation, std::string_view left, std::string_view right) {
    const BitVector a = literal(left);
    const BitVector b = literal(right);
    BitVector result(1);
    operation(result.mutableRef(), a.ref(), b.ref());
    return enzan::eir::toChar(result.get(0));
}

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

TEST(BitOpsMovement, CopyPreservesBothPlanes) {
    const BitVector source = literal("4'b10xz");
    BitVector destination(4);
    bitops::copy(destination.mutableRef(), source.ref());

    EXPECT_EQ(binary(destination.ref()), "10xz");
}

TEST(BitOpsMovement, CopyToItselfIsHarmless) {
    BitVector value = literal("4'b10xz");
    bitops::copy(value.mutableRef(), value.ref());

    EXPECT_EQ(binary(value.ref()), "10xz");
}

TEST(BitOpsMovement, ExtendZeroFills) {
    const BitVector source = literal("4'b1011");
    BitVector destination(8);
    bitops::extend(destination.mutableRef(), source.ref(), false);

    EXPECT_EQ(binary(destination.ref()), "00001011");
}

TEST(BitOpsMovement, ExtendSignFills) {
    const BitVector source = literal("4'b1011");
    BitVector destination(8);
    bitops::extend(destination.mutableRef(), source.ref(), true);

    EXPECT_EQ(binary(destination.ref()), "11111011");
}

TEST(BitOpsMovement, ExtendPropagatesAnUnknownSign) {
    const BitVector source = literal("4'bx011");
    BitVector destination(8);
    bitops::extend(destination.mutableRef(), source.ref(), true);

    EXPECT_EQ(binary(destination.ref()), "xxxxx011");
}

TEST(BitOpsMovement, ExtendTruncates) {
    const BitVector source = literal("8'b1010_1100");
    BitVector destination(4);
    bitops::extend(destination.mutableRef(), source.ref(), true);

    EXPECT_EQ(binary(destination.ref()), "1100");
}

TEST(BitOpsMovement, SetUnknownCoversTheWholeWidth) {
    BitVector value = BitVector::filled(70, Logic::Zero);
    bitops::setUnknown(value.mutableRef());

    EXPECT_TRUE(value.ref().isAll(Logic::Unknown));
}

TEST(BitOpsMovement, ExtendMatchesABitAtATimeAcrossWordBoundaries) {
    // Extension works a word at a time, so what can go wrong is at the edges of words: a
    // source or destination ending exactly on one, a step across one, a top word partly
    // used. Every pair of these widths is checked against the definition, a bit at a time,
    // for each value the sign bit can hold, into a destination that starts as all X so that
    // a bit left behind shows.
    constexpr std::array<std::uint32_t, 11> Widths = {1, 2, 7, 63, 64, 65, 100, 127, 128, 129, 200};
    static constexpr std::array<Logic, 4> Values = {Logic::Zero, Logic::One, Logic::HighZ,
                                                    Logic::Unknown};

    std::uint64_t state = 0x9E37'79B9'7F4A'7C15U;
    const auto next = [&state] {
        state = (state * 6364136223846793005U) + 1442695040888963407U;
        return Values[(state >> 33U) % 4];
    };

    for (const std::uint32_t sourceWidth : Widths) {
        for (const Logic sign : Values) {
            BitVector source = BitVector::filled(sourceWidth, Logic::Zero);
            for (std::uint32_t bit = 0; bit + 1 < sourceWidth; ++bit) source.set(bit, next());
            source.set(sourceWidth - 1, sign);

            for (const std::uint32_t destinationWidth : Widths) {
                for (const bool signExtend : {false, true}) {
                    BitVector destination(destinationWidth);
                    bitops::extend(destination.mutableRef(), source.ref(), signExtend);

                    BitVector expected = BitVector::filled(destinationWidth, Logic::Zero);
                    for (std::uint32_t bit = 0; bit < destinationWidth; ++bit) {
                        if (bit < sourceWidth)
                            expected.set(bit, source.get(bit));
                        else if (signExtend)
                            expected.set(bit, sign);
                    }

                    EXPECT_EQ(binary(destination.ref()), binary(expected.ref()))
                        << sourceWidth << " bits to " << destinationWidth
                        << (signExtend ? ", sign-extended" : ", zero-extended");
                    EXPECT_TRUE(std::ranges::equal(destination.ref().avalWords(),
                                                   expected.ref().avalWords())
                                && std::ranges::equal(destination.ref().bvalWords(),
                                                      expected.ref().bvalWords()))
                        << "canonical form, " << sourceWidth << " bits to " << destinationWidth;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Bitwise truth tables
// ---------------------------------------------------------------------------

TEST(BitOpsBitwise, AndTruthTable) {
    // Zero dominates, so `0 & x` is 0 rather than x. Getting this wrong is the
    // classic source of spurious X propagation through a gated clock or an enable.
    EXPECT_EQ(applyBinary(bitops::bitwiseAnd, "4'b01xz", "4'b0000"), "0000");
    EXPECT_EQ(applyBinary(bitops::bitwiseAnd, "4'b01xz", "4'b1111"), "01xx");
    EXPECT_EQ(applyBinary(bitops::bitwiseAnd, "4'b01xz", "4'bxxxx"), "0xxx");
    EXPECT_EQ(applyBinary(bitops::bitwiseAnd, "4'b01xz", "4'bzzzz"), "0xxx");
}

TEST(BitOpsBitwise, OrTruthTable) {
    // One dominates, mirroring AND.
    EXPECT_EQ(applyBinary(bitops::bitwiseOr, "4'b01xz", "4'b1111"), "1111");
    EXPECT_EQ(applyBinary(bitops::bitwiseOr, "4'b01xz", "4'b0000"), "01xx");
    EXPECT_EQ(applyBinary(bitops::bitwiseOr, "4'b01xz", "4'bxxxx"), "x1xx");
}

TEST(BitOpsBitwise, XorHasNoDominatingValue) {
    EXPECT_EQ(applyBinary(bitops::bitwiseXor, "4'b0101", "4'b0011"), "0110");
    EXPECT_EQ(applyBinary(bitops::bitwiseXor, "4'b01xz", "4'b0000"), "01xx");
    EXPECT_EQ(applyBinary(bitops::bitwiseXor, "4'b01xz", "4'b1111"), "10xx");
    EXPECT_EQ(applyBinary(bitops::bitwiseXor, "4'b0101", "4'bxxxx"), "xxxx");
}

TEST(BitOpsBitwise, XnorIsTheComplementOfXor) {
    EXPECT_EQ(applyBinary(bitops::bitwiseXnor, "4'b0101", "4'b0011"), "1001");
    EXPECT_EQ(applyBinary(bitops::bitwiseXnor, "4'b01xz", "4'b0000"), "10xx");
}

TEST(BitOpsBitwise, NotMapsBothUnknownsToX) {
    // Z carries no value to invert, so it becomes X rather than staying Z.
    EXPECT_EQ(applyUnary(bitops::bitwiseNot, "4'b01xz"), "10xx");
}

TEST(BitOpsBitwise, WorksAcrossLimbBoundaries) {
    const BitVector a = BitVector::filled(130, Logic::One);
    BitVector b = BitVector::filled(130, Logic::One);
    b.set(129, Logic::Zero);
    b.set(64, Logic::Zero);

    BitVector result(130);
    bitops::bitwiseAnd(result.mutableRef(), a.ref(), b.ref());

    EXPECT_EQ(result.get(129), Logic::Zero);
    EXPECT_EQ(result.get(64), Logic::Zero);
    EXPECT_EQ(result.get(128), Logic::One);
    EXPECT_EQ(result.get(0), Logic::One);
}

// ---------------------------------------------------------------------------
// Truth and reduction
// ---------------------------------------------------------------------------

TEST(BitOpsTruth, ASetBitDecidesTheCondition) {
    // A vector holding both a 1 and an X is *true*: the set bit decides the
    // condition whatever the unknown bits turn out to be. Treating this as unknown
    // would make `if (flags)` unusable on any partly-initialised register.
    EXPECT_EQ(bitops::truthValue(literal("4'b0000").ref()), Logic::Zero);
    EXPECT_EQ(bitops::truthValue(literal("4'b0001").ref()), Logic::One);
    EXPECT_EQ(bitops::truthValue(literal("4'b000x").ref()), Logic::Unknown);
    EXPECT_EQ(bitops::truthValue(literal("4'b000z").ref()), Logic::Unknown);
    EXPECT_EQ(bitops::truthValue(literal("4'b001x").ref()), Logic::One);
}

TEST(BitOpsReduction, ReduceAnd) {
    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::reduceAnd(d, a); }, "1'b1"), "1");

    const auto reduce = [](std::string_view text) {
        const BitVector value = literal(text);
        BitVector result(1);
        bitops::reduceAnd(result.mutableRef(), value.ref());
        return enzan::eir::toChar(result.get(0));
    };

    EXPECT_EQ(reduce("4'b1111"), '1');
    EXPECT_EQ(reduce("4'b1110"), '0');
    EXPECT_EQ(reduce("4'b111x"), 'x');
    // A zero still decides the result even with an unknown present.
    EXPECT_EQ(reduce("4'b110x"), '0');
}

TEST(BitOpsReduction, ReduceOr) {
    const auto reduce = [](std::string_view text) {
        const BitVector value = literal(text);
        BitVector result(1);
        bitops::reduceOr(result.mutableRef(), value.ref());
        return enzan::eir::toChar(result.get(0));
    };

    EXPECT_EQ(reduce("4'b0000"), '0');
    EXPECT_EQ(reduce("4'b0001"), '1');
    EXPECT_EQ(reduce("4'b000x"), 'x');
    EXPECT_EQ(reduce("4'b001x"), '1');
}

TEST(BitOpsReduction, ReduceXorIsUnknownIfAnyBitIs) {
    const auto reduce = [](std::string_view text) {
        const BitVector value = literal(text);
        BitVector result(1);
        bitops::reduceXor(result.mutableRef(), value.ref());
        return enzan::eir::toChar(result.get(0));
    };

    EXPECT_EQ(reduce("4'b0000"), '0');
    EXPECT_EQ(reduce("4'b0001"), '1');
    EXPECT_EQ(reduce("4'b0011"), '0');
    EXPECT_EQ(reduce("4'b0111"), '1');
    // Parity cannot be known if one input bit is not, so there is no early exit.
    EXPECT_EQ(reduce("4'b011x"), 'x');
}

TEST(BitOpsReduction, ReduceXorAcrossLimbs) {
    BitVector value = BitVector::filled(130, Logic::Zero);
    value.set(0, Logic::One);
    value.set(129, Logic::One);

    BitVector result(1);
    bitops::reduceXor(result.mutableRef(), value.ref());
    EXPECT_EQ(result.get(0), Logic::Zero) << "two set bits cancel";

    value.set(65, Logic::One);
    bitops::reduceXor(result.mutableRef(), value.ref());
    EXPECT_EQ(result.get(0), Logic::One);
}

TEST(BitOpsLogical, AndOrNot) {
    EXPECT_EQ(applyPredicate(bitops::logicalAnd, "4'b0010", "4'b0100"), '1');
    EXPECT_EQ(applyPredicate(bitops::logicalAnd, "4'b0010", "4'b0000"), '0');
    EXPECT_EQ(applyPredicate(bitops::logicalAnd, "4'b000x", "4'b0010"), 'x');
    // Zero still wins over unknown, which is what makes `&&` short-circuit safe.
    EXPECT_EQ(applyPredicate(bitops::logicalAnd, "4'b000x", "4'b0000"), '0');

    EXPECT_EQ(applyPredicate(bitops::logicalOr, "4'b0000", "4'b0000"), '0');
    EXPECT_EQ(applyPredicate(bitops::logicalOr, "4'b000x", "4'b0000"), 'x');
    EXPECT_EQ(applyPredicate(bitops::logicalOr, "4'b000x", "4'b0001"), '1');

    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::logicalNot(d, a); }, "1'b0"), "1");
    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::logicalNot(d, a); }, "1'b1"), "0");
    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::logicalNot(d, a); }, "1'bx"), "x");
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

TEST(BitOpsArithmetic, AddWraps) {
    EXPECT_EQ(applyBinary(bitops::add, "8'd200", "8'd100"), "00101100") << "300 mod 256 = 44";
    EXPECT_EQ(applyBinary(bitops::add, "4'b0001", "4'b0001"), "0010");
    EXPECT_EQ(applyBinary(bitops::add, "4'b1111", "4'b0001"), "0000");
}

TEST(BitOpsArithmetic, AddCarriesAcrossLimbs) {
    const BitVector a = literal("128'hFFFF_FFFF_FFFF_FFFF");
    const BitVector b = literal("128'h1");
    BitVector result(128);
    bitops::add(result.mutableRef(), a.ref(), b.ref());

    EXPECT_EQ(result.get(64), Logic::One) << "the carry must reach the second limb";
    EXPECT_EQ(result.get(63), Logic::Zero);
    EXPECT_EQ(result.get(0), Logic::Zero);
}

TEST(BitOpsArithmetic, AnyUnknownBitPoisonsTheWholeResult) {
    // A carry out of an unknown bit can reach any higher bit, so no result bit can
    // be trusted. This is IEEE 1800's rule and not merely caution.
    EXPECT_EQ(applyBinary(bitops::add, "4'b000x", "4'b0001"), "xxxx");
    EXPECT_EQ(applyBinary(bitops::add, "4'b0001", "4'bz001"), "xxxx");
    EXPECT_EQ(applyBinary(bitops::subtract, "4'b000x", "4'b0001"), "xxxx");
    EXPECT_EQ(applyBinary(bitops::multiply, "4'b000x", "4'b0001"), "xxxx");
}

TEST(BitOpsArithmetic, SubtractBorrows) {
    EXPECT_EQ(applyBinary(bitops::subtract, "8'd100", "8'd1"), "01100011");
    EXPECT_EQ(applyBinary(bitops::subtract, "4'b0000", "4'b0001"), "1111");
    EXPECT_EQ(applyBinary(bitops::subtract, "8'd5", "8'd5"), "00000000");
}

TEST(BitOpsArithmetic, SubtractBorrowsAcrossLimbs) {
    const BitVector a = literal("128'h1_0000_0000_0000_0000");
    const BitVector b = literal("128'h1");
    BitVector result(128);
    bitops::subtract(result.mutableRef(), a.ref(), b.ref());

    EXPECT_EQ(result.get(64), Logic::Zero);
    EXPECT_EQ(result.get(63), Logic::One);
    EXPECT_EQ(result.get(0), Logic::One);
}

TEST(BitOpsArithmetic, MultiplyKeepsTheLowBits) {
    EXPECT_EQ(applyBinary(bitops::multiply, "8'd12", "8'd12"), "10010000") << "144";
    EXPECT_EQ(applyBinary(bitops::multiply, "8'd16", "8'd16"), "00000000") << "256 wraps to 0";
    EXPECT_EQ(applyBinary(bitops::multiply, "4'b0011", "4'b0101"), "1111") << "3 * 5 = 15";
}

TEST(BitOpsArithmetic, MultiplyAcrossLimbs) {
    const BitVector a = literal("128'h1_0000_0000");
    BitVector result(128);
    bitops::multiply(result.mutableRef(), a.ref(), a.ref());

    EXPECT_EQ(result.get(64), Logic::One) << "2^32 squared is 2^64";
    EXPECT_EQ(result.get(63), Logic::Zero);
}

TEST(BitOpsArithmetic, UnsignedDivideAndModulo) {
    const auto divide = [](std::string_view a, std::string_view b) {
        return applyBinary([](auto d, auto x, auto y) { bitops::divide(d, x, y, false); }, a, b);
    };
    const auto modulo = [](std::string_view a, std::string_view b) {
        return applyBinary([](auto d, auto x, auto y) { bitops::modulo(d, x, y, false); }, a, b);
    };

    EXPECT_EQ(divide("8'd100", "8'd7"), "00001110") << "14";
    EXPECT_EQ(modulo("8'd100", "8'd7"), "00000010") << "2";
    EXPECT_EQ(divide("8'd7", "8'd100"), "00000000");
    EXPECT_EQ(modulo("8'd7", "8'd100"), "00000111");
    EXPECT_EQ(divide("8'hFF", "8'd1"), "11111111");
}

TEST(BitOpsArithmetic, SignedDivideTruncatesTowardsZero) {
    const auto divide = [](std::string_view a, std::string_view b) {
        return applyBinary([](auto d, auto x, auto y) { bitops::divide(d, x, y, true); }, a, b);
    };

    // -100 / 7 is -14, not -15: the language truncates rather than flooring.
    EXPECT_EQ(enzan::eir::toString(literal("8'd156").ref(), Radix::Decimal, true), "-100");
    EXPECT_EQ(divide("8'd156", "8'd7"), "11110010") << "-14";
    EXPECT_EQ(divide("8'd100", "8'd249"), "11110010") << "100 / -7 = -14";
    EXPECT_EQ(divide("8'd156", "8'd249"), "00001110") << "-100 / -7 = 14";
}

TEST(BitOpsArithmetic, SignedModuloTakesTheSignOfTheDividend) {
    const auto modulo = [](std::string_view a, std::string_view b) {
        return applyBinary([](auto d, auto x, auto y) { bitops::modulo(d, x, y, true); }, a, b);
    };

    // This is where the language differs from a mathematical modulus.
    EXPECT_EQ(modulo("8'd156", "8'd7"), "11111110") << "-100 % 7 = -2";
    EXPECT_EQ(modulo("8'd100", "8'd249"), "00000010") << "100 % -7 = 2";
}

TEST(BitOpsArithmetic, DivisionByZeroIsUnknownRatherThanFatal) {
    // A design that divides by zero for one cycle during reset is common enough
    // that aborting would make the simulator unusable on real code.
    EXPECT_EQ(applyBinary([](auto d, auto a, auto b) { bitops::divide(d, a, b, false); }, "8'd100",
                          "8'd0"),
              "xxxxxxxx");
    EXPECT_EQ(applyBinary([](auto d, auto a, auto b) { bitops::modulo(d, a, b, false); }, "8'd100",
                          "8'd0"),
              "xxxxxxxx");
}

TEST(BitOpsArithmetic, Negate) {
    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::negate(d, a); }, "8'd1"), "11111111");
    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::negate(d, a); }, "8'd0"), "00000000");
    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::negate(d, a); }, "8'h80"), "10000000")
        << "the most negative value negates to itself";
    EXPECT_EQ(applyUnary([](auto d, auto a) { bitops::negate(d, a); }, "8'bxxxxxxx1"), "xxxxxxxx");
}

// ---------------------------------------------------------------------------
// Shifts
// ---------------------------------------------------------------------------

TEST(BitOpsShift, LeftFillsWithZeroes) {
    const auto shift = [](std::string_view value, std::string_view amount) {
        return applyBinary(bitops::shiftLeft, value, amount);
    };

    EXPECT_EQ(shift("8'b0000_0001", "8'd3"), "00001000");
    EXPECT_EQ(shift("8'b1010_1010", "8'd1"), "01010100");
    EXPECT_EQ(shift("8'b1010_1010", "8'd0"), "10101010");
}

TEST(BitOpsShift, RightLogicalFillsWithZeroes) {
    EXPECT_EQ(applyBinary(bitops::shiftRightLogical, "8'b1000_0000", "8'd3"), "00010000");
    EXPECT_EQ(applyBinary(bitops::shiftRightLogical, "8'b1111_1111", "8'd4"), "00001111");
}

TEST(BitOpsShift, RightArithmeticFillsWithTheSignBit) {
    EXPECT_EQ(applyBinary(bitops::shiftRightArithmetic, "8'b1000_0000", "8'd3"), "11110000");
    EXPECT_EQ(applyBinary(bitops::shiftRightArithmetic, "8'b0100_0000", "8'd3"), "00001000");

    // An unknown sign bit is what gets replicated, not a guess.
    EXPECT_EQ(applyBinary(bitops::shiftRightArithmetic, "8'bx100_0000", "8'd3"), "xxxx1000");
}

TEST(BitOpsShift, ShiftingByAtLeastTheWidthClearsEverything) {
    // Defined behaviour in the language and in hardware, unlike C's shift by width.
    EXPECT_EQ(applyBinary(bitops::shiftLeft, "8'hFF", "8'd8"), "00000000");
    EXPECT_EQ(applyBinary(bitops::shiftLeft, "8'hFF", "8'd200"), "00000000");
    EXPECT_EQ(applyBinary(bitops::shiftRightLogical, "8'hFF", "8'd8"), "00000000");
    EXPECT_EQ(applyBinary(bitops::shiftRightArithmetic, "8'h80", "8'd8"), "11111111")
        << "the sign fills the whole result";
}

TEST(BitOpsShift, AnUnknownAmountMakesEverythingUnknown) {
    // The amount decides where every bit lands, so none of them is known.
    EXPECT_EQ(applyBinary(bitops::shiftLeft, "8'hFF", "8'bxxxxxxxx"), "xxxxxxxx");
    EXPECT_EQ(applyBinary(bitops::shiftLeft, "8'hFF", "8'b0000_000z"), "xxxxxxxx");
}

TEST(BitOpsShift, MovesUnknownBitsAlongWithKnownOnes) {
    EXPECT_EQ(applyBinary(bitops::shiftLeft, "4'b001x", "4'd1"), "01x0");
    EXPECT_EQ(applyBinary(bitops::shiftRightLogical, "4'bx100", "4'd1"), "0x10");
}

TEST(BitOpsShift, AcrossLimbBoundaries) {
    BitVector value = BitVector::filled(130, Logic::Zero);
    value.set(0, Logic::One);
    const BitVector amount = BitVector::fromUInt64(70, 130);

    BitVector result(130);
    bitops::shiftLeft(result.mutableRef(), value.ref(), amount.ref());

    EXPECT_EQ(result.get(70), Logic::One);
    EXPECT_EQ(result.get(0), Logic::Zero);
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

TEST(BitOpsCompare, EqualityIsUnknownOnlyWhenTheUnknownBitsCouldDecideIt) {
    EXPECT_EQ(applyPredicate(bitops::equal, "4'b1010", "4'b1010"), '1');
    EXPECT_EQ(applyPredicate(bitops::equal, "4'b1010", "4'b1011"), '0');
    EXPECT_EQ(applyPredicate(bitops::equal, "4'b101x", "4'b1010"), 'x');
    EXPECT_EQ(applyPredicate(bitops::equal, "4'b101z", "4'b101z"), 'x');

    // A bit known on both sides and different settles it, whatever the unknown bits hold
    // (IEEE 1800 section 11.4.5).
    EXPECT_EQ(applyPredicate(bitops::equal, "4'b001x", "4'b1010"), '0');
    EXPECT_EQ(applyPredicate(bitops::equal, "4'bz010", "4'b0011"), '0');

    EXPECT_EQ(applyPredicate(bitops::notEqual, "4'b1010", "4'b1011"), '1');
    EXPECT_EQ(applyPredicate(bitops::notEqual, "4'b1010", "4'b1010"), '0');
    EXPECT_EQ(applyPredicate(bitops::notEqual, "4'b101x", "4'b1010"), 'x');
    EXPECT_EQ(applyPredicate(bitops::notEqual, "4'b001x", "4'b1010"), '1');
}

TEST(BitOpsCompare, AKnownMismatchSettlesEqualityAcrossLimbs) {
    // The mismatch in the top limb, the unknown in the bottom one.
    BitVector a = BitVector::filled(130, Logic::Zero);
    const BitVector b = BitVector::filled(130, Logic::Zero);
    a.set(0, Logic::Unknown);
    a.set(129, Logic::One);

    BitVector result(1);
    bitops::equal(result.mutableRef(), a.ref(), b.ref());
    EXPECT_EQ(result.get(0), Logic::Zero);

    a.set(129, Logic::Zero);
    bitops::equal(result.mutableRef(), a.ref(), b.ref());
    EXPECT_EQ(result.get(0), Logic::Unknown);
}

TEST(BitOpsCompare, WildcardEqualityTreatsTheRightOperandsUnknownsAsAnything) {
    // IEEE 1800 section 11.4.6.
    EXPECT_EQ(applyPredicate(bitops::wildcardEqual, "4'b1010", "4'b1x1z"), '1');
    EXPECT_EQ(applyPredicate(bitops::wildcardEqual, "4'b0010", "4'b1x1z"), '0');
    EXPECT_EQ(applyPredicate(bitops::wildcardEqual, "4'b1010", "4'b1010"), '1');

    // The left operand's unknowns are not wildcards: where they meet a known bit the result
    // is x, unless a known mismatch elsewhere settles it.
    EXPECT_EQ(applyPredicate(bitops::wildcardEqual, "4'b1x10", "4'b1010"), 'x');
    EXPECT_EQ(applyPredicate(bitops::wildcardEqual, "4'b1x10", "4'b1x10"), '1');
    EXPECT_EQ(applyPredicate(bitops::wildcardEqual, "4'b0x10", "4'b1010"), '0');

    EXPECT_EQ(applyPredicate(bitops::wildcardNotEqual, "4'b0010", "4'b1x1z"), '1');
    EXPECT_EQ(applyPredicate(bitops::wildcardNotEqual, "4'b1010", "4'b1x1z"), '0');
    EXPECT_EQ(applyPredicate(bitops::wildcardNotEqual, "4'b1x10", "4'b1010"), 'x');
}

TEST(BitOpsCompare, CasezIgnoresZOnEitherSideAndComparesXExactly) {
    // IEEE 1800 section 12.5.1: z in the selector or the item is a don't-care; x is a value.
    EXPECT_EQ(applyPredicate(bitops::caseZMatch, "4'b1010", "4'b1z1z"), '1');
    EXPECT_EQ(applyPredicate(bitops::caseZMatch, "4'b1z1z", "4'b1010"), '1');
    EXPECT_EQ(applyPredicate(bitops::caseZMatch, "4'b1010", "4'b0z1z"), '0');
    EXPECT_EQ(applyPredicate(bitops::caseZMatch, "4'b1x10", "4'b1x10"), '1');
    EXPECT_EQ(applyPredicate(bitops::caseZMatch, "4'b1x10", "4'b1010"), '0');
}

TEST(BitOpsCompare, CasexIgnoresXAndZOnEitherSide) {
    EXPECT_EQ(applyPredicate(bitops::caseXMatch, "4'b1010", "4'b1x1z"), '1');
    EXPECT_EQ(applyPredicate(bitops::caseXMatch, "4'b1x10", "4'b1010"), '1');
    EXPECT_EQ(applyPredicate(bitops::caseXMatch, "4'b1010", "4'b0x1z"), '0');
}

TEST(BitOpsCompare, WildcardComparisonsWorkAcrossLimbs) {
    BitVector a = BitVector::filled(130, Logic::Zero);
    BitVector b = BitVector::filled(130, Logic::Zero);
    a.set(129, Logic::One);
    b.set(129, Logic::HighZ);

    BitVector result(1);
    bitops::wildcardEqual(result.mutableRef(), a.ref(), b.ref());
    EXPECT_EQ(result.get(0), Logic::One);
    bitops::caseZMatch(result.mutableRef(), a.ref(), b.ref());
    EXPECT_EQ(result.get(0), Logic::One);

    b.set(129, Logic::Zero);
    bitops::caseXMatch(result.mutableRef(), a.ref(), b.ref());
    EXPECT_EQ(result.get(0), Logic::Zero);
}

TEST(BitOpsCompare, CaseEqualityNeverProducesUnknown) {
    // `===` compares the encoding, which is why it distinguishes x from z and is
    // the only comparison usable in a testbench check against an unknown.
    EXPECT_EQ(applyPredicate(bitops::caseEqual, "4'b101x", "4'b101x"), '1');
    EXPECT_EQ(applyPredicate(bitops::caseEqual, "4'b101x", "4'b101z"), '0');
    EXPECT_EQ(applyPredicate(bitops::caseEqual, "4'b101x", "4'b1010"), '0');

    EXPECT_EQ(applyPredicate(bitops::caseNotEqual, "4'b101x", "4'b101z"), '1');
    EXPECT_EQ(applyPredicate(bitops::caseNotEqual, "4'b101x", "4'b101x"), '0');
}

TEST(BitOpsCompare, UnsignedOrdering) {
    const auto less = [](std::string_view a, std::string_view b) {
        return applyPredicate([](auto d, auto x, auto y) { bitops::lessThan(d, x, y, false); }, a,
                              b);
    };

    EXPECT_EQ(less("8'd1", "8'd2"), '1');
    EXPECT_EQ(less("8'd2", "8'd1"), '0');
    EXPECT_EQ(less("8'd1", "8'd1"), '0');
    EXPECT_EQ(less("8'd1", "8'hFF"), '1') << "0xFF is 255 when unsigned";
    EXPECT_EQ(less("8'd1", "8'bxxxx_xxxx"), 'x');
}

TEST(BitOpsCompare, SignedOrdering) {
    const auto less = [](std::string_view a, std::string_view b) {
        return applyPredicate([](auto d, auto x, auto y) { bitops::lessThan(d, x, y, true); }, a,
                              b);
    };

    EXPECT_EQ(less("8'hFF", "8'd1"), '1') << "0xFF is -1 when signed";
    EXPECT_EQ(less("8'd1", "8'hFF"), '0');
    EXPECT_EQ(less("8'h80", "8'hFF"), '1') << "-128 < -1";
    EXPECT_EQ(less("8'hFF", "8'h80"), '0');
    EXPECT_EQ(less("8'h7F", "8'h80"), '0') << "127 is not less than -128";
}

TEST(BitOpsCompare, LessEqual) {
    const auto lessEqual = [](std::string_view a, std::string_view b) {
        return applyPredicate([](auto d, auto x, auto y) { bitops::lessEqual(d, x, y, false); }, a,
                              b);
    };

    EXPECT_EQ(lessEqual("8'd1", "8'd1"), '1');
    EXPECT_EQ(lessEqual("8'd2", "8'd1"), '0');
    EXPECT_EQ(lessEqual("8'd1", "8'd2"), '1');
}

TEST(BitOpsCompare, OrderingAcrossLimbs) {
    const BitVector low = literal("128'h1");
    const BitVector high = literal("128'h1_0000_0000_0000_0000");
    BitVector result(1);

    bitops::lessThan(result.mutableRef(), low.ref(), high.ref(), false);
    EXPECT_EQ(result.get(0), Logic::One);

    bitops::lessThan(result.mutableRef(), high.ref(), low.ref(), false);
    EXPECT_EQ(result.get(0), Logic::Zero);
}

// ---------------------------------------------------------------------------
// Selection and splicing
// ---------------------------------------------------------------------------

TEST(BitOpsSelect, BitSelect) {
    const BitVector source = literal("4'b10xz");
    BitVector result(1);

    for (const auto& [index, expected] :
         {std::pair{0U, 'z'}, std::pair{1U, 'x'}, std::pair{2U, '0'}, std::pair{3U, '1'}}) {
        const BitVector position = BitVector::fromUInt64(index, 8);
        bitops::bitSelect(result.mutableRef(), source.ref(), position.ref());
        EXPECT_EQ(enzan::eir::toChar(result.get(0)), expected) << "bit " << index;
    }
}

TEST(BitOpsSelect, OutOfRangeBitSelectIsUnknown) {
    // IEEE 1800 defines it that way, and designs do index out of range at run time.
    const BitVector source = literal("4'b1010");
    const BitVector position = BitVector::fromUInt64(4, 8);
    BitVector result(1);

    bitops::bitSelect(result.mutableRef(), source.ref(), position.ref());
    EXPECT_EQ(result.get(0), Logic::Unknown);
}

TEST(BitOpsSelect, UnknownIndexMakesTheSelectUnknown) {
    const BitVector source = literal("4'b1010");
    const BitVector position = literal("8'bxxxx_xxxx");
    BitVector result(1);

    bitops::bitSelect(result.mutableRef(), source.ref(), position.ref());
    EXPECT_EQ(result.get(0), Logic::Unknown);
}

TEST(BitOpsSelect, PartSelect) {
    const BitVector source = literal("8'b1101_0010");
    BitVector result(4);
    const BitVector low = BitVector::fromUInt64(2, 8);

    bitops::partSelect(result.mutableRef(), source.ref(), low.ref());
    EXPECT_EQ(binary(result.ref()), "0100");
}

TEST(BitOpsSelect, PartSelectPastTheEndReadsUnknown) {
    const BitVector source = literal("8'b1101_0010");
    BitVector result(4);
    const BitVector low = BitVector::fromUInt64(6, 8);

    bitops::partSelect(result.mutableRef(), source.ref(), low.ref());
    EXPECT_EQ(binary(result.ref()), "xx11") << "bits 8 and 9 do not exist";
}

TEST(BitOpsSelect, AssignPartWritesAWindow) {
    BitVector destination = BitVector::filled(8, Logic::Zero);
    const BitVector source = literal("4'b1111");
    const BitVector low = BitVector::fromUInt64(2, 8);

    bitops::assignPart(destination.mutableRef(), source.ref(), low.ref());
    EXPECT_EQ(binary(destination.ref()), "00111100");
}

TEST(BitOpsSelect, AssignPartDiscardsBitsPastTheEnd) {
    BitVector destination = BitVector::filled(8, Logic::Zero);
    const BitVector source = literal("4'b1111");
    const BitVector low = BitVector::fromUInt64(6, 8);

    bitops::assignPart(destination.mutableRef(), source.ref(), low.ref());
    EXPECT_EQ(binary(destination.ref()), "11000000");
}

TEST(BitOpsSelect, AssignPartThroughAnUnknownIndexWritesNothing) {
    // The target of the write is unknown, so nothing may be written. Making the
    // whole vector X because one index bit was unknown would lose far more
    // information than dropping the assignment does.
    BitVector destination = BitVector::filled(8, Logic::Zero);
    const BitVector source = literal("4'b1111");
    const BitVector low = literal("8'bxxxx_xxxx");

    bitops::assignPart(destination.mutableRef(), source.ref(), low.ref());
    EXPECT_EQ(binary(destination.ref()), "00000000");
}

// ---------------------------------------------------------------------------
// The conditional operator
// ---------------------------------------------------------------------------

TEST(BitOpsConditional, SelectsAnArmWhenTheConditionIsKnown) {
    const BitVector whenTrue = literal("4'b1100");
    const BitVector whenFalse = literal("4'b0011");
    BitVector result(4);

    const BitVector yes = literal("1'b1");
    bitops::conditional(result.mutableRef(), yes.ref(), whenTrue.ref(), whenFalse.ref());
    EXPECT_EQ(binary(result.ref()), "1100");

    const BitVector no = literal("1'b0");
    bitops::conditional(result.mutableRef(), no.ref(), whenTrue.ref(), whenFalse.ref());
    EXPECT_EQ(binary(result.ref()), "0011");
}

TEST(BitOpsConditional, MergesPerBitWhenTheConditionIsUnknown) {
    // This is the rule that stops an unknown reset from turning a whole datapath X
    // when both arms would have driven the same value. Bits where the arms agree
    // stay known.
    const BitVector whenTrue = literal("4'b1100");
    const BitVector whenFalse = literal("4'b1010");
    const BitVector unknown = literal("1'bx");
    BitVector result(4);

    bitops::conditional(result.mutableRef(), unknown.ref(), whenTrue.ref(), whenFalse.ref());
    EXPECT_EQ(binary(result.ref()), "1xx0");
}

TEST(BitOpsConditional, AgreeingUnknownBitsSurviveTheMerge) {
    // Both arms hold z in the same position, so the result is z and not x: the
    // merge compares the encoding, not just the known-ness.
    const BitVector whenTrue = literal("4'b1z00");
    const BitVector whenFalse = literal("4'b1z11");
    const BitVector unknown = literal("1'bx");
    BitVector result(4);

    bitops::conditional(result.mutableRef(), unknown.ref(), whenTrue.ref(), whenFalse.ref());
    EXPECT_EQ(binary(result.ref()), "1zxx");
}

TEST(BitOpsConditional, MergeAcrossLimbs) {
    const BitVector arm = BitVector::filled(130, Logic::One);
    BitVector other = BitVector::filled(130, Logic::One);
    other.set(129, Logic::Zero);
    const BitVector unknown = literal("1'bx");
    BitVector result(130);

    bitops::conditional(result.mutableRef(), unknown.ref(), arm.ref(), other.ref());

    EXPECT_EQ(result.get(0), Logic::One) << "agreeing bits stay known";
    EXPECT_EQ(result.get(129), Logic::Unknown) << "the differing bit becomes x";
}

TEST(BitOpsCount, CeilLog2RoundsUpAndIsZeroForZeroAndOne) {
    EXPECT_EQ(applyUnary(bitops::ceilLog2, "8'd0"), "00000000");
    EXPECT_EQ(applyUnary(bitops::ceilLog2, "8'd1"), "00000000");
    EXPECT_EQ(applyUnary(bitops::ceilLog2, "8'd2"), "00000001");
    EXPECT_EQ(applyUnary(bitops::ceilLog2, "8'd5"), "00000011");
    EXPECT_EQ(applyUnary(bitops::ceilLog2, "8'd128"), "00000111");
    EXPECT_EQ(applyUnary(bitops::ceilLog2, "8'd129"), "00001000");
    EXPECT_EQ(applyUnary(bitops::ceilLog2, "8'b0000_01x1"), "xxxxxxxx");
}

TEST(BitOpsCount, CeilLog2LooksAcrossWords) {
    // 2**64 is a power of two in the second word; one more bit in the first rounds it up.
    BitVector value = BitVector::filled(100, Logic::Zero);
    value.mutableRef().set(64, Logic::One);
    BitVector result(32);
    bitops::ceilLog2(result.mutableRef(), value.ref());
    EXPECT_EQ(result.ref().toUInt64(), 64U);
    value.mutableRef().set(3, Logic::One);
    bitops::ceilLog2(result.mutableRef(), value.ref());
    EXPECT_EQ(result.ref().toUInt64(), 65U);
}

TEST(BitOpsCount, CountOnesCountsOnlyBitsThatAreExactlyOne) {
    EXPECT_EQ(applyUnary(bitops::countOnes, "8'b1010_0x1z"), "00000011");
    EXPECT_EQ(applyUnary(bitops::countOnes, "8'b0000_0000"), "00000000");

    // Across limbs, and into a result narrower than the operand.
    const BitVector wide = BitVector::filled(130, Logic::One);
    BitVector result(32);
    bitops::countOnes(result.mutableRef(), wide.ref());
    EXPECT_EQ(result.ref().toUInt64(), 130U);
}

// ---------------------------------------------------------------------------
// Power (IEEE 1800 section 11.4.3, table 11-4)
// ---------------------------------------------------------------------------

std::string applyPower(std::string_view base, std::string_view exponent, bool baseSigned,
                       bool exponentSigned) {
    const BitVector a = literal(base);
    const BitVector b = literal(exponent);
    BitVector result(a.width());
    bitops::power(result.mutableRef(), a.ref(), b.ref(), baseSigned, exponentSigned);
    return binary(result.ref());
}

TEST(BitOpsPower, RaisesModuloTheWidth) {
    EXPECT_EQ(applyPower("8'd3", "4'd4", false, false), "01010001"); // 81
    EXPECT_EQ(applyPower("8'd2", "4'd9", false, false), "00000000"); // 512 wraps to 0
    EXPECT_EQ(applyPower("8'd7", "4'd0", false, false), "00000001");
    EXPECT_EQ(applyPower("8'd0", "4'd0", false, false), "00000001");
    EXPECT_EQ(applyPower("8'd0", "4'd3", false, false), "00000000");
}

TEST(BitOpsPower, AnUnknownOperandMakesTheWholeResultX) {
    EXPECT_EQ(applyPower("8'd3", "4'b0x01", false, false), "xxxxxxxx");
    EXPECT_EQ(applyPower("8'b0000001z", "4'd2", false, false), "xxxxxxxx");
}

TEST(BitOpsPower, ANegativeExponentFollowsTheStandardsTable) {
    // Only a signed exponent can be negative; 4'b1101 is -3 signed and 13 unsigned.
    EXPECT_EQ(applyPower("8'd0", "4'b1101", true, true), "xxxxxxxx");
    EXPECT_EQ(applyPower("8'd1", "4'b1101", true, true), "00000001");
    EXPECT_EQ(applyPower("8'hff", "4'b1101", true, true), "11111111"); // -1 ** odd
    EXPECT_EQ(applyPower("8'hff", "4'b1110", true, true), "00000001"); // -1 ** even
    EXPECT_EQ(applyPower("8'd5", "4'b1101", true, true), "00000000");
    // An unsigned base of all ones is 255, not -1.
    EXPECT_EQ(applyPower("8'hff", "4'b1101", false, true), "00000000");
    // The same bits as an unsigned exponent are 13.
    EXPECT_EQ(applyPower("8'd1", "4'b1101", false, false), "00000001");
}

TEST(BitOpsPower, WideValuesGiveTheSameAnswerAsNarrowOnes) {
    // 3 ** 50 modulo 2 ** 96, checked against Icarus Verilog.
    const BitVector base = literal("96'd3");
    const BitVector exponent = literal("7'd50");
    BitVector result(96);
    bitops::power(result.mutableRef(), base.ref(), exponent.ref(), false, false);
    EXPECT_EQ(enzan::eir::toString(result.ref(), Radix::Hexadecimal), "0000980553f0db2fd09de3c9");
}

// ---------------------------------------------------------------------------
// Resolution (DESIGN_GUIDE.md section 3.7; IEEE 1800 tables 6-2 to 6-4)
// ---------------------------------------------------------------------------

/// A resolution table as the standard prints it: the row is the first driver and the column
/// the second, both in the order 0, 1, x, z.
using ResolutionTable = std::array<std::string_view, 4>;

constexpr std::array<Logic, 4> TableOrder = {Logic::Zero, Logic::One, Logic::Unknown, Logic::HighZ};

Logic fromTableChar(char value) {
    switch (value) {
    case '0': return Logic::Zero;
    case '1': return Logic::One;
    case 'z': return Logic::HighZ;
    default: return Logic::Unknown;
    }
}

template<typename Resolve>
void expectTable(Resolve resolveFunction, const ResolutionTable& table) {
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            BitVector acc(1);
            acc.set(0, TableOrder[row]);
            BitVector driver(1);
            driver.set(0, TableOrder[column]);
            resolveFunction(acc.mutableRef(), driver.ref());

            EXPECT_EQ(acc.get(0), fromTableChar(table[row][column]))
                << "row " << enzan::eir::toChar(TableOrder[row]) << ", column "
                << enzan::eir::toChar(TableOrder[column]);
        }
    }
}

TEST(BitOpsResolution, WireMatchesTheStandardsTable) {
    //                    0      1      x      z
    expectTable(bitops::resolveWire, {"0xx0", "x1x1", "xxxx", "01xz"});
}

TEST(BitOpsResolution, WiredAndMatchesTheStandardsTable) {
    expectTable(bitops::resolveWiredAnd, {"0000", "01x1", "0xxx", "01xz"});
}

TEST(BitOpsResolution, WiredOrMatchesTheStandardsTable) {
    expectTable(bitops::resolveWiredOr, {"01x0", "1111", "x1xx", "01xz"});
}

TEST(BitOpsResolution, PullReplacesOnlyHighImpedance) {
    BitVector value = literal("4'bz01x");
    bitops::pullHighImpedance(value.mutableRef(), Logic::Zero);
    EXPECT_EQ(binary(value.ref()), "001x");

    value = literal("4'bz01x");
    bitops::pullHighImpedance(value.mutableRef(), Logic::One);
    EXPECT_EQ(binary(value.ref()), "101x");
}

TEST(BitOpsResolution, WideVectorsResolveBitByBitAcrossLimbs) {
    // Every pairing of the four values at every position, over three limbs, compared with the
    // single-bit result of the same table.
    constexpr std::uint32_t Width = 150;
    BitVector left(Width);
    BitVector right(Width);
    for (std::uint32_t bit = 0; bit < Width; ++bit) {
        left.set(bit, TableOrder[bit % 4]);
        right.set(bit, TableOrder[(bit / 4) % 4]);
    }

    for (const auto resolveFunction :
         {bitops::resolveWire, bitops::resolveWiredAnd, bitops::resolveWiredOr}) {
        BitVector acc = left;
        resolveFunction(acc.mutableRef(), right.ref());

        for (std::uint32_t bit = 0; bit < Width; ++bit) {
            BitVector oneLeft(1);
            oneLeft.set(0, left.get(bit));
            BitVector oneRight(1);
            oneRight.set(0, right.get(bit));
            resolveFunction(oneLeft.mutableRef(), oneRight.ref());
            ASSERT_EQ(acc.get(bit), oneLeft.get(0)) << "bit " << bit;
        }
    }
}

} // namespace
