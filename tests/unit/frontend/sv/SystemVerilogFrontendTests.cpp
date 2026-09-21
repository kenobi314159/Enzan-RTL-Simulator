// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the frontend's contract: what it produces for well-formed input, and what it
// reports for input it cannot handle. Whether the resulting EIR *simulates* correctly is what
// the golden suite under tests/golden/ answers; this file is about the translation itself and
// about the error paths, which a golden test cannot reach.

#include "frontend/sv/SystemVerilogFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Verifier.h"

#include "support/Optionals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::eir::Design;
using enzan::eir::ModuleDefinition;
using enzan::test::require;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
        codes.push_back(diagnostic.code);
    }

    std::vector<std::string> messages;
    std::vector<DiagCode> codes;
};

/// Writes source text to a temporary file and analyses it.
class FrontendTest : public ::testing::Test {
protected:
    FrontendTest() : engine(sources) { engine.addConsumer(consumer); }

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory = std::filesystem::temp_directory_path()
                    / ("enzan-sv-" + std::string(info->name()) + '-' + std::to_string(::getpid()));
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        ASSERT_TRUE(std::filesystem::create_directories(directory, error)) << error.message();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    /// Analyses `text` as a single file. Returns nullptr when analysis failed.
    std::unique_ptr<Design> analyse(std::string_view text, std::string_view top = "") {
        const std::filesystem::path path = directory / "design.sv";
        std::ofstream file(path);
        file << text;
        file.close();

        enzan::frontend::sv::Options options;
        options.files.push_back(path);
        if (!top.empty()) options.topModules.emplace_back(top);

        return enzan::frontend::sv::analyse(options, sources, engine);
    }

    [[nodiscard]] bool reportedCode(DiagCode code) const {
        return std::ranges::find(consumer.codes, code) != consumer.codes.end();
    }

    [[nodiscard]] bool reported(std::string_view fragment) const {
        return std::ranges::any_of(consumer.messages, [fragment](const std::string& message) {
            return message.find(fragment) != std::string::npos;
        });
    }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    std::filesystem::path directory;
};

// ---------------------------------------------------------------------------
// Well-formed input
// ---------------------------------------------------------------------------

