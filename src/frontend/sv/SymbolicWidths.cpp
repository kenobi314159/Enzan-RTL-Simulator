// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/SymbolicWidths.h"

#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/DeclaredType.h"
#include "slang/syntax/AllSyntax.h"

#include <algorithm>
#include <utility>

namespace enzan::frontend::sv {

namespace ast = slang::ast;
namespace syntax = slang::syntax;

namespace {

using eir::Opcode;
using eir::ParameterNode;
using eir::ParameterNodeKind;

/// True for an expression slang widens in place to the context it is used in, rather than
/// converting (IEEE 1800 section 11.6.1): what decides its width is above it.
bool takesContext(const ast::Expression& expression) {
    switch (expression.kind) {
    case ast::ExpressionKind::UnbasedUnsizedIntegerLiteral:
    case ast::ExpressionKind::ConditionalOp: return true;
    case ast::ExpressionKind::UnaryOp:
        switch (expression.as<ast::UnaryExpression>().op) {
        case ast::UnaryOperator::Plus:
        case ast::UnaryOperator::Minus:
        case ast::UnaryOperator::BitwiseNot: return true;
        default: return false;
        }
    case ast::ExpressionKind::BinaryOp:
        switch (expression.as<ast::BinaryExpression>().op) {
        case ast::BinaryOperator::Add:
        case ast::BinaryOperator::Subtract:
        case ast::BinaryOperator::Multiply:
        case ast::BinaryOperator::Divide:
        case ast::BinaryOperator::Mod:
        case ast::BinaryOperator::BinaryAnd:
        case ast::BinaryOperator::BinaryOr:
        case ast::BinaryOperator::BinaryXor:
        case ast::BinaryOperator::BinaryXnor:
        case ast::BinaryOperator::Power:
        case ast::BinaryOperator::LogicalShiftLeft:
        case ast::BinaryOperator::LogicalShiftRight:
        case ast::BinaryOperator::ArithmeticShiftLeft:
        case ast::BinaryOperator::ArithmeticShiftRight: return true;
        default: return false;
        }
    default: return false;
    }
}

/// True for a conversion slang inserted rather than one the source wrote.
bool isImplicit(const ast::Expression& expression) {
    return expression.kind == ast::ExpressionKind::Conversion
           && expression.as<ast::ConversionExpression>().isImplicit();
}

/// The declared packed dimensions in `syntax`, or null when it has none of its own: a keyword
/// type such as `int`, a typedef, a struct.
const syntax::SyntaxList<syntax::VariableDimensionSyntax>*
packedDimensions(const syntax::DataTypeSyntax* syntax) {
    if (syntax == nullptr) return nullptr;
    switch (syntax->kind) {
    case syntax::SyntaxKind::LogicType:
    case syntax::SyntaxKind::BitType:
    case syntax::SyntaxKind::RegType: return &syntax->as<syntax::IntegerTypeSyntax>().dimensions;
    case syntax::SyntaxKind::ImplicitType:
        return &syntax->as<syntax::ImplicitTypeSyntax>().dimensions;
    default: return nullptr;
    }
}

/// The packed type under every unpacked dimension of `type`.
const ast::Type& packedPart(const ast::Type& type) {
    const ast::Type* level = &type.getCanonicalType();
    while (level->kind == ast::SymbolKind::FixedSizeUnpackedArrayType)
        level = &level->as<ast::FixedSizeUnpackedArrayType>().elementType.getCanonicalType();
    return *level;
}

} // namespace

SymbolicWidths::SymbolicWidths(eir::ParameterExpressions& into, Lower lower, Bind bind,
                               Numeric numericWidth,
                               std::span<const std::optional<eir::ParameterValue>> representative,
                               eir::ConstantFunctions* functions)
    : table(&into),
      lowerExpression(std::move(lower)),
      bindExpression(std::move(bind)),
      numeric(std::move(numericWidth)),
      evaluator(into, representative, functions) {}

std::uint32_t SymbolicWidths::node(const Width& width) {
    if (!width.isFixed()) return width.expression;
    return table->constant(eir::BitVector::fromInt64(width.value, 32), true);
}

Width SymbolicWidths::max(const Width& left, const Width& right) {
    if (left.sameAs(right)) return left;
    // No width is less than one bit, which specialisation enforces: one bit is never the wider.
    if (left.isFixed() && left.value <= 1) return right;
    if (right.isFixed() && right.value <= 1) return left;
    if (left.isFixed() && right.isFixed()) return fixed(std::max(left.value, right.value));
    const std::uint32_t a = node(left);
    const std::uint32_t b = node(right);
    const std::uint32_t less = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                        .opcode = Opcode::LessThan,
                                                        .operationSigned = true,
                                                        .width = 1,
                                                        .a = a,
                                                        .b = b});
    return Width{.value = std::max(left.value, right.value),
                 .expression = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                        .opcode = Opcode::Conditional,
                                                        .isSigned = true,
                                                        .width = 32,
                                                        .a = less,
                                                        .b = b,
                                                        .c = a})};
}

