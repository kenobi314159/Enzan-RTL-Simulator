// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Instruction.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using enzan::eir::EdgeKind;
using enzan::eir::Opcode;
using enzan::eir::Operand;

/// Every opcode, generated from the catalogue so that a newly added one is covered
/// by the checks below without anyone remembering to list it.
constexpr Opcode AllOpcodes[] = {
#define ENZAN_OPCODE(name, mnemonic, hasResult, sources, lowering) Opcode::name,
#include "eir/Opcodes.def"
#undef ENZAN_OPCODE
};

// ---------------------------------------------------------------------------
// The opcode catalogue
// ---------------------------------------------------------------------------

TEST(OpcodeCatalogue, EveryOpcodeRecordsAnLlvmLowering) {
    // A standing invariant: the JIT backend is meant to be a mechanical translation
    // of the interpreter, and an opcode that cannot be lowered mechanically blocks
    // it. A requirement nothing checks is a requirement that erodes, so it is
    // checked here.
    for (const Opcode opcode : AllOpcodes) {
        EXPECT_FALSE(enzan::eir::llvmLowering(opcode).empty())
            << "opcode '" << enzan::eir::mnemonic(opcode) << "' has no recorded LLVM lowering";
        EXPECT_GT(enzan::eir::llvmLowering(opcode).size(), 10U)
            << "opcode '" << enzan::eir::mnemonic(opcode)
            << "' has a lowering sketch too short to be a sketch";
    }
}

TEST(OpcodeCatalogue, MnemonicsAreNonEmptyAndUnique) {
    std::set<std::string> seen;
    for (const Opcode opcode : AllOpcodes) {
        const std::string text(enzan::eir::mnemonic(opcode));
        EXPECT_FALSE(text.empty());
        EXPECT_TRUE(seen.insert(text).second) << "mnemonic '" << text << "' is used twice";
    }
}

TEST(OpcodeCatalogue, SourceCountsAreWithinTheThreeAddressForm) {
    for (const Opcode opcode : AllOpcodes) {
        EXPECT_LE(enzan::eir::sourceCount(opcode), 3U)
            << enzan::eir::mnemonic(opcode) << " exceeds three-address form";
    }
}

TEST(OpcodeCatalogue, TerminatorsAndSuspensions) {
    EXPECT_TRUE(enzan::eir::isTerminator(Opcode::Jump));
    EXPECT_TRUE(enzan::eir::isTerminator(Opcode::BranchIf));
    EXPECT_TRUE(enzan::eir::isTerminator(Opcode::Return));
    EXPECT_FALSE(enzan::eir::isTerminator(Opcode::Add));

    // A suspension ends its block as surely as a branch does: control leaves for the
    // scheduler and comes back at the named resume block.
    EXPECT_TRUE(enzan::eir::suspends(Opcode::WaitTime));
    EXPECT_TRUE(enzan::eir::suspends(Opcode::WaitEdge));
    EXPECT_TRUE(enzan::eir::isTerminator(Opcode::WaitTime));
    EXPECT_TRUE(enzan::eir::isTerminator(Opcode::WaitEdge));
    EXPECT_FALSE(enzan::eir::suspends(Opcode::Jump));
}

TEST(OpcodeCatalogue, ResultFlagsMatchTheOperations) {
    EXPECT_TRUE(enzan::eir::hasResult(Opcode::Add));
    EXPECT_TRUE(enzan::eir::hasResult(Opcode::ReadObject));
    EXPECT_TRUE(enzan::eir::hasResult(Opcode::ReadTime));

    // A store's effect is on the arena, not on a register.
    EXPECT_FALSE(enzan::eir::hasResult(Opcode::Store));
    EXPECT_FALSE(enzan::eir::hasResult(Opcode::StoreNonBlocking));
    EXPECT_FALSE(enzan::eir::hasResult(Opcode::Jump));
    EXPECT_FALSE(enzan::eir::hasResult(Opcode::SysTask));
}

// ---------------------------------------------------------------------------
// Operands
// ---------------------------------------------------------------------------

TEST(Operand, DefaultIsAbsent) {
    EXPECT_FALSE(Operand().valid());
    EXPECT_EQ(Operand().kind(), Operand::Kind::None);
    EXPECT_EQ(Operand::none(), Operand());
}

TEST(Operand, EachKindCarriesItsIndex) {
    EXPECT_EQ(Operand::reg(7).kind(), Operand::Kind::Register);
    EXPECT_EQ(Operand::reg(7).index(), 7U);
    EXPECT_EQ(Operand::constant(3).kind(), Operand::Kind::Constant);
    EXPECT_EQ(Operand::object(11).kind(), Operand::Kind::Object);
    EXPECT_EQ(Operand::block(2).kind(), Operand::Kind::Block);
    EXPECT_EQ(Operand::sysCall(1).kind(), Operand::Kind::SysCall);
    EXPECT_EQ(Operand::immediate(42).index(), 42U);

    for (const Operand operand :
         {Operand::reg(0), Operand::constant(0), Operand::object(0), Operand::block(0)}) {
        EXPECT_TRUE(operand.valid());
    }
}

TEST(Operand, KindsWithTheSameIndexAreDistinct) {
    // Register 0 and object 0 must never compare equal, or a mis-typed operand would
    // silently read the wrong storage.
    EXPECT_NE(Operand::reg(0), Operand::object(0));
    EXPECT_NE(Operand::block(1), Operand::constant(1));
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

TEST(Instruction, DefaultsToAnUnsignedNop) {
    const enzan::eir::Instruction instruction;
    EXPECT_EQ(instruction.opcode, Opcode::Nop);
    EXPECT_FALSE(instruction.isSigned);
    EXPECT_FALSE(instruction.dst.valid());
}

TEST(Instruction, FactoriesFillTheOperands) {
    const auto add = enzan::eir::Instruction::make(Opcode::Add, Operand::reg(0), Operand::reg(1),
                                                   Operand::reg(2));
    EXPECT_EQ(add.opcode, Opcode::Add);
    EXPECT_EQ(add.dst, Operand::reg(0));
    EXPECT_EQ(add.a, Operand::reg(1));
    EXPECT_EQ(add.b, Operand::reg(2));
    EXPECT_FALSE(add.c.valid());
    EXPECT_FALSE(add.isSigned);

    const auto less = enzan::eir::Instruction::makeSigned(Opcode::LessThan, Operand::reg(0),
                                                          Operand::reg(1), Operand::reg(2));
    EXPECT_TRUE(less.isSigned);
}

TEST(EdgeKinds, HaveNames) {
    EXPECT_EQ(enzan::eir::toString(EdgeKind::AnyChange), "change");
    EXPECT_EQ(enzan::eir::toString(EdgeKind::Posedge), "posedge");
    EXPECT_EQ(enzan::eir::toString(EdgeKind::Negedge), "negedge");
}

} // namespace
