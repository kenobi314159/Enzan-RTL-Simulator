// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/ParameterExpression.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace {

using enzan::eir::BitVector;
using enzan::eir::Logic;
using enzan::eir::Opcode;
using enzan::eir::ParameterError;
using enzan::eir::ParameterEvaluator;
using enzan::eir::ParameterExpressions;
using enzan::eir::ParameterNode;
using enzan::eir::ParameterNodeKind;
using enzan::eir::ParameterValue;

/// A 32-bit signed value, which is what an `int` parameter holds.
ParameterValue intValue(std::int64_t value) {
    return {.bits = BitVector::fromInt64(value, 32), .isSigned = true};
}

/// A two-operand operation of `width` bits.
ParameterNode operation(Opcode opcode, std::uint32_t a, std::uint32_t b, std::uint32_t width,
                        bool isSigned = true) {
    return {.kind = ParameterNodeKind::Operation,
            .opcode = opcode,
            .isSigned = isSigned,
            .operationSigned = isSigned,
            .width = width,
            .a = a,
            .b = b};
}

TEST(ParameterExpressions, IdenticalNodesAreStoredOnce) {
    // Every `[W-1:0]` in a unit is one node, which is what makes a width cheap to evaluate
    // and two widths comparable by index.
    ParameterExpressions table;
    const std::uint32_t width = table.formal(0, 32, true);
    const std::uint32_t one = table.constant(BitVector::fromInt64(1, 32), true);
    const std::uint32_t first = table.add(operation(Opcode::Subtract, width, one, 32));
    const std::uint32_t second = table.add(operation(Opcode::Subtract, width, one, 32));
    EXPECT_EQ(first, second);
    EXPECT_EQ(table.constant(BitVector::fromInt64(1, 32), true), one);
    EXPECT_EQ(table.size(), 3U);
}

TEST(ParameterEvaluator, AWidthIsArithmeticOverAFormal) {
    // `logic [W-1:0]` is W bits: (W - 1) - 0 + 1.
    ParameterExpressions table;
    const std::uint32_t w = table.formal(0, 32, true);
    const std::uint32_t one = table.constant(BitVector::fromInt64(1, 32), true);
    const std::uint32_t msb = table.add(operation(Opcode::Subtract, w, one, 32));
    const std::uint32_t width = table.add(operation(Opcode::Add, msb, one, 32));

    const std::vector<std::optional<ParameterValue>> formals{intValue(12)};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.integer(width), 12U);
    EXPECT_EQ(evaluator.integer(msb), 11U);
}

TEST(ParameterEvaluator, Clog2IsTheOperationProcessesUse) {
    ParameterExpressions table;
    const std::uint32_t depth = table.formal(0, 32, true);
    const std::uint32_t log = table.add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                      .opcode = Opcode::CeilLog2,
                                                      .isSigned = true,
                                                      .width = 32,
                                                      .a = depth});
    for (const auto& [value, expected] : std::vector<std::pair<std::int64_t, std::uint32_t>>{
             {1, 0}, {2, 1}, {5, 3}, {16, 4}, {17, 5}}) {
        const std::vector<std::optional<ParameterValue>> formals{intValue(value)};
        ParameterEvaluator evaluator(table, formals);
        EXPECT_EQ(evaluator.integer(log), expected) << "$clog2(" << value << ")";
    }
}

TEST(ParameterEvaluator, ASignedComparisonGivesAnUnsignedBit) {
    // -1 < 0 when the operands are signed; the 1-bit result is unsigned, so extending it gives
    // 1 and not -1.
    ParameterExpressions table;
    const std::uint32_t value = table.formal(0, 32, true);
    const std::uint32_t zero = table.constant(BitVector::fromInt64(0, 32), true);
    const std::uint32_t less = table.add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                       .opcode = Opcode::LessThan,
                                                       .isSigned = false,
                                                       .operationSigned = true,
                                                       .width = 1,
                                                       .a = value,
                                                       .b = zero});
    const std::uint32_t widened =
        table.add(ParameterNode{.kind = ParameterNodeKind::Extend, .width = 32, .a = less});

    const std::vector<std::optional<ParameterValue>> formals{intValue(-1)};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.integer(widened), 1U);
}

