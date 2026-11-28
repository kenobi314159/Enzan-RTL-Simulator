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
    if (unsupported)
        process.emit(Instruction::make(Opcode::Power, Operand::reg(add), Operand::reg(lhs),
                                       Operand::reg(rhs), Operand::immediate(0)));
    else
        process.emit(Instruction::make(Opcode::Add, Operand::reg(add), Operand::reg(lhs),
                                       Operand::reg(rhs)));
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
    EXPECT_NE(native.messages().back().find("pow"), std::string::npos);
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
    for (const auto* name : {"eq", "ne", "ceq", "cne", "lt", "le", "weq", "wne", "casez", "casex"})
        top.addObject(eir::Object{.name = name, .type = eir::Type::logic()});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "compare");
    const auto a = process.reg(type);
    const auto b = process.reg(type);
    process.block("entry");
    process.readObject(a, 0);
    process.readObject(b, 1);
    std::uint32_t object = 2;
    for (const auto opcode :
         {Opcode::Equal, Opcode::NotEqual, Opcode::CaseEqual, Opcode::CaseNotEqual,
          Opcode::LessThan, Opcode::LessEqual, Opcode::WildcardEqual, Opcode::WildcardNotEqual,
          Opcode::CaseZMatch, Opcode::CaseXMatch}) {
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
          std::tuple{70U, "70'h200000000000000000", "70'h1fffffffffffffffff"},
          std::tuple{70U, "70'hx0000000000000001", "70'h00000000000000002"},
          std::tuple{130U, "130'h20000000000000000000000000000000x",
                     "130'h20000000000000000000000000000000z"},
          std::tuple{130U, "130'h000000000000000000000000000000000",
                     "130'h000000000000000000000000000000000"},
          std::tuple{1U, "1'bx", "1'bz"}}) {
        for (const bool isSigned : {false, true}) {
            test::Simulation native(comparisonDesign(width, left, right, isSigned), "tb", {}, {},
                                    jit::makeBackend);
            test::Simulation reference(comparisonDesign(width, left, right, isSigned), "tb");
            ASSERT_TRUE(native.ready());
            ASSERT_TRUE(reference.ready());
            EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
            EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
            for (const auto* name :
                 {"eq", "ne", "ceq", "cne", "lt", "le", "weq", "wne", "casez", "casex"}) {
                const std::string path = std::string("/tb/") + name;
                EXPECT_EQ(native.value(path), reference.value(path))
                    << left << ' ' << right << ' ' << name << " signed=" << isSigned;
            }
        }
    }
}

eir::Design realComparisonDesign(std::string_view left, std::string_view right) {
    eir::ModuleDefinition top;
    top.name = "tb";
    for (const auto* name : {"eq", "ne", "lt", "le"})
        top.addObject(eir::Object{.name = name, .type = eir::Type::logic()});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "compare_real");
    const auto a = process.reg(eir::Type::real());
    const auto b = process.reg(eir::Type::real());
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(a),
                                   Operand::constant(process.constant(left))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(b),
                                   Operand::constant(process.constant(right))));
    std::uint32_t object = 0;
    for (const auto opcode :
         {Opcode::Equal, Opcode::NotEqual, Opcode::LessThan, Opcode::LessEqual}) {
        const auto result = process.reg(eir::Type::logic());
        process.emit(
            Instruction::make(opcode, Operand::reg(result), Operand::reg(a), Operand::reg(b)));
        process.store(object++, result);
    }
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, RealComparisonsMatchInterpreterIncludingNanAndSignedZero) {
    for (const auto& [left, right, expected] :
         {std::tuple{"64'h0000000000000000", "64'h8000000000000000", "1001"},
          std::tuple{"64'h3ff0000000000000", "64'h4000000000000000", "0111"},
          std::tuple{"64'h7ff0000000000000", "64'h3ff0000000000000", "0100"},
          std::tuple{"64'h7ff8000000000000", "64'h3ff0000000000000", "0100"}}) {
        test::Simulation native(realComparisonDesign(left, right), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(realComparisonDesign(left, right), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        std::size_t index = 0;
        for (const auto* name : {"eq", "ne", "lt", "le"}) {
            const std::string path = std::string("/tb/") + name;
            EXPECT_EQ(native.value(path), reference.value(path))
                << left << ' ' << right << ' ' << name;
            EXPECT_EQ(native.value(path), std::string(1, expected[index++]));
        }
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design shiftDesign(std::uint32_t width, std::string_view source, std::string_view amount,
                        std::uint32_t amountWidth = 8) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto valueType = eir::Type::bits(width, false, true);
    const auto amountType = eir::Type::bits(amountWidth, false, true);
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

eir::Design resolvedDesign(bool throughCall = false, bool partial = false) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.language = eir::Language::Vhdl;
    top.addObject(eir::Object{
        .name = "a", .type = eir::Type::logic(), .initialValue = test::literal("1'b0")});
    top.addObject(eir::Object{
        .name = "b", .type = eir::Type::logic(), .initialValue = test::literal("1'b1")});
    eir::Object resolved{.name = "resolved", .type = eir::Type::logic()};
    resolved.resolution = {.function = "WORK.TEST.RESOLVE", .elementBits = 1};
    top.addObject(std::move(resolved));
    for (std::uint32_t source = 0; source < 2; ++source) {
        test::ProcessBuilder process(eir::ProcessKind::ContinuousAssign, "copy");
        const auto value = process.reg(eir::Type::logic());
        const auto offset = process.reg(eir::Type::bits(8, false, false));
        process.block("entry");
        process.readObject(value, source);
        if (throughCall)
            process.call("", "write_resolved", {Operand::reg(value)});
        else if (partial) {
            process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(offset),
                                           Operand::constant(process.constant("8'd0"))));
            process.storePartNonBlocking(2, value, offset, 1);
        } else
            process.store(2, value);
        process.emit(Instruction::make(Opcode::Return));
        auto body = process.finish();
        if (partial && !throughCall) body.blocks[0].instructions[2].opcode = Opcode::StorePart;
        body.drivesSignals = true;
        top.processes.push_back(std::move(body));
    }
    test::SubprogramBuilder resolver("resolve");
    resolver.parameter("drivers", eir::Type::bits(2, false, true));
    const auto result = resolver.result(eir::Type::logic());
    resolver.block("entry");
    resolver.call("", "resolved_value", {}, Operand::reg(result));
    resolver.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(resolver.finish());
    test::SubprogramBuilder value("resolved_value");
    const auto resolvedValue = value.result(eir::Type::logic());
    value.block("entry");
    value.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(resolvedValue),
                                 Operand::constant(value.constant("1'b1"))));
    value.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(value.finish());
    if (throughCall) {
        test::SubprogramBuilder writer("write_resolved");
        const auto input = writer.parameter("input", eir::Type::logic());
        const auto offset = writer.reg(eir::Type::bits(8, false, false));
        writer.block("entry");
        if (partial) {
            writer.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(offset),
                                          Operand::constant(writer.constant("8'd0"))));
            writer.storePartNonBlocking(2, input, offset, 1);
        } else {
            writer.store(2, input);
        }
        writer.emit(Instruction::make(Opcode::Return));
        auto writerBody = writer.finish();
        if (partial) writerBody.blocks[0].instructions[1].opcode = Opcode::StorePart;
        top.subprograms.push_back(std::move(writerBody));
    }
    eir::Design resultDesign;
    resultDesign.addModule(std::move(top));
    return resultDesign;
}