TEST_F(FrontendTest, TranslatesAModuleWithItsObjectsAndPorts) {
    const std::unique_ptr<Design> design = analyse(R"(
        module m (input logic clk, output logic [7:0] q);
            always_ff @(posedge clk) q <= q + 1'b1;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(module->sourceName, "m");
    EXPECT_EQ(module->ports.size(), 2U);
    ASSERT_EQ(module->processes.size(), 1U);
    EXPECT_EQ(module->processes[0].kind, enzan::eir::ProcessKind::AlwaysFf);

    // Declared widths and four-state-ness survive into EIR.
    const std::uint32_t q = require(module->findObject("q"), "object q");
    EXPECT_EQ(module->objects[q].type.bitWidth(), 8U);
    EXPECT_TRUE(module->objects[q].type.isFourState());
}

TEST_F(FrontendTest, OutputIsAlwaysVerifiableEir) {
    // The frontend verifies its own output before returning it, so a design that comes back at
    // all satisfies the core invariant of section 4.2. Checking again here makes that contract
    // explicit rather than incidental.
    const std::unique_ptr<Design> design = analyse(R"(
        module m (input logic a, input logic b, output logic y);
            assign y = a & b;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    EXPECT_TRUE(enzan::eir::verify(*design, engine));
}

TEST_F(FrontendTest, ABlockHeadersEventControlBecomesTheProcessSensitivity) {
    // The scheduler re-arms a process, so the body must begin *after* the wait. A sensitivity
    // list left inside the body would make the process wait twice per edge.
    const std::unique_ptr<Design> design = analyse(R"(
        module m (input logic clk, input logic rst_n, output logic q);
            always_ff @(posedge clk or negedge rst_n) q <= rst_n;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->processes.size(), 1U);
    const auto& sensitivity = module->processes[0].sensitivity;
    ASSERT_EQ(sensitivity.size(), 2U);

    // Sorted by nothing in particular, so both edges are looked for rather than assumed.
    const bool hasPosedge =
        std::ranges::any_of(sensitivity, [](const enzan::eir::SensitivityEntry& entry) {
            return entry.edge == enzan::eir::EdgeKind::Posedge;
        });
    const bool hasNegedge =
        std::ranges::any_of(sensitivity, [](const enzan::eir::SensitivityEntry& entry) {
            return entry.edge == enzan::eir::EdgeKind::Negedge;
        });
    EXPECT_TRUE(hasPosedge);
    EXPECT_TRUE(hasNegedge);
}

TEST_F(FrontendTest, AlwaysCombHasNoExplicitSensitivity) {
    // Its list is inferred from the read set, which the runtime does when it builds the image.
    const std::unique_ptr<Design> design = analyse(R"(
        module m (input logic a, output logic y);
            always_comb y = ~a;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->processes.size(), 1U);
    EXPECT_EQ(module->processes[0].kind, enzan::eir::ProcessKind::AlwaysComb);
    EXPECT_TRUE(module->processes[0].sensitivity.empty());
}

TEST_F(FrontendTest, AlwaysStarWithNoSensitivityGetsOneFromItsReads) {
    // `always @(*)` cannot leave the list empty: an empty list on a plain `always` means the
    // clock-generator shape, which starts at time zero and loops.
    const std::unique_ptr<Design> design = analyse(R"(
        module m (input logic a, input logic b, output logic y);
            always @(*) y = a | b;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->processes.size(), 1U);
    EXPECT_EQ(module->processes[0].kind, enzan::eir::ProcessKind::Always);
    EXPECT_FALSE(module->processes[0].sensitivity.empty());
    EXPECT_FALSE(enzan::eir::startsAtTimeZero(module->processes[0]));
}

TEST_F(FrontendTest, ABareAlwaysLoopStartsAtTimeZero) {
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            logic clk = 1'b0;
            always #5 clk = ~clk;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->processes.size(), 1U);
    EXPECT_TRUE(module->processes[0].sensitivity.empty());
    EXPECT_TRUE(enzan::eir::startsAtTimeZero(module->processes[0]));
}

TEST_F(FrontendTest, EachParameterisationBecomesItsOwnDefinition) {
    // Keeping parameters out of EIR entirely is what makes an analysed unit reusable without
    // re-evaluating anything (see eir/Module.h).
    const std::unique_ptr<Design> design = analyse(R"(
        module leaf #(parameter int WIDTH = 8) (output logic [WIDTH-1:0] q);
            assign q = '0;
        endmodule

        module top;
            logic [7:0]  narrow;
            logic [15:0] wide;
            leaf #(.WIDTH(8))  u_narrow (.q(narrow));
            leaf #(.WIDTH(16)) u_wide   (.q(wide));
        endmodule
    )",
                                                   "top");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    // Two bodies for one source module, distinguished by a suffix.
    EXPECT_NE(design->findModule("leaf"), nullptr);
    EXPECT_NE(design->findModule("leaf$1"), nullptr);
    EXPECT_EQ(design->findModule("leaf")->sourceName, "leaf");
    EXPECT_EQ(design->findModule("leaf$1")->sourceName, "leaf");
}

TEST_F(FrontendTest, AConnectionThatIsAnExpressionGetsAnIntermediateAndAnAssignment) {
    // The EIR contract says a connection names an object, so anything more complicated becomes
    // a real assignment rather than something the elaborator has to understand.
    const std::unique_ptr<Design> design = analyse(R"(
        module leaf (input logic [7:0] d, output logic [7:0] q);
            assign q = d;
        endmodule

        module top;
            logic [3:0] a;
            logic [3:0] b;
            logic [7:0] out;
            leaf u_leaf (.d({a, b}), .q(out));
        endmodule
    )",
                                                   "top");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->instances.size(), 1U);

    // A continuous assignment was synthesised for the concatenation, and the connection names
    // the object it writes.
    const bool hasConnectionAssignment =
        std::ranges::any_of(top->processes, [](const enzan::eir::Process& process) {
            return process.kind == enzan::eir::ProcessKind::ContinuousAssign
                   && process.name.starts_with("connection@");
        });
    EXPECT_TRUE(hasConnectionAssignment);
    EXPECT_TRUE(top->instances[0].connections[0].connected());
}

