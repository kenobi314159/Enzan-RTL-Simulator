// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/ParameterExpression.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slang::ast {
class CallExpression;
class Expression;
class SubroutineSymbol;
class Symbol;
class Type;
} // namespace slang::ast

namespace enzan::frontend::sv {

/// The units of the constant functions generic units call (DESIGN_GUIDE.md section 18, M5.1),
/// and slang's evaluation of them for the frontend's own use.
///
/// The frontend evaluates a generic unit's expressions to check them against slang's numbers,
/// for the representative and for a second parameterisation, and has no VM to run a function's
/// unit with: it has slang evaluate the function instead, as slang evaluates a call.
class FunctionUnits : public eir::ConstantFunctions {
public:
    /// The name of the unit of the constant function `subroutine` (`isFunction`), building it
    /// with the call slang evaluated — `arguments`, concatenated first highest, and
    /// `returned` — as the call it is checked against; nothing when it cannot be one.
    virtual std::optional<std::string> unitFor(const slang::ast::SubroutineSymbol& subroutine,
                                               const std::optional<eir::BitVector>& arguments,
                                               const eir::BitVector& returned) = 0;

    /// Records that the unit `function` is `subroutine`'s, for `call`.
    void remember(std::string_view function, const slang::ast::SubroutineSymbol& subroutine);

    /// What slang's evaluation of the function returns for `arguments`, or nothing when it
    /// cannot evaluate it.
    std::optional<eir::BitVector> call(std::string_view function,
                                       const std::optional<eir::BitVector>& arguments,
                                       std::uint32_t width) override;

private:
    std::map<std::string, const slang::ast::SubroutineSymbol*, std::less<>> subroutines;
    std::map<std::pair<std::string, std::string>, eir::BitVector, std::less<>> results;
};

/// Turns a constant expression of a body into an expression over the formal parameters of the
/// unit it becomes (DESIGN_GUIDE.md section 18, M5.1).
///
/// slang has typed the expression for the representative parameterisation, so every operand
/// already has the width the language's sizing rules give it; what this adds is which leaves
/// are formals. A subexpression that names no formal is folded to its value, since it is the
/// same in every parameterisation, and only what depends on a formal is kept as operations.
///
/// An expression that cannot be said this way — the width of a type, a call of a function
/// nothing makes a unit of — gives nothing, and the unit it belongs to stays specialised.
class ParameterLowering {
public:
    /// `formalIndices` maps each parameter symbol of the body to its formal's index;
    /// `evaluationScope` is where a subexpression that names no formal is evaluated; and
    /// `functionUnits`, when given, makes a call of a constant function a `Call` of its unit.
    /// All must outlive the lowering.
    ParameterLowering(eir::ParameterExpressions& into,
                      const std::map<const slang::ast::Symbol*, std::uint32_t>& formalIndices,
                      const slang::ast::Symbol& evaluationScope,
                      FunctionUnits* functionUnits = nullptr)
        : table(&into),
          formals(&formalIndices),
          scope(&evaluationScope),
          functions(functionUnits) {}

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
    std::optional<std::uint32_t> lowerCall(const slang::ast::CallExpression& call);
    std::optional<std::uint32_t> convert(std::uint32_t node, const slang::ast::Type& from,
                                         const slang::ast::Type& to);

    eir::ParameterExpressions* table;
    const std::map<const slang::ast::Symbol*, std::uint32_t>* formals;
    const slang::ast::Symbol* scope;
    FunctionUnits* functions;
};

} // namespace enzan::frontend::sv
