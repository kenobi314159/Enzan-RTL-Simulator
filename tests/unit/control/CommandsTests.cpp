// SPDX-License-Identifier: GPL-3.0-or-later
//
// The vocabulary of DESIGN_GUIDE.md section 8.1, driven the way a user drives it: through the
// interpreter, as script text. Testing the handlers directly would skip the argument
// marshalling and the result conversion, which is where most of what a command does lives.

#include "control/Commands.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "control/Interpreter.h"
#include "control/Session.h"

#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::control::EvalResult;
using enzan::control::EvalStatus;
using enzan::control::Interpreter;
using enzan::control::Session;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

/// A clock with nothing to stop it, for the tests that are about how long `run` runs for.
/// The counter design finishes at 200, which would end a run long before a limit measured in
/// nanoseconds on a picosecond design ever arrived.
constexpr std::string_view ClockSource = R"(
module ticker;
    logic clk = 1'b0;
    always #5 clk = ~clk;
endmodule
)";

constexpr std::string_view CounterSource = R"(
module tb;
    logic       clk = 1'b0;
    logic       enable = 1'b1;
    logic [7:0] count = 8'd0;

    always #5 clk = ~clk;

    always_ff @(posedge clk)
        if (enable)
            count <= count + 8'd1;

    initial begin
        #200;
        $finish;
    end
endmodule
)";

class CommandsTest : public ::testing::Test {
protected:
    CommandsTest() : engine(sources), session(sources, engine), interpreter(engine) {
        engine.addConsumer(consumer);
        session.setTranscript(&transcript);
        interpreter.setTranscript(&transcript);
        enzan::control::registerCommands(interpreter, session, sources);

        workspace.write("counter.sv", CounterSource);
    }

    EvalResult eval(const std::string& script) { return interpreter.evaluate(script); }

    /// Runs `script` and fails the test with the interpreter's message if it did not.
    std::string ok(const std::string& script) {
        const EvalResult result = eval(script);
        EXPECT_TRUE(result.ok()) << script << " -> " << result.value;
        return result.value;
    }

    /// Analyses and elaborates the shared design.
    void load() {
        ASSERT_TRUE(eval("vlog " + (workspace / "counter.sv").string()).ok());
        ASSERT_TRUE(eval("vsim tb").ok());
    }

    /// Analyses and elaborates the never-ending clock at the given time unit.
    void loadTicker(std::string_view unit) {
        workspace.write("ticker.sv", ClockSource);
        ASSERT_TRUE(eval("vlog " + (workspace / "ticker.sv").string()).ok());
        ASSERT_TRUE(eval("vsim -t " + std::string(unit) + " ticker").ok());
    }

    [[nodiscard]] std::string written() const { return transcript.str(); }

    enzan::test::Workspace workspace;
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    std::ostringstream transcript;
    Session session;
    Interpreter interpreter;
};

// ---------------------------------------------------------------------------
// The vocabulary exists
// ---------------------------------------------------------------------------

TEST_F(CommandsTest, EveryCommandInTheSpecificationIsRegistered) {
    // Section 8.1 names the vocabulary; a command that quietly went missing would only be
    // noticed by the user whose script stopped working.
    for (const char* name :
         {"vlog", "vcom", "vsim", "run", "restart", "add", "log", "force", "examine", "radix",
          "find", "quit", "do", "deposit", "noforce", "vcd", "fst"})
        EXPECT_TRUE(interpreter.hasCommand(name)) << name;
}

// ---------------------------------------------------------------------------
// vlog and vsim
// ---------------------------------------------------------------------------

TEST_F(CommandsTest, VlogAnalysesAndVsimElaborates) {
    load();
    EXPECT_TRUE(session.isElaborated());
    EXPECT_NE(written().find("# Loading tb"), std::string::npos) << written();
}

TEST_F(CommandsTest, VlogTakesDefinesAndIncludeDirectories) {
    workspace.write("include/sizes.svh", "`define WIDTH 4\n");
    workspace.write("defined.sv", "`include \"sizes.svh\"\n"
                                  "module tb;\n  logic [`WIDTH-1:0] value = '0;\nendmodule\n");

    ASSERT_TRUE(eval("vlog +incdir+" + (workspace / "include").string() + " +define+UNUSED=1 "
                     + (workspace / "defined.sv").string())
                    .ok());
    EXPECT_TRUE(eval("vsim tb").ok());
}

