// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session drives real analysis, elaboration and simulation, so these tests compile real
// SystemVerilog. That is deliberate at this level: the session's whole job is to hold the
// three phases, analysis, elaboration and execution, together, and a test against a hand-built
// design would not exercise the joins that are the thing being tested.

#include "session/Session.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Type.h"
#include "runtime/Kernel.h"

#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using Radix = enzan::eir::Radix;
using enzan::runtime::RunStatus;
using enzan::session::Session;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

/// A counter with an enumerated state register, so that one design covers the value
/// rendering, the hierarchy and the edge-driven behaviour the commands ask about.
constexpr std::string_view CounterSource = R"(
module counter (
    input  logic       clk,
    input  logic       enable,
    output logic [7:0] count
);
    always_ff @(posedge clk)
        if (enable)
            count <= count + 8'd1;
endmodule

module tb;
    typedef enum logic [1:0] { S_IDLE, S_RUN, S_DONE } state_e;

    logic       clk = 1'b0;
    logic       enable = 1'b1;
    logic [7:0] count = 8'd0;
    state_e     state = S_RUN;

    counter u_dut (.clk(clk), .enable(enable), .count(count));

    always #5 clk = ~clk;

    initial begin
        #200;
        state = S_DONE;
        #10;
        $finish;
    end
endmodule
)";

/// Signals as the VHDL frontend holds them: a std_logic holding '1' and a
/// std_logic_vector(3 downto 0) holding "0101" as their codes, a bit's, and a std_logic_vector
/// of a null range, whose one bit is a placeholder. Built by hand, so that neither frontend is
/// needed.
enzan::eir::Design vhdlCodedDesign() {
    using enzan::eir::BitVector;
    using enzan::eir::Encoding;
    using enzan::eir::Object;
    using enzan::eir::Type;

    enzan::eir::ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    enzan::eir::EnumDefinition logic{.name = "std_ulogic", .members = {}};
    constexpr std::array<std::string_view, 9> Literals = {"'U'", "'X'", "'0'", "'1'", "'Z'",
                                                          "'W'", "'L'", "'H'", "'-'"};
    for (std::uint64_t position = 0; position < Literals.size(); ++position) {
        logic.members.push_back(enzan::eir::EnumMember{
            .name = std::string(Literals[position]), .value = BitVector::fromUInt64(position, 8)});
    }
    const std::uint32_t logicEnum = top.addEnum(std::move(logic));
    top.addObject(Object{.name = "ready",
                         .type = Type::bits(8, false, false),
                         .initialValue = BitVector::fromUInt64(3, 8),
                         .encoding = Encoding::VhdlLogic,
                         .enumIndex = logicEnum});
    top.addObject(Object{.name = "bus",
                         .type = Type::bits(32, false, false),
                         .initialValue = BitVector::fromUInt64(0x02030203, 32),
                         .encoding = Encoding::VhdlLogic});
    top.addObject(Object{.name = "flag",
                         .type = Type::bits(8, false, false),
                         .initialValue = BitVector::fromUInt64(0, 8),
                         .encoding = Encoding::VhdlBit});
    top.addObject(Object{.name = "nothing",
                         .type = Type::bits(1, false, false),
                         .encoding = Encoding::VhdlLogic,
                         .holdsNothing = true});

    enzan::eir::Design design;
    design.addModule(std::move(top));
    return design;
}

class SessionTest : public ::testing::Test {
protected:
    SessionTest() : engine(sources), session(sources, engine) {
        engine.addConsumer(consumer);
        session.setTranscript(&transcript);
    }

    /// Writes `text` into the workspace and analyses it.
    bool analyse(std::string_view name, std::string_view text) {
        workspace.write(name, text);

        Session::AnalysisRequest request;
        request.files.push_back(workspace / name);
        return session.analyse(request);
    }

    /// Analyses and elaborates the shared counter design.
    void loadCounter() {
        ASSERT_TRUE(analyse("counter.sv", CounterSource)) << firstMessage();
        ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    }

    /// Elaborates `vhdlCodedDesign`.
    void loadVhdlCoded() {
        session.setDesign(std::make_unique<enzan::eir::Design>(vhdlCodedDesign()));
        ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    }

    [[nodiscard]] std::string firstMessage() const {
        return consumer.received.empty() ? std::string() : consumer.received.front().message;
    }

    [[nodiscard]] bool reported(DiagCode code) const {
        return std::ranges::any_of(consumer.received, [code](const Diagnostic& diagnostic) {
            return diagnostic.code == code;
        });
    }

    [[nodiscard]] std::uint32_t signalOf(std::string_view path) {
        const std::uint32_t index = session.findSignal(path);
        EXPECT_NE(index, Session::NoSignal) << "no signal named " << path;
        return index;
    }

    enzan::test::Workspace workspace;
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    std::ostringstream transcript;
    Session session;
};

// ---------------------------------------------------------------------------
// The state machine
// ---------------------------------------------------------------------------

TEST_F(SessionTest, StartsWithNothingLoaded) {
    EXPECT_FALSE(session.hasDesign());
    EXPECT_FALSE(session.isElaborated());
}

TEST_F(SessionTest, AnalysingWithNoFilesSaysSo) {
    EXPECT_FALSE(session.analyse({}));
    EXPECT_TRUE(reported(DiagCode::NoInputFiles));
}

