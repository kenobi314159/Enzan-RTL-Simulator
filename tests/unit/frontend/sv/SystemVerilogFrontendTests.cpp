// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the frontend's contract: what it produces for well-formed input, and what it
// reports for input it cannot handle. Whether the resulting EIR *simulates* correctly is what
// the golden suite under tests/golden/ answers; this file is about the translation itself and
// about the error paths, which a golden test cannot reach.

#include "frontend/sv/SystemVerilogFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Verifier.h"

#include "support/Optionals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
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
using enzan::eir::Instance;
using enzan::eir::ModuleDefinition;
using enzan::eir::WriteExtent;
using enzan::test::require;

/// Every process's extents of the object called `name`, one list per process that writes it,
/// in process order.
std::vector<std::vector<WriteExtent>> extentsOf(const ModuleDefinition& definition,
                                                std::string_view name) {
    const std::uint32_t objectIndex = require(definition.findObject(name));
    std::vector<std::vector<WriteExtent>> result;
    for (const enzan::eir::Process& process : definition.processes) {
        std::vector<WriteExtent> extents;
        std::ranges::copy_if(
            process.writeSet, std::back_inserter(extents),
            [objectIndex](const WriteExtent& extent) { return extent.objectIndex == objectIndex; });
        if (!extents.empty()) result.push_back(std::move(extents));
    }
    return result;
}

/// An extent of the object called `name` in `definition`.
WriteExtent extent(const ModuleDefinition& definition, std::string_view name, std::uint32_t lowBit,
                   std::uint32_t width) {
    return {.objectIndex = require(definition.findObject(name)), .lowBit = lowBit, .width = width};
}

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
        codes.push_back(diagnostic.code);
        received.push_back(diagnostic);
    }

    std::vector<std::string> messages;
    std::vector<DiagCode> codes;
    std::vector<Diagnostic> received;
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
    std::unique_ptr<Design> analyse(std::string_view text, std::string_view top = "",
                                    std::vector<std::string> parameters = {}) {
        const std::filesystem::path path = directory / "design.sv";
        std::ofstream file(path);
        file << text;
        file.close();

        enzan::frontend::sv::Options options;
        options.files.push_back(path);
        if (!top.empty()) options.topModules.emplace_back(top);
        options.parameterOverrides = std::move(parameters);
        options.timeUnitExponent = timeUnit;
        options.refineTimeUnit = refineTimeUnit;
        options.runTimeUnit = &runTimeUnit;
        options.fixedRunTimeUnit = fixedRunTimeUnit;
        options.requests = requests;
        options.requestAnswers = &answers;
        options.onlyRequested = onlyRequested;
        options.interfaces = interfaces;
        options.asked = &asked;
        options.unsettled = &unsettled;

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

    /// The unit a module with no time scale is in, whether a finer precision may replace it as
    /// the run's, and the run's unit the last analysis chose.
    std::int8_t timeUnit = -12;
    bool refineTimeUnit = false;
    std::int8_t runTimeUnit = 0;
    std::optional<std::int8_t> fixedRunTimeUnit;

    /// What another language's units ask for, whether they are the only tops, and the
    /// definitions the last analysis answered with.
    std::vector<enzan::eir::UnitRequest> requests;
    bool onlyRequested = false;
    std::vector<std::string> answers;

    /// The other language's units the sources instantiate, and what the last analysis asked of
    /// them.
    std::vector<enzan::eir::UnitInterface> interfaces;
    std::vector<enzan::eir::UnitRequest> asked;
    bool unsettled = false;
};

/// The values of the constants `process` loads.
std::vector<std::uint64_t> constantsOf(const enzan::eir::Process& process) {
    std::vector<std::uint64_t> values;
    values.reserve(process.constants.size());
    for (const enzan::eir::BitVector& constant : process.constants)
        values.push_back(constant.ref().toUInt64());
    return values;
}

/// True when `process` loads a constant of `value`.
bool loads(const enzan::eir::Process& process, std::uint64_t value) {
    const std::vector<std::uint64_t> values = constantsOf(process);
    return std::ranges::find(values, value) != values.end();
}

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

TEST_F(FrontendTest, AnEnumeratedObjectCarriesTheNamesOfItsValues) {
    // EIR does not model an enumeration as a type — every operation on one treats it as the
    // packed vector it is — but the names have to survive translation, because a reader
    // looking at a state register wants to be told `S_SEND` and nothing downstream of the
    // frontend could ever work that out (DESIGN_GUIDE.md section 10.7).
    const std::unique_ptr<Design> design = analyse(R"(
        module m (input logic clk);
            typedef enum logic [2:0] {
                S_IDLE = 3'd0,
                S_ARM  = 3'd1,
                S_SEND = 3'd4
            } state_t;

            state_t     state;
            logic [2:0] plain;

            always_ff @(posedge clk) state <= S_ARM;
        endmodule
    )");
    ASSERT_NE(design, nullptr);

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);

    const auto stateObject = require(module->findObject("state"));
    const std::uint32_t enumIndex = module->objects[stateObject].enumIndex;
    ASSERT_NE(enumIndex, enzan::eir::Object::NoEnum) << "an enumerated object has to point at one";
    ASSERT_LT(enumIndex, module->enums.size());

    const enzan::eir::EnumDefinition& enumeration = module->enums[enumIndex];
    EXPECT_EQ(enumeration.name, "state_t") << "the declared name, not the canonical type's";

    ASSERT_EQ(enumeration.members.size(), 3U);
    EXPECT_EQ(enumeration.members[0].name, "S_IDLE") << "declaration order, not value order";
    EXPECT_EQ(enumeration.members[2].name, "S_SEND");

    // The values are the enumeration's own base type, so that looking one up against a signal
    // of that type is a plain comparison rather than a width negotiation.
    EXPECT_EQ(enumeration.members[2].value.width(), 3U);
    EXPECT_EQ(enumeration.nameOf(enumeration.members[2].value), "S_SEND");
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_TRUE(enumeration.nameOf(*enzan::eir::BitVector::parse("3'd5")).empty())
        << "a value the enumeration does not name has no name";
    // NOLINTEND(bugprone-unchecked-optional-access)

    // And a plain vector of the same width is left alone: the mapping belongs to the type, not
    // to the width.
    const auto plainObject = require(module->findObject("plain"));
    EXPECT_EQ(module->objects[plainObject].enumIndex, enzan::eir::Object::NoEnum);
}

TEST_F(FrontendTest, OneEnumerationIsRecordedOnceHoweverManyObjectsHaveIt) {
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            typedef enum logic [1:0] { A = 2'd0, B = 2'd1 } pair_t;
            pair_t first;
            pair_t second;
            pair_t third;
        endmodule
    )");
    ASSERT_NE(design, nullptr);

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);

    EXPECT_EQ(module->enums.size(), 1U) << "three objects of one type share one table entry";

    const std::uint32_t first = module->objects[require(module->findObject("first"))].enumIndex;
    EXPECT_EQ(module->objects[require(module->findObject("second"))].enumIndex, first);
    EXPECT_EQ(module->objects[require(module->findObject("third"))].enumIndex, first);
}

