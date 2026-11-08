// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "slang/text/SourceLocation.h"

namespace slang::ast {
class RootSymbol;
} // namespace slang::ast

namespace slang::syntax {
class SyntaxTree;
} // namespace slang::syntax

namespace enzan::frontend::sv {

/// `tree` with each `defparam` that sets parameters of instances its module declares turned
/// into those instances' parameter value assignments, or `tree` itself when it has none.
///
/// A parameter declared without a type takes the type of its final value, after any override
/// (IEEE 1800 section 6.20.2), and a `defparam` is one (section 23.10.1). slang types an
/// instance's `#(.p(v))` so, but a `defparam` only replaces the value and leaves the type the
/// default implies: a string longer than the default is cut to the default's width, as
/// Quartus's netlists set every cell's `operation_mode`, and a dual-port memory reads as
/// another. The assignment slang does type by the value.
///
/// A `defparam` is moved only where nothing changes but its form: every assignment in it names
/// `instance.parameter`, the instance is declared in the same scope, the module or a generate
/// block, alone in its statement, with no dimensions and no parameter value assignment of its
/// own. The value's syntax is moved with it, so slang reports on it where it is written. Any
/// other `defparam` stays, and `mistypedDefParams` finds those slang would read wrongly.
[[nodiscard]] std::shared_ptr<slang::syntax::SyntaxTree>
moveDefParams(const std::shared_ptr<slang::syntax::SyntaxTree>& tree);

/// A `defparam` left in the elaborated design whose value is of another width than the type its
/// untyped parameter's default implies, which slang reads at the default's width.
struct MistypedDefParam {
    slang::SourceLocation location;
    std::string description;
};

/// Each such `defparam` under `root`, described for a refusal.
[[nodiscard]] std::vector<MistypedDefParam> mistypedDefParams(const slang::ast::RootSymbol& root);

} // namespace enzan::frontend::sv
