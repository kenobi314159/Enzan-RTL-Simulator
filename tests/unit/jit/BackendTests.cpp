// SPDX-License-Identifier: GPL-3.0-or-later

#include "jit/Backend.h"

#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "runtime/Execution.h"
#include "support/Literals.h"
#include "support/ProcessBuilder.h"
#include "support/Simulation.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

using namespace enzan;
using eir::Instruction;
using eir::Opcode;
using eir::Operand;

eir::Design design(std::string_view condition, std::string_view left, std::string_view right,
                   bool unsupported = false) {
    eir::ModuleDefinition top;
    top.name = "tb";
    for (const auto& [name, value] :
         {std::pair{"condition", condition}, std::pair{"left", left}, std::pair{"right", right}})
        top.addObject(eir::Object{.name = name,
                                  .type = eir::Type::bits(4, false, true),
                                  .initialValue = test::literal(value)});
    top.addObject(eir::Object{.name = "output", .type = eir::Type::bits(4, false, true)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "driver");
    const auto cond = process.reg(eir::Type::bits(4, false, true));
    const auto lhs = process.reg(eir::Type::bits(4, false, true));
    const auto rhs = process.reg(eir::Type::bits(4, false, true));
    const auto add = process.reg(eir::Type::bits(4, false, true));
    const auto mul = process.reg(eir::Type::bits(4, false, true));
    const auto old = process.reg(eir::Type::bits(4, false, true));
    const auto one = process.reg(eir::Type::bits(4, false, true));
    const auto difference = process.reg(eir::Type::bits(4, false, true));
    const auto delay = process.reg(eir::Type::bits(32, false, true));
    const auto oneConstant = process.constant("4'd1");
    const auto delayConstant = process.constant("32'd1");
    process.block("entry");
    process.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(one), Operand::constant(oneConstant)));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(delayConstant)));
    process.readObject(cond, 0);
    process.emit(Instruction::make(Opcode::BranchIf, {}, Operand::reg(cond), Operand::block(1),
                                   Operand::block(2)));
    process.block("add");
    process.readObject(lhs, 1);
    process.readObject(rhs, 2);
    process.emit(Instruction::make(unsupported ? Opcode::Divide : Opcode::Add, Operand::reg(add),
                                   Operand::reg(lhs), Operand::reg(rhs)));
    process.store(3, add);
    process.emit(Instruction::make(Opcode::Jump, {}, Operand::block(3)));
    process.block("multiply");
    process.readObject(lhs, 1);
    process.readObject(rhs, 2);
    process.emit(Instruction::make(Opcode::Multiply, Operand::reg(mul), Operand::reg(lhs),
                                   Operand::reg(rhs)));
    process.store(3, mul);
    process.emit(Instruction::make(Opcode::Jump, {}, Operand::block(3)));
    process.block("wait");
    process.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(4)));
    process.block("resume");
    process.readObject(old, 3);
    process.emit(Instruction::make(Opcode::Subtract, Operand::reg(difference), Operand::reg(old),
                                   Operand::reg(one)));
    process.store(3, difference);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, BranchArithmeticWaitAndResumeMatchInterpreter) {
    for (const auto& [condition, left, right] :
         {std::tuple{"4'b1x00", "4'd15", "4'd3"}, std::tuple{"4'b00x0", "4'd3", "4'd4"},
          std::tuple{"4'b1000", "4'b10z1", "4'd2"}}) {
        test::Simulation native(design(condition, left, right), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(design(condition, left, right), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(0), runtime::RunStatus::TimeLimitReached);
        EXPECT_EQ(reference.run(0), runtime::RunStatus::TimeLimitReached);
        EXPECT_EQ(native.value("/tb/output"), reference.value("/tb/output"));
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(native.value("/tb/output"), reference.value("/tb/output"));
        EXPECT_EQ(native.time(), reference.time());
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
                  reference.simulationKernel().totalDeltaCycles());
    }
}

TEST(JitBackend, UnsupportedInstructionFailsBeforeKernelInitialization) {
    const test::Simulation native(design("4'd1", "4'd2", "4'd3", true), "tb", {}, {},
                                  jit::makeBackend);
    EXPECT_FALSE(native.ready());
    ASSERT_FALSE(native.messages().empty());
    EXPECT_NE(native.messages().back().find("div"), std::string::npos);
    EXPECT_TRUE(native.transcript().empty());
}