TEST(JitBackend, ResolvedStoresReenterNativeCodeAndKeepInstructionCounts) {
    const elab::ResolutionAnswers answers = {{{"WORK.TEST.RESOLVE", 2}, {"tb", "resolve"}}};
    for (const auto& [throughCall, partial] : {std::pair{false, false}, std::pair{true, false},
                                               std::pair{false, true}, std::pair{true, true}}) {
        test::Simulation native(resolvedDesign(throughCall, partial), "tb", {}, answers,
                                jit::makeBackend);
        test::Simulation reference(resolvedDesign(throughCall, partial), "tb", {}, answers);
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(native.value("/tb/resolved"), "1");
        EXPECT_EQ(native.value("/tb/resolved"), reference.value("/tb/resolved"));
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
                  reference.simulationKernel().totalDeltaCycles());
    }
}

eir::Design encodingDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto codes = eir::Type::bits(32, false, false);
    const auto bits = eir::Type::bits(4, false, true);
    top.addObject(eir::Object{.name = "presented", .type = bits});
    for (const auto* name : {"logic", "bit", "weak", "weak_one"})
        top.addObject(eir::Object{.name = name, .type = codes});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "encoding");
    const auto stored = process.reg(codes);
    const auto presented = process.reg(bits);
    const auto shown = process.reg(bits);
    const auto logic = process.reg(codes);
    const auto bit = process.reg(codes);
    const auto weak = process.reg(codes);
    const auto weakOne = process.reg(codes);
    const auto vhdlLogic = Operand::immediate(static_cast<std::uint32_t>(eir::Encoding::VhdlLogic));
    const auto vhdlBit = Operand::immediate(static_cast<std::uint32_t>(eir::Encoding::VhdlBit));
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(stored),
                                   Operand::constant(process.constant("32'h03020704"))));
    process.emit(Instruction::make(Opcode::Present, Operand::reg(presented), Operand::reg(stored),
                                   vhdlLogic));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(shown),
                                   Operand::constant(process.constant("4'b10xz"))));
    process.emit(Instruction::make(Opcode::Encode, Operand::reg(logic), Operand::reg(shown),
                                   vhdlLogic, Operand::immediate(0)));
    process.emit(Instruction::make(Opcode::Encode, Operand::reg(bit), Operand::reg(shown), vhdlBit,
                                   Operand::immediate(0)));
    process.emit(Instruction::make(Opcode::Encode, Operand::reg(weak), Operand::reg(shown),
                                   vhdlLogic, Operand::immediate(eir::WeakZero | eir::WeakOne)));
    process.emit(Instruction::make(Opcode::Encode, Operand::reg(weakOne), Operand::reg(shown),
                                   vhdlLogic, Operand::immediate(eir::WeakOne)));
    process.store(0, presented);
    process.store(1, logic);
    process.store(2, bit);
    process.store(3, weak);
    process.store(4, weakOne);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, VhdlBoundaryEncodingsMatchInterpreter) {
    test::Simulation native(encodingDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(encodingDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    for (const auto* name : {"presented", "logic", "bit", "weak", "weak_one"}) {
        const std::string path = std::string("/tb/") + name;
        EXPECT_EQ(native.value(path), reference.value(path)) << name;
    }
    EXPECT_EQ(native.value("/tb/presented"), "101z");
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

eir::Design driveDesign(bool inertial) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.language = eir::Language::Vhdl;
    top.addObject(eir::Object{.name = "signal", .type = eir::Type::bits(8, false, false)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "driver");
    const auto value = process.reg(eir::Type::bits(8, false, false));
    const auto delay = process.reg(eir::Type::bits(64, false, false));
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(value),
                                   Operand::constant(process.constant("8'd1"))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(process.constant("64'd10"))));
    process.drive(0, value, delay, inertial);
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(value),
                                   Operand::constant(process.constant("8'd2"))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(process.constant("64'd5"))));
    process.drive(0, value, delay, inertial);
    process.emit(Instruction::make(Opcode::Return));
    auto body = process.finish();
    body.drivesSignals = true;
    top.processes.push_back(std::move(body));
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, VhdlDriverTransactionsMatchInterpreter) {
    for (const bool inertial : {false, true}) {
        test::Simulation native(driveDesign(inertial), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(driveDesign(inertial), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(native.value("/tb/signal", eir::Radix::Decimal), "2");
        EXPECT_EQ(native.value("/tb/signal"), reference.value("/tb/signal"));
        EXPECT_EQ(native.time(), reference.time());
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
        EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
                  reference.simulationKernel().totalDeltaCycles());
    }
}

eir::Design driveOverflowDesign(bool partial = false) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.language = eir::Language::Vhdl;
    top.addObject(eir::Object{.name = "signal", .type = eir::Type::bits(8, false, false)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "overflow");
    const auto value = process.reg(eir::Type::bits(8, false, false));
    const auto delay = process.reg(eir::Type::bits(64, false, false));
    const auto offsetAndDelay = process.reg(eir::Type::bits(128, false, false));
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(process.constant("64'd1"))));
    process.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    process.block("resume");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(process.constant("64'hffffffffffffffff"))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(value),
                                   Operand::constant(process.constant("8'd1"))));
    if (partial) {
        process.emit(Instruction::make(
            Opcode::LoadConstant, Operand::reg(offsetAndDelay),
            Operand::constant(process.constant("128'hffffffffffffffff0000000000000000"))));
        process.drivePart(0, value, offsetAndDelay, 8);
    } else {
        process.drive(0, value, delay);
    }
    process.emit(Instruction::make(Opcode::Return));
    auto body = process.finish();
    if (partial) body.blocks[1].instructions[3].opcode = Opcode::DrivePartAfter;
    body.drivesSignals = true;
    top.processes.push_back(std::move(body));
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, VhdlDriverTimeOverflowStopsLikeInterpreter) {
    for (const bool partial : {false, true}) {
        test::Simulation native(driveOverflowDesign(partial), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(driveOverflowDesign(partial), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Error);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Error);
        EXPECT_EQ(native.messages(), reference.messages());
        EXPECT_EQ(native.value("/tb/signal"), reference.value("/tb/signal"));
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design selectionDesign(std::string_view indexValue, std::uint32_t width) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto sourceType = eir::Type::bits(width, false, true);
    const auto indexType = eir::Type::bits(width <= 64 ? 8 : 70, false, true);
    const auto partType = eir::Type::bits(width <= 64 ? width : 70, false, true);
    const auto bitType = eir::Type::logic();
    std::string_view sourceValue = "130'h2x123456789abcdef0123456789abcde0z";
    std::string_view replacementValue = "70'hx00ab1234567890fed";
    if (width == 8) {
        sourceValue = "8'b10xz0011";
        replacementValue = "8'b01zx1100";
    } else if (width == 64) {
        sourceValue = "64'hx123456789abcdef";
        replacementValue = "64'hz00ab1234567890f";
    }
    top.addObject(eir::Object{
        .name = "source", .type = sourceType, .initialValue = test::literal(sourceValue)});
    top.addObject(
        eir::Object{.name = "index", .type = indexType, .initialValue = test::literal(indexValue)});
    top.addObject(eir::Object{.name = "part", .type = partType});
    top.addObject(eir::Object{.name = "bit", .type = bitType});
    top.addObject(eir::Object{.name = "splice", .type = sourceType});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "select");
    const auto source = process.reg(sourceType);
    const auto index = process.reg(indexType);
    const auto part = process.reg(partType);
    const auto bit = process.reg(bitType);
    const auto spliced = process.reg(sourceType);
    const auto replacement = process.reg(partType);
    process.block("entry");
    process.readObject(source, 0);
    process.readObject(index, 1);
    process.emit(Instruction::make(Opcode::PartSelect, Operand::reg(part), Operand::reg(source),
                                   Operand::reg(index)));
    process.emit(Instruction::make(Opcode::BitSelect, Operand::reg(bit), Operand::reg(source),
                                   Operand::reg(index)));
    process.emit(Instruction::make(Opcode::Move, Operand::reg(spliced), Operand::reg(source)));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(replacement),
                                   Operand::constant(process.constant(replacementValue))));
    process.emit(Instruction::make(Opcode::SplicePart, Operand::reg(spliced),
                                   Operand::reg(replacement), Operand::reg(index)));
    process.store(2, part);
    process.store(3, bit);
    process.store(4, spliced);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, SelectionsAndSplicesMatchInterpreter) {
    for (const auto& [index, width] :
         {std::pair{"8'd0", 8U}, std::pair{"8'd1", 8U}, std::pair{"8'd7", 8U},
          std::pair{"8'd8", 8U}, std::pair{"8'd64", 8U}, std::pair{"8'hx", 8U},
          std::pair{"8'd0", 64U}, std::pair{"8'd1", 64U}, std::pair{"8'd63", 64U},
          std::pair{"8'd64", 64U}, std::pair{"8'hx", 64U}, std::pair{"70'd0", 130U},
          std::pair{"70'd1", 130U}, std::pair{"70'd63", 130U}, std::pair{"70'd64", 130U},
          std::pair{"70'd129", 130U}, std::pair{"70'd130", 130U},
          std::pair{"70'h10000000000000000", 130U}, std::pair{"70'hx", 130U}}) {
        test::Simulation native(selectionDesign(index, width), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(selectionDesign(index, width), "tb");
        ASSERT_TRUE(native.ready()) << index;
        ASSERT_TRUE(reference.ready()) << index;
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        for (const auto* name : {"part", "bit", "splice"}) {
            const std::string path = std::string("/tb/") + name;
            EXPECT_EQ(native.value(path), reference.value(path)) << index << ' ' << name;
        }
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design callDesign(bool endless, bool finish) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto word = eir::Type::bits(16, false, true);
    top.addObject(eir::Object{.name = "result", .type = word});
    top.addObject(eir::Object{.name = "total", .type = word});
    top.addObject(eir::Object{.name = "produced", .type = word});

    test::SubprogramBuilder factorial("factorial");
    const auto n = factorial.parameter("n", word);
    const auto result = factorial.result(word);
    const auto one = factorial.reg(word);
    const auto base = factorial.reg(eir::Type::logic());
    const auto smaller = factorial.reg(word);
    const auto product = factorial.reg(word);
    factorial.block("entry");
    if (finish) {
        eir::SysTaskCall task;
        task.kind = eir::SysTaskKind::Finish;
        const auto taskIndex = factorial.sysCall(std::move(task));
        factorial.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(taskIndex)));
    }
    if (!endless) {
        factorial.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(one),
                                         Operand::constant(factorial.constant("16'd1"))));
        factorial.emit(Instruction::make(Opcode::LessEqual, Operand::reg(base), Operand::reg(n),
                                         Operand::reg(one)));
        factorial.emit(Instruction::make(Opcode::BranchIf, {}, Operand::reg(base),
                                         Operand::block(1), Operand::block(2)));
        factorial.block("base");
        factorial.emit(Instruction::make(Opcode::Move, Operand::reg(result), Operand::reg(one)));
        factorial.emit(Instruction::make(Opcode::Return));
        factorial.block("recurse");
        factorial.emit(Instruction::make(Opcode::Subtract, Operand::reg(smaller), Operand::reg(n),
                                         Operand::reg(one)));
    } else {
        factorial.emit(Instruction::make(Opcode::Move, Operand::reg(smaller), Operand::reg(n)));
    }
    factorial.call("", "factorial", {Operand::reg(smaller)}, Operand::reg(product));
    factorial.emit(Instruction::make(Opcode::Multiply, Operand::reg(result), Operand::reg(n),
                                     Operand::reg(product)));
    factorial.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(factorial.finish());

    test::SubprogramBuilder accumulate("accumulate");
    const auto total = accumulate.parameter("total", word, eir::ParameterMode::InOut);
    const auto step = accumulate.parameter("step", word);
    const auto sum = accumulate.reg(word);
    accumulate.block("entry");
    accumulate.emit(
        Instruction::make(Opcode::Add, Operand::reg(sum), Operand::reg(total), Operand::reg(step)));
    accumulate.emit(Instruction::make(Opcode::Move, Operand::reg(total), Operand::reg(sum)));
    accumulate.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(accumulate.finish());

    test::SubprogramBuilder produce("produce");
    const auto produced = produce.parameter("produced", word, eir::ParameterMode::Out);
    produce.block("entry");
    produce.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(produced),
                                   Operand::constant(produce.constant("16'd9"))));
    produce.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(produce.finish());

    test::ProcessBuilder process(eir::ProcessKind::Initial, "caller");
    const auto argument = process.reg(word);
    const auto answer = process.reg(word);
    const auto running = process.reg(word);
    const auto output = process.reg(word);
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(argument),
                                   Operand::constant(process.constant("16'd5"))));
    process.call("", "factorial", {Operand::reg(argument)}, Operand::reg(answer));
    process.call("", "accumulate", {Operand::reg(running), Operand::reg(answer)});
    process.call("", "accumulate",
                 {Operand::reg(running), Operand::constant(process.constant("16'd3"))});
    process.call("", "produce", {Operand::reg(output)});
    process.store(0, answer);
    process.store(1, running);
    process.store(2, output);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design design;
    design.addModule(std::move(top));
    return design;
}