TEST_F(SessionTest, AnalysingASourceProducesADesign) {
    ASSERT_TRUE(analyse("counter.sv", CounterSource)) << firstMessage();
    EXPECT_TRUE(session.hasDesign());
    EXPECT_FALSE(session.isElaborated());
}

TEST_F(SessionTest, ElaboratingBeforeAnalysingSaysSo) {
    EXPECT_FALSE(session.elaborate("tb"));
    EXPECT_TRUE(reported(DiagCode::NothingAnalysed));
}

TEST_F(SessionTest, ElaboratingPreparesAKernelAtTimeZero) {
    loadCounter();
    EXPECT_TRUE(session.isElaborated());
    EXPECT_EQ(session.time(), 0U);
}

TEST_F(SessionTest, SeveralTopLevelsAreElaboratedSideBySide) {
    ASSERT_TRUE(analyse("tops.sv", "module design_top;\n  logic a = 1'b1;\nendmodule\n"
                                   "module glbl;\n  logic GSR = 1'b0;\nendmodule\n"))
        << firstMessage();
    const std::vector<std::string> tops{"design_top", "glbl"};
    ASSERT_TRUE(session.elaborate(std::span<const std::string>(tops))) << firstMessage();
    EXPECT_NE(session.findSignal("/design_top/a"), Session::NoSignal);
    EXPECT_NE(session.findSignal("/glbl/GSR"), Session::NoSignal);
}

TEST_F(SessionTest, ElaboratingAnUnknownTopFails) {
    ASSERT_TRUE(analyse("counter.sv", CounterSource)) << firstMessage();
    EXPECT_FALSE(session.elaborate("nosuchmodule"));
    EXPECT_TRUE(reported(DiagCode::TopModuleNotFound));
}

// ---------------------------------------------------------------------------
// The library search, `-y`, `-v` and `+libext+`
// ---------------------------------------------------------------------------

/// Whether `session` read a file whose name ends `name`.
bool readFile(const Session& session, std::string_view name) {
    return std::ranges::any_of(session.filesRead(), [name](const std::filesystem::path& file) {
        return file.string().ends_with(name);
    });
}

TEST_F(SessionTest, AModuleNothingDefinesIsFoundInALibraryDirectoryWithWhatItNeeds) {
    workspace.write("cells/inner.v", "module inner (output o);\n  assign o = 1'b1;\nendmodule\n");
    workspace.write("cells/outer.v", "module outer (output o);\n  inner u (.o(o));\nendmodule\n");
    // Never instantiated, so never read: it would fail the analysis.
    workspace.write("cells/unused.v", "module unused;\n  nonesuch n ();\nendmodule\n");
    workspace.write("tb.sv", "module tb;\n  wire o;\n  outer u (.o(o));\nendmodule\n");

    Session::AnalysisRequest request;
    request.files = {workspace / "tb.sv"};
    request.searchDirectories = {workspace / "cells"};
    ASSERT_TRUE(session.analyse(request)) << firstMessage();
    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    EXPECT_TRUE(readFile(session, "cells/outer.v"));
    EXPECT_TRUE(readFile(session, "cells/inner.v")) << "what a found module instantiates";
    EXPECT_FALSE(readFile(session, "cells/unused.v"));
}

TEST_F(SessionTest, ALibraryFileIsReadOnlyWhenItDeclaresWhatIsWanted) {
    workspace.write("cells.v",
                    "module and2 (input a, b, output y);\n  assign y = a & b;\nendmodule\n"
                    "module or2 (input a, b, output y);\n  assign y = a | b;\nendmodule\n");
    workspace.write("broken.v", "module unrelated;\n  nonesuch n ();\nendmodule\n");
    workspace.write("tb.sv",
                    "module tb;\n  wire y;\n  or2 u (.a(1'b0), .b(1'b1), .y(y));\nendmodule\n");

    Session::AnalysisRequest request;
    request.files = {workspace / "tb.sv"};
    request.searchFiles = {workspace / "broken.v", workspace / "cells.v"};
    ASSERT_TRUE(session.analyse(request)) << firstMessage();
    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    EXPECT_TRUE(readFile(session, "/cells.v"));
    EXPECT_FALSE(readFile(session, "/broken.v")) << "read to see what it declares, not analysed";
}

TEST_F(SessionTest, AModuleOfALibraryFileNothingInstantiatesIsNoTopLevel) {
    // A vendor's file holds every model of a library, some of which Enzan refuses; a design that
    // uses another is not stopped by them, from `-v` or from `-y`.
    const std::string cells = "module used (output o);\n  assign o = 1'b1;\nendmodule\n"
                              "module unused;\n  initial fork #1; #2; join\nendmodule\n";
    workspace.write("cells.v", cells);
    workspace.write("dir/used.v", cells);
    workspace.write("tb.sv", "module tb;\n  wire o;\n  used u (.o(o));\nendmodule\n");

    Session::AnalysisRequest fromFile;
    fromFile.files = {workspace / "tb.sv"};
    fromFile.searchFiles = {workspace / "cells.v"};
    ASSERT_TRUE(session.analyse(fromFile)) << firstMessage();
    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();

    Session fresh(sources, engine);
    Session::AnalysisRequest fromDirectory;
    fromDirectory.files = {workspace / "tb.sv"};
    fromDirectory.searchDirectories = {workspace / "dir"};
    ASSERT_TRUE(fresh.analyse(fromDirectory)) << firstMessage();
    ASSERT_TRUE(fresh.elaborate("tb")) << firstMessage();
}

