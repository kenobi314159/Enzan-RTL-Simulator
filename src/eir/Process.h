// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "eir/ParameterExpression.h"
#include "eir/Type.h"

#include <cstdint>
#include <string>
#include <vector>

namespace enzan::eir {

/// Which system task a SysTask instruction invokes.
///
/// The set is closed rather than open-ended: a system task has semantics the
/// scheduler and the waveform database need to know about — `$finish` ends the run,
/// `$dumpvars` opens a logging scope — so it cannot be dispatched by name at run
/// time without the runtime knowing the names anyway.
enum class SysTaskKind : std::uint8_t {
    /// `$display`: formats its arguments and appends a newline.
    Display,
    /// `$write`: as Display with no newline.
    Write,
    /// `$finish`: ends the simulation successfully.
    Finish,
    /// `$fatal`: reports and ends the simulation as a failure.
    Fatal,
    /// `$error`, `$warning`, `$info`: report at the matching severity and continue.
    Error,
    Warning,
    Info,
    /// `$dumpfile`: names the waveform file.
    DumpFile,
    /// `$dumpvars`: opens a logging scope.
    DumpVars,
    /// `$fdisplay` and `$fwrite`: as Display and Write, to the file descriptor or channels
    /// that are the first argument.
    FileDisplay,
    FileWrite,
    /// `$fclose(descriptor)`.
    FileClose,
    /// `$fflush`, of one descriptor or, with no argument, of every file.
    FileFlush,
    /// `$readmemh` and `$readmemb`: load the memory that is the call's target object from the
    /// file named by the first argument, between the optional start and end addresses that
    /// follow (IEEE 1800 section 21.4).
    ReadMemHex,
    ReadMemBinary,

    // System functions, run by SysFunction, which have a result.

    /// `$random`, IEEE 1364's generator. With no argument it draws from the simulation's
    /// own seed and its result is 32 bits; given its seed's value, it is 64 bits: the seed
    /// the caller writes back, above the 32-bit draw.
    Random,
    /// `$urandom`: 32 unsigned bits from the calling process's own generator, which a seed
    /// argument resets first (IEEE 1800 section 18.13.1).
    URandom,
    /// `$urandom_range(max, min)`: a draw between the two, either way round.
    URandomRange,
    /// `$fopen(name, mode)`: a file descriptor, or with no mode a channel of a multichannel
    /// descriptor; 0 when the file cannot be opened.
    FileOpen,
};

std::string_view toString(SysTaskKind kind) noexcept;

/// True for the kinds a SysFunction runs, which have a result; the rest are SysTask's.
[[nodiscard]] bool isSystemFunction(SysTaskKind kind) noexcept;

/// One argument of a system task call.
struct SysTaskArgument {
    /// A register holding the value, or a constant pool entry.
    Operand value;

    /// The argument's type, which supplies the signedness a `%d` conversion needs.
    /// Carried per argument because the format string does not say it.
    Type type;
};

/// A system task call, described once and referred to by index from the
/// instruction stream.
///
/// Kept out of the instruction encoding because an instruction is fixed-size and a
/// call has a variable number of arguments; this is also what lets the JIT lower a
/// SysTask to a single call with the descriptor index as its argument.
struct SysTaskCall {
    SysTaskKind kind = SysTaskKind::Display;

    /// Index into the process's string pool holding the format string, or
    /// `NoFormat` when the call had no string literal as its first argument.
    std::uint32_t formatIndex = NoFormat;

    std::vector<SysTaskArgument> arguments;

    /// The object a call writes, `$readmemh`'s memory, or `NoObject`. An object rather than
    /// an argument, because what the call needs is where the memory is, not its value.
    std::uint32_t objectIndex = NoObject;

    /// Where the call appears, so that a malformed format string can be reported
    /// against the user's source rather than against the generated code.
    SourceLocation location;

    static constexpr std::uint32_t NoFormat = 0xFFFF'FFFFU;
    static constexpr std::uint32_t NoObject = 0xFFFF'FFFFU;

    [[nodiscard]] bool hasFormat() const noexcept { return formatIndex != NoFormat; }
};

/// A straight-line run of instructions ending in exactly one terminator.
struct BasicBlock {
    /// A short name used when dumping EIR. Not semantically meaningful, but the
    /// difference between a readable dump and an unreadable one.
    std::string label;

