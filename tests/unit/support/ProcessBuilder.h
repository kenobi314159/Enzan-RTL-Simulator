// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builders for hand-written EIR processes and subprograms.
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

/// What building a process and building a subprogram have in common: registers, constants,
/// blocks, the instructions in them, and the read and write sets the verifier requires.
template<typename BodyType>
class BodyBuilder {
public:
    std::uint32_t reg(eir::Type type) {
        body.registerTypes.push_back(type);
        return static_cast<std::uint32_t>(body.registerTypes.size() - 1);
    }

    std::uint32_t constant(std::string_view text) {
        body.constants.push_back(enzan::test::literal(text));
        return static_cast<std::uint32_t>(body.constants.size() - 1);
    }

    std::uint32_t string(std::string text) {
        body.strings.push_back(std::move(text));
        return static_cast<std::uint32_t>(body.strings.size() - 1);
    }

    std::uint32_t sysCall(eir::SysTaskCall call) {
        body.sysCalls.push_back(std::move(call));
        return static_cast<std::uint32_t>(body.sysCalls.size() - 1);
    }

    /// Starts a new basic block and returns its index.
    std::uint32_t block(std::string label) {
        eir::BasicBlock created;
        created.label = std::move(label);
        body.blocks.push_back(std::move(created));
        return static_cast<std::uint32_t>(body.blocks.size() - 1);
    }

    /// The index the next `block` will return, for a jump to a block not started yet.
    [[nodiscard]] std::uint32_t nextBlock() const {
        return static_cast<std::uint32_t>(body.blocks.size());
    }

    /// Sets the source location stamped onto everything emitted after it.
    ///
    /// The frontend stamps every instruction with the statement it came from, so a test that
    /// is about source lines — a breakpoint, a step — has to be able to say the same thing.
    void at(SourceLocation location) { current = location; }

    void emit(const eir::Instruction& instruction) {
        eir::Instruction stamped = instruction;
        stamped.location = current;
        body.blocks.back().instructions.push_back(stamped);
    }

    void readObject(std::uint32_t destination, std::uint32_t objectIndex) {
        reads.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::ReadObject, eir::Operand::reg(destination),
                                    eir::Operand::object(objectIndex)));
    }

    /// Reads the window of `objectIndex` as wide as `destination`, at the offset in
    /// `offsetRegister`.
    void readObjectPart(std::uint32_t destination, std::uint32_t objectIndex,
                        std::uint32_t offsetRegister) {
        reads.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::ReadObjectPart, eir::Operand::reg(destination),
                                    eir::Operand::object(objectIndex),
                                    eir::Operand::reg(offsetRegister)));
    }

    void store(std::uint32_t objectIndex, std::uint32_t source) {
        writes.push_back(eir::WriteExtent::whole(objectIndex, widthOf(source)));
        emit(eir::Instruction::make(eir::Opcode::Store, {}, eir::Operand::object(objectIndex),
                                    eir::Operand::reg(source)));
    }

    void storeNonBlocking(std::uint32_t objectIndex, std::uint32_t source) {
        writes.push_back(eir::WriteExtent::whole(objectIndex, widthOf(source)));
        emit(eir::Instruction::make(eir::Opcode::StoreNonBlocking, {},
                                    eir::Operand::object(objectIndex), eir::Operand::reg(source)));
    }

    /// A VHDL signal assignment's transaction, `delayRegister` from now: `Drive` when `inertial`,
    /// `DriveTransport` otherwise.
    void drive(std::uint32_t objectIndex, std::uint32_t source, std::uint32_t delayRegister,
               bool inertial = true) {
        writes.push_back(eir::WriteExtent::whole(objectIndex, widthOf(source)));
        emit(eir::Instruction::make(inertial ? eir::Opcode::Drive : eir::Opcode::DriveTransport, {},
                                    eir::Operand::object(objectIndex), eir::Operand::reg(source),
                                    eir::Operand::reg(delayRegister)));
    }

    /// An assignment to `width` bits of a signal from the offset in `offsetRegister`, which drives
    /// every bit of the object because nothing says which it will reach.
    void drivePart(std::uint32_t objectIndex, std::uint32_t source, std::uint32_t offsetRegister,
                   std::uint32_t objectWidth) {
        writes.push_back(eir::WriteExtent::whole(objectIndex, objectWidth));
        emit(eir::Instruction::make(eir::Opcode::DrivePart, {}, eir::Operand::object(objectIndex),
                                    eir::Operand::reg(source), eir::Operand::reg(offsetRegister)));
    }

    /// A part store whose index is a register, which drives every bit of the object because
    /// nothing says which it will reach.
    void storePartNonBlocking(std::uint32_t objectIndex, std::uint32_t source,
                              std::uint32_t indexRegister, std::uint32_t objectWidth) {
        writes.push_back(eir::WriteExtent::whole(objectIndex, objectWidth));
        emit(eir::Instruction::make(eir::Opcode::StorePartNonBlocking, {},
                                    eir::Operand::object(objectIndex), eir::Operand::reg(source),
                                    eir::Operand::reg(indexRegister)));
    }

    /// Calls `subprogram` of `definition` (empty for this body's own), passing `arguments` and
    /// returning a function's result into `result`.
    void call(std::string definition, std::string subprogram, std::vector<eir::Operand> arguments,
              eir::Operand result = eir::Operand::none()) {
        const auto index = static_cast<std::uint32_t>(body.calls.size());
        body.calls.push_back(eir::CallSite{.definition = std::move(definition),
                                           .subprogram = std::move(subprogram),
                                           .arguments = std::move(arguments),
                                           .result = result,
                                           .location = current});
        emit(eir::Instruction::make(eir::Opcode::Call, {}, eir::Operand::call(index)));
    }

