// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Instruction.h"

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace enzan::eir {

/// What one node of a parameter expression computes.
enum class ParameterNodeKind : std::uint8_t {
    /// A value fixed by the source: `a` indexes the expression table's constants.
    Constant,
    /// The value of one of the unit's formal parameters: `a` is its index (see
    /// `ParameterFormal`).
    Formal,
    /// An operation of EIR-B applied to `a`, and `b` when it takes two operands, with the
    /// semantics BitOps gives it. The operands have the widths the operation requires, as an
    /// instruction's registers do: the frontend inserts the extensions SystemVerilog's sizing
    /// rules call for.
    Operation,
    /// `a` widened to the node's width, sign-extended when `a` is signed.
    Extend,
    /// The low bits of `a`, as many as the node's width.
    Truncate,
    /// `a` above `b`.
    Concat,
    /// `b` repeated as many times as `a`, which is an integer. A count of zero is allowed only
    /// as an operand of a concatenation, as IEEE 1800 section 11.4.12.1 allows it.
    Replicate,
    /// The node's width of bits of `a`, from the bit `b` counts up from, which is an integer.
    PartSelect,
    /// The width of `a`'s value, as a 32-bit signed integer: `$bits` of something whose width
    /// is a parameter's.
    WidthOf,
};

/// One node of a parameter expression.
///
/// The result has `width` bits, unless `widthNode` names a node, in which case it has as many
/// as that node's value: the width of `{W{1'b0}}` is `W`. Signedness is fixed rather than
/// computed from the operands, because the frontend has already applied the language's rules
/// for it.
struct ParameterNode {
    ParameterNodeKind kind = ParameterNodeKind::Constant;
    /// The operation, when `kind` is `Operation`.
    Opcode opcode = Opcode::Nop;
    /// Whether the result is signed.
    bool isSigned = false;
    /// Whether a division, a modulo or a comparison treats its operands as signed, as an
    /// instruction's signedness flag says. Separate from the result's: comparing two signed
    /// values gives an unsigned bit.
    bool operationSigned = false;
    std::uint32_t width = 0;
    std::uint32_t widthNode = NoNode;
    std::uint32_t a = NoNode;
    std::uint32_t b = NoNode;
    std::uint32_t c = NoNode;

    static constexpr std::uint32_t NoNode = 0xFFFF'FFFFU;

    friend auto operator<=>(const ParameterNode&, const ParameterNode&) = default;
};

/// A parameter's value, which keeps its signedness so that an expression using it applies the
/// right rules.
struct ParameterValue {
    BitVector bits;
    bool isSigned = false;

    friend bool operator==(const ParameterValue&, const ParameterValue&) = default;
};

/// Why an expression has no value.
enum class ParameterError : std::uint8_t {
    /// An integer was needed — a width, a count, a select's position — and the value had an
    /// unknown bit or was negative.
    NotAnInteger,
    /// A width came out larger than any object Enzan can store.
    TooWide,
    /// A width came out zero: `logic [W-1:0]` with `W` of 0, which has no bits to store.
    ZeroWidth,
    /// A replication of zero copies stood somewhere other than inside a concatenation.
    EmptyReplication,
    /// A part select reached outside the value it selects from.
    SelectOutOfRange,
    /// A formal had no value: a parameter with no default that nothing set.
    MissingFormal,
};

/// The expressions over a unit's formal parameters, which say what each number that depends on
/// a parameter is (DESIGN_GUIDE.md section 18, M5.1).
///
/// Held per unit, and indexed by position, so that an object, a register or a constant names
/// the expression its number comes from with one integer and a unit stays independently usable
/// (section 4.4). Identical nodes are stored once: the width `W` of every signal declared
/// `[W-1:0]` is one node, which is what lets the specialiser evaluate it once and lets two
/// widths be compared by index.
///
/// Evaluation uses the operations of BitOps and nothing else, so an expression means exactly
/// what the same operation means when a process runs it: there is one implementation of each
/// operation in Enzan, not a second one for constants to drift from.
class ParameterExpressions {
public:
    /// Adds `node`, or finds an identical one, and returns its index.
    std::uint32_t add(const ParameterNode& node);

    /// Adds a constant leaf of `value`'s width and the given signedness.
    std::uint32_t constant(const BitVector& value, bool isSigned);

    /// Adds a leaf that is formal `index`, of `width` bits.
    ///
    /// The width is the formal's declared one, which an actual is converted to before it is
    /// used, so it is fixed for every parameterisation; `widthNode` names it when it depends
    /// on an earlier formal, as `parameter [W-1:0] INIT` does.
    std::uint32_t formal(std::uint32_t index, std::uint32_t width, bool isSigned,
                         std::uint32_t widthNode = ParameterNode::NoNode);

    [[nodiscard]] const ParameterNode& node(std::uint32_t index) const { return nodes.at(index); }

    [[nodiscard]] std::size_t size() const noexcept { return nodes.size(); }

    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }

    [[nodiscard]] std::span<const ParameterNode> allNodes() const noexcept { return nodes; }

    [[nodiscard]] std::span<const BitVector> allConstants() const noexcept { return constants; }

    /// Rebuilds a table from its parts, as a decoder has them. Returns nothing when a node
    /// refers to a node after itself or to a constant that does not exist, which is what makes
    /// evaluation terminate.
    static std::optional<ParameterExpressions> fromParts(std::vector<ParameterNode> nodes,
                                                         std::vector<BitVector> constants);

    friend bool operator==(const ParameterExpressions& left, const ParameterExpressions& right) {
        return left.nodes == right.nodes && left.constants == right.constants;
    }

private:
    std::vector<ParameterNode> nodes;
    std::vector<BitVector> constants;
    /// Position of each node, so that `add` finds an identical one without a scan. Ordered, so
    /// that nothing about the table depends on a hash (section 3.8).
    std::map<ParameterNode, std::uint32_t> positions;
};