Width SymbolicWidths::sum(const Width& left, const Width& right) {
    if (left.isFixed() && right.isFixed()) return fixed(left.value + right.value);
    return Width{.value = left.value + right.value,
                 .expression = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                        .opcode = Opcode::Add,
                                                        .isSigned = true,
                                                        .operationSigned = true,
                                                        .width = 32,
                                                        .a = node(left),
                                                        .b = node(right)})};
}

Width SymbolicWidths::product(const Width& left, const Width& right) {
    if (left.isFixed() && right.isFixed()) return fixed(left.value * right.value);
    if (left.isFixed() && left.value == 1) return right;
    if (right.isFixed() && right.value == 1) return left;
    return Width{.value = left.value * right.value,
                 .expression = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                        .opcode = Opcode::Multiply,
                                                        .isSigned = true,
                                                        .operationSigned = true,
                                                        .width = 32,
                                                        .a = node(left),
                                                        .b = node(right)})};
}

std::optional<std::int64_t> SymbolicWidths::representativeValue(std::uint32_t expression) {
    const eir::ParameterValue* value = evaluator.value(expression);
    if (value == nullptr || value->bits.ref().hasUnknown()) return std::nullopt;
    return value->isSigned ? value->bits.ref().toInt64()
                           : static_cast<std::int64_t>(value->bits.ref().toUInt64());
}

Width SymbolicWidths::checked(std::optional<Width> width, std::uint32_t expected) {
    if (!width.has_value()) {
        failed = true;
        return fixed(expected);
    }
    if (width->isFixed()) {
        if (width->value != expected) failed = true;
        return fixed(expected);
    }
    if (representativeValue(width->expression) != static_cast<std::int64_t>(expected))
        failed = true;
    return Width{.value = expected, .expression = width->expression};
}

std::optional<Width> SymbolicWidths::boundOf(const syntax::ExpressionSyntax& syntax,
                                             const ast::Scope& scope) {
    const ast::Expression* bound = bindExpression(syntax, scope);
    if (bound == nullptr || bound->type == nullptr || !bound->type->isIntegral())
        return std::nullopt;
    const std::optional<std::uint32_t> lowered = lowerExpression(*bound);
    if (!lowered.has_value()) return std::nullopt;

    // A width is a 32-bit signed integer whatever the bound was written as, so that every
    // width in a table can be combined with every other.
    std::uint32_t asInt = *lowered;
    if (bound->type->getBitWidth() != 32 || !bound->type->isSigned()) {
        asInt = table->add(ParameterNode{.kind = bound->type->getBitWidth() <= 32
                                                     ? ParameterNodeKind::Extend
                                                     : ParameterNodeKind::Truncate,
                                         .isSigned = true,
                                         .width = 32,
                                         .a = *lowered});
    }
    const std::optional<std::int64_t> value = representativeValue(asInt);
    if (!value.has_value()) return std::nullopt;
    const bool isConstant = table->node(*lowered).kind == ParameterNodeKind::Constant;
    return Width{.value = static_cast<std::uint32_t>(*value),
                 .expression = isConstant ? eir::NoExpression : asInt};
}