TEST(ParameterEvaluator, ExtensionFollowsTheOperandsSignedness) {
    ParameterExpressions table;
    const std::uint32_t signedByte = table.constant(BitVector::fromInt64(-2, 8), true);
    const std::uint32_t unsignedByte = table.constant(BitVector::fromUInt64(0xfe, 8), false);
    const std::uint32_t fromSigned =
        table.add(ParameterNode{.kind = ParameterNodeKind::Extend, .width = 16, .a = signedByte});
    const std::uint32_t fromUnsigned =
        table.add(ParameterNode{.kind = ParameterNodeKind::Extend, .width = 16, .a = unsignedByte});

    ParameterEvaluator evaluator(table, {});
    EXPECT_EQ(evaluator.value(fromSigned)->bits, BitVector::fromUInt64(0xfffe, 16));
    EXPECT_EQ(evaluator.value(fromUnsigned)->bits, BitVector::fromUInt64(0x00fe, 16));
}

TEST(ParameterEvaluator, AResultMayBeAsWideAsAnotherNodeSays) {
    // `{W{1'b1}}` as a constant: W ones.
    ParameterExpressions table;
    const std::uint32_t w = table.formal(0, 32, true);
    const std::uint32_t bit = table.constant(BitVector::fromUInt64(1, 1), false);
    const std::uint32_t ones =
        table.add(ParameterNode{.kind = ParameterNodeKind::Replicate, .a = w, .b = bit});
    const std::uint32_t widthOfOnes =
        table.add(ParameterNode{.kind = ParameterNodeKind::WidthOf, .isSigned = true, .a = ones});
    // And a zero of the same width, made by extending a one-bit zero to `W`.
    const std::uint32_t zeroBit = table.constant(BitVector::fromUInt64(0, 1), false);
    const std::uint32_t zero =
        table.add(ParameterNode{.kind = ParameterNodeKind::Extend, .widthNode = w, .a = zeroBit});

    const std::vector<std::optional<ParameterValue>> formals{intValue(5)};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.value(ones)->bits, BitVector::fromUInt64(0x1f, 5));
    EXPECT_EQ(evaluator.integer(widthOfOnes), 5U);
    EXPECT_EQ(evaluator.value(zero)->bits, BitVector::fromUInt64(0, 5));
}

TEST(ParameterEvaluator, AnEmptyReplicationIsAllowedOnlyInAConcatenation) {
    // `{{(W-N){1'b0}}, x}` with W == N is x; `{(W-N){1'b0}}` alone is no value at all.
    ParameterExpressions table;
    const std::uint32_t count = table.formal(0, 32, true);
    const std::uint32_t bit = table.constant(BitVector::fromUInt64(0, 1), false);
    const std::uint32_t padding =
        table.add(ParameterNode{.kind = ParameterNodeKind::Replicate, .a = count, .b = bit});
    const std::uint32_t x = table.constant(BitVector::fromUInt64(0xa, 4), false);
    const std::uint32_t padded =
        table.add(ParameterNode{.kind = ParameterNodeKind::Concat, .a = padding, .b = x});
    const std::uint32_t inverted = table.add(ParameterNode{
        .kind = ParameterNodeKind::Operation, .opcode = Opcode::Not, .width = 1, .a = padding});

    const std::vector<std::optional<ParameterValue>> formals{intValue(0)};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.value(padded)->bits, BitVector::fromUInt64(0xa, 4));
    EXPECT_EQ(evaluator.value(inverted), nullptr);
    EXPECT_EQ(evaluator.error(), ParameterError::EmptyReplication);
}

TEST(ParameterEvaluator, APartSelectStaysInsideItsSource) {
    ParameterExpressions table;
    const std::uint32_t source = table.constant(BitVector::fromUInt64(0xabcd, 16), false);
    const std::uint32_t low = table.formal(0, 32, true);
    const std::uint32_t nibble = table.add(
        ParameterNode{.kind = ParameterNodeKind::PartSelect, .width = 4, .a = source, .b = low});

    std::vector<std::optional<ParameterValue>> formals{intValue(4)};
    {
        ParameterEvaluator evaluator(table, formals);
        EXPECT_EQ(evaluator.value(nibble)->bits, BitVector::fromUInt64(0xc, 4));
    }
    formals[0] = intValue(13);
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.value(nibble), nullptr);
    EXPECT_EQ(evaluator.error(), ParameterError::SelectOutOfRange);
}

