// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include "slang/numeric/ConstantValue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slang {
class SourceLocation;
struct TimeScale;
struct TimeScaleValue;
} // namespace slang

namespace slang::ast {
enum class EdgeKind;
class Symbol;
class CallExpression;
class Scope;
class SubroutineSymbol;
class VariableSymbol;
class ElementSelectExpression;
class Expression;
class MemberAccessExpression;
class PrimitiveInstanceSymbol;
class RangeSelectExpression;
class Statement;
class StatementBlockSymbol;
class TimingControl;
class Type;
class ValueSymbol;
} // namespace slang::ast

namespace enzan::frontend::sv {

/// The subroutine `symbol` is a static variable of, declared in its body or in a block of it;
/// null for anything else, an argument, the return value and an automatic variable among them.
///
/// Such a variable is one variable however many calls there are, and keeps its value from one
/// call to the next, so it is storage of the module or package declaring the subroutine
/// rather than a register of each call.
[[nodiscard]] const slang::ast::SubroutineSymbol*
staticSubroutineOf(const slang::ast::ValueSymbol& symbol);

/// A slang time unit or precision as a power of ten of a second: `10ns` is -8.
[[nodiscard]] std::int8_t timeExponentOf(const slang::TimeScaleValue& value) noexcept;

/// The module paths into one output (IEEE 1800 section 30), as the module gathers them for the
/// process that delays the output's changes by them.
struct OutputPaths {
    /// An input window a path starts at, with the edge it counts, `None` counting every change.
    struct Source {
        std::uint32_t objectIndex = 0;
        std::uint32_t lowBit = 0;
        std::uint32_t width = 0;
        slang::ast::EdgeKind edge{};
    };

    struct Path {
        /// Index into `sources`.
        std::uint32_t source = 0;

        /// The path's condition, null for one that always holds.
        const slang::ast::Expression* condition = nullptr;

        /// The delays of the six transitions the standard names, 0 to 1, 1 to 0, 0 to z, z to 1,
        /// 1 to z and z to 0, each a constant expression.
        std::array<const slang::ast::Expression*, 6> delays{};
    };

    /// The output port's object, which the process writes, and the one the body's own writes of
    /// it write instead.
    std::uint32_t output = 0;
    std::uint32_t internal = 0;

    /// The bits of the output the process delays: a run of them whose paths are the same, the
    /// whole output when every bit's are.
    std::uint32_t lowBit = 0;
    std::uint32_t width = 0;

    /// The process's own state: the sources' values as it last took them, the time each last
    /// changed, 64 bits apiece, and the value it last scheduled for its bits of the output.
    std::uint32_t sourcesSeen = 0;
    std::uint32_t changedAt = 0;
    std::uint32_t scheduled = 0;

    std::vector<Source> sources;
    std::vector<Path> paths;

    /// True when the pulse limits are zero, so that every change reaches the output; otherwise a
    /// change replaces one still on its way, as the default limits have it.
    bool transport = false;
};

/// What a process translator needs from the module it belongs to.
///
/// An interface rather than a direct dependency so that statement and expression
/// translation stays in its own file: the module side owns the object table and the
/// location mapping, and the process side asks for them. The virtual calls are paid once
/// per AST node at analysis time, never during simulation.
class ModuleContext {
public:
    ModuleContext() = default;
    ModuleContext(const ModuleContext&) = delete;
    ModuleContext& operator=(const ModuleContext&) = delete;
    ModuleContext(ModuleContext&&) = delete;
    ModuleContext& operator=(ModuleContext&&) = delete;
    virtual ~ModuleContext() = default;

    /// Returns the EIR object index for `symbol`, creating one if the symbol has not been
    /// seen.
    ///
    /// Creating on demand rather than requiring every variable to have been collected
    /// first: block-local variables and `for` loop variables live in scopes that slang
    /// attaches in places that have changed between releases, and a frontend that depends
    /// on where they are attached breaks on upgrade. Asking for an object when one is
    /// needed cannot miss any.
    virtual std::optional<std::uint32_t> objectFor(const slang::ast::ValueSymbol& symbol) = 0;

