// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/FileNameArguments.h"

#include "slang/ast/types/Type.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"

#include <any>
#include <variant>

namespace enzan::frontend::sv {

namespace {

/// Where each `$fopen` call's arguments start, which is where slang reports a conversion of one.
class FileOpenCollector : public slang::syntax::SyntaxVisitor<FileOpenCollector> {
public:
    explicit FileOpenCollector(std::set<slang::SourceLocation>& into) : found(&into) {}

    void handle(const slang::syntax::InvocationExpressionSyntax& invocation) {
        if (invocation.left->kind == slang::syntax::SyntaxKind::SystemName
            && invocation.left->as<slang::syntax::SystemNameSyntax>().systemIdentifier.valueText()
                   == "$fopen"
            && invocation.arguments != nullptr) {
            for (const slang::syntax::ArgumentSyntax* argument : invocation.arguments->parameters)
                found->insert(argument->getFirstToken().location());
        }
        visitDefault(invocation);
    }

private:
    std::set<slang::SourceLocation>* found;
};

/// A diagnostic's type argument, or null when the argument is no type.
const slang::ast::Type* typeOf(const slang::Diagnostic::Arg& arg) {
    const auto* custom = std::get_if<slang::Diagnostic::CustomArgType>(&arg);
    if (custom == nullptr) return nullptr;
    const auto* type = std::any_cast<const slang::ast::Type*>(&custom->second);
    return type != nullptr ? *type : nullptr;
}

} // namespace

FileNameArguments::FileNameArguments(
    std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees) {
    FileOpenCollector collector(arguments);
    for (const std::shared_ptr<slang::syntax::SyntaxTree>& tree : trees)
        tree->root().visit(collector);
}

bool FileNameArguments::excuses(const slang::Diagnostic& diagnostic) const {
    // The conversion slang refuses is from the argument's type to `string`.
    if (diagnostic.code != slang::diag::NoImplicitConversion || diagnostic.args.size() < 2
        || !arguments.contains(diagnostic.location))
        return false;
    const slang::ast::Type* from = typeOf(diagnostic.args[0]);
    const slang::ast::Type* to = typeOf(diagnostic.args[1]);
    return from != nullptr && to != nullptr && from->isIntegral() && to->isString();
}

} // namespace enzan::frontend::sv