std::optional<Width> SymbolicWidths::sizeOf(const syntax::ExpressionSyntax* left,
                                            const syntax::ExpressionSyntax* right,
                                            const ast::Scope& scope,
                                            std::optional<Bounds>* bounds) {
    if (left == nullptr) return std::nullopt;
    const std::optional<Width> high = boundOf(*left, scope);
    if (!high.has_value()) return std::nullopt;

    // `[N]` is N elements, `[0:N-1]` (IEEE 1800 section 7.4.2).
    if (right == nullptr) {
        if (bounds != nullptr) {
            const Width last = high->isFixed() ? fixed(high->value - 1)
                                               : Width{.value = high->value - 1,
                                                       .expression = table->add(ParameterNode{
                                                           .kind = ParameterNodeKind::Operation,
                                                           .opcode = Opcode::Subtract,
                                                           .isSigned = true,
                                                           .operationSigned = true,
                                                           .width = 32,
                                                           .a = high->expression,
                                                           .b = node(fixed(1))})};
            *bounds = Bounds{.left = fixed(0), .right = last};
        }
        return high;
    }

    const std::optional<Width> low = boundOf(*right, scope);
    if (!low.has_value()) return std::nullopt;
    if (bounds != nullptr) *bounds = Bounds{.left = *high, .right = *low};

    const auto span = [&](std::int64_t a, std::int64_t b) {
        return static_cast<std::uint32_t>((a >= b ? a - b : b - a) + 1);
    };
    const std::uint32_t value =
        span(static_cast<std::int32_t>(high->value), static_cast<std::int32_t>(low->value));
    if (high->isFixed() && low->isFixed()) return fixed(value);

    // |high - low| + 1, as either order of the bounds declares it.
    const std::uint32_t a = node(*high);
    const std::uint32_t b = node(*low);
    const std::uint32_t one = node(fixed(1));
    const auto difference = [&](std::uint32_t from, std::uint32_t to) {
        const std::uint32_t minus = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                             .opcode = Opcode::Subtract,
                                                             .isSigned = true,
                                                             .operationSigned = true,
                                                             .width = 32,
                                                             .a = from,
                                                             .b = to});
        return table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                        .opcode = Opcode::Add,
                                        .isSigned = true,
                                        .operationSigned = true,
                                        .width = 32,
                                        .a = minus,
                                        .b = one});
    };
    const std::uint32_t descending = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                              .opcode = Opcode::LessEqual,
                                                              .operationSigned = true,
                                                              .width = 1,
                                                              .a = b,
                                                              .b = a});
    return Width{.value = value,
                 .expression = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                        .opcode = Opcode::Conditional,
                                                        .isSigned = true,
                                                        .width = 32,
                                                        .a = descending,
                                                        .b = difference(a, b),
                                                        .c = difference(b, a)})};
}

namespace {

/// The two bound expressions of a dimension, the second null for `[N]`; both null for one that
/// is not a fixed range.
std::pair<const syntax::ExpressionSyntax*, const syntax::ExpressionSyntax*>
dimensionBounds(const syntax::VariableDimensionSyntax& dimension) {
    if (dimension.specifier == nullptr
        || dimension.specifier->kind != syntax::SyntaxKind::RangeDimensionSpecifier)
        return {nullptr, nullptr};
    const syntax::SelectorSyntax* selector =
        dimension.specifier->as<syntax::RangeDimensionSpecifierSyntax>().selector;
    if (selector->kind == syntax::SyntaxKind::BitSelect)
        return {selector->as<syntax::BitSelectSyntax>().expr, nullptr};
    if (selector->kind == syntax::SyntaxKind::SimpleRangeSelect) {
        const auto& range = selector->as<syntax::RangeSelectSyntax>();
        return {range.left, range.right};
    }
    return {nullptr, nullptr};
}

} // namespace

std::optional<Width> SymbolicWidths::packed(const ast::ValueSymbol& symbol) {
    const ast::Scope* scope = symbol.getParentScope();
    const ast::DeclaredType* declared = symbol.getDeclaredType();
    const std::optional<std::uint32_t> expected = numeric(packedPart(symbol.getType()));
    if (scope == nullptr || declared == nullptr || !expected.has_value()) return std::nullopt;

    const syntax::SyntaxList<syntax::VariableDimensionSyntax>* dimensions =
        packedDimensions(declared->getTypeSyntax());
    if (dimensions == nullptr || dimensions->empty()) return fixed(*expected);

    Width width = fixed(1);
    for (const syntax::VariableDimensionSyntax* dimension : *dimensions) {
        const auto [left, right] = dimensionBounds(*dimension);
        const std::optional<Width> size = sizeOf(left, right, *scope, nullptr);
        if (!size.has_value()) return std::nullopt;
        width = product(width, *size);
    }
    return checked(width, *expected);
}