    /// Converts a slang type to an EIR type, or nothing if it is not supported.
    virtual std::optional<eir::Type> convertType(const slang::ast::Type& type) = 0;

    /// The bits of a constant of `type`, laid out as the storage of that type lays them out,
    /// or nothing when the constant is not one Enzan can hold.
    virtual std::optional<eir::BitVector> constantBits(const slang::ConstantValue& value,
                                                       const slang::ast::Type& type) = 0;

    /// Maps a slang source location into Enzan's source manager.
    virtual SourceLocation convertLocation(slang::SourceLocation location) = 0;

    /// Reports a construct the frontend does not handle yet.
    virtual void reportUnsupported(slang::SourceLocation location, std::string_view what) = 0;

    /// The module being built, for object types and widths.
    [[nodiscard]] virtual const eir::ModuleDefinition& definition() const = 0;

    /// The instance body, in whose scope an expression of constants is evaluated.
    [[nodiscard]] virtual const slang::ast::Symbol& evaluationScope() const = 0;

    /// The run's time unit, as a power of ten of a second, which every delay in the design is
    /// converted to count.
    [[nodiscard]] virtual std::int8_t runTimeUnit() const = 0;

    /// Reports that a delay its scope states is no whole number of the run's unit and is rounded
    /// to it.
    virtual void reportRoundedDelay(slang::SourceLocation location, std::string_view delay,
                                    std::string_view rounded) = 0;

    /// Adds the process that evaluates `value` again for the procedural `assign` or `force` of
    /// `objectIndex` at `location`, and returns its index among the module's processes; nothing,
    /// having reported why, when `value` cannot be translated.
    virtual std::optional<std::uint32_t> addOverrideProcess(std::uint32_t objectIndex,
                                                            const slang::ast::Expression& value,
                                                            bool force,
                                                            slang::SourceLocation location) = 0;

    /// Adds an object holding the value of `expression`, which an event control waits on, and
    /// the continuous assignment that keeps it at that value, and returns the object's index;
    /// nothing, having reported why, when `expression` cannot be translated.
    virtual std::optional<std::uint32_t>
    addEventValue(const slang::ast::Expression& expression) = 0;
};

/// A kind of slang's syntax tree, `ElementSelect`, in the words a reader would use for it,
/// `element select`.
[[nodiscard]] std::string kindName(std::string_view kind);

/// The low bit of each real in a value of `type` as the flat vector it is stored as
/// (DESIGN_GUIDE.md section 4.3): one at 0 for a real, one for each real element or field of an
/// unpacked array or struct, and none for anything else.
[[nodiscard]] std::vector<std::uint32_t> realOffsets(ModuleContext& module,
                                                     const slang::ast::Type& type);

/// Translates one procedural block or continuous assignment into an EIR process.
///
/// Produces EIR-B directly rather than an intermediate tree: slang has already resolved
/// names, types and widths and expanded generate constructs, so what is left is a
/// straightforward lowering of a typed AST to three-address form.
///
/// A construct the translator does not handle is reported and the whole process is
/// abandoned. Emitting a partially translated body would produce a design that simulates
/// and gives the wrong answer, which is the worst possible outcome; refusing to elaborate
/// is the safe failure.
class ProcessTranslator {
public:
    ProcessTranslator(ModuleContext& owner, eir::ProcessKind kind, std::string name,
                      SourceLocation location);

    /// Translates a procedural block body. Returns false having reported why, on failure.
    bool translateBody(const slang::ast::Statement& body);

    /// Translates a continuous assignment's expression.
    /// `delay`, when there is one, is the assignment's delay: inertial, so that a change
    /// arriving before an earlier one has taken effect replaces it (IEEE 1800 section 10.3.3).
    bool translateContinuousAssignment(const slang::ast::Expression& assignment,
                                       const slang::ast::TimingControl* delay = nullptr);

    /// Translates an instance of a built-in gate, `and` to `xnor`, `buf`, `not`, the four
    /// tristate buffers, `pullup` or `pulldown`, as the continuous assignment of its outputs it
    /// is (IEEE 1800 section 28): an input of `z` reads as `x`, as the gate tables have it, and a
    /// tristate buffer whose control is unknown drives `x`, the `L` or `H` of the standard being
    /// no value four states hold. Returns false having reported why.
    bool translateGate(const slang::ast::PrimitiveInstanceSymbol& gate);

