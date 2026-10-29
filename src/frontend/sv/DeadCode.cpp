// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/DeadCode.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/NumericDiags.h"

#include <algorithm>

namespace enzan::frontend::sv {

namespace ast = slang::ast;

std::optional<ConstantTruth> constantTruth(const ast::Expression& condition,
                                           const ast::Symbol& scope) {
    ast::EvalContext context(scope);
    const slang::ConstantValue value = condition.eval(context);
    if (!value.isInteger()) return std::nullopt;

    // A condition is true when some bit is 1 (IEEE 1800 section 12.4), false when every bit is
    // 0, and unknown otherwise.
    const slang::logic_t truth = value.integer().reductionOr();
    if (truth.isUnknown()) return ConstantTruth::Unknown;
    return static_cast<bool>(truth) ? ConstantTruth::True : ConstantTruth::False;
}

namespace {

/// Where a statement or an expression is. slang gives the invalid node that wraps one no range
/// of its own, so a wrapper is where what it wraps is.
template<typename Node>
slang::SourceRange rangeOf(const Node& node) {
    const Node* current = &node;
    while (current->sourceRange.start() == current->sourceRange.end()) {
        const Node* child = nullptr;
        if constexpr (std::is_same_v<Node, ast::Statement>) {
            if (current->kind == ast::StatementKind::Invalid)
                child = current->template as<ast::InvalidStatement>().child;
        } else {
            if (current->kind == ast::ExpressionKind::Invalid)
                child = current->template as<ast::InvalidExpression>().child;
        }
        if (child == nullptr) break;
        current = child;
    }
    return current->sourceRange;
}

bool empty(slang::SourceRange range) {
    return range.start() == range.end();
}

/// Where the dead part of a conditional is: `ruledOut`'s own range when slang kept one. A part
/// slang could not bind may be its shared invalid node, with no range at all, and then it is
/// the rest of `whole` beside the live part, `first` when the dead part comes first. Erring
/// wide is safe: the live part is walked too, and an error there leaves an invalid node in
/// live code, which no dead range excuses.
slang::SourceRange deadPart(slang::SourceRange whole, slang::SourceRange ruledOut,
                            slang::SourceRange live, bool first) {
    if (!empty(ruledOut)) return ruledOut;
    if (empty(live)) return whole;
    return first ? slang::SourceRange(whole.start(), live.start())
                 : slang::SourceRange(live.end(), whole.end());
}

/// Records the dead branches and arms of every instance, and every invalid node in live code.
///
/// Every instance's own body, not one per definition: whether a branch is dead depends on the
/// parameters, which differ between instances.
///
/// Every node, the bad ones included: an invalid node, and an expression of the error type,
/// such as a `?:` whose dead arm slang could not bind. slang's visitor visits those only with
/// its `Bad` flag, and `AllGood | Bad` is no value of its flags' enumeration, which clang-tidy
/// rightly reports; so `visit` is this class's own, the base's without the check for bad nodes.
class Walker : public ast::ASTVisitor<Walker, ast::VisitFlags::AllGood> {
public:
    explicit Walker(DeadCode& into) : found(&into) {}

    template<typename T>
    void visit(const T& node) {
        if constexpr (requires { handle(node); })
            handle(node);
        else
            visitDefault(node);
    }

    // An instance slang found to duplicate another's body, parameterisation and connections alike,
    // has the same dead code as that one, which is walked where it is the body. Walking the
    // duplicate would make slang bind every one of them, which is the cost its instance cache
    // exists to avoid, and the one translation avoids too.
    void handle(const ast::InstanceSymbol& instance) {
        if (instance.getCanonicalBody() != nullptr) return;
        visitDefault(instance);
    }

    void handle(const ast::InstanceBodySymbol& body) {
        const ast::Symbol* const outer = scope;
        scope = &body;
        visitDefault(body);
        scope = outer;
    }

