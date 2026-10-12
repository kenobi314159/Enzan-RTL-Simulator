// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for running NVC's analysis: units reaching the library, NVC's diagnostics reaching
// Enzan's engine where they belong, and NVC's fatal errors stopping the analysis rather than
// Enzan.

#include "frontend/vhdl/VhdlFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::frontend::vhdl::analyseUnits;
using enzan::frontend::vhdl::UnitOptions;
using enzan::test::Workspace;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

/// Analyses VHDL text against a `std` the suite bootstraps once from NVC's own source.
class VhdlAnalysisTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        libraries = std::make_unique<Workspace>();

        SourceManager standardSources;
        DiagnosticEngine standardDiagnostics(standardSources);
        const enzan::FileReadResult standard = standardSources.readFile(
            std::filesystem::path(ENZAN_NVC_SOURCE_DIR) / "lib" / "std.08" / "standard.vhd");
        ASSERT_TRUE(standard.ok());

        UnitOptions options;
        options.sources = {standard.buffer};
        options.work = {.name = "std", .directory = *libraries / "std.08"};
        options.bootstrap = true;
        bootstrapped = analyseUnits(options, standardSources, standardDiagnostics);
    }

    static void TearDownTestSuite() { libraries.reset(); }

    VhdlAnalysisTest() : engine(sources) { engine.addConsumer(consumer); }

    void SetUp() override { ASSERT_TRUE(bootstrapped) << "std.standard did not bootstrap"; }

    /// Analyses `text` into a library called `name` in the workspace, against `std` and whatever
    /// else the workspace holds.
    bool analyse(std::string_view text, std::string_view name = "work") {
        const enzan::BufferId buffer = sources.addBuffer(
            "design" + std::to_string(sources.bufferCount()) + ".vhd", std::string(text));
        UnitOptions options;
        options.sources = {buffer};
        options.work = {.name = std::string(name), .directory = workspace / name};
        options.searchDirectories = {libraries->path(), workspace.path()};
        return analyseUnits(options, sources, engine);
    }

    [[nodiscard]] const Diagnostic* first(DiagCode code) const {
        const auto found = std::ranges::find(consumer.received, code, &Diagnostic::code);
        return found == consumer.received.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::string transcript() const {
        std::string text;
        for (const Diagnostic& diagnostic : consumer.received) text += diagnostic.message + '\n';
        return text;
    }

    static inline std::unique_ptr<Workspace> libraries;
    static inline bool bootstrapped = false;

    Workspace workspace;
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
};

TEST_F(VhdlAnalysisTest, BootstrapsTheStandardPackage) {
    EXPECT_TRUE(std::filesystem::exists(*libraries / "std.08" / "STD.STANDARD"));
}

TEST_F(VhdlAnalysisTest, WritesEachUnitIntoTheLibrary) {
    ASSERT_TRUE(analyse("entity counter is\n"
                        "  port (clk : in bit; count : out natural);\n"
                        "end entity;\n"
                        "architecture rtl of counter is\n"
                        "begin\n"
                        "end architecture;\n"))
        << transcript();

    EXPECT_TRUE(std::filesystem::exists(workspace / "work" / "WORK.COUNTER"));
    EXPECT_TRUE(std::filesystem::exists(workspace / "work" / "WORK.COUNTER-RTL"));
    EXPECT_TRUE(consumer.received.empty()) << transcript();
}

TEST_F(VhdlAnalysisTest, ReportsAnErrorWhereItIs) {
    EXPECT_FALSE(analyse("entity e is\n"
                         "end entity;\n"
                         "architecture a of e is\n"
                         "  signal s : bit := 2;\n"
                         "begin\n"
                         "end architecture;\n"));

    const Diagnostic* const error = first(DiagCode::VhdlError);
    ASSERT_NE(error, nullptr) << transcript();
    ASSERT_TRUE(error->location.valid());
    EXPECT_EQ(sources.lineNumber(error->location), 4U);
    EXPECT_EQ(sources.columnNumber(error->location), 21U) << "at the literal 2";
}

