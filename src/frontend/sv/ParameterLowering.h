// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/ParameterExpression.h"

#include <cstdint>
#include <map>
#include <optional>

namespace slang::ast {
class Expression;
class Symbol;
class Type;
} // namespace slang::ast

namespace enzan::frontend::sv {

/// Turns a constant expression of a body into an expression over the formal parameters of the
/// unit it becomes (DESIGN_GUIDE.md section 18, M5.1).
///
/// slang has typed the expression for the representative parameterisation, so every operand
/// already has the width the language's sizing rules give it; what this adds is which leaves
/// are formals. A subexpression that names no formal is folded to its value, since it is the
/// same in every parameterisation, and only what depends on a formal is kept as operations.
///
/// An expression that cannot be said this way — a call of a function, the width of a type —
/// gives nothing, and the unit it belongs to stays specialised.
class ParameterLowering {
public:
    /// `formalIndices` maps each parameter symbol of the body to its formal's index;
    /// `evaluationScope` is where a subexpression that names no formal is evaluated. All three
    /// must outlive the lowering.
    ParameterLowering(eir::ParameterExpressions& into,
                      const std::map<const slang::ast::Symbol*, std::uint32_t>& formalIndices,
                      const slang::ast::Symbol& evaluationScope)
        : table(&into), formals(&formalIndices), scope(&evaluationScope) {}

    /// The node `expression` is, or nothing.
    [[nodiscard]] std::optional<std::uint32_t> lower(const slang::ast::Expression& expression);

    /// The node `expression` is, converted as an assignment would convert it to `type`.
    [[nodiscard]] std::optional<std::uint32_t> lowerAs(const slang::ast::Expression& expression,
                                                       const slang::ast::Type& type);

    /// True when `expression` names a formal anywhere, and so differs between
    /// parameterisations.
    [[nodiscard]] bool namesFormal(const slang::ast::Expression& expression) const;

private:
    std::optional<std::uint32_t> constant(const slang::ast::Expression& expression);
    std::optional<std::uint32_t> convert(std::uint32_t node, const slang::ast::Type& from,
                                         const slang::ast::Type& to);

    eir::ParameterExpressions* table;
    const std::map<const slang::ast::Symbol*, std::uint32_t>* formals;
    const slang::ast::Symbol* scope;
};

} // namespace enzan::frontend::sv
