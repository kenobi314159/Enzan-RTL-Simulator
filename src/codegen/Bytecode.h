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
/// The opcode is EIR-B's own, deliberately: DESIGN_GUIDE.md section 6.1 wants the JIT
/// to be a mechanical translation of the interpreter, and sharing the opcode space
/// keeps the correspondence one-to-one instead of introducing a second vocabulary to
/// map between. What the bytecode adds is *resolution*: virtual registers become Word
/// offsets into the process's frame, blocks become instruction indices, and every
/// width the operation needs is present in the instruction.
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

    SourceLocation location;

    [[nodiscard]] bool hasFormat() const noexcept {
        return formatIndex != eir::SysTaskCall::NoFormat;
    }
};

/// Where a run of bytecode came from in the source.
///
/// One entry per *change* of location rather than one per instruction, which is what a
/// debugger's line table has always been and is why it is affordable: a statement compiles to
/// several instructions that all share a line, so the table is a small fraction of the code it
/// describes. Entries are sorted by `pc` and cover from their own `pc` up to the next one's.
struct LineEntry {
    /// Index of the first instruction this location covers.
    std::uint32_t pc = 0;

    SourceLocation location;
};

/// One compiled process body.
///
/// Shared by every instance of the module that declares it: the body refers to state
/// by EIR object index, and the runtime resolves those per instance. That is what
/// keeps compiled code proportional to the source rather than to the elaborated
/// hierarchy.
struct CompiledProcess {
    /// The instructions, with blocks laid out one after another.
    std::vector<Instruction> code;

    /// Instruction index each EIR block begins at. Kept so that a bytecode dump can
    /// be read against the EIR it came from, and so that a resume index recorded by a
    /// suspension can be checked.
    std::vector<std::uint32_t> blockStarts;

    /// Where each run of instructions came from, sorted by `pc`.
    ///
    /// Deliberately a side table and not a field on `Instruction`: a breakpoint or a step is
    /// asked about once, when the kernel is already stopping, and the interpreter's inner loop
    /// must not grow a field it never reads (section 13.4). A JIT keeps the same table against
    /// its own addresses.
    std::vector<LineEntry> lines;

    /// Size of the register frame, in Words.
    ///
    /// The frame holds every virtual register, wide ones included, which is what
    /// section 6.1 means by spilling wide vectors to a per-process scratch area: there
    /// is only one place values live, so nothing has to decide when to spill.
    std::uint32_t frameWords = 0;

    /// Constant values, laid out end to end in two-plane form.
    std::vector<eir::Word> constantPool;
    std::vector<Constant> constants;

    std::vector<std::string> strings;
    std::vector<SysTaskDescriptor> sysCalls;

    // --- Metadata the scheduler needs, copied from EIR ---------------------

    eir::ProcessKind kind = eir::ProcessKind::Always;
    std::string name;
    SourceLocation location;
    std::vector<eir::SensitivityEntry> sensitivity;
    std::vector<std::uint32_t> readSet;
    std::vector<std::uint32_t> writeSet;

    /// True when the process must be scheduled once at time zero.
    bool startsAtTimeZero = false;

    /// Returns where the instruction at `pc` came from, or an invalid location.
    ///
    /// A binary search over the line table, which is why the table is sorted. Called when the
    /// kernel has already decided to stop, never per instruction.
    [[nodiscard]] SourceLocation locationAt(std::uint32_t pc) const noexcept;

    /// Returns the first instruction index whose location is on `line` of `buffer`, or
    /// `NoInstruction`.
    ///
    /// What a breakpoint resolves to. The *first* match, because a line compiles to a run of
    /// instructions and stopping anywhere but the front of it would report a line the design
    /// has already half executed.
    [[nodiscard]] std::uint32_t firstInstructionOnLine(BufferId buffer, std::uint32_t lineStart,
                                                       std::uint32_t lineEnd) const noexcept;

    static constexpr std::uint32_t NoInstruction = 0xFFFF'FFFFU;

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
    [[nodiscard]] std::span<const CompiledProcess> bodies() const noexcept { return processes; }

    [[nodiscard]] const CompiledProcess& body(std::uint32_t index) const {
        return processes[index];
    }

    /// Returns the body index for a definition's process, or `NotCompiled`.
    [[nodiscard]] std::uint32_t bodyIndex(std::uint32_t definitionIndex,
                                          std::uint32_t processIndex) const;

    static constexpr std::uint32_t NotCompiled = 0xFFFF'FFFFU;

private:
    friend class Compiler;

    std::vector<CompiledProcess> processes;

    /// (definitionIndex, processIndex) to body index. A flat sorted vector rather than
    /// a hash map: it is built once, queried once per elaborated process, and its
    /// iteration order must not vary between runs (DESIGN_GUIDE.md section 3.8).
    struct Key {
        std::uint32_t definitionIndex;
        std::uint32_t processIndex;
        std::uint32_t bodyIndex;
    };

    std::vector<Key> keys;
};

} // namespace enzan::codegen