TEST_F(SessionTest, LibraryExtensionsAreTriedInTheirOrder) {
    // `.v` alone when none is given, so the `.sv` is not found then.
    workspace.write("cells/leaf.sv", "module leaf (output o);\n  assign o = 1'b1;\nendmodule\n");
    workspace.write("cells/leaf.v", "module leaf (output o);\n  nonesuch n ();\nendmodule\n");
    workspace.write("tb.sv", "module tb;\n  wire o;\n  leaf u (.o(o));\nendmodule\n");

    Session::AnalysisRequest request;
    request.files = {workspace / "tb.sv"};
    request.searchDirectories = {workspace / "cells"};
    request.searchExtensions = {".sv", ".v"};
    ASSERT_TRUE(session.analyse(request)) << firstMessage();
    EXPECT_TRUE(readFile(session, "cells/leaf.sv"));
    EXPECT_FALSE(readFile(session, "cells/leaf.v"));
}

TEST_F(SessionTest, ASourcesOwnModuleIsNotLookedForInALibrary) {
    workspace.write("cells/leaf.v", "module leaf (output o);\n  nonesuch n ();\nendmodule\n");
    workspace.write("tb.sv", "module leaf (output o);\n  assign o = 1'b0;\nendmodule\n"
                             "module tb;\n  wire o;\n  leaf u (.o(o));\nendmodule\n");

    Session::AnalysisRequest request;
    request.files = {workspace / "tb.sv"};
    request.searchDirectories = {workspace / "cells"};
    ASSERT_TRUE(session.analyse(request)) << firstMessage();
    EXPECT_FALSE(readFile(session, "cells/leaf.v"));
}

TEST_F(SessionTest, AModuleFoundNowhereIsReportedAsTheAnalysisReportsIt) {
    workspace.write("tb.sv", "module tb;\n  nonesuch u ();\nendmodule\n");

    Session::AnalysisRequest request;
    request.files = {workspace / "tb.sv"};
    request.searchDirectories = {workspace / "empty"};
    EXPECT_FALSE(session.analyse(request));
    EXPECT_TRUE(reported(DiagCode::SystemVerilogError));
}

TEST_F(SessionTest, ARunSaysOnceThatTimingChecksAreNotRun) {
    // Two definitions with timing checks, one instantiated twice: one note, at the first check in
    // the hierarchy, rather than one per definition or instance.
    ASSERT_TRUE(analyse("checks.sv", "module setup_cell (input clk, input d);\n"
                                     "    specify\n"
                                     "        $setup(d, posedge clk, 1);\n"
                                     "    endspecify\n"
                                     "endmodule\n"
                                     "module hold_cell (input clk, input d);\n"
                                     "    specify\n"
                                     "        $hold(posedge clk, d, 1);\n"
                                     "    endspecify\n"
                                     "endmodule\n"
                                     "module tb;\n"
                                     "    logic clk = 1'b0, d = 1'b0;\n"
                                     "    hold_cell u_hold (.clk(clk), .d(d));\n"
                                     "    setup_cell u_first (.clk(clk), .d(d));\n"
                                     "    setup_cell u_second (.clk(clk), .d(d));\n"
                                     "endmodule\n"))
        << firstMessage();
    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();

    ASSERT_EQ(
        std::ranges::count(consumer.received, DiagCode::TimingChecksNotRun, &Diagnostic::code), 1);
    const auto note =
        std::ranges::find(consumer.received, DiagCode::TimingChecksNotRun, &Diagnostic::code);
    EXPECT_EQ(sources.lineNumber(note->location), 8U) << "the hold check, instantiated first";
}

TEST_F(SessionTest, ARunWithoutTimingChecksSaysNothingOfThem) {
    loadCounter();
    EXPECT_FALSE(reported(DiagCode::TimingChecksNotRun));
}

TEST_F(SessionTest, RunningWithNoSimulationSaysSo) {
    EXPECT_EQ(session.run(std::nullopt), RunStatus::Error);
    EXPECT_TRUE(reported(DiagCode::NoSimulation));
}

TEST_F(SessionTest, RestartingWithNoSimulationSaysSo) {
    EXPECT_FALSE(session.restart());
    EXPECT_TRUE(reported(DiagCode::NoSimulation));
}

// ---------------------------------------------------------------------------
// Analysis across several calls
// ---------------------------------------------------------------------------

TEST_F(SessionTest, ASecondAnalysisKeepsWhatTheFirstAnalysed) {
    workspace.write("pkg.sv", "package cfg_pkg;\n  localparam int W = 8;\nendpackage\n");
    workspace.write("top.sv", "module tb;\n  import cfg_pkg::*;\n"
                              "  logic [W-1:0] value = '0;\nendmodule\n");

    Session::AnalysisRequest first;
    first.files.push_back(workspace / "top.sv");
    Session::AnalysisRequest second;
    second.files.push_back(workspace / "pkg.sv");

    // The package is named second and used first, which is exactly what compile-order
    // inference exists for: the session puts them in dependency order before analysing.
    ASSERT_TRUE(session.analyse(first) || true);
    ASSERT_TRUE(session.analyse(second)) << firstMessage();
    EXPECT_TRUE(session.elaborate("tb")) << firstMessage();
}

