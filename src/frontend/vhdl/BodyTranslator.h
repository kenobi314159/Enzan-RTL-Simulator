// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "frontend/vhdl/Folder.h"
#include "frontend/vhdl/NvcApi.h"
#include "frontend/vhdl/Types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::frontend::vhdl {

/// The shape a call gives the subprogram it calls, one entry per formal parameter in order: the
/// bounds of an array actual, and the value of a scalar one when it is static.
///
/// A subprogram is translated once per shape it is called with (DESIGN_GUIDE.md section 4.1), so
/// that inside it the bounds of its unconstrained parameters are static, as everything an EIR
/// body holds has to be: `l'length` folds, and a guard for a null array that cannot be one folds
/// away with it. A scalar's value is part of the shape only when the subprogram's bounds depend on
/// it, as `to_unsigned(arg, size)`'s result does on `size`; which those are is the callee's to say.
struct Shape {
    std::vector<std::optional<Folder::Range>> bounds;
    std::vector<std::optional<std::int64_t>> values;
};

/// A subprogram specialised for one shape: where a call finds it, and what its parameters and its
/// result are.
struct Callee {
    std::string definition;
    std::string name;
    std::vector<Layout> parameters;
    std::vector<eir::ParameterMode> modes;

    /// A function's result; a procedure has none.
    std::optional<Layout> result;
};

/// What a body translator needs from the definition it is translated into: an architecture's, or
/// a package's.
///
/// An interface, as the SystemVerilog frontend's `ModuleContext` is, so that statement and
/// expression translation stays apart from the hierarchy walk: the definition's side owns the
/// objects, the generics and the subprograms, and the body asks for them.
class DefinitionScope {
public:
    DefinitionScope() = default;
    DefinitionScope(const DefinitionScope&) = delete;
    DefinitionScope& operator=(const DefinitionScope&) = delete;
    DefinitionScope(DefinitionScope&&) = delete;
    DefinitionScope& operator=(DefinitionScope&&) = delete;
    virtual ~DefinitionScope() = default;

    /// The EIR object holding `declaration`, a signal or a port of this architecture's entity, or
    /// nothing when it has none.
    [[nodiscard]] virtual std::optional<std::uint32_t> objectFor(tree_t declaration) const = 0;

    /// Static evaluation, with this instance's generics bound.
    [[nodiscard]] virtual const Folder& folder() const = 0;

    [[nodiscard]] virtual SourceLocation locate(tree_t where) const = 0;

    /// Reports a construct the adapter does not translate yet.
    virtual void unsupported(tree_t where, std::string_view what) = 0;

    /// Reports a process that contains no `wait`, and so would never give the simulation back.
    virtual void reportNeverWaits(tree_t process) = 0;

    /// The definition being built, for the types of its objects.
    [[nodiscard]] virtual const eir::ModuleDefinition& definition() const = 0;

    /// The unit EIR's delays count, as a power of ten of a second.
    [[nodiscard]] virtual std::int8_t timeUnitExponent() const = 0;

    /// The subprogram `declaration` specialised for `shape`, translated into the definition it
    /// belongs to the first time it is asked for: a package's subprogram into the package's, and
    /// one of an architecture into this one. Nothing, having reported why, when it cannot be.
    [[nodiscard]] virtual std::optional<Callee> callee(tree_t declaration, const Shape& shape,
                                                       tree_t where) = 0;
};

/// What specialising a subprogram for a shape gives it: the bindings that make its bounds
/// static, and each parameter's layout.
struct SubprogramBindings {
    Folder folder;
    std::vector<Layout> parameters;
};

/// The parameters of `body`, a subprogram, whose values its bounds depend on: those named in the
/// constraints of what it declares, or in the values of its constants, which such a constraint
/// may use.
[[nodiscard]] std::vector<bool> shapeParameters(tree_t body);