eir::Design wideDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(
        eir::Object{.name = "input",
                    .type = eir::Type::bits(130, false, true),
                    .initialValue = test::literal("130'h20000000000000000000000000000000x")});
    top.addObject(eir::Object{.name = "output", .type = eir::Type::bits(130, false, true)});
    top.addObject(eir::Object{.name = "choice", .type = eir::Type::bits(4, false, true)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "wide");
    const auto source = process.reg(eir::Type::bits(130, false, true));
    const auto moved = process.reg(eir::Type::bits(130, false, true));
    const auto constant = process.reg(eir::Type::bits(4, false, true));
    const auto first = process.constant("4'b1xz0");
    const auto second = process.constant("4'b0101");
    process.block("entry");
    process.readObject(source, 0);
    process.emit(Instruction::make(Opcode::Move, Operand::reg(moved), Operand::reg(source)));
    process.store(1, moved);
    process.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(constant), Operand::constant(first)));
    process.emit(Instruction::make(Opcode::BranchIf, {}, Operand::reg(moved), Operand::block(1),
                                   Operand::block(2)));
    process.block("true");
    process.store(2, constant);
    process.emit(Instruction::make(Opcode::Return));
    process.block("false");
    process.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(constant), Operand::constant(second)));
    process.store(2, constant);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, WideMovesBranchesAndRuntimeConstantsMatchInterpreter) {
    test::Simulation native(wideDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(wideDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/output"), reference.value("/tb/output"));
    EXPECT_EQ(native.value("/tb/choice"), "1xz0");
    EXPECT_EQ(native.value("/tb/choice"), reference.value("/tb/choice"));
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

eir::Design taskDesign(eir::SysTaskKind kind) {
    eir::ModuleDefinition top;
    top.name = "tb";
    test::ProcessBuilder process(eir::ProcessKind::Initial, "task");
    eir::SysTaskCall task;
    task.kind = kind;
    if (kind == eir::SysTaskKind::Display || kind == eir::SysTaskKind::Fatal)
        task.formatIndex = process.string("message");
    const auto descriptor = process.sysCall(std::move(task));
    process.block("entry");
    process.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, SystemTaskContinuationAndStopMatchInterpreter) {
    for (const auto kind :
         {eir::SysTaskKind::Display, eir::SysTaskKind::Finish, eir::SysTaskKind::Fatal}) {
        test::Simulation native(taskDesign(kind), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(taskDesign(kind), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), reference.run());
        EXPECT_EQ(native.transcript(), reference.transcript());
        EXPECT_EQ(native.messages(), reference.messages());
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design eventDesign(bool eventList) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(eir::Object{
        .name = "clock", .type = eir::Type::logic(), .initialValue = test::literal("1'b0")});
    top.addObject(eir::Object{
        .name = "output", .type = eir::Type::logic(), .initialValue = test::literal("1'b0")});
    test::ProcessBuilder clock(eir::ProcessKind::Initial, "clock");
    const auto delay = clock.reg(eir::Type::bits(32, false, true));
    const auto high = clock.reg(eir::Type::logic());
    const auto oneTime = clock.constant("32'd1");
    const auto oneBit = clock.constant("1'b1");
    clock.block("entry");
    clock.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(oneTime)));
    clock.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(high), Operand::constant(oneBit)));
    clock.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    clock.block("resume");
    clock.store(0, high);
    clock.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(clock.finish());

    test::ProcessBuilder waiter(eir::ProcessKind::Initial, "waiter");
    const auto writeValue = waiter.reg(eir::Type::logic());
    const auto writeConstant = waiter.constant("1'b1");
    waiter.block("entry");
    waiter.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(writeValue),
                                  Operand::constant(writeConstant)));
    if (eventList)
        waiter.waitEvents({eir::SensitivityEntry{.objectIndex = 0, .edge = eir::EdgeKind::Posedge}},
                          1);
    else
        waiter.waitEdge(0, eir::EdgeKind::Posedge, 1);
    waiter.block("resume");
    waiter.storeNonBlocking(1, writeValue);
    waiter.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(waiter.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, EdgeAndEventWaitsScheduleNonblockingWritesLikeInterpreter) {
    for (const bool eventList : {false, true}) {
        test::Simulation native(eventDesign(eventList), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(eventDesign(eventList), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(0), runtime::RunStatus::TimeLimitReached);
        EXPECT_EQ(reference.run(0), runtime::RunStatus::TimeLimitReached);
        EXPECT_EQ(native.value("/tb/output"), "0");
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(native.value("/tb/output"), "1");
        EXPECT_EQ(native.value("/tb/output"), reference.value("/tb/output"));
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
                  reference.simulationKernel().totalDeltaCycles());
    }
}

