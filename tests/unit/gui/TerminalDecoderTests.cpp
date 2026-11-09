// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading the codes a program colours its output with for a terminal: the sixteen colours and
// emphasis kept, a colour of the 256 or by its channels taken to the nearest of the sixteen, every
// other escape sequence dropped, and a code split between two reads finished by the second.

#include "gui/TerminalDecoder.h"

#include <gtest/gtest.h>

#include <QString>

#include <string>
#include <vector>

namespace {

using enzan::gui::TerminalDecoder;
using enzan::gui::TerminalRun;
using enzan::gui::TerminalStyle;

constexpr int NoColor = TerminalStyle::NoColor;

/// The runs as `text|colour` each, for readable expectations.
std::vector<std::string> described(const std::vector<TerminalRun>& runs) {
    std::vector<std::string> out;
    for (const TerminalRun& run : runs) {
        std::string one = run.text.toStdString() + '|' + std::to_string(run.style.color);
        if (run.style.bold) one += " bold";
        if (run.style.italic) one += " italic";
        if (run.style.underline) one += " underline";
        out.push_back(std::move(one));
    }
    return out;
}

std::vector<std::string> decoded(const char* text) {
    TerminalDecoder decoder;
    return described(decoder.decode(QString::fromUtf8(text)));
}

/// The colour of the text after `code`, a colour code's parameters.
int colorAfter(const std::string& code) {
    TerminalDecoder decoder;
    const std::vector<TerminalRun> runs =
        decoder.decode(QString::fromStdString("\x1b[" + code + "mx"));
    return runs.size() == 1 ? runs[0].style.color : -2;
}

TEST(TerminalDecoderTests, PlainTextIsOneRunInNoColour) {
    EXPECT_EQ(decoded("passed\n"), (std::vector<std::string>{"passed\n|-1"}));
}

TEST(TerminalDecoderTests, TheSixteenColoursAndEmphasisAreKept) {
    EXPECT_EQ(decoded("\x1b[32mpassed\x1b[0m, \x1b[1;91mfailed\x1b[22m twice\x1b[m."),
              (std::vector<std::string>{"passed|2", ", |-1", "failed|9 bold", " twice|9", ".|-1"}));
    EXPECT_EQ(
        decoded("\x1b[3;4mnote\x1b[23;24m \x1b[4:0m\x1b[21md\x1b[39;24me"),
        (std::vector<std::string>{"note|-1 italic underline", " |-1", "d|-1 underline", "e|-1"}));
    EXPECT_EQ(colorAfter("30"), 0);
    EXPECT_EQ(colorAfter("37"), 7);
    EXPECT_EQ(colorAfter("90"), 8);
    EXPECT_EQ(colorAfter("97"), 15);
    EXPECT_EQ(colorAfter("31;39"), NoColor);
}

TEST(TerminalDecoderTests, AColourOfTheExtendedSetsIsTheNearestOfTheSixteen) {
    // The sixteen of the 256 are themselves.
    EXPECT_EQ(colorAfter("38;5;1"), 1);
    EXPECT_EQ(colorAfter("38;5;12"), 12);
    // The cube: 196 is full red, 160 a darker one, 21 full blue, 226 full yellow.
    EXPECT_EQ(colorAfter("38;5;196"), 9);
    EXPECT_EQ(colorAfter("38;5;160"), 1);
    EXPECT_EQ(colorAfter("38;5;21"), 12);
    EXPECT_EQ(colorAfter("38:5:226"), 11);
    // The greys, from black to white.
    EXPECT_EQ(colorAfter("38;5;232"), 0);
    EXPECT_EQ(colorAfter("38;5;240"), 8);
    EXPECT_EQ(colorAfter("38;5;250"), 7);
    EXPECT_EQ(colorAfter("38;5;255"), 15);
    // By channels, with a colour space or without.
    EXPECT_EQ(colorAfter("38;2;255;128;0"), 11);
    EXPECT_EQ(colorAfter("38;2;0;160;0"), 2);
    EXPECT_EQ(colorAfter("38:2::0:0:255"), 12);
    EXPECT_EQ(colorAfter("38:2:0:0:0"), 0);
    // A code naming no colour changes nothing.
    EXPECT_EQ(colorAfter("31;38;5;300"), 1);
}

TEST(TerminalDecoderTests, BackgroundsAreDroppedWithTheirArguments) {
    // `48;2;1;2;3` takes the three after it, which are no codes of their own.
    EXPECT_EQ(colorAfter("41"), NoColor);
    EXPECT_EQ(colorAfter("48;5;1"), NoColor);
    EXPECT_EQ(colorAfter("48;2;1;2;3;31"), 1);
    EXPECT_EQ(colorAfter("7;33"), 3) << "reverse video is not shown";
}

TEST(TerminalDecoderTests, EveryOtherSequenceIsDropped) {
    EXPECT_EQ(decoded("a\x1b[2Kb\x1b[1;1Hc\x1b[?25ld\x1b"
                      "7e"),
              (std::vector<std::string>{"abcde|-1"}));
    // A title and a hyperlink, ended by a bell and by a string terminator.
    EXPECT_EQ(decoded("\x1b]0;title\x07"
                      "x \x1b]8;;https://example.org\x1b\\link\x1b]8;;\x1b\\"),
              (std::vector<std::string>{"x link|-1"}));
    // A private sequence ending in `m` is no colour code.
    EXPECT_EQ(decoded("\x1b[>4;1mx"), (std::vector<std::string>{"x|-1"}));
}

TEST(TerminalDecoderTests, AStrayEscapeCannotJoinTwoLines) {
    EXPECT_EQ(decoded("one\x1b\ntwo\x1b[3\nthree\n"),
              (std::vector<std::string>{"one\ntwo\nthree\n|-1"}));
}

TEST(TerminalDecoderTests, ACodeSplitBetweenReadsIsFinishedByTheNext) {
    TerminalDecoder decoder;
    EXPECT_EQ(described(decoder.decode(QStringLiteral("a\x1b"))),
              (std::vector<std::string>{"a|-1"}));
    EXPECT_TRUE(decoder.decode(QStringLiteral("[3")).empty());
    EXPECT_EQ(described(decoder.decode(QStringLiteral("2mgreen\n"))),
              (std::vector<std::string>{"green\n|2"}));
    // The colour holds into the next line until reset.
    EXPECT_EQ(described(decoder.decode(QStringLiteral("still\x1b[0m\n"))),
              (std::vector<std::string>{"still|2", "\n|-1"}));

    decoder.decode(QStringLiteral("\x1b[31m"));
    decoder.reset();
    EXPECT_EQ(described(decoder.decode(QStringLiteral("x"))), (std::vector<std::string>{"x|-1"}));
}

} // namespace
