// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/FileList.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::control::FileList;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

/// A directory of file lists that cleans itself up, plus the working directory the relative
/// paths in a `-f` are resolved against.
///
/// The working directory is changed for the duration of a test because that is what a `.f`
/// file's relative paths mean (section 8.2), and testing the convention without standing in
/// the right directory would test something else.
class Workspace {
public:
    Workspace() {
        root = std::filesystem::temp_directory_path()
               / ("enzan-filelist-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(root);
        previousDirectory = std::filesystem::current_path();
        std::filesystem::current_path(root);
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;

    ~Workspace() {
        std::error_code ignored;
        std::filesystem::current_path(previousDirectory, ignored);
        std::filesystem::remove_all(root, ignored);
    }

    /// Writes `content` to `name`, creating any directories it names.
    void write(std::string_view name, std::string_view content) const {
        const std::filesystem::path target = root / name;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target);
        file << content;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root; }

private:
    static inline int counter = 0;
    std::filesystem::path root;
    std::filesystem::path previousDirectory;
};

class FileListTest : public ::testing::Test {
protected:
    FileListTest() : engine(sources) { engine.addConsumer(consumer); }

    /// Reads `name` from the workspace and returns whether it succeeded.
    bool read(std::string_view name) {
        return enzan::control::readFileList(workspace.path() / name, sources, engine, list);
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

    Workspace workspace;
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

    ASSERT_TRUE(read("outer.f"));
    EXPECT_EQ(fileNames(), (std::vector<std::string>{"b.sv"}));
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