    /// Translates the process that delays an output by its module paths: when the value the body
    /// gives the output changes, it is written to the output after the shortest delay of the
    /// paths whose input changed most recently, of those whose condition holds, and no earlier
    /// than now (IEEE 1800 sections 30.4 and 30.6). An edge-sensitive path counts only its edge; an
    /// unknown condition holds no path, as Icarus Verilog has it. Returns false having reported
    /// why.
    bool translateModulePaths(const OutputPaths& paths);

    /// Translates an instance of a user-defined primitive as the continuous assignment of its
    /// output its table is (IEEE 1800 section 29), matched by the runtime against the table
    /// constant eir/Udp.h lays out. A sequential primitive, and only one, is given `held`, which
    /// the module adds for it to keep its inputs as it last took them above its state. Returns
    /// false having reported why.
    bool translatePrimitive(const slang::ast::PrimitiveInstanceSymbol& instance,
                            std::optional<std::uint32_t> held);

    /// Translates `expression` and stores its value into `objectIndex`.
    ///
    /// Used for the implicit continuous assignment a port connection needs when it is an
    /// expression rather than a plain signal reference. The EIR contract says a connection
    /// names an object, so the expression becomes a real assignment in the EIR rather than
    /// something the elaborator has to understand.
    bool translateExpressionInto(std::uint32_t objectIndex,
                                 const slang::ast::Expression& expression);

    /// Reads `objectIndex` and assigns it to whatever `target` designates.
    ///
    /// The mirror image of `translateExpressionInto`, for an **output** port connected to
    /// something that is not a plain signal — a part select, an element select, a
    /// concatenation. The port keeps its own storage and an implicit continuous assignment
    /// copies it outwards, which is what the EIR contract requires: a connection names an
    /// object, so anything more complicated becomes a real assignment rather than something
    /// the elaborator has to understand.
    bool translateObjectInto(const slang::ast::Expression& target, std::uint32_t objectIndex);

    /// Translates the body of an override process: `value`, written by `Override` into
    /// `objectIndex`, which the statement that started the override names.
    bool translateOverride(std::uint32_t objectIndex, const slang::ast::Expression& value);

    /// Fills the sensitivity list from everything the process read, once it has been
    /// translated.
    ///
    /// This is what `always @(*)` means. It cannot be applied up front because the read set
    /// is only known after the body has been walked.
    void inferSensitivityFromReads();

    /// Applies a static sensitivity list taken from the block's event control.
    ///
    /// Called by the module translator when the body turned out to begin with an event
    /// control, because a sensitivity list belongs to the process rather than to a
    /// statement inside it.
    void setSensitivity(std::vector<eir::SensitivityEntry> sensitivity);

    /// The sensitivity entry an event control on `expression` is: the whole of an object, or a
    /// constant window of one, such as `posedge clk[2]` (IEEE 1800 section 9.4.2). Nothing for
    /// any other expression, for which `eventValueEntry` is.
    std::optional<eir::SensitivityEntry> eventEntry(const slang::ast::Expression& expression,
                                                    slang::ast::EdgeKind edge);

    /// The sensitivity entry an event control on any other `expression` is, `@(pipe[head])`: the
    /// whole of an object the module keeps at the expression's value. Nothing when the
    /// expression reads a variable of the process's own or changes something, which the
    /// caller reports, or when it cannot be translated, having reported why.
    std::optional<eir::SensitivityEntry> eventValueEntry(const slang::ast::Expression& expression,
                                                         slang::ast::EdgeKind edge);

    /// Hands over the finished process. The translator must not be used afterwards.
    eir::Process take();

private:
    /// A translated value: the register holding it and the type it has.
    struct Value {
        std::uint32_t reg = 0;
        eir::Type type;
    };

    // --- emission helpers --------------------------------------------------