    std::vector<Instruction> instructions;
};

/// What kind of procedural block a process came from.
///
/// The distinction survives into EIR because it changes scheduling: `always_comb`
/// runs once at time zero regardless of sensitivity, `always_ff` may not be
/// triggered by anything but its event control, and `initial` runs once and never
/// re-arms.
enum class ProcessKind : std::uint8_t {
    /// `initial`: runs once from time zero.
    Initial,
    /// `final`: runs once when the simulation ends.
    Final,
    /// `always`: re-arms on its explicit sensitivity after each run.
    Always,
    /// `always_comb`: sensitivity is every object it reads, and it additionally runs
    /// once at time zero so that combinational outputs are consistent before any
    /// input has changed.
    AlwaysComb,
    /// `always_latch`: as AlwaysComb; kept distinct so that the diagnostics and the
    /// GUI can name it as the user wrote it.
    AlwaysLatch,
    /// `always_ff`: re-arms on its explicit sensitivity and does not run at time
    /// zero.
    AlwaysFf,
    /// A continuous assignment (`assign`), modelled as a process sensitive to
    /// everything on its right-hand side.
    ContinuousAssign,
};

std::string_view toString(ProcessKind kind) noexcept;

/// True when the process re-arms and runs again after completing, rather than
/// running once.
bool isRepeating(ProcessKind kind) noexcept;

/// One entry of a process's sensitivity list.
struct SensitivityEntry {
    /// Index into the enclosing module's object table.
    std::uint32_t objectIndex = 0;

    EdgeKind edge = EdgeKind::AnyChange;

    /// The window of the object watched, for an event on a select such as `posedge clk[2]`:
    /// its lowest bit and its width, or a width of 0 for the whole object. An edge is the edge
    /// of the window's lowest bit, as IEEE 1800 section 9.4.2 takes an edge of a vector to be
    /// the edge of its least significant bit; any change is a change to any bit of it.
    std::uint32_t lowBit = 0;
    std::uint32_t width = 0;

    /// In a generic unit, the expressions `lowBit` and `width` are the values of, or
    /// `NoExpression` where the number stands for itself (DESIGN_GUIDE.md section 18, M5.1).
    std::uint32_t lowBitExpression = NoExpression;
    std::uint32_t widthExpression = NoExpression;

    friend bool operator==(SensitivityEntry, SensitivityEntry) noexcept = default;
};

/// The bits of one object a process writes.
///
/// A range rather than the object alone, because a driver owns the bits it assigns and not
/// the signal they are part of (DESIGN_GUIDE.md section 3.7): `N` instances each driving one
/// bit of `ready` are `N` disjoint drivers, and a write set that named only `ready` would make
/// them `N` drivers of one signal. A select whose position is not constant records every bit
/// it could reach, since a driver's range is fixed before anything runs.
struct WriteExtent {
    std::uint32_t objectIndex = 0;

    /// The lowest bit written, counted from the least significant bit of the storage.
    std::uint32_t lowBit = 0;

    /// How many bits, from `lowBit` upwards. Never zero.
    std::uint32_t width = 0;

    /// In a generic unit, the expressions `lowBit` and `width` are the values of, or
    /// `NoExpression` where the number stands for itself.
    std::uint32_t lowBitExpression = NoExpression;
    std::uint32_t widthExpression = NoExpression;

    [[nodiscard]] std::uint32_t endBit() const noexcept { return lowBit + width; }

    /// Every bit of an object `width` bits wide.
    static WriteExtent whole(std::uint32_t objectIndex, std::uint32_t width) noexcept {
        return {.objectIndex = objectIndex, .lowBit = 0, .width = width};
    }