/// Translates one VHDL process into an EIR process, or one subprogram, specialised for a shape,
/// into an EIR subprogram.
///
/// Produces EIR-B directly from NVC's tree, as the SystemVerilog frontend does from slang's: names
/// and types are resolved and every concurrent statement is already a process (NVC rewrites a
/// concurrent assignment into the process IEEE 1076 section 11.6 says it is equivalent to), so what
/// is left is lowering statements to blocks and expressions to registers.
///
/// The process is a loop, as VHDL's is: its declarations are initialised once, on entry, and its
/// statements then run round for ever, giving the simulation back only at a `wait` (section 11.3).
/// A process with a sensitivity list arrives with the equivalent `wait on` at its end.
///
/// Its variables are registers. A process's frame lasts as long as the process, so a register
/// keeps its value from one resumption to the next as a variable has to.
///
/// A construct it cannot translate is reported and the process abandoned, never emitted in part:
/// a process missing a statement simulates and gives a wrong answer (DESIGN_GUIDE.md section 1.2).
class BodyTranslator {
public:
    /// A translator of the process `statement`.
    BodyTranslator(DefinitionScope& scope, tree_t statement, std::string name);

    /// A translator of the subprogram `body`, specialised as `specialisation` says.
    BodyTranslator(DefinitionScope& scope, tree_t body, std::string name,
                   SubprogramBindings specialisation);

    /// The translated process, or nothing having reported why.
    std::optional<eir::Process> translate();

    /// A process that keeps one end of a port association equal to the other, for a port
    /// associated in parts or with something other than a whole signal: `target <= value`, then a
    /// wait on whatever that read, over and over. A null end is the whole of the port, which is
    /// `portObject` here, laid out as `portLayout`; a formal naming part of the port names part of
    /// that object.
    ///
    /// When `seed` holds a value, the port object's initial one, the bits the association gives the
    /// target at the start are spliced into it where they can be known statically: the actual's
    /// initial value or its static value.
    std::optional<eir::Process> translateAssociation(tree_t target, tree_t value,
                                                     std::uint32_t portObject,
                                                     const Layout& portLayout,
                                                     std::optional<eir::BitVector>* seed);

    /// The translated subprogram and its result's layout, or nothing having reported why.
    struct Translated {
        eir::Subprogram subprogram;
        std::optional<Layout> result;
    };

    std::optional<Translated> translateSubprogram();

private:
    /// A translated value: the register holding it, and its type in EIR and in VHDL.
    struct Value {
        std::uint32_t reg = 0;
        eir::Type type;
        type_t vhdlType = nullptr;

        /// The bounds and the elements of an array value, which a scalar's leaves empty.
        Layout layout;
    };

    /// What a name designates: a signal, a port or a variable, or a part of one, an element or a
    /// slice, through indices that may be known only at run time.
    struct Place {
        /// A variable's register, or else a signal or a port.
        bool isVariable = false;
        Value variable;
        std::uint32_t objectIndex = 0;

        /// How the part designated is laid out, and where its bits start in the whole: from a
        /// static offset, or from the 64-bit register `offset` holds.
        Layout layout;
        std::int64_t staticOffset = 0;
        std::optional<Value> offset;
        bool whole = true;

        /// Where `offset` is known only at run time, the bits the part lies within whatever it is:
        /// the static part around it, a field say, from `withinLow`, `withinWidth` wide. A width
        /// of zero is the whole object.
        std::int64_t withinLow = 0;
        std::uint32_t withinWidth = 0;

        /// True for a null slice, which designates nothing: assigning it does nothing, as VHDL has
        /// it, and the adapter has no value to read from it.
        bool empty = false;
    };

    /// A subprogram translated into this body rather than called: where its `return` goes, and the
    /// register it leaves a function's result in, made at its first `return`.
    struct Inlined {
        tree_t body = nullptr;
        std::uint32_t exitBlock = 0;
        std::optional<Value> result;
    };

    /// A loop that `exit` and `next` can name.
    struct Loop {
        ident_t label = nullptr;
        std::uint32_t exitBlock = 0;
        std::uint32_t nextBlock = 0;
    };

    // --- emission -----------------------------------------------------------

