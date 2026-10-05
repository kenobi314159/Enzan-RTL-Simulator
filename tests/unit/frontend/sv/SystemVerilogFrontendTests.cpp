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
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Verifier.h"

#include "support/Optionals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
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

TEST_F(FrontendTest, AnUnpackedArrayIsStoredFlatAndKeepsItsDimensions) {
    const std::unique_ptr<Design> design = analyse(R"(
        module top;
            logic [7:0] memory [0:15];
            logic [3:0] grid [2:0][0:3];
        endmodule
    )");
    ASSERT_NE(design, nullptr) << consumer.messages.front();
    const ModuleDefinition* found = design->findModule("top");
    ASSERT_NE(found, nullptr);

    using enzan::eir::UnpackedDimension;
    const enzan::eir::Object& memory = found->objects[require(found->findObject("memory"))];
    EXPECT_EQ(memory.type.bitWidth(), 128U);
    EXPECT_EQ(memory.elementWidth, 8U);
    EXPECT_EQ(memory.unpackedDimensions,
              (std::vector<UnpackedDimension>{UnpackedDimension{.left = 0, .right = 15}}));

    const enzan::eir::Object& grid = found->objects[require(found->findObject("grid"))];
    EXPECT_EQ(grid.type.bitWidth(), 48U);
    EXPECT_EQ(grid.elementWidth, 4U);
    EXPECT_EQ(grid.unpackedDimensions,
              (std::vector<UnpackedDimension>{UnpackedDimension{.left = 2, .right = 0},
                                              UnpackedDimension{.left = 0, .right = 3}}));
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

} // namespace