    friend bool operator==(WriteExtent, WriteExtent) noexcept = default;
};

/// Puts extents into the form a write set holds: ordered by object and then by low bit, with
/// extents of one object that overlap or touch merged into one.
///
/// Touching extents are merged as well as overlapping ones because both belong to one
/// process, so splitting them distinguishes nothing, and one canonical form means two
/// processes that write the same bits declare the same set (section 3.8).
void normaliseWriteSet(std::vector<WriteExtent>& extents);

/// True when `writeSet`, which must be normalised, has an extent of `objectIndex`.
[[nodiscard]] bool writesObject(const std::vector<WriteExtent>& writeSet,
                                std::uint32_t objectIndex) noexcept;

/// The objects `writeSet` writes, each once and in order.
///
/// A store is routed by its object's position in this list (DESIGN_GUIDE.md section 3.7), so
/// codegen, which numbers the stores, and the runtime, which routes them, both take the list
/// from here.
[[nodiscard]] std::vector<std::uint32_t> writtenObjects(const std::vector<WriteExtent>& writeSet);

/// An EIR-B process: the body of one procedural block, as a control-flow graph.
///
/// **The core invariant.** A process is expressible as a pure function over an
/// explicit state arena, with a declared read set and write set (DESIGN_GUIDE.md
/// section 4.2). This is the single most important structural rule in the project:
/// static partitioning for threading, JIT compilation, activity-based scheduling
/// and checkpointing all depend on knowing exactly what a process touches. A
/// process that reaches arbitrary global state closes those doors permanently.
///
/// The invariant is not left to review. `verify()` in Verifier.h checks that every
/// object an instruction touches appears in the declared sets, and the frontends'
/// output is verified before it is accepted.
struct Process {
    ProcessKind kind = ProcessKind::Always;

    /// A readable identifier such as `always_ff@counter.sv:14`, used in diagnostics,
    /// in the Processes window and in delta-loop reports.
    std::string name;

    SourceLocation location;

    /// Type of each virtual register, indexed by register number. Registers are not
    /// in SSA form: the compiler reuses them, and `codegen` maps them to scratch
    /// offsets.
    std::vector<Type> registerTypes;

    /// Constant values referred to by LoadConstant and by system task arguments.
    std::vector<BitVector> constants;

    /// In a generic unit, the expression giving each register's width, by register number, or
    /// `NoExpression` for a width that is the same in every parameterisation. Empty when no
    /// register's width depends on a parameter.
    ///
    /// The width in `registerTypes` is the representative parameterisation's, which is what
    /// keeps a generic unit a valid unit for everything that ignores these (section 18, M5.1).
    std::vector<std::uint32_t> registerWidthExpressions;

    /// In a generic unit, the expression giving each constant, by index, or `NoExpression` for
    /// a constant that is the same in every parameterisation. Empty when none depends on one.
    std::vector<std::uint32_t> constantExpressions;

    /// String literals, referred to by system task descriptors.
    std::vector<std::string> strings;

    /// Basic blocks. Block 0 is the entry point.
    std::vector<BasicBlock> blocks;

    std::vector<SysTaskCall> sysCalls;

    /// Explicit sensitivity. Empty for `initial`, `final` and `always_comb`, whose
    /// triggering is derived from the kind and the read set instead.
    std::vector<SensitivityEntry> sensitivity;

    /// The event lists `WaitEvents` waits on, by index: `@(posedge a or negedge b)` inside a
    /// body is one, and resumes on whichever of its entries happens first.
    std::vector<std::vector<SensitivityEntry>> eventLists;

    /// Object indices this process reads. Sorted and duplicate-free, so that
    /// comparing or intersecting two processes' sets is cheap and so that iteration
    /// order cannot vary between runs (section 3.8).
    std::vector<std::uint32_t> readSet;

    /// The bits this process writes, normalised by `normaliseWriteSet` so that the same
    /// guarantees hold.
    std::vector<WriteExtent> writeSet;
};

/// True when `process` must be scheduled once at time zero, before anything it
/// reads has changed.
///
/// The answer depends on the sensitivity list as well as the kind, which is why it
/// is a function of the whole process:
///
/// - `initial` and a continuous assignment obviously start at time zero.
/// - `always_comb` and `always_latch` do, because IEEE 1800 defines them to
///   evaluate once at time zero so that combinational outputs are consistent
///   before any input has moved.
/// - `always` with **no** sensitivity list starts at time zero and then loops. This
///   is the `always #5 clk = ~clk;` clock generator, which is the single most common
///   construct in any testbench and would never run under a sensitivity-only model.
/// - `always` **with** a sensitivity list does not. The body blocks at its event
///   control before doing anything, so a design using `always @(*)` for
///   combinational logic genuinely holds X until an input moves. That is a
///   well-known Verilog trap and the reason `always_comb` exists; reproducing it is
///   correct, and quietly "fixing" it would make Enzan disagree with every other
///   simulator.
/// - `always_ff` does not. A flip-flop whose clock has not yet edged has not done
///   anything, and running its body at time zero would load a reset value without a
///   reset ever having been asserted.
/// - `final` runs at the end of the simulation, not the start.
bool startsAtTimeZero(const Process& process) noexcept;

} // namespace enzan::eir
