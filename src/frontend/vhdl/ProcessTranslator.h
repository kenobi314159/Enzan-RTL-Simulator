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

    void noteRead(std::uint32_t objectIndex);
    void noteWrite(std::uint32_t objectIndex);

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

    /// The object a signal assignment writes, when it is a whole signal or port.
    std::optional<std::uint32_t> targetObject(tree_t target);

    /// A register holding `delay`, a value of `time` in femtoseconds, as a count of EIR's unit.
    std::optional<Value> delay(tree_t delay);

    /// The test of one choice of a `case` against `selector`.
    std::optional<Value> choiceTest(const Value& selector, tree_t choice);

    // --- expressions ---------------------------------------------------------

    std::optional<Value> expression(tree_t expression);
    std::optional<Value> reference(tree_t expression);
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
    std::set<std::uint32_t> written;

    /// The register of each variable, constant and loop parameter declared in the process.
    std::map<tree_t, Value> locals;

    std::vector<Loop> loops;
};

} // namespace enzan::frontend::vhdl
