// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Execution.h"
#include "runtime/ExecutionHelpers.h"

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
#include <string_view>
#include <utility>

namespace {

using namespace enzan;
using eir::Instruction;
using eir::Object;
using eir::Opcode;
using eir::Operand;
using eir::ProcessKind;
using eir::Type;
using runtime::ExecutionContext;
using runtime::ExecutionResult;
using runtime::RunStatus;
using test::ProcessBuilder;
using test::Simulation;

// This backend represents one known compiled body, using the same opaque-context calls that
// generated code will use. It does not interpret instructions or fall back to the interpreter.
class WaitBody final : public runtime::ExecutionBackend {
public:
    ExecutionResult run(ExecutionContext& context) override {
        auto* frame = runtime::enzanExecFrame(&context);
        const auto resume = runtime::enzanExecResumePc(&context);
        if (resume == 0) {
            const auto* source = runtime::enzanExecReadObject(&context, 0);
            frame[0] = source[0];
            frame[1] = source[1];
            runtime::enzanExecCount(&context, 2);
            runtime::enzanExecStore(&context, 1, 0, 0, 4);
            runtime::enzanExecCount(&context, 1);
            return static_cast<ExecutionResult>(runtime::enzanExecWaitTime(
                &context, 2, 32, context.activation().body->blockStarts[1]));
        }
        EXPECT_EQ(resume, context.activation().body->blockStarts[1]);
        runtime::enzanExecCount(&context, 1);
        runtime::enzanExecStore(&context, 1, 0, 4, 4);
        runtime::enzanExecCount(&context, 1);
        return ExecutionResult::Completed;
    }

    ExecutionResult call(ExecutionContext& /*context*/) override {
        ADD_FAILURE() << "Unexpected subprogram call";
        return ExecutionResult::Stopped;
    }
};

eir::Design waitDesign(std::string_view delayValue) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "source",
                         .type = Type::bits(4, false, true),
                         .initialValue = test::literal("4'b1xz0")});
    top.addObject(Object{.name = "output", .type = Type::bits(4, false, true)});
    ProcessBuilder process(ProcessKind::Initial, "driver");
    const auto value = process.reg(Type::bits(4, false, true));
    const auto delay = process.reg(Type::bits(32, false, true));
    const auto finalValue = process.reg(Type::bits(4, false, true));
    const auto delayConstant = process.constant(delayValue);
    const auto finalConstant = process.constant("4'b0011");
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(delayConstant)));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(finalValue),
                                   Operand::constant(finalConstant)));
    process.readObject(value, 0);
    process.store(1, value);
    process.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    process.block("resume");
    process.store(1, finalValue);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design design;
    design.addModule(std::move(top));
    return design;
}

