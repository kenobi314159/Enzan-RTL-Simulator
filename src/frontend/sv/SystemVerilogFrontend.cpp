// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/SystemVerilogFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Module.h"
#include "eir/Verifier.h"
#include "frontend/sv/ProcessTranslator.h"
#include "frontend/sv/SvIntConversion.h"

#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <format>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace enzan::frontend::sv {

namespace ast = slang::ast;

namespace {

/// Strips the conversions slang wraps a port connection or an expression in, to reach the
/// signal reference underneath.
const ast::Expression& withoutConversions(const ast::Expression& expression) {
    const ast::Expression* current = &expression;
    while (current->kind == ast::ExpressionKind::Conversion)
        current = &current->as<ast::ConversionExpression>().operand();
    return *current;
}

/// Returns the *external* side of a port connection.
///
/// slang models a connection as an assignment between the port and what it is attached to,
/// and which side is which depends on the direction: an input flows into the port, an
/// output out of it. Picking the wrong side binds the instance to its own port, and the
/// design then behaves as though the connection were not there at all.
const ast::Expression& externalSideOf(const ast::Expression& expression,
                                      slang::ast::ArgumentDirection direction) {
    if (expression.kind != ast::ExpressionKind::Assignment) return withoutConversions(expression);

    const auto& assignment = expression.as<ast::AssignmentExpression>();
    const ast::Expression& external =
        direction == slang::ast::ArgumentDirection::Out ? assignment.left() : assignment.right();
    return withoutConversions(external);
}

/// Translates one analysed module body into an EIR module definition.
class ModuleBuilder final : public ModuleContext {
public:
    /// Shared state the builder needs from the analysis as a whole.
    struct Shared {
        eir::Design& design;
        DiagnosticEngine& diagnostics;
        enzan::SourceManager& sources;
        const slang::SourceManager& slangSources;

        /// Enzan buffer for each slang buffer, so that a slang location can be rendered
        /// against the same text Enzan read.
        ///
        /// Keyed by slang's own buffer identifier rather than by the file name it reports.
        /// The name is a rendering — slang shortens an absolute path to its last component —
        /// so a map keyed on it silently missed for every source named by an absolute path,
        /// which is what a build system passes. The identifier is what the two managers
        /// actually agree about.
        const std::map<slang::BufferID, BufferId>& buffers;
    };

    ModuleBuilder(Shared& sharedState, std::string name, const ast::InstanceBodySymbol& bodySymbol)
        : shared(&sharedState), body(&bodySymbol) {
        definitionValue.name = std::move(name);
        definitionValue.sourceName = std::string(bodySymbol.getDefinition().name);
        definitionValue.location = convertLocation(bodySymbol.getDefinition().location);
    }

    /// Collects declarations and ports. Instances and processes come after, because an
    /// instance's connections resolve against this module's objects.
    bool collectDeclarations();

    /// Records the port index of each port symbol, for the parent's connections.
    [[nodiscard]] const std::map<const ast::Symbol*, std::uint32_t>& portIndices() const {
        return portIndexBySymbol;
    }

    /// Translates the procedural blocks and continuous assignments.
    bool translateProcesses();

    /// Adds an instance of an already-translated module.
    bool addInstance(const ast::InstanceSymbol& instance, const std::string& targetName,
                     const std::map<const ast::Symbol*, std::uint32_t>& targetPorts);

    eir::ModuleDefinition take() { return std::move(definitionValue); }

    // --- ModuleContext -----------------------------------------------------

    std::optional<std::uint32_t> objectFor(const ast::ValueSymbol& symbol) override;

    std::optional<eir::Type> convertType(const ast::Type& type) override;

    SourceLocation convertLocation(slang::SourceLocation location) override;

    void reportUnsupported(slang::SourceLocation location, std::string_view what) override;

    [[nodiscard]] const eir::ModuleDefinition& definition() const override {
        return definitionValue;
    }

private:
    /// Walks a scope, collecting declarations and remembering what to translate later.
    bool collectScope(const ast::Scope& scope, const std::string& prefix);

    /// Returns a name not already used by an object in this module.
    std::string uniqueObjectName(std::string_view preferred);

