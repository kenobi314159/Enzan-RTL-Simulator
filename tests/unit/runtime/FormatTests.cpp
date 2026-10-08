// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Format.h"

#include "eir/BitVector.h"
#include "runtime/Time.h"

#include "support/Literals.h"

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
using enzan::test::literal;

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
    const std::vector<FormatArgument> arguments{{.value = value.ref(), .isSigned = false}};

    EXPECT_EQ(render("%d", arguments), "  7") << "an 8-bit value needs three digits";
    EXPECT_EQ(render("%0d", arguments), "7");
}

TEST(Format, SignedDecimalUsesTheArgumentsSignedness) {
    // The format string does not say it: %d prints -1 or 255 for the same bits depending
    // on the argument's declared type.
    const BitVector value = literal("8'hFF");
    EXPECT_EQ(render("%0d", {{.value = value.ref(), .isSigned = true}}), "-1");
    EXPECT_EQ(render("%0d", {{.value = value.ref(), .isSigned = false}}), "255");
}

TEST(Format, HexadecimalBinaryAndOctal) {
    const BitVector value = literal("8'hA5");
    const std::vector<FormatArgument> arguments{{.value = value.ref(), .isSigned = false}};

    EXPECT_EQ(render("%h", arguments), "a5");
    EXPECT_EQ(render("%x", arguments), "a5");
    EXPECT_EQ(render("%b", arguments), "10100101");
    EXPECT_EQ(render("%o", arguments), "245");
}

TEST(Format, AnUppercaseConversionIsTheLowercaseOne) {
    // Checked against Icarus Verilog. Uppercasing would also turn `x`, a digit wholly unknown,
    // into `X`, a digit partly unknown.
    const BitVector value = literal("8'hAB");
    EXPECT_EQ(render("%H", {{.value = value.ref(), .isSigned = false}}), "ab");
    EXPECT_EQ(render("%X", {{.value = value.ref(), .isSigned = false}}), "ab");

    const BitVector unknown = literal("8'hxx");
    EXPECT_EQ(render("%X", {{.value = unknown.ref(), .isSigned = false}}), "xx");
    EXPECT_EQ(render("%D", {{.value = unknown.ref(), .isSigned = false}}), "  x");
    EXPECT_EQ(render("%B", {{.value = literal("4'b1x0z").ref(), .isSigned = false}}), "1x0z");
}

TEST(Format, AMinimalRadixDropsItsLeadingZeroDigits) {
    // Checked against Icarus Verilog: zeros go, one digit stays, and an unknown digit is kept.
    const auto minimal = [](std::string_view format, std::string_view text) {
        const BitVector value = literal(text);
        return render(format, {{.value = value.ref(), .isSigned = false}});
    };
    EXPECT_EQ(minimal("%0h", "16'h00ab"), "ab");
    EXPECT_EQ(minimal("%0h", "16'h0x3f"), "x3f");
    EXPECT_EQ(minimal("%0h", "16'h00zz"), "zz");
    EXPECT_EQ(minimal("%0h", "32'hxxxxxxxx"), "xxxxxxxx");
    EXPECT_EQ(minimal("%0h", "8'h00"), "0");
    EXPECT_EQ(minimal("%0b", "8'b00000101"), "101");
    EXPECT_EQ(minimal("%0o", "9'o000"), "0");
    EXPECT_EQ(minimal("%0h", "12'b0000_01x0_0000"), "X0");
}

TEST(Format, UnknownBitsRenderAsTheValueEngineRendersThem) {
    const BitVector value = literal("8'b1010_xxxx");
    EXPECT_EQ(render("%b", {{.value = value.ref(), .isSigned = false}}), "1010xxxx");
    EXPECT_EQ(render("%h", {{.value = value.ref(), .isSigned = false}}), "ax");
    EXPECT_EQ(render("%0d", {{.value = value.ref(), .isSigned = false}}), "X")
        << "decimal cannot localise an unknown to a digit";
}

TEST(Format, SeveralConversionsConsumeArgumentsInOrder) {
    const BitVector first = literal("8'd1");
    const BitVector second = literal("8'd2");
    EXPECT_EQ(render("%0d then %0d", {{.value = first.ref(), .isSigned = false},
                                      {.value = second.ref(), .isSigned = false}}),
              "1 then 2");
}