TEST_F(FrontendTest, ParametersAreRecordedWithWhatTheyResolvedTo) {
    // A parameterised module is already one definition per parameterisation, so nothing here
    // is used to simulate anything. It is recorded so that a reader looking at one of three
    // differently-configured copies of a module can see which copy they are looking at
    // (DESIGN_GUIDE.md section 10.7).
    const std::unique_ptr<Design> design = analyse(R"(
        module leaf #(parameter int WIDTH = 8) ();
            localparam int BYTES = WIDTH / 8;
            logic [WIDTH-1:0] q;
        endmodule

        module m;
            leaf #(.WIDTH(16)) u_wide ();
            leaf #(.WIDTH(8))  u_narrow ();
        endmodule
    )",
                                                   "m");
    ASSERT_NE(design, nullptr);

    // Two parameterisations, two definitions, each carrying its own values.
    std::vector<int> widths;
    std::vector<int> byteCounts;
    for (const ModuleDefinition& module : design->modules()) {
        if (module.sourceName != "leaf") continue;

        for (const enzan::eir::Parameter& parameter : module.parameters) {
            const auto value = static_cast<int>(parameter.value.ref().toUInt64());

            if (parameter.name == "WIDTH") {
                EXPECT_FALSE(parameter.isLocal);
                widths.push_back(value);
            } else if (parameter.name == "BYTES") {
                EXPECT_TRUE(parameter.isLocal) << "a localparam is told apart from a parameter";
                byteCounts.push_back(value);
            }
        }
    }

    std::ranges::sort(widths);
    std::ranges::sort(byteCounts);
    EXPECT_EQ(widths, (std::vector<int>{8, 16}));
    EXPECT_EQ(byteCounts, (std::vector<int>{1, 2})) << "a localparam derived from the parameter";
}

TEST_F(FrontendTest, AParameterThatIsNotAnIntegerIsSkippedRatherThanRefused) {
    // Nothing is being translated, so nothing can be mistranslated. Refusing the design over a
    // `parameter string` that only names a log file would fail a design that simulates
    // perfectly well; the cost of leaving it out of a list is visible, which is the point.
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            parameter string NAME = "run";
            parameter int    SIZE = 4;
            logic q;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << "a string parameter must not fail the analysis";

    // Named for its parameter values, so it is found by the name the source gave it.
    const std::vector<std::uint32_t> found = design->findSpecialisations("m");
    ASSERT_EQ(found.size(), 1U);
    const ModuleDefinition* module = &design->module(found.front());

    ASSERT_EQ(module->parameters.size(), 1U);
    EXPECT_EQ(module->parameters[0].name, "SIZE");
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

    // Two bodies for one source module, each named for its own parameter values rather than
    // by a counter. The distinction matters beyond this design: a counter numbers the
    // parameterisations in the order one compilation walked them, so `leaf$1` from another
    // analysis would be a different module under the same name, and a design assembled from
    // two libraries (section 8.2) would bind an instance to the wrong body.
    const std::vector<std::uint32_t> leaves = design->findSpecialisations("leaf");
    ASSERT_EQ(leaves.size(), 2U);

    std::vector<std::string> names;
    for (const std::uint32_t index : leaves) {
        names.push_back(design->module(index).name);
        EXPECT_EQ(design->module(index).sourceName, "leaf");
    }
    std::ranges::sort(names);
    EXPECT_EQ(names, std::vector<std::string>({"leaf$WIDTH_16", "leaf$WIDTH_8"}))
        << "the suffix says which parameterisation, not which was seen first";

    // Named by their values, so the same parameterisation is the same name wherever it is
    // produced, which is what lets a library be instantiated by whoever uses it.
    EXPECT_NE(design->findModule("leaf$WIDTH_8"), nullptr);
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

    // Two blocks declaring `value` must not collide, and each is named for its iteration as the
    // standard names it, so that a reader can tell which is which.
    EXPECT_TRUE(module->findObject("g_loop[0].value").has_value());
    EXPECT_TRUE(module->findObject("g_loop[1].value").has_value());
}

TEST_F(FrontendTest, AnUnnamedGenerateBlockIsNamedAsTheStandardNamesIt) {
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            localparam int P = 1;
            if (P == 1) begin
                logic x;
            end
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(module->findObject("genblk1.x").has_value()) << "IEEE 1800 section 27.6";
}

TEST_F(FrontendTest, AnInstanceInAGenerateBlockHasTheBlockInItsName) {
    // Without the block in it, every iteration's instance has the same name and so the same
    // path, and nothing can tell them apart.
    const std::unique_ptr<Design> design = analyse(R"(
        module leaf (input logic a);
        endmodule
        module m;
            logic a;
            for (genvar i = 0; i < 2; i++) begin : g_loop
                leaf u_leaf (.a(a));
            end
            if (1) begin : g_if
                leaf u_leaf (.a(a));
            end
        endmodule
    )",
                                                   "m");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    std::vector<std::string> names;
    names.reserve(module->instances.size());
    for (const enzan::eir::Instance& instance : module->instances) names.push_back(instance.name);
    EXPECT_EQ(names,
              (std::vector<std::string>{"g_loop[0].u_leaf", "g_loop[1].u_leaf", "g_if.u_leaf"}));
}

// ---------------------------------------------------------------------------
// Write sets
// ---------------------------------------------------------------------------
//
// A write set records the bits a process writes, because a driver owns bits rather than a
// signal (DESIGN_GUIDE.md section 3.7). What these pin is which bits: elaboration decides
// whether two processes are in conflict by comparing them.

TEST_F(FrontendTest, AWholeAssignmentWritesEveryBit) {
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [7:0] a;
            logic [7:0] x;
            assign x = a;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    EXPECT_EQ(extentsOf(top, "x"),
              (std::vector<std::vector<WriteExtent>>{{extent(top, "x", 0, 8)}}));
}

TEST_F(FrontendTest, ANetRecordsTheKindItWasDeclaredAs) {
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            wire    a;
            wand    b;
            trior   c;
            tri1    d;
            supply0 e;
            uwire   f;
            logic   g;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);

    using enzan::eir::NetKind;
    const auto kindOf = [found](std::string_view name) {
        return found->objects[require(found->findObject(name))].netKind;
    };
    EXPECT_EQ(kindOf("a"), NetKind::Wire);
    EXPECT_EQ(kindOf("b"), NetKind::WiredAnd);
    EXPECT_EQ(kindOf("c"), NetKind::TriOr);
    EXPECT_EQ(kindOf("d"), NetKind::Tri1);
    EXPECT_EQ(kindOf("e"), NetKind::Supply0);
    EXPECT_EQ(kindOf("f"), NetKind::UWire);
    EXPECT_EQ(found->objects[require(found->findObject("g"))].kind,
              enzan::eir::ObjectKind::Variable);
}