std::optional<std::vector<Bounds>> SymbolicWidths::packedBounds(const ast::ValueSymbol& symbol) {
    const ast::Scope* scope = symbol.getParentScope();
    const ast::DeclaredType* declared = symbol.getDeclaredType();
    if (scope == nullptr || declared == nullptr) return std::nullopt;

    std::vector<Bounds> result;
    const syntax::SyntaxList<syntax::VariableDimensionSyntax>* dimensions =
        packedDimensions(declared->getTypeSyntax());
    if (dimensions == nullptr) return result;
    for (const syntax::VariableDimensionSyntax* dimension : *dimensions) {
        const auto [left, right] = dimensionBounds(*dimension);
        std::optional<Bounds> bounds;
        if (right == nullptr || !sizeOf(left, right, *scope, &bounds).has_value()
            || !bounds.has_value())
            return std::nullopt;
        result.push_back(*bounds);
    }
    // The outermost against slang's own range, as every width is checked.
    if (!result.empty()) {
        const slang::ConstantRange range = packedPart(symbol.getType()).getFixedRange();
        const auto check = [&](const Width& bound, std::int32_t expected) {
            if (bound.isFixed() ? static_cast<std::int32_t>(bound.value) != expected
                                : representativeValue(bound.expression) != expected)
                failed = true;
        };
        check(result.front().left, range.left);
        check(result.front().right, range.right);
    }
    return result;
}

std::optional<std::vector<Bounds>> SymbolicWidths::unpacked(const ast::ValueSymbol& symbol) {
    const ast::Scope* scope = symbol.getParentScope();
    const ast::DeclaredType* declared = symbol.getDeclaredType();
    if (scope == nullptr || declared == nullptr) return std::nullopt;

    std::vector<Bounds> result;
    const syntax::SyntaxList<syntax::VariableDimensionSyntax>* dimensions =
        declared->getDimensionSyntax();
    if (dimensions == nullptr) return result;
    const ast::Type* level = &symbol.getType().getCanonicalType();
    for (const syntax::VariableDimensionSyntax* dimension : *dimensions) {
        if (level->kind != ast::SymbolKind::FixedSizeUnpackedArrayType) return std::nullopt;
        const auto& array = level->as<ast::FixedSizeUnpackedArrayType>();
        const auto [left, right] = dimensionBounds(*dimension);
        std::optional<Bounds> bounds;
        if (!sizeOf(left, right, *scope, &bounds).has_value() || !bounds.has_value())
            return std::nullopt;
        // Each bound checked against slang's own, as a width is.
        const auto check = [&](const Width& bound, std::int32_t expected) {
            if (bound.isFixed() ? static_cast<std::int32_t>(bound.value) != expected
                                : representativeValue(bound.expression) != expected)
                failed = true;
        };
        check(bounds->left, array.range.left);
        check(bounds->right, array.range.right);
        result.push_back(*bounds);
        level = &array.elementType.getCanonicalType();
    }
    return result;
}

std::optional<Width> SymbolicWidths::storage(const ast::ValueSymbol& symbol) {
    if (const auto found = storages.find(&symbol); found != storages.end()) return found->second;

    std::optional<Width> result = packed(symbol);
    if (result.has_value()) {
        const std::optional<std::vector<Bounds>> dimensions = unpacked(symbol);
        if (!dimensions.has_value()) {
            result.reset();
        } else {
            for (const Bounds& bounds : *dimensions) result = product(*result, spanOf(bounds));
            if (const std::optional<std::uint32_t> expected = numeric(symbol.getType()))
                result = checked(result, *expected);
        }
    }
    storages.emplace(&symbol, result);
    return result;
}

Width SymbolicWidths::spanOf(const Bounds& bounds) {
    const auto a = static_cast<std::int32_t>(bounds.left.value);
    const auto b = static_cast<std::int32_t>(bounds.right.value);
    const auto value = static_cast<std::uint32_t>((a >= b ? a - b : b - a) + 1);
    if (bounds.left.isFixed() && bounds.right.isFixed()) return fixed(value);
    const std::uint32_t left = node(bounds.left);
    const std::uint32_t right = node(bounds.right);
    const std::uint32_t one = node(fixed(1));
    const auto difference = [&](std::uint32_t from, std::uint32_t to) {
        const std::uint32_t minus = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                             .opcode = Opcode::Subtract,
                                                             .isSigned = true,
                                                             .operationSigned = true,
                                                             .width = 32,
                                                             .a = from,
                                                             .b = to});
        return table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                        .opcode = Opcode::Add,
                                        .isSigned = true,
                                        .operationSigned = true,
                                        .width = 32,
                                        .a = minus,
                                        .b = one});
    };
    const std::uint32_t descending = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                              .opcode = Opcode::LessEqual,
                                                              .operationSigned = true,
                                                              .width = 1,
                                                              .a = right,
                                                              .b = left});
    return Width{.value = value,
                 .expression = table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                                        .opcode = Opcode::Conditional,
                                                        .isSigned = true,
                                                        .width = 32,
                                                        .a = descending,
                                                        .b = difference(left, right),
                                                        .c = difference(right, left)})};
}