TEST(ParameterEvaluator, AnUnknownOrNegativeValueIsNoInteger) {
    ParameterExpressions table;
    const std::uint32_t value = table.formal(0, 32, true);

    std::vector<std::optional<ParameterValue>> formals{intValue(-3)};
    {
        ParameterEvaluator evaluator(table, formals);
        EXPECT_FALSE(evaluator.integer(value).has_value());
        EXPECT_EQ(evaluator.error(), ParameterError::NotAnInteger);
    }
    formals[0] = ParameterValue{.bits = BitVector::filled(32, Logic::Unknown), .isSigned = true};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_FALSE(evaluator.integer(value).has_value());
    EXPECT_EQ(evaluator.error(), ParameterError::NotAnInteger);
}

TEST(ParameterEvaluator, AWidthBeyondAnyObjectIsRefused) {
    ParameterExpressions table;
    const std::uint32_t w = table.formal(0, 32, true);
    const std::uint32_t zeroBit = table.constant(BitVector::fromUInt64(0, 1), false);
    const std::uint32_t zero =
        table.add(ParameterNode{.kind = ParameterNodeKind::Extend, .widthNode = w, .a = zeroBit});

    const std::vector<std::optional<ParameterValue>> formals{intValue(1 << 30)};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.value(zero), nullptr);
    EXPECT_EQ(evaluator.error(), ParameterError::TooWide);
    EXPECT_EQ(evaluator.failedNode(), w);
}

TEST(ParameterEvaluator, AWidthOfZeroIsRefused) {
    ParameterExpressions table;
    const std::uint32_t w = table.formal(0, 32, true);
    const std::uint32_t zeroBit = table.constant(BitVector::fromUInt64(0, 1), false);
    const std::uint32_t zero =
        table.add(ParameterNode{.kind = ParameterNodeKind::Extend, .widthNode = w, .a = zeroBit});

    const std::vector<std::optional<ParameterValue>> formals{intValue(0)};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.value(zero), nullptr);
    EXPECT_EQ(evaluator.error(), ParameterError::ZeroWidth);
}

TEST(ParameterEvaluator, AFormalWithNoValueIsReported) {
    ParameterExpressions table;
    const std::uint32_t w = table.formal(1, 32, true);
    const std::vector<std::optional<ParameterValue>> formals{intValue(1), std::nullopt};
    ParameterEvaluator evaluator(table, formals);
    EXPECT_EQ(evaluator.value(w), nullptr);
    EXPECT_EQ(evaluator.error(), ParameterError::MissingFormal);
}

TEST(ParameterExpressions, FromPartsRefusesWhatCouldNotHaveBeenBuilt) {
    // A decoder's table must be one `add` could have produced: operands before the node using
    // them, constants that exist, and no operation that does anything but compute.
    const std::vector<BitVector> constants{BitVector::fromInt64(1, 32)};
    const ParameterNode constant{.kind = ParameterNodeKind::Constant, .width = 32, .a = 0};

    EXPECT_TRUE(ParameterExpressions::fromParts({constant}, constants).has_value());
    EXPECT_FALSE(
        ParameterExpressions::fromParts({operation(Opcode::Add, 0, 1, 32), constant}, constants)
            .has_value())
        << "an operand after the node";
    EXPECT_FALSE(
        ParameterExpressions::fromParts(
            {ParameterNode{.kind = ParameterNodeKind::Constant, .width = 32, .a = 3}}, constants)
            .has_value())
        << "a constant that does not exist";
    EXPECT_FALSE(
        ParameterExpressions::fromParts({constant, operation(Opcode::Store, 0, 0, 32)}, constants)
            .has_value())
        << "an operation that is not an expression";
    EXPECT_FALSE(ParameterExpressions::fromParts({constant, constant}, constants).has_value())
        << "a node stored twice";
}

TEST(ParameterEvaluator, EachNodeIsEvaluatedOnce) {
    // A failure is remembered as well as a value, so asking again gives the same answer
    // without evaluating again.
    ParameterExpressions table;
    const std::uint32_t missing = table.formal(0, 32, true);
    ParameterEvaluator evaluator(table, {});
    EXPECT_EQ(evaluator.value(missing), nullptr);
    EXPECT_EQ(evaluator.value(missing), nullptr);
    EXPECT_EQ(evaluator.error(), ParameterError::MissingFormal);
}

} // namespace