TEST_F(FrontendTest, ANetWithNoResolutionTableIsReported) {
    // A trireg stores charge, which Enzan has no model of; treating it as a wire would lose
    // the stored value the moment its drivers let go.
    EXPECT_EQ(analyse(R"(
        module m;
            trireg t;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a 'trireg' net"));
}

TEST_F(FrontendTest, ANetDeclarationAssignmentIsAContinuousAssignment) {
    // `wire w = a & b;` drives the net for the whole run, not only at time zero.
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [3:0] a;
            logic [3:0] b;
            wire  [3:0] w = a & b;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    const std::uint32_t w = require(top.findObject("w"));
    EXPECT_FALSE(top.objects[w].initialValue.has_value());
    ASSERT_EQ(top.processes.size(), 1U);
    EXPECT_EQ(top.processes[0].kind, enzan::eir::ProcessKind::ContinuousAssign);
    EXPECT_EQ(extentsOf(top, "w"),
              (std::vector<std::vector<WriteExtent>>{{extent(top, "w", 0, 4)}}));
}

TEST_F(FrontendTest, ASubroutinesStaticVariableIsStorageEveryCallShares) {
    // One variable for every call, named behind its subroutine, starting from its initialiser
    // once; an automatic one stays each call's own. A package's subroutine keeps its static
    // variables in the package, which a module reaches by path.
    const std::unique_ptr<Design> design = analyse(R"(
        package p;
            function integer serial();
                static integer last = 7;
                last = last + 1;
                return last;
            endfunction
        endpackage
        module top;
            function integer next_id();
                static integer count = 5;
                count = count + 1;
                return count;
            endfunction
            function automatic integer fresh();
                integer n = 0;
                n = n + 1;
                return n;
            endfunction
            integer x, y, z;
            initial begin
                x = next_id();
                y = fresh();
                z = p::serial();
            end
        endmodule
    )",
                                                   "top");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);

    const enzan::eir::Object& count = top->objects[require(top->findObject("next_id.count"))];
    EXPECT_TRUE(count.subroutineStatic);
    ASSERT_TRUE(count.initialValue.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    EXPECT_EQ(count.initialValue->ref().toUInt64(), 5U);
    EXPECT_FALSE(top->findObject("n").has_value());
    EXPECT_FALSE(top->findObject("fresh.n").has_value());
    EXPECT_FALSE(top->objects[require(top->findObject("x"))].subroutineStatic);

    const auto reference = std::ranges::find(top->objects, std::string("/p/serial.last"),
                                             &enzan::eir::Object::hierarchicalPath);
    ASSERT_NE(reference, top->objects.end());
    EXPECT_TRUE(reference->subroutineStatic);

    const ModuleDefinition* package = design->findModule("p::");
    ASSERT_NE(package, nullptr);
    const enzan::eir::Object& last = package->objects[require(package->findObject("serial.last"))];
    EXPECT_TRUE(last.subroutineStatic);
    ASSERT_TRUE(last.initialValue.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    EXPECT_EQ(last.initialValue->ref().toUInt64(), 7U);
}

TEST_F(FrontendTest, AVariableWithoutStorageIsReportedOnceWhereItIsRead) {
    // What makes the storage impossible is reported where the storage is asked for; the read
    // asking for it has nothing to add.
    EXPECT_EQ(analyse(R"(
        module top;
            function integer next(input integer a);
                static integer t = a + 1;
                return t;
            endfunction
            integer r;
            initial r = next(4);
        endmodule
    )",
                      "top"),
              nullptr);
    EXPECT_TRUE(reported("the initialiser of 't'"));
    EXPECT_EQ(std::ranges::count(consumer.codes, enzan::DiagCode::UnsupportedConstruct), 1)
        << consumer.messages.back();
}

TEST_F(FrontendTest, AnUnpackedArrayIsStoredFlatAndKeepsItsDimensions) {
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [7:0] memory [0:15];
            logic signed [3:0] grid [2:0][0:3];
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);

    using enzan::eir::ArrayDimension;
    const enzan::eir::Object& memory = found->objects[require(found->findObject("memory"))];
    EXPECT_EQ(memory.type.bitWidth(), 128U);
    EXPECT_EQ(memory.elementWidth, 8U);
    EXPECT_FALSE(memory.elementSigned);
    EXPECT_EQ(memory.unpackedDimensions,
              (std::vector<ArrayDimension>{ArrayDimension{.left = 0, .right = 15}}));

    const enzan::eir::Object& grid = found->objects[require(found->findObject("grid"))];
    EXPECT_EQ(grid.type.bitWidth(), 48U);
    EXPECT_EQ(grid.elementWidth, 4U);
    EXPECT_TRUE(grid.elementSigned) << "the flat vector is unsigned, and the element is not";
    EXPECT_EQ(grid.unpackedDimensions,
              (std::vector<ArrayDimension>{ArrayDimension{.left = 2, .right = 0},
                                           ArrayDimension{.left = 0, .right = 3}}));
}

TEST_F(FrontendTest, AnElementOrMemberWritesItsOwnBitsOfTheFlatStorage) {
    // The left element is highest: element 0 of `[0:3]` is bits [31:24], and `[3:0]` puts
    // element 3 there. A struct's first member is highest too.
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            typedef struct { logic [3:0] a; logic [11:0] b; } record_t;
            logic [7:0] up [0:3];
            logic [7:0] down [3:0];
            record_t    record;
            logic [7:0] value;
            assign up[0] = value;
            assign down[0] = value;
            assign record.a = value[3:0];
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    using Extents = std::vector<std::vector<WriteExtent>>;
    EXPECT_EQ(extentsOf(top, "up"), (Extents{{extent(top, "up", 24, 8)}}));
    EXPECT_EQ(extentsOf(top, "down"), (Extents{{extent(top, "down", 0, 8)}}));
    EXPECT_EQ(extentsOf(top, "record"), (Extents{{extent(top, "record", 12, 4)}}));
}

TEST_F(FrontendTest, AnAscendingPackedRangeHasItsLeftIndexMostSignificant) {
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [0:7] a;
            logic       b;
            assign a[0] = b;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(extentsOf(*found, "a"),
              (std::vector<std::vector<WriteExtent>>{{extent(*found, "a", 7, 1)}}));
}

TEST_F(FrontendTest, ConstantSelectsWriteExactlyTheirBitsAndTouchingOnesMerge) {
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic       a;
            logic [7:0] x;
            always_comb begin
                x[0] = a;
                x[1] = a;
                x[5:4] = {a, a};
            end
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    EXPECT_EQ(extentsOf(top, "x"), (std::vector<std::vector<WriteExtent>>{
                                       {extent(top, "x", 0, 2), extent(top, "x", 4, 2)}}));
}

TEST_F(FrontendTest, ASelectOfAPackedArrayOrAnOffsetRangeWritesTheBitsItNames) {
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [7:0]      a;
            logic [3:0][7:0] bytes;
            logic [15:8]     high;
            logic [7:0]      up;
            logic [7:0]      down;
            assign bytes[2] = a;
            assign high[13:10] = a[3:0];
            assign up[1 +: 2] = a[1:0];
            assign down[6 -: 3] = a[2:0];
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    using Extents = std::vector<std::vector<WriteExtent>>;
    EXPECT_EQ(extentsOf(top, "bytes"), (Extents{{extent(top, "bytes", 16, 8)}}));
    EXPECT_EQ(extentsOf(top, "high"), (Extents{{extent(top, "high", 2, 4)}}));
    EXPECT_EQ(extentsOf(top, "up"), (Extents{{extent(top, "up", 1, 2)}}));
    EXPECT_EQ(extentsOf(top, "down"), (Extents{{extent(top, "down", 4, 3)}}));
}