TEST_F(VhdlAnalysisTest, LeavesNoTerminalMarkupInAMessage) {
    EXPECT_FALSE(analyse("entity e is\n"
                         "end entity;\n"
                         "architecture a of e is\n"
                         "begin\n"
                         "  nonesuch <= '1';\n"
                         "end architecture;\n"));

    ASSERT_FALSE(consumer.received.empty());
    for (const Diagnostic& diagnostic : consumer.received) {
        EXPECT_EQ(diagnostic.message.find('$'), std::string::npos) << diagnostic.message;
        for (const enzan::DiagnosticNote& note : diagnostic.notes)
            EXPECT_EQ(note.message.find('$'), std::string::npos) << note.message;
    }
}

TEST_F(VhdlAnalysisTest, ResolvesAnotherLibraryOnTheSearchPath) {
    ASSERT_TRUE(analyse("package widths is\n"
                        "  constant word : natural := 32;\n"
                        "end package;\n",
                        "shapes"))
        << transcript();

    EXPECT_TRUE(analyse("library shapes;\n"
                        "use shapes.widths.all;\n"
                        "entity e is\n"
                        "  generic (n : natural := word);\n"
                        "end entity;\n"))
        << transcript();
}

TEST_F(VhdlAnalysisTest, StopsTheAnalysisNotEnzanOnAFatalError) {
    // NVC ends its process when a library a design names cannot be found.
    EXPECT_FALSE(analyse("library nonesuch;\n"
                         "use nonesuch.things.all;\n"
                         "entity e is\n"
                         "end entity;\n"));

    const Diagnostic* const stopped = first(DiagCode::VhdlAnalysisStopped);
    ASSERT_NE(stopped, nullptr) << transcript();
    EXPECT_NE(stopped->message.find("NONESUCH"), std::string::npos) << stopped->message;
}

/// Leaves a file behind when it is destroyed, as any of Enzan's own static objects might act on the
/// world from a destructor.
class ExitMark {
public:
    explicit ExitMark(std::filesystem::path where) : path(std::move(where)) {}

    ExitMark(const ExitMark&) = delete;
    ExitMark& operator=(const ExitMark&) = delete;
    ExitMark(ExitMark&&) = delete;
    ExitMark& operator=(ExitMark&&) = delete;

    ~ExitMark() { std::ofstream(path) << "destroyed\n"; }

private:
    std::filesystem::path path;
};

TEST_F(VhdlAnalysisTest, RunsNoneOfEnzansExitHandlersWhenNvcStops) {
    // NVC leaves a fatal error through exit(), which in the child that runs it would destroy the
    // statics of the Enzan it was forked from. This one is registered before the fork.
    static const ExitMark Mark(workspace / "destroyed");

    EXPECT_FALSE(analyse("library nonesuch;\n"
                         "use nonesuch.things.all;\n"
                         "entity e is\n"
                         "end entity;\n"));

    EXPECT_FALSE(std::filesystem::exists(workspace / "destroyed"));
}

TEST_F(VhdlAnalysisTest, ReportsAFatalErrorOnce) {
    EXPECT_FALSE(analyse("library nonesuch;\n"
                         "use nonesuch.things.all;\n"
                         "entity e is\n"
                         "end entity;\n"));

    EXPECT_EQ(
        std::ranges::count(consumer.received, DiagCode::VhdlAnalysisStopped, &Diagnostic::code), 1)
        << transcript();
}

TEST_F(VhdlAnalysisTest, StartsEachAnalysisAfresh) {
    // Two libraries of one name in two places. NVC caches a library by its name for as long as
    // its process lives, so had both analyses run in one process the second would have written
    // into the first's directory.
    const Workspace elsewhere;
    ASSERT_TRUE(analyse("package first is\nend package;\n")) << transcript();

    const enzan::BufferId buffer =
        sources.addBuffer("second.vhd", "package second is\nend package;\n");
    UnitOptions options;
    options.sources = {buffer};
    options.work = {.name = "work", .directory = elsewhere / "work"};
    options.searchDirectories = {libraries->path()};
    ASSERT_TRUE(analyseUnits(options, sources, engine)) << transcript();

    EXPECT_TRUE(std::filesystem::exists(workspace / "work" / "WORK.FIRST"));
    EXPECT_FALSE(std::filesystem::exists(workspace / "work" / "WORK.SECOND"));
    EXPECT_TRUE(std::filesystem::exists(elsewhere / "work" / "WORK.SECOND"));
}

} // namespace
