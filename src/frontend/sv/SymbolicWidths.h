// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/ParameterExpression.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace slang::ast {
class Expression;
class Scope;
class Statement;
class SubroutineSymbol;
class Type;
class ValueSymbol;
} // namespace slang::ast

namespace slang::syntax {
class ExpressionSyntax;
} // namespace slang::syntax

namespace enzan::frontend::sv {

/// A width as a generic unit holds it: a node of its expressions when a formal decides it, or
/// the number slang gave it when nothing does (DESIGN_GUIDE.md section 18, M5.1).
struct Width {
    std::uint32_t value = 0;
    std::uint32_t expression = eir::NoExpression;

    [[nodiscard]] bool isFixed() const noexcept { return expression == eir::NoExpression; }

    /// True when the two are the same width in every parameterisation: the same number, or
    /// the same expression. Two expressions that happen to agree are not known to.
    [[nodiscard]] bool sameAs(const Width& other) const noexcept {
        return isFixed() ? other.isFixed() && value == other.value : expression == other.expression;
    }
};

/// One unpacked dimension of a declaration, its bounds as widths are held.
struct Bounds {
    Width left;
    Width right;
};

/// The widths of a generic unit's declarations and expressions as expressions over its
/// formals (DESIGN_GUIDE.md section 18, M5.1).
///
/// A declaration's width comes from its dimensions, which are bound expressions over
/// parameters; an expression's from the sizing rules of IEEE 1800 section 11.6 applied to its
/// operands', which is what slang applied to the representative's numbers. Every width found is
/// checked against slang's number for the representative, so a rule this gets wrong for a
/// construct shows as a disagreement there rather than as a unit that is wrong elsewhere; one it
/// cannot say at all gives nothing, and the unit stays specialised.
class SymbolicWidths {
public:
    /// Lowers a bound constant expression into the unit's table, or gives nothing.
    using Lower = std::function<std::optional<std::uint32_t>(const slang::ast::Expression&)>;

    /// Binds a dimension's expression in a scope, for the declaration it belongs to.
    using Bind = std::function<const slang::ast::Expression*(const slang::syntax::ExpressionSyntax&,
                                                             const slang::ast::Scope&)>;

    /// The width a slang type has as the frontend stores it, or nothing for one it cannot.
    using Numeric = std::function<std::optional<std::uint32_t>(const slang::ast::Type&)>;

    /// `representative` holds the value of each formal for the parameterisation the unit is
    /// translated from. Everything given must outlive this.
    /// `functions` answers the constant function calls the expressions make.
    SymbolicWidths(eir::ParameterExpressions& table, Lower lower, Bind bind, Numeric numeric,
                   std::span<const std::optional<eir::ParameterValue>> representative,
                   eir::ConstantFunctions* functions = nullptr);

    /// The width of the packed part of `symbol`'s declared type, or nothing when a formal
    /// decides it in a way this cannot say.
    std::optional<Width> packed(const slang::ast::ValueSymbol& symbol);

    /// The packed dimensions of `symbol`, outermost first, or nothing as for `packed`; empty
    /// for a type with none of its own, such as `int`.
    std::optional<std::vector<Bounds>> packedBounds(const slang::ast::ValueSymbol& symbol);

    /// The unpacked dimensions of `symbol`, outermost first, or nothing as for `packed`.
    std::optional<std::vector<Bounds>> unpacked(const slang::ast::ValueSymbol& symbol);

    /// The width of `symbol`'s storage: its packed width times every unpacked dimension's size.
    std::optional<Width> storage(const slang::ast::ValueSymbol& symbol);

    /// The width `expression` has as slang typed it, the context it is used in included, or
    /// nothing. Expressions whose context was prepared get theirs; any other is taken as
    /// self-determined.
    std::optional<Width> of(const slang::ast::Expression& expression);

    /// Records the context every expression under `statement` is used in, which decides the
    /// width of a context-determined one. Called on a process's body before it is translated.
    void prepare(const slang::ast::Statement& statement);

    /// As `prepare`, for an expression used where it is `context` wide: an assignment's right
    /// hand side, a port connection.
    void prepare(const slang::ast::Expression& expression, std::optional<Width> context);

    /// As `prepare`, for the body of a subroutine about to be inlined, whose `return` is sized
    /// to what the subroutine returns.
    void prepare(const slang::ast::SubroutineSymbol& subroutine);

    // --- arithmetic on widths, for what the translator derives from them ------------------

    [[nodiscard]] Width fixed(std::uint32_t value) const { return Width{.value = value}; }

    Width max(const Width& left, const Width& right);
    Width sum(const Width& left, const Width& right);
    Width product(const Width& left, const Width& right);

    /// The number of elements `bounds` spans, `[left:right]` in either order.
    Width spanOf(const Bounds& bounds);

    /// The value of node `expression` for the representative, as an integer.
    std::optional<std::int64_t> representativeValue(std::uint32_t expression);

    /// A node holding `width`: its expression, or a constant of its number.
    std::uint32_t node(const Width& width);

private:
    void inContext(const slang::ast::Expression& expression, std::optional<Width> target);
    void prepareIn(const slang::ast::Statement& statement,
                   const slang::ast::SubroutineSymbol* subroutine);
    std::optional<Width> self(const slang::ast::Expression& expression);
    void visit(const slang::ast::Expression& expression, std::optional<Width> context);
    std::optional<Width> sizeOf(const slang::syntax::ExpressionSyntax* left,
                                const slang::syntax::ExpressionSyntax* right,
                                const slang::ast::Scope& scope, std::optional<Bounds>* bounds);
    std::optional<Width> boundOf(const slang::syntax::ExpressionSyntax& syntax,
                                 const slang::ast::Scope& scope);
    Width checked(std::optional<Width> width, std::uint32_t expected);

    eir::ParameterExpressions* table;
    Lower lowerExpression;
    Bind bindExpression;
    Numeric numeric;
    eir::ParameterEvaluator evaluator;

    /// The storage width of each symbol asked about, so that one is worked out once.
    std::map<const slang::ast::ValueSymbol*, std::optional<Width>> storages;

    /// The width of each expression whose context was prepared.
    std::map<const slang::ast::Expression*, Width> prepared;
    /// Set when a width could not be said; the unit is then not generic.
    bool failed = false;

public:
    [[nodiscard]] bool hasFailed() const noexcept { return failed; }
};

} // namespace enzan::frontend::sv
