// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slang {
class SourceLocation;
}

namespace slang::ast {
class CallExpression;
class Expression;
class RangeSelectExpression;
class Statement;
class TimingControl;
class Type;
class ValueSymbol;
} // namespace slang::ast

namespace enzan::frontend::sv {

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

    /// Maps a slang source location into Enzan's source manager.
    virtual SourceLocation convertLocation(slang::SourceLocation location) = 0;

    /// Reports a construct the frontend does not handle yet.
    virtual void reportUnsupported(slang::SourceLocation location, std::string_view what) = 0;

    /// The module being built, for object types and widths.
    [[nodiscard]] virtual const eir::ModuleDefinition& definition() const = 0;
};

/// Translates one procedural block or continuous assignment into an EIR process.
///
/// Produces EIR-B directly rather than an intermediate tree: slang has already resolved
/// names, types and widths and expanded generate constructs, so what is left is a
/// straightforward lowering of a typed AST to three-address form.
///
/// A construct the translator does not handle is reported and the whole process is
/// abandoned. Emitting a partially translated body would produce a design that simulates
/// and gives the wrong answer, which DESIGN_GUIDE.md section 1.2 ranks as the worst
/// possible outcome; refusing to elaborate is the safe failure.
class ProcessTranslator {
public:
    ProcessTranslator(ModuleContext& owner, eir::ProcessKind kind, std::string name,
                      SourceLocation location);

    /// Translates a procedural block body. Returns false having reported why, on failure.
    bool translateBody(const slang::ast::Statement& body);

    /// Translates a continuous assignment's expression.
    bool translateContinuousAssignment(const slang::ast::Expression& assignment);

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
    /// could reach (DESIGN_GUIDE.md section 3.7). What it could reach is bounded by the
    /// index's type: a two-bit index into a byte reaches four positions, not eight. An index
    /// of 32 bits or more reaches anything, since no object is wider than a 32-bit count of
    /// bits.
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
    bool translateTimed(const slang::ast::Statement& statement);

    /// Emits the suspension for a timing control. Returns false on an unsupported one.
    bool translateTimingControl(const slang::ast::TimingControl& timing);

    // --- expressions -------------------------------------------------------

    std::optional<Value> translateExpression(const slang::ast::Expression& expression);
    std::optional<Value> translateBinary(const slang::ast::Expression& expression);
    std::optional<Value> translateUnary(const slang::ast::Expression& expression);
    std::optional<Value> translateConditionalExpression(const slang::ast::Expression& expression);
    std::optional<Value> translateConcatenation(const slang::ast::Expression& expression);
    std::optional<Value> translateReplication(const slang::ast::Expression& expression);
    std::optional<Value> translateElementSelect(const slang::ast::Expression& expression);
    std::optional<Value> translateRangeSelect(const slang::ast::Expression& expression);
    std::optional<Value> translateCall(const slang::ast::Expression& expression);
    std::optional<Value> translateInside(const slang::ast::Expression& expression);

    /// `$isunknown`, `$countones`, `$onehot` and `$onehot0` (IEEE 1800 section 20.9).
    std::optional<Value> translateBitCount(std::string_view name,
                                           const slang::ast::CallExpression& call,
                                           const slang::ast::Expression& expression);

    /// Emits a comparison of two values, each extended to the wider of the two and compared
    /// as signed only when both are, and returns its one-bit result.
    Value compare(eir::Opcode opcode, const Value& left, const Value& right);

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

    /// Scales a declared index to a bit offset, accounting for the element width.
    std::optional<Value> scaleIndexToBitOffset(const Value& index, std::int32_t rangeLow,
                                               std::uint32_t elementBits);

    /// Converts a declared index into a bit offset.
    ///
    /// A declared range such as `[15:8]` puts its least significant bit at index 8, so a
    /// select of index 8 addresses bit 0 of the storage. The subtraction is emitted only
    /// when the range's low bound is not zero, which is the overwhelmingly common case.
    Value shiftIndexToBitOffset(const Value& index, std::int32_t rangeLow);

    /// Computes the bit offset of the low end of a range select.
    std::optional<Value> lowBitOfRangeSelect(const slang::ast::RangeSelectExpression& select,
                                             std::int32_t rangeLow, eir::Type partType,
                                             std::uint32_t elementBits);

    /// The bits a write through a range select drives, as a low bit and a width: exactly its
    /// bits when its position is constant, and everything it could reach when it is not.
    std::pair<std::int64_t, std::int64_t>
    writtenBitsOfRangeSelect(const slang::ast::RangeSelectExpression& select, std::int32_t rangeLow,
                             std::uint32_t elementBits, std::uint32_t width,
                             std::uint32_t objectWidth);

    /// Returns the object a select is being written through, or nothing having reported
    /// why.
    ///
    /// Writing through anything but a plain signal reference — a select of a select, a
    /// function call result — is refused rather than approximated, because a write to the
    /// wrong bits is indistinguishable from a design bug.
    std::optional<std::uint32_t> objectOfLValue(const slang::ast::Expression& base);

    ModuleContext* module;

    /// The current value of a compound assignment's target, while its right-hand side is
    /// being translated.
    ///
    /// slang rewrites `a += b` as `a = <lvalue> + b` so that the target is evaluated once,
    /// and `<lvalue>` is an LValueReference expression standing for that single evaluation.
    /// Saved and restored around the translation, so that nested compound assignments do not
    /// see each other's target.
    std::optional<Value> lvalueReference;
    eir::Process process;
    std::uint32_t current = 0;

    std::vector<std::uint32_t> reads;
    std::vector<eir::WriteExtent> writes;

    bool failed = false;
};

} // namespace enzan::frontend::sv
