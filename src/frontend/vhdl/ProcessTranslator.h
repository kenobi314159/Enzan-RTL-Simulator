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

/// What a process translator needs from the architecture it belongs to.
///
/// An interface, as the SystemVerilog frontend's `ModuleContext` is, so that statement and
/// expression translation stays apart from the hierarchy walk: the architecture side owns the
/// objects and the generics, and the process side asks for them.
class ArchitectureScope {
public:
    ArchitectureScope() = default;
    ArchitectureScope(const ArchitectureScope&) = delete;
    ArchitectureScope& operator=(const ArchitectureScope&) = delete;
    ArchitectureScope(ArchitectureScope&&) = delete;
    ArchitectureScope& operator=(ArchitectureScope&&) = delete;
    virtual ~ArchitectureScope() = default;

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
};

/// Translates one VHDL process into an EIR process.
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
class ProcessTranslator {
public:
    ProcessTranslator(ArchitectureScope& scope, tree_t statement, std::string name);

    /// The translated process, or nothing having reported why.
    std::optional<eir::Process> translate();

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
    bool assertion(tree_t statement);
    bool report(tree_t statement);
    bool procedureCall(tree_t statement);

    /// Emits a `report` system task of `severity`, a position of `severity_level`, with the
    /// message `message` translates to, or `fallback` when there is none.
    bool emitReport(tree_t where, const Value& severity, tree_t message, std::string_view fallback);

    /// Appends the text of `message`, a string expression, to `format`, with what it computes as
    /// arguments: a literal is text, and a value's `'image` a conversion. Messages are built that
    /// way almost always, and it needs no string values at run time.
    bool messageParts(tree_t message, std::string& format,
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

    /// The element of `array` that `index` designates.
    std::optional<Value> element(const Value& array, tree_t index, type_t vhdlType);

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

    ArchitectureScope& owner;
    tree_t source;
    eir::Process process;

    std::uint32_t current = 0;
    SourceLocation statementLocation;

    std::set<std::uint32_t> reads;
    std::vector<eir::WriteExtent> writes;

    /// The register of each variable, constant and loop parameter declared in the process.
    std::map<tree_t, Value> locals;

    std::vector<Loop> loops;
};

} // namespace enzan::frontend::vhdl