    void handle(const ast::ConditionalStatement& statement) {
        const std::optional<ConstantTruth> truth = conditionTruth(statement.conditions);
        if (!truth.has_value()) {
            visitDefault(statement);
            return;
        }

        // A condition that is not true takes the `else`, an unknown one included.
        const ast::Statement* const live =
            *truth == ConstantTruth::True ? &statement.ifTrue : statement.ifFalse;
        const ast::Statement* const ruledOut =
            *truth == ConstantTruth::True ? statement.ifFalse : &statement.ifTrue;
        statement.conditions.front().expr->visit(*this);
        if (ruledOut != nullptr)
            found->addDead(deadPart(statement.sourceRange, rangeOf(*ruledOut),
                                    live != nullptr ? rangeOf(*live) : slang::SourceRange(),
                                    ruledOut == &statement.ifTrue));
        if (live != nullptr) live->visit(*this);
    }

    void handle(const ast::ConditionalExpression& expression) {
        const std::optional<ConstantTruth> truth = conditionTruth(expression.conditions);

        // An unknown condition merges both arms, so neither is dead.
        if (!truth.has_value() || *truth == ConstantTruth::Unknown) {
            visitDefault(expression);
            return;
        }
        const bool isTrue = *truth == ConstantTruth::True;
        expression.conditions.front().expr->visit(*this);
        found->addDead(deadPart(expression.sourceRange,
                                rangeOf(isTrue ? expression.right() : expression.left()),
                                rangeOf(isTrue ? expression.left() : expression.right()), !isTrue));
        (isTrue ? expression.left() : expression.right()).visit(*this);
    }

    // slang makes a node that holds an invalid one invalid too, wrapping it, all the way up: an
    // `if` whose dead branch holds the error is itself an invalid statement. A wrapper is
    // where the error originates only when nothing it wraps was recorded already; otherwise
    // counting it would make the dead branch inside it count as live.
    void handle(const ast::InvalidStatement& statement) {
        invalid(rangeOf<ast::Statement>(statement), statement.child);
    }

    void handle(const ast::InvalidExpression& expression) {
        invalid(rangeOf<ast::Expression>(expression), expression.child);
    }

private:
    template<typename Node>
    void invalid(slang::SourceRange range, const Node* child) {
        const std::size_t deadBefore = found->deadCount();
        const std::size_t invalidBefore = found->liveInvalidCount();
        if (child != nullptr) child->visit(*this);
        if (found->deadCount() != deadBefore || found->liveInvalidCount() != invalidBefore) return;
        found->addLiveInvalid(range);
    }

    /// The truth of a single plain condition, or nothing; a condition with a pattern, or more
    /// than one, is never taken as constant.
    template<typename Condition>
    [[nodiscard]] std::optional<ConstantTruth>
    conditionTruth(std::span<const Condition> conditions) const {
        if (scope == nullptr || conditions.size() != 1 || conditions.front().pattern != nullptr)
            return std::nullopt;
        return constantTruth(*conditions.front().expr, *scope);
    }

    DeadCode* found;
    const ast::Symbol* scope = nullptr;
};

bool within(slang::SourceRange range, slang::SourceLocation location) {
    return range.start().buffer() == location.buffer() && range.contains(location);
}

} // namespace

DeadCode::DeadCode(const ast::RootSymbol& root) {
    Walker walker(*this);
    root.visit(walker);
}

bool DeadCode::excuses(const slang::Diagnostic& diagnostic) const {
    if (diagnostic.code != slang::diag::ValueMustBePositive
        && diagnostic.code != slang::diag::ReplicationZeroOutsideConcat)
        return false;

    const slang::SourceLocation location = diagnostic.location;
    const auto covers = [location](slang::SourceRange range) { return within(range, location); };
    return std::ranges::any_of(dead, covers) && std::ranges::none_of(liveInvalid, covers);
}

} // namespace enzan::frontend::sv