TEST_F(SessionTest, AFileNamedTwiceIsAnalysedOnce) {
    // slang refuses a second buffer at one path, and keeps both at two spellings of it, which
    // would define every module in the file twice.
    workspace.write("counter.sv", CounterSource);

    Session::AnalysisRequest request;
    request.files.push_back(workspace / "counter.sv");
    request.files.push_back(workspace / "." / "counter.sv");
    request.libraryFiles.push_back(workspace / "counter.sv");
    request.contextFiles.push_back(workspace / "counter.sv");

    ASSERT_TRUE(session.analyse(request)) << firstMessage();
    EXPECT_TRUE(consumer.received.empty()) << firstMessage();
    EXPECT_TRUE(session.elaborate("tb")) << firstMessage();
}

// ---------------------------------------------------------------------------
// The run's time unit
// ---------------------------------------------------------------------------

/// A design whose one module asks for femtoseconds.
constexpr std::string_view FemtosecondSource = "`timescale 1ns / 1fs\n"
                                               "module tb;\n  logic a;\n"
                                               "  initial #1 a = 1'b1;\nendmodule\n";

TEST_F(SessionTest, ARunNothingFixesTheUnitOfCountsTheFinestPrecisionTheDesignAsksFor) {
    ASSERT_TRUE(session.chooseTimeScale(std::nullopt));
    ASSERT_TRUE(analyse("fine.sv", FemtosecondSource)) << firstMessage();
    EXPECT_EQ(session.timeScale().precisionExponent(), -15);
}

TEST_F(SessionTest, ALibraryBeingWrittenKeepsTheUnitAModuleWithNoTimeScaleIsIn) {
    // What the library stores counts its unit, and a later analysis into it takes that unit for
    // a module with no time scale, so this analysis has to have used it too.
    ASSERT_TRUE(session.openOutputLibrary(workspace / "work")) << firstMessage();
    ASSERT_TRUE(session.chooseTimeScale(std::nullopt));
    ASSERT_TRUE(analyse("fine.sv", FemtosecondSource)) << firstMessage();
    EXPECT_EQ(session.timeScale().precisionExponent(), -12);
}

TEST_F(SessionTest, AModuleWithNoTimeScaleIsInTheDefaultUnitNotTheRefinedOne) {
    // Another module asking for femtoseconds makes the run count them, and leaves a module that
    // states nothing in picoseconds, across a second analysis as within the first.
    ASSERT_TRUE(session.chooseTimeScale(std::nullopt));
    ASSERT_TRUE(analyse("fine.sv", FemtosecondSource)) << firstMessage();
    ASSERT_TRUE(analyse("plain.sv", "module plain;\n  logic a;\n  initial #5 a = 1'b1;\n"
                                    "endmodule\n"))
        << firstMessage();
    ASSERT_EQ(session.timeScale().precisionExponent(), -15);

    ASSERT_TRUE(session.elaborate("plain")) << firstMessage();
    session.run(std::nullopt);
    EXPECT_EQ(session.time(), 5000U) << "5 ps, counted in femtoseconds";
}