std::optional<Width> SymbolicWidths::self(const ast::Expression& expression) {
    if (expression.type == nullptr) return std::nullopt;
    const std::optional<std::uint32_t> expected = numeric(*expression.type);
    if (!expected.has_value()) return std::nullopt;
    const Width own = fixed(*expected);

    switch (expression.kind) {
    case ast::ExpressionKind::NamedValue:
    case ast::ExpressionKind::HierarchicalValue: {
        const ast::ValueSymbol& symbol = expression.as<ast::ValueExpressionBase>().symbol;
        if (symbol.kind == ast::SymbolKind::Parameter || symbol.kind == ast::SymbolKind::EnumValue)
            return own;
        return storage(symbol);
    }

    case ast::ExpressionKind::IntegerLiteral:
    case ast::ExpressionKind::RealLiteral:
    case ast::ExpressionKind::StringLiteral:
    case ast::ExpressionKind::TimeLiteral: return own;

    // As wide as its context makes it, and one bit on its own.
    case ast::ExpressionKind::UnbasedUnsizedIntegerLiteral: return fixed(1);

    case ast::ExpressionKind::UnaryOp: {
        const auto& unary = expression.as<ast::UnaryExpression>();
        switch (unary.op) {
        case ast::UnaryOperator::Plus:
        case ast::UnaryOperator::Minus:
        case ast::UnaryOperator::BitwiseNot:
        case ast::UnaryOperator::Preincrement:
        case ast::UnaryOperator::Predecrement:
        case ast::UnaryOperator::Postincrement:
        case ast::UnaryOperator::Postdecrement: return self(unary.operand());
        default: return own;
        }
    }

    case ast::ExpressionKind::BinaryOp: {
        const auto& binary = expression.as<ast::BinaryExpression>();
        switch (binary.op) {
        case ast::BinaryOperator::Add:
        case ast::BinaryOperator::Subtract:
        case ast::BinaryOperator::Multiply:
        case ast::BinaryOperator::Divide:
        case ast::BinaryOperator::Mod:
        case ast::BinaryOperator::BinaryAnd:
        case ast::BinaryOperator::BinaryOr:
        case ast::BinaryOperator::BinaryXor:
        case ast::BinaryOperator::BinaryXnor: {
            const std::optional<Width> left = self(binary.left());
            const std::optional<Width> right = self(binary.right());
            if (!left.has_value() || !right.has_value()) return std::nullopt;
            return max(*left, *right);
        }
        case ast::BinaryOperator::Power:
        case ast::BinaryOperator::LogicalShiftLeft:
        case ast::BinaryOperator::LogicalShiftRight:
        case ast::BinaryOperator::ArithmeticShiftLeft:
        case ast::BinaryOperator::ArithmeticShiftRight: return self(binary.left());
        default: return own;
        }
    }

    case ast::ExpressionKind::ConditionalOp: {
        const auto& conditional = expression.as<ast::ConditionalExpression>();
        const std::optional<Width> left = self(conditional.left());
        const std::optional<Width> right = self(conditional.right());
        if (!left.has_value() || !right.has_value()) return std::nullopt;
        return max(*left, *right);
    }

    case ast::ExpressionKind::Conversion: {
        const auto& conversion = expression.as<ast::ConversionExpression>();
        // A conversion slang inserted stands for what it converts, whose own width is what the
        // sizing rules start from; one the source wrote that keeps the width — `signed'(x)`,
        // `$signed(x)` — is its operand's width too; any other cast's width is the type it
        // names, which the source rule keeps free of formals.
        if (conversion.isImplicit()) return self(conversion.operand());
        if (conversion.operand().type != nullptr && numeric(*conversion.operand().type) == expected
            && expression.syntax != nullptr
            && expression.syntax->kind == syntax::SyntaxKind::SignedCastExpression)
            return self(conversion.operand());
        return own;
    }

    case ast::ExpressionKind::Concatenation: {
        Width total = fixed(0);
        for (const ast::Expression* operand :
             expression.as<ast::ConcatenationExpression>().operands()) {
            if (operand->type == nullptr || operand->type->isVoid()) continue;
            const std::optional<Width> part = self(*operand);
            if (!part.has_value()) return std::nullopt;
            total = sum(total, *part);
        }
        return total;
    }

    case ast::ExpressionKind::Replication: {
        const auto& replication = expression.as<ast::ReplicationExpression>();
        const slang::ConstantValue* count = replication.count().getConstant();
        const std::optional<Width> copy = self(replication.concat());
        if (count == nullptr || !count->isInteger() || !copy.has_value()) return std::nullopt;
        const std::optional<std::int64_t> times = count->integer().as<std::int64_t>();
        if (!times.has_value() || *times < 0) return std::nullopt;
        return product(fixed(static_cast<std::uint32_t>(*times)), *copy);
    }

    case ast::ExpressionKind::ElementSelect: {
        const auto& select = expression.as<ast::ElementSelectExpression>();
        const ast::Expression& value = select.value();
        // An element of an array declared here is as wide as what is left of the declaration
        // once its outermost unpacked dimension is taken; a bit of a vector is one bit.
        if (value.type != nullptr
            && value.type->getCanonicalType().kind == ast::SymbolKind::FixedSizeUnpackedArrayType) {
            if (value.kind != ast::ExpressionKind::NamedValue) return own;
            const ast::ValueSymbol& symbol = value.as<ast::ValueExpressionBase>().symbol;
            std::optional<Width> width = packed(symbol);
            const std::optional<std::vector<Bounds>> dimensions = unpacked(symbol);
            if (!width.has_value() || !dimensions.has_value()) return std::nullopt;
            for (std::size_t index = 1; index < dimensions->size(); ++index)
                width = product(*width, spanOf((*dimensions)[index]));
            return width;
        }
        return own;
    }

    case ast::ExpressionKind::Call: {
        const auto& call = expression.as<ast::CallExpression>();
        if (call.isSystemCall()) {
            const std::string_view name = call.getSubroutineName();
            if ((name == "$signed" || name == "$unsigned") && !call.arguments().empty())
                return self(*call.arguments().front());
            return own;
        }
        const ast::SubroutineSymbol* subroutine =
            std::get_if<const ast::SubroutineSymbol*>(&call.subroutine) != nullptr
                ? std::get<const ast::SubroutineSymbol*>(call.subroutine)
                : nullptr;
        if (subroutine == nullptr || subroutine->returnValVar == nullptr) return own;
        return storage(*subroutine->returnValVar);
    }

    case ast::ExpressionKind::Assignment:
        return self(expression.as<ast::AssignmentExpression>().left());

    // Fixed by the source rule: a range select's bounds and a member's type name no formal.
    default: return own;
    }
}

