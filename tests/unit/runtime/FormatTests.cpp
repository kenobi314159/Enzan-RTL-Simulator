// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Format.h"

#include "eir/BitVector.h"
#include "runtime/Time.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::eir::BitVector;
using enzan::runtime::FormatArgument;
using enzan::runtime::FormatContext;
using enzan::runtime::TimeScale;

BitVector literal(std::string_view text) {
    std::optional<BitVector> parsed = BitVector::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(BitVector::filled(1, enzan::eir::Logic::Unknown));
}

/// Formats with default context, so that the common case reads as one line.
std::string render(std::string_view format, const std::vector<FormatArgument>& arguments = {}) {
    const FormatContext context{.time = 0, .timeScale = TimeScale(-12), .scopeName = "/tb"};
    return enzan::runtime::formatSystemTask(format, arguments, context);
}

TEST(Format, LiteralTextPassesThrough) {
    EXPECT_EQ(render("hello"), "hello");
    EXPECT_EQ(render(""), "");
    EXPECT_EQ(render("a\nb"), "a\nb");
}

TEST(Format, PercentPercentIsALiteralPercent) {
    EXPECT_EQ(render("100%%"), "100%");
}

TEST(Format, DecimalIsPaddedUnlessTheZeroFlagIsGiven) {
    // IEEE 1800 pads a %d to the width the type can need, which is what makes columns
    // line up in a transcript. %0d is the minimal form almost everyone wants.
    const BitVector value = literal("8'd7");
    const std::vector<FormatArgument> arguments{{value.ref(), false}};

    EXPECT_EQ(render("%d", arguments), "  7") << "an 8-bit value needs three digits";
    EXPECT_EQ(render("%0d", arguments), "7");
}

TEST(Format, SignedDecimalUsesTheArgumentsSignedness) {
    // The format string does not say it: %d prints -1 or 255 for the same bits depending
    // on the argument's declared type.
    const BitVector value = literal("8'hFF");
    EXPECT_EQ(render("%0d", {{value.ref(), true}}), "-1");
    EXPECT_EQ(render("%0d", {{value.ref(), false}}), "255");
}

TEST(Format, HexadecimalBinaryAndOctal) {
    const BitVector value = literal("8'hA5");
    const std::vector<FormatArgument> arguments{{value.ref(), false}};

    EXPECT_EQ(render("%h", arguments), "a5");
    EXPECT_EQ(render("%x", arguments), "a5");
    EXPECT_EQ(render("%b", arguments), "10100101");
    EXPECT_EQ(render("%o", arguments), "245");
}

TEST(Format, UppercaseConversionsUppercaseTheDigits) {
    const BitVector value = literal("8'hAB");
    EXPECT_EQ(render("%H", {{value.ref(), false}}), "AB");
    EXPECT_EQ(render("%X", {{value.ref(), false}}), "AB");
}

TEST(Format, UnknownBitsRenderAsTheValueEngineRendersThem) {
    const BitVector value = literal("8'b1010_xxxx");
    EXPECT_EQ(render("%b", {{value.ref(), false}}), "1010xxxx");
    EXPECT_EQ(render("%h", {{value.ref(), false}}), "ax");
    EXPECT_EQ(render("%0d", {{value.ref(), false}}), "X")
        << "decimal cannot localise an unknown to a digit";
}

TEST(Format, SeveralConversionsConsumeArgumentsInOrder) {
    const BitVector first = literal("8'd1");
    const BitVector second = literal("8'd2");
    EXPECT_EQ(render("%0d then %0d", {{first.ref(), false}, {second.ref(), false}}), "1 then 2");
}

TEST(Format, TimeUsesTheGlobalPrecision) {
    const FormatContext context{.time = 1234, .timeScale = TimeScale(-9), .scopeName = "/tb"};
    EXPECT_EQ(enzan::runtime::formatSystemTask("at %t", {}, context), "at 1234ns");
}

TEST(Format, ScopeNameExpandsToTheHierarchicalPath) {
    const FormatContext context{.time = 0, .timeScale = TimeScale(), .scopeName = "/tb/u_dut"};
    EXPECT_EQ(enzan::runtime::formatSystemTask("in %m", {}, context), "in /tb/u_dut");
}

TEST(Format, StringConversionRendersAscii) {
    // A string literal is packed most significant byte first, and zero-extension on the
    // left must not put invisible control characters in the transcript.
    const BitVector text = literal("40'h48_65_6C_6C_6F");
    EXPECT_EQ(render("%s", {{text.ref(), false}}), "Hello");

    const BitVector padded = literal("64'h0000_0000_4F_4B");
    EXPECT_EQ(render("%s", {{padded.ref(), false}}), "OK");
}

TEST(Format, AnUnrecognisedConversionIsShownRatherThanSwallowed) {
    // A format string Enzan does not understand must be visible in the transcript, not
    // silently lose output.
    EXPECT_EQ(render("%q"), "%q");
    EXPECT_EQ(render("%0q"), "%0q");
}

TEST(Format, ATrailingPercentIsShown) {
    EXPECT_EQ(render("done %"), "done %");
    EXPECT_EQ(render("done %0"), "done %0");
}

TEST(Format, TooFewArgumentsLeavesTheConversionVisible) {
    // A format string with too few arguments is a mistake in the design; showing the
    // unconsumed conversion is more useful than aborting the run.
    EXPECT_EQ(render("a=%0d b=%0d", {{literal("8'd1").ref(), false}}), "a=1 b=%0d");
}

TEST(Format, DefaultFormattingConcatenatesInDecimal) {
    // `$display(a, b)` has no separator, which matches every other simulator.
    const BitVector first = literal("8'd12");
    const BitVector second = literal("8'd34");
    const std::vector<FormatArgument> arguments{{first.ref(), false}, {second.ref(), false}};

    EXPECT_EQ(enzan::runtime::formatDefault(arguments), "1234");
    EXPECT_EQ(enzan::runtime::formatDefault({}), "");
}

TEST(Format, AsciiConversionDropsOnlyLeadingNuls) {
    const BitVector value = literal("24'h41_00_42");
    EXPECT_EQ(enzan::runtime::toAsciiString(value.ref()), std::string("A\0B", 3))
        << "an interior NUL is part of the string";
}

TEST(Format, PaddingWidthsMatchTheTypesRange) {
    // One digit for 4 bits, three for 8, five for 16: ceil(n * log10 2).
    EXPECT_EQ(render("%d", {{literal("4'd3").ref(), false}}), " 3");
    EXPECT_EQ(render("%d", {{literal("16'd3").ref(), false}}), "    3");
    EXPECT_EQ(render("%h", {{literal("12'h3").ref(), false}}), "003");
    EXPECT_EQ(render("%o", {{literal("9'd3").ref(), false}}), "003");
}

} // namespace
