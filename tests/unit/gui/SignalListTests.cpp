// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading and writing signal list files (DESIGN_GUIDE.md section 10.8): Tcl's syntax, two
// commands, and nothing evaluated; and what a scope resolves to, in a hierarchy built here with
// the empty and deep scopes the window's fixture design lacks. What a list turns into in a
// window is SignalListLoadTests.

#include "gui/SignalList.h"

#include "gui/KernelSession.h"
#include "gui/TclSyntax.h"
#include "gui/Theme.h"
#include "gui/WaveformModel.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using enzan::eir::Radix;
using enzan::gui::KernelSession;
using enzan::gui::ParsedSignalList;
using enzan::gui::parseSignalList;
using enzan::gui::quoteTclWord;
using enzan::gui::radixFromWord;
using enzan::gui::radixWord;
using enzan::gui::ResolvedSignalList;
using enzan::gui::resolveSignalList;
using enzan::gui::scopesNeededBy;
using enzan::gui::SignalInfo;
using enzan::gui::SignalListEntry;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveRow;
using enzan::ipc::InstanceEntry;

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

TEST(SignalList, AWrongRadixIsToldEveryRadixThereIs) {
    const auto parsed = parse("add wave -radix ternary /tb/a\n");
    ASSERT_EQ(parsed.errors.size(), 1);
    EXPECT_TRUE(parsed.errors.front().text.endsWith(QStringLiteral("bin, oct, dec, hex, ascii")))
        << firstError(parsed);
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
    for (int level = 0; level < 1000; ++level) deep += "group g {";
    for (int level = 0; level < 1000; ++level) deep += "}";
    const ParsedSignalList parsed = parseSignalList(deep);
    ASSERT_FALSE(parsed.errors.isEmpty());
    EXPECT_NE(parsed.errors.front().text.indexOf(QStringLiteral("nested")), -1);
}

// ---------------------------------------------------------------------------
// Writing words back
// ---------------------------------------------------------------------------

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
            QStringLiteral("add wave -label %1 /tb/a\n").arg(quoteTclWord(word)).toUtf8();
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

// ---------------------------------------------------------------------------
// Scopes, in a hierarchy of the test's own
// ---------------------------------------------------------------------------

/// Instances and their objects as the kernel lists them: a depth-first walk, each instance's
/// path its parent's and its own name.
struct Hierarchy {
    QVector<InstanceEntry> instances;
    QHash<QString, QVector<SignalInfo>> objects;
    std::uint32_t nextSignal = 0;

    /// Adds `name` below `parent`, or as a top when there is none, declaring one signal for each
    /// of `names`. Returns its index for the instances below it.
    std::uint32_t add(std::uint32_t parent, const std::string& name,
                      std::initializer_list<const char*> names = {}) {
        const std::string path =
            (parent == InstanceEntry::NoParent ? std::string() : instances[parent].path) + '/'
            + name;
        instances.push_back(InstanceEntry{.path = path, .name = name, .parent = parent});

        QVector<SignalInfo>& declared = objects[QString::fromStdString(path)];
        for (const char* leaf : names) {
            SignalInfo info;
            info.signalIndex = nextSignal++;
            info.name = QString::fromUtf8(leaf);
            info.path = QString::fromStdString(path) + u'/' + info.name;
            declared.push_back(info);
        }
        return static_cast<std::uint32_t>(instances.size() - 1);
    }

    [[nodiscard]] ResolvedSignalList resolve(const char* text) const {
        const ParsedSignalList parsed = parse(text);
        EXPECT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
        return resolveSignalList(parsed.entries, instances, objects);
    }
};

/// Rows written as `label{children}`, and a signal as its label, so that a tree is one
/// expectation.
std::string shapeOf(const std::vector<WaveformModel::PendingRow>& rows) {
    std::string shape;
    for (const WaveformModel::PendingRow& pending : rows) {
        if (!shape.empty()) shape += ' ';
        shape += pending.row.label.toStdString();
        if (pending.row.kind == WaveRow::Kind::Group)
            shape += '{' + shapeOf(pending.children) + '}';
    }
    return shape;
}

/// Whether `rows` and everything below them show `radix`.
bool allShow(const std::vector<WaveformModel::PendingRow>& rows, Radix radix) {
    return std::ranges::all_of(rows, [radix](const WaveformModel::PendingRow& pending) {
        return pending.row.radix == radix && allShow(pending.children, radix);
    });
}