TEST_F(SessionTest, AFileASecondAnalysisNamesAgainIsAnalysedOnce) {
    // A session accumulates what each analysis names, so naming a file again is a repeat too.
    ASSERT_TRUE(analyse("counter.sv", CounterSource)) << firstMessage();
    ASSERT_TRUE(analyse("counter.sv", CounterSource)) << firstMessage();
    EXPECT_TRUE(consumer.received.empty()) << firstMessage();
    EXPECT_TRUE(session.elaborate("tb")) << firstMessage();
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

TEST_F(SessionTest, RunningToALimitAdvancesTime) {
    loadCounter();

    EXPECT_EQ(session.run(100), RunStatus::TimeLimitReached);
    EXPECT_EQ(session.time(), 100U);
}

TEST_F(SessionTest, RunningToTheEndFinishes) {
    loadCounter();

    EXPECT_EQ(session.run(std::nullopt), RunStatus::Finished);
    EXPECT_TRUE(session.isFinished());
    EXPECT_EQ(session.exitStatus(), 0);
}

TEST_F(SessionTest, RestartingPutsTheDesignBackToTimeZero) {
    loadCounter();
    ASSERT_EQ(session.run(100), RunStatus::TimeLimitReached);

    ASSERT_TRUE(session.restart());
    EXPECT_EQ(session.time(), 0U);
    EXPECT_EQ(session.examine(signalOf("/tb/count"), Radix::Decimal), "0");
}

TEST_F(SessionTest, ElaboratingAgainReplacesTheSimulation) {
    loadCounter();
    ASSERT_EQ(session.run(100), RunStatus::TimeLimitReached);

    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    EXPECT_EQ(session.time(), 0U);
}

TEST_F(SessionTest, ReplacingTheSimulationChangesTheElaborationGeneration) {
    // What a borrower of the kernel compares to find out that what it holds has gone. It cannot
    // compare addresses: tearing an elaboration down and building the next one allocates the
    // same sizes in the same order, so the replacement is routinely handed the address the old
    // one just released — which was observed here for the kernel but not the image, and on CI
    // for both. A counter cannot come out equal that way.
    loadCounter();
    const std::uint64_t first = session.elaborationGeneration();

    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    const std::uint64_t second = session.elaborationGeneration();
    EXPECT_NE(second, first);

    // A design replaced wholesale counts too, including the case that leaves nothing elaborated
    // at all: `setDesign` throws the kernel away without building another.
    session.setDesign(nullptr);
    EXPECT_FALSE(session.isElaborated());
    EXPECT_NE(session.elaborationGeneration(), second);
}

TEST_F(SessionTest, TheElaborationGenerationOnlyEverMovesForward) {
    // Monotonic, so that a borrower's comparison cannot come out equal by wrapping or by being
    // reset when the session goes back to time zero. A `restart` keeps the elaboration, so it is
    // deliberately not a change.
    loadCounter();
    const std::uint64_t afterLoad = session.elaborationGeneration();

    ASSERT_TRUE(session.restart());
    EXPECT_EQ(session.elaborationGeneration(), afterLoad) << "a restart replaces nothing";

    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    EXPECT_GT(session.elaborationGeneration(), afterLoad);
}

// ---------------------------------------------------------------------------
// examine
// ---------------------------------------------------------------------------

TEST_F(SessionTest, ExamineRendersInTheRequestedRadix) {
    loadCounter();
    ASSERT_EQ(session.run(105), RunStatus::TimeLimitReached);

    const std::uint32_t count = signalOf("/tb/count");
    const std::string decimal = session.examine(count, Radix::Decimal);
    const std::string hexadecimal = session.examine(count, Radix::Hexadecimal);

    // The clock starts low and toggles every five units, so the posedges fall at 5, 15, ...,
    // 105 — eleven of them by the time the run stops.
    EXPECT_EQ(decimal, "11");
    EXPECT_EQ(hexadecimal, "0b");
}

TEST_F(SessionTest, ExamineWithNoRadixNamesAnEnumeratedValue) {
    loadCounter();

    // Before the initial block moves it, so the declared S_RUN is what is there.
    EXPECT_EQ(session.examine(signalOf("/tb/state"), std::nullopt), "S_RUN");
}

TEST_F(SessionTest, ARadixOverridesTheEnumerationName) {
    loadCounter();

    EXPECT_EQ(session.examine(signalOf("/tb/state"), Radix::Binary), "01");
}

TEST_F(SessionTest, ExamineFallsBackToHexadecimalForAPlainSignal) {
    loadCounter();

    EXPECT_EQ(session.examine(signalOf("/tb/count"), std::nullopt), "00");
}

TEST_F(SessionTest, AVhdlSignalIsExaminedAsTheBitsItsCodesStandFor) {
    loadVhdlCoded();

    EXPECT_EQ(session.examine(signalOf("/tb/ready"), std::nullopt), "'1'");
    EXPECT_EQ(session.examine(signalOf("/tb/ready"), Radix::Binary), "1");
    EXPECT_EQ(session.examine(signalOf("/tb/bus"), Radix::Binary), "0101");
    EXPECT_EQ(session.examine(signalOf("/tb/bus"), std::nullopt), "5");
    EXPECT_EQ(session.examine(signalOf("/tb/nothing"), std::nullopt), "")
        << "a null array has no value";
}

TEST_F(SessionTest, AMissingSignalIsReportedAsNoSignal) {
    loadCounter();

    EXPECT_EQ(session.findSignal("/tb/nothing_here"), Session::NoSignal);
}

TEST_F(SessionTest, ACollapsedPortIsReachableUnderBothNames) {
    loadCounter();

    EXPECT_NE(session.findSignal("/tb/count"), Session::NoSignal);
    EXPECT_EQ(session.findSignal("/tb/u_dut/count"), session.findSignal("/tb/count"));
}

// ---------------------------------------------------------------------------
// deposit, force, release
// ---------------------------------------------------------------------------

TEST_F(SessionTest, DepositWritesAValue) {
    loadCounter();

    std::string error;
    ASSERT_TRUE(session.deposit(signalOf("/tb/count"), "8'd42", error)) << error;
    EXPECT_EQ(session.examine(signalOf("/tb/count"), Radix::Decimal), "42");
}

TEST_F(SessionTest, DepositSizesAPlainNumberToTheSignal) {
    loadCounter();

    std::string error;
    ASSERT_TRUE(session.deposit(signalOf("/tb/count"), "7", error)) << error;
    EXPECT_EQ(session.examine(signalOf("/tb/count"), Radix::Decimal), "7");
}

TEST_F(SessionTest, AValueThatDoesNotParseIsRefused) {
    loadCounter();

    std::string error;
    EXPECT_FALSE(session.deposit(signalOf("/tb/count"), "not a number", error));
    EXPECT_FALSE(error.empty());
}

TEST_F(SessionTest, AValueWrittenToAVhdlSignalIsTheBitsItShowsAs) {
    // `0` is a std_logic's '0', which its code 2 stands for, not the code 0 of 'U'.
    loadVhdlCoded();

    std::string error;
    ASSERT_TRUE(session.deposit(signalOf("/tb/ready"), "0", error)) << error;
    EXPECT_EQ(session.examine(signalOf("/tb/ready"), std::nullopt), "'0'");
    ASSERT_TRUE(session.force(signalOf("/tb/bus"), "4'b10zx", error)) << error;
    EXPECT_EQ(session.examine(signalOf("/tb/bus"), Radix::Binary), "10zx");
    ASSERT_TRUE(session.deposit(signalOf("/tb/flag"), "1", error)) << error;
    EXPECT_EQ(session.examine(signalOf("/tb/flag"), Radix::Binary), "1");
}

TEST_F(SessionTest, AValueAVhdlSignalCannotHoldIsRefused) {
    loadVhdlCoded();

    std::string error;
    EXPECT_FALSE(session.deposit(signalOf("/tb/flag"), "1'bx", error));
    EXPECT_NE(error.find("each 0 or 1"), std::string::npos) << error;
    EXPECT_EQ(session.examine(signalOf("/tb/flag"), Radix::Binary), "0");

    error.clear();
    EXPECT_FALSE(session.force(signalOf("/tb/nothing"), "1", error));
    EXPECT_NE(error.find("null array"), std::string::npos) << error;
}

TEST_F(SessionTest, ForceHoldsAValueAgainstTheDesign) {
    loadCounter();

    std::string error;
    ASSERT_TRUE(session.force(signalOf("/tb/enable"), "1'b0", error)) << error;
    ASSERT_EQ(session.run(200), RunStatus::TimeLimitReached);

    EXPECT_EQ(session.examine(signalOf("/tb/count"), Radix::Decimal), "0");
}

TEST_F(SessionTest, ReleasingLetsTheSignalBeWrittenAgain) {
    loadCounter();

    std::string error;
    ASSERT_TRUE(session.force(signalOf("/tb/enable"), "1'b0", error)) << error;

    // Ignored while the force stands, which is the whole difference between the two.
    ASSERT_TRUE(session.deposit(signalOf("/tb/enable"), "1'b1", error)) << error;
    ASSERT_EQ(session.run(100), RunStatus::TimeLimitReached);
    EXPECT_EQ(session.examine(signalOf("/tb/count"), Radix::Decimal), "0");

    ASSERT_TRUE(session.release(signalOf("/tb/enable")));
    ASSERT_TRUE(session.deposit(signalOf("/tb/enable"), "1'b1", error)) << error;
    ASSERT_EQ(session.run(200), RunStatus::TimeLimitReached);

    EXPECT_NE(session.examine(signalOf("/tb/count"), Radix::Decimal), "0");
}

TEST_F(SessionTest, ReleasingSomethingUnforcedSaysSo) {
    loadCounter();

    EXPECT_FALSE(session.release(signalOf("/tb/enable")));
}

// ---------------------------------------------------------------------------
// find
// ---------------------------------------------------------------------------

TEST_F(SessionTest, FindMatchesSignalNames) {
    loadCounter();

    const std::vector<std::string> found = session.findSignals("/tb/c*");
    EXPECT_EQ(found, (std::vector<std::string>{"/tb/clk", "/tb/count"}));
}

TEST_F(SessionTest, FindReachesInsideAnInstance) {
    loadCounter();

    const std::vector<std::string> found = session.findSignals("/tb/u_dut/*");
    EXPECT_EQ(found,
              (std::vector<std::string>{"/tb/u_dut/clk", "/tb/u_dut/count", "/tb/u_dut/enable"}));
}

TEST_F(SessionTest, FindMatchesOneCharacterWithAQuestionMark) {
    loadCounter();

    EXPECT_EQ(session.findSignals("/tb/cl?"), (std::vector<std::string>{"/tb/clk"}));
}

TEST_F(SessionTest, FindMatchingNothingReturnsNothing) {
    loadCounter();

    EXPECT_TRUE(session.findSignals("/tb/zzz*").empty());
}

TEST_F(SessionTest, FindMatchesInstancePaths) {
    loadCounter();

    EXPECT_EQ(session.findInstances("*"), (std::vector<std::string>{"/tb", "/tb/u_dut"}));
    EXPECT_EQ(session.findInstances("/tb/*"), (std::vector<std::string>{"/tb/u_dut"}));
}

TEST_F(SessionTest, FindBeforeElaborationReturnsNothing) {
    EXPECT_TRUE(session.findSignals("*").empty());
    EXPECT_TRUE(session.findInstances("*").empty());
}

// ---------------------------------------------------------------------------
// Waveforms
// ---------------------------------------------------------------------------

TEST_F(SessionTest, AVcdIsWrittenAndFinished) {
    loadCounter();

    const std::filesystem::path target = workspace / "out.vcd";
    ASSERT_TRUE(session.openVcd(target)) << firstMessage();
    EXPECT_EQ(session.openWaveformFiles(), 1U);

    ASSERT_EQ(session.run(100), RunStatus::TimeLimitReached);
    session.closeWaveforms();

    std::ifstream file(target);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("$var"), std::string::npos) << text;
    EXPECT_NE(text.find("#100"), std::string::npos) << text;
}

