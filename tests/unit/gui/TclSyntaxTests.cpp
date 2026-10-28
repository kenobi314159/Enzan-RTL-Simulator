// SPDX-License-Identifier: GPL-3.0-or-later
//
// Splitting a file with Tcl's syntax into commands without evaluating any of it, and writing a
// word so that it reads back. Signal lists and the configuration file both stand on these, so
// what they promise is pinned here rather than through either.

#include "gui/TclSyntax.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

namespace {

using enzan::gui::quoteTclWord;
using enzan::gui::splitTclCommands;
using enzan::gui::TclCommand;
using enzan::gui::TclOrigin;
using enzan::gui::TclSyntaxError;

struct Split {
    std::vector<TclCommand> commands;
    std::optional<TclSyntaxError> malformed;
    bool read = false;
};

Split split(const QByteArray& text) {
    Split result;
    const TclOrigin origin{.base = text.constData(), .baseLine = 1, .exact = true};
    result.read = splitTclCommands(
        text.constData(), static_cast<int>(text.size()), origin,
        [&result](const TclCommand& command) {
            result.commands.push_back(command);
            return true;
        },
        result.malformed);
    return result;
}

TEST(TclSyntax, CommandsAreSplitWithTheirLines) {
    const QByteArray text("# a comment\nfirst a b\n\nsecond {c d} ;# after it\nthird\n");
    const Split result = split(text);

    ASSERT_TRUE(result.read);
    ASSERT_EQ(result.commands.size(), 3U);
    EXPECT_EQ(result.commands[0].line, 2);
    EXPECT_EQ(result.commands[1].line, 4);
    EXPECT_EQ(result.commands[2].line, 5);

    ASSERT_EQ(result.commands[1].words.size(), 2U);
    EXPECT_EQ(result.commands[1].words[1].text, QByteArray("c d"));
}

TEST(TclSyntax, NothingIsSubstituted) {
    const Split result = split("set x $y [list z]\n");

    ASSERT_TRUE(result.read);
    ASSERT_EQ(result.commands.size(), 1U);
    ASSERT_EQ(result.commands[0].words.size(), 4U);
    EXPECT_EQ(result.commands[0].words[2].text, QByteArray("$y"));
    EXPECT_EQ(result.commands[0].words[3].text, QByteArray("[list z]"));
}

TEST(TclSyntax, AWordKnowsWhereItStandsInTheText) {
    // A writer replaces a word where it stands, so the span covers its braces or quotes.
    const QByteArray text("theme {my theme.tcl}\n");
    const Split result = split(text);

    ASSERT_EQ(result.commands.size(), 1U);
    const auto& word = result.commands[0].words[1];
    EXPECT_EQ(QByteArray(word.start, word.size), QByteArray("{my theme.tcl}"));
    EXPECT_EQ(word.text, QByteArray("my theme.tcl"));
}

TEST(TclSyntax, TextThatIsNoCommandIsReportedAtItsLine) {
    const Split result = split("fine\nbroken {never closed\n");

    EXPECT_FALSE(result.read);
    ASSERT_TRUE(result.malformed.has_value());
    const TclSyntaxError malformed = result.malformed.value_or(TclSyntaxError{});
    EXPECT_EQ(malformed.line, 2);
    EXPECT_FALSE(malformed.text.isEmpty());
}

TEST(TclSyntax, AStoppedSplitIsNotAMalformedOne) {
    const QByteArray text("one\ntwo\nthree\n");
    const TclOrigin origin{.base = text.constData(), .baseLine = 1, .exact = true};
    std::optional<TclSyntaxError> malformed;
    int seen = 0;

    const bool read = splitTclCommands(
        text.constData(), static_cast<int>(text.size()), origin,
        [&seen](const TclCommand&) { return ++seen < 2; }, malformed);

    EXPECT_FALSE(read);
    EXPECT_FALSE(malformed.has_value());
    EXPECT_EQ(seen, 2);
}

TEST(TclSyntax, AWordIsQuotedOnlyWhenItMustBe) {
    EXPECT_EQ(quoteTclWord(QStringLiteral("/tb/gen[0].q")), QStringLiteral("/tb/gen[0].q"));
    EXPECT_EQ(quoteTclWord(QStringLiteral("two words")), QStringLiteral("{two words}"));
    EXPECT_EQ(quoteTclWord(QString()), QStringLiteral("{}"));
}

} // namespace