TEST(SignalList, ARecursiveScopeNestsItsSubtreeAsTheDesignDoes) {
    Hierarchy design;
    const std::uint32_t tb = design.add(InstanceEntry::NoParent, "tb", {"clk"});
    const std::uint32_t a = design.add(tb, "a", {"x"});
    design.add(a, "leaf", {"y"});
    design.add(tb, "b", {"z"});

    // Below the top an instance is named by its name, the group around it saying where it is;
    // a scope's own signals come first, in a group of their own.
    const ResolvedSignalList resolved = design.resolve("add wave -r -radix bin /tb\n");
    EXPECT_EQ(shapeOf(resolved.rows), "/tb{$top{clk} a{$top{x} leaf{y}} b{z}}");
    EXPECT_TRUE(allShow(resolved.rows, Radix::Binary)) << "every row the command adds";

    // Without -r, the scope alone.
    EXPECT_EQ(shapeOf(design.resolve("add wave /tb/a\n").rows), "/tb/a{x}");
}

TEST(SignalList, AScopeWithNoSignalsAddsNoGroup) {
    Hierarchy design;
    const std::uint32_t tb = design.add(InstanceEntry::NoParent, "tb");
    design.add(tb, "empty");
    design.add(design.add(tb, "hollow"), "void");
    design.add(design.add(tb, "only"), "kid", {"k"});
    design.add(design.add(tb, "lone", {"s"}), "void");

    // A subtree with nothing in it leaves nothing behind, and a scope whose subtree turned out
    // empty is the group of its signals, with no `$top` around them.
    EXPECT_EQ(shapeOf(design.resolve("add wave -r /tb\n").rows), "/tb{only{kid{k}} lone{s}}");
    EXPECT_TRUE(design.resolve("add wave /tb\n").rows.empty());
    EXPECT_TRUE(design.resolve("add wave -r /tb/hollow\n").rows.empty());
}

TEST(SignalList, ASubtreeIsTheInstancesBelowItWhateverTheirNames) {
    // An escaped identifier may hold a separator, and the instance it names is still a child
    // of the scope it is in rather than a scope two levels down.
    Hierarchy design;
    const std::uint32_t tb = design.add(InstanceEntry::NoParent, "tb", {"clk"});
    design.add(tb, "\\a/b ", {"q"});

    const ResolvedSignalList resolved = design.resolve("add wave -r /tb\n");
    EXPECT_EQ(shapeOf(resolved.rows), "/tb{$top{clk} \\a/b {q}}");

    const ParsedSignalList parsed = parse("add wave -r /tb\n");
    EXPECT_EQ(scopesNeededBy(parsed.entries, design.instances),
              QStringList({QStringLiteral("/tb"), QStringLiteral("/tb/\\a/b ")}));
}

TEST(SignalList, EachScopeIsAskedAboutOnceInTheOrderItIsFirstNeeded) {
    Hierarchy design;
    const std::uint32_t tb = design.add(InstanceEntry::NoParent, "tb", {"clk"});
    const std::uint32_t a = design.add(tb, "a", {"x"});
    design.add(a, "leaf", {"y"});
    design.add(tb, "b", {"z"});

    const ParsedSignalList parsed = parse("add wave /tb/b/z\n"
                                          "add wave -r /tb\n"
                                          "group g { add wave /tb/a }\n");
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    EXPECT_EQ(scopesNeededBy(parsed.entries, design.instances),
              QStringList({QStringLiteral("/tb/b"), QStringLiteral("/tb"), QStringLiteral("/tb/a"),
                           QStringLiteral("/tb/a/leaf")}));
}

TEST(SignalList, TheDeepestSubtreeSavesAListThatLoadsBack) {
    // A recursive add nests a group for each level of the design, and slang elaborates a
    // hierarchy up to 128 instances deep. Saving writes the nesting as it is, so loading what
    // was saved must accept it.
    Hierarchy design;
    std::uint32_t scope = design.add(InstanceEntry::NoParent, "tb", {"clk"});
    for (int level = 0; level < 128; ++level)
        scope = design.add(scope, "l" + std::to_string(level), {"q"});

    const KernelSession session;
    const Theme theme;
    WaveformModel added(session, theme);
    static_cast<void>(added.insertSignalRows(design.resolve("add wave -r /tb\n").rows, -1, -1));
    const QString saved = added.writeSignalList();

    const ParsedSignalList parsed = parseSignalList(saved.toUtf8());
    ASSERT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);

    WaveformModel loaded(session, theme);
    static_cast<void>(loaded.insertSignalRows(
        resolveSignalList(parsed.entries, design.instances, design.objects).rows, -1, -1));
    EXPECT_EQ(loaded.writeSignalList(), saved);
}

} // namespace