TEST_F(FrontendTest, AVariableSelectWritesEveryBitItsIndexCanReach) {
    // A two-bit index reaches four positions of a sixteen-bit vector, and nothing past them;
    // an `int` index reaches anything.
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [1:0]  i;
            int          j;
            logic        a;
            logic [15:0] narrow;
            logic [15:0] wide;
            logic [3:0][7:0] bytes;
            always_comb narrow[i] = a;
            always_comb wide[j] = a;
            always_comb bytes[i + 2'd0] = {8{a}};
            always_comb narrow[i + 4'd8 +: 2] = {a, a};
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    using Extents = std::vector<std::vector<WriteExtent>>;
    EXPECT_EQ(extentsOf(top, "narrow"),
              (Extents{{extent(top, "narrow", 0, 4)}, {extent(top, "narrow", 0, 16)}}));
    EXPECT_EQ(extentsOf(top, "wide"), (Extents{{extent(top, "wide", 0, 16)}}));
    EXPECT_EQ(extentsOf(top, "bytes"), (Extents{{extent(top, "bytes", 0, 32)}}));
}

TEST_F(FrontendTest, AConstantSelectOutsideTheObjectWritesNothing) {
    // The language discards such a write, so it drives no bit and is not emitted at all.
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [3:0] n;
            logic       a;
            always_comb begin
                n[1] = a;
                n[7] = a;
            end
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    EXPECT_EQ(extentsOf(top, "n"),
              (std::vector<std::vector<WriteExtent>>{{extent(top, "n", 1, 1)}}));
}

TEST_F(FrontendTest, EachInstanceOfAGeneratedArrayDrivesItsOwnBit) {
    // The case section 3.7 is written around: a parameterised number of instances whose
    // outputs are bits of one array. Each connection drives one bit and no other.
    const std::unique_ptr<Design> design = analyse(R"(
        module channel (output logic ready);
            assign ready = 1'b1;
        endmodule

        module top;
            logic [3:0] ready;
            for (genvar i = 0; i < 4; i++) begin : gen
                channel u (.ready(ready[i]));
            end
        endmodule
    )",
                                                   "top");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    const ModuleDefinition& top = *found;

    EXPECT_EQ(extentsOf(top, "ready"), (std::vector<std::vector<WriteExtent>>{
                                           {extent(top, "ready", 0, 1)},
                                           {extent(top, "ready", 1, 1)},
                                           {extent(top, "ready", 2, 1)},
                                           {extent(top, "ready", 3, 1)},
                                       }));
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

TEST_F(FrontendTest, ReportsADelayOnANet) {
    EXPECT_EQ(analyse(R"(
        module m;
            logic a;
            wire #5 w;
            assign w = a;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a delay on a net"));
}

TEST_F(FrontendTest, ReportsAnInoutConnectedToASignalOfAnotherWidth) {
    // Connected through an assignment, as an input or an output of another width is, which an
    // inout cannot be; the signal is still a signal, not an expression.
    EXPECT_EQ(analyse(R"(
        module child(inout wire [1:0] p);
        endmodule
        module top;
            wire [2:0] w;
            child u(.p(w));
        endmodule
    )",
                      "top"),
              nullptr);
    EXPECT_TRUE(reported("an inout port connected to an expression, a hierarchical reference or "
                         "a signal of another width"));
}

TEST_F(FrontendTest, ReportsADriveStrengthOtherThanStrong) {
    // Resolution has no strength model, so a weak driver would lose to nothing and tie with a
    // strong one.
    EXPECT_EQ(analyse(R"(
        module m;
            logic a;
            wire w;
            assign (weak0, weak1) w = a;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a drive strength other than strong"));
}

TEST_F(FrontendTest, AnExplicitStrongDriveIsTheDefaultAndAccepted) {
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            logic a;
            wire w;
            assign (strong0, strong1) w = a;
        endmodule
    )");
    EXPECT_NE(design, nullptr) << consumer.messages.front();
}

TEST_F(FrontendTest, ACalledFunctionIsInlinedAndItsReadsAreTheCallers) {
    // The body becomes part of the process that calls it, so what it reads is in that process's
    // read set, and an always_comb calling it is sensitive to it (DESIGN_GUIDE.md section 4.2).
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [7:0] scale;
            logic [7:0] b;
            logic [7:0] y;
            function automatic logic [7:0] times_scale(input logic [7:0] value);
                return value * scale;
            endfunction
            always_comb y = times_scale(b);
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->processes.size(), 1U);

    const std::vector<std::uint32_t>& reads = found->processes[0].readSet;
    EXPECT_TRUE(std::ranges::binary_search(reads, require(found->findObject("scale"))));
    EXPECT_TRUE(std::ranges::binary_search(reads, require(found->findObject("b"))));
    // The function's own variables are registers, not state.
    EXPECT_FALSE(found->findObject("value").has_value());
}

TEST_F(FrontendTest, AHierarchicalReferenceIsAPathFromTheInstanceHoldingIt) {
    // Down from the testbench, and up and across from one leaf to another. One definition
    // serves both leaves, so the path is relative rather than from the root.
    const std::unique_ptr<Design> design = analyse(R"(
        module leaf;
            logic [3:0] value;
            logic [3:0] seen;
            assign seen = tb.u_other.value;
        endmodule

        module tb;
            leaf u_one ();
            leaf u_other ();
            logic [3:0] copy;
            assign copy = u_one.value;
        endmodule
    )",
                                                   "tb");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const auto referencesOf = [](const ModuleDefinition& definition) {
        std::vector<std::string> paths;
        for (const enzan::eir::Object& object : definition.objects)
            if (!object.hierarchicalPath.empty()) paths.push_back(object.hierarchicalPath);
        return paths;
    };
    const ModuleDefinition* tb = design->findModule("tb");
    const ModuleDefinition* leaf = design->findModule("leaf");
    ASSERT_NE(tb, nullptr);
    ASSERT_NE(leaf, nullptr);
    EXPECT_EQ(referencesOf(*tb), (std::vector<std::string>{"u_one/value"}));
    EXPECT_EQ(referencesOf(*leaf), (std::vector<std::string>{"../u_other/value"}));
}

TEST_F(FrontendTest, AModuleWithoutATimescaleBesideOneWithIsAccepted) {
    // IEEE 1800 makes the mixture an error and slang enforces it, but simulators warn and real
    // designs rely on that; Enzan counts delays in its own unit, which is the default given.
    const std::unique_ptr<Design> design = analyse(R"(
        `timescale 1ns / 1ps
        module top;
            leaf u ();
        endmodule
        `resetall
        module leaf;
        endmodule
    )",
                                                   "top");
    EXPECT_NE(design, nullptr) << consumer.messages.front();
}

TEST_F(FrontendTest, AMemberReachedThroughAnInterfacePortGoesThroughThePort) {
    // The reference names the port, not the interface it is connected to in one instance, and
    // the instance records what each of its interface ports is connected to.
    const std::unique_ptr<Design> design = analyse(R"(
        interface bus_if;
            logic [3:0] data;
            modport sink (input data);
        endinterface

        module leaf (bus_if.sink in, output logic [3:0] copy);
            assign copy = in.data;
        endmodule

        module middle (bus_if.sink in, output logic [3:0] copy);
            leaf u_leaf (.in(in), .copy(copy));
        endmodule

        module top;
            bus_if bus ();
            logic [3:0] copy;
            middle u_middle (.in(bus), .copy(copy));
        endmodule
    )",
                                                   "top");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* leaf = design->findModule("leaf");
    ASSERT_NE(leaf, nullptr);
    EXPECT_TRUE(std::ranges::any_of(leaf->objects, [](const enzan::eir::Object& object) {
        return object.hierarchicalPath == "@in/data";
    }));

    using enzan::eir::InterfaceBinding;
    const ModuleDefinition* top = design->findModule("top");
    const ModuleDefinition* middle = design->findModule("middle");
    ASSERT_NE(top, nullptr);
    ASSERT_NE(middle, nullptr);
    const auto bindingsOf = [](const ModuleDefinition& definition, std::string_view instance) {
        for (const enzan::eir::Instance& child : definition.instances)
            if (child.name == instance) return child.interfaceBindings;
        return std::vector<InterfaceBinding>{};
    };
    EXPECT_EQ(bindingsOf(*top, "u_middle"),
              (std::vector<InterfaceBinding>{InterfaceBinding{.port = "in", .target = "bus"}}));
    EXPECT_EQ(bindingsOf(*middle, "u_leaf"),
              (std::vector<InterfaceBinding>{InterfaceBinding{.port = "in", .target = "@in"}}));
}