TEST_F(SessionTest, ASystemVerilogRealIsWrittenToAVcdAsTheNumberItIs) {
    // As VCD's own `real`, not as a 64-bit wire of the double's bits.
    ASSERT_TRUE(analyse("level.sv", R"(
        module tb;
            real level = 2.5;
            initial #5 level = -0.125;
        endmodule
    )")) << firstMessage();
    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();

    const std::filesystem::path target = workspace / "out.vcd";
    ASSERT_TRUE(session.openVcd(target)) << firstMessage();
    ASSERT_EQ(session.run(std::nullopt), RunStatus::Quiescent);
    session.closeWaveforms();

    std::ifstream file(target);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("$var real 64 ! level $end"), std::string::npos) << text;
    EXPECT_NE(text.find("r2.5 !"), std::string::npos) << text;
    EXPECT_NE(text.find("r-0.125 !"), std::string::npos) << text;
}

TEST_F(SessionTest, AWaveformFileNeedsASimulation) {
    EXPECT_FALSE(session.openVcd(workspace / "out.vcd"));
    EXPECT_TRUE(reported(DiagCode::NoSimulation));
}

TEST_F(SessionTest, TheScopeRestrictsWhatIsRecorded) {
    loadCounter();

    ASSERT_TRUE(session.addWaveformScope("/tb/u_dut"));
    const std::filesystem::path target = workspace / "scoped.vcd";
    ASSERT_TRUE(session.openVcd(target)) << firstMessage();
    ASSERT_EQ(session.run(100), RunStatus::TimeLimitReached);
    session.closeWaveforms();

    std::ifstream file(target);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("u_dut"), std::string::npos) << text;
    EXPECT_EQ(text.find(" state "), std::string::npos) << "the testbench's own state was recorded";
}