TEST(JitBackend, RecursiveCallsAndParameterCopyBackMatchInterpreter) {
    test::Simulation native(callDesign(false, false), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(callDesign(false, false), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/result", eir::Radix::Decimal), "120");
    EXPECT_EQ(native.value("/tb/total", eir::Radix::Decimal), "123");
    EXPECT_EQ(native.value("/tb/produced", eir::Radix::Decimal), "9");
    EXPECT_EQ(native.value("/tb/result"), reference.value("/tb/result"));
    EXPECT_EQ(native.value("/tb/total"), reference.value("/tb/total"));
    EXPECT_EQ(native.value("/tb/produced"), reference.value("/tb/produced"));
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

TEST(JitBackend, RecursionLimitStopsWithoutUsingTheHostCallStack) {
    test::Simulation native(callDesign(true, false), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(callDesign(true, false), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Error);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Error);
    EXPECT_EQ(native.messages(), reference.messages());
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

TEST(JitBackend, FinishInsideCallStopsLikeInterpreter) {
    test::Simulation native(callDesign(false, true), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(callDesign(false, true), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), reference.run());
    EXPECT_EQ(native.messages(), reference.messages());
    EXPECT_EQ(native.value("/tb/result"), reference.value("/tb/result"));
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

eir::Design partialDriveDesign(Opcode opcode, std::string_view offsetAndDelay) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.language = eir::Language::Vhdl;
    top.addObject(eir::Object{.name = "signal",
                              .type = eir::Type::bits(8, false, true),
                              .initialValue = test::literal("8'h00")});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "partial_driver");
    const auto value = process.reg(eir::Type::bits(4, false, true));
    const auto argument =
        process.reg(eir::Type::bits(opcode == Opcode::DrivePart ? 64 : 128, false, true));
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(value),
                                   Operand::constant(process.constant("4'b10xz"))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(argument),
                                   Operand::constant(process.constant(offsetAndDelay))));
    process.drivePart(0, value, argument, 8);
    process.emit(Instruction::make(Opcode::Return));
    auto body = process.finish();
    body.blocks[0].instructions[2].opcode = opcode;
    body.drivesSignals = true;
    top.processes.push_back(std::move(body));
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, PartialVhdlDriverTransactionsMatchInterpreter) {
    for (const auto& [opcode, argument] :
         {std::pair{Opcode::DrivePart, "64'd0"}, std::pair{Opcode::DrivePart, "64'd3"},
          std::pair{Opcode::DrivePart, "64'd7"}, std::pair{Opcode::DrivePart, "64'd8"},
          std::pair{Opcode::DrivePart, "64'hx"},
          std::pair{Opcode::DrivePartAfter, "128'h00000000000000050000000000000003"},
          std::pair{Opcode::DriveTransportPart, "128'h00000000000000050000000000000003"},
          std::pair{Opcode::DrivePartAfter, "128'hx0000000000000050000000000000003"},
          std::pair{Opcode::DriveTransportPart, "128'h0000000000000005000000000000000x"}}) {
        test::Simulation native(partialDriveDesign(opcode, argument), "tb", {}, {},
                                jit::makeBackend);
        test::Simulation reference(partialDriveDesign(opcode, argument), "tb");
        ASSERT_TRUE(native.ready()) << argument;
        ASSERT_TRUE(reference.ready()) << argument;
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(native.value("/tb/signal"), reference.value("/tb/signal")) << argument;
        EXPECT_EQ(native.time(), reference.time()) << argument;
        EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
                  reference.simulationKernel().totalDeltaCycles());
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design overlappingDriveDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.language = eir::Language::Vhdl;
    top.addObject(eir::Object{.name = "signal", .type = eir::Type::bits(8, false, false)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "driver");
    const auto whole = process.reg(eir::Type::bits(8, false, false));
    const auto part = process.reg(eir::Type::bits(4, false, false));
    const auto delay = process.reg(eir::Type::bits(64, false, false));
    const auto offset = process.reg(eir::Type::bits(64, false, false));
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(whole),
                                   Operand::constant(process.constant("8'h11"))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(part),
                                   Operand::constant(process.constant("4'h2"))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(process.constant("64'd10"))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(offset),
                                   Operand::constant(process.constant("64'd0"))));
    process.drive(0, whole, delay, false);
    process.drivePart(0, part, offset, 8);
    process.emit(Instruction::make(Opcode::Return));
    auto body = process.finish();
    body.drivesSignals = true;
    top.processes.push_back(std::move(body));
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, PartialDriveCancelsOnlyTheOverlappingPendingBits) {
    test::Simulation native(overlappingDriveDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(overlappingDriveDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(5), runtime::RunStatus::TimeLimitReached);
    EXPECT_EQ(reference.run(5), runtime::RunStatus::TimeLimitReached);
    EXPECT_EQ(native.value("/tb/signal", eir::Radix::Hexadecimal), "02");
    EXPECT_EQ(native.value("/tb/signal"), reference.value("/tb/signal"));
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/signal", eir::Radix::Hexadecimal), "12");
    EXPECT_EQ(native.value("/tb/signal"), reference.value("/tb/signal"));
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

eir::Design objectPartsDesign(std::string_view offsetValue, bool nonblocking) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto objectType = eir::Type::bits(130, false, true);
    const auto offsetType = eir::Type::bits(128, false, true);
    const auto partType = eir::Type::bits(16, false, true);
    top.addObject(
        eir::Object{.name = "source",
                    .type = objectType,
                    .initialValue = test::literal("130'h2x123456789abcdef0123456789abcde0z")});
    top.addObject(eir::Object{.name = "part", .type = partType});
    top.addObject(
        eir::Object{.name = "destination",
                    .type = objectType,
                    .initialValue = test::literal("130'h123456789abcdef0123456789abcdef00")});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "part_access");
    const auto offset = process.reg(offsetType);
    const auto part = process.reg(partType);
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(offset),
                                   Operand::constant(process.constant(offsetValue))));
    process.readObjectPart(part, 0, offset);
    process.store(1, part);
    process.storePartNonBlocking(2, part, offset, 130);
    process.emit(Instruction::make(Opcode::Return));
    auto body = process.finish();
    if (!nonblocking) body.blocks[0].instructions[3].opcode = Opcode::StorePart;
    top.processes.push_back(std::move(body));
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, PartialObjectAccessesMatchInterpreter) {
    for (const auto* offset : {"128'd0", "128'd1", "128'd63", "128'd64", "128'd129", "128'd130",
                               "128'h10000000000000000", "128'hx"}) {
        for (const bool nonblocking : {false, true}) {
            test::Simulation native(objectPartsDesign(offset, nonblocking), "tb", {}, {},
                                    jit::makeBackend);
            test::Simulation reference(objectPartsDesign(offset, nonblocking), "tb");
            ASSERT_TRUE(native.ready()) << offset;
            ASSERT_TRUE(reference.ready()) << offset;
            EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
            EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
            EXPECT_EQ(native.value("/tb/part"), reference.value("/tb/part")) << offset;
            EXPECT_EQ(native.value("/tb/destination"), reference.value("/tb/destination"))
                << offset;
            EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
                      reference.simulationKernel().totalDeltaCycles());
            EXPECT_EQ(native.simulationKernel().executedInstructions(),
                      reference.simulationKernel().executedInstructions());
        }
    }
}