    std::uint32_t newRegister(eir::Type type);
    std::uint32_t newBlock(std::string label);
    void emit(const eir::Instruction& instruction);

    /// The statement being translated, stamped onto everything `emit` produces.
    SourceLocation currentStatement;
    std::uint32_t addConstant(const eir::BitVector& value);

    /// A register holding one bit of `value`.
    std::uint32_t constantRegister(eir::Logic value);
    std::uint32_t addString(std::string text);

    /// Switches emission to `block`.
    void selectBlock(std::uint32_t block);

    /// True when the current block already ends in a terminator.
    [[nodiscard]] bool currentBlockTerminated() const;

    /// Records that the process reads an object.
    void noteRead(std::uint32_t objectIndex);

    /// Records that the process writes `width` bits of an object from `lowBit` upwards, and
    /// returns false when none of them lie inside it.
    ///
    /// Signed and 64 bits wide so that a select below or beyond the declared range arrives
    /// as written: such a write is discarded by the language, so only the bits inside the
    /// object are recorded, and a write with none inside should not be emitted at all.
    bool noteWrite(std::uint32_t objectIndex, std::int64_t lowBit, std::int64_t width);

    /// Records that the process writes every bit of an object.
    void noteWholeWrite(std::uint32_t objectIndex);

    /// The bits a select whose position is not constant could write, as a low bit and a
    /// width.
    ///
    /// A driver's range is fixed at elaboration, so a variable select drives every bit it
    /// could reach. What it could reach is bounded by the index's type: a two-bit index into
    /// a byte reaches four positions, not eight. An index of 32 bits or more reaches
    /// anything, since no object is wider than a 32-bit count of bits.
    ///
    /// `downward` is the `-:` form, whose low bit is `width - 1` below its base.
    [[nodiscard]] static std::pair<std::int64_t, std::int64_t>
    reachOfSelect(const slang::ast::Expression& index, std::int32_t rangeLow,
                  std::uint32_t elementBits, std::uint32_t width, bool downward,
                  std::uint32_t objectWidth);

    /// Returns `value` widened or narrowed to `type`, emitting an extension if needed.
    Value coerce(const Value& value, eir::Type type);

    /// Returns `value` with unknown bits forced to zero when `type` is two-state.
    Value coerceState(const Value& value, eir::Type type);

    /// A register holding the real `value`.
    Value realConstant(double value);

    /// `value` as the truth a condition tests: a real is true where it is not 0.0, which its bits
    /// alone do not say of -0.0; any other value is its own truth.
    Value truthOf(const Value& value);

    /// The value of an expression whose truth decides a branch, as `truthOf` makes it.
    std::optional<Value> translateCondition(const slang::ast::Expression& condition);

    // --- statements --------------------------------------------------------

    bool translateStatement(const slang::ast::Statement& statement);

    /// Translates an expression used as a statement.
    ///
    /// A void system task call is handled here rather than in expression translation,
    /// because it produces no value and an expression translator that had to return
    /// "nothing, successfully" would make every caller check two kinds of absence.
    bool translateExpressionStatement(const slang::ast::Expression& expression);
    bool translateBlock(const slang::ast::Statement& statement);
    bool translateConditional(const slang::ast::Statement& statement);
    bool translateCase(const slang::ast::Statement& statement);
    bool translateForLoop(const slang::ast::Statement& statement);
    bool translateWhileLoop(const slang::ast::Statement& statement, bool testFirst);
    bool translateRepeatLoop(const slang::ast::Statement& statement);
    bool translateForeverLoop(const slang::ast::Statement& statement);

    /// `foreach` over fixed dimensions: a counting loop per dimension it names, from the
    /// dimension's left bound to its right, with the iterators in registers.
    bool translateForeachLoop(const slang::ast::Statement& statement);

    /// `break` and `continue`, which jump to the innermost loop's exit and to where its next
    /// iteration starts.
    bool translateLoopJump(const slang::ast::Statement& statement, bool isBreak);

    /// `disable` of a named block the statement is in, which jumps to just after the block.
    bool translateDisable(const slang::ast::Statement& statement);
    bool translateTimed(const slang::ast::Statement& statement);
    bool translateReturn(const slang::ast::Statement& statement);
    bool translateImmediateAssertion(const slang::ast::Statement& statement);

