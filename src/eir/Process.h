// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/BitVector.h"
#include "eir/Instruction.h"
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
    /// VHDL's `report`, and its `assert` when the condition is false: the first argument is the
    /// severity, as the position of `std.standard.severity_level` (note, warning, error,
    /// failure), and the format string with the rest is the message. A failure stops the run,
    /// as GHDL's default does; an error is reported and the run goes on to fail at its end.
    Report,
    /// std.textio's `deallocate` of a line: frees what the argument's handle designates, a handle
    /// into the kernel's heap of access values.
    LineDeallocate,
    /// VHDL's `write` to a file of a type other than text: writes the second argument's bits to
    /// the file the first names, in whole bytes, the lowest first. A third argument, for a file
    /// of an unconstrained array type, is the value's count of elements, written before it in
    /// four bytes, the lowest first, for a read to learn its length from.
    FileWriteValue,

    // Strings whose lengths only the run knows, held in the kernel's scratch (DESIGN_GUIDE.md
    // section 4.1) by handle: the first argument of each, but StringMark's and StringRelease's.
    // The kernel keeps each one's bounds, and an index of one is of those bounds.

    /// Writes the characters of the second argument, the leftmost in its highest bits, into the
    /// string from the index the first after the handle holds; a part reaching outside the string
    /// ends the run with a failure.
    StringSplice,
    /// Releases every string made since the mark the argument holds, which StringMark gave.
    StringRelease,

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
    /// `$fgetc(fd)`: the next byte of the file the descriptor names, or -1 at its end and when
    /// the descriptor names no file open for reading (IEEE 1800 section 21.3.4.1).
    FileGetCharacter,

    // std.textio, run natively over the kernel's heap, whose lines are handles: 0 for `null`, and
    // otherwise numbered as they are allocated, freed ones reused last first, so that a run
    // allocates the same handles every time.

    /// `write`: appends the message the format and the arguments after the first three make to
    /// the line the first argument designates, allocating one when it is null, justified as the
    /// second says (0 right, 1 left) in a field as wide as the third. The result is the line's
    /// handle.
    LineWrite,
    /// `writeline`: writes the line the second argument designates, and a line feed, to the file
    /// the first names, frees it, and results in the handle of a new empty line.
    LineWriteLine,
    /// `readline`: frees the line the second argument designates and results in the handle of a
    /// new one holding the next line of the file the first names, without its line end.
    LineReadLine,
    /// `read` of a character: takes the first character off the line the first argument
    /// designates. The result is the character's code in its low byte, and above it a bit saying
    /// whether there was one; with a second argument that is not 0, a `read` with no `good` to
    /// tell, an empty line ends the run with a failure instead.
    LineReadCharacter,
    /// `read` of a string: the string the second argument holds with its characters replaced by
    /// as many taken off the line the first argument designates, the leftmost in the highest
    /// bits, and above them a bit saying whether the line held that many. A line holding fewer
    /// keeps them and gives the string back as it was, as GHDL's textio does; with a third
    /// argument that is not 0, a `read` with no `good` to tell, it ends the run with a failure.
    LineReadString,
    /// The length of the line the argument designates, its 'LENGTH, 'HIGH and 'RIGHT: a line
    /// the runtime holds is indexed from 1, as NVC's textio makes every one. A null line ends the
    /// run with a failure.
    LineLength,
    /// The character of the line the first argument designates at the index the second holds, in
    /// the result's low byte; an index outside the line ends the run with a failure.
    LineElement,
    /// The characters of the line the first argument designates from the index the second holds,
    /// as many as fill the result, the leftmost in its highest bits; a slice reaching outside the
    /// line ends the run with a failure.
    LineSlice,
    /// `endfile`: whether the file the argument names has nothing left to read.
    FileEnd,
    /// `write` of a string whose length only the run shows, built in a line of its own: appends
    /// the line the fourth argument designates, and frees it, to the one the first designates,
    /// justified and in a field as for LineWrite. The result is the first line's handle.
    LineAppend,
    /// VHDL's `read` from a file of a type other than text: the next value of the file the
    /// argument names, as FileWriteValue wrote it, as wide as the result. With a second argument,
    /// the width of an element of an unconstrained array, and a third, the array read into, the
    /// value is one written with its count of elements: the result is how many of them were read
    /// in its top 32 bits, and below them the array with its leftmost elements replaced by the
    /// value's leftmost, as many as fit.
    FileReadValue,

    /// A new string of run-time length, of the bounds the first two arguments give, ascending
    /// where the third is not 0, each character the fourth's code. The result is its handle.
    StringNew,
    /// The string's left bound in the highest 64 bits of the result, its right below that, and
    /// its length in the lowest 64.
    StringBounds,
    /// The characters of the string from the index the second argument holds, as many as fill
    /// the result, the leftmost in its highest bits; one is an element. A part reaching outside
    /// the string ends the run with a failure.
    StringSlice,
    /// A new string holding the elements of the array the first argument holds, of the bounds the
    /// second and third give, from the index the fourth holds to the one the fifth does, in the
    /// direction the sixth says, ascending where it is not 0. A slice reaching outside the array
    /// ends the run with a failure. The result is its handle.
    StringFromArray,
    /// The array the first argument holds, of the bounds the second and third give, with the
    /// elements at the indices of the string the fourth designates replaced by its characters.
    StringIntoArray,
    /// How the string compares with the one the second argument designates, character by
    /// character from the left and a shorter one first where one runs out: -1, 0 or 1, in 8 bits.
    StringCompare,
    /// std.textio's `read` into the string: as many characters as it holds taken off the line
    /// the second argument designates, where it holds that many, as LineReadString takes them,
    /// with the third argument as LineReadString's. The result is whether it did.
    StringRead,
    /// How many strings of run-time length there are, the mark StringRelease releases to.
    StringMark,
    /// VHDL's `'path_name` of something in the calling process's instance, or with the argument
    /// not 0 its `'instance_name` (IEEE 1076 section 16.2.5): a new string, ascending from 1, of
    /// the instance's path, `:tb:u:` or `:tb(a):u@child(rtl):`, and after it the characters of
    /// the format string, which name what is in the instance. The result is its handle.
    PathName,
};

