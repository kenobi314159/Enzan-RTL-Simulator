// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/CompileOrder.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::BufferId;
using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::control::Declaration;
using enzan::control::PackageReference;
using enzan::control::SourceScan;
using enzan::control::UnitKind;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

// ---------------------------------------------------------------------------
// The pre-scan
// ---------------------------------------------------------------------------

class ScanTest : public ::testing::Test {
protected:
    SourceScan scan(std::string_view text) {
        const BufferId buffer = sources.addBuffer("unit.sv", std::string(text));
        return enzan::control::scanSystemVerilog(buffer, sources.bufferText(buffer));
    }

    static std::vector<std::string> names(const SourceScan& result, UnitKind kind) {
        std::vector<std::string> found;
        for (const Declaration& declaration : result.declarations)
            if (declaration.kind == kind) found.push_back(declaration.name);
        return found;
    }

    static std::vector<std::string> referenced(const SourceScan& result) {
        std::vector<std::string> found;
        found.reserve(result.packageReferences.size());
        for (const PackageReference& reference : result.packageReferences)
            found.push_back(reference.name);
        return found;
    }

    SourceManager sources;
};

TEST_F(ScanTest, FindsEachKindOfDeclaration) {
    const SourceScan result = scan("module m; endmodule\n"
                                   "package p; endpackage\n"
                                   "interface i; endinterface\n"
                                   "program g; endprogram\n"
                                   "primitive u(o, a); endprimitive\n"
                                   "class c; endclass\n");

    EXPECT_EQ(names(result, UnitKind::Module), (std::vector<std::string>{"m"}));
    EXPECT_EQ(names(result, UnitKind::Package), (std::vector<std::string>{"p"}));
    EXPECT_EQ(names(result, UnitKind::Interface), (std::vector<std::string>{"i"}));
    EXPECT_EQ(names(result, UnitKind::Program), (std::vector<std::string>{"g"}));
    EXPECT_EQ(names(result, UnitKind::Primitive), (std::vector<std::string>{"u"}));
    EXPECT_EQ(names(result, UnitKind::Class), (std::vector<std::string>{"c"}));
}

TEST_F(ScanTest, RecordsWhereADeclarationIs) {
    const SourceScan result = scan("// a comment\nmodule top;\nendmodule\n");

    ASSERT_EQ(result.declarations.size(), 1U);
    EXPECT_EQ(sources.lineNumber(result.declarations.front().location), 2U);
}

TEST_F(ScanTest, IgnoresDeclarationsInsideAnotherConstruct) {
    const SourceScan result = scan("module outer;\n  module inner; endmodule\nendmodule\n");

    EXPECT_EQ(names(result, UnitKind::Module), (std::vector<std::string>{"outer"}));
}

TEST_F(ScanTest, SkipsKeywordsInCommentsAndStrings) {
    const SourceScan result = scan("// module commented\n"
                                   "/* package blocked */\n"
                                   "module real_one;\n"
                                   "  initial $display(\"package quoted\");\n"
                                   "endmodule\n");

    EXPECT_EQ(names(result, UnitKind::Module), (std::vector<std::string>{"real_one"}));
    EXPECT_TRUE(names(result, UnitKind::Package).empty());
}

TEST_F(ScanTest, SkipsAnEscapeInsideAString) {
    const SourceScan result =
        scan("module m;\n  initial $display(\"a \\\" package p\");\nendmodule\n");

    EXPECT_TRUE(names(result, UnitKind::Package).empty());
}

TEST_F(ScanTest, FindsAnImportedPackage) {
    const SourceScan result = scan("module m;\n  import cfg_pkg::*;\nendmodule\n");

    EXPECT_EQ(referenced(result), (std::vector<std::string>{"cfg_pkg"}));
}

TEST_F(ScanTest, FindsAQualifiedReference) {
    const SourceScan result = scan("module m;\n  localparam int W = cfg_pkg::WIDTH;\nendmodule\n");

    EXPECT_EQ(referenced(result), (std::vector<std::string>{"cfg_pkg"}));
}

