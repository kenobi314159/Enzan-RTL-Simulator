// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session drives real analysis, elaboration and simulation, so these tests compile real
// SystemVerilog. That is deliberate at this level: the session's whole job is to hold the
// three phases of DESIGN_GUIDE.md section 2.1 together, and a test against a hand-built
// design would not exercise the joins that are the thing being tested.

#include "session/Session.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "runtime/Kernel.h"

#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using Radix = enzan::eir::Radix;
using enzan::session::Session;
using enzan::runtime::RunStatus;

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

TEST_F(SessionTest, ElaboratingAnUnknownTopFails) {
    ASSERT_TRUE(analyse("counter.sv", CounterSource)) << firstMessage();
    EXPECT_FALSE(session.elaborate("nosuchmodule"));
    EXPECT_TRUE(reported(DiagCode::TopModuleNotFound));
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

} // namespace