eir::Design signalAttributesDesign(std::uint32_t timeWidth) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.language = eir::Language::Vhdl;
    const auto byte = eir::Type::bits(8, false, false);
    const auto bit = eir::Type::bits(1, false, false);
    const auto wide = eir::Type::bits(timeWidth, false, false);
    top.addObject(eir::Object{.name = "signal", .type = byte});
    top.addObject(eir::Object{.name = "event", .type = bit});
    top.addObject(eir::Object{.name = "active", .type = bit});
    top.addObject(eir::Object{.name = "last", .type = byte});
    top.addObject(eir::Object{.name = "time", .type = wide});

    test::ProcessBuilder driver(eir::ProcessKind::Initial, "driver");
    const auto value = driver.reg(byte);
    const auto delay = driver.reg(eir::Type::bits(64, false, false));
    driver.block("entry");
    driver.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(value),
                                  Operand::constant(driver.constant("8'd1"))));
    driver.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                  Operand::constant(driver.constant("64'd5"))));
    driver.drive(0, value, delay, false);
    driver.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                  Operand::constant(driver.constant("64'd10"))));
    driver.drive(0, value, delay, false);
    driver.emit(Instruction::make(Opcode::Return));
    auto driverBody = driver.finish();
    driverBody.drivesSignals = true;
    top.processes.push_back(std::move(driverBody));

    test::ProcessBuilder reader(eir::ProcessKind::Initial, "reader");
    const auto wait = reader.reg(eir::Type::bits(64, false, false));
    const auto source = reader.reg(byte);
    const auto event = reader.reg(bit);
    const auto active = reader.reg(bit);
    const auto last = reader.reg(byte);
    const auto time = reader.reg(wide);
    reader.block("entry");
    reader.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(wait),
                                  Operand::constant(reader.constant("64'd5"))));
    reader.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(wait), Operand::block(1)));
    reader.block("sample");
    reader.readObject(source, 0);
    reader.emit(Instruction::make(Opcode::ReadEvent, Operand::reg(event), Operand::object(0)));
    reader.emit(Instruction::make(Opcode::ReadActive, Operand::reg(active), Operand::object(0)));
    reader.emit(Instruction::make(Opcode::ReadLastValue, Operand::reg(last), Operand::object(0)));
    reader.emit(Instruction::make(Opcode::ReadTime, Operand::reg(time)));
    reader.store(1, event);
    reader.store(2, active);
    reader.store(3, last);
    reader.store(4, time);
    reader.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(wait), Operand::block(1)));
    top.processes.push_back(reader.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, SignalEventActiveLastValueAndTimeMatchInterpreter) {
    for (const auto width : {1U, 8U, 64U, 65U, 130U}) {
        test::Simulation native(signalAttributesDesign(width), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(signalAttributesDesign(width), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        for (const auto time : {5U, 10U}) {
            EXPECT_EQ(native.run(time), runtime::RunStatus::TimeLimitReached);
            EXPECT_EQ(reference.run(time), runtime::RunStatus::TimeLimitReached);
            for (const auto* name : {"event", "active", "last", "time"}) {
                const std::string path = std::string("/tb/") + name;
                EXPECT_EQ(native.value(path), reference.value(path)) << time << ' ' << name;
            }
            EXPECT_EQ(native.value("/tb/event"), time == 5 ? "1" : "0");
            EXPECT_EQ(native.value("/tb/active"), "1");
            EXPECT_EQ(native.value("/tb/last", eir::Radix::Decimal), "0");
            EXPECT_EQ(native.value("/tb/time", eir::Radix::Decimal),
                      std::to_string(width == 1 ? time & 1U : time));
            EXPECT_EQ(native.simulationKernel().executedInstructions(),
                      reference.simulationKernel().executedInstructions());
        }
    }
}

eir::Design divisionDesign(std::uint32_t width, std::string_view left, std::string_view right,
                           bool isSigned) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto type = eir::Type::bits(width, isSigned, true);
    top.addObject(eir::Object{.name = "left", .type = type, .initialValue = test::literal(left)});
    top.addObject(eir::Object{.name = "right", .type = type, .initialValue = test::literal(right)});
    top.addObject(eir::Object{.name = "quotient", .type = type});
    top.addObject(eir::Object{.name = "remainder", .type = type});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "division");
    const auto a = process.reg(type);
    const auto b = process.reg(type);
    const auto quotient = process.reg(type);
    const auto remainder = process.reg(type);
    process.block("entry");
    process.readObject(a, 0);
    process.readObject(b, 1);
    process.emit(isSigned ? Instruction::makeSigned(Opcode::Divide, Operand::reg(quotient),
                                                    Operand::reg(a), Operand::reg(b))
                          : Instruction::make(Opcode::Divide, Operand::reg(quotient),
                                              Operand::reg(a), Operand::reg(b)));
    process.emit(isSigned ? Instruction::makeSigned(Opcode::Modulo, Operand::reg(remainder),
                                                    Operand::reg(a), Operand::reg(b))
                          : Instruction::make(Opcode::Modulo, Operand::reg(remainder),
                                              Operand::reg(a), Operand::reg(b)));
    process.store(2, quotient);
    process.store(3, remainder);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, DivisionAndRemainderMatchInterpreter) {
    for (const auto& [width, left, right] :
         {std::tuple{1U, "1'b1", "1'b1"}, std::tuple{4U, "4'b1010", "4'b0011"},
          std::tuple{4U, "4'b1000", "4'b1111"}, std::tuple{4U, "4'b0101", "4'b0000"},
          std::tuple{4U, "4'b0x01", "4'b0011"}, std::tuple{4U, "4'b0101", "4'b0z11"},
          std::tuple{64U, "64'h8000000000000000", "64'hffffffffffffffff"},
          std::tuple{64U, "64'hffffffffffffffec", "64'h0000000000000003"},
          std::tuple{70U, "70'h200000000000000000", "70'h3fffffffffffffffff"},
          std::tuple{130U, "130'h200000000000000000000000000000001",
                     "130'h000000000000000000000000000000003"},
          std::tuple{130U, "130'h00000000000000000000000000000000x",
                     "130'h000000000000000000000000000000001"}}) {
        for (const bool isSigned : {false, true}) {
            test::Simulation native(divisionDesign(width, left, right, isSigned), "tb", {}, {},
                                    jit::makeBackend);
            test::Simulation reference(divisionDesign(width, left, right, isSigned), "tb");
            ASSERT_TRUE(native.ready()) << left << ' ' << right;
            ASSERT_TRUE(reference.ready()) << left << ' ' << right;
            EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
            EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
            for (const auto* name : {"quotient", "remainder"}) {
                const std::string path = std::string("/tb/") + name;
                EXPECT_EQ(native.value(path), reference.value(path))
                    << left << ' ' << right << ' ' << isSigned << ' ' << name;
            }
            EXPECT_EQ(native.simulationKernel().executedInstructions(),
                      reference.simulationKernel().executedInstructions());
        }
    }
}