TEST_F(ScanTest, AQualifiedReferenceNeedsTheColonsAdjacent) {
    const SourceScan result = scan("module m;\n  initial x = a ? b : c;\nendmodule\n");

    EXPECT_TRUE(referenced(result).empty());
}

TEST_F(ScanTest, AGenericInterfacePortIsNotADeclaration) {
    const SourceScan result = scan("module m(interface bus);\nendmodule\n"
                                   "module after;\nendmodule\n");

    EXPECT_TRUE(names(result, UnitKind::Interface).empty());
    EXPECT_EQ(names(result, UnitKind::Module), (std::vector<std::string>{"m", "after"}));
}

TEST_F(ScanTest, AVirtualInterfaceIsNotADeclaration) {
    const SourceScan result = scan("module m;\n  virtual interface bus_if handle;\nendmodule\n");

    EXPECT_TRUE(names(result, UnitKind::Interface).empty());
}

TEST_F(ScanTest, AVirtualClassStillDeclaresAClass) {
    const SourceScan result = scan("virtual class base; endclass\n");

    EXPECT_EQ(names(result, UnitKind::Class), (std::vector<std::string>{"base"}));
}

TEST_F(ScanTest, AnExternModulePrototypeDoesNotSwallowTheRestOfTheFile) {
    const SourceScan result = scan("extern module proto(input a);\n"
                                   "module real_one;\nendmodule\n");

    EXPECT_EQ(names(result, UnitKind::Module), (std::vector<std::string>{"real_one"}));
}

TEST_F(ScanTest, ScansTextInsideAFalseIfdef) {
    // Not evaluated, deliberately: analysing a package nothing imports is harmless, and not
    // analysing one that something does is not.
    const SourceScan result = scan("`ifdef NEVER\npackage hidden_pkg; endpackage\n`endif\n");

    EXPECT_EQ(names(result, UnitKind::Package), (std::vector<std::string>{"hidden_pkg"}));
}

TEST_F(ScanTest, AnEmptyFileScansToNothing) {
    const SourceScan result = scan("");

    EXPECT_TRUE(result.declarations.empty());
    EXPECT_TRUE(result.packageReferences.empty());
}