    /// Procedural `assign` and `force`, `deassign` and `release` (IEEE 1800 section 10.6), of a
    /// whole object: what a part of one or a concatenation would need is refused.
    bool translateProceduralAssign(const slang::ast::Statement& statement);
    bool translateProceduralDeassign(const slang::ast::Statement& statement);

    /// `-> e` and `->> e` (IEEE 1800 section 15.5.1): the event's bit inverted, now or in the NBA
    /// region, which wakes what waits on it. One with a delay is refused.
    bool translateEventTrigger(const slang::ast::Statement& statement);

    /// The object a procedural `assign`, `force`, `deassign` or `release` names as its target,
    /// recorded in the override set; nothing, having reported why, for anything but the whole
    /// of one.
    std::optional<std::uint32_t> overrideTarget(const slang::ast::Expression& target,
                                                std::string_view statement);

    /// `wait (condition) statement`: tests the condition and, while it is not true, waits for
    /// a change to anything it reads and tests it again (IEEE 1800 section 9.4.3).
    bool translateWait(const slang::ast::Statement& statement);

    /// Emits the suspension for a timing control. Returns false on an unsupported one.
    bool translateTimingControl(const slang::ast::TimingControl& timing);

    /// A delay as a count of the run's unit: `amount` counts the time unit of the scope it is
    /// in, and is rounded to that scope's precision (IEEE 1800 section 3.14.2.1) and then to the
    /// run's unit, which is reported where the design states the delay. A delay that is unknown
    /// is none (section 9.4.1).
    std::optional<Value> translateDelay(const slang::ast::Expression& amount);

    /// A delay that is a constant, as `translateDelay` counts it: `constant` says whether it is
    /// one, and nothing comes back for one that cannot be counted, having reported why.
    std::optional<std::uint64_t> constantDelay(const slang::ast::Expression& amount,
                                               bool& constant);

    /// The time scale of the scope being translated: the subroutine being inlined, whose own
    /// module, package or compilation unit gives its delays their unit, or else the body.
    [[nodiscard]] slang::TimeScale timeScaleHere() const;

    /// Adds `call` to the process's system task calls, in the time unit of the scope it is in,
    /// and returns its index.
    std::uint32_t addSysCall(eir::SysTaskCall call);

    // --- expressions -------------------------------------------------------

    std::optional<Value> translateExpression(const slang::ast::Expression& expression);
    std::optional<Value> translateBinary(const slang::ast::Expression& expression);
    std::optional<Value> translateUnary(const slang::ast::Expression& expression);
    std::optional<Value> translateConditionalExpression(const slang::ast::Expression& expression);
    std::optional<Value> translateConcatenation(const slang::ast::Expression& expression);
    std::optional<Value> translateReplication(const slang::ast::Expression& expression);

    /// A streaming concatenation's stream (IEEE 1800 section 11.4.14): its operands packed, the
    /// first most significant, and for `<<` its slices reversed.
    std::optional<Value> translateStreaming(const slang::ast::Expression& expression);

    /// `stream` with its slices of `sliceSize` bits in the reverse order. Packing counts them from
    /// the least significant end, so that `{<< 4 {6'b11_0101}}` is `6'b0101_11`; unpacking a
    /// target, which undoes that, from the most significant end, with `fromTop`.
    Value reverseSlices(const Value& stream, std::uint64_t sliceSize, bool fromTop);

    /// `stream` as a value of `type`, at least as wide: left-justified, as a stream assigned to a
    /// wider target is, and zero-filled on the right.
    Value leftJustified(const Value& stream, eir::Type type);

    /// The bits of `value` from `offset` up, as many as `type` holds, as a value of it.
    Value bitsOf(const Value& value, std::uint32_t offset, eir::Type type);
    std::optional<Value> translateCall(const slang::ast::Expression& expression);

    /// `$readmemh` and `$readmemb`, which write a whole memory from a file.
    bool translateReadMem(std::string_view name, const slang::ast::CallExpression& call);