    /// Records `type`'s enumeration in this definition and returns its index, or `NoEnum`
    /// when the type is not an enumeration.
    std::uint32_t enumFor(const ast::Type& type);

    /// Creates an intermediate object plus a continuous assignment for a port connection
    /// that is an expression rather than a plain signal.
    std::optional<std::uint32_t> materialiseConnection(const ast::InstanceSymbol& instance,
                                                       const ast::PortSymbol& port,
                                                       const ast::Expression& expression);

    /// Turns a block header's event control into a sensitivity list.
    /// Which kind of sensitivity a block header's timing control describes.
    enum class SensitivityForm : std::uint8_t {
        /// Not an event control at all, such as `always #5`. The body keeps it.
        NotAnEventControl,
        /// An explicit list, such as `@(posedge clk or negedge rst)`.
        Explicit,
        /// `@(*)`, whose list is everything the body reads.
        Implicit,
    };

    SensitivityForm extractSensitivity(const ast::TimingControl& timing,
                                       std::vector<eir::SensitivityEntry>& into);

    Shared* shared;
    const ast::InstanceBodySymbol* body;
    eir::ModuleDefinition definitionValue;

    std::map<const ast::ValueSymbol*, std::uint32_t> objectBySymbol;
    std::map<const ast::Symbol*, std::uint32_t> portIndexBySymbol;

    std::vector<std::pair<std::string, const ast::ProceduralBlockSymbol*>> proceduralBlocks;
    std::vector<const ast::ContinuousAssignSymbol*> continuousAssignments;

