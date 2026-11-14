// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/NonStandardPaths.h"

#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"

#include <any>
#include <variant>

namespace enzan::frontend::sv {

namespace {

/// Where the sources declare each parameter and localparam: the location of its name, which is
/// the location slang gives the symbol.
class ParameterCollector : public slang::syntax::SyntaxVisitor<ParameterCollector> {
public:
    explicit ParameterCollector(std::set<slang::SourceLocation>& into) : found(&into) {}

    void handle(const slang::syntax::ParameterDeclarationSyntax& declaration) {
        for (const slang::syntax::DeclaratorSyntax* declarator : declaration.declarators)
            found->insert(declarator->name.location());
    }

private:
    std::set<slang::SourceLocation>* found;
};

/// The bit width of a diagnostic's type argument, or nothing when the argument is no type.
std::optional<std::uint64_t> typeWidth(const slang::Diagnostic::Arg& arg) {
    const auto* custom = std::get_if<slang::Diagnostic::CustomArgType>(&arg);
    if (custom == nullptr) return std::nullopt;
    const auto* type = std::any_cast<const slang::ast::Type*>(&custom->second);
    if (type == nullptr || *type == nullptr) return std::nullopt;
    return (*type)->getBitWidth();
}

} // namespace

NonStandardPaths::NonStandardPaths(
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees,
    std::span<const slang::Diagnostic> diagnostics) {
    ParameterCollector collector(parameters);
    for (const std::shared_ptr<slang::syntax::SyntaxTree>& tree : trees)
        tree->root().visit(collector);
    for (const slang::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.code == slang::diag::SpecifyPathBadReference && namesParameter(diagnostic))
            excusedConditions.insert(diagnostic.location);
    }
}

bool NonStandardPaths::namesParameter(const slang::Diagnostic& diagnostic) const {
    return !diagnostic.notes.empty() && parameters.contains(diagnostic.notes.front().location);
}

std::optional<DiagCode> NonStandardPaths::excuses(const slang::Diagnostic& diagnostic) const {
    if (diagnostic.code == slang::diag::SpecifyPathBadReference) {
        if (excusedConditions.contains(diagnostic.location))
            return DiagCode::ParameterInPathCondition;
        return std::nullopt;
    }
    if (diagnostic.code == slang::diag::ParallelPathWidth && diagnostic.args.size() == 2) {
        if (typeWidth(diagnostic.args[0]) == 1) return DiagCode::ParallelPathFromOneBit;
    }
    return std::nullopt;
}

} // namespace enzan::frontend::sv