TEST_F(FrontendTest, AModuleGivenInterfacesOfTwoWidthsIsTwoParameterisations) {
    // `sink` sets no parameter of its own, yet its widths come from the interface it is
    // given. Two instances agreeing on their own parameters are therefore not one
    // parameterisation, and sharing a translation between them gave one the other's widths.
    const std::unique_ptr<Design> design = analyse(R"(
        interface bus_if #(parameter int W = 8);
            logic [W-1:0] data;
        endinterface

        module sink (bus_if b);
            localparam int W = b.W;
            logic [W-1:0] copy;
            assign copy = b.data;
        endmodule

        module top;
            bus_if #(.W(8))  narrow ();
            bus_if #(.W(16)) wide ();
            sink u_narrow (.b(narrow));
            sink u_wide (.b(wide));
        endmodule
    )",
                                                   "top");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const std::vector<std::uint32_t> sinks = design->findSpecialisations("sink");
    ASSERT_EQ(sinks.size(), 2U);
    std::set<std::uint32_t> widths;
    for (const std::uint32_t index : sinks) {
        const ModuleDefinition& sink = design->module(index);
        widths.insert(sink.objects[require(sink.findObject("copy"))].type.bitWidth());
    }
    EXPECT_EQ(widths, (std::set<std::uint32_t>{8, 16}));
}

TEST_F(FrontendTest, AParameterOverrideSetsTheTopLevelsParameter) {
    const std::unique_ptr<Design> design =
        analyse("module top #(parameter int W = 4); endmodule\n", "top", {"W=9"});
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    ASSERT_EQ(design->modules().size(), 1U);
    const ModuleDefinition& top = design->modules().front();
    ASSERT_EQ(top.parameters.size(), 1U);
    EXPECT_EQ(top.parameters[0].value.ref().toUInt64(), 9U);
}

TEST_F(FrontendTest, AnOverrideNamingNoParameterIsReported) {
    // slang ignores it, which would let a misspelt `-G` change nothing without a word.
    const std::unique_ptr<Design> design =
        analyse("module top #(parameter int W = 4); endmodule\n", "top", {"WIDTH=9"});
    EXPECT_NE(design, nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::UnusedParameterOverride));
}

TEST_F(FrontendTest, AParameterOverrideIsNotAFileRead) {
    // slang parses each override from text of its own, in a buffer with a made-up name, and a
    // dependency file naming it has a prerequisite that never exists, so `make` re-analyses on
    // every run. The hierarchical form becomes a `defparam` and gets a buffer as well.
    const std::filesystem::path header = directory / "width.svh";
    std::ofstream(header) << "`define DEPTH 2\n";
    const std::filesystem::path source = directory / "design.sv";
    std::ofstream(source) << "`include \"width.svh\"\n"
                             "module leaf #(parameter int D = `DEPTH); endmodule\n"
                             "module top #(parameter int W = 4); leaf u(); endmodule\n";

    std::vector<std::filesystem::path> read;
    enzan::frontend::sv::Options options;
    options.files.push_back(source);
    options.includeDirectories.emplace_back(directory);
    options.topModules.emplace_back("top");
    options.parameterOverrides = {"W=9", "top.u.D=3"};
    options.filesRead = &read;

    ASSERT_NE(enzan::frontend::sv::analyse(options, sources, engine), nullptr)
        << consumer.messages.front();
    EXPECT_EQ(read, (std::vector<std::filesystem::path>{source, header}));
}