    bool failed = false;

public:
    /// Instances found while collecting, in declaration order.
    std::vector<const ast::InstanceSymbol*> instances;
};

std::string ModuleBuilder::uniqueObjectName(std::string_view preferred) {
    std::string candidate(preferred.empty() ? "unnamed" : preferred);
    if (!definitionValue.findObject(candidate).has_value()) return candidate;

    for (unsigned suffix = 1;; ++suffix) {
        std::string attempt = std::format("{}${}", candidate, suffix);
        if (!definitionValue.findObject(attempt).has_value()) return attempt;
    }
}

std::uint32_t ModuleBuilder::enumFor(const ast::Type& type) {
    if (!type.isEnum()) return eir::Object::NoEnum;

    const auto& enumeration = type.getCanonicalType().as<ast::EnumType>();

    eir::EnumDefinition definition;

    // The *declared* name, not the canonical type's: for `typedef enum ... state_t` this is
    // the alias, which is what the reader wrote and what they want in a tooltip. A bare
    // `enum { ... }` has no name and is left nameless, which is honest — its members still
    // have names, and those are the point.
    definition.name = std::string(type.name);

    for (const ast::EnumValueSymbol& member : enumeration.values()) {
        const slang::ConstantValue& value = member.getValue();

        // A member whose value is not an integer cannot happen in a well-formed enumeration,
        // and dropping it silently would give a display that names some values and not others
        // with nothing to say why. The whole enumeration is dropped instead: no names at all
        // reads as "this is a plain vector", which is at least true.
        if (!value.isInteger()) return eir::Object::NoEnum;

        definition.members.push_back(eir::EnumMember{
            .name = std::string(member.name),
            .value = toBitVector(value.integer()).resized(type.getBitWidth(), type.isSigned())});
    }

    if (definition.members.empty()) return eir::Object::NoEnum;

    return definitionValue.addEnum(std::move(definition));
}

std::optional<eir::Type> ModuleBuilder::convertType(const ast::Type& type) {
    // Packed types — vectors, packed structs, packed arrays, enums — are all integral to
    // slang and all behave as bit vectors, so they convert for free. Unpacked aggregates,
    // reals and strings do not, and are reported where they are used rather than here, so
    // that the diagnostic points at the user's line.
    if (!type.isIntegral()) return std::nullopt;

    const slang::bitwidth_t width = type.getBitWidth();
    if (width == 0) return std::nullopt;

    return eir::Type::bits(width, type.isSigned(), type.isFourState());
}

SourceLocation ModuleBuilder::convertLocation(slang::SourceLocation location) {
    if (!location) return {};

    const auto found = shared->buffers.find(location.buffer());
    if (found == shared->buffers.end()) return {};

    // slang reports a line and a column; Enzan's locations are byte offsets. Going through
    // the line start is exact because both managers hold the same bytes.
    const std::size_t line = shared->slangSources.getLineNumber(location);
    const std::size_t column = shared->slangSources.getColumnNumber(location);
    const SourceLocation start =
        shared->sources.lineStart(found->second, static_cast<std::uint32_t>(line));
    if (!start.valid()) return {};

    return start.advanced(static_cast<std::uint32_t>(column > 0 ? column - 1 : 0));
}

void ModuleBuilder::reportUnsupported(slang::SourceLocation location, std::string_view what) {
    failed = true;
    shared->diagnostics.report(DiagCode::UnsupportedConstruct, convertLocation(location), what);
}

std::optional<std::uint32_t> ModuleBuilder::objectFor(const ast::ValueSymbol& symbol) {
    const auto found = objectBySymbol.find(&symbol);
    if (found != objectBySymbol.end()) return found->second;

    const std::optional<eir::Type> type = convertType(symbol.getType());
    if (!type.has_value()) {
        reportUnsupported(
            symbol.location,
            std::format("the type of '{}', which is not a packed integral type", symbol.name));
        return std::nullopt;
    }

    eir::Object object;
    object.name = uniqueObjectName(symbol.name);
    object.type = *type;
    object.kind = symbol.kind == slang::ast::SymbolKind::Net ? eir::ObjectKind::Net
                                                             : eir::ObjectKind::Variable;
    object.location = convertLocation(symbol.location);
    object.enumIndex = enumFor(symbol.getType());

    // A declaration initialiser is a time-zero value when it is constant. A non-constant
    // one is a static initialisation that has to run, which needs the process machinery an
    // `initial` block gets and is reported rather than silently dropped.
    if (const ast::Expression* initializer = symbol.getInitializer(); initializer != nullptr) {
        ast::EvalContext evalContext(symbol);
        const slang::ConstantValue value = initializer->eval(evalContext);
        if (value.isInteger()) {
            // One shared conversion, because the X/Z encodings of slang and Enzan differ
            // and two copies of that mapping means one of them is wrong.
            const eir::BitVector initial = toBitVector(value.integer());
            object.initialValue = initial.resized(type->bitWidth(), type->isSigned());
        } else {
            reportUnsupported(
                symbol.location,
                std::format("the initialiser of '{}', which is not a constant", symbol.name));
            return std::nullopt;
        }
    }

    const std::uint32_t index = definitionValue.addObject(std::move(object));
    objectBySymbol.emplace(&symbol, index);
    return index;
}

bool ModuleBuilder::collectScope(const ast::Scope& scope, const std::string& prefix) {
    for (const ast::Symbol& member : scope.members()) {
        switch (member.kind) {
        case slang::ast::SymbolKind::Variable:
        case slang::ast::SymbolKind::Net: {
            const auto& value = member.as<ast::ValueSymbol>();
            if (!objectFor(value).has_value()) continue;
            // A hoisted declaration keeps its scope in its name, so that two generate
            // blocks declaring `q` do not collide and the user can still recognise it.
            if (!prefix.empty()) {
                const std::uint32_t index = objectBySymbol.at(&value);
                definitionValue.objects[index].name =
                    uniqueObjectName(prefix + std::string(value.name));
            }
            break;
        }

        case slang::ast::SymbolKind::Parameter: {
            // Recorded for display only: a parameterised module is already one definition per
            // parameterisation, so nothing here is used to *simulate* anything.
            //
            // A parameter whose value is not an integer — a `real`, a `string`, an unpacked
            // array — is skipped rather than reported. It is not being translated, so nothing
            // can be mistranslated; refusing the design over a `parameter string NAME` that
            // only names a log file would fail a design that simulates perfectly well today.
            // The cost of skipping it is that it is absent from a list, which is visible.
            const auto& parameter = member.as<ast::ParameterSymbol>();
            const slang::ConstantValue& value = parameter.getValue();
            if (!value.isInteger()) break;

            definitionValue.parameters.push_back(
                eir::Parameter{.name = prefix + std::string(parameter.name),
                               .value = toBitVector(value.integer()),
                               .isLocal = parameter.isLocalParam(),
                               .location = convertLocation(parameter.location)});
            break;
        }

        case slang::ast::SymbolKind::ProceduralBlock:
            proceduralBlocks.emplace_back(prefix, &member.as<ast::ProceduralBlockSymbol>());
            break;

        case slang::ast::SymbolKind::ContinuousAssign:
            continuousAssignments.push_back(&member.as<ast::ContinuousAssignSymbol>());
            break;

        case slang::ast::SymbolKind::Instance:
            instances.push_back(&member.as<ast::InstanceSymbol>());
            break;

        case slang::ast::SymbolKind::GenerateBlock: {
            // slang has already decided which generate branch is taken; an
            // uninstantiated one is the branch that was not, and must be skipped
            // rather than translated.
            const auto& block = member.as<ast::GenerateBlockSymbol>();
            if (block.isUninstantiated) break;
            if (!collectScope(block, prefix + std::string(block.name) + ".")) return false;
            break;
        }

        case slang::ast::SymbolKind::GenerateBlockArray: {
            const auto& array = member.as<ast::GenerateBlockArraySymbol>();
            for (const ast::GenerateBlockSymbol* entry : array.entries) {
                if (entry->isUninstantiated) continue;
                if (!collectScope(*entry, prefix + std::string(entry->name) + ".")) return false;
            }
            break;
        }

        case slang::ast::SymbolKind::StatementBlock:
            // Block-local variables. Collected so that they get readable names;
            // anything missed here is still created on demand when it is referenced.
            if (!collectScope(member.as<ast::StatementBlockSymbol>(), prefix)) return false;
            break;

        default:
            // Type aliases, genvars, modports and the rest carry no state.
            // A construct that does carry state and is not handled — a task, a class —
            // is reported when something uses it, which puts the diagnostic on the
            // user's line rather than on a declaration they may not have written.
            break;
        }
    }

    return true;
}

bool ModuleBuilder::collectDeclarations() {
    if (!collectScope(*body, "")) return false;

    for (const ast::Symbol* portSymbol : body->getPortList()) {
        if (portSymbol->kind != slang::ast::SymbolKind::Port) {
            reportUnsupported(portSymbol->location, "interface ports and multi-ports");
            continue;
        }

        const auto& port = portSymbol->as<ast::PortSymbol>();
        if (port.internalSymbol == nullptr) {
            reportUnsupported(port.location, "a port with no internal connection");
            continue;
        }

        const std::optional<std::uint32_t> objectIndex =
            objectFor(port.internalSymbol->as<ast::ValueSymbol>());
        if (!objectIndex.has_value()) continue;

        eir::PortDirection direction = eir::PortDirection::Input;
        switch (port.direction) {
        case slang::ast::ArgumentDirection::In: direction = eir::PortDirection::Input; break;
        case slang::ast::ArgumentDirection::Out: direction = eir::PortDirection::Output; break;
        case slang::ast::ArgumentDirection::InOut: direction = eir::PortDirection::InOut; break;
        case slang::ast::ArgumentDirection::Ref:
            reportUnsupported(port.location, "`ref` ports");
            continue;
        }

        portIndexBySymbol.emplace(portSymbol,
                                  static_cast<std::uint32_t>(definitionValue.ports.size()));
        definitionValue.ports.push_back(eir::Port{.name = std::string(port.name),
                                                  .direction = direction,
                                                  .objectIndex = *objectIndex,
                                                  .location = convertLocation(port.location)});
    }

    return !failed;
}

bool ModuleBuilder::translateProcesses() {
    for (const auto& [prefix, block] : proceduralBlocks) {
        eir::ProcessKind kind = eir::ProcessKind::Always;
        switch (block->procedureKind) {
        case slang::ast::ProceduralBlockKind::Initial: kind = eir::ProcessKind::Initial; break;
        case slang::ast::ProceduralBlockKind::Final: kind = eir::ProcessKind::Final; break;
        case slang::ast::ProceduralBlockKind::Always: kind = eir::ProcessKind::Always; break;
        case slang::ast::ProceduralBlockKind::AlwaysComb:
            kind = eir::ProcessKind::AlwaysComb;
            break;
        case slang::ast::ProceduralBlockKind::AlwaysLatch:
            kind = eir::ProcessKind::AlwaysLatch;
            break;
        case slang::ast::ProceduralBlockKind::AlwaysFF: kind = eir::ProcessKind::AlwaysFf; break;
        }

        bool inferSensitivity = false;
        const SourceLocation location = convertLocation(block->location);
        std::string name =
            std::format("{}@{}", toString(kind), shared->slangSources.getFileName(block->location));
        if (block->location) {
            name += ':';
            name += std::to_string(shared->slangSources.getLineNumber(block->location));
        }

        ProcessTranslator translator(*this, kind, std::move(name), location);

        // An event control on the block header is the process's sensitivity, not a
        // statement inside it: the scheduler re-arms the process, so the body must start
        // after the wait rather than with it.
        const ast::Statement* bodyStatement = &block->getBody();
        if (bodyStatement->kind == ast::StatementKind::Timed) {
            const auto& timed = bodyStatement->as<ast::TimedStatement>();
            std::vector<eir::SensitivityEntry> sensitivity;
            switch (extractSensitivity(timed.timing, sensitivity)) {
            case SensitivityForm::Explicit:
                translator.setSensitivity(std::move(sensitivity));
                bodyStatement = &timed.stmt;
                break;
            case SensitivityForm::Implicit:
                // `always @(*)`: the list is everything the body reads, which is only
                // known once it has been walked.
                inferSensitivity = true;
                bodyStatement = &timed.stmt;
                break;
            case SensitivityForm::NotAnEventControl: break;
            }
        }

        if (!translator.translateBody(*bodyStatement)) {
            failed = true;
            continue;
        }

        if (inferSensitivity) translator.inferSensitivityFromReads();

        definitionValue.processes.push_back(translator.take());
    }

    for (const ast::ContinuousAssignSymbol* assignment : continuousAssignments) {
        const SourceLocation location = convertLocation(assignment->location);
        ProcessTranslator translator(
            *this, eir::ProcessKind::ContinuousAssign,
            std::format("assign@{}:{}", shared->slangSources.getFileName(assignment->location),
                        shared->slangSources.getLineNumber(assignment->location)),
            location);

        if (!translator.translateContinuousAssignment(assignment->getAssignment())) {
            failed = true;
            continue;
        }

        definitionValue.processes.push_back(translator.take());
    }

    return !failed;
}

ModuleBuilder::SensitivityForm
ModuleBuilder::extractSensitivity(const ast::TimingControl& timing,
                                  std::vector<eir::SensitivityEntry>& into) {
    switch (timing.kind) {
    case ast::TimingControlKind::SignalEvent: {
        const auto& event = timing.as<ast::SignalEventControl>();
        if (event.iffCondition != nullptr) return SensitivityForm::NotAnEventControl;

        const ast::Expression& reference = withoutConversions(event.expr);
        if (reference.kind != ast::ExpressionKind::NamedValue
            && reference.kind != ast::ExpressionKind::HierarchicalValue) {
            // An event on an expression — `@(a & b)` — is legal and needs a temporary
            // the scheduler can watch. Left to the body, which reports it.
            return SensitivityForm::NotAnEventControl;
        }

        const std::optional<std::uint32_t> objectIndex =
            objectFor(reference.as<ast::ValueExpressionBase>().symbol);
        if (!objectIndex.has_value()) return SensitivityForm::NotAnEventControl;

        eir::EdgeKind edge = eir::EdgeKind::AnyChange;
        switch (event.edge) {
        case ast::EdgeKind::PosEdge: edge = eir::EdgeKind::Posedge; break;
        case ast::EdgeKind::NegEdge: edge = eir::EdgeKind::Negedge; break;
        case ast::EdgeKind::None:
        case ast::EdgeKind::BothEdges: edge = eir::EdgeKind::AnyChange; break;
        }

        into.push_back(eir::SensitivityEntry{.objectIndex = *objectIndex, .edge = edge});
        return SensitivityForm::Explicit;
    }

    case ast::TimingControlKind::EventList: {
        for (const ast::TimingControl* event : timing.as<ast::EventListControl>().events) {
            if (extractSensitivity(*event, into) != SensitivityForm::Explicit)
                return SensitivityForm::NotAnEventControl;
        }
        return SensitivityForm::Explicit;
    }

    case ast::TimingControlKind::ImplicitEvent: return SensitivityForm::Implicit;

    default: return SensitivityForm::NotAnEventControl;
    }
}

std::optional<std::uint32_t>
ModuleBuilder::materialiseConnection(const ast::InstanceSymbol& instance,
                                     const ast::PortSymbol& port,
                                     const ast::Expression& expression) {
    // An `inout` would need the assignment to run both ways at once, which is driver
    // resolution and therefore M5. Inputs and outputs each need it in one direction only.
    if (port.direction == slang::ast::ArgumentDirection::InOut) {
        reportUnsupported(expression.sourceRange.start(),
                          "an inout port connected to an expression rather than a signal");
        return std::nullopt;
    }

    const bool isInput = port.direction == slang::ast::ArgumentDirection::In;

    const std::optional<eir::Type> type =
        convertType(port.internalSymbol->as<ast::ValueSymbol>().getType());
    if (!type.has_value()) {
        reportUnsupported(port.location, "the type of this port");
        return std::nullopt;
    }

    eir::Object object;
    object.name = uniqueObjectName(std::format("{}${}", instance.name, port.name));
    object.type = *type;
    object.kind = eir::ObjectKind::Net;
    object.location = convertLocation(expression.sourceRange.start());
    const std::uint32_t objectIndex = definitionValue.addObject(std::move(object));

    ProcessTranslator translator(*this, eir::ProcessKind::ContinuousAssign,
                                 std::format("connection@{}:{}",
                                             shared->slangSources.getFileName(instance.location),
                                             shared->slangSources.getLineNumber(instance.location)),
                                 convertLocation(instance.location));

    // An input is computed into the port's storage; an output is copied out of it. Either
    // way the port keeps its own slot rather than being collapsed, and the extra delta cycle
    // the assignment costs is visible in the EIR rather than hidden in the elaborator.
    const bool translated = isInput ? translator.translateExpressionInto(objectIndex, expression)
                                    : translator.translateObjectInto(expression, objectIndex);
    if (!translated) {
        failed = true;
        return std::nullopt;
    }

    definitionValue.processes.push_back(translator.take());
    return objectIndex;
}

bool ModuleBuilder::addInstance(const ast::InstanceSymbol& instance, const std::string& targetName,
                                const std::map<const ast::Symbol*, std::uint32_t>& targetPorts) {
    eir::Instance result;
    result.name = std::string(instance.name);
    result.definitionName = targetName;
    result.location = convertLocation(instance.location);

    for (const ast::PortConnection* connection : instance.getPortConnections()) {
        const auto portIndex = targetPorts.find(&connection->port);
        if (portIndex == targetPorts.end()) {
            // A port the child could not translate; the diagnostic was already reported
            // there, and binding it here would only add noise.
            continue;
        }

        eir::PortConnection bound;
        bound.portIndex = portIndex->second;

        const ast::Expression* expression = connection->getExpression();
        if (expression == nullptr) {
            // Deliberately left unconnected. Elaboration warns and gives it its own
            // storage, so it reads as X.
            result.connections.push_back(bound);
            continue;
        }

        const auto& port = connection->port.as<ast::PortSymbol>();
        const ast::Expression& reference = externalSideOf(*expression, port.direction);

        std::optional<std::uint32_t> objectIndex;
        if (reference.kind == ast::ExpressionKind::NamedValue
            || reference.kind == ast::ExpressionKind::HierarchicalValue) {
            objectIndex = objectFor(reference.as<ast::ValueExpressionBase>().symbol);
        } else {
            objectIndex = materialiseConnection(instance, port, reference);
        }

        if (!objectIndex.has_value()) {
            failed = true;
            return false;
        }

        bound.objectIndex = *objectIndex;
        result.connections.push_back(bound);
    }

    definitionValue.instances.push_back(std::move(result));
    return true;
}

/// Walks the elaborated hierarchy, translating each distinct module body once.
class Walker {
public:
    Walker(eir::Design& target, ModuleBuilder::Shared sharedState)
        : design(&target), shared(sharedState) {}

