// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/ParameterLowering.h"

#include "frontend/sv/SvIntConversion.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/types/Type.h"

#include <utility>

namespace enzan::frontend::sv {

namespace ast = slang::ast;

namespace {

using eir::Opcode;
using eir::ParameterNode;
using eir::ParameterNodeKind;

/// The EIR operation a binary operator is, and whether its operands are swapped to get it:
/// `a > b` is `b < a`.
std::optional<std::pair<Opcode, bool>> binaryOperation(ast::BinaryOperator op, bool leftSigned) {
    switch (op) {
    case ast::BinaryOperator::Add: return std::pair{Opcode::Add, false};
    case ast::BinaryOperator::Subtract: return std::pair{Opcode::Subtract, false};
    case ast::BinaryOperator::Multiply: return std::pair{Opcode::Multiply, false};
    case ast::BinaryOperator::Divide: return std::pair{Opcode::Divide, false};
    case ast::BinaryOperator::Mod: return std::pair{Opcode::Modulo, false};
    case ast::BinaryOperator::BinaryAnd: return std::pair{Opcode::And, false};
    case ast::BinaryOperator::BinaryOr: return std::pair{Opcode::Or, false};
    case ast::BinaryOperator::BinaryXor: return std::pair{Opcode::Xor, false};
    case ast::BinaryOperator::BinaryXnor: return std::pair{Opcode::Xnor, false};
    case ast::BinaryOperator::Equality: return std::pair{Opcode::Equal, false};
    case ast::BinaryOperator::Inequality: return std::pair{Opcode::NotEqual, false};
    case ast::BinaryOperator::CaseEquality: return std::pair{Opcode::CaseEqual, false};
    case ast::BinaryOperator::CaseInequality: return std::pair{Opcode::CaseNotEqual, false};
    case ast::BinaryOperator::WildcardEquality: return std::pair{Opcode::WildcardEqual, false};
    case ast::BinaryOperator::WildcardInequality: return std::pair{Opcode::WildcardNotEqual, false};
    case ast::BinaryOperator::LessThan: return std::pair{Opcode::LessThan, false};
    case ast::BinaryOperator::LessThanEqual: return std::pair{Opcode::LessEqual, false};
    case ast::BinaryOperator::GreaterThan: return std::pair{Opcode::LessThan, true};
    case ast::BinaryOperator::GreaterThanEqual: return std::pair{Opcode::LessEqual, true};
    case ast::BinaryOperator::LogicalAnd: return std::pair{Opcode::LogicalAnd, false};
    case ast::BinaryOperator::LogicalOr: return std::pair{Opcode::LogicalOr, false};
    case ast::BinaryOperator::LogicalShiftLeft:
    case ast::BinaryOperator::ArithmeticShiftLeft: return std::pair{Opcode::ShiftLeft, false};
    case ast::BinaryOperator::LogicalShiftRight: return std::pair{Opcode::ShiftRightLogical, false};
    case ast::BinaryOperator::ArithmeticShiftRight:
        return std::pair{leftSigned ? Opcode::ShiftRightArithmetic : Opcode::ShiftRightLogical,
                         false};
    case ast::BinaryOperator::Power: return std::pair{Opcode::Power, false};
    default: return std::nullopt;
    }
}

} // namespace

bool ParameterLowering::namesFormal(const ast::Expression& expression) const {
    bool found = false;
    expression.visit(ast::makeVisitor([&](auto&, const ast::NamedValueExpression& named) {
        if (formals->contains(&named.symbol)) found = true;
    }));
    return found;
}

std::optional<std::uint32_t> ParameterLowering::constant(const ast::Expression& expression) {
    if (expression.type == nullptr || !expression.type->isIntegral()) return std::nullopt;

    slang::ConstantValue value;
    if (const slang::ConstantValue* folded = expression.getConstant()) {
        value = *folded;
    } else {
        ast::EvalContext context(*scope);
        value = expression.eval(context);
    }
    if (!value.isInteger()) return std::nullopt;
    return table->constant(
        toBitVector(value.integer())
            .resized(expression.type->getBitWidth(), expression.type->isSigned()),
        expression.type->isSigned());
}

std::optional<std::uint32_t> ParameterLowering::convert(std::uint32_t node, const ast::Type& from,
                                                        const ast::Type& to) {
    if (!from.isIntegral() || !to.isIntegral()) return std::nullopt;
    const std::uint32_t width = to.getBitWidth();
    if (from.getBitWidth() == width && from.isSigned() == to.isSigned()) return node;

    // Widening uses the source's signedness, which the operand's node carries; an equal width
    // with another signedness is a copy that relabels it.
    return table->add(ParameterNode{.kind = from.getBitWidth() <= width
                                                ? ParameterNodeKind::Extend
                                                : ParameterNodeKind::Truncate,
                                    .isSigned = to.isSigned(),
                                    .width = width,
                                    .a = node});
}

std::optional<std::uint32_t> ParameterLowering::lowerAs(const ast::Expression& expression,
                                                        const ast::Type& type) {
    const std::optional<std::uint32_t> node = lower(expression);
    if (!node.has_value() || expression.type == nullptr) return std::nullopt;
    return convert(*node, *expression.type, type);
}

std::optional<std::uint32_t> ParameterLowering::lower(const ast::Expression& expression) {
    if (expression.type == nullptr || !expression.type->isIntegral()) return std::nullopt;
    if (!namesFormal(expression)) return constant(expression);

    const ast::Type& type = *expression.type;
    const std::uint32_t width = type.getBitWidth();
    const bool isSigned = type.isSigned();

    switch (expression.kind) {
    case ast::ExpressionKind::NamedValue: {
        const auto& named = expression.as<ast::NamedValueExpression>();
        return table->formal(formals->at(&named.symbol), width, isSigned);
    }

    case ast::ExpressionKind::Conversion: {
        const auto& conversion = expression.as<ast::ConversionExpression>();
        const std::optional<std::uint32_t> operand = lower(conversion.operand());
        if (!operand.has_value() || conversion.operand().type == nullptr) return std::nullopt;
        return convert(*operand, *conversion.operand().type, type);
    }

    case ast::ExpressionKind::UnaryOp: {
        const auto& unary = expression.as<ast::UnaryExpression>();
        const std::optional<std::uint32_t> operand = lower(unary.operand());
        if (!operand.has_value()) return std::nullopt;
        const auto apply = [&](Opcode opcode, std::uint32_t of) {
            return table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                            .opcode = opcode,
                                            .isSigned = isSigned,
                                            .width = width,
                                            .a = of});
        };
        switch (unary.op) {
        case ast::UnaryOperator::Plus: return operand;
        case ast::UnaryOperator::Minus: return apply(Opcode::Negate, *operand);
        case ast::UnaryOperator::BitwiseNot: return apply(Opcode::Not, *operand);
        case ast::UnaryOperator::LogicalNot: return apply(Opcode::LogicalNot, *operand);
        case ast::UnaryOperator::BitwiseAnd: return apply(Opcode::ReduceAnd, *operand);
        case ast::UnaryOperator::BitwiseOr: return apply(Opcode::ReduceOr, *operand);
        case ast::UnaryOperator::BitwiseXor: return apply(Opcode::ReduceXor, *operand);
        case ast::UnaryOperator::BitwiseNand:
            return apply(Opcode::Not, apply(Opcode::ReduceAnd, *operand));
        case ast::UnaryOperator::BitwiseNor:
            return apply(Opcode::Not, apply(Opcode::ReduceOr, *operand));
        case ast::UnaryOperator::BitwiseXnor:
            return apply(Opcode::Not, apply(Opcode::ReduceXor, *operand));
        default: return std::nullopt;
        }
    }