eir::Design realDivisionDesign(std::string_view left, std::string_view right) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(eir::Object{.name = "quotient", .type = eir::Type::real()});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "real_division");
    const auto a = process.reg(eir::Type::real());
    const auto b = process.reg(eir::Type::real());
    const auto quotient = process.reg(eir::Type::real());
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(a),
                                   Operand::constant(process.constant(left))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(b),
                                   Operand::constant(process.constant(right))));
    process.emit(Instruction::make(Opcode::Divide, Operand::reg(quotient), Operand::reg(a),
                                   Operand::reg(b)));
    process.store(0, quotient);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, RealDivisionMatchesInterpreter) {
    for (const auto& [left, right] : {std::pair{"64'h4008000000000000", "64'h4000000000000000"},
                                      std::pair{"64'hbff0000000000000", "64'h0000000000000000"},
                                      std::pair{"64'h0000000000000000", "64'h0000000000000000"}}) {
        test::Simulation native(realDivisionDesign(left, right), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(realDivisionDesign(left, right), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(native.value("/tb/quotient"), reference.value("/tb/quotient"))
            << left << ' ' << right;
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design conditionalDesign(std::uint32_t width, std::string_view condition,
                              std::string_view whenTrue, std::string_view whenFalse) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto outputType = eir::Type::bits(width, false, true);
    const auto conditionType = eir::Type::bits(70, false, true);
    top.addObject(eir::Object{.name = "output", .type = outputType});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "conditional");
    const auto selector = process.reg(conditionType);
    const auto left = process.reg(outputType);
    const auto right = process.reg(outputType);
    const auto result = process.reg(outputType);
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(selector),
                                   Operand::constant(process.constant(condition))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(left),
                                   Operand::constant(process.constant(whenTrue))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(right),
                                   Operand::constant(process.constant(whenFalse))));
    process.emit(Instruction::make(Opcode::Conditional, Operand::reg(result),
                                   Operand::reg(selector), Operand::reg(left),
                                   Operand::reg(right)));
    process.store(0, result);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design resultDesign;
    resultDesign.addModule(std::move(top));
    return resultDesign;
}