void SymbolicWidths::visit(const ast::Expression& expression, std::optional<Width> context) {
    if (expression.type == nullptr) return;
    const std::optional<std::uint32_t> expected = numeric(*expression.type);
    std::optional<Width> own = self(expression);

    // What slang widened in place, or converted, is as wide as its context.
    std::optional<Width> width = own;
    if (context.has_value() && (takesContext(expression) || isImplicit(expression)))
        width = context;
    if (expected.has_value()) prepared.insert_or_assign(&expression, checked(width, *expected));

    const auto selfDetermined = [this](const ast::Expression& operand) {
        visit(operand, std::nullopt);
    };

    switch (expression.kind) {
    case ast::ExpressionKind::UnaryOp: {
        const auto& unary = expression.as<ast::UnaryExpression>();
        if (takesContext(expression))
            visit(unary.operand(), width);
        else
            selfDetermined(unary.operand());
        break;
    }

    case ast::ExpressionKind::BinaryOp: {
        const auto& binary = expression.as<ast::BinaryExpression>();
        switch (binary.op) {
        case ast::BinaryOperator::Power:
        case ast::BinaryOperator::LogicalShiftLeft:
        case ast::BinaryOperator::LogicalShiftRight:
        case ast::BinaryOperator::ArithmeticShiftLeft:
        case ast::BinaryOperator::ArithmeticShiftRight:
            visit(binary.left(), width);
            selfDetermined(binary.right());
            break;
        case ast::BinaryOperator::LogicalAnd:
        case ast::BinaryOperator::LogicalOr:
        case ast::BinaryOperator::LogicalImplication:
        case ast::BinaryOperator::LogicalEquivalence:
            selfDetermined(binary.left());
            selfDetermined(binary.right());
            break;
        default:
            if (takesContext(expression)) {
                visit(binary.left(), width);
                visit(binary.right(), width);
            } else {
                // A comparison sizes both operands to the wider of the two.
                const std::optional<Width> left = self(binary.left());
                const std::optional<Width> right = self(binary.right());
                const std::optional<Width> common = left.has_value() && right.has_value()
                                                        ? std::optional(max(*left, *right))
                                                        : std::nullopt;
                inContext(binary.left(), common);
                inContext(binary.right(), common);
            }
            break;
        }
        break;
    }

    case ast::ExpressionKind::ConditionalOp: {
        const auto& conditional = expression.as<ast::ConditionalExpression>();
        for (const auto& condition : conditional.conditions) selfDetermined(*condition.expr);
        visit(conditional.left(), width);
        visit(conditional.right(), width);
        break;
    }

    case ast::ExpressionKind::Conversion: {
        const ast::Expression& operand = expression.as<ast::ConversionExpression>().operand();
        if (isImplicit(expression) && takesContext(operand) && width.has_value()) {
            const std::optional<Width> operandOwn = self(operand);
            visit(operand,
                  operandOwn.has_value() ? std::optional(max(*width, *operandOwn)) : std::nullopt);
        } else {
            selfDetermined(operand);
        }
        break;
    }

    case ast::ExpressionKind::Assignment: {
        const auto& assignment = expression.as<ast::AssignmentExpression>();
        selfDetermined(assignment.left());
        inContext(assignment.right(), self(assignment.left()));
        break;
    }

    case ast::ExpressionKind::Concatenation:
        for (const ast::Expression* operand :
             expression.as<ast::ConcatenationExpression>().operands())
            selfDetermined(*operand);
        break;

    case ast::ExpressionKind::Replication: {
        const auto& replication = expression.as<ast::ReplicationExpression>();
        selfDetermined(replication.count());
        selfDetermined(replication.concat());
        break;
    }

    case ast::ExpressionKind::ElementSelect: {
        const auto& select = expression.as<ast::ElementSelectExpression>();
        selfDetermined(select.value());
        selfDetermined(select.selector());
        break;
    }

    case ast::ExpressionKind::RangeSelect: {
        const auto& select = expression.as<ast::RangeSelectExpression>();
        selfDetermined(select.value());
        selfDetermined(select.left());
        selfDetermined(select.right());
        break;
    }

    case ast::ExpressionKind::MemberAccess:
        selfDetermined(expression.as<ast::MemberAccessExpression>().value());
        break;

    case ast::ExpressionKind::Call: {
        const auto& call = expression.as<ast::CallExpression>();
        const ast::SubroutineSymbol* subroutine =
            std::get_if<const ast::SubroutineSymbol*>(&call.subroutine) != nullptr
                ? std::get<const ast::SubroutineSymbol*>(call.subroutine)
                : nullptr;
        const std::span<const ast::Expression* const> arguments = call.arguments();
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (arguments[index] == nullptr) continue;
            // An argument is assigned to its formal, and sized as an assignment is.
            if (subroutine != nullptr && index < subroutine->getArguments().size())
                inContext(*arguments[index], storage(*subroutine->getArguments()[index]));
            else
                selfDetermined(*arguments[index]);
        }
        break;
    }

    default: break;
    }
}

