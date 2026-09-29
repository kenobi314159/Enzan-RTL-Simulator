// SPDX-License-Identifier: GPL-3.0-or-later

#include "session/FileList.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::session::FileList;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

class FileListTest : public ::testing::Test {
protected:
    // The tests stand in the workspace because that is where a `.f` file's relative paths
    // resolve to (section 8.3), and checking that convention from another directory would
    // check something else.
    FileListTest() : standing(workspace.path()), engine(sources) { engine.addConsumer(consumer); }

    /// Reads `name` from the workspace and returns whether it succeeded.
    bool read(std::string_view name) {
        return enzan::session::readFileList(workspace / name, sources, engine, list);
    }

    [[nodiscard]] std::vector<std::string> fileNames() const {
        std::vector<std::string> names;
        names.reserve(list.files.size());
        for (const std::filesystem::path& file : list.files) names.push_back(file.string());
        return names;
    }

    [[nodiscard]] DiagCode firstCode() const {
        return consumer.received.empty() ? DiagCode::InternalError : consumer.received.front().code;
    }

    enzan::test::Workspace workspace;
    enzan::test::ScopedWorkingDirectory standing;
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    FileList list;
};

// ---------------------------------------------------------------------------
// The ordinary shape of a file list
// ---------------------------------------------------------------------------

TEST_F(FileListTest, ReadsFilesOneToALine) {
    workspace.write("design.f", "rtl/alu.sv\nrtl/regfile.sv\ntb/tb_top.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(),
              (std::vector<std::string>{"rtl/alu.sv", "rtl/regfile.sv", "tb/tb_top.sv"}));
}