    /// `$fopen`, whose descriptor the runtime chooses.
    std::optional<Value> translateFileOpen(const slang::ast::CallExpression& call,
                                           const slang::ast::Expression& expression);

    /// `$fgetc`, a byte of a file the runtime reads.
    std::optional<Value> translateFileGetCharacter(const slang::ast::CallExpression& call,
                                                   const slang::ast::Expression& expression);

    /// `$random`, `$urandom` and `$urandom_range`, which the runtime draws.
    std::optional<Value> translateRandom(std::string_view name,
                                         const slang::ast::CallExpression& call,
                                         const slang::ast::Expression& expression);

    /// `&&` or `||` whose right operand has side effects, which it evaluates only when the
    /// left one does not decide the result (IEEE 1800 section 11.4.7): not when the left is
    /// known 0 for `&&` or known 1 for `||`. An unknown left decides nothing.
    std::optional<Value> translateShortCircuit(const slang::ast::Expression& expression,
                                               const Value& left);

    /// One block for each of the condition known true, known false, and unknown, for the
    /// operators that evaluate their operands only as far as the condition requires. Ends in the
    /// current block, which the caller fills; returns the true, false and unknown blocks and
    /// the join, in that order.
    std::array<std::uint32_t, 4> branchOnTruth(const Value& condition);

    /// Emits a store of `reg` to the whole of `objectIndex`, in the form `nonBlocking` and
    /// `storeDelay` call for. False, having reported, when `storeDelayFor` refuses the delay.
    bool emitWholeStore(std::uint32_t objectIndex, std::uint32_t reg, bool nonBlocking);

    /// Reports a delayed write to something a delayed store cannot address, and returns true,
    /// when an assignment delay is in effect; returns false otherwise.
    bool refuseDelayedWrite(const slang::ast::Expression& target);

    /// The value of an integral `expression` that computes with reals when every operand is a
    /// constant, folded as slang folds it: `ClkPeriod >= 2ps` with `ClkPeriod` a parameter.
    /// Nothing when some operand is not a constant, and the reals are then computed as the design
    /// runs.
    std::optional<Value> foldThroughReals(const slang::ast::Expression& expression,
                                          const eir::Type& type);
    std::optional<Value> translateInside(const slang::ast::Expression& expression);
    std::optional<Value> translateAssignmentPattern(const slang::ast::Expression& expression);

    /// Inlines a call of a user-defined function or task into this process, and returns the
    /// function's value, or a placeholder for a task or a void function.
    ///
    /// Inlined rather than called: the body becomes part of the process that calls it, so a
    /// task's delays suspend that process as any of its own statements would, its read and
    /// write sets are exactly what the call touches, and nothing at run time keeps a call
    /// stack. A recursive call is refused, as is a `ref` argument.
    std::optional<Value> inlineCall(const slang::ast::CallExpression& call,
                                    const slang::ast::Expression& expression);

    /// Binds every variable of `scope` and the scopes inside it — arguments, the return
    /// variable, locals — to a register, which is what an automatic variable is.
    void bindLocals(const slang::ast::Scope& scope);

    /// The register bound to `symbol` while a subroutine that declares it is being inlined.
    [[nodiscard]] const Value* localFor(const slang::ast::ValueSymbol& symbol) const;

    /// A copy of a register, so that a later assignment to a local does not reach back into a
    /// value already read from it.
    Value copyOf(const Value& value);

    /// A register holding a type's default: X when four-state, zero when two-state.
    Value defaultValue(eir::Type type);

    /// `$isunknown`, `$countones`, `$onehot` and `$onehot0` (IEEE 1800 section 20.9).
    std::optional<Value> translateBitCount(std::string_view name,
                                           const slang::ast::CallExpression& call,
                                           const slang::ast::Expression& expression);

    /// Emits a comparison of two values, each extended to the wider of the two and compared
    /// as signed only when both are, and returns its one-bit result.
    Value compare(eir::Opcode opcode, const Value& left, const Value& right);

