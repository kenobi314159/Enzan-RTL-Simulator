// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Execution.h"
#include "runtime/Kernel.h"

#include "codegen/Bytecode.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "elab/Elaborator.h"
#include "support/Literals.h"
#include "support/ProcessBuilder.h"
#include "support/Simulation.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace {

using enzan::eir::Design;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::runtime::ExecutionBackend;
using enzan::runtime::ExecutionContext;
using enzan::runtime::ExecutionResult;
using enzan::runtime::Kernel;
using enzan::runtime::RunStatus;
using enzan::test::literal;
using enzan::test::ProcessBuilder;
using enzan::test::Simulation;
using enzan::test::SubprogramBuilder;

struct Observations {
    std::uint32_t runs = 0;
    std::uint32_t connections = 0;
    std::uint32_t suspended = 0;
    std::uint32_t resumed = 0;
    std::uint32_t calls = 0;
    std::uint32_t nestedCalls = 0;
    std::uint32_t depth = 0;
    bool destroyed = false;
    bool stateAliveAtDestruction = false;
};

/// A separately selected backend that delegates to the interpreter while recording entry paths.
class ObservingBackend final : public ExecutionBackend {
public:
    explicit ObservingBackend(Observations& observations)
        : observed(observations), delegate(enzan::runtime::makeInterpreterBackend()) {}

    ~ObservingBackend() override {
        observed.destroyed = true;
        if (lastKernel != nullptr)
            observed.stateAliveAtDestruction =
                !lastKernel->arena().all().empty() && !lastKernel->image().signals().empty();
    }

    ExecutionResult run(ExecutionContext& context) override {
        Kernel& kernel = context.kernel();
        lastKernel = &kernel;
        ++observed.runs;
        const auto& process = kernel.image().processes()[context.processIndex()];
        EXPECT_EQ(context.activation().bodyIndex, process.bodyIndex);
        EXPECT_EQ(context.activation().body,
                  &kernel.image().compiledDesign().body(process.bodyIndex));
        if (kernel.image().compiledDesign().body(process.bodyIndex).connectsPort)
            ++observed.connections;
        if (context.resumePc() != 0) ++observed.resumed;
        ++observed.depth;
        const ExecutionResult result = delegate->run(context);
        --observed.depth;
        if (result == ExecutionResult::Suspended) ++observed.suspended;
        return result;
    }

    ExecutionResult call(ExecutionContext& context) override {
        Kernel& kernel = context.kernel();
        lastKernel = &kernel;
        ++observed.calls;
        const auto bodyIndex = context.activation().bodyIndex;
        EXPECT_LT(bodyIndex, kernel.image().compiledDesign().bodies().size());
        if (bodyIndex < kernel.image().compiledDesign().bodies().size()) {
            EXPECT_EQ(context.activation().body, &kernel.image().compiledDesign().body(bodyIndex));
        }
        if (observed.depth != 0) ++observed.nestedCalls;
        return delegate->call(context);
    }

private:
    Observations& observed;
    std::unique_ptr<ExecutionBackend> delegate;
    Kernel* lastKernel = nullptr;
};

class StoppingBackend final : public ExecutionBackend {
public:
    explicit StoppingBackend(std::uint32_t& count) : runs(count) {}

    ExecutionResult run(ExecutionContext& context) override {
        ++runs;
        context.kernel().requestFinish();
        return ExecutionResult::Stopped;
    }

    ExecutionResult call(ExecutionContext& /*context*/) override {
        return ExecutionResult::Stopped;
    }

private:
    std::uint32_t& runs;
};

Process copy(std::uint32_t from, std::uint32_t to) {
    ProcessBuilder builder(ProcessKind::ContinuousAssign, "copy");
    const std::uint32_t value = builder.reg(Type::logic());
    builder.block("entry");
    builder.readObject(value, from);
    builder.store(to, value);
    builder.emit(Instruction::make(Opcode::Return));
    return builder.finish();
}

Design connectionDesign() {
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "source", .type = Type::logic(), .initialValue = literal("1'b0")});
    top.addObject(Object{.name = "output", .type = Type::logic()});

    Process connection = copy(0, 1);
    connection.connectsPort = true;
    top.processes.push_back(std::move(connection));

    ProcessBuilder driver(ProcessKind::Initial, "driver");
    const std::uint32_t delay = driver.reg(Type::bits(32, false, false));
    const std::uint32_t value = driver.reg(Type::logic());
    const std::uint32_t oneTick = driver.constant("32'd1");
    const std::uint32_t one = driver.constant("1'b1");
    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(oneTick)));
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    driver.block("resume");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(one)));
    driver.store(0, value);
    driver.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(driver.finish());

    Design design;
    design.addModule(std::move(top));
    return design;
}

