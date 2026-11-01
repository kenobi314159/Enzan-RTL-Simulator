// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for reading a value typed as the wave window writes one.
//
// The rule they pin is that whatever the value column shows can be typed back and means what it
// showed: `ff` in a hexadecimal row is 255, and what the column writes for any value reads back as
// that value. The other half is what the rule refuses. A value too wide for the row is refused
// rather than cut down to fit, because a value cut down is another value, and a reader looking for
// one thing would be shown something else without being told.

#include "gui/ValueText.h"

#include "eir/BitVector.h"

#include <gtest/gtest.h>

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using enzan::eir::ArrayDimension;
using enzan::eir::BitVector;
using enzan::eir::Logic;
using enzan::eir::Radix;
using enzan::gui::PartialDigit;
using enzan::gui::readArray;
using enzan::gui::readNumber;
using enzan::gui::splitList;
using enzan::gui::ValueReading;

std::string binary(const BitVector& value) {
    return enzan::eir::toString(value.ref(), Radix::Binary);
}

/// A reading in binary, or why there is none.
std::string shown(const ValueReading& reading) {
    if (!reading.value.has_value()) return "refused: " + reading.problem.toStdString();
    return binary(*reading.value);
}

/// What `text` reads as in a row of `width` bits shown in `radix`, in binary, or why it does not.
std::string read(const QString& text, std::uint32_t width, Radix radix) {
    const ValueReading reading = readNumber(text, width, radix);
    if (!reading.value.has_value()) return "refused: " + reading.problem.toStdString();
    EXPECT_TRUE(reading.problem.isEmpty());
    EXPECT_EQ(reading.value->width(), width) << "a reading is always the row's width";
    return binary(*reading.value);
}

