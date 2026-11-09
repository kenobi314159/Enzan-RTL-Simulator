// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/DefParams.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/Type.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxRewriter.h"
#include "slang/syntax/SyntaxTree.h"

#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::frontend::sv {

namespace {

namespace syntax = slang::syntax;
using slang::parsing::Token;
using slang::parsing::TokenKind;

/// A parameter a `defparam` sets, and the value it sets it to.
struct Setting {
    Token parameter;
    const syntax::ExpressionSyntax* value = nullptr;
};

class DefParamMover : public syntax::SyntaxRewriter<DefParamMover> {
public:
    // A nested module is not entered: a defparam there stays.
    void handle(const syntax::ModuleDeclarationSyntax& module) { moveIn(module.members); }

private:
    /// Moves the defparams of the scope `members` declares onto its instances, and those of
    /// each generate block among them onto the block's own. A generate region is no scope: its
    /// members are the enclosing scope's.
    void moveIn(const syntax::SyntaxList<syntax::MemberSyntax>& members) {
        std::vector<const syntax::MemberSyntax*> scope;
        collect(members, scope);

        // The instances a defparam may be moved onto, by name: each alone in a statement with no
        // parameter value assignment, and without dimensions. A name declared twice is neither.
        std::map<std::string_view, const syntax::HierarchyInstantiationSyntax*> instances;
        std::set<std::string_view> unusable;
        for (const syntax::MemberSyntax* member : scope) {
            if (member->kind != syntax::SyntaxKind::HierarchyInstantiation) continue;
            const auto& statement = member->as<syntax::HierarchyInstantiationSyntax>();
            for (const syntax::HierarchicalInstanceSyntax* instance : statement.instances) {
                if (instance->decl == nullptr) continue;
                const std::string_view name = instance->decl->name.valueText();
                const bool alone = statement.parameters == nullptr
                                   && statement.instances.size() == 1
                                   && instance->decl->dimensions.empty();
                if (!alone || !instances.emplace(name, &statement).second) unusable.insert(name);
            }
        }
        for (const std::string_view name : unusable) instances.erase(name);

        std::map<const syntax::HierarchyInstantiationSyntax*, std::vector<Setting>> settings;
        for (const syntax::MemberSyntax* member : scope) {
            if (member->kind != syntax::SyntaxKind::DefParam) continue;
            std::vector<std::pair<const syntax::HierarchyInstantiationSyntax*, Setting>> moved;
            for (const syntax::DefParamAssignmentSyntax* assignment :
                 member->as<syntax::DefParamSyntax>().assignments) {
                const std::optional<std::pair<std::string_view, Token>> target =
                    targetOf(*assignment->name);
                const auto found =
                    target.has_value() ? instances.find(target->first) : instances.end();
                if (found == instances.end()) {
                    moved.clear();
                    break;
                }
                moved.emplace_back(found->second, Setting{.parameter = target->second,
                                                          .value = assignment->setter->expr});
            }
            if (moved.empty()) continue;
            remove(*member);
            for (auto& [statement, setting] : moved) settings[statement].push_back(setting);
        }

        for (const auto& [statement, list] : settings)
            replace(*statement, withSettings(*statement, list));
    }

    /// Adds to `scope` the members of the scope `members` declares, those of its generate
    /// regions included, and moves in each generate block among them.
    void collect(const syntax::SyntaxList<syntax::MemberSyntax>& members,
                 std::vector<const syntax::MemberSyntax*>& scope) {
        for (const syntax::MemberSyntax* member : members) {
            if (member->kind == syntax::SyntaxKind::GenerateRegion)
                collect(member->as<syntax::GenerateRegionSyntax>().members, scope);
            else if (!moveInGenerated(*member))
                scope.push_back(member);
        }
    }

    /// Moves in the generate blocks `node` holds, when it is a generate construct or a block
    /// of one: the branches of an `if` and of a `case`, and a loop's body. False for anything
    /// else. A branch of one member, with no `begin`, declares nothing a defparam could stand
    /// beside.
    bool moveInGenerated(const syntax::SyntaxNode& node) {
        switch (node.kind) {
        case syntax::SyntaxKind::GenerateBlock:
            moveIn(node.as<syntax::GenerateBlockSyntax>().members);
            return true;
        case syntax::SyntaxKind::IfGenerate: {
            const auto& branch = node.as<syntax::IfGenerateSyntax>();
            moveInGenerated(*branch.block);
            if (branch.elseClause != nullptr) moveInGenerated(*branch.elseClause->clause);
            return true;
        }
        case syntax::SyntaxKind::LoopGenerate:
            moveInGenerated(*node.as<syntax::LoopGenerateSyntax>().block);
            return true;
        case syntax::SyntaxKind::CaseGenerate:
            for (const syntax::CaseItemSyntax* item : node.as<syntax::CaseGenerateSyntax>().items) {
                if (item->kind == syntax::SyntaxKind::StandardCaseItem)
                    moveInGenerated(*item->as<syntax::StandardCaseItemSyntax>().clause);
                else if (item->kind == syntax::SyntaxKind::DefaultCaseItem)
                    moveInGenerated(*item->as<syntax::DefaultCaseItemSyntax>().clause);
            }
            return true;
        default: return false;
        }
    }

