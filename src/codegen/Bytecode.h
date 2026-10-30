// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace enzan::codegen {

/// One executable instruction.
///
/// The opcode is EIR-B's own, deliberately: the JIT is to be a mechanical translation of the
/// interpreter, and sharing the opcode space keeps the correspondence one-to-one instead of
/// introducing a second vocabulary to map between. What the bytecode adds is *resolution*: virtual
/// registers become Word offsets into the process's frame, blocks become instruction indices, and
/// every width the operation needs is present in the instruction.
///
/// **Nothing here requires a lookup in a side table.** That is what lets the
/// interpreter's inner loop touch only the instruction and the data, and what lets a
/// JIT emit code for an instruction in isolation. The cost is 48 bytes per
/// instruction, which is cache-unfriendly by the standards of a tight bytecode and is
/// the first thing to revisit when M8 measures the interpreter.
struct Instruction {
    eir::Opcode opcode = eir::Opcode::Nop;

    /// True when the operation treats its operands as signed.
    bool isSigned = false;

    /// True when the first source operand is a real, so that arithmetic and comparison are a
    /// double's and Convert goes from a real to an integer rather than the other way.
    bool isReal = false;

    /// Operand values. What each one means depends on the opcode, exactly as in
    /// EIR-B (see eir/Opcodes.def), with these resolutions applied:
    ///
    /// - a register operand is a Word offset into the process's frame;
    /// - a block operand is an index into the instruction array;
    /// - an object operand is an EIR object index, which the runtime turns into an
    ///   arena offset through its instance's table;
    /// - a constant operand indexes the compiled constant pool;
    /// - a system task operand indexes the descriptor list;
    /// - an immediate operand is the value itself.
    ///
    /// A store has no destination register, and its `dst` is instead the position of its
    /// object among the objects the process writes (`eir::writtenObjects`): the index into
    /// the bound process's routes, which say whether the store goes straight into the
    /// signal or through drivers to be resolved.
    std::uint32_t dst = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;

    /// Width in bits of the corresponding operand, when it is a register. Zero for
    /// operands that are not registers.
    ///
    /// Present per operand rather than once per instruction because several
    /// operations mix widths legitimately: a shift amount, a bit index and a
    /// condition are all self-determined in SystemVerilog and need not match the
    /// value they act on.
    std::uint32_t dstWidth = 0;
    std::uint32_t aWidth = 0;
    std::uint32_t bWidth = 0;
    std::uint32_t cWidth = 0;
};

/// One entry of a compiled constant pool.
struct Constant {
    /// Offset of the value's storage in the pool's Word array.
    std::uint32_t wordOffset = 0;
    std::uint32_t bitWidth = 0;
};

/// A system task call, with its arguments resolved to frame offsets.
struct SysTaskDescriptor {
    eir::SysTaskKind kind = eir::SysTaskKind::Display;

    /// Index into the compiled string pool, or `eir::SysTaskCall::NoFormat`.
    std::uint32_t formatIndex = eir::SysTaskCall::NoFormat;

    struct Argument {
        /// True when the value comes from the constant pool rather than the frame.
        bool isConstant = false;

        /// Frame Word offset, or constant pool index.
        std::uint32_t location = 0;

        std::uint32_t bitWidth = 0;

        /// Signedness for a `%d` conversion, which the format string does not carry.
        bool isSigned = false;
    };

    std::vector<Argument> arguments;

    /// The object the call writes, or `eir::SysTaskCall::NoObject`.
    std::uint32_t objectIndex = eir::SysTaskCall::NoObject;

    /// The unit a `%t` argument counts, as `eir::SysTaskCall::timeUnit`.
    std::int8_t timeUnit = eir::SysTaskCall::RunUnit;

    SourceLocation location;

    [[nodiscard]] bool hasFormat() const noexcept {
        return formatIndex != eir::SysTaskCall::NoFormat;
    }
};

/// The caller's half of a call: where each argument is, and where the result goes.
///
/// The callee's half — where its parameters and its result live in its own registers — is on
/// the callee's body, and which body that is, elaboration decides (elab::SubprogramInstance):
/// one call in a compiled body reaches a different binding of the callee in each instance.
struct CallDescriptor {
    struct Argument {
        /// True when the value comes from the constant pool rather than the frame.
        bool isConstant = false;

        /// Frame Word offset, or constant pool index.
        std::uint32_t location = 0;

        std::uint32_t bitWidth = 0;
    };

    /// One per parameter of the callee, in order.
    std::vector<Argument> arguments;

    /// True for a call of a function, whose result is copied into `resultLocation`.
    bool hasResult = false;
    std::uint32_t resultLocation = 0;
    std::uint32_t resultWidth = 0;

    SourceLocation location;
};