    /// Emits `==`, `!=`, `===` or `!==` of two unpacked arrays or structs with reals at
    /// `reals` (`realOffsets`), element by element as the language compares them (IEEE 1800
    /// section 11.4.5): each real as a real, so that 0.0 equals -0.0 and a NaN nothing, and the
    /// rest of their bits as bits. Comparing the flat vectors would get both of those wrong.
    Value compareAggregates(eir::Opcode opcode, const Value& left, const Value& right,
                            const std::vector<std::uint32_t>& reals);

    /// Emits whether `value` matches one item of an `inside` set or a `case ... inside`: by
    /// `==?` for a value, so that an X or Z bit of the item matches anything, and by bounds for
    /// a range `[low:high]` (IEEE 1800 section 11.4.13).
    std::optional<Value> insideItem(const Value& value, const slang::ast::Expression& item);
    std::optional<Value> translateAssignment(const slang::ast::Expression& expression);

    /// Writes `value` into the location `target` designates.
    bool assignTo(const slang::ast::Expression& target, const Value& value, bool nonBlocking);

    /// Emits a system task call, or returns false if the task is not supported.
    bool translateSystemTask(const slang::ast::Expression& expressionNode);

    /// Evaluates a constant expression, or nothing when it is not constant.
    std::optional<std::int64_t> constantValueOf(const slang::ast::Expression& expression);

    /// Widens an index to a width that can hold a bit offset into any object.
    ///
    /// An index is only as wide as the thing it indexes, and a bit offset is that index
    /// scaled by the element width. Doing the arithmetic in the index's own width makes a
    /// 2-bit pointer into a four-element array of bytes wrap to zero, so every element
    /// aliases the first.
    Value widenForOffsetArithmetic(const Value& index);

    /// Returns how many bits one element of `type` occupies.
    ///
    /// For a plain vector that is one bit, and an element select is a bit select. For a
    /// packed array — `logic [3:0][7:0] mem` — it is the element's width, and the select
    /// addresses a window rather than a bit. Treating every element select as a bit select
    /// would silently read one bit of a byte.
    static std::uint32_t elementBitsOf(const slang::ast::Type& type);

    /// Where a value lives: a window of an object's storage, or of a register holding a value
    /// that is not an object (see `locate`).
    struct Location {
        /// The object the window is in, or `NoObject` when it is in `root`.
        std::uint32_t objectIndex = NoObject;

        /// The value the window is in, when it is not in an object.
        Value root;

        /// The type of the window itself.
        eir::Type type;

        /// The window's low bit, when every select on the way to it was constant.
        std::optional<std::int64_t> constantLow;

        /// The window's low bit as a register, when some select was not constant.
        std::optional<Value> low;

        /// True when the window is the whole of the object or value.
        bool whole = true;

        /// True when a constant select fell outside its dimension: a read gives the type's
        /// default and a write does nothing.
        bool outOfRange = false;

        /// The bits a write here could reach, for the write set when the window moves.
        std::int64_t reachLow = 0;
        std::int64_t reachWidth = 0;

        /// True when `root` is the register of an inlined subroutine's local, which a write
        /// may change in place.
        bool rootIsLocal = false;

        static constexpr std::uint32_t NoObject = 0xFFFF'FFFFU;

        [[nodiscard]] bool inObject() const noexcept { return objectIndex != NoObject; }
    };

    /// Places `expression` as a window, composing every select and member access on the way to
    /// an object, or to a value that is not one. Returns nothing having reported why.
    std::optional<Location> locate(const slang::ast::Expression& expression);
    std::optional<Location> locateElement(const slang::ast::ElementSelectExpression& select);
    std::optional<Location> locateRange(const slang::ast::RangeSelectExpression& select);
    std::optional<Location> locateMember(const slang::ast::MemberAccessExpression& access);

    /// `base` narrowed to a window of `type` at a further offset, constant or in a register.
    Location narrowed(const Location& base, std::optional<std::int64_t> constantLow,
                      std::optional<Value> low, eir::Type type);

    /// Reads a window: in place for an object, by a part select for a value.
    std::optional<Value> readLocation(const Location& location);

    /// Writes `value` into a window of an object, with a store or a part store, and records
    /// the bits it drives. `target` is where a diagnostic points.
    bool writeLocation(const slang::ast::Expression& target, const Location& location,
                       const Value& value, bool nonBlocking);