/// Evaluates nodes of one table against one set of formal values.
///
/// Every node is evaluated at most once, however many others use it: a width referred to by a
/// hundred registers costs one evaluation per parameterisation.
class ParameterEvaluator {
public:
    /// `formals` holds each formal's value, by index; one with no value is absent. Both
    /// arguments must outlive the evaluator.
    ParameterEvaluator(const ParameterExpressions& expressions,
                       std::span<const std::optional<ParameterValue>> formals);

    /// The value of node `index`, or why it has none.
    [[nodiscard]] const ParameterValue* value(std::uint32_t index);

    /// The value of node `index` as a non-negative integer that fits in 32 bits: a width, a
    /// count or a bit position.
    [[nodiscard]] std::optional<std::uint32_t> integer(std::uint32_t index);

    /// Why the last call that returned nothing did.
    [[nodiscard]] ParameterError error() const noexcept { return lastError; }

    /// The node whose evaluation failed first, for a diagnostic to point at.
    [[nodiscard]] std::uint32_t failedNode() const noexcept { return failed; }

private:
    const ParameterValue* compute(std::uint32_t index);
    bool fail(std::uint32_t index, ParameterError error);
    std::optional<std::uint32_t> resultWidth(const ParameterNode& node);

    const ParameterExpressions* expressions;
    std::span<const std::optional<ParameterValue>> formals;
    std::vector<std::optional<ParameterValue>> cache;
    std::vector<bool> attempted;
    /// Nodes whose value has no bits: a replication of zero copies, or a concatenation of
    /// nothing else. A BitVector has at least one bit, so the value cached for one is a
    /// placeholder that nothing but a concatenation may read.
    std::vector<bool> empty;
    ParameterError lastError = ParameterError::NotAnInteger;
    std::uint32_t failed = ParameterNode::NoNode;
};

/// The index that says a number is the number written beside it rather than the value of an
/// expression: every number of a unit that does not depend on a parameter.
inline constexpr std::uint32_t NoExpression = ParameterNode::NoNode;

/// The widest value a parameter expression may produce, which is the widest vector slang
/// accepts: a width beyond it is a mistake in the design rather than something to allocate.
inline constexpr std::uint32_t MaxParameterWidth = 1U << 24;

} // namespace enzan::eir