TEST(ExecutionBackend, SelectedBackendRunsConnectionsAndResumesIndependently) {
    Observations observed;
    {
        Simulation selected(connectionDesign(), "tb", std::make_unique<ObservingBackend>(observed));
        const std::uint32_t initialRuns = observed.runs;
        Simulation reference(connectionDesign(), "tb");
        ASSERT_TRUE(selected.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_GT(observed.connections, 0U); // Initialization executes the connection.
        EXPECT_EQ(observed.runs, initialRuns);
        EXPECT_EQ(selected.value("/tb/output"), reference.value("/tb/output"));
        EXPECT_EQ(selected.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(selected.simulationKernel().totalDeltaCycles(),
                  reference.simulationKernel().totalDeltaCycles());

        EXPECT_EQ(selected.run(0), RunStatus::TimeLimitReached);
        EXPECT_EQ(reference.run(0), RunStatus::TimeLimitReached);
        EXPECT_GT(observed.suspended, 0U);
        EXPECT_EQ(selected.run(), RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), RunStatus::Quiescent);
        EXPECT_EQ(selected.value("/tb/output"), "1");
        EXPECT_GT(observed.resumed, 0U);
        EXPECT_EQ(selected.value("/tb/output"), reference.value("/tb/output"));
        EXPECT_EQ(selected.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(selected.simulationKernel().totalDeltaCycles(),
                  reference.simulationKernel().totalDeltaCycles());
        EXPECT_GE(observed.connections, 2U);
        EXPECT_FALSE(observed.destroyed);

        const std::uint32_t beforeRestart = observed.runs;
        selected.simulationKernel().initialize();
        reference.simulationKernel().initialize();
        EXPECT_GT(observed.runs, beforeRestart);
        EXPECT_FALSE(observed.destroyed);
        EXPECT_EQ(selected.run(), RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), RunStatus::Quiescent);
        EXPECT_EQ(selected.value("/tb/output"), reference.value("/tb/output"));
        EXPECT_EQ(selected.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
    EXPECT_TRUE(observed.destroyed);
    EXPECT_TRUE(observed.stateAliveAtDestruction);
}

TEST(ExecutionBackend, AStoppedBackendDoesNotFallBackToTheInterpreter) {
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "value", .type = Type::logic(), .initialValue = literal("1'b0")});
    ProcessBuilder driver(ProcessKind::Initial, "driver");
    const std::uint32_t value = driver.reg(Type::logic());
    const std::uint32_t one = driver.constant("1'b1");
    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(one)));
    driver.store(0, value);
    driver.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(driver.finish());
    Design design;
    design.addModule(std::move(top));

    std::uint32_t runs = 0;
    Simulation simulation(std::move(design), "tb", std::make_unique<StoppingBackend>(runs));
    ASSERT_TRUE(simulation.ready());
    EXPECT_EQ(simulation.run(), RunStatus::Finished);
    EXPECT_EQ(runs, 1U);
    EXPECT_EQ(simulation.value("/tb/value"), "0");
    EXPECT_EQ(simulation.simulationKernel().executedInstructions(), 0U);
}

TEST(ExecutionBackend, KernelResolutionUsesTheSelectedBackendAndCanReenterIt) {
    ModuleDefinition top;
    top.name = "tb";
    top.language = enzan::eir::Language::Vhdl;
    top.addObject(Object{.name = "a", .type = Type::logic(), .initialValue = literal("1'b0")});
    top.addObject(Object{.name = "b", .type = Type::logic(), .initialValue = literal("1'b1")});
    Object resolved{.name = "resolved", .type = Type::logic()};
    resolved.resolution = {.function = "WORK.TEST.RESOLVE", .elementBits = 1};
    top.addObject(std::move(resolved));
    Process first = copy(0, 2);
    Process second = copy(1, 2);
    first.drivesSignals = true;
    second.drivesSignals = true;
    top.processes.push_back(std::move(first));
    top.processes.push_back(std::move(second));

    SubprogramBuilder resolver("resolve");
    resolver.parameter("drivers", Type::bits(2, false, true));
    const std::uint32_t result = resolver.result(Type::logic());
    const std::uint32_t one = resolver.constant("1'b1");
    resolver.block("entry");
    resolver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(result), Operand::constant(one)));
    resolver.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(resolver.finish());

    Design design;
    design.addModule(std::move(top));
    const enzan::elab::ResolutionAnswers answers = {
        {{"WORK.TEST.RESOLVE", 2}, {"tb", "resolve"}},
    };
    Observations observed;
    Simulation simulation(std::move(design), "tb", std::make_unique<ObservingBackend>(observed),
                          answers);
    ASSERT_TRUE(simulation.ready());
    ASSERT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/resolved"), "1");
    EXPECT_GT(observed.calls, 0U);
    EXPECT_GT(observed.nestedCalls, 0U);
}

} // namespace