void SymbolicWidths::inContext(const ast::Expression& expression, std::optional<Width> target) {
    if (!target.has_value()) {
        visit(expression, std::nullopt);
        return;
    }
    if (isImplicit(expression)) {
        visit(expression, target);
        return;
    }
    if (takesContext(expression)) {
        const std::optional<Width> own = self(expression);
        visit(expression, own.has_value() ? std::optional(max(*target, *own)) : std::nullopt);
        return;
    }
    visit(expression, std::nullopt);
}

void SymbolicWidths::prepare(const ast::Expression& expression, std::optional<Width> context) {
    inContext(expression, context);
}

void SymbolicWidths::prepare(const ast::Statement& statement) {
    prepareIn(statement, nullptr);
}

void SymbolicWidths::prepareIn(const ast::Statement& statement,
                               const ast::SubroutineSymbol* subroutine) {
    switch (statement.kind) {
    case ast::StatementKind::List:
        for (const ast::Statement* item : statement.as<ast::StatementList>().list)
            prepareIn(*item, subroutine);
        break;
    case ast::StatementKind::Block:
        prepareIn(statement.as<ast::BlockStatement>().body, subroutine);
        break;
    case ast::StatementKind::ExpressionStatement:
        visit(statement.as<ast::ExpressionStatement>().expr, std::nullopt);
        break;
    case ast::StatementKind::Conditional: {
        const auto& conditional = statement.as<ast::ConditionalStatement>();
        for (const auto& condition : conditional.conditions) visit(*condition.expr, std::nullopt);
        prepareIn(conditional.ifTrue, subroutine);
        if (conditional.ifFalse != nullptr) prepareIn(*conditional.ifFalse, subroutine);
        break;
    }
    case ast::StatementKind::Case: {
        // The selector and every item are sized to the widest of them.
        const auto& caseStatement = statement.as<ast::CaseStatement>();
        std::optional<Width> common = self(caseStatement.expr);
        for (const auto& item : caseStatement.items) {
            for (const ast::Expression* expression : item.expressions) {
                const std::optional<Width> width = self(*expression);
                common = common.has_value() && width.has_value()
                             ? std::optional(max(*common, *width))
                             : std::nullopt;
            }
        }
        inContext(caseStatement.expr, common);
        for (const auto& item : caseStatement.items) {
            for (const ast::Expression* expression : item.expressions)
                inContext(*expression, common);
            prepareIn(*item.stmt, subroutine);
        }
        if (caseStatement.defaultCase != nullptr) prepareIn(*caseStatement.defaultCase, subroutine);
        break;
    }
    case ast::StatementKind::ForLoop: {
        const auto& loop = statement.as<ast::ForLoopStatement>();
        for (const ast::Expression* initializer : loop.initializers)
            visit(*initializer, std::nullopt);
        if (loop.stopExpr != nullptr) visit(*loop.stopExpr, std::nullopt);
        for (const ast::Expression* step : loop.steps) visit(*step, std::nullopt);
        prepareIn(loop.body, subroutine);
        break;
    }
    case ast::StatementKind::WhileLoop: {
        const auto& loop = statement.as<ast::WhileLoopStatement>();
        visit(loop.cond, std::nullopt);
        prepareIn(loop.body, subroutine);
        break;
    }
    case ast::StatementKind::DoWhileLoop: {
        const auto& loop = statement.as<ast::DoWhileLoopStatement>();
        visit(loop.cond, std::nullopt);
        prepareIn(loop.body, subroutine);
        break;
    }
    case ast::StatementKind::RepeatLoop: {
        const auto& loop = statement.as<ast::RepeatLoopStatement>();
        visit(loop.count, std::nullopt);
        prepareIn(loop.body, subroutine);
        break;
    }
    case ast::StatementKind::ForeverLoop:
        prepareIn(statement.as<ast::ForeverLoopStatement>().body, subroutine);
        break;
    case ast::StatementKind::ForeachLoop:
        prepareIn(statement.as<ast::ForeachLoopStatement>().body, subroutine);
        break;
    case ast::StatementKind::Timed:
        prepareIn(statement.as<ast::TimedStatement>().stmt, subroutine);
        break;
    case ast::StatementKind::Wait: {
        const auto& wait = statement.as<ast::WaitStatement>();
        visit(wait.cond, std::nullopt);
        prepareIn(wait.stmt, subroutine);
        break;
    }
    case ast::StatementKind::Return: {
        const auto& returned = statement.as<ast::ReturnStatement>();
        if (returned.expr != nullptr) {
            inContext(*returned.expr, subroutine != nullptr && subroutine->returnValVar != nullptr
                                          ? storage(*subroutine->returnValVar)
                                          : std::nullopt);
        }
        break;
    }
    case ast::StatementKind::VariableDeclaration: {
        const ast::VariableSymbol& variable = statement.as<ast::VariableDeclStatement>().symbol;
        if (const ast::Expression* initializer = variable.getInitializer())
            inContext(*initializer, storage(variable));
        break;
    }
    default: break;
    }
}

void SymbolicWidths::prepare(const ast::SubroutineSymbol& subroutine) {
    prepareIn(subroutine.getBody(), &subroutine);
}

std::optional<Width> SymbolicWidths::of(const ast::Expression& expression) {
    if (const auto found = prepared.find(&expression); found != prepared.end())
        return found->second;
    if (expression.type == nullptr) return std::nullopt;
    const std::optional<std::uint32_t> expected = numeric(*expression.type);
    if (!expected.has_value()) return std::nullopt;
    return checked(self(expression), *expected);
}

} // namespace enzan::frontend::sv