/// The severities a Report takes as its first argument: the positions of
/// `std.standard.severity_level`.
inline constexpr std::uint8_t SeverityNote = 0;
inline constexpr std::uint8_t SeverityWarning = 1;
inline constexpr std::uint8_t SeverityError = 2;
inline constexpr std::uint8_t SeverityFailure = 3;

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

    /// The time unit, as a power of ten of a second, of the scope the call is in: what a `%t`
    /// argument counts (IEEE 1800 section 21.3.1), as `$time` there does. `RunUnit` for a call
    /// whose times count the run's unit, as VHDL's do.
    std::int8_t timeUnit = RunUnit;

    static constexpr std::uint32_t NoFormat = 0xFFFF'FFFFU;
    static constexpr std::uint32_t NoObject = 0xFFFF'FFFFU;
    static constexpr std::int8_t RunUnit = 127;

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
    /// What a procedural `assign` or `force` assigns once it has started: the value evaluated
    /// again whenever anything it reads changes, and written while the statement's override
    /// holds its target (IEEE 1800 section 10.6). The statement itself starts the override and
    /// writes its first value, so this never runs at time zero.
    Override,
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
/// processes that write the same bits declare the same set.
void normaliseWriteSet(std::vector<WriteExtent>& extents);

/// True when `writeSet`, which must be normalised, has an extent of `objectIndex`.
[[nodiscard]] bool writesObject(const std::vector<WriteExtent>& writeSet,
                                std::uint32_t objectIndex) noexcept;

/// The objects `writeSet` writes, each once and in order.
///
/// A store is routed by its object's position in this list, so codegen, which numbers the
/// stores, and the runtime, which routes them, both take the list from here.
[[nodiscard]] std::vector<std::uint32_t> writtenObjects(const std::vector<WriteExtent>& writeSet);