    case ast::ExpressionKind::BinaryOp: {
        const auto& binary = expression.as<ast::BinaryExpression>();
        if (binary.left().type == nullptr || !binary.left().type->isIntegral()) return std::nullopt;
        const std::optional<std::pair<Opcode, bool>> operation =
            binaryOperation(binary.op, binary.left().type->isSigned());
        const std::optional<std::uint32_t> left = lower(binary.left());
        const std::optional<std::uint32_t> right = lower(binary.right());
        if (!operation.has_value() || !left.has_value() || !right.has_value()) return std::nullopt;
        const auto [opcode, swapped] = *operation;
        return table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                        .opcode = opcode,
                                        .isSigned = isSigned,
                                        .operationSigned = binary.left().type->isSigned()
                                                           && binary.right().type->isSigned(),
                                        .width = width,
                                        .a = swapped ? *right : *left,
                                        .b = swapped ? *left : *right});
    }

    case ast::ExpressionKind::ConditionalOp: {
        const auto& conditional = expression.as<ast::ConditionalExpression>();
        if (conditional.conditions.size() != 1 || conditional.conditions.front().pattern != nullptr)
            return std::nullopt;
        const std::optional<std::uint32_t> condition = lower(*conditional.conditions.front().expr);
        const std::optional<std::uint32_t> whenTrue = lower(conditional.left());
        const std::optional<std::uint32_t> whenFalse = lower(conditional.right());
        if (!condition.has_value() || !whenTrue.has_value() || !whenFalse.has_value())
            return std::nullopt;
        return table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                        .opcode = Opcode::Conditional,
                                        .isSigned = isSigned,
                                        .width = width,
                                        .a = *condition,
                                        .b = *whenTrue,
                                        .c = *whenFalse});
    }

    case ast::ExpressionKind::Concatenation: {
        std::optional<std::uint32_t> result;
        for (const ast::Expression* operand :
             expression.as<ast::ConcatenationExpression>().operands()) {
            const std::optional<std::uint32_t> part = lower(*operand);
            if (!part.has_value()) return std::nullopt;
            result = result.has_value()
                         ? table->add(ParameterNode{
                               .kind = ParameterNodeKind::Concat, .a = *result, .b = *part})
                         : *part;
        }
        return result;
    }

    case ast::ExpressionKind::Replication: {
        const auto& replication = expression.as<ast::ReplicationExpression>();
        const std::optional<std::uint32_t> count = lower(replication.count());
        const std::optional<std::uint32_t> copy = lower(replication.concat());
        if (!count.has_value() || !copy.has_value()) return std::nullopt;
        return table->add(
            ParameterNode{.kind = ParameterNodeKind::Replicate, .a = *count, .b = *copy});
    }

    case ast::ExpressionKind::Call: {
        const auto& call = expression.as<ast::CallExpression>();
        if (!call.isSystemCall() || call.arguments().size() != 1) return std::nullopt;
        const std::string_view name = call.getSubroutineName();
        const std::optional<std::uint32_t> operand = lower(*call.arguments().front());
        if (!operand.has_value()) return std::nullopt;
        if (name == "$clog2") {
            return table->add(ParameterNode{.kind = ParameterNodeKind::Operation,
                                            .opcode = Opcode::CeilLog2,
                                            .isSigned = isSigned,
                                            .width = width,
                                            .a = *operand});
        }
        if (name == "$signed" || name == "$unsigned") {
            return convert(*operand, *call.arguments().front()->type, type);
        }
        return std::nullopt;
    }

    default: return std::nullopt;
    }
}

} // namespace enzan::frontend::sv