    std::uint32_t newRegister(eir::Type type);
    std::uint32_t newBlock(std::string label);
    void selectBlock(std::uint32_t block);
    [[nodiscard]] bool terminated() const;
    void emit(const eir::Instruction& instruction);
    void jump(std::uint32_t block);
    std::uint32_t addConstant(const eir::BitVector& value);
    std::uint32_t addString(std::string text);
    std::uint32_t addSysCall(eir::SysTaskCall call);

    /// A register holding `value` as `type` holds it.
    Value constant(std::int64_t value, eir::Type type, type_t vhdlType);

    /// `value` sign- or zero-extended, or truncated, to `width` bits.
    Value resize(const Value& value, std::uint32_t width);

    /// A register holding `bits`, an array value laid out as `layout`.
    Value constantArray(const eir::BitVector& bits, const Layout& layout, type_t vhdlType);

    /// How a value of `type` is laid out, with this instance's generics bound.
    [[nodiscard]] std::optional<Layout> layoutFor(type_t type) const;

    /// The body being built: the process's, or the subprogram's.
    eir::Body& body() { return isProcess ? static_cast<eir::Body&>(process) : subprogram; }

    [[nodiscard]] const eir::Body& body() const {
        return isProcess ? static_cast<const eir::Body&>(process) : subprogram;
    }

    /// Static evaluation where translation is: with the generics, and the parameters of every
    /// subprogram being inlined, bound.
    [[nodiscard]] const Folder& folder() const { return folders.back(); }

    void noteRead(std::uint32_t objectIndex);

    /// Records that the process writes `place`: the bits it designates when they are known, and
    /// the whole object when an index is not (DESIGN_GUIDE.md section 3.7).
    void noteWrite(const Place& place);

    bool fail(tree_t where, std::string_view what);

    // --- declarations and statements ----------------------------------------

    bool declare(tree_t declaration);
    bool statements(tree_t container);
    bool statement(tree_t statement);
    bool signalAssignment(tree_t statement);
    bool variableAssignment(tree_t statement);
    bool ifStatement(tree_t statement);
    bool caseStatement(tree_t statement);
    bool whileLoop(tree_t statement);
    bool forLoop(tree_t statement);
    bool loopJump(tree_t statement, bool isExit);
    bool waitStatement(tree_t statement);

    /// Where a slice whose bounds are known only at run time lies in an array laid out as
    /// `layout`: the offset of its lowest bit, and how many bits it holds, which is not positive
    /// for a null slice. Both are 64-bit signed registers.
    struct MovingSlice {
        Value lowBit;
        Value bits;
    };

    std::optional<MovingSlice> movingSlice(tree_t range, const Layout& layout);

    /// A variable assignment to a slice whose bounds are known only at run time, as numeric_std's
    /// shifts by a count have, `r(l downto n) := a(l - n downto 0)`: its value shifted into place
    /// and merged under a mask of the slice's bits.
    bool movingSliceAssignment(tree_t statement);

    // --- std.textio and files, run natively (Textio.cpp) -----------------------------------------

    /// std.textio's procedures and the predefined file operations, which the runtime runs over
    /// the heap of DESIGN_GUIDE.md section 4.3 rather than the adapter translating textio's body.
    /// Nothing when `declaration` is not one of them, so that the call is translated as any other.
    std::optional<bool> nativeProcedure(tree_t call, tree_t declaration);

    /// `endfile(f)`.
    std::optional<Value> endOfFile(tree_t call);

    /// A file the process declares: a register holding its descriptor, opened where it is
    /// declared when the declaration says so.
    bool declareFile(tree_t declaration);

    /// The descriptor of the file `name` designates: one the process declares, or std.textio's
    /// `output` or `input`.
    std::optional<Value> fileDescriptor(tree_t name);

    /// Opens the file `name` holds in `mode`, a position of `file_open_kind`, and returns its
    /// descriptor.
    Value openFile(const Value& name, std::int64_t mode, tree_t where);

    /// Emits a system function and returns its result, of `type`.
    Value systemFunction(eir::SysTaskCall call, eir::Type type);

    /// Writes `value` into the whole variable `name` designates, an inout parameter's actual.
    bool assignWhole(tree_t name, const Value& value);