/// How a subprogram's parameter passes its value.
///
/// By copy in both directions, which is what VHDL requires of a scalar and allows of a
/// composite, and what SystemVerilog's `input`, `output` and `inout` mean.
enum class ParameterMode : std::uint8_t {
    /// Copied into the callee's register when it is called: `in`, `input`.
    In,
    /// Copied back into the caller's register when the callee returns: `out`, `output`.
    Out,
    /// Both: `inout`.
    InOut,
};

std::string_view toString(ParameterMode mode) noexcept;

/// One parameter of a subprogram.
struct SubprogramParameter {
    std::string name;

    ParameterMode mode = ParameterMode::In;

    /// The register of the subprogram's own register file that holds the parameter.
    std::uint32_t registerIndex = 0;

    friend bool operator==(const SubprogramParameter&, const SubprogramParameter&) = default;
};

/// A call of a subprogram, described once and referred to by index from the instruction
/// stream, as a system task call is.
///
/// Kept out of the instruction for the same reason: a call has as many arguments as its callee
/// has parameters, and an instruction is fixed-size.
struct CallSite {
    /// The definition declaring the callee, named as an instance names the definition it
    /// instantiates (`Instance::definitionName`), or empty for the calling body's own.
    ///
    /// A name rather than an index for the same reason as there: a package may be analysed in
    /// another library, or later, than the unit calling into it.
    std::string definition;

    /// The callee's name within that definition (`Subprogram::name`).
    std::string subprogram;

    /// One per parameter of the callee, in order: a register or a constant for an `In`
    /// parameter, and for an `Out` or `InOut` one the register its value is copied back into.
    std::vector<Operand> arguments;

    /// The register a function's result is copied into, or no operand for a procedure.
    Operand result;

    SourceLocation location;
};

/// What a process and a subprogram both are: a body of EIR-B over registers of its own, as a
/// control-flow graph, with the read and write sets of everything it touches.
///
/// **The core invariant.** A body is expressible as a pure function over an explicit state
/// arena, with a declared read set and write set (DESIGN_GUIDE.md section 4.2). This is the
/// single most important structural rule in the project: static partitioning for threading,
/// JIT compilation, activity-based scheduling and checkpointing all depend on knowing exactly
/// what a process touches. A body that reaches arbitrary global state closes those doors
/// permanently.
///
/// The invariant is not left to review. `verify()` in Verifier.h checks that every object an
/// instruction touches appears in the declared sets, and the frontends' output is verified
/// before it is accepted. A body's sets are its own: what its callees touch is theirs, and
/// elaboration adds it to a process's when it binds the calls.
struct Body {
    /// A readable identifier such as `always_ff@counter.sv:14`, used in diagnostics, in the
    /// Processes window and in delta-loop reports. For a subprogram, the name calls name it by.
    std::string name;

    SourceLocation location;

    /// Type of each virtual register, indexed by register number. Registers are not in SSA
    /// form: the compiler reuses them, and `codegen` maps them to scratch offsets.
    std::vector<Type> registerTypes;

    /// Constant values referred to by LoadConstant, by system task arguments and by calls.
    std::vector<BitVector> constants;

    /// String literals, referred to by system task descriptors.
    std::vector<std::string> strings;

    /// Basic blocks. Block 0 is the entry point.
    std::vector<BasicBlock> blocks;

    std::vector<SysTaskCall> sysCalls;

    /// The subprograms this body calls, referred to by `Call` instructions.
    std::vector<CallSite> calls;

    /// Object indices this body reads. Sorted and duplicate-free, so that comparing or
    /// intersecting two bodies' sets is cheap and so that iteration order cannot vary between
    /// runs.
    std::vector<std::uint32_t> readSet;

    /// The bits this body writes, normalised by `normaliseWriteSet` so that the same
    /// guarantees hold.
    std::vector<WriteExtent> writeSet;
};