TEST(Format, TimeFormatsItsArgumentWithTheGlobalUnit) {
    // `%t` renders a value it is given, not the clock: `$display("%t", $time)` passes the
    // time in. Reading the clock instead would make the conversion consume no argument, and
    // every later conversion would then take the wrong one.
    const BitVector value = literal("64'd1234");
    const std::vector<FormatArgument> arguments{{.value = value.ref(), .isSigned = false}};
    const FormatContext context{.time = 99, .timeScale = TimeScale(-9), .scopeName = "/tb"};

    EXPECT_EQ(enzan::runtime::formatSystemTask("at %t", arguments, context), "at 1234ns");
}

TEST(Format, TimeConsumesAnArgumentSoLaterConversionsAreNotShifted) {
    const BitVector now = literal("64'd40");
    const BitVector count = literal("8'd7");
    const std::vector<FormatArgument> arguments{{.value = now.ref(), .isSigned = false},
                                                {.value = count.ref(), .isSigned = false}};
    const FormatContext context{.time = 40, .timeScale = TimeScale(-9), .scopeName = "/tb"};

    EXPECT_EQ(enzan::runtime::formatSystemTask("[%0t] count = %0d", arguments, context),
              "[40ns] count = 7");
}

TEST(Format, TimeWithNoArgumentIsShownRatherThanInventingOne) {
    const FormatContext context{.time = 1234, .timeScale = TimeScale(-9), .scopeName = "/tb"};
    EXPECT_EQ(enzan::runtime::formatSystemTask("at %t", {}, context), "at %t");
}

TEST(Format, ScopeNameExpandsToTheHierarchicalPath) {
    const FormatContext context{.time = 0, .timeScale = TimeScale(), .scopeName = "/tb/u_dut"};
    EXPECT_EQ(enzan::runtime::formatSystemTask("in %m", {}, context), "in /tb/u_dut");
}

TEST(Format, StringConversionRendersAscii) {
    // A string literal is packed most significant byte first, and zero-extension on the
    // left must not put invisible control characters in the transcript.
    const BitVector text = literal("40'h48_65_6C_6C_6F");
    EXPECT_EQ(render("%s", {{.value = text.ref(), .isSigned = false}}), "Hello");

    const BitVector padded = literal("64'h0000_0000_4F_4B");
    EXPECT_EQ(render("%s", {{.value = padded.ref(), .isSigned = false}}), "OK");
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
    const std::vector<FormatArgument> arguments{{.value = first.ref(), .isSigned = false},
                                                {.value = second.ref(), .isSigned = false}};

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

TEST(Format, AFieldWidthWidensButNeverTruncates) {
    // Checked against Icarus Verilog. A radix whose digits cover fixed bits shows them all and
    // a width only adds to that; a decimal is minimal and then widened.
    const BitVector word = BitVector::fromUInt64(0x1a, 32);
    const BitVector five = BitVector::fromInt64(5, 32);
    const BitVector minusFive = BitVector::fromInt64(-5, 32);
    const auto one = [](const BitVector& value, bool isSigned) {
        return std::vector<FormatArgument>{{.value = value.ref(), .isSigned = isSigned}};
    };

    EXPECT_EQ(render("[%12h]", one(word, false)), "[    0000001a]");
    EXPECT_EQ(render("[%3h]", one(word, false)), "[0000001a]");
    EXPECT_EQ(render("[%016x]", one(word, false)), "[000000000000001a]");
    EXPECT_EQ(render("[%5d]", one(five, true)), "[    5]");
    EXPECT_EQ(render("[%05d]", one(five, true)), "[00005]");
    EXPECT_EQ(render("[%05d]", one(minusFive, true)), "[-0005]");
    EXPECT_EQ(render("[%-4d]", one(five, true)), "[5   ]");
    EXPECT_EQ(render("[%02d]", one(five, true)), "[05]");
}

TEST(Format, LeftJustifyingWithoutAFieldWidthUsesTheValuesOwn) {
    // Checked against Icarus Verilog: `%-d` is as wide as `%d`, with the padding on the right.
    const BitVector ten = BitVector::fromUInt64(10, 32);
    const BitVector minusThree = BitVector::fromInt64(-3, 8);

    EXPECT_EQ(render("[%-d]", {{.value = ten.ref(), .isSigned = false}}), "[10        ]");
    EXPECT_EQ(render("[%-d]", {{.value = minusThree.ref(), .isSigned = true}}), "[-3  ]");
    EXPECT_EQ(render("[%-h]", {{.value = ten.ref(), .isSigned = false}}), "[0000000a]");
}

} // namespace
