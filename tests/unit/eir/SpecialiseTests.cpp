// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Specialise.h"

#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/ParameterExpression.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include <gtest/gtest.h>

#include <optional>
#include <variant>
#include <vector>

namespace {

using enzan::eir::BasicBlock;
using enzan::eir::BitVector;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ParameterFormal;
using enzan::eir::ParameterValue;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::eir::WriteExtent;

/// A unit generic over `W` that copies `in`, `W` bits wide, into `out`, `W + 1` bits wide,
/// extending it on the way: an extension a specialisation keeps, followed by one whose two
/// sides, `W + 1` and `max(W + 1, W)`, are always equal but are two expressions.
ModuleDefinition widening() {
    ModuleDefinition unit;
    unit.name = "widening";
    enzan::eir::ParameterExpressions& table = unit.expressions;
    const std::uint32_t w = table.formal(0, 32, true);
    const std::uint32_t one = table.constant(BitVector::fromInt64(1, 32), true);
    const std::uint32_t wider =
        table.add(enzan::eir::ParameterNode{.kind = enzan::eir::ParameterNodeKind::Operation,
                                            .opcode = Opcode::Add,
                                            .isSigned = true,
                                            .operationSigned = true,
                                            .width = 32,
                                            .a = w,
                                            .b = one});
    // An expression standing for the same width another way, as a context's maximum does.
    const std::uint32_t same =
        table.add(enzan::eir::ParameterNode{.kind = enzan::eir::ParameterNodeKind::Operation,
                                            .opcode = Opcode::Or,
                                            .isSigned = true,
                                            .width = 32,
                                            .a = wider,
                                            .b = wider});
    unit.formals.push_back(
        ParameterFormal{.name = "W", .value = table.constant(BitVector::fromInt64(4, 32), true)});

    unit.addObject(Object{.name = "in", .type = Type::bits(4, false, true).withWidthExpression(w)});
    unit.addObject(
        Object{.name = "out", .type = Type::bits(5, false, true).withWidthExpression(wider)});

    Process process;
    process.kind = ProcessKind::ContinuousAssign;
    process.name = "assign";
    process.registerTypes = {Type::bits(4, false, true).withWidthExpression(w),
                             Type::bits(5, false, true).withWidthExpression(same),
                             Type::bits(5, false, true).withWidthExpression(wider)};
    process.readSet = {0};
    process.writeSet = {WriteExtent{.objectIndex = 1, .width = 5, .widthExpression = wider}};
    BasicBlock entry;
    entry.label = "entry";
    entry.instructions = {
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(0)),
        Instruction::make(Opcode::Extend, Operand::reg(1), Operand::reg(0)),
        Instruction::make(Opcode::Extend, Operand::reg(2), Operand::reg(1)),
        Instruction::make(Opcode::Store, {}, Operand::object(1), Operand::reg(2)),
        Instruction::make(Opcode::Return)};
    process.blocks.push_back(std::move(entry));
    unit.processes.push_back(std::move(process));
    return unit;
}

std::vector<Opcode> opcodes(const Process& process) {
    std::vector<Opcode> result;
    for (const BasicBlock& block : process.blocks)
        for (const Instruction& instruction : block.instructions)
            result.push_back(instruction.opcode);
    return result;
}

TEST(SpecialiseAt, DropsAConversionBetweenWidthsThatCameOutEqual) {
    // What a frontend translating W = 6 directly emits: one extension, from 6 bits to 7, and no
    // copy of the result to a register of the same width.
    const std::vector<std::optional<ParameterValue>> values{
        ParameterValue{.bits = BitVector::fromInt64(6, 32), .isSigned = true}};
    auto result = enzan::eir::specialiseAt(widening(), values);
    ASSERT_TRUE(std::holds_alternative<ModuleDefinition>(result));
    const Process& process = std::get<ModuleDefinition>(result).processes[0];

    EXPECT_EQ(opcodes(process), (std::vector<Opcode>{Opcode::ReadObject, Opcode::Extend,
                                                     Opcode::Store, Opcode::Return}));
    EXPECT_EQ(process.registerTypes,
              (std::vector<Type>{Type::bits(6, false, true), Type::bits(7, false, true)}));
    const Instruction& store = process.blocks[0].instructions[2];
    EXPECT_EQ(store.b.index(), 1U) << "the store reads what the remaining extension wrote";
}

TEST(SpecialiseAt, KeepsAConversionWhoseResultIsWrittenTwice) {
    // Using the source for every use of the result is right only when nothing else writes it.
    ModuleDefinition unit = widening();
    Process& process = unit.processes[0];
    process.blocks[0].instructions.insert(
        process.blocks[0].instructions.begin() + 2,
        Instruction::make(Opcode::Move, Operand::reg(1), Operand::reg(1)));
    const std::vector<std::optional<ParameterValue>> values{
        ParameterValue{.bits = BitVector::fromInt64(6, 32), .isSigned = true}};
    auto result = enzan::eir::specialiseAt(unit, values);
    ASSERT_TRUE(std::holds_alternative<ModuleDefinition>(result));
    EXPECT_EQ(std::get<ModuleDefinition>(result).processes[0].registerTypes.size(), 3U);
}

} // namespace