    /// Translates `instance`'s body if it has not been seen, and returns its EIR name.
    std::optional<std::string> translate(const ast::InstanceSymbol& instance);

    [[nodiscard]] bool ok() const noexcept { return succeeded; }

private:
    struct BodyInfo {
        std::string name;
        std::map<const ast::Symbol*, std::uint32_t> portIndices;
    };

    /// Returns a module name not already used.
    ///
    /// slang caches one body per distinct parameterisation, so a parameterised module
    /// produces several. Each becomes its own EIR definition — which is what keeps
    /// parameters out of EIR entirely (see eir/Module.h) — and a numeric suffix
    /// distinguishes them. The suffixes are assigned in the order the hierarchy is walked,
    /// so they are the same on every run.
    std::string uniqueModuleName(std::string_view preferred);

    eir::Design* design;
    ModuleBuilder::Shared shared;
    std::map<const ast::InstanceBodySymbol*, BodyInfo> bodies;
    std::map<std::string, unsigned, std::less<>> nameCounters;
    bool succeeded = true;
};

std::string Walker::uniqueModuleName(std::string_view preferred) {
    std::string base(preferred);
    const unsigned count = nameCounters[base]++;
    if (count == 0) return base;
    return std::format("{}${}", base, count);
}

std::optional<std::string> Walker::translate(const ast::InstanceSymbol& instance) {
    const ast::InstanceBodySymbol& body = instance.body;

    const auto existing = bodies.find(&body);
    if (existing != bodies.end()) return existing->second.name;

    if (!instance.isModule()) {
        shared.diagnostics.report(DiagCode::UnsupportedConstruct, SourceLocation(),
                                  "interface and program instances");
        succeeded = false;
        return std::nullopt;
    }

    std::string name = uniqueModuleName(instance.getDefinition().name);
    ModuleBuilder builder(shared, name, body);

    if (!builder.collectDeclarations()) {
        succeeded = false;
        return std::nullopt;
    }

    // The name is reserved before recursing so that a hierarchy which reaches the same body
    // again — directly or through a cycle slang did not reject — terminates here rather
    // than recursing until the stack runs out.
    bodies.emplace(&body, BodyInfo{.name = name, .portIndices = builder.portIndices()});

    for (const ast::InstanceSymbol* child : builder.instances) {
        const std::optional<std::string> childName = translate(*child);
        if (!childName.has_value()) {
            succeeded = false;
            return std::nullopt;
        }

        const auto childInfo = bodies.find(&child->body);
        if (childInfo == bodies.end()) {
            succeeded = false;
            return std::nullopt;
        }

        if (!builder.addInstance(*child, *childName, childInfo->second.portIndices)) {
            succeeded = false;
            return std::nullopt;
        }
    }

    if (!builder.translateProcesses()) {
        succeeded = false;
        return std::nullopt;
    }

    design->addModule(builder.take());
    return name;
}

} // namespace