TEST(JitBackend, ConditionalMergesFourStateArmsLikeInterpreter) {
    for (const auto& [width, left, right] :
         {std::tuple{4U, "4'b10xz", "4'b1zxx"}, std::tuple{4U, "4'b0000", "4'b1111"},
          std::tuple{4U, "4'bzzzz", "4'bzzzz"}, std::tuple{4U, "4'bxxxx", "4'bxxxx"},
          std::tuple{64U, "64'hx123456789abcdez", "64'hx123456789abcde1"},
          std::tuple{130U, "130'h2x123456789abcdef0123456789abcde0z",
                     "130'h2z123456789abcdef0123456789abcde0x"}}) {
        for (const auto* condition :
             {"70'd0", "70'd1", "70'h1000000000000000x", "70'hx", "70'hz"}) {
            test::Simulation native(conditionalDesign(width, condition, left, right), "tb", {}, {},
                                    jit::makeBackend);
            test::Simulation reference(conditionalDesign(width, condition, left, right), "tb");
            ASSERT_TRUE(native.ready()) << condition;
            ASSERT_TRUE(reference.ready()) << condition;
            EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
            EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
            EXPECT_EQ(native.value("/tb/output"), reference.value("/tb/output"))
                << condition << ' ' << width;
            EXPECT_EQ(native.simulationKernel().executedInstructions(),
                      reference.simulationKernel().executedInstructions());
        }
    }
}

eir::Design systemFunctionDesign(bool fail) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto word = eir::Type::bits(32, false, true);
    const auto handle = eir::Type::bits(64, false, false);
    const auto index = eir::Type::bits(64, true, false);
    const auto byte = eir::Type::bits(8, false, false);
    const auto bit = eir::Type::bits(1, false, false);
    top.addObject(eir::Object{.name = "random", .type = word});
    test::SubprogramBuilder function("generate");
    const auto result = function.result(word);
    const auto left = function.reg(index);
    const auto right = function.reg(index);
    const auto descending = function.reg(bit);
    const auto fill = function.reg(byte);
    const auto bad = function.reg(index);
    const auto string = function.reg(handle);
    const auto element = function.reg(byte);
    eir::SysTaskCall random;
    random.kind = eir::SysTaskKind::URandom;
    const auto randomCall = function.sysCall(std::move(random));
    const auto arg = [](std::uint32_t reg, eir::Type type) {
        return eir::SysTaskArgument{.value = Operand::reg(reg), .type = type};
    };
    eir::SysTaskCall making;
    making.kind = eir::SysTaskKind::StringNew;
    making.arguments = {arg(left, index), arg(right, index), arg(descending, bit), arg(fill, byte)};
    const auto make = function.sysCall(std::move(making));
    eir::SysTaskCall slicing;
    slicing.kind = eir::SysTaskKind::StringSlice;
    slicing.arguments = {arg(string, handle), arg(bad, index)};
    const auto slice = function.sysCall(std::move(slicing));
    function.block("entry");
    function.emit(
        Instruction::make(Opcode::SysFunction, Operand::reg(result), Operand::sysCall(randomCall)));
    if (fail) {
        function.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(left),
                                        Operand::constant(function.constant("64'sd3"))));
        function.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(right),
                                        Operand::constant(function.constant("64'sd1"))));
        function.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(descending),
                                        Operand::constant(function.constant("1'b0"))));
        function.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(fill),
                                        Operand::constant(function.constant("8'h20"))));
        function.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(bad),
                                        Operand::constant(function.constant("64'sd4"))));
        function.emit(
            Instruction::make(Opcode::SysFunction, Operand::reg(string), Operand::sysCall(make)));
        function.emit(
            Instruction::make(Opcode::SysFunction, Operand::reg(element), Operand::sysCall(slice)));
    }
    function.emit(Instruction::make(Opcode::Return));
    top.subprograms.push_back(function.finish());

    test::ProcessBuilder process(eir::ProcessKind::Initial, "caller");
    const auto resultRegister = process.reg(word);
    process.block("entry");
    process.call("", "generate", {}, Operand::reg(resultRegister));
    process.store(0, resultRegister);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design resultDesign;
    resultDesign.addModule(std::move(top));
    return resultDesign;
}

