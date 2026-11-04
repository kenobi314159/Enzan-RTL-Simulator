// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for running NVC's analysis: units reaching the library, NVC's diagnostics reaching
// Enzan's engine where they belong, and NVC's fatal errors stopping the analysis rather than
// Enzan. Each analysis runs against the standard libraries the build analysed
// (tools/vhdl-libraries).

#include "frontend/vhdl/VhdlFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
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
using enzan::frontend::vhdl::Standard;
using enzan::frontend::vhdl::UnitOptions;
using enzan::test::Workspace;

/// Where the build put the standard libraries.
std::filesystem::path libraries() {
    return ENZAN_VHDL_LIBRARY_ROOT;
}

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

/// Analyses VHDL text against the standard libraries and the workspace.
class VhdlAnalysisTest : public ::testing::Test {
protected:
    VhdlAnalysisTest() : engine(sources) { engine.addConsumer(consumer); }

    /// Analyses `text` into a library called `name` in the workspace, against the standard
    /// libraries and whatever else the workspace holds.
    bool analyse(std::string_view text, std::string_view name = "work",
                 Standard standard = Standard::Vhdl2008) {
        const enzan::BufferId buffer = sources.addBuffer(
            "design" + std::to_string(sources.bufferCount()) + ".vhd", std::string(text));
        UnitOptions options;
        options.sources = {buffer};
        options.work = {.name = std::string(name), .directory = workspace / name};
        options.searchDirectories = {libraries(), workspace.path()};
        options.standard = standard;
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

    Workspace workspace;
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
};

TEST(VhdlLibrariesTest, TheBuildHoldsTheStandardLibrariesOfEachRevision) {
    for (const std::string suffix : {"", ".08", ".19"}) {
        SCOPED_TRACE("revision suffix '" + suffix + "'");
        EXPECT_TRUE(std::filesystem::exists(libraries() / ("std" + suffix) / "STD.STANDARD"));
        EXPECT_TRUE(std::filesystem::exists(libraries() / ("std" + suffix) / "STD.TEXTIO-body"));
        EXPECT_TRUE(
            std::filesystem::exists(libraries() / ("ieee" + suffix) / "IEEE.NUMERIC_STD-body"));
        EXPECT_TRUE(std::filesystem::exists(libraries() / ("nvc" + suffix) / "NVC.PSL_SUPPORT"));
    }
}

TEST_F(VhdlAnalysisTest, AnalysesAgainstIeeeAsVhdl2008HasIt) {
    // A concurrent assertion brings in NVC's psl_support, and stop() is std.env's.
    EXPECT_TRUE(analyse("library ieee;\n"
                        "use ieee.std_logic_1164.all;\n"
                        "use ieee.numeric_std.all;\n"
                        "entity counter is\n"
                        "  port (clk : in std_logic; count : out unsigned(7 downto 0));\n"
                        "end entity;\n"
                        "architecture rtl of counter is\n"
                        "  signal value : unsigned(7 downto 0) := (others => '0');\n"
                        "begin\n"
                        "  value <= value + 1 when rising_edge(clk);\n"
                        "  count <= value;\n"
                        "  assert value /= x\"ff\" report \"wrapped\";\n"
                        "  process begin\n"
                        "    wait for 10 ns;\n"
                        "    std.env.stop;\n"
                        "  end process;\n"
                        "end architecture;\n"))
        << transcript();
}

TEST_F(VhdlAnalysisTest, AnalysesAgainstSynopsysPackagesAsVhdl1993HasThem) {
    EXPECT_TRUE(analyse("library ieee;\n"
                        "use ieee.std_logic_1164.all;\n"
                        "use ieee.std_logic_unsigned.all;\n"
                        "entity adder is\n"
                        "  port (a, b : in std_logic_vector(3 downto 0);\n"
                        "        sum : out std_logic_vector(3 downto 0));\n"
                        "end entity;\n"
                        "architecture rtl of adder is\n"
                        "begin\n"
                        "  sum <= a + b;\n"
                        "end architecture;\n",
                        "work", Standard::Vhdl1993))
        << transcript();
}

TEST_F(VhdlAnalysisTest, AnalysesAgainstVhdl2019sLibraries) {
    EXPECT_TRUE(analyse("library ieee;\n"
                        "use ieee.std_logic_1164.all;\n"
                        "entity inverter is\n"
                        "  port (a : in std_logic; y : out std_logic);\n"
                        "end entity;\n"
                        "architecture rtl of inverter is\n"
                        "begin\n"
                        "  y <= not a;\n"
                        "end architecture;\n",
                        "work", Standard::Vhdl2019))
        << transcript();
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

TEST_F(VhdlAnalysisTest, RefusesAnEncryptedEnvelopeWithOneError) {
    // NVC refuses protect tool directives (IEEE 1076-2008 section 24.1) at the first and skips to
    // `end_protected`. Pinned so that an NVC upgrade skipping envelopes silently is noticed. The
    // envelope is the whole file, as in Vivado's encrypted packages, and its blocks hold '+', '/'
    // and '=', as real ones do: NVC skips by tokens, misses `end_protected` after a block with
    // none, and misreads what follows an envelope in its file, refusing the file all the same.
    EXPECT_FALSE(analyse("`protect begin_protected\n"
                         "`protect version = 1\n"
                         "`protect encrypt_agent = \"written for this test\"\n"
                         "`protect key_keyowner = \"nobody\", key_method = \"rsa\"\n"
                         "`protect encoding = (enctype = \"base64\", line_length = 64, bytes = 3)\n"
                         "`protect key_block\n"
                         "AA+/\n"
                         "`protect encoding = (enctype = \"base64\", line_length = 64, bytes = 5)\n"
                         "`protect data_block\n"
                         "AAAA+/8=\n"
                         "`protect end_protected\n"));

    ASSERT_EQ(std::ranges::count(consumer.received, DiagCode::VhdlError, &Diagnostic::code), 1)
        << transcript();
    const Diagnostic* const error = first(DiagCode::VhdlError);
    EXPECT_NE(error->message.find("protect"), std::string::npos) << error->message;
    ASSERT_TRUE(error->location.valid());
    EXPECT_EQ(sources.lineNumber(error->location), 1U);
}

TEST_F(VhdlAnalysisTest, TellsADesignNamingVitalThatItIsOutOfScope) {
    // Enzan's `ieee` has no VITAL, and NVC says only that a unit is missing, which reads as a
    // broken installation. This also pins the NVC wording the frontend recognises.
    EXPECT_FALSE(analyse("library ieee;\n"
                         "use ieee.vital_timing.all;\n"
                         "entity cell is\n"
                         "end entity;\n"));

    const Diagnostic* const vital = first(DiagCode::VhdlVital);
    ASSERT_NE(vital, nullptr) << transcript();
    EXPECT_NE(vital->message.find("'ieee.vital_timing'"), std::string::npos) << vital->message;
    ASSERT_TRUE(vital->location.valid());
    EXPECT_EQ(sources.lineNumber(vital->location), 2U);
    EXPECT_EQ(transcript().find("not found in library"), std::string::npos) << transcript();
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
    options.searchDirectories = {libraries()};
    ASSERT_TRUE(analyseUnits(options, sources, engine)) << transcript();

    EXPECT_TRUE(std::filesystem::exists(workspace / "work" / "WORK.FIRST"));
    EXPECT_FALSE(std::filesystem::exists(workspace / "work" / "WORK.SECOND"));
    EXPECT_TRUE(std::filesystem::exists(elsewhere / "work" / "WORK.SECOND"));
}

} // namespace