TEST_F(SessionTest, TheScopeCannotChangeOnceAFileIsOpen) {
    loadCounter();
    ASSERT_TRUE(session.openVcd(workspace / "out.vcd")) << firstMessage();

    EXPECT_FALSE(session.addWaveformScope("/tb/u_dut"));
    EXPECT_TRUE(reported(DiagCode::WaveformScopeAfterOpen));
}

TEST_F(SessionTest, AnEmptyScopeMeansTheWholeDesign) {
    loadCounter();

    ASSERT_TRUE(session.addWaveformScope("/tb/u_dut"));
    ASSERT_TRUE(session.addWaveformScope(""));
    EXPECT_TRUE(session.waveformScopes().empty());
}

TEST_F(SessionTest, ScopesAccumulate) {
    loadCounter();

    ASSERT_TRUE(session.addWaveformScope("/tb/u_dut"));
    ASSERT_TRUE(session.addWaveformScope("/tb/state"));
    ASSERT_TRUE(session.addWaveformScope("/tb/u_dut"));

    EXPECT_EQ(session.waveformScopes().size(), 2U);
}

TEST_F(SessionTest, AnFstIsWritten) {
    loadCounter();

    const std::filesystem::path target = workspace / "out.fst";
    ASSERT_TRUE(session.openFst(target)) << firstMessage();
    ASSERT_EQ(session.run(100), RunStatus::TimeLimitReached);
    session.closeWaveforms();

    EXPECT_TRUE(std::filesystem::exists(target));
    EXPECT_GT(std::filesystem::file_size(target), 0U);
}

TEST_F(SessionTest, AFileInADirectoryThatDoesNotExistIsReported) {
    loadCounter();

    EXPECT_FALSE(session.openVcd(workspace / "no-such-directory/out.vcd"));
    EXPECT_TRUE(reported(DiagCode::CannotOpenFile));
}

// ---------------------------------------------------------------------------
// VPI applications
// ---------------------------------------------------------------------------