protected:
    BodyBuilder() = default;

    /// Writes the sets gathered so far into the body and hands it over.
    BodyType finishBody() {
        std::ranges::sort(reads);
        reads.erase(std::ranges::unique(reads).begin(), reads.end());
        eir::normaliseWriteSet(writes);
        body.readSet = reads;
        body.writeSet = writes;
        return std::move(body);
    }

    /// The body being built, for a builder of one kind of body to fill in what only it has.
    BodyType body;

    std::vector<std::uint32_t> reads;

private:
    /// A whole store moves a register as wide as its object, which the verifier checks.
    [[nodiscard]] std::uint32_t widthOf(std::uint32_t source) const {
        return body.registerTypes[source].bitWidth();
    }

    SourceLocation current;
    std::vector<eir::WriteExtent> writes;
};

/// Builds one EIR process, tracking the read and write sets the verifier requires.
///
/// Writing these by hand without a builder means every test also maintains two sorted
/// sets, which is exactly the bookkeeping the verifier exists to catch — so the tests
/// would end up testing the bookkeeping rather than the kernel.
class ProcessBuilder : public BodyBuilder<eir::Process> {
public:
    ProcessBuilder(eir::ProcessKind kind, std::string name) {
        body.kind = kind;
        body.name = std::move(name);
    }

    void sensitive(std::uint32_t objectIndex, eir::EdgeKind edge) {
        reads.push_back(objectIndex);
        body.sensitivity.push_back(eir::SensitivityEntry{.objectIndex = objectIndex, .edge = edge});
    }

    void waitEdge(std::uint32_t objectIndex, eir::EdgeKind edge, std::uint32_t resumeBlock) {
        reads.push_back(objectIndex);
        emit(eir::Instruction::make(eir::Opcode::WaitEdge, {}, eir::Operand::object(objectIndex),
                                    eir::Operand::immediate(static_cast<std::uint32_t>(edge)),
                                    eir::Operand::block(resumeBlock)));
    }

    /// Waits for whichever of `entries` happens first, as `@(a or posedge b)` does.
    void waitEvents(std::vector<eir::SensitivityEntry> entries, std::uint32_t resumeBlock) {
        for (const eir::SensitivityEntry& entry : entries) reads.push_back(entry.objectIndex);
        const auto list = static_cast<std::uint32_t>(body.eventLists.size());
        body.eventLists.push_back(std::move(entries));
        emit(eir::Instruction::make(eir::Opcode::WaitEvents, {}, eir::Operand::immediate(list),
                                    eir::Operand::block(resumeBlock)));
    }

    eir::Process finish() { return finishBody(); }
};

/// Builds one EIR subprogram: its parameters and result as well as its body.
class SubprogramBuilder : public BodyBuilder<eir::Subprogram> {
public:
    explicit SubprogramBuilder(std::string name) { body.name = std::move(name); }

    /// Declares the next parameter and returns the register holding it.
    std::uint32_t parameter(std::string name, eir::Type type,
                            eir::ParameterMode mode = eir::ParameterMode::In) {
        const std::uint32_t registerIndex = reg(type);
        body.parameters.push_back(eir::SubprogramParameter{
            .name = std::move(name), .mode = mode, .registerIndex = registerIndex});
        return registerIndex;
    }

    /// Makes the subprogram a function of `type`, and returns the register holding its result.
    std::uint32_t result(eir::Type type) {
        body.resultType = type;
        body.resultRegister = reg(type);
        return body.resultRegister;
    }

    eir::Subprogram finish() { return finishBody(); }
};

} // namespace enzan::test

#endif // ENZAN_TESTS_UNIT_SUPPORT_PROCESSBUILDER_H