TEST_F(CommandsTest, VlogReadsAFileList) {
    workspace.write("design.f", (workspace / "counter.sv").string() + "\n");

    ASSERT_TRUE(eval("vlog -f " + (workspace / "design.f").string()).ok());
    EXPECT_TRUE(session.hasDesign());
}

TEST_F(CommandsTest, VlogRefusesAnOptionItDoesNotUnderstand) {
    const EvalResult result = eval("vlog -nologo x.sv");
    EXPECT_EQ(result.status, EvalStatus::Error);
    EXPECT_NE(result.value.find("-nologo"), std::string::npos) << result.value;
}

TEST_F(CommandsTest, VlogAcceptsMinusSv) {
    EXPECT_TRUE(eval("vlog -sv " + (workspace / "counter.sv").string()).ok());
}

TEST_F(CommandsTest, VcomIsRefusedRatherThanIgnored) {
    const EvalResult result = eval("vcom design.vhd");
    EXPECT_EQ(result.status, EvalStatus::Error);
    EXPECT_NE(result.value.find("M6"), std::string::npos) << result.value;
}

TEST_F(CommandsTest, VsimTakesATimeUnit) {
    ASSERT_TRUE(eval("vlog " + (workspace / "counter.sv").string()).ok());
    ASSERT_TRUE(eval("vsim -t ns tb").ok());

    EXPECT_EQ(session.timeScale().unitName(), "ns");
}

TEST_F(CommandsTest, VsimRefusesANonsenseTimeUnit) {
    ASSERT_TRUE(eval("vlog " + (workspace / "counter.sv").string()).ok());
    EXPECT_EQ(eval("vsim -t fortnights tb").status, EvalStatus::Error);
}