TEST_F(FileListTest, ReadsSeveralFilesFromOneLine) {
    workspace.write("design.f", "a.sv b.sv\tc.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"a.sv", "b.sv", "c.sv"}));
}

TEST_F(FileListTest, KeepsAFileNamedTwice) {
    workspace.write("design.f", "a.sv\na.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"a.sv", "a.sv"}));
}

TEST_F(FileListTest, SkipsLineAndBlockComments) {
    workspace.write("design.f", "// the core\na.sv // and its testbench\n"
                                "/* several\n   lines */ b.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"a.sv", "b.sv"}));
}

TEST_F(FileListTest, ReadsAQuotedPathWithSpaces) {
    workspace.write("design.f", "\"with space/alu.sv\"\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"with space/alu.sv"}));
}

TEST_F(FileListTest, AnEmptyListIsNotAnError) {
    workspace.write("design.f", "// nothing here yet\n");

    EXPECT_TRUE(read("design.f"));
    EXPECT_TRUE(list.files.empty());
    EXPECT_TRUE(consumer.received.empty());
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

TEST_F(FileListTest, CollectsIncludeDirectories) {
    workspace.write("design.f", "+incdir+rtl/include+vendor/include\n+incdir+more\na.sv\n");

    ASSERT_TRUE(read("design.f"));
    ASSERT_EQ(list.includeDirectories.size(), 3U);
    EXPECT_EQ(list.includeDirectories[0], "rtl/include");
    EXPECT_EQ(list.includeDirectories[1], "vendor/include");
    EXPECT_EQ(list.includeDirectories[2], "more");
}

TEST_F(FileListTest, CollectsDefines) {
    workspace.write("design.f", "+define+SYNTHESIS+WIDTH=32\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(list.defines, (std::vector<std::string>{"SYNTHESIS", "WIDTH=32"}));
}

TEST_F(FileListTest, CollectsLibraryFilesAndDirectories) {
    workspace.write("design.f", "-v vendor/cells.v\n-y vendor/lib\n+libext+.v+.sv\n");

    ASSERT_TRUE(read("design.f"));
    ASSERT_EQ(list.libraryFiles.size(), 1U);
    EXPECT_EQ(list.libraryFiles.front(), "vendor/cells.v");
    ASSERT_EQ(list.libraryDirectories.size(), 1U);
    EXPECT_EQ(list.libraryDirectories.front(), "vendor/lib");
    EXPECT_EQ(list.libraryExtensions, (std::vector<std::string>{".v", ".sv"}));
    EXPECT_TRUE(list.files.empty());
}

TEST_F(FileListTest, SaysSoWhenALibrarySearchIsAskedForAndNotPerformed) {
    // Nothing searches for an undefined module yet. An option this reader understands and then
    // drops is the same failure as one it does not understand — what is analysed stops matching
    // what was asked for — so it is reported, at the position that asked for it.
    workspace.write("design.f", "-y vendor/lib\n-v vendor/cells.v\na.sv\n");

    // A warning, not a refusal: the list is still read, because most lists carrying a `-v` carry
    // it for a cell the design never instantiates.
    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"a.sv"}));

    ASSERT_EQ(consumer.received.size(), 2U);
    EXPECT_EQ(consumer.received[0].code, DiagCode::FileListLibrarySearchIgnored);
    EXPECT_NE(consumer.received[0].message.find("-y"), std::string::npos)
        << consumer.received[0].message;
    EXPECT_EQ(consumer.received[1].code, DiagCode::FileListLibrarySearchIgnored);
    EXPECT_NE(consumer.received[1].message.find("-v"), std::string::npos)
        << consumer.received[1].message;

    // The position, so the fix is a visible edit rather than a hunt (section 8.3).
    EXPECT_TRUE(consumer.received[0].location.valid());
}

TEST_F(FileListTest, AListWithNoLibrarySearchReportsNothing) {
    workspace.write("design.f", "+incdir+rtl\n+define+W=8\na.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_TRUE(consumer.received.empty());
}

TEST_F(FileListTest, AcceptsMinusSvWithoutRecordingIt) {
    workspace.write("design.f", "-sv a.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"a.sv"}));
}

TEST_F(FileListTest, TrailingPlusesInAListAreIgnored) {
    workspace.write("design.f", "+incdir+one++two+\n");

    ASSERT_TRUE(read("design.f"));
    ASSERT_EQ(list.includeDirectories.size(), 2U);
    EXPECT_EQ(list.includeDirectories[0], "one");
    EXPECT_EQ(list.includeDirectories[1], "two");
}

// ---------------------------------------------------------------------------
// Nesting
// ---------------------------------------------------------------------------

TEST_F(FileListTest, NestsWithMinusF) {
    workspace.write("outer.f", "a.sv\n-f inner.f\nc.sv\n");
    workspace.write("inner.f", "b.sv\n");

    ASSERT_TRUE(read("outer.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"a.sv", "b.sv", "c.sv"}));
}

TEST_F(FileListTest, MinusCapitalFResolvesAgainstTheListsOwnDirectory) {
    workspace.write("sub/inner.f", "b.sv\n");
    workspace.write("outer.f", "-F sub/inner.f\n");

    // The nested list is found beside the list that named it, and so is everything *inside* it:
    // `-F` says the list was written as though the tool had been run from its own directory
    // (DESIGN_GUIDE.md section 8.3). Anchoring only the list's own path and not its contents
    // would send the analysis looking for `b.sv` in the working directory, where it is not.
    ASSERT_TRUE(read("outer.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{(workspace / "sub" / "b.sv").string()}));
}

TEST_F(FileListTest, MinusCapitalFAnchorsTheOptionsInsideItToo) {
    workspace.write("sub/inner.f", "+incdir+include\n-v cells.v\n-y cells\n");
    workspace.write("outer.f", "-F sub/inner.f\n");

    ASSERT_TRUE(read("outer.f"));
    ASSERT_EQ(list.includeDirectories.size(), 1U);
    EXPECT_EQ(list.includeDirectories.front(), workspace / "sub" / "include");
    ASSERT_EQ(list.libraryFiles.size(), 1U);
    EXPECT_EQ(list.libraryFiles.front(), workspace / "sub" / "cells.v");
    ASSERT_EQ(list.libraryDirectories.size(), 1U);
    EXPECT_EQ(list.libraryDirectories.front(), workspace / "sub" / "cells");
}

TEST_F(FileListTest, AnAbsolutePathInsideAMinusCapitalFListIsLeftAlone) {
    const std::string absolute = (workspace / "rtl" / "a.sv").string();
    workspace.write("sub/inner.f", absolute + "\n");
    workspace.write("outer.f", "-F sub/inner.f\n");

    ASSERT_TRUE(read("outer.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{absolute}));
}

TEST_F(FileListTest, AMinusFListInsideAMinusCapitalFOneGoesBackToTheWorkingDirectory) {
    // Each switch says how the list it names was written, and `-f` says "from where the tool was
    // run" whatever the list containing it was reached by. Nesting does not accumulate.
    workspace.write("sub/inner.f", "-f plain.f\n");
    workspace.write("plain.f", "c.sv\n");
    workspace.write("outer.f", "-F sub/inner.f\n");

    ASSERT_TRUE(read("outer.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"c.sv"}));
}

TEST_F(FileListTest, MinusFResolvesAgainstTheWorkingDirectory) {
    // The nested list names itself relative to where the tool was run, not relative to the
    // list that included it, which is the convention these files were written against.
    workspace.write("sub/inner.f", "b.sv\n");
    workspace.write("sub/outer.f", "-f sub/inner.f\n");

    ASSERT_TRUE(read("sub/outer.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"b.sv"}));
}

TEST_F(FileListTest, ReportsAListThatIncludesItself) {
    workspace.write("outer.f", "-f inner.f\n");
    workspace.write("inner.f", "-f outer.f\n");

    EXPECT_FALSE(read("outer.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListCycle);
    EXPECT_NE(consumer.received.front().message.find("inner.f"), std::string::npos)
        << consumer.received.front().message;
}

TEST_F(FileListTest, ReportsAListThatIncludesItselfDirectly) {
    workspace.write("loop.f", "-f loop.f\n");

    EXPECT_FALSE(read("loop.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListCycle);
}

TEST_F(FileListTest, TheSameListTwiceIsNotACycle) {
    workspace.write("common.f", "common.sv\n");
    workspace.write("outer.f", "-f common.f\n-f common.f\n");

    ASSERT_TRUE(read("outer.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"common.sv", "common.sv"}));
}

TEST_F(FileListTest, ReportsANestedListThatDoesNotExist) {
    workspace.write("outer.f", "-f missing.f\n");

    EXPECT_FALSE(read("outer.f"));
    EXPECT_EQ(firstCode(), DiagCode::CannotOpenFile);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST_F(FileListTest, RefusesAnOptionItDoesNotUnderstand) {
    workspace.write("design.f", "a.sv\n-timescale=1ns/1ps\nb.sv\n");

    EXPECT_FALSE(read("design.f"));
    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(firstCode(), DiagCode::FileListUnknownOption);
    EXPECT_EQ(sources.lineNumber(consumer.received.front().location), 2U);
}

TEST_F(FileListTest, RefusesAnUnknownPlusOption) {
    workspace.write("design.f", "+systemverilogext+.sv\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListUnknownOption);
}

TEST_F(FileListTest, ReportsAnOptionWithNothingAfterIt) {
    workspace.write("design.f", "a.sv\n-v\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListMissingValue);
}

TEST_F(FileListTest, ReportsAnEmptyPlusList) {
    workspace.write("design.f", "+incdir+\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListEmptyOption);
}

TEST_F(FileListTest, ReportsAnUnclosedBlockComment) {
    workspace.write("design.f", "a.sv\n/* never ends\nb.sv\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListUnterminatedComment);
    EXPECT_EQ(sources.lineNumber(consumer.received.front().location), 2U);
}

TEST_F(FileListTest, ReportsAnUnclosedQuote) {
    workspace.write("design.f", "\"never ends\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListUnterminatedQuote);
}

TEST_F(FileListTest, ReportsAListThatDoesNotExist) {
    EXPECT_FALSE(read("absent.f"));
    EXPECT_EQ(firstCode(), DiagCode::CannotOpenFile);
}

// ---------------------------------------------------------------------------
// Environment substitution
// ---------------------------------------------------------------------------

/// Sets an environment variable for the duration of a test.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* variable, const char* value) : name(variable) {
        ::setenv(name, value, 1);
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
    ScopedEnvironment(ScopedEnvironment&&) = delete;
    ScopedEnvironment& operator=(ScopedEnvironment&&) = delete;

    ~ScopedEnvironment() { ::unsetenv(name); }

private:
    const char* name;
};

TEST_F(FileListTest, SubstitutesAnEnvironmentVariable) {
    const ScopedEnvironment variable("ENZAN_TEST_RTL", "/opt/rtl");
    workspace.write("design.f", "$ENZAN_TEST_RTL/alu.sv\n${ENZAN_TEST_RTL}/regfile.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"/opt/rtl/alu.sv", "/opt/rtl/regfile.sv"}));
}

TEST_F(FileListTest, SubstitutesInsideAQuotedArgument) {
    const ScopedEnvironment variable("ENZAN_TEST_RTL", "/opt/my rtl");
    workspace.write("design.f", "\"$ENZAN_TEST_RTL/alu.sv\"\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"/opt/my rtl/alu.sv"}));
}

TEST_F(FileListTest, RefusesAnUnsetVariableRatherThanExpandingItToNothing) {
    ::unsetenv("ENZAN_TEST_UNSET");
    workspace.write("design.f", "$ENZAN_TEST_UNSET/alu.sv\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::EnvironmentVariableNotSet);
}

TEST_F(FileListTest, ReportsAnUnclosedBrace) {
    const ScopedEnvironment variable("ENZAN_TEST_RTL", "/opt/rtl");
    workspace.write("design.f", "${ENZAN_TEST_RTL/alu.sv\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::FileListUnterminatedBrace);
}

TEST_F(FileListTest, ADollarFollowedByNothingNameLikeIsALiteralDollar) {
    workspace.write("design.f", "weird$.sv\n");

    ASSERT_TRUE(read("design.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"weird$.sv"}));
}

TEST_F(FileListTest, ADollarFollowedByANameIsASubstitutionEvenMidWord) {
    // `rtl$TOP.sv` naming a file with a dollar in it is vanishingly rare next to it naming a
    // variable the user forgot to set, and the second reading is the one that can be reported.
    ::unsetenv("ENZAN_TEST_MIDWORD");
    workspace.write("design.f", "rtl$ENZAN_TEST_MIDWORD.sv\n");

    EXPECT_FALSE(read("design.f"));
    EXPECT_EQ(firstCode(), DiagCode::EnvironmentVariableNotSet);
}

} // namespace
