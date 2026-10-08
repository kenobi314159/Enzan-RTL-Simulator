// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/ParameterExpression.h"

#include "common/Assert.h"
#include "eir/BitOps.h"

#include <array>
#include <limits>
#include <utility>

namespace enzan::eir {

namespace {

/// True for the operations an expression may apply: those that compute a value from values and
/// touch nothing else.
bool isExpressionOperation(Opcode opcode) {
    switch (opcode) {
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor:
    case Opcode::Xnor:
    case Opcode::Not:
    case Opcode::ReduceAnd:
    case Opcode::ReduceOr:
    case Opcode::ReduceXor:
    case Opcode::CountOnes:
    case Opcode::CeilLog2:
    case Opcode::LogicalAnd:
    case Opcode::LogicalOr:
    case Opcode::LogicalNot:
    case Opcode::Add:
    case Opcode::Subtract:
    case Opcode::Multiply:
    case Opcode::Divide:
    case Opcode::Modulo:
    case Opcode::Negate:
    case Opcode::Power:
    case Opcode::ShiftLeft:
    case Opcode::ShiftRightLogical:
    case Opcode::ShiftRightArithmetic:
    case Opcode::Equal:
    case Opcode::NotEqual:
    case Opcode::CaseEqual:
    case Opcode::CaseNotEqual:
    case Opcode::WildcardEqual:
    case Opcode::WildcardNotEqual:
    case Opcode::LessThan:
    case Opcode::LessEqual:
    case Opcode::Conditional: return true;
    default: return false;
    }
}

/// The operands `node` refers to, in `a`, `b`, `c` order; a missing one is `NoNode`.
std::array<std::uint32_t, 3> operandsOf(const ParameterNode& node) {
    switch (node.kind) {
    case ParameterNodeKind::Constant:
    case ParameterNodeKind::Formal:
        return {ParameterNode::NoNode, ParameterNode::NoNode, ParameterNode::NoNode};
    case ParameterNodeKind::Call: return {ParameterNode::NoNode, node.b, ParameterNode::NoNode};
    default: return {node.a, node.b, node.c};
    }
}

/// `value` as an unsigned integer when every bit is known and it fits in 64 bits.
std::optional<std::uint64_t> knownUnsigned(const ParameterValue& value) {
    const ConstBitRef bits = value.bits.ref();
    if (bits.hasUnknown()) return std::nullopt;
    if (value.isSigned && bits.width() > 0 && bits.get(bits.width() - 1) == Logic::One)
        return std::nullopt;
    if (bits.width() > 64) {
        BitVector upper(bits.width() - 64);
        bitops::partSelectAt(upper.mutableRef(), bits, 64);
        if (!upper.ref().isZero()) return std::nullopt;
    }
    return bits.toUInt64();
}

} // namespace

std::uint32_t ParameterExpressions::add(const ParameterNode& node) {
    for (const std::uint32_t operand : operandsOf(node))
        ENZAN_ASSERT(operand == ParameterNode::NoNode || operand < nodes.size(),
                     "a node's operands are added before it");
    ENZAN_ASSERT(node.widthNode == ParameterNode::NoNode || node.widthNode < nodes.size(),
                 "a node's width is added before it");
    ENZAN_ASSERT(node.kind != ParameterNodeKind::Operation || isExpressionOperation(node.opcode),
                 "an expression applies only operations that compute a value");

    const auto found = positions.find(node);
    if (found != positions.end()) return found->second;

    const auto position = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back(node);
    positions.emplace(node, position);
    return position;
}

std::uint32_t ParameterExpressions::constant(const BitVector& value, bool isSigned) {
    std::uint32_t position = 0;
    while (position < constants.size() && !(constants[position] == value)) ++position;
    if (position == constants.size()) constants.push_back(value);

    return add(ParameterNode{.kind = ParameterNodeKind::Constant,
                             .isSigned = isSigned,
                             .width = value.width(),
                             .a = position});
}

std::uint32_t ParameterExpressions::formal(std::uint32_t formalIndex, std::uint32_t width,
                                           bool isSigned, std::uint32_t widthNode) {
    return add(ParameterNode{.kind = ParameterNodeKind::Formal,
                             .isSigned = isSigned,
                             .width = width,
                             .widthNode = widthNode,
                             .a = formalIndex});
}

std::uint32_t ParameterExpressions::function(std::string_view name) {
    std::uint32_t position = 0;
    while (position < functions.size() && functions[position] != name) ++position;
    if (position == functions.size()) functions.emplace_back(name);
    return position;
}

std::optional<ParameterExpressions>
ParameterExpressions::fromParts(std::vector<ParameterNode> parts, std::vector<BitVector> values,
                                std::vector<std::string> names) {
    ParameterExpressions result;
    result.constants = std::move(values);
    result.functions = std::move(names);
    for (const ParameterNode& node : parts) {
        const auto position = static_cast<std::uint32_t>(result.nodes.size());
        for (const std::uint32_t operand : operandsOf(node))
            if (operand != ParameterNode::NoNode && operand >= position) return std::nullopt;
        if (node.widthNode != ParameterNode::NoNode && node.widthNode >= position)
            return std::nullopt;
        if (node.kind == ParameterNodeKind::Constant && node.a >= result.constants.size())
            return std::nullopt;
        if (node.kind == ParameterNodeKind::Call && node.a >= result.functions.size())
            return std::nullopt;
        if (node.kind == ParameterNodeKind::Operation && !isExpressionOperation(node.opcode))
            return std::nullopt;
        if (!result.positions.emplace(node, position).second) return std::nullopt;
        result.nodes.push_back(node);
    }
    return result;
}

ParameterEvaluator::ParameterEvaluator(const ParameterExpressions& table,
                                       std::span<const std::optional<ParameterValue>> values,
                                       ConstantFunctions* runner)
    : expressions(&table),
      formals(values),
      functions(runner),
      cache(table.size()),
      attempted(table.size(), false),
      empty(table.size(), false) {}

const ParameterValue* ParameterEvaluator::value(std::uint32_t index) {
    ENZAN_ASSERT(index < expressions->size(), "a node of this evaluator's table");
    // A table only grows, and a node never changes once added, so what was evaluated stays
    // right and the cache only has to catch up with nodes added since.
    if (index >= cache.size()) {
        cache.resize(expressions->size());
        attempted.resize(expressions->size(), false);
        empty.resize(expressions->size(), false);
    }
    if (cache[index].has_value()) return &*cache[index];
    if (attempted[index]) return nullptr;
    attempted[index] = true;
    return compute(index);
}

std::optional<std::uint32_t> ParameterEvaluator::integer(std::uint32_t index) {
    const ParameterValue* result = value(index);
    if (result == nullptr) return std::nullopt;
    const std::optional<std::uint64_t> known = knownUnsigned(*result);
    if (!known.has_value()) {
        fail(index, ParameterError::NotAnInteger);
        return std::nullopt;
    }
    if (*known > std::numeric_limits<std::uint32_t>::max()) {
        fail(index, ParameterError::TooWide);
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*known);
}

bool ParameterEvaluator::fail(std::uint32_t index, ParameterError error) {
    lastError = error;
    failed = index;
    return false;
}

std::optional<std::uint32_t> ParameterEvaluator::resultWidth(const ParameterNode& node) {
    if (node.widthNode == ParameterNode::NoNode) return node.width;
    const std::optional<std::uint32_t> width = integer(node.widthNode);
    if (!width.has_value()) return std::nullopt;
    if (*width == 0) {
        fail(node.widthNode, ParameterError::ZeroWidth);
        return std::nullopt;
    }
    if (*width > MaxParameterWidth) {
        fail(node.widthNode, ParameterError::TooWide);
        return std::nullopt;
    }
    return width;
}

const ParameterValue* ParameterEvaluator::compute(std::uint32_t index) {
    const ParameterNode& node = expressions->node(index);

    // An empty value exists only as an empty replication, and only a concatenation may hold
    // one.
    const auto operand = [&](std::uint32_t operandIndex) -> const ParameterValue* {
        const ParameterValue* result = value(operandIndex);
        if (result != nullptr && empty[operandIndex] && node.kind != ParameterNodeKind::Concat) {
            fail(operandIndex, ParameterError::EmptyReplication);
            return nullptr;
        }
        return result;
    };
    const auto store = [&](BitVector bits) -> const ParameterValue* {
        cache[index] = ParameterValue{.bits = std::move(bits), .isSigned = node.isSigned};
        return &*cache[index];
    };
    const auto storeEmpty = [&]() -> const ParameterValue* {
        empty[index] = true;
        return store(BitVector::fromUInt64(0, 1));
    };

    switch (node.kind) {
    case ParameterNodeKind::Constant: return store(expressions->allConstants()[node.a]);

    case ParameterNodeKind::Formal: {
        if (node.a >= formals.size() || !formals[node.a].has_value()) {
            fail(index, ParameterError::MissingFormal);
            return nullptr;
        }
        return store(formals[node.a]->bits);
    }

    case ParameterNodeKind::Extend:
    case ParameterNodeKind::Truncate: {
        const ParameterValue* source = operand(node.a);
        const std::optional<std::uint32_t> width = resultWidth(node);
        if (source == nullptr || !width.has_value()) return nullptr;
        return store(source->bits.resized(*width, node.kind == ParameterNodeKind::Extend
                                                      && source->isSigned));
    }

    case ParameterNodeKind::Concat: {
        const ParameterValue* high = operand(node.a);
        const ParameterValue* low = operand(node.b);
        if (high == nullptr || low == nullptr) return nullptr;
        if (empty[node.a] && empty[node.b]) return storeEmpty();
        if (empty[node.a]) return store(low->bits);
        if (empty[node.b]) return store(high->bits);
        const std::uint64_t width = std::uint64_t{high->bits.width()} + low->bits.width();
        if (width > MaxParameterWidth) {
            fail(index, ParameterError::TooWide);
            return nullptr;
        }
        BitVector result(static_cast<std::uint32_t>(width));
        bitops::assignPartAt(result.mutableRef(), low->bits, 0);
        bitops::assignPartAt(result.mutableRef(), high->bits, low->bits.width());
        return store(std::move(result));
    }

    case ParameterNodeKind::Replicate: {
        const std::optional<std::uint32_t> count = integer(node.a);
        const ParameterValue* copy = operand(node.b);
        if (!count.has_value() || copy == nullptr) return nullptr;
        if (*count == 0 || empty[node.b]) return storeEmpty();
        const std::uint64_t width = std::uint64_t{*count} * copy->bits.width();
        if (width > MaxParameterWidth) {
            fail(index, ParameterError::TooWide);
            return nullptr;
        }
        BitVector result(static_cast<std::uint32_t>(width));
        for (std::uint32_t copyIndex = 0; copyIndex < *count; ++copyIndex)
            bitops::assignPartAt(result.mutableRef(), copy->bits, copyIndex * copy->bits.width());
        return store(std::move(result));
    }

    case ParameterNodeKind::PartSelect: {
        const ParameterValue* source = operand(node.a);
        const std::optional<std::uint32_t> low = integer(node.b);
        const std::optional<std::uint32_t> width = resultWidth(node);
        if (source == nullptr || !low.has_value() || !width.has_value()) return nullptr;
        if (std::uint64_t{*low} + *width > source->bits.width()) {
            fail(index, ParameterError::SelectOutOfRange);
            return nullptr;
        }
        BitVector result(*width);
        bitops::partSelectAt(result.mutableRef(), source->bits, *low);
        return store(std::move(result));
    }

    case ParameterNodeKind::WidthOf: {
        const ParameterValue* source = operand(node.a);
        if (source == nullptr) return nullptr;
        return store(BitVector::fromUInt64(source->bits.width(), 32));
    }

    case ParameterNodeKind::Call: {
        std::optional<BitVector> arguments;
        if (node.b != ParameterNode::NoNode) {
            const ParameterValue* given = operand(node.b);
            if (given == nullptr) return nullptr;
            arguments = given->bits;
        }
        const std::optional<std::uint32_t> width = resultWidth(node);
        if (!width.has_value()) return nullptr;
        std::optional<BitVector> returned;
        if (functions != nullptr)
            returned = functions->call(expressions->allFunctions()[node.a], arguments, *width);
        if (!returned.has_value() || returned->width() != *width) {
            fail(index, ParameterError::CallFailed);
            return nullptr;
        }
        return store(std::move(*returned));
    }

    case ParameterNodeKind::Operation: break;
    }

    const std::optional<std::uint32_t> width = resultWidth(node);
    const ParameterValue* a = operand(node.a);
    const ParameterValue* b = node.b != ParameterNode::NoNode ? operand(node.b) : nullptr;
    const ParameterValue* c = node.c != ParameterNode::NoNode ? operand(node.c) : nullptr;
    if (!width.has_value() || a == nullptr) return nullptr;
    if ((node.b != ParameterNode::NoNode && b == nullptr)
        || (node.c != ParameterNode::NoNode && c == nullptr))
        return nullptr;

    BitVector result(*width);
    const BitRef dst = result.mutableRef();
    const auto needs = [&](const ParameterValue* operandValue) {
        ENZAN_ASSERT(operandValue != nullptr, "the operation's operands are present");
        return operandValue->bits.ref();
    };

    switch (node.opcode) {
    case Opcode::And: bitops::bitwiseAnd(dst, a->bits, needs(b)); break;
    case Opcode::Or: bitops::bitwiseOr(dst, a->bits, needs(b)); break;
    case Opcode::Xor: bitops::bitwiseXor(dst, a->bits, needs(b)); break;
    case Opcode::Xnor: bitops::bitwiseXnor(dst, a->bits, needs(b)); break;
    case Opcode::Not: bitops::bitwiseNot(dst, a->bits); break;
    case Opcode::ReduceAnd: bitops::reduceAnd(dst, a->bits); break;
    case Opcode::ReduceOr: bitops::reduceOr(dst, a->bits); break;
    case Opcode::ReduceXor: bitops::reduceXor(dst, a->bits); break;
    case Opcode::CountOnes: bitops::countOnes(dst, a->bits); break;
    case Opcode::CeilLog2: bitops::ceilLog2(dst, a->bits); break;
    case Opcode::LogicalAnd: bitops::logicalAnd(dst, a->bits, needs(b)); break;
    case Opcode::LogicalOr: bitops::logicalOr(dst, a->bits, needs(b)); break;
    case Opcode::LogicalNot: bitops::logicalNot(dst, a->bits); break;
    case Opcode::Add: bitops::add(dst, a->bits, needs(b)); break;
    case Opcode::Subtract: bitops::subtract(dst, a->bits, needs(b)); break;
    case Opcode::Multiply: bitops::multiply(dst, a->bits, needs(b)); break;
    case Opcode::Divide: bitops::divide(dst, a->bits, needs(b), node.operationSigned); break;
    case Opcode::Modulo: bitops::modulo(dst, a->bits, needs(b), node.operationSigned); break;
    case Opcode::Negate: bitops::negate(dst, a->bits); break;
    case Opcode::Power:
        bitops::power(dst, a->bits, needs(b), a->isSigned, b != nullptr && b->isSigned);
        break;
    case Opcode::ShiftLeft: bitops::shiftLeft(dst, a->bits, needs(b)); break;
    case Opcode::ShiftRightLogical: bitops::shiftRightLogical(dst, a->bits, needs(b)); break;
    case Opcode::ShiftRightArithmetic: bitops::shiftRightArithmetic(dst, a->bits, needs(b)); break;
    case Opcode::Equal: bitops::equal(dst, a->bits, needs(b)); break;
    case Opcode::NotEqual: bitops::notEqual(dst, a->bits, needs(b)); break;
    case Opcode::CaseEqual: bitops::caseEqual(dst, a->bits, needs(b)); break;
    case Opcode::CaseNotEqual: bitops::caseNotEqual(dst, a->bits, needs(b)); break;
    case Opcode::WildcardEqual: bitops::wildcardEqual(dst, a->bits, needs(b)); break;
    case Opcode::WildcardNotEqual: bitops::wildcardNotEqual(dst, a->bits, needs(b)); break;
    case Opcode::LessThan: bitops::lessThan(dst, a->bits, needs(b), node.operationSigned); break;
    case Opcode::LessEqual: bitops::lessEqual(dst, a->bits, needs(b), node.operationSigned); break;
    case Opcode::Conditional: bitops::conditional(dst, a->bits, needs(b), needs(c)); break;
    default: ENZAN_UNREACHABLE("an operation fromParts or add accepted");
    }
    return store(std::move(result));
}

} // namespace enzan::eir