TEST(ExecutionHelpers, FourStateStoreAndTimedResumeMatchInterpreter) {
    Simulation selected(waitDesign("32'd1"), "tb", std::make_unique<WaitBody>());
    Simulation reference(waitDesign("32'd1"), "tb");
    ASSERT_TRUE(selected.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(selected.run(0), RunStatus::TimeLimitReached);
    EXPECT_EQ(reference.run(0), RunStatus::TimeLimitReached);
    EXPECT_EQ(selected.value("/tb/output"), "1xz0");
    EXPECT_EQ(selected.value("/tb/output"), reference.value("/tb/output"));
    EXPECT_EQ(selected.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
    EXPECT_EQ(selected.run(), RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), RunStatus::Quiescent);
    EXPECT_EQ(selected.value("/tb/output"), "0011");
    EXPECT_EQ(selected.value("/tb/output"), reference.value("/tb/output"));
    EXPECT_EQ(selected.time(), reference.time());
    EXPECT_EQ(selected.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
    EXPECT_EQ(selected.simulationKernel().totalDeltaCycles(),
              reference.simulationKernel().totalDeltaCycles());
}

TEST(ExecutionHelpers, UnknownDelayResumesAtZeroLikeInterpreter) {
    Simulation selected(waitDesign("32'bx"), "tb", std::make_unique<WaitBody>());
    Simulation reference(waitDesign("32'bx"), "tb");
    ASSERT_TRUE(selected.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(selected.run(), RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), RunStatus::Quiescent);
    EXPECT_EQ(selected.time(), 0U);
    EXPECT_EQ(selected.value("/tb/output"), reference.value("/tb/output"));
    EXPECT_EQ(selected.value("/tb/output"), "0011");
    EXPECT_EQ(selected.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
    EXPECT_EQ(selected.simulationKernel().totalDeltaCycles(),
              reference.simulationKernel().totalDeltaCycles());
}

class TaskBody final : public runtime::ExecutionBackend {
public:
    ExecutionResult run(ExecutionContext& context) override {
        EXPECT_EQ(runtime::enzanExecFrame(&context), nullptr);
        runtime::enzanExecCount(&context, 1);
        const auto result = static_cast<ExecutionResult>(runtime::enzanExecSystemTask(&context, 0));
        if (result == ExecutionResult::Completed) runtime::enzanExecCount(&context, 1);
        return result;
    }

    ExecutionResult call(ExecutionContext& /*context*/) override {
        ADD_FAILURE() << "Unexpected subprogram call";
        return ExecutionResult::Stopped;
    }
};

eir::Design taskDesign(eir::SysTaskKind kind) {
    eir::ModuleDefinition top;
    top.name = "tb";
    ProcessBuilder process(ProcessKind::Initial, "task");
    eir::SysTaskCall task;
    task.kind = kind;
    if (kind == eir::SysTaskKind::Fatal || kind == eir::SysTaskKind::Display)
        task.formatIndex = process.string("test message");
    const auto descriptor = process.sysCall(std::move(task));
    process.block("entry");
    process.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design design;
    design.addModule(std::move(top));
    return design;
}

TEST(ExecutionHelpers, SystemTasksContinueOrStopLikeInterpreter) {
    for (auto kind :
         {eir::SysTaskKind::Display, eir::SysTaskKind::Finish, eir::SysTaskKind::Fatal}) {
        Simulation selected(taskDesign(kind), "tb", std::make_unique<TaskBody>());
        Simulation reference(taskDesign(kind), "tb");
        ASSERT_TRUE(selected.ready());
        ASSERT_TRUE(reference.ready());
        const RunStatus expected = kind == eir::SysTaskKind::Finish  ? RunStatus::Finished
                                   : kind == eir::SysTaskKind::Fatal ? RunStatus::Error
                                                                     : RunStatus::Quiescent;
        EXPECT_EQ(selected.run(), expected);
        EXPECT_EQ(reference.run(), expected);
        EXPECT_EQ(selected.simulationKernel().exitStatus(),
                  reference.simulationKernel().exitStatus());
        EXPECT_EQ(selected.simulationKernel().executedInstructions(),
                  kind == eir::SysTaskKind::Display ? 2U : 1U);
        EXPECT_EQ(selected.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(selected.messages(), reference.messages());
        EXPECT_EQ(selected.transcript(), reference.transcript());
    }
}

class ResolvedBody final : public runtime::ExecutionBackend {
public:
    ExecutionResult run(ExecutionContext& context) override {
        // The source object is a constant of each compiled copy body.
        const auto sourceObject = context.activation().body->code[0].a;
        const auto* source = runtime::enzanExecReadObject(&context, sourceObject);
        auto* frame = runtime::enzanExecFrame(&context);
        frame[0] = source[0];
        frame[1] = source[1];
        runtime::enzanExecCount(&context, 2);
        ++depth;
        runtime::enzanExecStore(&context, 2, 0, 0, 1);
        --depth;
        runtime::enzanExecCount(&context, 1);
        return ExecutionResult::Completed;
    }

    ExecutionResult call(ExecutionContext& context) override {
        if (depth != 0) ++nestedCalls;
        // The resolver's result is a frame-initialized constant; its only instruction is return.
        runtime::enzanExecCount(&context, 1);
        return ExecutionResult::Completed;
    }

    std::uint32_t nestedCalls = 0;
    std::uint32_t depth = 0;
};

eir::Design resolvedDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.language = eir::Language::Vhdl;
    top.addObject(
        Object{.name = "a", .type = Type::logic(), .initialValue = test::literal("1'b0")});
    top.addObject(
        Object{.name = "b", .type = Type::logic(), .initialValue = test::literal("1'b1")});
    Object resolved{.name = "resolved", .type = Type::logic()};
    resolved.resolution = {.function = "WORK.TEST.RESOLVE", .elementBits = 1};
    top.addObject(std::move(resolved));
    for (std::uint32_t source = 0; source < 2; ++source) {
        ProcessBuilder process(ProcessKind::ContinuousAssign, "copy");
        const auto value = process.reg(Type::logic());
        process.block("entry");
        process.readObject(value, source);
        process.store(2, value);
        process.emit(Instruction::make(Opcode::Return));
        auto body = process.finish();
        body.drivesSignals = true;
        top.processes.push_back(std::move(body));
    }
    test::SubprogramBuilder resolver("resolve");
    resolver.parameter("drivers", Type::bits(2, false, true));
    const auto result = resolver.result(Type::logic());
    const auto one = resolver.constant("1'b1");
    resolver.block("entry");
    resolver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(result), Operand::constant(one)));
    resolver.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(resolver.finish());
    eir::Design design;
    design.addModule(std::move(top));
    return design;
}

TEST(ExecutionHelpers, ResolvedStoreCanReenterTheBackend) {
    const elab::ResolutionAnswers answers = {{{"WORK.TEST.RESOLVE", 2}, {"tb", "resolve"}}};
    auto backend = std::make_unique<ResolvedBody>();
    const auto* observed = backend.get();
    Simulation selected(resolvedDesign(), "tb", std::move(backend), answers);
    Simulation reference(resolvedDesign(), "tb", {}, answers);
    ASSERT_TRUE(selected.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(selected.run(), RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), RunStatus::Quiescent);
    EXPECT_EQ(selected.value("/tb/resolved"), "1");
    EXPECT_EQ(selected.value("/tb/resolved"), reference.value("/tb/resolved"));
    EXPECT_EQ(selected.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
    EXPECT_GT(observed->nestedCalls, 0U);
}

} // namespace
