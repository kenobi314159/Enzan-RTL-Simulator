// SPDX-License-Identifier: GPL-3.0-or-later

#include "session/ConstantFunctionRunner.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "frontend/sv/SystemVerilogFrontend.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::eir::BitVector;
using enzan::session::ConstantFunctionRunner;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const enzan::Diagnostic& diagnostic) override { codes.push_back(diagnostic.code); }

    std::vector<DiagCode> codes;
};

/// A design whose generic unit calls `widths::count_width`, which calls another function, as
/// the frontend analyses it: the function's unit, and the call slang evaluated for it.
class ConstantFunctionRunnerTest : public ::testing::Test {
protected:
    ConstantFunctionRunnerTest() : engine(sources) {
        engine.addConsumer(consumer);
        workspace.write("design.sv", R"(
            package widths;
                function automatic int unsigned index_width(input int unsigned count);
                    if (count > 32'd1) return unsigned'($clog2(count));
                    return 32'd1;
                endfunction
                function automatic int unsigned count_width(input int unsigned largest);
                    return index_width(largest + 1);
                endfunction
            endpackage
            module stage #(parameter int unsigned N = 4) (
                input logic clk, output logic [widths::count_width(N)-1:0] q);
                always_ff @(posedge clk) q <= q + 1'b1;
            endmodule
            module top;
                logic clk;
                logic [2:0] q;
                stage #(.N(5)) u (.clk, .q);
            endmodule
        )");
        enzan::frontend::sv::Options options;
        options.files.push_back(workspace.path() / "design.sv");
        options.topModules.emplace_back("top");
        design = enzan::frontend::sv::analyse(options, sources, engine);
        // Only what the runner reports is under test, not slang's warnings about the source.
        consumer.codes.clear();
    }

    std::optional<BitVector> countWidth(ConstantFunctionRunner& runner, std::uint64_t largest) {
        return runner.call("widths::count_width", BitVector::fromUInt64(largest, 32), 32);
    }

    enzan::test::Workspace workspace;
    enzan::SourceManager sources;
    enzan::DiagnosticEngine engine;
    CollectingConsumer consumer;
    std::unique_ptr<enzan::eir::Design> design;
};

TEST_F(ConstantFunctionRunnerTest, RunsAFunctionForArgumentsSlangNeverSaw) {
    ASSERT_NE(design, nullptr);
    ConstantFunctionRunner runner(*design, sources, engine);

    // slang saw 5; the function is its own unit, so any argument is a run of it.
    const std::optional<BitVector> hundred = countWidth(runner, 100);
    ASSERT_TRUE(hundred.has_value());
    EXPECT_EQ(hundred->ref().toUInt64(), 7U);
    const std::optional<BitVector> zero = countWidth(runner, 0);
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(zero->ref().toUInt64(), 1U);
    EXPECT_TRUE(consumer.codes.empty());

    EXPECT_FALSE(runner.call("widths::nothing", BitVector::fromUInt64(1, 32), 32).has_value());
    EXPECT_FALSE(runner.call("widths::count_width", BitVector::fromUInt64(1, 8), 32).has_value())
        << "arguments of another width than the function takes";
}

TEST_F(ConstantFunctionRunnerTest, AFunctionThatDisagreesWithSlangIsNotTrusted) {
    // The unit holds the call slang evaluated; a translation that does not reproduce it is
    // wrong, and every width it would decide is refused rather than used.
    ASSERT_NE(design, nullptr);
    enzan::eir::Design tampered;
    for (const enzan::eir::ModuleDefinition& definition : design->modules()) {
        enzan::eir::ModuleDefinition copy = definition;
        if (copy.isFunction) {
            enzan::eir::Object& result = copy.objects[copy.ports.back().objectIndex];
            result.initialValue = BitVector::fromUInt64(9, 32);
        }
        tampered.addModule(std::move(copy));
    }

    ConstantFunctionRunner runner(tampered, sources, engine);
    EXPECT_FALSE(countWidth(runner, 100).has_value());
    EXPECT_FALSE(countWidth(runner, 3).has_value());
    EXPECT_EQ(std::ranges::count(consumer.codes, DiagCode::ConstantFunctionDisagrees), 1)
        << "reported once, however often it is called";
}

} // namespace