std::unique_ptr<eir::Design> analyse(const Options& options, SourceManager& sources,
                                     DiagnosticEngine& diagnostics) {
    if (options.files.empty()) {
        diagnostics.report(DiagCode::NoInputFiles, SourceLocation());
        return nullptr;
    }

    // Every file is read through Enzan's source manager and the text is then handed to
    // slang, so that a diagnostic from slang and one from elaboration quote the same bytes
    // and the same snippet. Reading each file twice would be slower and could disagree.
    slang::SourceManager slangSources;
    std::map<slang::BufferID, BufferId> buffers;
    std::vector<slang::SourceBuffer> slangBuffers;

    for (const std::filesystem::path& path : options.files) {
        const FileReadResult result = sources.readFile(path);
        if (!result.ok()) {
            reportFileReadFailure(diagnostics, path, result);
            return nullptr;
        }

        const slang::SourceBuffer buffer =
            slangSources.assignText(path.string(), sources.bufferText(result.buffer));
        buffers.emplace(buffer.id, result.buffer);
        slangBuffers.push_back(buffer);
    }

    slang::Bag bag;
    slang::parsing::PreprocessorOptions preprocessorOptions;
    preprocessorOptions.predefines = options.defines;
    preprocessorOptions.additionalIncludePaths = options.includeDirectories;
    bag.set(preprocessorOptions);

    slang::ast::CompilationOptions compilationOptions;
    for (const std::string& top : options.topModules) compilationOptions.topModules.insert(top);
    bag.set(compilationOptions);

    slang::ast::Compilation compilation(bag);
    for (const slang::SourceBuffer& buffer : slangBuffers)
        compilation.addSyntaxTree(slang::syntax::SyntaxTree::fromBuffer(buffer, slangSources, bag));

    // slang's diagnostics are dramatically better than most commercial tools', and
    // preserving that quality end to end is a genuine selling point rather than polish
    // (DESIGN_GUIDE.md section 10.6). They are translated into Enzan's engine so that the
    // transcript is uniform regardless of which phase produced a message.
    const slang::DiagnosticEngine slangEngine(slangSources);
    bool hasError = false;

    for (const slang::Diagnostic& diagnostic : compilation.getAllDiagnostics()) {
        const slang::DiagnosticSeverity severity = slang::getDefaultSeverity(diagnostic.code);
        if (severity == slang::DiagnosticSeverity::Ignored) continue;

        const std::string message = slangEngine.formatMessage(diagnostic);
        const SourceLocation location = [&] {
            if (!diagnostic.location) return SourceLocation();
            const auto found = buffers.find(diagnostic.location.buffer());
            if (found == buffers.end()) return SourceLocation();
            const SourceLocation start = sources.lineStart(
                found->second,
                static_cast<std::uint32_t>(slangSources.getLineNumber(diagnostic.location)));
            if (!start.valid()) return SourceLocation();
            const std::size_t column = slangSources.getColumnNumber(diagnostic.location);
            return start.advanced(static_cast<std::uint32_t>(column > 0 ? column - 1 : 0));
        }();

        switch (severity) {
        case slang::DiagnosticSeverity::Note:
            diagnostics.report(DiagCode::SystemVerilogNote, location, message);
            break;
        case slang::DiagnosticSeverity::Warning:
            diagnostics.report(DiagCode::SystemVerilogWarning, location, message);
            break;
        case slang::DiagnosticSeverity::Error:
        case slang::DiagnosticSeverity::Fatal:
            diagnostics.report(DiagCode::SystemVerilogError, location, message);
            hasError = true;
            break;
        case slang::DiagnosticSeverity::Ignored: break;
        }
    }

    if (hasError) return nullptr;

    const slang::ast::RootSymbol& root = compilation.getRoot();
    if (root.topInstances.empty()) {
        diagnostics.report(DiagCode::NoTopLevelFound, SourceLocation());
        return nullptr;
    }

    auto design = std::make_unique<eir::Design>();
    const ModuleBuilder::Shared shared{.design = *design,
                                       .diagnostics = diagnostics,
                                       .sources = sources,
                                       .slangSources = slangSources,
                                       .buffers = buffers};

    Walker walker(*design, shared);
    for (const slang::ast::InstanceSymbol* instance : root.topInstances) {
        if (!walker.translate(*instance).has_value()) return nullptr;
    }

    if (!walker.ok()) return nullptr;

    // A frontend's output is verified before it is accepted. Section 4.2's core invariant —
    // a process is a pure function over a declared slice of the arena — is exactly the kind
    // of property a translator gets quietly wrong, and catching it here means the defect is
    // a diagnostic rather than a mysterious wrong value.
    if (!eir::verify(*design, diagnostics)) return nullptr;

    return design;
}

} // namespace enzan::frontend::sv
