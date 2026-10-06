// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading and writing signal list files (DESIGN_GUIDE.md section 10.8): Tcl's syntax, two
// commands, and nothing evaluated. What a list turns into in a window is SignalListLoadTests.

#include "gui/SignalList.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <string>

namespace {

using enzan::eir::Radix;
using enzan::gui::ParsedSignalList;
using enzan::gui::parseSignalList;
using enzan::gui::quoteSignalListWord;
using enzan::gui::radixFromWord;
using enzan::gui::radixWord;
using enzan::gui::SignalListEntry;

ParsedSignalList parse(const char* text) {
    return parseSignalList(QByteArray(text));
}

/// The first error's text and line, for a readable failure.
std::string firstError(const ParsedSignalList& parsed) {
    if (parsed.errors.isEmpty()) return "no error";
    return std::to_string(parsed.errors.front().line) + ": "
           + parsed.errors.front().text.toStdString();
}

// ---------------------------------------------------------------------------
// The two commands
// ---------------------------------------------------------------------------

TEST(SignalList, ReadsAddWaveWithItsOptions) {
    const ParsedSignalList parsed =
        parse("add wave -radix dec -label {the count} -highlight 2 /tb/count\n");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    ASSERT_EQ(parsed.entries.size(), 1U);

    const SignalListEntry& entry = parsed.entries.front();
    EXPECT_EQ(entry.kind, SignalListEntry::Kind::AddWave);
    EXPECT_EQ(entry.paths, QStringList{QStringLiteral("/tb/count")});
    EXPECT_EQ(entry.radix, Radix::Decimal);
    EXPECT_EQ(entry.label, QStringLiteral("the count"));
    EXPECT_EQ(entry.highlight, 2);
}

TEST(SignalList, AddWaveTakesSeveralPaths) {
    const ParsedSignalList parsed = parse("add wave /tb/clk /tb/rst_n /tb/count\n");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    EXPECT_EQ(parsed.entries.front().paths.size(), 3);
}

TEST(SignalList, GroupsNestAndMayBeEmpty) {
    const ParsedSignalList parsed = parse(R"(
        group -highlight 3 "Outer group" {
            add wave /tb/clk
            group Inner {}
        }
    )");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    ASSERT_EQ(parsed.entries.size(), 1U);

    const SignalListEntry& outer = parsed.entries.front();
    EXPECT_EQ(outer.kind, SignalListEntry::Kind::Group);
    EXPECT_EQ(outer.name, QStringLiteral("Outer group"));
    EXPECT_EQ(outer.highlight, 3);
    ASSERT_EQ(outer.body.size(), 2U);
    EXPECT_EQ(outer.body[1].kind, SignalListEntry::Kind::Group);
    EXPECT_TRUE(outer.body[1].body.empty()) << "an empty group is a group with nothing in it";
}

TEST(SignalList, ContentsIsABodyAndMayBeEmpty) {
    const ParsedSignalList parsed = parse(R"(
        add wave -contents {
            add wave /tb/bus[7]
            add wave -radix bin /tb/bus[3]
        } /tb/bus
        add wave -contents {} /tb/other
    )");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    ASSERT_EQ(parsed.entries.size(), 2U);

    EXPECT_TRUE(parsed.entries[0].contents);
    EXPECT_EQ(parsed.entries[0].body.size(), 2U);
    EXPECT_TRUE(parsed.entries[1].contents) << "told apart from no -contents at all";
    EXPECT_TRUE(parsed.entries[1].body.empty());
}

// ---------------------------------------------------------------------------
// Tcl's syntax, nothing evaluated
// ---------------------------------------------------------------------------

TEST(SignalList, BracketsAndDollarsAreOrdinaryCharacters) {
    // In Tcl the first would call a command named `0`, and the second would read a variable.
    const ParsedSignalList parsed = parse("add wave /tb/gen[0].q /tb/odd$name\n");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    EXPECT_EQ(parsed.entries.front().paths,
              (QStringList{QStringLiteral("/tb/gen[0].q"), QStringLiteral("/tb/odd$name")}));
}

TEST(SignalList, QuotingBackslashesAndContinuationAreTcls) {
    const ParsedSignalList parsed = parse("add wave -label \"two words\" \\\n"
                                          "    /tb/a\\ b\n");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    ASSERT_EQ(parsed.entries.size(), 1U) << "a line ending in a backslash continues";
    EXPECT_EQ(parsed.entries.front().label, QStringLiteral("two words"));
    EXPECT_EQ(parsed.entries.front().paths, QStringList{QStringLiteral("/tb/a b")});
}

TEST(SignalList, ACommentStartsOnlyWhereACommandCould) {
    const ParsedSignalList parsed = parse("# a comment line\n"
                                          "add wave /tb/a ;# a comment after a command\n"
                                          "add wave /tb/b # not a comment\n");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    ASSERT_EQ(parsed.entries.size(), 2U);
    EXPECT_EQ(parsed.entries[0].paths.size(), 1);
    EXPECT_EQ(parsed.entries[1].paths.size(), 5) << "as in Tcl, `#` mid-command is a word";
}

TEST(SignalList, NothingIsEvaluated) {
    const ParsedSignalList parsed = parse("set x 1\nadd wave /tb/a\n");
    ASSERT_EQ(parsed.errors.size(), 1);
    EXPECT_EQ(parsed.errors.front().line, 1);
    EXPECT_NE(parsed.errors.front().text.indexOf(QStringLiteral("'set'")), -1);
}

TEST(SignalList, ExpandingWhatWouldNeedEvaluatingIsRefused) {
    EXPECT_FALSE(parse("add wave {*}$paths\n").errors.isEmpty());

    // Literal text expands into the words it plainly is; Tcl's parser does that itself, and
    // nothing is evaluated in doing it.
    const ParsedSignalList literal = parse("add wave {*}{/tb/a /tb/b}\n");
    ASSERT_TRUE(literal.errors.isEmpty()) << firstError(literal);
    EXPECT_EQ(literal.entries.front().paths.size(), 2);
}

// ---------------------------------------------------------------------------
// Mistakes
// ---------------------------------------------------------------------------

TEST(SignalList, AnUnclosedBraceReadsNothingAndNamesItsLine) {
    const ParsedSignalList parsed = parse("add wave /tb/a\n"
                                          "group G {\n"
                                          "    add wave /tb/b\n");
    ASSERT_EQ(parsed.errors.size(), 1);
    EXPECT_EQ(parsed.errors.front().line, 2) << firstError(parsed);
    EXPECT_TRUE(parsed.entries.empty()) << "not even the line before it";
}

TEST(SignalList, AMistakeInsideABodyIsReportedOnItsOwnLine) {
    const ParsedSignalList parsed = parse("group G {\n"
                                          "    add wave /tb/a\n"
                                          "    add wave -colour red /tb/b\n"
                                          "}\n");
    ASSERT_EQ(parsed.errors.size(), 1);
    EXPECT_EQ(parsed.errors.front().line, 3) << firstError(parsed);
}

TEST(SignalList, ValuesAreChecked) {
    EXPECT_FALSE(parse("add wave -radix ternary /tb/a\n").errors.isEmpty());
    EXPECT_FALSE(parse("add wave -highlight 6 /tb/a\n").errors.isEmpty());
    EXPECT_FALSE(parse("add wave -highlight two /tb/a\n").errors.isEmpty());
    EXPECT_FALSE(parse("add wave -label\n").errors.isEmpty());
    EXPECT_FALSE(parse("add wave\n").errors.isEmpty());
    EXPECT_FALSE(parse("add /tb/a\n").errors.isEmpty());
    EXPECT_FALSE(parse("group G\n").errors.isEmpty());
}

TEST(SignalList, OptionsThatContradictTheirPathsAreRefused) {
    EXPECT_FALSE(parse("add wave -label x /tb/a /tb/b\n").errors.isEmpty());
    EXPECT_FALSE(parse("add wave -contents {} /tb/a /tb/b\n").errors.isEmpty());
    EXPECT_FALSE(parse("add wave -expand -contents {} /tb/a\n").errors.isEmpty());
    EXPECT_FALSE(parse("add wave -r -label x /tb\n").errors.isEmpty());
}

TEST(SignalList, NestingIsBounded) {
    // A file is untrusted input, and each level of a body is a level of recursion.
    QByteArray deep;
    for (int level = 0; level < 100; ++level) deep += "group g {";
    for (int level = 0; level < 100; ++level) deep += "}";
    const ParsedSignalList parsed = parseSignalList(deep);
    ASSERT_FALSE(parsed.errors.isEmpty());
    EXPECT_NE(parsed.errors.front().text.indexOf(QStringLiteral("nested")), -1);
}

// ---------------------------------------------------------------------------
// Writing words back
// ---------------------------------------------------------------------------

TEST(SignalList, AWordIsQuotedOnlyWhenItMustBe) {
    EXPECT_EQ(quoteSignalListWord(QStringLiteral("/tb/gen[0].q")), QStringLiteral("/tb/gen[0].q"));
    EXPECT_EQ(quoteSignalListWord(QStringLiteral("two words")), QStringLiteral("{two words}"));
    EXPECT_EQ(quoteSignalListWord(QString()), QStringLiteral("{}"));
}

TEST(SignalList, EveryWordReadsBackAsItWasWritten) {
    const QStringList words = {
        QStringLiteral("plain"),      QStringLiteral("two words"),   QStringLiteral("a{b"),
        QStringLiteral("a}b"),        QStringLiteral("{balanced}"),  QStringLiteral("back\\slash"),
        QStringLiteral("semi;colon"), QStringLiteral("dollar$sign"), QStringLiteral("half[open"),
        QStringLiteral("close]half"), QStringLiteral("#hash"),       QStringLiteral("quote\"d"),
        QStringLiteral("new\nline"),  QStringLiteral("tab\there"),   QStringLiteral("${unclosed"),
        QStringLiteral("x$y(z"),      QStringLiteral("end\\"),       QStringLiteral("ünïcode"),
    };

    for (const QString& word : words) {
        const QByteArray text =
            QStringLiteral("add wave -label %1 /tb/a\n").arg(quoteSignalListWord(word)).toUtf8();
        const ParsedSignalList parsed = parseSignalList(text);
        ASSERT_TRUE(parsed.errors.isEmpty()) << word.toStdString() << " written as "
                                             << text.toStdString() << ": " << firstError(parsed);
        ASSERT_EQ(parsed.entries.size(), 1U) << word.toStdString();
        EXPECT_EQ(parsed.entries.front().label, word) << "written as " << text.toStdString();
    }
}

TEST(SignalList, RadixWordsRoundTrip) {
    for (const Radix radix :
         {Radix::Binary, Radix::Octal, Radix::Decimal, Radix::Hexadecimal, Radix::Ascii}) {
        EXPECT_EQ(radixFromWord(radixWord(radix)), radix);
    }
    EXPECT_FALSE(radixFromWord(QStringLiteral("Hexadecimal")).has_value());
}

} // namespace