/// One compiled body: a process's, or a subprogram's.
///
/// Shared by every instance of the module that declares it: the body refers to state
/// by EIR object index, and the runtime resolves those per instance. That is what
/// keeps compiled code proportional to the source rather than to the elaborated
/// hierarchy.
struct CompiledBody {
    /// The instructions, with blocks laid out one after another.
    std::vector<Instruction> code;

    /// Instruction index each EIR block begins at. Kept so that a bytecode dump can
    /// be read against the EIR it came from, and so that a resume index recorded by a
    /// suspension can be checked.
    std::vector<std::uint32_t> blockStarts;

    /// Size of the register frame, in Words.
    ///
    /// The frame holds every virtual register, wide ones included, so it is also the
    /// per-process scratch area wide vectors spill to: there is only one place values
    /// live, so nothing has to decide when to spill.
    std::uint32_t frameWords = 0;

    /// What the frame holds as the body starts, `frameWords` long: zero, but for the registers
    /// that hold one constant throughout, which hold it from the start. Their loads are not
    /// emitted, so a frame starts as a copy of this rather than as zeros.
    std::vector<eir::Word> frameImage;

    /// Constant values, laid out end to end in two-plane form.
    std::vector<eir::Word> constantPool;
    std::vector<Constant> constants;

    std::vector<std::string> strings;
    std::vector<SysTaskDescriptor> sysCalls;

    /// The calls this body makes, indexed by a `Call` instruction's first operand.
    std::vector<CallDescriptor> calls;

    // --- What a call of a subprogram's body needs --------------------------

    /// Each parameter's register, as a frame Word offset, with its width and mode.
    struct Parameter {
        std::uint32_t offset = 0;
        std::uint32_t bitWidth = 0;
        eir::ParameterMode mode = eir::ParameterMode::In;
    };

    std::vector<Parameter> parameters;

    /// The register holding a function's result when the body returns.
    std::uint32_t resultOffset = 0;
    std::uint32_t resultWidth = 0;

    // --- Metadata the scheduler needs, copied from EIR; a subprogram's is unused --

    eir::ProcessKind kind = eir::ProcessKind::Always;

    /// True for a VHDL process, eir::Process::drivesSignals: its wait of no time resumes in the
    /// next simulation cycle, once the signals have taken what that cycle's transactions assign
    /// (IEEE 1076 section 14.7.5.3), where `#0` resumes in the Inactive region before them.
    bool resumesNextCycle = false;

    /// eir::Process::postponed: past initialization, the kernel resumes it in the Postponed region
    /// rather than in Active.
    bool postponed = false;

    /// eir::Process::connectsPort: the kernel runs it as part of the signal update.
    bool connectsPort = false;

    std::string name;
    SourceLocation location;
    std::vector<eir::SensitivityEntry> sensitivity;
    std::vector<std::vector<eir::SensitivityEntry>> eventLists;
    std::vector<std::uint32_t> readSet;
    std::vector<eir::WriteExtent> writeSet;

    /// True when the process must be scheduled once at time zero.
    bool startsAtTimeZero = false;

    /// Returns a read-only view of constant `index`.
    [[nodiscard]] eir::ConstBitRef constant(std::uint32_t constantNumber) const noexcept {
        const Constant& entry = constants[constantNumber];
        return {constantPool.data() + entry.wordOffset, entry.bitWidth};
    }
};

/// Every compiled body of a design, addressed the way the elaborated design addresses
/// processes.
class CompiledDesign {
public:
    [[nodiscard]] std::span<const CompiledBody> bodies() const noexcept { return processes; }

    [[nodiscard]] const CompiledBody& body(std::uint32_t index) const { return processes[index]; }

    /// Returns the body index for a definition's process, or `NotCompiled`.
    [[nodiscard]] std::uint32_t bodyIndex(std::uint32_t definitionIndex,
                                          std::uint32_t processIndex) const;

    /// Returns the body index for a definition's subprogram, or `NotCompiled`.
    [[nodiscard]] std::uint32_t subprogramBodyIndex(std::uint32_t definitionIndex,
                                                    std::uint32_t subprogramIndex) const;

    static constexpr std::uint32_t NotCompiled = 0xFFFF'FFFFU;

private:
    friend class Compiler;

    std::vector<CompiledBody> processes;

    /// (definitionIndex, processIndex) to body index. A flat sorted vector rather than
    /// a hash map: it is built once, queried once per elaborated process, and its
    /// iteration order must not vary between runs.
    struct Key {
        std::uint32_t definitionIndex;
        std::uint32_t processIndex;
        std::uint32_t bodyIndex;
    };

    std::vector<Key> keys;

    /// The same, for subprograms, whose `processIndex` is the subprogram's index.
    std::vector<Key> subprogramKeys;

    static std::uint32_t findBody(const std::vector<Key>& keys, std::uint32_t definitionIndex,
                                  std::uint32_t index);
};

} // namespace enzan::codegen