    /// The window's low bit as a register, loading it when it is constant.
    Value lowOf(const Location& location);

    /// A 64-bit signed constant, for offset arithmetic.
    Value offsetConstant(std::int64_t offset);

    /// `left opcode right` on offsets, both extended to the wider of the two.
    Value offsetArithmetic(eir::Opcode opcode, const Value& left, const Value& right);

    /// The bit offset of element `index` of a dimension declared `range`, `stride` bits apart.
    Value positionValue(const Value& index, slang::ConstantRange range, std::uint32_t stride);

    /// `offset` where `index` lies within `range`, and X where it does not.
    Value checkedOffset(const Value& offset, const Value& index, slang::ConstantRange range);

    /// A subroutine being inlined: where a `return` in its body goes.
    struct InlineFrame {
        const slang::ast::SubroutineSymbol* subroutine = nullptr;
        std::uint32_t exitBlock = 0;
        const slang::ast::VariableSymbol* returnVariable = nullptr;
    };

    /// The subroutines being inlined, innermost last, for `return` and to refuse recursion.
    std::vector<InlineFrame> inlineFrames;

    /// The registers the locals of the subroutines being inlined are bound to.
    ///
    /// Keyed on slang's symbols, which is sound here and only here: the map lives for one
    /// translation of one body and is never shared between instances.
    std::map<const slang::ast::ValueSymbol*, Value> locals;

    ModuleContext* module;

    /// The current value of a compound assignment's target, while its right-hand side is
    /// being translated.
    ///
    /// slang rewrites `a += b` as `a = <lvalue> + b` so that the target is evaluated once,
    /// and `<lvalue>` is an LValueReference expression standing for that single evaluation.
    /// Saved and restored around the translation, so that nested compound assignments do not
    /// see each other's target.
    std::optional<Value> lvalueReference;

    /// The register holding the delay of the assignment being translated, while one is: a
    /// nonblocking `a <= #d b`, or a continuous `assign #d a = b`. Its stores are the delayed
    /// forms, and a store the delayed forms cannot express is refused rather than undelayed.
    std::optional<std::uint32_t> storeDelay;

    /// A continuous assignment's or a gate's rise, fall and turn-off delays, as counts of the
    /// run's unit, when they are not all one: each store then takes the one the value it stores
    /// calls for, and `storeDelay` only says that there is a delay.
    std::optional<std::array<std::uint64_t, 3>> transitionDelays;

    /// Sets `storeDelay`, and `transitionDelays` where they differ, from a continuous
    /// assignment's or a gate's `delay`. False having reported.
    bool setContinuousDelay(const slang::ast::TimingControl& delay);

    /// The register holding the delay of a store of `stored`, a value of `width` bits: the one
    /// delay, or of separate ones what the value calls for (IEEE 1800 section 28.16), the rise to
    /// 1, the fall to 0, the turn-off to z and the smallest to x. Nothing, having reported, for
    /// separate delays of a vector, whose bits may each call for another.
    std::optional<std::uint32_t> storeDelayFor(std::uint32_t stored, std::uint32_t width,
                                               const slang::ast::Expression* target);

    /// Where `continue` and `break` go in each enclosing loop, innermost last.
    struct LoopTargets {
        std::uint32_t continueBlock = 0;
        std::uint32_t breakBlock = 0;
    };

    std::vector<LoopTargets> loopTargets;

    /// A named block being translated, innermost last: where a `disable` of it goes, made when
    /// the first asks for it, and how many subroutines were being inlined when it began.
    struct NamedBlock {
        const slang::ast::StatementBlockSymbol* symbol = nullptr;
        std::optional<std::uint32_t> exitBlock;
        std::size_t inlineDepth = 0;
    };

    std::vector<NamedBlock> namedBlocks;
    eir::Process process;
    std::uint32_t current = 0;

    std::vector<std::uint32_t> reads;
    std::vector<eir::WriteExtent> writes;
    std::vector<std::uint32_t> overrides;

    bool failed = false;
};

} // namespace enzan::frontend::sv