    /// The instance and the parameter `name` names, when it is `instance.parameter`.
    static std::optional<std::pair<std::string_view, Token>>
    targetOf(const syntax::NameSyntax& name) {
        if (name.kind != syntax::SyntaxKind::ScopedName) return std::nullopt;
        const auto& scoped = name.as<syntax::ScopedNameSyntax>();
        if (scoped.separator.kind != TokenKind::Dot
            || scoped.left->kind != syntax::SyntaxKind::IdentifierName
            || scoped.right->kind != syntax::SyntaxKind::IdentifierName)
            return std::nullopt;
        return std::pair{scoped.left->as<syntax::IdentifierNameSyntax>().identifier.valueText(),
                         scoped.right->as<syntax::IdentifierNameSyntax>().identifier};
    }

    /// `statement` with `#(.p(v), ...)` for the parameters `list` sets, in the order the
    /// defparams set them.
    syntax::HierarchyInstantiationSyntax&
    withSettings(const syntax::HierarchyInstantiationSyntax& statement,
                 const std::vector<Setting>& list) {
        std::vector<syntax::TokenOrSyntax> assignments;
        for (const Setting& setting : list) {
            if (!assignments.empty()) assignments.emplace_back(makeComma());
            // The value's syntax is the defparam's, which the rewritten tree takes over; the
            // factory asks for it mutable, to make itself its parent.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            auto* value = const_cast<syntax::ExpressionSyntax*>(setting.value);
            assignments.emplace_back(
                &factory.namedParamAssignment(makeToken(TokenKind::Dot, "."), setting.parameter,
                                              makeToken(TokenKind::OpenParenthesis, "("), value,
                                              makeToken(TokenKind::CloseParenthesis, ")")));
        }
        auto& parameters = factory.parameterValueAssignment(
            makeToken(TokenKind::Hash, "#"), makeToken(TokenKind::OpenParenthesis, "("),
            syntax::SeparatedSyntaxList<syntax::ParamAssignmentSyntax>(alloc, assignments),
            makeToken(TokenKind::CloseParenthesis, ")"));
        return factory.hierarchyInstantiation(statement.attributes, statement.type, &parameters,
                                              statement.instances, statement.semi);
    }
};

/// The defparams of the elaborated design, each with its target.
class DefParamFinder
    : public slang::ast::ASTVisitor<DefParamFinder, slang::ast::VisitFlags::Symbols> {
public:
    std::vector<MistypedDefParam> found;

    void handle(const slang::ast::DefParamSymbol& defparam) {
        const slang::ast::Symbol* target = defparam.getTarget();
        if (target == nullptr || target->kind != slang::ast::SymbolKind::Parameter) return;
        const auto& parameter = target->as<slang::ast::ParameterSymbol>();

        // Only an untyped parameter takes its type from its value.
        const syntax::DataTypeSyntax* typeSyntax = parameter.getDeclaredType()->getTypeSyntax();
        if (typeSyntax == nullptr || typeSyntax->kind != syntax::SyntaxKind::ImplicitType) return;
        const auto& implicit = typeSyntax->as<syntax::ImplicitTypeSyntax>();
        if (implicit.signing || !implicit.dimensions.empty()) return;

        const slang::ConstantValue& value = defparam.getValue();
        const slang::ast::Type& type = parameter.getType();
        if (!value.isInteger() || !type.isIntegral()) return;
        const std::uint32_t given = value.integer().getBitWidth();
        const std::uint64_t declared = type.getBitWidth();
        if (given == declared) return;
        found.push_back(MistypedDefParam{
            .location = defparam.location,
            .description = std::format("a defparam setting the untyped parameter '{}' to {} bits, "
                                       "where its default has {}, other than on an instance "
                                       "declared beside it",
                                       parameter.name, given, declared)});
    }
};

} // namespace

std::shared_ptr<slang::syntax::SyntaxTree>
moveDefParams(const std::shared_ptr<slang::syntax::SyntaxTree>& tree) {
    if (!tree->getMetadata().hasDefparams) return tree;
    return DefParamMover().transform(tree);
}

std::vector<MistypedDefParam> mistypedDefParams(const slang::ast::RootSymbol& root) {
    DefParamFinder finder;
    root.visit(finder);
    return std::move(finder.found);
}

} // namespace enzan::frontend::sv