eir::Design bitwiseDesign(std::uint32_t width, std::string_view left, std::string_view right) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto type = eir::Type::bits(width, false, true);
    top.addObject(eir::Object{.name = "left", .type = type, .initialValue = test::literal(left)});
    top.addObject(eir::Object{.name = "right", .type = type, .initialValue = test::literal(right)});
    for (const auto* name : {"and", "or", "xor", "xnor", "not", "two"})
        top.addObject(eir::Object{.name = name, .type = type});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "bitwise");
    const auto a = process.reg(type);
    const auto b = process.reg(type);
    process.block("entry");
    process.readObject(a, 0);
    process.readObject(b, 1);
    std::uint32_t object = 2;
    for (const auto opcode : {Opcode::And, Opcode::Or, Opcode::Xor, Opcode::Xnor, Opcode::Not,
                              Opcode::CoerceTwoState}) {
        const auto result = process.reg(type);
        const auto second = opcode == Opcode::Not || opcode == Opcode::CoerceTwoState
                                ? Operand::none()
                                : Operand::reg(b);
        process.emit(Instruction::make(opcode, Operand::reg(result), Operand::reg(a), second));
        process.store(object++, result);
    }
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, FourStateBitwiseAndTwoStateCoercionMatchInterpreter) {
    for (const auto& [width, left, right] :
         {std::tuple{4U, "4'b0x1z", "4'b10zz"}, std::tuple{4U, "4'b0000", "4'b01xz"},
          std::tuple{4U, "4'b1111", "4'b01xz"}, std::tuple{4U, "4'bxxxx", "4'b01xz"},
          std::tuple{4U, "4'bzzzz", "4'b01xz"},
          std::tuple{64U, "64'hf00000000000000x", "64'h100000000000000z"},
          std::tuple{70U, "70'h31x00000000000000z", "70'h1020000000000000x5"},
          std::tuple{4096U, "4096'hx1", "4096'hf"}}) {
        test::Simulation native(bitwiseDesign(width, left, right), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(bitwiseDesign(width, left, right), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        for (const auto* name : {"and", "or", "xor", "xnor", "not", "two"}) {
            const std::string path = std::string("/tb/") + name;
            EXPECT_EQ(native.value(path), reference.value(path)) << name;
        }
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design extensionDesign(std::string_view literal) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(eir::Object{.name = "source",
                              .type = eir::Type::bits(4, false, true),
                              .initialValue = test::literal(literal)});
    top.addObject(eir::Object{.name = "narrow", .type = eir::Type::bits(8, false, true)});
    top.addObject(eir::Object{.name = "wide", .type = eir::Type::bits(130, false, true)});
    top.addObject(eir::Object{.name = "truncated", .type = eir::Type::bits(8, false, true)});
    top.addObject(eir::Object{.name = "unsigned", .type = eir::Type::bits(8, false, true)});
    top.addObject(eir::Object{.name = "small", .type = eir::Type::bits(2, false, true)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "extend");
    const auto input = process.reg(eir::Type::bits(4, false, true));
    const auto narrow = process.reg(eir::Type::bits(8, false, true));
    const auto wide = process.reg(eir::Type::bits(130, false, true));
    const auto truncated = process.reg(eir::Type::bits(8, false, true));
    const auto unsignedValue = process.reg(eir::Type::bits(8, false, true));
    const auto small = process.reg(eir::Type::bits(2, false, true));
    process.block("entry");
    process.readObject(input, 0);
    process.emit(
        Instruction::makeSigned(Opcode::Extend, Operand::reg(narrow), Operand::reg(input)));
    process.emit(Instruction::makeSigned(Opcode::Extend, Operand::reg(wide), Operand::reg(input)));
    process.emit(Instruction::make(Opcode::Truncate, Operand::reg(truncated), Operand::reg(wide)));
    process.emit(
        Instruction::make(Opcode::Extend, Operand::reg(unsignedValue), Operand::reg(input)));
    process.emit(Instruction::make(Opcode::Truncate, Operand::reg(small), Operand::reg(input)));
    process.store(1, narrow);
    process.store(2, wide);
    process.store(3, truncated);
    process.store(4, unsignedValue);
    process.store(5, small);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, SignedExtensionsAndWideTruncationPreserveUnknownBits) {
    for (const auto* value : {"4'b0110", "4'b1010", "4'bx001", "4'bz001"}) {
        test::Simulation native(extensionDesign(value), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(extensionDesign(value), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        for (const auto* name : {"narrow", "wide", "truncated", "unsigned", "small"}) {
            const std::string path = std::string("/tb/") + name;
            EXPECT_EQ(native.value(path), reference.value(path)) << value << ' ' << name;
        }
    }
}

eir::Design comparisonDesign(std::uint32_t width, std::string_view left, std::string_view right,
                             bool isSigned) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto type = eir::Type::bits(width, false, true);
    top.addObject(eir::Object{.name = "left", .type = type, .initialValue = test::literal(left)});
    top.addObject(eir::Object{.name = "right", .type = type, .initialValue = test::literal(right)});
    for (const auto* name : {"eq", "ne", "ceq", "cne", "lt", "le"})
        top.addObject(eir::Object{.name = name, .type = eir::Type::logic()});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "compare");
    const auto a = process.reg(type);
    const auto b = process.reg(type);
    process.block("entry");
    process.readObject(a, 0);
    process.readObject(b, 1);
    std::uint32_t object = 2;
    for (const auto opcode : {Opcode::Equal, Opcode::NotEqual, Opcode::CaseEqual,
                              Opcode::CaseNotEqual, Opcode::LessThan, Opcode::LessEqual}) {
        const auto result = process.reg(eir::Type::logic());
        const auto instruction =
            isSigned
                ? Instruction::makeSigned(opcode, Operand::reg(result), Operand::reg(a),
                                          Operand::reg(b))
                : Instruction::make(opcode, Operand::reg(result), Operand::reg(a), Operand::reg(b));
        process.emit(instruction);
        process.store(object++, result);
    }
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, FourStateAndSignedComparisonsMatchInterpreter) {
    for (const auto& [width, left, right] :
         {std::tuple{4U, "4'b0x1z", "4'b100z"}, std::tuple{4U, "4'b0x1z", "4'b001z"},
          std::tuple{4U, "4'b1000", "4'b0111"},
          std::tuple{64U, "64'h8000000000000000", "64'h7fffffffffffffff"},
          std::tuple{1U, "1'bx", "1'bz"}}) {
        for (const bool isSigned : {false, true}) {
            test::Simulation native(comparisonDesign(width, left, right, isSigned), "tb", {}, {},
                                    jit::makeBackend);
            test::Simulation reference(comparisonDesign(width, left, right, isSigned), "tb");
            ASSERT_TRUE(native.ready());
            ASSERT_TRUE(reference.ready());
            EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
            EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
            for (const auto* name : {"eq", "ne", "ceq", "cne", "lt", "le"}) {
                const std::string path = std::string("/tb/") + name;
                EXPECT_EQ(native.value(path), reference.value(path))
                    << left << ' ' << right << ' ' << name << " signed=" << isSigned;
            }
        }
    }
}

eir::Design shiftDesign(std::uint32_t width, std::string_view source, std::string_view amount) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto valueType = eir::Type::bits(width, false, true);
    const auto amountType = eir::Type::bits(8, false, true);
    top.addObject(
        eir::Object{.name = "source", .type = valueType, .initialValue = test::literal(source)});
    top.addObject(
        eir::Object{.name = "amount", .type = amountType, .initialValue = test::literal(amount)});
    for (const auto* name : {"left", "logical", "arithmetic"})
        top.addObject(eir::Object{.name = name, .type = valueType});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "shift");
    const auto value = process.reg(valueType);
    const auto count = process.reg(amountType);
    process.block("entry");
    process.readObject(value, 0);
    process.readObject(count, 1);
    std::uint32_t object = 2;
    for (const auto opcode :
         {Opcode::ShiftLeft, Opcode::ShiftRightLogical, Opcode::ShiftRightArithmetic}) {
        const auto result = process.reg(valueType);
        process.emit(Instruction::make(opcode, Operand::reg(result), Operand::reg(value),
                                       Operand::reg(count)));
        process.store(object++, result);
    }
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, ShiftsPreserveFourStateValuesAndHandleLargeAmounts) {
    for (const auto& [width, source, amount] :
         {std::tuple{4U, "4'b1010", "8'd0"}, std::tuple{4U, "4'b1010", "8'd3"},
          std::tuple{4U, "4'bz101", "8'd4"}, std::tuple{4U, "4'bx101", "8'd255"},
          std::tuple{4U, "4'b1010", "8'bx"}, std::tuple{64U, "64'h8000000000000001", "8'd63"},
          std::tuple{64U, "64'hz000000000000001", "8'd64"}}) {
        test::Simulation native(shiftDesign(width, source, amount), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(shiftDesign(width, source, amount), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        for (const auto* name : {"left", "logical", "arithmetic"}) {
            const std::string path = std::string("/tb/") + name;
            EXPECT_EQ(native.value(path), reference.value(path))
                << source << ' ' << amount << ' ' << name;
        }
    }
}

} // namespace