TEST_F(FrontendTest, FoldsAComparisonOfConstantTimes) {
    // `2ps` is a real; with a parameter on the other side the comparison is a constant.
    const auto design = analyse(R"(
        module m #(parameter time Period = 10ns);
            logic ok;
            initial ok = Period >= 2ps && Period * 1.5 > 14;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
}

TEST_F(FrontendTest, ReportsARealComputationOnAVariable) {
    EXPECT_EQ(analyse(R"(
        module m;
            logic [7:0] x;
            logic ok;
            initial ok = x * 1.5 > 2;
        endmodule
    )"),
              nullptr);
}

TEST_F(FrontendTest, ReportsADelayedWriteToPartOfASignal) {
    // A delayed store writes a whole object; splitting the update would let the rest of the
    // signal be overwritten with the value it had when the statement ran.
    EXPECT_EQ(analyse(R"(
        module m;
            logic [7:0] x;
            initial x[3:0] <= #2 4'd5;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a delayed write to part of a signal"));
}

TEST_F(FrontendTest, ReportsSeparateRiseAndFallDelays) {
    EXPECT_EQ(analyse(R"(
        module m;
            logic a;
            wire  y;
            assign #(1, 2) y = a;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("separate rise, fall and turn-off delays"));
}

TEST_F(FrontendTest, ReportsReadingAMemoryOfTwoDimensions) {
    // The file's words follow one address; which of two dimensions they would run along is
    // not something to guess.
    EXPECT_EQ(analyse(R"(
        module m;
            logic [7:0] grid [0:3][0:3];
            initial $readmemh("grid.hex", grid);
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a memory of other than one unpacked dimension"));
}

// ---------------------------------------------------------------------------
// Errors excused in code a constant condition rules out
// ---------------------------------------------------------------------------

TEST_F(FrontendTest, ADegenerateSelectInADeadBranchIsAWarning) {
    // With W = 0 the select is of width 0, an error IEEE 1800 section 11.5.1 makes no exception
    // for, but the branch can never run.
    EXPECT_NE(analyse(R"(
        module m #(parameter int W = 0) (input logic [7:0] x, output logic [7:0] y);
            always_comb if (W > 0) y = x[7 -: W]; else y = x;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::ErrorInDeadCode));
    EXPECT_FALSE(reportedCode(DiagCode::SystemVerilogError));
}

TEST_F(FrontendTest, ADegenerateConstructInADeadArmIsAWarning) {
    EXPECT_NE(analyse(R"(
        module m #(parameter int W = 2, parameter int N = 4)
                  (input logic [7:0] x, output logic [7:0] y);
            assign y = W > N ? {{(W - N){1'b0}}, x} : x;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::ErrorInDeadCode));
}

TEST_F(FrontendTest, TheSameSelectLiveInAnotherInstanceIsStillAnError) {
    // One location, dead in u_dead and live in u_live with the same width: slang reports it
    // once, and the instance in which it can run keeps it an error.
    EXPECT_EQ(analyse(R"(
        module sub #(parameter int W = 0, parameter bit Guard = 1)
                    (input logic [7:0] x, output logic [7:0] y);
            always_comb if (!Guard || W > 0) y = x[7 -: W]; else y = x;
        endmodule
        module top;
            logic [7:0] x, a, b;
            sub #(.W(0), .Guard(1)) u_dead (.x, .y(a));
            sub #(.W(0), .Guard(0)) u_live (.x, .y(b));
        endmodule
    )",
                      "top"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::SystemVerilogError));
}

TEST_F(FrontendTest, ADeadBranchInTwoCopiesOfOneParameterisationIsAWarning) {
    // slang elaborates the second copy's body as a duplicate of the first, and the dead code is
    // found in the first alone; the second must not count as an instance in which it can run.
    EXPECT_NE(analyse(R"(
        module sub #(parameter int W = 0) (input logic [7:0] x, output logic [7:0] y);
            always_comb if (W > 0) y = x[7 -: W]; else y = x;
        endmodule
        module top;
            logic [7:0] x, a, b;
            sub #(.W(0)) u_a (.x, .y(a));
            sub #(.W(0)) u_b (.x, .y(b));
        endmodule
    )",
                      "top"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::ErrorInDeadCode));
    EXPECT_FALSE(reportedCode(DiagCode::SystemVerilogError));
}

TEST_F(FrontendTest, ADegenerateSelectInLiveCodeIsStillAnError) {
    EXPECT_EQ(analyse(R"(
        module m #(parameter int W = 0) (input logic [7:0] x, output logic [7:0] y);
            always_comb y = x[7 -: W];
        endmodule
    )"),
              nullptr);
    EXPECT_FALSE(reportedCode(DiagCode::ErrorInDeadCode));
}

TEST_F(FrontendTest, ACaseItemAConstantSelectorPassesOverIsNotDead) {
    // slang does not treat the other items of a constant `case` as dead, and neither does Enzan.
    EXPECT_EQ(analyse(R"(
        module m #(parameter int W = 0) (input logic [7:0] x, output logic [7:0] y);
            always_comb case (W) 0: y = x; default: y = x[7 -: W]; endcase
        endmodule
    )"),
              nullptr);
    EXPECT_FALSE(reportedCode(DiagCode::ErrorInDeadCode));
}

TEST_F(FrontendTest, ReportsADeferredAssertion) {
    // Evaluated on the spot it would report the glitches it exists to filter out.
    EXPECT_EQ(analyse(R"(
        module m;
            logic a;
            always_comb assert #0 (a);
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a deferred assertion"));
}

TEST_F(FrontendTest, ReportsARecursiveCall) {
    // An inlined body cannot contain itself.
    EXPECT_EQ(analyse(R"(
        module m;
            logic [7:0] n;
            logic [7:0] y;
            function automatic logic [7:0] factorial(input logic [7:0] k);
                return k <= 1 ? 8'd1 : k * factorial(k - 1);
            endfunction
            always_comb y = factorial(n);
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a recursive call of 'factorial'"));
}

TEST_F(FrontendTest, ReportsARefArgument) {
    EXPECT_EQ(analyse(R"(
        module m;
            logic [7:0] v;
            task automatic bump(ref logic [7:0] target);
                target = target + 1;
            endtask
            initial bump(v);
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a `ref` argument"));
}

TEST_F(FrontendTest, ReportsAnUnsupportedSystemTaskByName) {
    EXPECT_EQ(analyse(R"(
        module m;
            initial $sdf_annotate("timing.sdf");
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::UnsupportedConstruct));
    EXPECT_TRUE(reported("$sdf_annotate")) << "the diagnostic must name the task";
}

TEST_F(FrontendTest, EachCaseFormComparesItsItemsWithItsOwnOpcode) {
    // A wildcard compare approximated with `==` would silently take the wrong branch, so each
    // form is pinned to the comparison IEEE 1800 section 12.5 gives it.
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            logic [3:0] s;
            logic       y;
            always_comb case (s)   4'b1x0z: y = 1'b1; default: y = 1'b0; endcase
            always_comb casez (s)  4'b1???: y = 1'b1; default: y = 1'b0; endcase
            always_comb casex (s)  4'b1xxx: y = 1'b1; default: y = 1'b0; endcase
            always_comb case (s) inside 4'b1???: y = 1'b1; default: y = 1'b0; endcase
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("m");
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->processes.size(), 4U);

    const auto uses = [](const enzan::eir::Process& process, enzan::eir::Opcode opcode) {
        return std::ranges::any_of(process.blocks, [opcode](const enzan::eir::BasicBlock& block) {
            return std::ranges::any_of(block.instructions,
                                       [opcode](const enzan::eir::Instruction& instruction) {
                                           return instruction.opcode == opcode;
                                       });
        });
    };
    EXPECT_TRUE(uses(found->processes[0], enzan::eir::Opcode::CaseEqual));
    EXPECT_TRUE(uses(found->processes[1], enzan::eir::Opcode::CaseZMatch));
    EXPECT_TRUE(uses(found->processes[2], enzan::eir::Opcode::CaseXMatch));
    EXPECT_TRUE(uses(found->processes[3], enzan::eir::Opcode::WildcardEqual));
}

TEST_F(FrontendTest, AToleranceRangeInAnInsideSetIsRefused) {
    // A tolerance range is IEEE 1800-2023 syntax, which slang refuses in the 2017 mode Enzan
    // runs it in. The frontend refuses it as well, so that selecting a later standard reports
    // it rather than comparing against the bounds as though they were a plain range.
    EXPECT_EQ(analyse(R"(
        module m;
            logic [7:0] v;
            logic       y;
            always_comb y = v inside {[8'd10 +/- 8'd2]};
        endmodule
    )"),
              nullptr);
}

TEST_F(FrontendTest, DiagnosticsCarryASourceLocationInEnzansOwnBuffers) {
    // The location has to resolve in Enzan's source manager, or the transcript shows a
    // message with no snippet and the quality that section 10.6 is about is lost.
    //
    // The file is named by an absolute path, which is what a build system passes and what a
    // file list expands to. slang shortens such a path when it renders a file *name*, so a
    // conversion that matched names rather than buffers resolved nothing at all here — every
    // diagnostic in the design lost its file, its line and its caret.
    EXPECT_EQ(analyse("module m;\n  initial $sdf_annotate(\"timing.sdf\");\nendmodule\n"), nullptr);

    ASSERT_TRUE(reportedCode(DiagCode::UnsupportedConstruct));
    EXPECT_GE(sources.bufferCount(), 1U);

    const Diagnostic* unsupported = nullptr;
    for (const Diagnostic& diagnostic : consumer.received)
        if (diagnostic.code == DiagCode::UnsupportedConstruct) unsupported = &diagnostic;

    ASSERT_NE(unsupported, nullptr);
    ASSERT_TRUE(unsupported->location.valid()) << "the location did not resolve";
    EXPECT_EQ(sources.lineNumber(unsupported->location), 2U);
    EXPECT_EQ(sources.bufferName(unsupported->location.buffer()),
              (directory / "design.sv").string());
}

TEST_F(FrontendTest, AModuleDefinitionCarriesTheFileItWasWrittenIn) {
    // The library attributes units to source files by their definition's location (section
    // 4.4), so a definition with no location is a unit that can never be cached.
    const std::unique_ptr<Design> design = analyse("module m;\nendmodule\n");
    ASSERT_NE(design, nullptr);
    ASSERT_EQ(design->size(), 1U);

    ASSERT_TRUE(design->module(0).location.valid());
    EXPECT_EQ(sources.bufferName(design->module(0).location.buffer()),
              (directory / "design.sv").string());
}

TEST_F(FrontendTest, EveryInstanceOfOneParameterisationSharesOneDefinitionAndIsWiredToIt) {
    // One translation serves every instance of a parameterisation, which is only sound while
    // port indices are keyed by port *name*. Keyed by slang's port symbols — a separate object
    // per instance — the shared map binds nothing, and the design still elaborates and still
    // simulates, to the wrong answer. So what this pins is the wiring, not the sharing: each
    // instance must reach its *own* port object.
    const std::unique_ptr<Design> design = analyse(R"(
        module leaf #(parameter int WIDTH = 8) (input logic [WIDTH-1:0] d,
                                                output logic [WIDTH-1:0] q);
            assign q = d;
        endmodule

        module top;
            logic [7:0] a, b, c, ra, rb, rc;
            leaf #(.WIDTH(8)) u_a (.d(a), .q(ra));
            leaf #(.WIDTH(8)) u_b (.d(b), .q(rb));
            leaf #(.WIDTH(8)) u_c (.d(c), .q(rc));
        endmodule
    )",
                                                   "top");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    // Three instances, one definition between them.
    EXPECT_EQ(design->findSpecialisations("leaf").size(), 1U)
        << "identical parameterisations must not each get a definition of their own";

    const ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->instances.size(), 3U);

    // Each instance connects both its ports, and no two instances share a port object: that is
    // the property a pointer-keyed port map destroys.
    std::set<std::uint32_t> objects;
    for (const Instance& instance : top->instances) {
        EXPECT_EQ(instance.connections.size(), 2U)
            << "instance '" << instance.name << "' lost a connection";
        for (const auto& connection : instance.connections) objects.insert(connection.objectIndex);
    }
    EXPECT_EQ(objects.size(), 6U) << "instances are sharing port storage";
}

// ---------------------------------------------------------------------------
// Time units (DESIGN_GUIDE.md section 3.1)
// ---------------------------------------------------------------------------

TEST_F(FrontendTest, ADelayCountsItsModulesUnitAndIsGivenInTheRuns) {
    const std::unique_ptr<Design> design = analyse(R"(
`timescale 1ns / 1ps
        module m;
            logic a;
            initial #2.5 a = 1'b1;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const ModuleDefinition* module = design->findModule("m");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->processes.size(), 1U);
    EXPECT_TRUE(loads(module->processes[0], 2500)) << "2.5 ns is 2500 ps";
    EXPECT_EQ(runTimeUnit, -12);
}

TEST_F(FrontendTest, ARunWhoseUnitNothingFixedCountsTheFinestPrecisionAskedFor) {
    refineTimeUnit = true;
    const std::unique_ptr<Design> design = analyse(R"(
`timescale 1ns / 1fs
        module m;
            logic a;
            initial #1 a = 1'b1;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    EXPECT_EQ(runTimeUnit, -15);
    EXPECT_TRUE(loads(design->findModule("m")->processes[0], 1'000'000)) << "1 ns in fs";
}

TEST_F(FrontendTest, ARunWhoseUnitIsFixedKeepsItAndADelayFinerIsRoundedAndReported) {
    timeUnit = -9;
    const std::unique_ptr<Design> design = analyse(R"(
`timescale 1ns / 1ps
        module m;
            logic a;
            initial #1.6 a = 1'b1;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    EXPECT_EQ(runTimeUnit, -9);
    EXPECT_TRUE(loads(design->findModule("m")->processes[0], 2)) << "1600 ps is 2 ns, rounded";
    EXPECT_TRUE(reportedCode(DiagCode::DelayRounded));
    EXPECT_TRUE(reported("1600ps")) << consumer.messages.front();
    EXPECT_TRUE(reported("2ns")) << consumer.messages.front();
}

TEST_F(FrontendTest, ADelayIsRoundedToItsModulesPrecisionFirst) {
    const std::unique_ptr<Design> design = analyse(R"(
`timescale 1us / 1ns
        module m;
            logic a;
            initial #0.0036 a = 1'b1;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    EXPECT_TRUE(loads(design->findModule("m")->processes[0], 4000)) << "3.6 ns is 4 ns";
    EXPECT_FALSE(reportedCode(DiagCode::DelayRounded)) << "the language rounds it, not the run";
}

TEST_F(FrontendTest, ADelayInAPackageTaskCountsThePackagesUnit) {
    const std::unique_ptr<Design> design = analyse(R"(
`timescale 1ns / 1ns
        package waits;
            task automatic one;
                #1;
            endtask
        endpackage
`timescale 1ps / 1ps
        module m;
            initial waits::one();
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    EXPECT_TRUE(loads(design->findModule("m")->processes[0], 1000)) << "1 ns, not 1 ps";
}

TEST_F(FrontendTest, ADelayKnownOnlyAsTheProcessRunsIsScaledAsItRuns) {
    const std::unique_ptr<Design> design = analyse(R"(
`timescale 1ns / 1ps
        module m;
            int amount = 3;
            logic a;
            initial #amount a = 1'b1;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const enzan::eir::Process& process = design->findModule("m")->processes[0];
    EXPECT_TRUE(loads(process, 1000)) << "the count is multiplied by the ps in a ns";
    EXPECT_TRUE(std::ranges::any_of(process.blocks, [](const enzan::eir::BasicBlock& block) {
        return std::ranges::any_of(block.instructions, [](const enzan::eir::Instruction& op) {
            return op.opcode == enzan::eir::Opcode::Multiply;
        });
    }));
}

TEST_F(FrontendTest, ANegativeDelayIsRefused) {
    EXPECT_EQ(analyse(R"(
        module m;
            logic a;
            initial #(-1) a = 1'b1;
        endmodule
    )"),
              nullptr);
    EXPECT_TRUE(reported("a negative delay"));
}

TEST_F(FrontendTest, ASystemTaskCallCarriesTheTimeUnitOfItsScope) {
    const std::unique_ptr<Design> design = analyse(R"(
`timescale 1ns / 1ps
        module m;
            initial $display("%t", $time);
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    const enzan::eir::Process& process = design->findModule("m")->processes[0];
    ASSERT_EQ(process.sysCalls.size(), 1U);
    EXPECT_EQ(process.sysCalls[0].timeUnit, -9);
}

// ---------------------------------------------------------------------------
// Modules another language instantiates (DESIGN_GUIDE.md section 18, M7)
// ---------------------------------------------------------------------------

/// A module VHDL instantiates, whose width has no default, and one nothing instantiates whose
/// parameter has none either, which as a top level could not be elaborated.
constexpr std::string_view RequestedModules = R"(
    module Adder #(parameter int Width, parameter int Step = 1) (
        input logic [Width-1:0] a,
        output logic [Width-1:0] y
    );
        assign y = Step > 0 ? a : ~a;
    endmodule

    module unused #(parameter int P) ();
    endmodule
)";

TEST_F(FrontendTest, AModuleAnotherLanguageAsksForIsSpecialisedAsItAsks) {
    requests.push_back({.placeholder = "adder #(.width(5))",
                        .unit = "adder",
                        .parameters = {{.name = "width", .value = "5"}},
                        .ports = {}});
    onlyRequested = true;
    const std::unique_ptr<Design> design = analyse(RequestedModules);
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    ASSERT_EQ(answers.size(), 1U);
    const ModuleDefinition* adder = design->findModule(answers[0]);
    ASSERT_NE(adder, nullptr) << answers[0];
    ASSERT_EQ(adder->ports.size(), 2U);
    EXPECT_EQ(adder->objects[adder->ports[0].objectIndex].type.bitWidth(), 5U);

    // Nothing but what was asked for: the module asking is no part of the design, and a module
    // nothing asked for is not a top level.
    EXPECT_EQ(design->size(), 1U);
    EXPECT_TRUE(consumer.codes.empty()) << consumer.messages.front();
}

TEST_F(FrontendTest, TwoRequestsOfOneModuleWithOtherValuesAreTwoSpecialisations) {
    requests.push_back({.placeholder = "a",
                        .unit = "Adder",
                        .parameters = {{.name = "WIDTH", .value = "5"}},
                        .ports = {}});
    requests.push_back(
        {.placeholder = "b",
         .unit = "Adder",
         .parameters = {{.name = "width", .value = "7"}, {.name = "step", .value = "-2"}},
         .ports = {}});
    onlyRequested = true;
    const std::unique_ptr<Design> design = analyse(RequestedModules);
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    ASSERT_EQ(answers.size(), 2U);
    EXPECT_NE(answers[0], answers[1]);
    EXPECT_NE(design->findModule(answers[0]), nullptr);
    EXPECT_NE(design->findModule(answers[1]), nullptr);
}

TEST_F(FrontendTest, WithoutATopAModuleNothingInstantiatesIsATopAsWell) {
    requests.push_back({.placeholder = "a",
                        .unit = "adder",
                        .parameters = {{.name = "width", .value = "5"}},
                        .ports = {}});
    const std::unique_ptr<Design> design = analyse(R"(
        module Adder #(parameter int Width) (input logic [Width-1:0] a);
        endmodule

        module checker_top;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    EXPECT_EQ(design->candidateTopModules(), (std::vector<std::string>{answers[0], "checker_top"}));
}

TEST_F(FrontendTest, ARequestSettingAParameterTheModuleLacksIsRefusedByName) {
    requests.push_back({.placeholder = "a",
                        .unit = "adder",
                        .parameters = {{.name = "depth", .value = "5"}},
                        .ports = {}});
    onlyRequested = true;
    EXPECT_EQ(analyse(RequestedModules), nullptr);
    EXPECT_TRUE(reportedCode(DiagCode::RequestedParameterMissing));
    EXPECT_TRUE(reported("'depth'")) << consumer.messages.front();
}

TEST_F(FrontendTest, TheNamesSourcesDeclareAndInstantiateAreListedWithoutElaborating) {
    // `missing` is no module of the sources, and so may be another language's unit; an interface
    // and a primitive are declared as modules are, and a gate is no instance of anything.
    const std::filesystem::path path = directory / "design.sv";
    std::ofstream(path) << RequestedModules << R"(
        interface Bus;
        endinterface

        primitive inverter(output o, input i);
            table 0 : 1; 1 : 0; endtable
        endprimitive

        module top;
            wire a, b;
            Bus bus ();
            inverter i1 (b, a);
            and g1 (b, a, a);
            Adder #(.Width(4)) u1 (.a(), .y());
            if (1) begin : g
                Missing m ();
            end
            Missing n ();
        endmodule
    )";

    enzan::frontend::sv::Options options;
    options.files.push_back(path);
    const enzan::frontend::sv::ModuleNames names =
        enzan::frontend::sv::moduleNames(options, sources, engine);
    EXPECT_EQ(names.declared, (std::vector<std::string>{"Adder", "unused", "top"}));
    EXPECT_EQ(names.undeclared, std::vector<std::string>{"Missing"});
    EXPECT_TRUE(consumer.codes.empty());
}

TEST_F(FrontendTest, ARunUnitAnotherLanguageFixedIsTheOneDelaysCount) {
    fixedRunTimeUnit = -15;
    refineTimeUnit = true;
    const std::unique_ptr<Design> design = analyse(R"(
        module m;
            logic a;
            initial #2 a = 1'b1;
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    EXPECT_EQ(runTimeUnit, -15);
    EXPECT_TRUE(loads(design->findModule("m")->processes[0], 2000)) << "2 ps in fs";
}

TEST_F(FrontendTest, AnInstanceOfAnotherLanguagesUnitAsksForItWithItsValues) {
    // A VHDL entity `fifo` with a width generic and a name, whose ports are as wide as its one
    // translation so far says for the width 8, and one bit wide for any other.
    using Kind = enzan::eir::UnitInterface::Kind;
    using enzan::eir::PortDirection;
    interfaces.push_back(
        {.name = "Fifo",
         .parameters = {{.name = "Width", .kind = Kind::Integer, .length = 0, .value = "8"},
                        {.name = "Name", .kind = Kind::String, .length = 0, .value = "\"f\""}},
         .ports = {{.name = "Clk",
                    .direction = PortDirection::Input,
                    .fourState = true,
                    .isSigned = false},
                   {.name = "Din",
                    .direction = PortDirection::Input,
                    .fourState = true,
                    .isSigned = false},
                   {.name = "Count",
                    .direction = PortDirection::Output,
                    .fourState = false,
                    .isSigned = true}},
         .widths = {
             {.values = {"8", "\"f\""}, .ports = {{.bits = 1}, {.bits = 8}, {.bits = 32}}}}});
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic clk;
            logic [7:0] d;
            int n, m, k;
            Fifo u (.Clk(clk), .Din(d), .Count(n));
            Fifo #(.Width(8)) v (.Clk(clk), .Din(d), .Count(m));
            Fifo #(.Width(4), .Name("g")) w (.Clk(clk), .Din(d[3:0]), .Count(k));
        endmodule
)");
    ASSERT_NE(design, nullptr) << consumer.messages.front();

    ASSERT_EQ(asked.size(), 2U) << "u and v ask for the same";
    EXPECT_EQ(asked[0].unit, "Fifo");
    using Parameter = enzan::eir::UnitRequest::Parameter;
    EXPECT_EQ(asked[0].parameters, (std::vector<Parameter>{{.name = "Width", .value = "8"},
                                                           {.name = "Name", .value = "\"f\""}}));
    EXPECT_EQ(asked[1].parameters, (std::vector<Parameter>{{.name = "Width", .value = "4"},
                                                           {.name = "Name", .value = "\"g\""}}));
    ASSERT_EQ(asked[0].ports.size(), 3U);
    EXPECT_EQ(asked[0].ports[2].name, "Count");
    EXPECT_EQ(asked[0].ports[2].direction, PortDirection::Output);

    // The unit is not translated; its instances name the requests, and connect by its ports.
    EXPECT_EQ(design->size(), 1U);
    const ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->instances.size(), 3U);
    EXPECT_EQ(top->instances[0].definitionName, asked[0].placeholder);
    EXPECT_EQ(top->instances[1].definitionName, asked[0].placeholder);
    EXPECT_EQ(top->instances[2].definitionName, asked[1].placeholder);
    const Instance& u = top->instances[0];
    ASSERT_EQ(u.connections.size(), 3U);
    EXPECT_EQ(u.connections[1].portIndex, 1U);
    EXPECT_EQ(top->objects[u.connections[1].objectIndex].name, "d")
        << "the row for the width 8 makes Din as wide as d";

    // No row is for the width 4 yet, so w's ports are one bit until an analysis with one.
    EXPECT_EQ(consumer.messages,
              (std::vector<std::string>{
                  "implicit conversion of port connection truncates from 4 to 1 bits",
                  "implicit conversion of port connection expands from 1 to 32 bits"}));
}

TEST_F(FrontendTest, AnAnalysisFailingBeforeAnInstancesWidthsAreKnownSaysWhatItAsks) {
    // An unpacked port is an unpacked array of one one-bit element until a row says otherwise,
    // which no array of four bytes can be connected to; what the instance asks for is said, so that
    // it can be translated and the analysis made again with its widths.
    using enzan::eir::PortDirection;
    interfaces.push_back({.name = "Lanes",
                          .parameters = {},
                          .ports = {{.name = "Words",
                                     .direction = PortDirection::Input,
                                     .fourState = true,
                                     .isSigned = false,
                                     .unpacked = true}},
                          .widths = {}});
    const std::string source = R"(
        module top;
            logic [7:0] words [0:3];
            Lanes u (.Words(words));
        endmodule
)";
    EXPECT_EQ(analyse(source), nullptr);
    EXPECT_TRUE(unsettled);
    ASSERT_EQ(asked.size(), 1U);
    EXPECT_EQ(asked[0].unit, "Lanes");

    // With the row its translation gives, the same analysis succeeds.
    interfaces[0].widths.push_back({.values = {}, .ports = {{.bits = 8, .left = 0, .right = 3}}});
    unsettled = false;
    consumer.messages.clear();
    const std::unique_ptr<Design> design = analyse(source);
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    EXPECT_FALSE(unsettled);
    const ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->instances.size(), 1U);
    ASSERT_EQ(top->instances[0].connections.size(), 1U);
    EXPECT_EQ(top->objects[top->instances[0].connections[0].objectIndex].name, "words");
}

TEST_F(FrontendTest, ARealParameterOfAnotherLanguagesUnitIsAskedForAsAReal) {
    // With a point, so that it reads as a real, whatever the instance wrote.
    using Kind = enzan::eir::UnitInterface::Kind;
    interfaces.push_back(
        {.name = "rx",
         .parameters = {{.name = "fclk", .kind = Kind::Real, .length = 0, .value = "19200.0"}},
         .ports = {},
         .widths = {}});
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            rx #(.fclk(100000000)) u ();
            rx v ();
        endmodule
)");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    ASSERT_EQ(asked.size(), 2U);
    EXPECT_EQ(asked[0].parameters[0].value, "100000000.0");
    EXPECT_EQ(asked[1].parameters[0].value, "19200.0");
}

} // namespace
