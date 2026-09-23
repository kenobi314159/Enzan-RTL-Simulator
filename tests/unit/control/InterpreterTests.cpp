// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/Interpreter.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using enzan::BufferId;
using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::control::CommandArguments;
using enzan::control::CommandResult;
using enzan::control::EvalResult;
using enzan::control::EvalStatus;
using enzan::control::Interpreter;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

class InterpreterTest : public ::testing::Test {
protected:
    InterpreterTest() : engine(sources), interpreter(engine) {
        engine.addConsumer(consumer);
        interpreter.setTranscript(&transcript);
    }

    [[nodiscard]] std::string written() const { return transcript.str(); }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    Interpreter interpreter;
    std::ostringstream transcript;
};

/// Writes `text` to a uniquely named file under the test's temporary directory.
class TemporaryScript {
public:
    explicit TemporaryScript(std::string_view text) {
        location = std::filesystem::temp_directory_path()
                   / ("enzan-interpreter-" + std::to_string(::getpid()) + "-"
                      + std::to_string(counter++) + ".tcl");
        std::ofstream file(location);
        file << text;
    }

    TemporaryScript(const TemporaryScript&) = delete;
    TemporaryScript& operator=(const TemporaryScript&) = delete;
    TemporaryScript(TemporaryScript&&) = delete;
    TemporaryScript& operator=(TemporaryScript&&) = delete;