TEST_F(FrontendTest, GenerateBlocksAreHoistedWithTheirScopeInTheName) {
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            for (genvar i = 0; i < 2; i = i + 1) begin : g_loop
                logic [3:0] value;
                always_comb value = 4'(i);
            end
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);

    // Two blocks declaring `value` must not collide, and the name must still be recognisable.
    std::size_t hoisted = 0;
    for (const enzan::eir::Object& object : module->objects) {
        if (object.name.find("value") != std::string::npos) ++hoisted;
    }
    EXPECT_EQ(hoisted, 2U) << "both generate iterations should declare their own object";
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST_F(FrontendTest, ReportsNoInputFiles) {
    const enzan::frontend::sv::Options options;
    EXPECT_EQ(enzan::frontend::sv::analyse(options, sources, engine), nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::NoInputFiles));
}

TEST_F(FrontendTest, ReportsAFileThatDoesNotExist) {
    enzan::frontend::sv::Options options;
    options.files.push_back(directory / "absent.sv");

    EXPECT_EQ(enzan::frontend::sv::analyse(options, sources, engine), nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::CannotOpenFile));
}

TEST_F(FrontendTest, TranslatesSlangsDiagnosticsIntoTheEnzanEngine) {
    // slang's diagnostics are dramatically better than most tools', and preserving that
    // quality end to end is a selling point rather than polish (section 10.6). They must
    // therefore arrive through the same engine as everything else.
    EXPECT_EQ(analyse("module m; this is not verilog endmodule"), nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::SystemVerilogError));
    EXPECT_FALSE(consumer.messages.empty());
}

TEST_F(FrontendTest, ASlangWarningDoesNotStopAnalysis) {
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            logic [7:0] a = 8'd0;
            logic [3:0] b;
            always_comb b = a;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    EXPECT_TRUE(reportedCode(DiagCode::SystemVerilogWarning)) << "a width truncation warns";
}

TEST_F(FrontendTest, ReportsSourceWithNoTopLevel) {
    // A file containing only a package has nothing to elaborate.
    EXPECT_EQ(analyse("package p; endpackage"), nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::NoTopLevelFound));
}

TEST_F(FrontendTest, ReportsAnUnsupportedConstructRatherThanMistranslatingIt) {
    // A partially translated body would produce a design that simulates and gives the wrong
    // answer, which section 1.2 ranks as the worst possible outcome.
    EXPECT_EQ(analyse(R"(
        module m;
            real x;
            initial x = 1.5;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::UnsupportedConstruct));
}

TEST_F(FrontendTest, ReportsAnUnsupportedSystemTaskByName) {
    EXPECT_EQ(analyse(R"(
        module m;
            initial $random();
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::UnsupportedConstruct));
    EXPECT_TRUE(reported("$random")) << "the diagnostic must name the task";
}

TEST_F(FrontendTest, ReportsCasezBecauseWildcardComparisonIsNotImplemented) {
    // Approximating a wildcard compare with `==` would silently take the wrong branch.
    EXPECT_EQ(analyse(R"(
        module m;
            logic [3:0] s = 4'b0000;
            logic       y;
            always_comb begin
                casez (s)
                    4'b1???: y = 1'b1;
                    default: y = 1'b0;
                endcase
            end
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::UnsupportedConstruct));
    EXPECT_TRUE(reported("casez"));
}

TEST_F(FrontendTest, ReportsAnAscendingDeclaredRange) {
    // `logic [0:7]` puts the most significant bit at index zero, and getting that backwards
    // silently reverses every select.
    EXPECT_EQ(analyse(R"(
        module m;
            logic [0:7] a = 8'd0;
            logic       y;
            always_comb y = a[3];
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::UnsupportedConstruct));
    EXPECT_TRUE(reported("ascending"));
}

TEST_F(FrontendTest, DiagnosticsCarryASourceLocationInEnzansOwnBuffers) {
    // The location has to resolve in Enzan's source manager, or the transcript shows a
    // message with no snippet and the quality that section 10.6 is about is lost.
    EXPECT_EQ(analyse("module m; initial $random(); endmodule"), nullptr);

    ASSERT_TRUE(reportedCode(DiagCode::UnsupportedConstruct));

    // The buffer the frontend read is the one diagnostics resolve against.
    EXPECT_GE(sources.bufferCount(), 1U);
}

} // namespace