    /// True when a slice in `loop` has bounds naming `parameter`, the loop's.
    static bool slicesMoveWith(tree_t loop, tree_t parameter);

    /// `statement`, a for loop over `range`, as one copy of its body for each value, with the
    /// parameter bound to it, so that what depends on it folds.
    bool unrolledLoop(tree_t statement, tree_t parameter, const Folder::Range& range);

    /// How many iterations a loop may have to be unrolled.
    static constexpr std::int64_t MaxUnrolledIterations = 1024;
    bool assertion(tree_t statement);
    bool report(tree_t statement);
    bool procedureCall(tree_t statement);
    bool returnStatement(tree_t statement);

    /// The parameters and the actuals of a call of `declaration`, in the order of its formals, a
    /// default value standing in for an actual the call leaves out. Nothing, having reported
    /// why, when the call associates one in a way the adapter does not take apart.
    std::optional<std::vector<tree_t>> actualsOf(tree_t call, tree_t declaration);

    /// A call of a function or a procedure: callable, in a body of its own, or inlined into this
    /// one when it must be, which is when it waits or assigns a signal or takes one as a
    /// parameter. Returns a function's result; a procedure's is a placeholder.
    std::optional<Value> callSubprogram(tree_t call, tree_t declaration);
    std::optional<Value> inlineCall(tree_t call, tree_t declaration, tree_t calleeBody,
                                    const std::vector<tree_t>& actuals);

    /// Emits a `report` system task of `severity`, a position of `severity_level`, with the
    /// message `message` translates to, or `fallback` when there is none.
    bool emitReport(tree_t where, const Value& severity, tree_t message, std::string_view fallback);

    /// Appends the text of `message`, a string expression, to `format`, with what it computes as
    /// arguments: a literal is text, and a value's `'image` a conversion. Messages are built that
    /// way almost always, and it needs no string values at run time.
    bool messageParts(tree_t message, std::string& format,
                      std::vector<eir::SysTaskArgument>& arguments);

    /// The image of `operand`, a value of an enumeration known only at run time, as a part of a
    /// message.
    bool enumerationImage(tree_t operand, std::string& format,
                          std::vector<eir::SysTaskArgument>& arguments);

    /// What `name` designates: a signal, a port or a variable of the process, or an element or a
    /// slice of one. Nothing having reported why for anything else.
    std::optional<Place> place(tree_t name);

    /// True when `name` is a signal, a port or a variable of the process, or a part of one, so that
    /// `place` can find it.
    [[nodiscard]] bool designatesStorage(tree_t name) const;

    /// The value at `place`.
    Value read(const Place& place, type_t vhdlType);

    /// A register holding the offset of `place`'s part, for an instruction that takes one.
    std::uint32_t offsetRegister(const Place& place);

    /// The offset of the element `index` designates in an array laid out as `layout`: a static
    /// one, or a register holding it once checked to lie inside the bounds. Nothing having
    /// reported why when a static index lies outside them.
    struct Offset {
        std::int64_t staticOffset = 0;
        std::optional<Value> dynamic;
    };

    std::optional<Offset> elementOffset(const Layout& layout, tree_t index);

    /// The element of `array` that the indices of `name`, a T_ARRAY_REF, designate.
    std::optional<Value> element(const Value& array, tree_t name, type_t vhdlType);

    /// The offset of the element the indices of `name` designate, one per dimension, in an array
    /// laid out as `layout`, and that element's layout.
    std::optional<std::pair<Offset, Layout>> indexed(const Layout& layout, tree_t name);

    /// The sum of two offsets, in a register when either is.
    Offset sum(const Offset& first, const Offset& second);

    /// The slice of `array` that `range` designates.
    std::optional<Value> slice(const Value& array, tree_t range, type_t vhdlType);

    /// The layout of a slice by `range` of an array laid out as `layout`, and the offset of its
    /// bits, or nothing having reported why.
    std::optional<std::pair<Layout, std::int64_t>> sliceOf(const Layout& layout, tree_t range);