TEST(JitBackend, SystemFunctionsAndErrorsInsideCallsMatchInterpreter) {
    for (const bool fail : {false, true}) {
        test::Simulation native(systemFunctionDesign(fail), "tb", {}, {}, jit::makeBackend);
        test::Simulation reference(systemFunctionDesign(fail), "tb");
        ASSERT_TRUE(native.ready());
        ASSERT_TRUE(reference.ready());
        EXPECT_EQ(native.run(), fail ? runtime::RunStatus::Error : runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(),
                  fail ? runtime::RunStatus::Error : runtime::RunStatus::Quiescent);
        EXPECT_EQ(native.value("/tb/random"), reference.value("/tb/random"));
        EXPECT_EQ(native.messages(), reference.messages());
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design arithmeticDesign(std::uint32_t width, std::string_view left, std::string_view right,
                             bool real) {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto type = real ? eir::Type::real() : eir::Type::bits(width, false, true);
    for (const auto* name : {"sum", "difference", "product", "negated"})
        top.addObject(eir::Object{.name = name, .type = type});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "arithmetic");
    const auto a = process.reg(type);
    const auto b = process.reg(type);
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(a),
                                   Operand::constant(process.constant(left))));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(b),
                                   Operand::constant(process.constant(right))));
    std::uint32_t object = 0;
    for (const auto opcode : {Opcode::Add, Opcode::Subtract, Opcode::Multiply}) {
        const auto result = process.reg(type);
        process.emit(
            Instruction::make(opcode, Operand::reg(result), Operand::reg(a), Operand::reg(b)));
        process.store(object++, result);
    }
    const auto negated = process.reg(type);
    process.emit(Instruction::make(Opcode::Negate, Operand::reg(negated), Operand::reg(a)));
    process.store(object, negated);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, IntegerAndRealArithmeticMatchInterpreter) {
    for (const auto& [width, left, right, real] :
         {std::tuple{4U, "4'b1000", "4'b0011", false}, std::tuple{4U, "4'b1x00", "4'b0011", false},
          std::tuple{64U, "64'hffffffffffffffff", "64'h0000000000000001", false},
          std::tuple{70U, "70'h3fffffffffffffffff", "70'h000000000000000001", false},
          std::tuple{70U, "70'h100000000000000003", "70'h200000000000000005", false},
          std::tuple{130U, "130'h200000000000000000000000000000001",
                     "130'h000000000000000000000000000000003", false},
          std::tuple{130U, "130'h20000000000000000000000000000000z",
                     "130'h000000000000000000000000000000001", false},
          std::tuple{64U, "64'h3ff8000000000000", "64'h4000000000000000", true},
          std::tuple{64U, "64'h7ff0000000000000", "64'hbff0000000000000", true},
          std::tuple{64U, "64'h0000000000000000", "64'h8000000000000000", true}}) {
        test::Simulation native(arithmeticDesign(width, left, right, real), "tb", {}, {},
                                jit::makeBackend);
        test::Simulation reference(arithmeticDesign(width, left, right, real), "tb");
        ASSERT_TRUE(native.ready()) << left << ' ' << right;
        ASSERT_TRUE(reference.ready()) << left << ' ' << right;
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        for (const auto* name : {"sum", "difference", "product", "negated"}) {
            const std::string path = std::string("/tb/") + name;
            EXPECT_EQ(native.value(path), reference.value(path))
                << left << ' ' << right << ' ' << name;
        }
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design longStraightLineDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(eir::Object{.name = "output", .type = eir::Type::bits(8, false, false)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "long_process");
    const auto delay = process.reg(eir::Type::bits(64, false, false));
    const auto value = process.reg(eir::Type::bits(8, false, false));
    process.block("entry");
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                   Operand::constant(process.constant("64'd1"))));
    for (int index = 0; index < 130; ++index) process.emit(Instruction::make(Opcode::Nop));
    process.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    process.block("resume");
    for (int index = 0; index < 130; ++index) process.emit(Instruction::make(Opcode::Nop));
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(value),
                                   Operand::constant(process.constant("8'd42"))));
    process.store(0, value);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, LongStraightLineBlocksResumeAndCountLikeInterpreter) {
    test::Simulation native(longStraightLineDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(longStraightLineDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(0), runtime::RunStatus::TimeLimitReached);
    EXPECT_EQ(reference.run(0), runtime::RunStatus::TimeLimitReached);
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/output", eir::Radix::Decimal), "42");
    EXPECT_EQ(native.value("/tb/output"), reference.value("/tb/output"));
    EXPECT_GT(native.simulationKernel().executedInstructions(), 260U);
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

eir::Design multipleLargeBodiesDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto word = eir::Type::bits(8, false, false);
    top.addObject(eir::Object{.name = "first", .type = word});
    top.addObject(eir::Object{.name = "second", .type = word});
    for (const auto& [name, literal] :
         {std::pair{"first_value", "8'd17"}, std::pair{"second_value", "8'd29"}}) {
        test::SubprogramBuilder function(name);
        const auto value = function.result(word);
        function.block("entry");
        for (int index = 0; index < 5000; ++index) function.emit(Instruction::make(Opcode::Nop));
        function.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(value),
                                        Operand::constant(function.constant(literal))));
        function.emit(Instruction::make(Opcode::Return));
        top.subprograms.push_back(function.finish());
    }
    test::ProcessBuilder process(eir::ProcessKind::Initial, "caller");
    const auto first = process.reg(word);
    const auto second = process.reg(word);
    process.block("entry");
    process.call("", "first_value", {}, Operand::reg(first));
    process.call("", "second_value", {}, Operand::reg(second));
    process.store(0, first);
    process.store(1, second);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, CallsAcrossLargeBodiesKeepResultsAndCounts) {
    test::Simulation native(multipleLargeBodiesDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(multipleLargeBodiesDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/first", eir::Radix::Decimal), "17");
    EXPECT_EQ(native.value("/tb/second", eir::Radix::Decimal), "29");
    EXPECT_EQ(native.value("/tb/first"), reference.value("/tb/first"));
    EXPECT_EQ(native.value("/tb/second"), reference.value("/tb/second"));
    EXPECT_GT(native.simulationKernel().executedInstructions(), 10000U);
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

TEST(JitBackend, DesignWithoutBodiesCompletes) {
    eir::ModuleDefinition top;
    top.name = "tb";
    eir::Design design;
    design.addModule(std::move(top));
    test::Simulation native(std::move(design), "tb", {}, {}, jit::makeBackend);
    ASSERT_TRUE(native.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.simulationKernel().executedInstructions(), 0U);
}

TEST(JitBackend, WideShiftsAndCountsMatchInterpreter) {
    for (const auto& [width, source, amountWidth, amount] :
         {std::tuple{70U, "70'h20000000000000000z", 8U, "8'd0"},
          std::tuple{70U, "70'h20000000000000000z", 8U, "8'd63"},
          std::tuple{70U, "70'h20000000000000000z", 8U, "8'd64"},
          std::tuple{70U, "70'hx00000000000000001", 8U, "8'd70"},
          std::tuple{70U, "70'hz00000000000000001", 8U, "8'd69"},
          std::tuple{130U, "130'h20000000000000000000000000000000x", 8U, "8'd129"},
          std::tuple{130U, "130'h20000000000000000000000000000000x", 8U, "8'd130"},
          std::tuple{64U, "64'h8000000000000001", 70U, "70'd63"},
          std::tuple{64U, "64'h8000000000000001", 70U, "70'h10000000000000000"},
          std::tuple{70U, "70'h20000000000000000z", 70U, "70'hx"}}) {
        test::Simulation native(shiftDesign(width, source, amount, amountWidth), "tb", {}, {},
                                jit::makeBackend);
        test::Simulation reference(shiftDesign(width, source, amount, amountWidth), "tb");
        ASSERT_TRUE(native.ready()) << source << ' ' << amount;
        ASSERT_TRUE(reference.ready()) << source << ' ' << amount;
        EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
        EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
        for (const auto* name : {"left", "logical", "arithmetic"}) {
            const std::string path = std::string("/tb/") + name;
            EXPECT_EQ(native.value(path), reference.value(path))
                << source << ' ' << amount << ' ' << name;
        }
        EXPECT_EQ(native.simulationKernel().executedInstructions(),
                  reference.simulationKernel().executedInstructions());
    }
}

eir::Design distinctPortReadDesign() {
    const auto word = eir::Type::bits(70, false, true);
    eir::ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.sourceName = "leaf";
    leaf.addObject(eir::Object{.name = "io", .type = word});
    leaf.ports.push_back(
        eir::Port{.name = "io", .direction = eir::PortDirection::InOut, .objectIndex = 0});
    test::ProcessBuilder copy(eir::ProcessKind::Initial, "copy");
    const auto value = copy.reg(word);
    copy.block("entry");
    copy.readObject(value, 0);
    copy.store(0, value);
    copy.emit(Instruction::make(Opcode::Return));
    leaf.processes.push_back(copy.finish());

    eir::ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(eir::Object{.name = "effective1",
                              .type = word,
                              .initialValue = test::literal("70'h10000000000000000x")});
    top.addObject(
        eir::Object{.name = "driving1", .type = word, .initialValue = test::literal("70'd0")});
    top.addObject(eir::Object{.name = "effective2",
                              .type = word,
                              .initialValue = test::literal("70'h20000000000000000z")});
    top.addObject(
        eir::Object{.name = "driving2", .type = word, .initialValue = test::literal("70'd0")});
    for (std::uint32_t index = 0; index < 2; ++index) {
        eir::Instance child;
        child.name = index == 0 ? "first" : "second";
        child.definitionName = "leaf";
        child.connections.push_back(eir::PortConnection{
            .portIndex = 0, .objectIndex = 2 * index, .driveObjectIndex = 2 * index + 1});
        top.instances.push_back(std::move(child));
    }
    eir::Design result;
    result.addModule(std::move(leaf));
    result.addModule(std::move(top));
    return result;
}

TEST(JitBackend, SharedBodiesReadTheBoundObjectsRatherThanTheirWriteRoutes) {
    test::Simulation native(distinctPortReadDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(distinctPortReadDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    for (const auto* name : {"effective1", "driving1", "effective2", "driving2"}) {
        const std::string path = std::string("/tb/") + name;
        EXPECT_EQ(native.value(path), reference.value(path)) << name;
    }
    EXPECT_EQ(native.value("/tb/driving1"), native.value("/tb/effective1"));
    EXPECT_EQ(native.value("/tb/driving2"), native.value("/tb/effective2"));
    EXPECT_NE(native.value("/tb/driving1"), native.value("/tb/driving2"));
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

eir::Design packageReadDesign() {
    eir::ModuleDefinition package;
    package.name = "counter_pkg::";
    package.sourceName = "counter_pkg";
    package.isPackage = true;
    const auto word = eir::Type::bits(8, false, true);
    package.addObject(
        eir::Object{.name = "count", .type = word, .initialValue = test::literal("8'd0")});
    test::SubprogramBuilder bump("bump");
    const auto current = bump.reg(word);
    const auto one = bump.reg(word);
    const auto result = bump.result(word);
    bump.block("entry");
    bump.readObject(current, 0);
    bump.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(one),
                                Operand::constant(bump.constant("8'd1"))));
    bump.emit(Instruction::make(Opcode::Add, Operand::reg(result), Operand::reg(current),
                                Operand::reg(one)));
    bump.store(0, result);
    bump.emit(Instruction::make(Opcode::Return));
    package.subprograms.push_back(bump.finish());

    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(eir::Object{.name = "result", .type = word});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "caller");
    const auto output = process.reg(word);
    process.block("entry");
    process.call("counter_pkg::", "bump", {}, Operand::reg(output));
    process.call("counter_pkg::", "bump", {}, Operand::reg(output));
    process.store(0, output);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design resultDesign;
    resultDesign.addModule(std::move(package));
    resultDesign.addModule(std::move(top));
    return resultDesign;
}

TEST(JitBackend, SubprogramsReadTheirOwnInstancesAcrossCalls) {
    test::Simulation native(packageReadDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(packageReadDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/result", eir::Radix::Decimal), "2");
    EXPECT_EQ(native.value("/counter_pkg/count"), reference.value("/counter_pkg/count"));
    EXPECT_EQ(native.value("/tb/result"), reference.value("/tb/result"));
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
}

eir::Design callbackReadDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    const auto word = eir::Type::bits(8, false, false);
    for (const auto* name : {"trigger", "input", "output"})
        top.addObject(
            eir::Object{.name = name, .type = word, .initialValue = test::literal("8'd0")});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "callback_read");
    const auto value = process.reg(word);
    const auto trigger = process.reg(word);
    process.block("entry");
    process.readObject(value, 1);
    process.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(trigger),
                                   Operand::constant(process.constant("8'd1"))));
    process.store(0, trigger);
    process.readObject(value, 1);
    process.store(2, value);
    process.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design result;
    result.addModule(std::move(top));
    return result;
}

class CallbackWriter final : public runtime::ValueChangeListener {
public:
    CallbackWriter(runtime::Kernel& kernel, std::uint32_t triggerSignal, std::uint32_t inputSignal)
        : owner(kernel),
          trigger(triggerSignal),
          input(inputSignal),
          value(test::literal("8'ha5")) {}

    void onValueChange(std::uint32_t signal, eir::ConstBitRef /*value*/) override {
        if (signal == trigger) owner.writeSignal(input, value.ref());
    }

    void onTimeAdvance(runtime::SimTime /*time*/) override {}

private:
    runtime::Kernel& owner;
    std::uint32_t trigger;
    std::uint32_t input;
    eir::BitVector value;
};

TEST(JitBackend, ObjectReadAfterCallbackSeesTheNewValue) {
    test::Simulation native(callbackReadDesign(), "tb", {}, {}, jit::makeBackend);
    test::Simulation reference(callbackReadDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(reference.ready());
    CallbackWriter nativeCallback(native.simulationKernel(), native.signal("/tb/trigger"),
                                  native.signal("/tb/input"));
    CallbackWriter referenceCallback(reference.simulationKernel(), reference.signal("/tb/trigger"),
                                     reference.signal("/tb/input"));
    native.simulationKernel().addListener(nativeCallback);
    reference.simulationKernel().addListener(referenceCallback);
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(reference.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/output", eir::Radix::Hexadecimal), "a5");
    EXPECT_EQ(native.value("/tb/output"), reference.value("/tb/output"));
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              reference.simulationKernel().executedInstructions());
    native.simulationKernel().removeListener(nativeCallback);
    reference.simulationKernel().removeListener(referenceCallback);
}

} // namespace