TEST_F(ScanTest, AnUnclosedBlockCommentEndsTheScanRatherThanSpinning) {
    const SourceScan result = scan("module m; endmodule\n/* never closed\npackage p;\n");

    EXPECT_EQ(names(result, UnitKind::Module), (std::vector<std::string>{"m"}));
    EXPECT_TRUE(names(result, UnitKind::Package).empty());
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------

class CompileOrderTest : public ::testing::Test {
protected:
    CompileOrderTest() : engine(sources) { engine.addConsumer(consumer); }

    void write(std::string_view name, std::string_view content) {
        workspace.write(name, content);
        given.push_back(workspace / name);
    }

    [[nodiscard]] bool order() {
        return enzan::control::inferCompileOrder(given, sources, engine, ordered);
    }

    [[nodiscard]] std::vector<std::string> orderedNames() const {
        std::vector<std::string> names;
        names.reserve(ordered.size());
        for (const std::filesystem::path& file : ordered) names.push_back(file.filename().string());
        return names;
    }

    enzan::test::Workspace workspace;
    std::vector<std::filesystem::path> given;
    std::vector<std::filesystem::path> ordered;
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
};

TEST_F(CompileOrderTest, PutsAPackageBeforeWhatImportsIt) {
    write("top.sv", "module top;\n  import cfg_pkg::*;\nendmodule\n");
    write("cfg_pkg.sv", "package cfg_pkg;\n  localparam int W = 8;\nendpackage\n");

    ASSERT_TRUE(order());
    EXPECT_EQ(orderedNames(), (std::vector<std::string>{"cfg_pkg.sv", "top.sv"}));
}

TEST_F(CompileOrderTest, OrdersAChainOfPackages) {
    write("c.sv", "module c;\n  import b_pkg::*;\nendmodule\n");
    write("b.sv", "package b_pkg;\n  import a_pkg::*;\nendpackage\n");
    write("a.sv", "package a_pkg;\nendpackage\n");

    ASSERT_TRUE(order());
    EXPECT_EQ(orderedNames(), (std::vector<std::string>{"a.sv", "b.sv", "c.sv"}));
}

TEST_F(CompileOrderTest, KeepsTheGivenOrderWhereNothingDepends) {
    write("z.sv", "module z; endmodule\n");
    write("y.sv", "module y; endmodule\n");
    write("x.sv", "module x; endmodule\n");

    ASSERT_TRUE(order());
    EXPECT_EQ(orderedNames(), (std::vector<std::string>{"z.sv", "y.sv", "x.sv"}));
}

TEST_F(CompileOrderTest, InstantiationIsNotAnOrderingConstraint) {
    // Elaboration resolves an instance against the library by name, so two modules that
    // instantiate each other are the legal mutual reference section 8.3 forbids reporting.
    write("a.sv", "module a;\n  b u_b();\nendmodule\n");
    write("b.sv", "module b;\n  a u_a();\nendmodule\n");

    ASSERT_TRUE(order());
    EXPECT_EQ(orderedNames(), (std::vector<std::string>{"a.sv", "b.sv"}));
    EXPECT_TRUE(consumer.received.empty());
}

TEST_F(CompileOrderTest, AScopeThatNamesNoPackageOrdersNothing) {
    write("use.sv", "module use_it;\n  initial x = some_class::CONST;\nendmodule\n");
    write("cls.sv", "class some_class;\nendclass\n");

    ASSERT_TRUE(order());
    EXPECT_EQ(orderedNames(), (std::vector<std::string>{"use.sv", "cls.sv"}));
}

TEST_F(CompileOrderTest, APackageReferringToItselfIsNotACycle) {
    write("p.sv", "package p_pkg;\n  localparam int W = 8;\n  localparam int V = p_pkg::W;\n"
                  "endpackage\n");

    ASSERT_TRUE(order());
    EXPECT_EQ(orderedNames(), (std::vector<std::string>{"p.sv"}));
}

TEST_F(CompileOrderTest, ReportsAGenuineCycleWithTheCycleSpelledOut) {
    write("a.sv", "package a_pkg;\n  import b_pkg::*;\nendpackage\n");
    write("b.sv", "package b_pkg;\n  import a_pkg::*;\nendpackage\n");

    EXPECT_FALSE(order());
    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().code, DiagCode::CompileOrderCycle);

    const std::string& message = consumer.received.front().message;
    EXPECT_NE(message.find("a.sv"), std::string::npos) << message;
    EXPECT_NE(message.find("b.sv"), std::string::npos) << message;
    EXPECT_NE(message.find("->"), std::string::npos) << message;
}

TEST_F(CompileOrderTest, ReportsACycleThatSomethingElseDependsOn) {
    write("a.sv", "package a_pkg;\n  import b_pkg::*;\nendpackage\n");
    write("b.sv", "package b_pkg;\n  import a_pkg::*;\nendpackage\n");
    write("top.sv", "module top;\n  import a_pkg::*;\nendmodule\n");

    EXPECT_FALSE(order());
    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().code, DiagCode::CompileOrderCycle);
}

TEST_F(CompileOrderTest, ReportsAFileItCannotRead) {
    given.push_back(workspace / "absent.sv");

    EXPECT_FALSE(order());
    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().code, DiagCode::CannotOpenFile);
}

TEST_F(CompileOrderTest, OrderingNothingSucceedsWithNothing) {
    EXPECT_TRUE(order());
    EXPECT_TRUE(ordered.empty());
}

} // namespace