/// A strength a driver drives a value with (IEEE 1800 section 28.11), of the four levels a
/// value can be driven at. Resolution tells only two classes apart, `Supply` and `Strong` the
/// strong, `Pull` and `Weak` the weak, and elaboration refuses drivers whose levels the standard
/// orders and a class does not (DESIGN_GUIDE.md section 3.7).
enum class Strength : std::uint8_t { Supply, Strong, Pull, Weak };

/// True for `Pull` and `Weak`, which yield to any driver of the strong class.
[[nodiscard]] constexpr bool isWeak(Strength strength) noexcept {
    return strength == Strength::Pull || strength == Strength::Weak;
}

/// The strengths a driver drives 0 and 1 with; strong for both unless declared otherwise.
struct DriveStrength {
    Strength zero = Strength::Strong;
    Strength one = Strength::Strong;

    /// True when either value is driven in the weak class.
    [[nodiscard]] constexpr bool anyWeak() const noexcept { return isWeak(zero) || isWeak(one); }

    friend bool operator==(const DriveStrength&, const DriveStrength&) = default;
};

/// An EIR-B process: the body of one procedural block, scheduled by the kernel.
struct Process : Body {
    ProcessKind kind = ProcessKind::Always;

    /// True when every object the process writes, it drives, as a continuous assignment drives
    /// its target, rather than merely writing it as a procedural block does.
    ///
    /// A VHDL process is one (IEEE 1076 section 14.7.2): each signal it assigns has a driver of
    /// its own in that process, so two processes assigning one unresolved signal are an error and
    /// not a race the last write wins. Elaboration treats its writes as it treats a continuous
    /// assignment's.
    bool drivesSignals = false;

    /// What a continuous assignment drives with, as `assign (strong1, weak0)` declares it. Every
    /// other process drives strong.
    DriveStrength strength;

    /// True for a VHDL postponed process (IEEE 1076 section 11.3): past initialization, it
    /// resumes only in the last delta cycle of a time step, once nothing else is due then, and so
    /// sees the values the step ends with.
    bool postponed = false;

    /// True for what the VHDL adapter makes of a port associated by parts or through a conversion
    /// (IEEE 1076 section 6.5.6.3): a copy from one side of the association to the other, which is
    /// the connection itself rather than a process of the design. The kernel runs it as part of
    /// the signal update, so what it copies changes in the same delta cycle as what it copies
    /// from, and once at initialization, so the signal it drives starts at what it copies.
    bool connectsPort = false;

    /// Explicit sensitivity. Empty for `initial`, `final` and `always_comb`, whose
    /// triggering is derived from the kind and the read set instead.
    std::vector<SensitivityEntry> sensitivity;

    /// The event lists `WaitEvents` waits on, by index: `@(posedge a or negedge b)` inside a
    /// body is one, and resumes on whichever of its entries happens first.
    ///
    /// An empty list waits for nothing, so the process never resumes: VHDL's `wait;`, which is
    /// how a testbench process ends.
    std::vector<std::vector<SensitivityEntry>> eventLists;

    /// The objects the process's `assign`, `force`, `deassign` and `release` name, sorted and
    /// each once. Declared beside the write set rather than in it, because an override writes an
    /// object without driving it: a forced net keeps its drivers, which take it back on release
    /// (DESIGN_GUIDE.md section 3.7).
    std::vector<std::uint32_t> overrideSet;
};

/// A body that is called rather than scheduled: a VHDL function or procedure, and in time a
/// SystemVerilog function.
///
/// It runs to its `Return` without suspending — a subprogram that waits is inlined into its
/// caller instead, which is what lets every call's registers live on the kernel's one stack —
/// and it writes only objects that no driver resolves, since a store into a resolved signal
/// goes through the drivers of the process making it.
struct Subprogram : Body {
    std::vector<SubprogramParameter> parameters;

    /// The result: void for a procedure.
    Type resultType;

    /// The register holding the result when the body returns. Meaningless for a procedure.
    std::uint32_t resultRegister = 0;

    [[nodiscard]] bool isFunction() const noexcept { return !resultType.isVoid(); }
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