bool refused(const QString& text, std::uint32_t width, Radix radix) {
    const ValueReading reading = readNumber(text, width, radix);
    return !reading.value.has_value() && !reading.problem.isEmpty();
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

TEST(ValueText, BareDigitsAreReadInTheRowsRadix) {
    EXPECT_EQ(read(QStringLiteral("ff"), 8, Radix::Hexadecimal), "11111111");
    EXPECT_EQ(read(QStringLiteral("5"), 8, Radix::Hexadecimal), "00000101");
    EXPECT_EQ(read(QStringLiteral("1010"), 4, Radix::Binary), "1010");
    EXPECT_EQ(read(QStringLiteral("17"), 6, Radix::Octal), "001111");
    EXPECT_EQ(read(QStringLiteral("200"), 8, Radix::Decimal), "11001000");

    // The same digits are a different value in another row, which is the point: `10` is what a
    // hexadecimal row shows for sixteen.
    EXPECT_EQ(read(QStringLiteral("10"), 8, Radix::Hexadecimal), "00010000");
    EXPECT_EQ(read(QStringLiteral("10"), 8, Radix::Decimal), "00001010");
}

TEST(ValueText, SpaceAroundAndUnderscoresWithinAreIgnored) {
    EXPECT_EQ(read(QStringLiteral("  1010_0101 "), 8, Radix::Binary), "10100101");
    EXPECT_EQ(read(QStringLiteral("1_000"), 16, Radix::Decimal), "0000001111101000");
}

TEST(ValueText, AnUnknownDigitIsUnknownInEveryBitItCovers) {
    EXPECT_EQ(read(QStringLiteral("1x"), 8, Radix::Hexadecimal), "0001xxxx");
    EXPECT_EQ(read(QStringLiteral("10z1"), 4, Radix::Binary), "10z1");
    EXPECT_EQ(read(QStringLiteral("x5"), 8, Radix::Hexadecimal), "xxxx0101");

    // Read as `8'hx` is, so one x is the whole row unknown, as IEEE 1800 pads a literal.
    EXPECT_EQ(read(QStringLiteral("x"), 8, Radix::Hexadecimal), "xxxxxxxx");
    EXPECT_EQ(read(QStringLiteral("z"), 4, Radix::Binary), "zzzz");

    // Decimal cannot place an unknown in a digit, so its x and z are the whole value.
    EXPECT_EQ(read(QStringLiteral("x"), 8, Radix::Decimal), "xxxxxxxx");
    EXPECT_EQ(read(QStringLiteral("z"), 4, Radix::Decimal), "zzzz");
}

/// The partial digits of `text` read in a row of `width` bits shown in `radix`.
std::vector<PartialDigit> partialOf(const QString& text, std::uint32_t width, Radix radix) {
    const ValueReading reading = readNumber(text, width, radix);
    EXPECT_TRUE(reading.value.has_value()) << reading.problem.toStdString();
    return reading.partial;
}

TEST(ValueText, ADigitInCapitalsIsOnlyPartlyUnknown) {
    // What the column writes for a digit with some of its bits unknown and not all, or with some
    // high impedance and none unknown: not one value but any of several, kept beside the value.
    EXPECT_EQ(partialOf(QStringLiteral("1X"), 8, Radix::Hexadecimal),
              std::vector({PartialDigit{.highBit = 3, .lowBit = 0, .kind = Logic::Unknown}}));
    EXPECT_EQ(partialOf(QStringLiteral("Z0"), 8, Radix::Hexadecimal),
              std::vector({PartialDigit{.highBit = 7, .lowBit = 4, .kind = Logic::HighZ}}));
    EXPECT_EQ(partialOf(QStringLiteral("X7Z"), 9, Radix::Octal),
              std::vector({PartialDigit{.highBit = 8, .lowBit = 6, .kind = Logic::Unknown},
                           PartialDigit{.highBit = 2, .lowBit = 0, .kind = Logic::HighZ}}));
    EXPECT_EQ(read(QStringLiteral("1X"), 8, Radix::Hexadecimal), "0001xxxx")
        << "its bits in the value are a lowercase digit's";

    // The top digit of a width its bits do not divide covers what there is of it.
    EXPECT_EQ(partialOf(QStringLiteral("X0"), 7, Radix::Hexadecimal),
              std::vector({PartialDigit{.highBit = 6, .lowBit = 4, .kind = Logic::Unknown}}));

    // Decimal writes one for the whole value.
    EXPECT_EQ(partialOf(QStringLiteral("X"), 8, Radix::Decimal),
              std::vector({PartialDigit{.highBit = 7, .lowBit = 0, .kind = Logic::Unknown}}));
    EXPECT_EQ(partialOf(QStringLiteral("Z"), 8, Radix::Decimal),
              std::vector({PartialDigit{.highBit = 7, .lowBit = 0, .kind = Logic::HighZ}}));
}

TEST(ValueText, WhereADigitCannotBePartlyAnythingItsCapitalIsItsLowercase) {
    // A digit of one bit, as every binary digit is, is unknown or not.
    EXPECT_TRUE(partialOf(QStringLiteral("10X1"), 4, Radix::Binary).empty());
    EXPECT_EQ(read(QStringLiteral("10X1"), 4, Radix::Binary), "10x1");
    EXPECT_TRUE(partialOf(QStringLiteral("X5"), 5, Radix::Hexadecimal).empty())
        << "the top digit of five bits covers one";
    EXPECT_TRUE(partialOf(QStringLiteral("X"), 1, Radix::Decimal).empty());

    // A literal is read as SystemVerilog reads one, where case does not matter.
    EXPECT_TRUE(partialOf(QStringLiteral("8'hX5"), 8, Radix::Hexadecimal).empty());
    EXPECT_EQ(read(QStringLiteral("8'hX5"), 8, Radix::Hexadecimal), "xxxx0101");
    EXPECT_TRUE(partialOf(QStringLiteral("'hZ5"), 8, Radix::Hexadecimal).empty());
    EXPECT_TRUE(partialOf(QStringLiteral("'dX"), 8, Radix::Hexadecimal).empty());
}

TEST(ValueText, AListMovesItsElementsPartialDigitsToWhereTheirBitsGo) {
    // `logic [1:0][7:0]`, the right element the lowest.
    const std::vector<ArrayDimension> bytes = {{.left = 1, .right = 0}, {.left = 7, .right = 0}};
    const ValueReading listed =
        readArray(QStringLiteral("{X1}{2Z}"), 16, bytes, Radix::Hexadecimal);
    ASSERT_TRUE(listed.value.has_value()) << listed.problem.toStdString();
    EXPECT_EQ(listed.partial,
              std::vector({PartialDigit{.highBit = 15, .lowBit = 12, .kind = Logic::Unknown},
                           PartialDigit{.highBit = 3, .lowBit = 0, .kind = Logic::HighZ}}));
}

TEST(ValueText, ALiteralMeansWhatItSaysInAnyRadix) {
    EXPECT_EQ(read(QStringLiteral("8'hff"), 8, Radix::Binary), "11111111");
    EXPECT_EQ(read(QStringLiteral("8'd200"), 8, Radix::Hexadecimal), "11001000");
    EXPECT_EQ(read(QStringLiteral("4'b10xz"), 4, Radix::Decimal), "10xz");

    // A narrower literal is extended with zeros, as an assignment extends it.
    EXPECT_EQ(read(QStringLiteral("4'h5"), 8, Radix::Hexadecimal), "00000101");
    EXPECT_EQ(read(QStringLiteral("4'hx"), 8, Radix::Hexadecimal), "0000xxxx");

    // A fill literal fills the row, and an unsized one takes the row's width.
    EXPECT_EQ(read(QStringLiteral("'1"), 8, Radix::Hexadecimal), "11111111");
    EXPECT_EQ(read(QStringLiteral("'x"), 4, Radix::Decimal), "xxxx");
    EXPECT_EQ(read(QStringLiteral("'z"), 3, Radix::Binary), "zzz");
    EXPECT_EQ(read(QStringLiteral("'hx"), 8, Radix::Binary), "xxxxxxxx");
    EXPECT_EQ(read(QStringLiteral("'h5"), 8, Radix::Binary), "00000101");
    EXPECT_EQ(read(QStringLiteral("'b1?"), 2, Radix::Binary), "1z")
        << "a literal takes ? for z, as SystemVerilog does";
}

TEST(ValueText, AValueTooWideForTheRowIsRefusedRatherThanCutDown) {
    EXPECT_TRUE(refused(QStringLiteral("1ff"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("256"), 8, Radix::Decimal));
    EXPECT_TRUE(refused(QStringLiteral("101"), 2, Radix::Binary));
    EXPECT_TRUE(refused(QStringLiteral("16'h1ff"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("'h1ff"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("'d300"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("xx"), 4, Radix::Hexadecimal)) << "a digit too many";
    EXPECT_TRUE(refused(QStringLiteral("8'hxx"), 4, Radix::Hexadecimal));

    // A top digit past a width its bits do not divide: a one there is too wide.
    EXPECT_TRUE(refused(QStringLiteral("ff"), 7, Radix::Hexadecimal));

    EXPECT_EQ(readNumber(QStringLiteral("1ff"), 8, Radix::Hexadecimal).problem,
              QStringLiteral("'1ff' does not fit in 8 bits"));
    EXPECT_EQ(readNumber(QStringLiteral("'h1ff"), 8, Radix::Hexadecimal).problem,
              QStringLiteral("''h1ff' does not fit in 8 bits"))
        << "the literal is named whole";
    EXPECT_EQ(readNumber(QStringLiteral("2"), 1, Radix::Decimal).problem,
              QStringLiteral("'2' does not fit in 1 bit"));
}

TEST(ValueText, ALiteralStatingAHugeWidthIsReadWithoutMakingThatManyBits) {
    // A typo can state billions of bits, which made the field allocate a gigabyte and then walk
    // every bit of it. Past the row and past its digits a literal is only padding, so it reads
    // as it would at a width just past both.
    EXPECT_EQ(read(QStringLiteral("4000000000'h0"), 8, Radix::Hexadecimal), "00000000");
    EXPECT_EQ(read(QStringLiteral("4000000000'sh5_a"), 8, Radix::Hexadecimal), "01011010");
    EXPECT_EQ(read(QStringLiteral("4294967295'd200"), 8, Radix::Hexadecimal), "11001000");
    EXPECT_TRUE(refused(QStringLiteral("4000000000'h1ff"), 8, Radix::Hexadecimal));

    // An x or a z on the left pads every bit it states, which is past the row.
    EXPECT_TRUE(refused(QStringLiteral("4000000000'hx"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("4000000000'bz1"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("4000000000'dx"), 8, Radix::Hexadecimal));
}

TEST(ValueText, LeadingZerosAndAnUnknownTopDigitStillFit) {
    EXPECT_EQ(read(QStringLiteral("00ff"), 8, Radix::Hexadecimal), "11111111");
    EXPECT_EQ(read(QStringLiteral("000"), 1, Radix::Hexadecimal), "0");
    EXPECT_EQ(read(QStringLiteral("16'h00ff"), 8, Radix::Hexadecimal), "11111111");
    EXPECT_EQ(read(QStringLiteral("'h0ff"), 8, Radix::Hexadecimal), "11111111");

    // The column writes a seven-bit value in two hexadecimal digits, the top one covering three
    // bits, and an x there is as much of an x as the row has.
    EXPECT_EQ(read(QStringLiteral("xx"), 7, Radix::Hexadecimal), "xxxxxxx");
    EXPECT_EQ(read(QStringLiteral("x"), 1, Radix::Hexadecimal), "x");
    EXPECT_EQ(read(QStringLiteral("7f"), 7, Radix::Hexadecimal), "1111111");
    EXPECT_EQ(read(QStringLiteral("'hxx"), 7, Radix::Hexadecimal), "xxxxxxx");
}

TEST(ValueText, WhatIsNoNumberOfTheRadixIsRefused) {
    EXPECT_TRUE(refused(QStringLiteral("fg"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("12"), 8, Radix::Binary));
    EXPECT_TRUE(refused(QStringLiteral("8"), 8, Radix::Octal));
    EXPECT_TRUE(refused(QStringLiteral("1x"), 8, Radix::Decimal)) << "x among decimal digits";
    EXPECT_TRUE(refused(QStringLiteral("-1"), 8, Radix::Decimal));
    EXPECT_TRUE(refused(QStringLiteral("s5"), 8, Radix::Hexadecimal))
        << "a literal's signed marker is no digit";
    EXPECT_TRUE(refused(QStringLiteral("1 2"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("_"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("?"), 8, Radix::Hexadecimal))
        << "there are no wildcards, and the column never writes a ?";
    EXPECT_TRUE(refused(QStringLiteral("8'hfg"), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("'q1"), 8, Radix::Hexadecimal));

    EXPECT_EQ(readNumber(QStringLiteral("fg"), 8, Radix::Hexadecimal).problem,
              QStringLiteral("'fg' is not a hexadecimal number"));
    EXPECT_EQ(readNumber(QStringLiteral("8'hfg"), 8, Radix::Binary).problem,
              QStringLiteral("'8'hfg' is not a value Enzan understands"));
}

TEST(ValueText, NothingTypedIsNoValue) {
    EXPECT_TRUE(refused(QString(), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QStringLiteral("   "), 8, Radix::Hexadecimal));
    EXPECT_TRUE(refused(QString(), 8, Radix::Ascii));
    EXPECT_TRUE(refused(QStringLiteral("1"), 0, Radix::Hexadecimal)) << "a row of no bits";
}

// ---------------------------------------------------------------------------
// Characters
// ---------------------------------------------------------------------------

TEST(ValueText, AnAsciiRowReadsItsCharactersPaddedWithNulsOnTheLeft) {
    EXPECT_EQ(read(QStringLiteral("AB"), 32, Radix::Ascii),
              binary(BitVector::fromUInt64(0x4142, 32)));
    EXPECT_EQ(read(QStringLiteral(" A"), 16, Radix::Ascii),
              binary(BitVector::fromUInt64(0x2041, 16)))
        << "a space is a character, so it is kept";

    // The escapes the column writes for what it cannot print.
    EXPECT_EQ(read(QStringLiteral("\\n\\t"), 16, Radix::Ascii),
              binary(BitVector::fromUInt64(0x0a09, 16)));
    EXPECT_EQ(read(QStringLiteral("A\\0B"), 24, Radix::Ascii),
              binary(BitVector::fromUInt64(0x410042, 24)));
    EXPECT_EQ(read(QStringLiteral("\\\\\\x7f"), 16, Radix::Ascii),
              binary(BitVector::fromUInt64(0x5c7f, 16)));
}

TEST(ValueText, InAnAsciiRowALiteralIsALiteralAndAnythingElseIsText) {
    EXPECT_EQ(read(QStringLiteral("8'h41"), 8, Radix::Ascii), "01000001");
    EXPECT_EQ(read(QStringLiteral("'x"), 8, Radix::Ascii), "xxxxxxxx");
    EXPECT_EQ(read(QStringLiteral("it's"), 32, Radix::Ascii),
              binary(BitVector::fromUInt64(0x69742773, 32)));
}

TEST(ValueText, CharactersThatCannotBeABytesValueAreRefused) {
    EXPECT_TRUE(refused(QStringLiteral("ABC"), 16, Radix::Ascii)) << "a character too many";
    EXPECT_TRUE(refused(QStringLiteral("AB"), 12, Radix::Ascii))
        << "A's top nibble is past 12 bits";
    EXPECT_TRUE(refused(QStringLiteral("\\q"), 8, Radix::Ascii));
    EXPECT_TRUE(refused(QStringLiteral("\\x4"), 8, Radix::Ascii));
    EXPECT_TRUE(refused(QStringLiteral("é"), 8, Radix::Ascii));

    // U+FFFD is what the column writes for a byte with an unknown bit, which no text can name.
    const ValueReading unknown = readNumber(QStringLiteral("A�"), 16, Radix::Ascii);
    EXPECT_FALSE(unknown.value.has_value());
    EXPECT_TRUE(unknown.problem.contains(QStringLiteral("unknown bit")));
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------

TEST(ValueText, AListSplitsIntoTheWordsInItsOuterBraces) {
    EXPECT_EQ(splitList(QStringLiteral("{a}{b{c}}")),
              QStringList({QStringLiteral("a"), QStringLiteral("b{c}")}));
    EXPECT_EQ(splitList(QStringLiteral(" {1} {2} ")),
              QStringList({QStringLiteral("1"), QStringLiteral("2")}));
    EXPECT_EQ(splitList(QStringLiteral("{}")), QStringList({QString()}));

    EXPECT_FALSE(splitList(QStringLiteral("{a}{b")).has_value());
    EXPECT_FALSE(splitList(QStringLiteral("{a}b")).has_value());
    EXPECT_FALSE(splitList(QStringLiteral("a")).has_value());
    EXPECT_FALSE(splitList(QString()).has_value());
}

TEST(ValueText, AnArrayReadsAsTheListOfItsElements) {
    // `logic [1:0][3:0]`, which the column writes `{5}{a}` for 8'h5a.
    const std::vector<ArrayDimension> nibbles = {{.left = 1, .right = 0}, {.left = 3, .right = 0}};
    const ValueReading listed = readArray(QStringLiteral("{5}{a}"), 8, nibbles, Radix::Hexadecimal);
    EXPECT_EQ(shown(listed), "01011010") << "the left element is the highest";

    // A number of the whole width is read too.
    const ValueReading whole = readArray(QStringLiteral("5a"), 8, nibbles, Radix::Hexadecimal);
    EXPECT_EQ(shown(whole), "01011010");

    // Three dimensions nest.
    const std::vector<ArrayDimension> cube = {
        {.left = 1, .right = 0}, {.left = 1, .right = 0}, {.left = 1, .right = 0}};
    const ValueReading nested =
        readArray(QStringLiteral("{{1}{2}}{{3}{0}}"), 8, cube, Radix::Hexadecimal);
    EXPECT_EQ(shown(nested), "01101100");
}

TEST(ValueText, AListOfTheWrongShapeIsRefused) {
    const std::vector<ArrayDimension> nibbles = {{.left = 1, .right = 0}, {.left = 3, .right = 0}};

    const ValueReading tooFew = readArray(QStringLiteral("{5}"), 8, nibbles, Radix::Hexadecimal);
    EXPECT_FALSE(tooFew.value.has_value());
    EXPECT_EQ(tooFew.problem, QStringLiteral("'{5}' lists 1, and the row has 2 elements"));

    EXPECT_FALSE(
        readArray(QStringLiteral("{5}{a"), 8, nibbles, Radix::Hexadecimal).value.has_value());
    EXPECT_FALSE(
        readArray(QStringLiteral("{5}{1f}"), 8, nibbles, Radix::Hexadecimal).value.has_value())
        << "an element too wide for its four bits";
}

// ---------------------------------------------------------------------------
// What the column writes reads back
// ---------------------------------------------------------------------------

TEST(ValueText, WhatTheColumnWritesReadsBackAsTheSameValue) {
    // Values whose unknowns cover whole digits, which is when the column writes them exactly: a
    // partly unknown digit is written `X`, which says less than the bits do.
    for (const std::uint32_t width : {1U, 3U, 4U, 7U, 8U, 12U, 33U, 64U, 72U}) {
        std::vector<BitVector> values = {
            BitVector::filled(width, Logic::Zero),
            BitVector::filled(width, Logic::One),
            BitVector::filled(width, Logic::Unknown),
            BitVector::filled(width, Logic::HighZ),
            BitVector::fromUInt64(0x5a5a'1234'8001'7f3cULL, width),
            BitVector::fromUInt64(1, width),
        };
        if (width > 64) {
            BitVector top = BitVector::fromUInt64(0x0123'4567'89ab'cdefULL, width);
            top.set(width - 1, Logic::One);
            values.push_back(top);
        }

        for (const Radix radix :
             {Radix::Binary, Radix::Octal, Radix::Decimal, Radix::Hexadecimal}) {
            for (const BitVector& value : values) {
                const QString written = QString::fromStdString(toString(value.ref(), radix));
                const ValueReading reading = readNumber(written, width, radix);
                ASSERT_TRUE(reading.value.has_value())
                    << written.toStdString() << " at " << width
                    << " bits: " << reading.problem.toStdString();
                EXPECT_EQ(reading.value, std::optional(value))
                    << written.toStdString() << " at " << width;
                EXPECT_TRUE(reading.partial.empty()) << written.toStdString() << " at " << width;
            }
        }
    }

    // Characters, unprintable ones and an inner NUL among them.
    for (const std::uint64_t bytes :
         {0x0000'0041'4243'4445ULL, 0x0041'7f00'0a5c'0941ULL, 0x0000'0000'0000'2020ULL}) {
        const BitVector value = BitVector::fromUInt64(bytes, 64);
        const QString written = QString::fromStdString(toString(value.ref(), Radix::Ascii));
        const ValueReading reading = readNumber(written, 64, Radix::Ascii);
        ASSERT_TRUE(reading.value.has_value()) << written.toStdString();
        EXPECT_EQ(reading.value, std::optional(value)) << written.toStdString();
    }
}

/// True when `value` is one of the values `reading` reads: its bits where no digit is partial, and
/// under each partial digit some bits unknown and not all, or, none unknown, some high impedance
/// and not all.
bool describes(const ValueReading& reading, const BitVector& value) {
    std::vector<bool> partial(value.width(), false);
    for (const PartialDigit& digit : reading.partial) {
        std::uint32_t unknown = 0;
        std::uint32_t highZ = 0;
        for (std::uint32_t bit = digit.lowBit; bit <= digit.highBit; ++bit) {
            partial[bit] = true;
            unknown += value.get(bit) == Logic::Unknown ? 1U : 0U;
            highZ += value.get(bit) == Logic::HighZ ? 1U : 0U;
        }
        const std::uint32_t count = digit.highBit - digit.lowBit + 1;
        const bool shows = digit.kind == Logic::Unknown
                               ? unknown > 0 && unknown < count
                               : unknown == 0 && highZ > 0 && highZ < count;
        if (!shows) return false;
    }
    for (std::uint32_t bit = 0; bit < value.width(); ++bit) {
        if (!partial[bit] && value.get(bit) != reading.value->get(bit)) return false;
    }
    return true;
}

TEST(ValueText, WhatTheColumnWritesForAPartlyUnknownValueReadsBackAsDescribingIt) {
    for (const std::uint32_t width : {4U, 7U, 8U, 12U, 33U, 72U}) {
        const BitVector known = BitVector::fromUInt64(0x5a5a'1234'8001'7f3cULL, width);
        std::vector<BitVector> values(3, known);
        values[0].set(1, Logic::Unknown);
        values[1].set(1, Logic::HighZ);
        values[2].set(1, Logic::Unknown);
        values[2].set(2, Logic::HighZ);
        values[2].set(width - 1, Logic::HighZ);

        for (const Radix radix : {Radix::Octal, Radix::Decimal, Radix::Hexadecimal}) {
            for (const BitVector& value : values) {
                const QString written = QString::fromStdString(toString(value.ref(), radix));
                const ValueReading reading = readNumber(written, width, radix);
                ASSERT_TRUE(reading.value.has_value())
                    << written.toStdString() << ": " << reading.problem.toStdString();
                EXPECT_FALSE(reading.partial.empty()) << written.toStdString();
                EXPECT_TRUE(describes(reading, value)) << written.toStdString() << " at " << width;

                // A digit wholly unknown is written in lowercase, and is not the one read.
                BitVector whole = value;
                whole.set(0, Logic::Unknown);
                whole.set(1, Logic::Unknown);
                whole.set(2, Logic::Unknown);
                whole.set(3, Logic::Unknown);
                EXPECT_FALSE(describes(reading, whole) && radix != Radix::Decimal)
                    << written.toStdString() << " at " << width;
            }
        }
    }
}

} // namespace