    /// A register holding `delay`, a value of `time` in femtoseconds, as a count of EIR's unit.
    std::optional<Value> delay(tree_t delay);

    /// The test of one choice of a `case` against `selector`.
    std::optional<Value> choiceTest(const Value& selector, tree_t choice);

    // --- expressions ---------------------------------------------------------

    /// The value of `expression`. `context` is the layout of where the value goes, which an
    /// aggregate with `others` takes its bounds from, as VHDL's does from its context.
    std::optional<Value> expression(tree_t expression, const Layout* context = nullptr);
    std::optional<Value> reference(tree_t expression);

    /// An aggregate of values not all static, built element by element into a register.
    std::optional<Value> aggregate(tree_t expression, const Layout& layout);

    /// An aggregate of positional elements and whole arrays, which is what NVC makes of a chain of
    /// concatenations: its length is its pieces', and its bounds `layout`'s when that is given.
    std::optional<Value> positionalAggregate(tree_t expression, const Layout* layout);

    /// The layout of an array of `type` with `length` elements and no bounds of its own: those
    /// of the index subtype's left bound and direction, as VHDL-2008 gives a concatenation (IEEE
    /// 1076-2008 section 9.2.5).
    std::optional<Layout> unboundedLayout(type_t type, std::int64_t length, tree_t where);

    /// A concatenation of arrays and elements, the left operand in the highest bits.
    std::optional<Value> concatenation(tree_t expression);

    /// The field of `record` that `name`, a T_RECORD_REF, selects.
    std::optional<Value> field(const Value& record, tree_t name, type_t vhdlType);

    /// A record aggregate of values not all static, built field by field into a register.
    std::optional<Value> recordAggregate(tree_t expression, const Layout& layout);

    /// The predefined operators on arrays: equality, ordering, and the logical operators on
    /// arrays of `bit` and `boolean`.
    std::optional<Value> arrayOperator(tree_t expression, subprogram_kind_t kind);
    std::optional<Value> call(tree_t expression);
    std::optional<Value> conversion(tree_t expression);

    /// An attribute of a value computed at run time; one of a type, or of a value known
    /// statically, the folder has already reduced.
    std::optional<Value> attribute(tree_t expression);

    /// One of the predefined operators on scalars.
    std::optional<Value> predefined(tree_t expression, subprogram_kind_t kind);

    /// A predicate's one-bit result as the `boolean` it is in VHDL.
    Value booleanOf(std::uint32_t bit, type_t booleanType);

    /// `and`, `or`, `nand` and `nor` on `bit` and `boolean`, which evaluate their right operand
    /// only when the left one does not decide the result (IEEE 1076 section 9.2.2).
    std::optional<Value> shortCircuit(tree_t expression, subprogram_kind_t kind);

    /// The comparison `opcode` of two values of one type, as a one-bit result.
    std::uint32_t compare(eir::Opcode opcode, const Value& left, const Value& right);

    /// VHDL's `mod`, whose result takes the sign of its right operand.
    Value modulo(const Value& dividend, const Value& divisor);

    /// The scale between VHDL's femtoseconds and EIR's unit: how many of the first make one of the
    /// second.
    [[nodiscard]] std::int64_t femtosecondsPerUnit() const;

    DefinitionScope& owner;
    tree_t source;

    /// What is being built: a process, or a subprogram with the layouts its parameters and its
    /// result have.
    bool isProcess = true;
    eir::Process process;
    eir::Subprogram subprogram;
    std::vector<Layout> parameterLayouts;
    std::optional<Layout> resultLayout;

    std::vector<Folder> folders;
    std::vector<Inlined> inlined;

    std::uint32_t current = 0;
    SourceLocation statementLocation;

    std::set<std::uint32_t> reads;
    std::vector<eir::WriteExtent> writes;

    /// The register of each variable, constant and loop parameter declared in the process, and of
    /// each parameter of a subprogram.
    std::map<tree_t, Value> locals;

    /// What each alias and signal parameter of an inlined subprogram designates.
    std::map<tree_t, Place> aliases;

    std::vector<Loop> loops;
};

} // namespace enzan::frontend::vhdl