    ~TemporaryScript() {
        std::error_code ignored;
        std::filesystem::remove(location, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return location; }

private:
    static inline int counter = 0;
    std::filesystem::path location;
};

// ---------------------------------------------------------------------------
// Coming up
// ---------------------------------------------------------------------------

TEST_F(InterpreterTest, StartsWithAWorkingInterpreter) {
    ASSERT_TRUE(interpreter.valid());

    const EvalResult result = interpreter.evaluate("expr {1 + 1}");
    EXPECT_EQ(result.status, EvalStatus::Ok);
    EXPECT_EQ(result.value, "2");
}

TEST_F(InterpreterTest, ReportsNothingWhenItStartsCleanly) {
    EXPECT_TRUE(consumer.received.empty());
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

TEST_F(InterpreterTest, RunsARegisteredCommand) {
    interpreter.registerCommand("greet", [](CommandArguments arguments) {
        std::string joined;
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            if (index > 1) joined += ' ';
            joined += arguments[index];
        }
        return CommandResult::ok("hello " + joined);
    });

    ASSERT_TRUE(interpreter.hasCommand("greet"));
    EXPECT_EQ(interpreter.evaluate("greet there you").value, "hello there you");
}

TEST_F(InterpreterTest, PassesTheInvokedNameAsTheFirstArgument) {
    std::string seen;
    interpreter.registerCommand("named", [&seen](CommandArguments arguments) {
        seen = arguments.empty() ? "" : std::string(arguments[0]);
        return CommandResult::ok();
    });

    ASSERT_TRUE(interpreter.evaluate("named").ok());
    EXPECT_EQ(seen, "named");
}

TEST_F(InterpreterTest, ACommandResultIsCapturedByTheScript) {
    interpreter.registerCommand("answer", [](CommandArguments) { return CommandResult::ok("42"); });

    EXPECT_EQ(interpreter.evaluate("set x [answer]; expr {$x * 2}").value, "84");
}

TEST_F(InterpreterTest, AFailedCommandBecomesAScriptError) {
    interpreter.registerCommand("refuse",
                                [](CommandArguments) { return CommandResult::error("not today"); });

    const EvalResult result = interpreter.evaluate("refuse");
    EXPECT_EQ(result.status, EvalStatus::Error);
    EXPECT_EQ(result.value, "not today");
    EXPECT_FALSE(result.errorInfo.empty());
}

TEST_F(InterpreterTest, AFailedCommandStopsTheRestOfTheScript) {
    bool reached = false;
    interpreter.registerCommand("refuse",
                                [](CommandArguments) { return CommandResult::error("not today"); });
    interpreter.registerCommand("mark", [&reached](CommandArguments) {
        reached = true;
        return CommandResult::ok();
    });

    EXPECT_FALSE(interpreter.evaluate("refuse\nmark").ok());
    EXPECT_FALSE(reached);
}

TEST_F(InterpreterTest, AStoppedCommandEndsTheScriptWithoutFailing) {
    bool reached = false;
    interpreter.registerCommand("bail", [](CommandArguments) { return CommandResult::stopped(); });
    interpreter.registerCommand("mark", [&reached](CommandArguments) {
        reached = true;
        return CommandResult::ok();
    });

    const EvalResult result = interpreter.evaluate("bail\nmark");
    EXPECT_EQ(result.status, EvalStatus::Stopped);
    EXPECT_TRUE(result.ok());
    EXPECT_FALSE(reached);
}

TEST_F(InterpreterTest, RegisteringTwiceReplacesTheCommand) {
    interpreter.registerCommand("which",
                                [](CommandArguments) { return CommandResult::ok("first"); });
    interpreter.registerCommand("which",
                                [](CommandArguments) { return CommandResult::ok("second"); });

    EXPECT_EQ(interpreter.evaluate("which").value, "second");
}

TEST_F(InterpreterTest, ReportsNoCommandThatWasNeverRegistered) {
    EXPECT_FALSE(interpreter.hasCommand("vsim"));
    EXPECT_FALSE(interpreter.evaluate("vsim").ok());
}

// ---------------------------------------------------------------------------
// The transcript
// ---------------------------------------------------------------------------

TEST_F(InterpreterTest, PutsWritesToTheTranscript) {
    ASSERT_TRUE(interpreter.evaluate("puts {time is now}").ok());
    EXPECT_EQ(written(), "time is now\n");
}

TEST_F(InterpreterTest, PutsHonoursNonewline) {
    ASSERT_TRUE(interpreter.evaluate("puts -nonewline a; puts b").ok());
    EXPECT_EQ(written(), "ab\n");
}

TEST_F(InterpreterTest, PutsToStderrAlsoReachesTheTranscript) {
    ASSERT_TRUE(interpreter.evaluate("puts stderr {something went wrong}").ok());
    EXPECT_EQ(written(), "something went wrong\n");
}

TEST_F(InterpreterTest, PutsToAFileStillWritesToTheFile) {
    const std::filesystem::path target = std::filesystem::temp_directory_path()
                                         / ("enzan-puts-" + std::to_string(::getpid()) + ".txt");

    const std::string script =
        "set f [open " + target.string() + " w]; puts $f {into the file}; close $f";
    ASSERT_TRUE(interpreter.evaluate(script).ok()) << interpreter.evaluate(script).value;

    std::ifstream file(target);
    std::string line;
    std::getline(file, line);
    EXPECT_EQ(line, "into the file");
    EXPECT_TRUE(written().empty());

    std::error_code ignored;
    std::filesystem::remove(target, ignored);
}

TEST_F(InterpreterTest, PutsWithTooManyArgumentsIsRefused) {
    EXPECT_FALSE(interpreter.evaluate("puts a b c d").ok());
}

// ---------------------------------------------------------------------------
// Variables and stopping
// ---------------------------------------------------------------------------

TEST_F(InterpreterTest, VariablesCrossBetweenCppAndTcl) {
    interpreter.setVariable("now", "150 ns");
    EXPECT_EQ(interpreter.evaluate("set now").value, "150 ns");

    ASSERT_TRUE(interpreter.evaluate("set ::answer 42").ok());
    EXPECT_EQ(interpreter.variable("answer"), "42");
}

TEST_F(InterpreterTest, AnUnsetVariableReadsAsEmpty) {
    EXPECT_EQ(interpreter.variable("nothing"), "");
}

TEST_F(InterpreterTest, StoppingIsRecordedWithItsStatus) {
    EXPECT_FALSE(interpreter.stopRequested());

    interpreter.registerCommand("quit", [this](CommandArguments) {
        interpreter.requestStop(3);
        return CommandResult::stopped();
    });

    EXPECT_EQ(interpreter.evaluate("quit").status, EvalStatus::Stopped);
    EXPECT_TRUE(interpreter.stopRequested());
    EXPECT_EQ(interpreter.exitStatus(), 3);
}

// ---------------------------------------------------------------------------
// Scripts from files
// ---------------------------------------------------------------------------

TEST_F(InterpreterTest, EvaluatesAScriptFromAFile) {
    const TemporaryScript script("puts {from a file}\n");

    EXPECT_TRUE(interpreter.evaluateFile(script.path(), sources).ok());
    EXPECT_EQ(written(), "from a file\n");
}

TEST_F(InterpreterTest, AFailureInAFileCarriesItsLine) {
    const TemporaryScript script("puts one\nputs two\nnosuchcommand\nputs four\n");

    const EvalResult result = interpreter.evaluateFile(script.path(), sources);
    ASSERT_FALSE(result.ok());
    ASSERT_TRUE(result.location.valid());
    EXPECT_EQ(sources.lineNumber(result.location), 3U);
}

TEST_F(InterpreterTest, AFailureInAFileIsReportedAgainstThatLine) {
    const TemporaryScript script("puts one\nnosuchcommand\n");

    interpreter.reportFailure(interpreter.evaluateFile(script.path(), sources));

    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().code, DiagCode::ScriptError);
    EXPECT_EQ(sources.lineNumber(consumer.received.front().location), 2U);
}

TEST_F(InterpreterTest, AMissingScriptFileIsAFileDiagnosticAndNotAScriptError) {
    const EvalResult result =
        interpreter.evaluateFile("/nonexistent/enzan/does-not-exist.tcl", sources);

    EXPECT_FALSE(result.ok());
    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().code, DiagCode::CannotOpenFile);
}

TEST_F(InterpreterTest, ASuccessfulEvaluationReportsNothing) {
    interpreter.reportFailure(interpreter.evaluate("expr {1 + 1}"));
    EXPECT_TRUE(consumer.received.empty());
}

TEST_F(InterpreterTest, AFailureInAStringHasNoLocation) {
    const EvalResult result = interpreter.evaluate("nosuchcommand");

    ASSERT_FALSE(result.ok());
    EXPECT_FALSE(result.location.valid());
}

TEST_F(InterpreterTest, EvaluatesTextAgainstABufferTheCallerAlreadyHas) {
    const std::string text = "puts a\nnosuchcommand\n";
    const BufferId buffer = sources.addBuffer("typed.tcl", text);

    const EvalResult result = interpreter.evaluateBuffer(buffer, text);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(sources.lineNumber(result.location), 2U);
}

} // namespace