TEST_F(CommandsTest, VsimRefusesTwoTopLevels) {
    ASSERT_TRUE(eval("vlog " + (workspace / "counter.sv").string()).ok());
    EXPECT_EQ(eval("vsim tb other").status, EvalStatus::Error);
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

TEST_F(CommandsTest, RunNeedsASimulation) {
    const EvalResult result = eval("run 10");
    EXPECT_EQ(result.status, EvalStatus::Error);
    EXPECT_NE(result.value.find("vsim"), std::string::npos) << result.value;
}

TEST_F(CommandsTest, RunAdvancesByTheGivenAmount) {
    load();

    ASSERT_TRUE(ok("run 50").empty());
    EXPECT_EQ(session.time(), 50U);

    ASSERT_TRUE(ok("run 50").empty());
    EXPECT_EQ(session.time(), 100U) << "a second run continues rather than restarting";
}

TEST_F(CommandsTest, RunConvertsACoarserUnit) {
    loadTicker("ps");

    ok("run 2 ns");
    EXPECT_EQ(session.time(), 2000U);
}

TEST_F(CommandsTest, RunAcceptsTheUnitJoinedToTheNumber) {
    loadTicker("ps");

    ok("run 3ns");
    EXPECT_EQ(session.time(), 3000U);
}

TEST_F(CommandsTest, RunRefusesAUnitFinerThanThePrecision) {
    loadTicker("ns");

    // Turning `run 1ps` into `run 0` would make the script appear to run and do nothing,
    // which is worse than refusing it.
    const EvalResult result = eval("run 1 ps");
    EXPECT_EQ(result.status, EvalStatus::Error);
    EXPECT_NE(result.value.find("ns"), std::string::npos) << result.value;
}

TEST_F(CommandsTest, RunAllRunsToTheEnd) {
    load();

    ok("run -all");
    EXPECT_TRUE(session.isFinished());
    EXPECT_NE(written().find("$finish"), std::string::npos) << written();
}

TEST_F(CommandsTest, BareRunRunsToTheEnd) {
    load();

    ok("run");
    EXPECT_TRUE(session.isFinished());
}

TEST_F(CommandsTest, RunRefusesSomethingThatIsNotATime) {
    load();
    EXPECT_EQ(eval("run soon").status, EvalStatus::Error);
}

TEST_F(CommandsTest, RestartPutsTheDesignBack) {
    load();
    ok("run 50");
    ASSERT_EQ(session.time(), 50U);

    ok("restart");
    EXPECT_EQ(session.time(), 0U);
}

// ---------------------------------------------------------------------------
// examine, force, deposit
// ---------------------------------------------------------------------------

TEST_F(CommandsTest, ExamineReturnsAValueAScriptCanUse) {
    load();
    ok("run 105");

    // Posedges at 5, 15, ..., 105: eleven of them.
    EXPECT_EQ(ok("examine -radix decimal /tb/count"), "11");
    EXPECT_EQ(ok("expr {[examine -radix decimal /tb/count] * 2}"), "22");
}

TEST_F(CommandsTest, ExamineReturnsAListForSeveralSignals) {
    load();

    EXPECT_EQ(ok("llength [examine /tb/clk /tb/enable]"), "2");
}

TEST_F(CommandsTest, ExamineReportsASignalThatDoesNotExist) {
    load();

    const EvalResult result = eval("examine /tb/absent");
    EXPECT_EQ(result.status, EvalStatus::Error);
    EXPECT_NE(result.value.find("/tb/absent"), std::string::npos) << result.value;

    // A command error rather than a diagnostic: a script that probes for a signal can catch
    // this, and a diagnostic would make an otherwise clean run look failed.
    EXPECT_TRUE(consumer.received.empty());
}

TEST_F(CommandsTest, ExamineRefusesANonsenseRadix) {
    load();
    EXPECT_EQ(eval("examine -radix roman /tb/count").status, EvalStatus::Error);
}

TEST_F(CommandsTest, RadixSetsWhatExamineUsesByDefault) {
    load();
    ok("run 105");

    EXPECT_EQ(ok("radix decimal"), "decimal");
    EXPECT_EQ(ok("examine /tb/count"), "11");
    EXPECT_EQ(ok("radix"), "decimal");

    ok("radix -hex");
    EXPECT_EQ(ok("examine /tb/count"), "0b");
}

TEST_F(CommandsTest, RadixSymbolicGoesBackToNamingTheValue) {
    load();
    ok("radix decimal");

    EXPECT_EQ(ok("radix symbolic"), "symbolic");
    EXPECT_EQ(ok("radix"), "symbolic");
}

TEST_F(CommandsTest, DepositWritesAValue) {
    load();

    ok("deposit /tb/count 8'd42");
    EXPECT_EQ(ok("examine -radix decimal /tb/count"), "42");
}

TEST_F(CommandsTest, ForceHoldsAValueAndNoforceReleasesIt) {
    load();

    ok("force /tb/enable 1'b0");
    ok("deposit /tb/enable 1'b1");
    ok("run 100");
    EXPECT_EQ(ok("examine -radix decimal /tb/count"), "0") << "the deposit beat the force";

    ok("noforce /tb/enable");
    ok("deposit /tb/enable 1'b1");
    ok("run 190");
    EXPECT_NE(ok("examine -radix decimal /tb/count"), "0");
}

TEST_F(CommandsTest, NoforceOnSomethingUnforcedIsNotAnError) {
    load();
    EXPECT_TRUE(eval("noforce /tb/enable").ok());
}

TEST_F(CommandsTest, ForceRefusesAValueThatDoesNotParse) {
    load();
    EXPECT_EQ(eval("force /tb/count {not a value}").status, EvalStatus::Error);
}

TEST_F(CommandsTest, ForceWantsExactlyAPathAndAValue) {
    load();
    EXPECT_EQ(eval("force /tb/count").status, EvalStatus::Error);
    EXPECT_EQ(eval("force /tb/count 1 2").status, EvalStatus::Error);
}

// ---------------------------------------------------------------------------
// find
// ---------------------------------------------------------------------------

TEST_F(CommandsTest, FindReturnsATclList) {
    load();

    EXPECT_EQ(ok("llength [find signals /tb/c*]"), "2");
    EXPECT_EQ(ok("lindex [find signals /tb/c*] 0"), "/tb/clk");
}

TEST_F(CommandsTest, FindListsInstances) {
    load();
    EXPECT_EQ(ok("find instances *"), "/tb");
}

TEST_F(CommandsTest, FindRefusesAKindItDoesNotKnow) {
    load();
    EXPECT_EQ(eval("find widgets *").status, EvalStatus::Error);
}

// ---------------------------------------------------------------------------
// log, add wave, vcd, fst
// ---------------------------------------------------------------------------

TEST_F(CommandsTest, LogSelectsWhatIsRecorded) {
    load();

    ok("log -r /tb/count");
    EXPECT_EQ(session.waveformScopes().size(), 1U);
}

TEST_F(CommandsTest, LogEverythingClearsTheSelection) {
    load();

    ok("log /tb/count");
    ok("log -r /*");
    EXPECT_TRUE(session.waveformScopes().empty());
}

TEST_F(CommandsTest, AddWaveSelectsTheSameThingAsLog) {
    load();

    ok("add wave /tb/count");
    EXPECT_EQ(session.waveformScopes().size(), 1U);
}

TEST_F(CommandsTest, AddRefusesAnythingButWave) {
    load();
    EXPECT_EQ(eval("add list /tb/count").status, EvalStatus::Error);
}

TEST_F(CommandsTest, VcdFileWritesAFile) {
    load();

    ok("vcd file " + (workspace / "out.vcd").string());
    ok("run 100");
    session.closeWaveforms();

    EXPECT_TRUE(std::filesystem::exists(workspace / "out.vcd"));
}

TEST_F(CommandsTest, FstFileWritesAFile) {
    load();

    ok("fst file " + (workspace / "out.fst").string());
    ok("run 100");
    session.closeWaveforms();

    EXPECT_TRUE(std::filesystem::exists(workspace / "out.fst"));
}

TEST_F(CommandsTest, VcdWantsTheFileSubcommand) {
    load();
    EXPECT_EQ(eval("vcd add /tb/count").status, EvalStatus::Error);
}

// ---------------------------------------------------------------------------
// quit and do
// ---------------------------------------------------------------------------

TEST_F(CommandsTest, QuitStopsTheScriptAndRecordsItsStatus) {
    const EvalResult result = eval("quit -code 3\nputs {not reached}");

    EXPECT_EQ(result.status, EvalStatus::Stopped);
    EXPECT_TRUE(interpreter.stopRequested());
    EXPECT_EQ(interpreter.exitStatus(), 3);
    EXPECT_TRUE(written().empty());
}

TEST_F(CommandsTest, QuitDefaultsToSuccess) {
    ASSERT_EQ(eval("quit").status, EvalStatus::Stopped);
    EXPECT_EQ(interpreter.exitStatus(), 0);
}

TEST_F(CommandsTest, QuitAcceptsTheSwitchesAScriptCarries) {
    EXPECT_EQ(eval("quit -f -sim").status, EvalStatus::Stopped);
}

TEST_F(CommandsTest, QuitRefusesAnUnknownSwitch) {
    EXPECT_EQ(eval("quit -whenever").status, EvalStatus::Error);
}

TEST_F(CommandsTest, QuitFinishesTheWaveformFiles) {
    load();
    ok("vcd file " + (workspace / "out.vcd").string());
    ok("run 100");

    ASSERT_EQ(eval("quit").status, EvalStatus::Stopped);
    EXPECT_EQ(session.openWaveformFiles(), 0U);
}

TEST_F(CommandsTest, DoRunsAScript) {
    workspace.write("script.do",
                    "vlog " + (workspace / "counter.sv").string() + "\nvsim tb\nrun 50\n");

    ASSERT_TRUE(eval("do " + (workspace / "script.do").string()).ok());
    EXPECT_EQ(session.time(), 50U);
}

TEST_F(CommandsTest, DoPassesItsArguments) {
    workspace.write("args.do", "puts \"got $argc: $argv\"\n");

    ASSERT_TRUE(eval("do " + (workspace / "args.do").string() + " alpha beta").ok());
    EXPECT_NE(written().find("got 2: alpha beta"), std::string::npos) << written();
}

TEST_F(CommandsTest, DoReportsAFailingScript) {
    workspace.write("bad.do", "puts one\nnosuchcommand\n");

    EXPECT_EQ(eval("do " + (workspace / "bad.do").string()).status, EvalStatus::Error);
    EXPECT_FALSE(consumer.received.empty());
}

TEST_F(CommandsTest, AQuitInsideADoStopsTheOuterScript) {
    workspace.write("quitting.do", "quit -code 2\n");

    const EvalResult result =
        eval("do " + (workspace / "quitting.do").string() + "\nputs {not reached}");
    EXPECT_EQ(result.status, EvalStatus::Stopped);
    EXPECT_EQ(interpreter.exitStatus(), 2);
    EXPECT_TRUE(written().empty());
}

TEST_F(CommandsTest, DoNeedsAFile) {
    EXPECT_EQ(eval("do").status, EvalStatus::Error);
}

} // namespace