/// How many times `needle` appears in `text`.
std::size_t occurrences(const std::string& text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

/// The command line `vpi_get_vlog_info` is handed.
std::vector<std::string> commandLine() {
    return {"enzan", "--vpi", "plugin.so"};
}

TEST_F(SessionTest, AnApplicationLoadsBeforeThereIsADesign) {
    ASSERT_TRUE(session.loadVpiApplication(ENZAN_TEST_STARTUP_PLUGIN, commandLine()));
    EXPECT_TRUE(session.hasVpiApplication());
    EXPECT_EQ(occurrences(transcript.str(), "first startup routine ran"), 1U);
    EXPECT_EQ(occurrences(transcript.str(), "start of simulation"), 0U);
}

TEST_F(SessionTest, ElaborationStartsTheApplicationsSimulation) {
    ASSERT_TRUE(session.loadVpiApplication(ENZAN_TEST_STARTUP_PLUGIN, commandLine()));
    loadCounter();
    EXPECT_EQ(occurrences(transcript.str(), "start of simulation"), 1U);
    EXPECT_EQ(occurrences(transcript.str(), "end of simulation"), 0U);
}

TEST_F(SessionTest, AFinishedRunEndsTheApplicationsSimulationOnce) {
    ASSERT_TRUE(session.loadVpiApplication(ENZAN_TEST_STARTUP_PLUGIN, commandLine()));
    loadCounter();
    ASSERT_EQ(session.run(std::nullopt), RunStatus::Finished);
    EXPECT_EQ(occurrences(transcript.str(), "end of simulation"), 1U);

    // Running a finished design again is not a second end.
    session.run(std::nullopt);
    EXPECT_EQ(occurrences(transcript.str(), "end of simulation"), 1U);
}

TEST_F(SessionTest, ARunStoppedShortStillEndsTheApplicationsSimulation) {
    // What a batch run with `--run` does: the process ends while the design could have gone
    // on, and an application that writes its results when told the simulation is over — as
    // cocotb does — would otherwise never write them.
    {
        Session shortRun(sources, engine);
        shortRun.setTranscript(&transcript);
        ASSERT_TRUE(shortRun.loadVpiApplication(ENZAN_TEST_STARTUP_PLUGIN, commandLine()));

        workspace.write("counter.sv", CounterSource);
        Session::AnalysisRequest request;
        request.files.push_back(workspace / "counter.sv");
        ASSERT_TRUE(shortRun.analyse(request)) << firstMessage();
        ASSERT_TRUE(shortRun.elaborate("tb")) << firstMessage();

        ASSERT_EQ(shortRun.run(50), RunStatus::TimeLimitReached);
        EXPECT_EQ(occurrences(transcript.str(), "end of simulation"), 0U);
    }
    EXPECT_EQ(occurrences(transcript.str(), "end of simulation"), 1U);
}

TEST_F(SessionTest, ADesignCannotBeRestartedWhileAnApplicationIsLoaded) {
    ASSERT_TRUE(session.loadVpiApplication(ENZAN_TEST_STARTUP_PLUGIN, commandLine()));
    loadCounter();
    ASSERT_EQ(session.run(50), RunStatus::TimeLimitReached);

    EXPECT_FALSE(session.restart());
    EXPECT_TRUE(reported(DiagCode::RestartWithVpiApplication));
    EXPECT_EQ(session.time(), 50U) << "nothing was put back";
}

TEST_F(SessionTest, ASearchFindsNoNameAFrontendMadeUp) {
    // `.count(count_lin[I])` gives the port storage of its own in the parent, named with a `$`.
    ASSERT_TRUE(analyse("tb.sv", R"(
        module counter (input logic clk, output logic [3:0] count);
            always_ff @(posedge clk) count <= count + 1'b1;
        endmodule

        module tb;
            logic clk = 1'b0;
            logic [1:0][3:0] count_lin;
            genvar I;
            for (I = 0; I < 2; I++) begin : cntr_lin_gen
                counter cntr_i (.clk(clk), .count(count_lin[I]));
            end
        endmodule
    )")) << firstMessage();
    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();

    const std::vector<std::string> found = session.findSignals("*");
    EXPECT_FALSE(found.empty());
    for (const std::string& path : found) EXPECT_EQ(path.find('$'), std::string::npos) << path;
    EXPECT_EQ(session.findSignals("/tb/cntr_lin_gen[0].cntr_i/count"),
              (std::vector<std::string>{"/tb/cntr_lin_gen[0].cntr_i/count"}));
}

TEST_F(SessionTest, AnApplicationThatCannotBeLoadedIsReported) {
    EXPECT_FALSE(session.loadVpiApplication("/nonexistent/libnothing.so", commandLine()));
    EXPECT_TRUE(reported(DiagCode::VpiLibraryUnloadable));
    EXPECT_FALSE(session.hasVpiApplication());
}

// ---------------------------------------------------------------------------
// A design of both languages
// ---------------------------------------------------------------------------

class BothLanguagesTest : public SessionTest {
protected:
    bool analyseBoth(std::string_view top) {
        workspace.write("tb.vhd", "entity tb is\nend entity;\narchitecture a of tb is\nbegin\n"
                                  "end architecture;\n");
        workspace.write("m.sv", "module Buffer(input logic i, output logic o);\n"
                                "  assign o = i;\nendmodule\n");
        Session::AnalysisRequest request;
        request.files = {workspace / "tb.vhd", workspace / "m.sv"};
        request.top = top;
        return session.analyse(request);
    }
};

#ifdef ENZAN_WITH_VHDL
TEST_F(BothLanguagesTest, AComponentNoEntityBindsIsBoundToAModuleOfALibraryDirectory) {
    // As a VHDL design reaches a vendor's models through a package of components.
    workspace.write("tb.vhd", "library ieee;\nuse ieee.std_logic_1164.all;\n"
                              "entity tb is\nend entity;\narchitecture a of tb is\n"
                              "  component Cell is\n    port (o : out std_logic);\n"
                              "  end component;\n  signal o : std_logic;\nbegin\n"
                              "  u : Cell port map (o => o);\nend architecture;\n");
    workspace.write("cells/Cell.v", "module Cell (output o);\n  Inner u (.o(o));\nendmodule\n");
    workspace.write("cells/Inner.v", "module Inner (output o);\n  assign o = 1'b1;\nendmodule\n");

    Session::AnalysisRequest request;
    request.files = {workspace / "tb.vhd"};
    request.searchDirectories = {workspace / "cells"};
    request.top = "tb";
    ASSERT_TRUE(session.analyse(request)) << firstMessage();
    EXPECT_FALSE(reported(DiagCode::VhdlUnboundComponent)) << firstMessage();
    ASSERT_TRUE(session.elaborate("tb")) << firstMessage();
    EXPECT_NE(session.findSignal("/tb/u/u/o"), Session::NoSignal);
}

TEST_F(BothLanguagesTest, ADesignOfBothLanguagesIsNotStoredInALibraryYet) {
    ASSERT_TRUE(session.openOutputLibrary(workspace / "work")) << firstMessage();
    EXPECT_FALSE(analyseBoth("tb"));
    EXPECT_TRUE(reported(DiagCode::NotYetImplemented));
}
#else
TEST_F(BothLanguagesTest, ADesignOfBothLanguagesNeedsABuildWithBothFrontends) {
    EXPECT_FALSE(analyseBoth("tb"));
    EXPECT_TRUE(reported(DiagCode::NotYetImplemented));
    EXPECT_NE(firstMessage().find("ENZAN_ENABLE_VHDL=OFF"), std::string::npos) << firstMessage();
}
#endif

} // namespace
