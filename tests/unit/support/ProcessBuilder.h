// SPDX-License-Identifier: GPL-3.0-or-later
//
// A builder for hand-written EIR processes.
//
// Tests that drive the kernel, the server or anything else downstream of codegen need real
// compiled bodies, and writing them out by hand means every such test also maintains two
// sorted sets for the verifier — which is how a test ends up testing its own bookkeeping
// instead of the code under test. This is that bookkeeping, written once.

#ifndef ENZAN_TESTS_UNIT_SUPPORT_PROCESSBUILDER_H
#define ENZAN_TESTS_UNIT_SUPPORT_PROCESSBUILDER_H

#include "common/SourceLocation.h"
#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include "support/Literals.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::test {

/// Builds one EIR process, tracking the read and write sets the verifier requires.
///
/// Writing these by hand without a builder means every test also maintains two sorted
/// sets, which is exactly the bookkeeping the verifier exists to catch — so the tests
/// would end up testing the bookkeeping rather than the kernel.
class ProcessBuilder {
public:
    ProcessBuilder(eir::ProcessKind kind, std::string name) {
        process.kind = kind;
        process.name = std::move(name);
    }

    std::uint32_t reg(eir::Type type) {
        process.registerTypes.push_back(type);
        return static_cast<std::uint32_t>(process.registerTypes.size() - 1);
    }

    std::uint32_t constant(std::string_view text) {
        process.constants.push_back(enzan::test::literal(text));
        return static_cast<std::uint32_t>(process.constants.size() - 1);
    }

    std::uint32_t string(std::string text) {
        process.strings.push_back(std::move(text));
        return static_cast<std::uint32_t>(process.strings.size() - 1);
    }

    std::uint32_t sysCall(eir::SysTaskCall call) {
        process.sysCalls.push_back(std::move(call));
        return static_cast<std::uint32_t>(process.sysCalls.size() - 1);
    }

    /// Starts a new basic block and returns its index.
    std::uint32_t block(std::string label) {
        eir::BasicBlock created;
        created.label = std::move(label);
        process.blocks.push_back(std::move(created));
        return static_cast<std::uint32_t>(process.blocks.size() - 1);
    }

    /// Sets the source location stamped onto everything emitted after it.
    ///
    /// The frontend stamps every instruction with the statement it came from, so a test that
    /// is about source lines — a breakpoint, a step — has to be able to say the same thing.
    void at(SourceLocation location) { current = location; }

    void emit(const eir::Instruction& instruction) {
        eir::Instruction stamped = instruction;
        stamped.location = current;
        process.blocks.back().instructions.push_back(stamped);
    }

    void readObject(std::uint32_t destination, std::uint32_t objectIndex) {
        reads.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::ReadObject, eir::Operand::reg(destination),
                                    eir::Operand::object(objectIndex)));
    }

    void store(std::uint32_t objectIndex, std::uint32_t source) {
        writes.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::Store, {}, eir::Operand::object(objectIndex),
                                    eir::Operand::reg(source)));
    }

    void storeNonBlocking(std::uint32_t objectIndex, std::uint32_t source) {
        writes.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::StoreNonBlocking, {},
                                    eir::Operand::object(objectIndex), eir::Operand::reg(source)));
    }

    void storePartNonBlocking(std::uint32_t objectIndex, std::uint32_t source,
                              std::uint32_t indexRegister) {
        writes.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::StorePartNonBlocking, {},
                                    eir::Operand::object(objectIndex), eir::Operand::reg(source),
                                    eir::Operand::reg(indexRegister)));
    }

    void sensitive(std::uint32_t objectIndex, eir::EdgeKind edge) {
        reads.push_back(objectIndex);
        process.sensitivity.push_back(
            eir::SensitivityEntry{.objectIndex = objectIndex, .edge = edge});
    }

    void waitEdge(std::uint32_t objectIndex, eir::EdgeKind edge, std::uint32_t resumeBlock) {
        reads.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::WaitEdge, {}, eir::Operand::object(objectIndex),
                                    eir::Operand::immediate(static_cast<std::uint32_t>(edge)),
                                    eir::Operand::block(resumeBlock)));
    }

    eir::Process finish() {
        std::ranges::sort(reads);
        reads.erase(std::ranges::unique(reads).begin(), reads.end());
        std::ranges::sort(writes);
        writes.erase(std::ranges::unique(writes).begin(), writes.end());
        process.readSet = reads;
        process.writeSet = writes;
        return std::move(process);
    }

private:
    eir::Process process;
    SourceLocation current;
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> writes;
};

} // namespace enzan::test

#endif // ENZAN_TESTS_UNIT_SUPPORT_PROCESSBUILDER_H
