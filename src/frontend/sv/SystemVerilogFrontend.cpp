// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/SystemVerilogFrontend.h"

#include "common/Diagnostics.h"
#include "common/Names.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "eir/Module.h"
#include "eir/Verifier.h"
#include "frontend/sv/DeadCode.h"
#include "frontend/sv/DefParams.h"
#include "frontend/sv/FileNameArguments.h"
#include "frontend/sv/NonStandardPaths.h"
#include "frontend/sv/ProcessTranslator.h"
#include "frontend/sv/SvIntConversion.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SpecifySymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/PreprocessorDiags.h"
#include "slang/numeric/Time.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <bit>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
/// and which side is which depends on the direction: an input flows into the port, and what an
/// output or an inout is attached to is bound as an lvalue, the assignment's left. Picking the
/// wrong side binds the instance to its own port, and the design then behaves as though the
/// connection were not there at all.
const ast::Expression& externalSideOf(const ast::Expression& expression,
                                      slang::ast::ArgumentDirection direction) {
    if (expression.kind != ast::ExpressionKind::Assignment) return withoutConversions(expression);

    const auto& assignment = expression.as<ast::AssignmentExpression>();
    const ast::Expression& external =
        direction == slang::ast::ArgumentDirection::In ? assignment.right() : assignment.left();
    return withoutConversions(external);
}

/// slang's unit for a power of ten of a second, when it has one.
std::optional<slang::TimeUnit> timeUnitOf(std::int8_t exponent) {
    switch (exponent) {
    case 0: return slang::TimeUnit::Seconds;
    case -3: return slang::TimeUnit::Milliseconds;
    case -6: return slang::TimeUnit::Microseconds;
    case -9: return slang::TimeUnit::Nanoseconds;
    case -12: return slang::TimeUnit::Picoseconds;
    case -15: return slang::TimeUnit::Femtoseconds;
    default: return std::nullopt;
    }
}

/// The finest precision a time scale of the design declares, as a power of ten of a second: of
/// the bodies it instantiates, and of its packages and compilation units, whose subroutines'
/// delays count theirs.
class FinestPrecision : public ast::ASTVisitor<FinestPrecision, ast::VisitFlags::Symbols> {
public:
    explicit FinestPrecision(std::int8_t start) noexcept : finest(start) {}

    // An instance whose body duplicates another's is walked where that body is.
    void handle(const ast::InstanceSymbol& instance) {
        if (instance.getCanonicalBody() != nullptr) return;
        visitDefault(instance);
    }

    void handle(const ast::InstanceBodySymbol& body) {
        consider(body.getTimeScale());
        visitDefault(body);
    }

    void consider(const std::optional<slang::TimeScale>& scale) {
        if (scale.has_value()) finest = std::min(finest, timeExponentOf(scale->precision));
    }

    [[nodiscard]] std::int8_t value() const noexcept { return finest; }

private:
    std::int8_t finest;
};

/// The index part of an instance's name when it is an element of an instance array, `[2]` or
/// `[1][0]`, as IEEE 1800 section 23.3.3.5 names it; empty for an instance of its own.
std::string arraySuffixOf(const ast::InstanceSymbol& instance) {
    if (instance.arrayPath.empty()) return {};
    slang::SmallVector<slang::ConstantRange, 4> dimensions;
    instance.getArrayDimensions(dimensions);
    std::string suffix;
    for (std::size_t index = 0; index < instance.arrayPath.size() && index < dimensions.size();
         ++index) {
        suffix += std::format("[{}]", static_cast<std::int32_t>(instance.arrayPath[index])
                                          + dimensions[index].lower());
    }
    return suffix;
}

/// How many instances `array` holds, counting through its dimensions.
std::size_t countElements(const ast::InstanceArraySymbol& array) {
    std::size_t count = 0;
    for (const ast::Symbol* element : array.elements) {
        count += element->kind == slang::ast::SymbolKind::InstanceArray
                     ? countElements(element->as<ast::InstanceArraySymbol>())
                     : 1;
    }
    return count;
}

/// The first instance `array` holds, or null when it holds none.
const ast::InstanceSymbol* firstElement(const ast::InstanceArraySymbol& array) {
    for (const ast::Symbol* element : array.elements) {
        if (element->kind == slang::ast::SymbolKind::Instance)
            return &element->as<ast::InstanceSymbol>();
        if (const ast::InstanceSymbol* found =
                firstElement(element->as<ast::InstanceArraySymbol>()))
            return found;
    }
    return nullptr;
}

/// FNV-1a over `text`, in hexadecimal: stable across runs, builds and machines, which a std::hash
/// is not required to be, so that a name made of it is the same wherever it is made.
std::string digestOf(std::string_view text) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x0000'0100'0000'01b3ULL;
    }
    return std::format("{:016x}", hash);
}

/// What `expressions` name, for a diagnostic: each signal by its name, `'a', 'b'`, and anything
/// else as an expression.
std::string signalNames(std::span<const ast::Expression* const> expressions) {
    std::string names;
    for (const ast::Expression* expression : expressions) {
        if (!names.empty()) names += ", ";
        const ast::Symbol* symbol = expression->getSymbolReference();
        names += symbol != nullptr ? std::format("'{}'", symbol->name) : "an expression";
    }
    return names;
}

/// The name an instance has in the module holding it: its own, or its array's with its index.
std::string instanceName(const ast::InstanceSymbol& instance) {
    if (instance.arrayPath.empty()) return std::string(instance.name);
    return std::string(instance.getArrayName()) + arraySuffixOf(instance);
}

/// The name `symbol` has in the module that declares it, its generate blocks joined to it with
/// dots as `collectScope` names a hoisted object or an instance, and the body that module's
/// instance has; null for the top level's instance, which no body holds. Nothing when the
/// symbol is somewhere a name cannot reach, a package, or a subroutine for anything but its
/// static variables.
std::optional<std::string> nameWithinBody(const ast::Symbol& symbol,
                                          const ast::InstanceBodySymbol*& owner) {
    std::string prefix;
    std::string ownName = symbol.kind == slang::ast::SymbolKind::Instance
                              ? instanceName(symbol.as<ast::InstanceSymbol>())
                              : std::string(symbol.name);
    const ast::Scope* scope = symbol.getParentScope();
    while (scope != nullptr) {
        const ast::Symbol& parent = scope->asSymbol();
        switch (parent.kind) {
        case slang::ast::SymbolKind::InstanceBody:
            owner = &parent.as<ast::InstanceBodySymbol>();
            return prefix + ownName;

        case slang::ast::SymbolKind::Root:
        case slang::ast::SymbolKind::CompilationUnit:
            if (symbol.kind != slang::ast::SymbolKind::Instance) return std::nullopt;
            owner = nullptr;
            return ownName;

        case slang::ast::SymbolKind::GenerateBlock: {
            const auto& block = parent.as<ast::GenerateBlockSymbol>();
            const ast::Scope* above = block.getParentScope();
            if (above != nullptr
                && above->asSymbol().kind == slang::ast::SymbolKind::GenerateBlockArray) {
                const auto& array = above->asSymbol().as<ast::GenerateBlockArraySymbol>();
                const slang::SVInt* index = block.getArrayIndex();
                const std::string entry =
                    index != nullptr
                        ? std::format("{}[{}]", array.getExternalName(),
                                      index->toString(slang::LiteralBase::Decimal, false))
                        : array.getExternalName();
                prefix.insert(0, ".");
                prefix.insert(0, entry);
                scope = array.getParentScope();
            } else {
                prefix.insert(0, ".");
                prefix.insert(0, block.getExternalName());
                scope = above;
            }
            break;
        }

        // A static variable of a subroutine is named behind it, `f.count`, as IEEE 1800 section
        // 23.6 names it, so that two subroutines' `count` are two variables.
        case slang::ast::SymbolKind::Subroutine:
            if (!ast::ValueSymbol::isKind(symbol.kind)
                || staticSubroutineOf(symbol.as<ast::ValueSymbol>()) == nullptr)
                return std::nullopt;
            prefix.insert(0, ".");
            prefix.insert(0, parent.name);
            scope = parent.getParentScope();
            break;

        // A procedural block's locals are named without it, as collectScope names them, and an
        // element of an instance array carries its index in its own name already.
        case slang::ast::SymbolKind::StatementBlock:
        case slang::ast::SymbolKind::InstanceArray: scope = parent.getParentScope(); break;

        // A static property of a class declared in a module is that module's storage, named
        // with its class.
        case slang::ast::SymbolKind::ClassType:
            prefix.insert(0, "::");
            prefix.insert(0, parent.name);
            scope = parent.getParentScope();
            break;

        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

/// The instances from the top level down to the one whose body is `body`, by the names
/// `nameWithinBody` gives them.
std::optional<std::vector<std::string>> instanceChain(const ast::InstanceBodySymbol& body) {
    std::vector<std::string> chain;
    const ast::InstanceBodySymbol* current = &body;
    while (current != nullptr && current->parentInstance != nullptr) {
        const ast::InstanceBodySymbol* owner = nullptr;
        const std::optional<std::string> name = nameWithinBody(*current->parentInstance, owner);
        if (!name.has_value()) return std::nullopt;
        chain.push_back(*name);
        current = owner;
    }
    std::ranges::reverse(chain);
    return chain;
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

        /// Every package variable a module refers to, in the order they were first met, for
        /// the package definitions that hold them. Only those: a package may declare variables
        /// of types Enzan does not have, and one nothing uses should not stop a design.
        std::vector<const ast::ValueSymbol*>* packageVariables = nullptr;

        /// The run's time unit, as a power of ten of a second, which every delay counts.
        std::int8_t runTimeUnit = -12;
    };

    ModuleBuilder(Shared& sharedState, std::string name, const ast::InstanceBodySymbol& bodySymbol)
        : shared(&sharedState), body(&bodySymbol) {
        definitionValue.name = std::move(name);
        definitionValue.sourceName = std::string(bodySymbol.getDefinition().name);
        definitionValue.location = convertLocation(bodySymbol.getDefinition().location);
    }

    /// A builder for the definition that holds a package's variables, which has no body.
    ModuleBuilder(Shared& sharedState, const ast::PackageSymbol& packageSymbol)
        : shared(&sharedState), body(nullptr), package(&packageSymbol) {
        definitionValue.name = std::format("{}::", packageSymbol.name);
        definitionValue.sourceName = std::string(packageSymbol.name);
        definitionValue.location = convertLocation(packageSymbol.location);
        definitionValue.isPackage = true;
    }

    /// The package that declares `symbol` directly, or declares the subroutine it is a static
    /// variable of, if one does.
    static const ast::PackageSymbol* packageOf(const ast::ValueSymbol& symbol);

    /// The path from `from`'s instance to `name`, declared in `owner`, another instance's body.
    /// Through one of `from`'s interface ports, `@bus/data`, when one reaches that instance; from
    /// the root, `/other/reset`, when the two are under different top levels; otherwise up to the
    /// instance both share and down to the one declaring it, `core/state` or `../u_peer/ready`.
    /// Nothing when either instance has no name a path can use.
    static std::optional<std::string> pathTo(const ast::InstanceBodySymbol& from,
                                             const ast::InstanceBodySymbol& owner,
                                             const std::string& name);

    /// Collects declarations and ports. Instances and processes come after, because an
    /// instance's connections resolve against this module's objects.
    bool collectDeclarations();

    /// Records the port index of each port symbol, for the parent's connections.
    /// Records the EIR port index of each port, by name.
    ///
    /// By name and not by the `ast::Symbol*`: slang creates a separate port symbol for every
    /// instance, even instances of one parameterisation, so a map keyed on the pointer is only
    /// usable for the one body it was built from. Keying it by name is what lets one
    /// translation serve every instance of a module (see `Walker::bodies`) — and a pointer-keyed
    /// map shared between bodies does not fail loudly, it silently binds nothing and the design
    /// simulates to the wrong answer.
    [[nodiscard]] const std::map<std::string, std::uint32_t, std::less<>>& portIndices() const {
        return portIndexByName;
    }

    /// Translates the procedural blocks and continuous assignments.
    bool translateProcesses();

    /// Adds an instance of an already-translated module, named `prefix` and its own name.
    bool addInstance(const ast::InstanceSymbol& instance, const std::string& prefix,
                     const std::string& targetName,
                     const std::map<std::string, std::uint32_t, std::less<>>& targetPorts);

    eir::ModuleDefinition take() { return std::move(definitionValue); }

    // --- ModuleContext -----------------------------------------------------

    std::optional<std::uint32_t> objectFor(const ast::ValueSymbol& symbol) override;

    std::optional<eir::Type> convertType(const ast::Type& type) override;

    std::optional<eir::BitVector> constantBits(const slang::ConstantValue& value,
                                               const ast::Type& type) override;

    SourceLocation convertLocation(slang::SourceLocation location) override;

    void reportUnsupported(slang::SourceLocation location, std::string_view what) override;

    [[nodiscard]] const eir::ModuleDefinition& definition() const override {
        return definitionValue;
    }

    [[nodiscard]] const ast::Symbol& evaluationScope() const override { return *body; }

    [[nodiscard]] std::int8_t runTimeUnit() const override { return shared->runTimeUnit; }

    void reportRoundedDelay(slang::SourceLocation location, std::string_view delay,
                            std::string_view rounded) override;

    std::optional<std::uint32_t> addOverrideProcess(std::uint32_t objectIndex,
                                                    const ast::Expression& value, bool force,
                                                    slang::SourceLocation location) override;

    std::optional<std::uint32_t> addEventValue(const ast::Expression& expression) override;

    std::optional<std::uint32_t> sampledCopyOf(std::uint32_t objectIndex) override;

private:
    /// Walks a scope, collecting declarations and remembering what to translate later.
    bool collectScope(const ast::Scope& scope, const std::string& prefix);

    /// Declares the static variables of a subroutine, in `scope` and the blocks within it.
    void declareStatics(const ast::Scope& scope);

    /// Reads what a functional simulation needs of a specify block: its module paths, its pulse
    /// limits, and where its first timing check is.
    void collectSpecifyBlock(const ast::SpecifyBlockSymbol& block);

    /// Adds, for each output a module path with a delay reaches, the process that delays it, and
    /// the objects it and the body's own writes of the output use. False, having reported, for a
    /// path whose delay Enzan does not model.
    bool translateModulePaths();

    /// Sends the body's own writes of each output `translateModulePaths` delays to the object it
    /// gave the output's process to read, every process's but the first `pathProcesses` from
    /// `firstPathProcess`, which are the outputs' own.
    void redirectDelayedOutputs(std::size_t firstPathProcess, std::size_t pathProcesses);

    /// The object, bits and width a module path's terminal names: a port, or a constant select of
    /// one. Nothing, having reported, for any other.
    std::optional<OutputPaths::Source> pathTerminal(const ast::Expression& terminal);

    /// The EIR kind of a slang net kind, or nothing for one Enzan cannot resolve.
    static std::optional<eir::NetKind> convertNetKind(ast::NetType::NetKind kind);

    /// Records what `connection`, an interface port of `instance`, is connected to here.
    bool bindInterface(const ast::InstanceSymbol& instance, const ast::PortConnection& connection,
                       eir::Instance& result);

    /// Creates the object for a reference whose path is already known.
    std::optional<std::uint32_t> referenceThrough(const ast::ValueSymbol& symbol, std::string path,
                                                  eir::Type type);

    /// The strengths `strength` declares for 0 and for 1, strong where it declares none; nothing,
    /// having reported, for `highz`, which drives `z` where a strength would and no model Enzan
    /// reads uses.
    std::optional<eir::DriveStrength> driveStrengthOf(
        std::pair<std::optional<ast::DriveStrength>, std::optional<ast::DriveStrength>> strength,
        slang::SourceLocation location);

    /// The strengths a MOS switch passes from its data input: those every driver of the input
    /// in this module has, supply reduced to strong (IEEE 1800 section 28.13). Nothing, having
    /// reported, where this module does not fix them.
    std::optional<eir::DriveStrength> switchStrength(const ast::PrimitiveInstanceSymbol& gate);

    /// Collects the elements of an instance array, and of the arrays nested in it.
    void collectInstanceArray(const ast::InstanceArraySymbol& array, const std::string& prefix) {
        for (const ast::Symbol* element : array.elements) {
            if (element->kind == slang::ast::SymbolKind::InstanceArray) {
                collectInstanceArray(element->as<ast::InstanceArraySymbol>(), prefix);
            } else if (element->kind == slang::ast::SymbolKind::Instance) {
                instances.push_back(CollectedInstance{
                    .prefix = prefix, .symbol = &element->as<ast::InstanceSymbol>()});
            } else if (element->kind == slang::ast::SymbolKind::PrimitiveInstance) {
                collectPrimitiveInstance(element->as<ast::PrimitiveInstanceSymbol>());
            }
        }
    }

    /// Keeps an instance of a built-in gate or of a user-defined primitive to translate as the
    /// continuous assignment it is.
    void collectPrimitiveInstance(const ast::PrimitiveInstanceSymbol& instance) {
        if (instance.primitiveType.primitiveKind == ast::PrimitiveSymbol::UserDefined)
            primitives.push_back(&instance);
        else
            gates.push_back(&instance);
    }

    /// Returns a name not already used by an object in this module.
    std::string uniqueObjectName(std::string_view preferred);

    /// Records `type`'s enumeration in this definition and returns its index, or `NoEnum`
    /// when the type is not an enumeration.
    std::uint32_t enumFor(const ast::Type& type);

    /// True when `type` is as wide as `port`, so that elaboration can collapse the port onto a
    /// signal of it. A port whose type does not convert is left to the connection to report.
    bool hasPortWidth(const ast::PortSymbol& port, const eir::Type& type);

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

    SensitivityForm extractSensitivity(ProcessTranslator& translator,
                                       const ast::TimingControl& timing,
                                       std::vector<eir::SensitivityEntry>& into);

    Shared* shared;
    const ast::InstanceBodySymbol* body;
    /// The package whose variables this builder declares, for a package's builder.
    const ast::PackageSymbol* package = nullptr;
    eir::ModuleDefinition definitionValue;

    std::map<const ast::ValueSymbol*, std::uint32_t> objectBySymbol;

    /// The copy of each object a concurrent assertion reads sampled, by the object's index.
    std::map<std::uint32_t, std::uint32_t> sampledCopies;
    std::map<std::string, std::uint32_t, std::less<>> portIndexByName;

    std::vector<std::pair<std::string, const ast::ProceduralBlockSymbol*>> proceduralBlocks;
    std::vector<const ast::ContinuousAssignSymbol*> continuousAssignments;

    /// Instances of built-in gates, each a continuous assignment of its outputs.
    std::vector<const ast::PrimitiveInstanceSymbol*> gates;

    /// Instances of user-defined primitives, each a continuous assignment of its output.
    std::vector<const ast::PrimitiveInstanceSymbol*> primitives;

    /// The module paths of this body's specify blocks, and whether their pulse limits are zero,
    /// which lets every pulse through (`PATHPULSE$ = 0`).
    std::vector<const ast::TimingPathSymbol*> modulePaths;
    bool transportPaths = false;

    /// What else a specify block says of pulses, a limit of one path or other than zero, a
    /// pulse style or `showcancelled`: where it is and what, refused where a path has a delay.
    std::vector<std::pair<slang::SourceLocation, std::string>> pulseShapers;

    /// Each output a module path with a delay reaches, by its port's object, and the object the
    /// body's own writes of it write instead.
    std::map<std::uint32_t, std::uint32_t> delayedOutputs;

    /// The interface ports of this body, in declaration order.
    std::vector<const ast::InterfacePortSymbol*> interfacePorts;

    /// How `from` reaches `instance` through one of its interface ports: `@port`, or `@port[2]`
    /// when the port is an array and the instance one of its elements. Empty when no interface
    /// port reaches it.
    [[nodiscard]] static std::string portReaching(const ast::InstanceBodySymbol& from,
                                                  const ast::InstanceSymbol& instance) {
        for (const ast::Symbol* portSymbol : from.getPortList()) {
            if (portSymbol->kind != slang::ast::SymbolKind::InterfacePort) continue;
            const auto& port = portSymbol->as<ast::InterfacePortSymbol>();
            const ast::Symbol* connected = port.getConnection().first;
            if (connected == &instance) return std::format("@{}", port.name);
            if (connected != nullptr && connected->kind == slang::ast::SymbolKind::InstanceArray
                && containsElement(connected->as<ast::InstanceArraySymbol>(), instance)) {
                return std::format("@{}{}", port.name, arraySuffixOf(instance));
            }
        }
        return {};
    }

    /// The instance array, declared in some body, holding exactly the elements of `array`, or
    /// null: an array slice holds only some of them, and is not supported.
    static const ast::InstanceArraySymbol* declaredArrayOf(const ast::InstanceArraySymbol& array) {
        if (array.getParentScope() != nullptr) return &array;

        const ast::Symbol* leaf = &array;
        while (leaf->kind == slang::ast::SymbolKind::InstanceArray) {
            const auto& elements = leaf->as<ast::InstanceArraySymbol>().elements;
            if (elements.empty()) return nullptr;
            leaf = elements.front();
        }
        const ast::Scope* scope = leaf->getParentScope();
        const ast::InstanceArraySymbol* declared = nullptr;
        while (scope != nullptr
               && scope->asSymbol().kind == slang::ast::SymbolKind::InstanceArray) {
            declared = &scope->asSymbol().as<ast::InstanceArraySymbol>();
            scope = declared->getParentScope();
        }
        if (declared == nullptr || countElements(*declared) != countElements(array)) return nullptr;
        return declared;
    }

    static bool containsElement(const ast::InstanceArraySymbol& array,
                                const ast::InstanceSymbol& instance) {
        return std::ranges::any_of(array.elements, [&instance](const ast::Symbol* element) {
            if (element == &instance) return true;
            return element->kind == slang::ast::SymbolKind::InstanceArray
                   && containsElement(element->as<ast::InstanceArraySymbol>(), instance);
        });
    }

    /// Nets declared with an assignment, `wire w = a & b;`, which is a continuous assignment
    /// written in the declaration rather than a time-zero value.
    std::vector<const ast::NetSymbol*> netAssignments;

    bool failed = false;

public:
    /// An instance found while collecting, with the generate blocks it is inside.
    struct CollectedInstance {
        /// What its name begins with, as an object's does: `gen[0].` inside a generate block.
        std::string prefix;
        const ast::InstanceSymbol* symbol = nullptr;
    };

    /// Instances found while collecting, in declaration order.
    std::vector<CollectedInstance> instances;
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
    // `real`, `realtime` and `shortreal` are all a double, as Type::real says.
    if (type.isFloating()) return eir::Type::real();

    // A named event is a bit its trigger toggles, so that what waits on it sees it change.
    if (type.isEvent()) return eir::Type::bits(1, false, false);

    // Packed types — vectors, packed structs, packed arrays, enums — are all integral to
    // slang and all behave as bit vectors, so they convert for free.
    if (type.isIntegral()) {
        const slang::bitwidth_t width = type.getBitWidth();
        if (width == 0) return std::nullopt;
        return eir::Type::bits(width, type.isSigned(), type.isFourState());
    }

    // A fixed-size unpacked array and an unpacked struct are stored flat, as the vector of all
    // their bits: every operation the language allows on one as a whole — assignment,
    // equality, a port connection — is then the same operation on bits. A real among them is its
    // double's 64 bits, as a VHDL array of reals is, two-state, and equality compares it as a
    // real (ProcessTranslator::compareAggregates). Strings, unions, dynamic arrays and classes are
    // none of these, and are reported where they are used, so that the diagnostic points at the
    // user's line.
    const ast::Type& canonical = type.getCanonicalType();
    std::uint64_t width = 0;
    bool fourState = false;

    if (canonical.kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType) {
        const auto& array = canonical.as<ast::FixedSizeUnpackedArrayType>();
        const std::optional<eir::Type> element = convertType(array.elementType);
        if (!element.has_value()) return std::nullopt;
        width = std::uint64_t{element->bitWidth()} * array.range.width();
        fourState = element->isFourState();
    } else if (canonical.kind == slang::ast::SymbolKind::UnpackedStructType) {
        for (const ast::FieldSymbol* field : canonical.as<ast::UnpackedStructType>().fields) {
            const std::optional<eir::Type> member = convertType(field->getType());
            if (!member.has_value()) return std::nullopt;
            width += member->bitWidth();
            fourState = fourState || member->isFourState();
        }
    } else {
        return std::nullopt;
    }

    if (width == 0 || width > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return eir::Type::bits(static_cast<std::uint32_t>(width), false, fourState);
}

std::optional<eir::BitVector> ModuleBuilder::constantBits(const slang::ConstantValue& value,
                                                          const ast::Type& type) {
    const std::optional<eir::Type> converted = convertType(type);
    if (!converted.has_value()) return std::nullopt;

    // A real's bits are its double's, whatever kind of number the constant is.
    if (converted->isReal()) {
        double real = 0.0;
        if (value.isReal()) {
            real = value.real();
        } else if (value.isShortReal()) {
            real = static_cast<double>(value.shortReal());
        } else if (value.isInteger() && !value.integer().hasUnknown()) {
            real = value.integer().toDouble();
        } else {
            return std::nullopt;
        }
        return eir::BitVector::fromUInt64(std::bit_cast<std::uint64_t>(real), 64);
    }

    if (value.isInteger()) {
        // One shared conversion, because the X/Z encodings of slang and Enzan differ and two
        // copies of that mapping means one of them is wrong.
        return toBitVector(value.integer()).resized(converted->bitWidth(), type.isSigned());
    }
    if (!value.isUnpacked()) return std::nullopt;

    // The elements in order, the first one highest, as the storage lays them out.
    const ast::Type& canonical = type.getCanonicalType();
    const std::span<const slang::ConstantValue> elements = value.elements();
    eir::BitVector result = eir::BitVector::filled(converted->bitWidth(), eir::Logic::Zero);
    std::uint32_t offset = converted->bitWidth();
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const ast::Type* elementType = nullptr;
        if (canonical.kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType) {
            elementType = &canonical.as<ast::FixedSizeUnpackedArrayType>().elementType;
        } else if (canonical.kind == slang::ast::SymbolKind::UnpackedStructType) {
            const auto fields = canonical.as<ast::UnpackedStructType>().fields;
            if (index >= fields.size()) return std::nullopt;
            elementType = &fields[index]->getType();
        } else {
            return std::nullopt;
        }

        const std::optional<eir::BitVector> element = constantBits(elements[index], *elementType);
        if (!element.has_value() || element->width() > offset) return std::nullopt;
        offset -= element->width();
        eir::bitops::assignPartAt(result.mutableRef(), element->ref(), offset);
    }
    if (offset != 0) return std::nullopt;
    return result;
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

void ModuleBuilder::reportRoundedDelay(slang::SourceLocation location, std::string_view delay,
                                       std::string_view rounded) {
    shared->diagnostics.report(DiagCode::DelayRounded, convertLocation(location), delay, rounded);
}

std::optional<std::uint32_t> ModuleBuilder::addOverrideProcess(std::uint32_t objectIndex,
                                                               const ast::Expression& value,
                                                               bool force,
                                                               slang::SourceLocation location) {
    ProcessTranslator translator(*this, eir::ProcessKind::Override,
                                 std::format("{}@{}:{}", force ? "force" : "assign",
                                             shared->slangSources.getFileName(location),
                                             shared->slangSources.getLineNumber(location)),
                                 convertLocation(location));
    if (!translator.translateOverride(objectIndex, value)) return std::nullopt;

    // Appended while the statement's own process is still being translated, so its index is
    // fixed now and the statement can name it; processes are only ever appended.
    const auto index = static_cast<std::uint32_t>(definitionValue.processes.size());
    definitionValue.processes.push_back(translator.take());
    return index;
}

std::optional<std::uint32_t> ModuleBuilder::addEventValue(const ast::Expression& expression) {
    const slang::SourceLocation location = expression.sourceRange.start();
    const std::optional<eir::Type> type = convertType(*expression.type);
    if (!type.has_value()) {
        reportUnsupported(location, "an event control on an expression of this type");
        return std::nullopt;
    }
    eir::Object object;
    object.name = uniqueObjectName("event$value");
    object.type = *type;
    object.kind = eir::ObjectKind::Variable;
    object.location = convertLocation(location);
    const std::uint32_t objectIndex = definitionValue.addObject(std::move(object));

    ProcessTranslator translator(*this, eir::ProcessKind::ContinuousAssign,
                                 std::format("event@{}:{}",
                                             shared->slangSources.getFileName(location),
                                             shared->slangSources.getLineNumber(location)),
                                 convertLocation(location));
    if (!translator.translateExpressionInto(objectIndex, expression)) return std::nullopt;

    // Appended while the waiting process is still being translated, as an override's is, so
    // it comes before that process and has the value before the process first waits on it.
    definitionValue.processes.push_back(translator.take());
    return objectIndex;
}

std::optional<std::uint32_t> ModuleBuilder::sampledCopyOf(std::uint32_t objectIndex) {
    if (const auto found = sampledCopies.find(objectIndex); found != sampledCopies.end())
        return found->second;
    const eir::Object& source = definitionValue.objects[objectIndex];
    eir::Object object;
    object.name = uniqueObjectName("sampled$" + source.name);
    object.type = source.type;
    object.kind = eir::ObjectKind::Variable;
    object.location = source.location;
    const SourceLocation location = source.location;
    const std::uint32_t copy = definitionValue.addObject(std::move(object));

    // At the end of each time slot the object changed in, which leaves the copy holding the
    // value the next slot begins with until the object next changes.
    ProcessTranslator translator(*this, eir::ProcessKind::ContinuousAssign,
                                 "sampled@" + definitionValue.objects[objectIndex].name, location);
    if (!translator.translateCopyInto(copy, objectIndex)) return std::nullopt;
    eir::Process process = translator.take();
    process.postponed = true;
    definitionValue.processes.push_back(std::move(process));
    sampledCopies.emplace(objectIndex, copy);
    return copy;
}

const ast::PackageSymbol* ModuleBuilder::packageOf(const ast::ValueSymbol& symbol) {
    const ast::Scope* scope = symbol.getParentScope();
    if (staticSubroutineOf(symbol) != nullptr) {
        while (scope != nullptr
               && (scope->asSymbol().kind == slang::ast::SymbolKind::Subroutine
                   || scope->asSymbol().kind == slang::ast::SymbolKind::StatementBlock))
            scope = scope->asSymbol().getParentScope();
    }
    if (scope == nullptr || scope->asSymbol().kind != slang::ast::SymbolKind::Package)
        return nullptr;
    return &scope->asSymbol().as<ast::PackageSymbol>();
}

std::optional<std::uint32_t> ModuleBuilder::objectFor(const ast::ValueSymbol& symbol) {
    // A member reached through a modport is the modport's stand-in for the interface's own
    // variable or net, which is what has storage.
    if (symbol.kind == slang::ast::SymbolKind::ModportPort) {
        const ast::Symbol* internal = symbol.as<ast::ModportPortSymbol>().internalSymbol;
        if (internal == nullptr || !ast::ValueSymbol::isKind(internal->kind)) {
            reportUnsupported(
                symbol.location,
                std::format("the modport port '{}', which names no signal", symbol.name));
            return std::nullopt;
        }
        return objectFor(internal->as<ast::ValueSymbol>());
    }

    const auto found = objectBySymbol.find(&symbol);
    if (found != objectBySymbol.end()) return found->second;

    const std::optional<eir::Type> type = convertType(symbol.getType());
    if (!type.has_value()) {
        reportUnsupported(
            symbol.location,
            std::format("the type of '{}', which is not a packed integral type", symbol.name));
        return std::nullopt;
    }

    // A package variable is one variable however many modules use it, so a module refers to the
    // package's own, at the root, as it would to another instance's (IEEE 1800 section 26.2).
    // A static variable of a subroutine is named behind it, as nameWithinBody names it.
    const ast::SubroutineSymbol* const subroutine = staticSubroutineOf(symbol);
    std::string name(symbol.name);
    if (subroutine != nullptr) name = std::format("{}.{}", subroutine->name, symbol.name);

    const ast::PackageSymbol* const declaringPackage = packageOf(symbol);
    if (declaringPackage != nullptr && declaringPackage != package) {
        std::vector<const ast::ValueSymbol*>& used = *shared->packageVariables;
        if (std::ranges::find(used, &symbol) == used.end()) used.push_back(&symbol);
        return referenceThrough(symbol, std::format("/{}/{}", declaringPackage->name, name), *type);
    }

    // A symbol another instance declares is a hierarchical reference, a name here for that
    // instance's object, and gets no storage of its own: elaboration binds it. Giving it storage
    // would make a testbench reading `dut.state` read a copy nothing ever writes. A package's
    // builder declares the package's own variables, and has no body to compare against.
    if (declaringPackage == nullptr) {
        const ast::InstanceBodySymbol* owner = nullptr;
        const std::optional<std::string> nameThere = nameWithinBody(symbol, owner);
        if (!nameThere.has_value() || owner == nullptr) {
            reportUnsupported(symbol.location,
                              std::format("a reference to '{}', which is not declared in a module "
                                          "or a package",
                                          symbol.name));
            return std::nullopt;
        }
        // Within the module, a subroutine's static variable takes the generate blocks its
        // subroutine is in as well.
        if (subroutine != nullptr) name = *nameThere;
        if (owner != body) {
            std::optional<std::string> path = pathTo(*body, *owner, *nameThere);
            if (!path.has_value()) {
                reportUnsupported(symbol.location,
                                  std::format("the hierarchical reference to '{}'", symbol.name));
                return std::nullopt;
            }
            return referenceThrough(symbol, std::move(*path), *type);
        }
    }

    eir::Object object;
    object.name = uniqueObjectName(name);
    object.type = *type;
    object.kind = symbol.kind == slang::ast::SymbolKind::Net ? eir::ObjectKind::Net
                                                             : eir::ObjectKind::Variable;
    object.location = convertLocation(symbol.location);
    object.enumIndex = enumFor(symbol.getType());
    object.subroutineStatic = subroutine != nullptr;
    // A real's 64 bits are one double, which a waveform shows as the number rather than as bits.
    if (type->isReal()) object.encoding = eir::Encoding::Real;
    object.holdsReals = !type->isReal() && !realOffsets(*this, symbol.getType()).empty();

    // An array keeps its declared dimensions, outermost first, for what reads it as an array
    // rather than as the bits it is stored as: VPI, and the waveform database. The unpacked
    // ones come first, then the packed ones of their element.
    const ast::Type* level = &symbol.getType().getCanonicalType();
    while (level->kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType) {
        const auto& array = level->as<ast::FixedSizeUnpackedArrayType>();
        object.unpackedDimensions.push_back(
            eir::ArrayDimension{.left = array.range.left, .right = array.range.right});
        level = &array.elementType.getCanonicalType();
    }
    if (!object.unpackedDimensions.empty()) {
        if (const std::optional<eir::Type> element = convertType(*level)) {
            object.elementWidth = element->bitWidth();
            object.elementSigned = element->isSigned();
        }
    }
    while (level->kind == slang::ast::SymbolKind::PackedArrayType) {
        const auto& array = level->as<ast::PackedArrayType>();
        object.packedDimensions.push_back(
            eir::ArrayDimension{.left = array.range.left, .right = array.range.right});
        level = &array.elementType.getCanonicalType();
    }

    if (symbol.kind == slang::ast::SymbolKind::Net) {
        const auto& net = symbol.as<ast::NetSymbol>();
        const std::optional<eir::NetKind> netKind = convertNetKind(net.netType.netKind);
        if (!netKind.has_value()) {
            reportUnsupported(symbol.location,
                              std::format("a '{}' net", net.netType.name.empty()
                                                            ? std::string_view("user-defined")
                                                            : net.netType.name));
            return std::nullopt;
        }
        object.netKind = *netKind;
        // A net delay postpones every change by an inertial delay; dropping it would make the
        // net change early and the design simulate to an answer that looks plausible.
        if (net.getDelay() != nullptr) {
            reportUnsupported(symbol.location, "a delay on a net");
            return std::nullopt;
        }
        if (!driveStrengthOf(net.getDriveStrength(), symbol.location)) return std::nullopt;
    }

    // A net's initialiser is a continuous assignment, `wire w = a & b;`, translated with the
    // module's other continuous assignments. An automatic variable's runs where it is declared,
    // every time, and the process translator emits it there. A static variable's is a time-zero
    // value when it is constant. A non-constant one is a static initialisation that has to run,
    // which needs the process machinery an `initial` block gets and is reported rather than
    // silently dropped.
    const ast::Expression* initializer = symbol.getInitializer();
    const bool automatic =
        ast::VariableSymbol::isKind(symbol.kind)
        && symbol.as<ast::VariableSymbol>().lifetime == ast::VariableLifetime::Automatic;
    if (initializer != nullptr && symbol.kind == slang::ast::SymbolKind::Net) {
        netAssignments.push_back(&symbol.as<ast::NetSymbol>());
    } else if (initializer != nullptr && !automatic) {
        ast::EvalContext evalContext(symbol);
        const slang::ConstantValue value = initializer->eval(evalContext);
        if (std::optional<eir::BitVector> initial = constantBits(value, symbol.getType())) {
            object.initialValue = std::move(*initial);
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
                               .location = convertLocation(parameter.location),
                               .fromGenvar = parameter.isFromGenvar()});
            break;
        }

        case slang::ast::SymbolKind::ProceduralBlock:
            proceduralBlocks.emplace_back(prefix, &member.as<ast::ProceduralBlockSymbol>());
            break;

        case slang::ast::SymbolKind::ContinuousAssign:
            continuousAssignments.push_back(&member.as<ast::ContinuousAssignSymbol>());
            break;

        case slang::ast::SymbolKind::Instance:
            instances.push_back(
                CollectedInstance{.prefix = prefix, .symbol = &member.as<ast::InstanceSymbol>()});
            break;

        case slang::ast::SymbolKind::InstanceArray:
            // Each element is an instance of its own, named with its index.
            collectInstanceArray(member.as<ast::InstanceArraySymbol>(), prefix);
            break;

        case slang::ast::SymbolKind::PrimitiveInstance:
            collectPrimitiveInstance(member.as<ast::PrimitiveInstanceSymbol>());
            break;

        case slang::ast::SymbolKind::SpecifyBlock:
            collectSpecifyBlock(member.as<ast::SpecifyBlockSymbol>());
            break;

        case slang::ast::SymbolKind::NetAlias:
            // An alias makes its nets one, so that a driver of either drives both; left out,
            // each would see only its own drivers.
            reportUnsupported(
                member.location,
                std::format("the net alias of {}",
                            signalNames(member.as<ast::NetAliasSymbol>().getNetReferences())));
            break;

        case slang::ast::SymbolKind::GenerateBlock: {
            // slang has already decided which generate branch is taken; an
            // uninstantiated one is the branch that was not, and must be skipped
            // rather than translated.
            //
            // An unnamed block is named `genblk<n>`, as IEEE 1800 section 27.6 names it.
            const auto& block = member.as<ast::GenerateBlockSymbol>();
            if (block.isUninstantiated) break;
            if (!collectScope(block, prefix + block.getExternalName() + ".")) return false;
            break;
        }

        case slang::ast::SymbolKind::GenerateBlockArray: {
            // An iteration of a loop has no name of its own: it is the loop's name and its
            // index, `gen[0]`, which is how the standard names it and how slang paths it.
            const auto& array = member.as<ast::GenerateBlockArraySymbol>();
            const std::string arrayName = array.getExternalName();
            for (const ast::GenerateBlockSymbol* entry : array.entries) {
                if (entry->isUninstantiated) continue;
                const slang::SVInt* index = entry->getArrayIndex();
                const std::string entryName =
                    index != nullptr
                        ? std::format("{}[{}]", arrayName,
                                      index->toString(slang::LiteralBase::Decimal, false))
                        : arrayName;
                if (!collectScope(*entry, prefix + entryName + ".")) return false;
            }
            break;
        }

        case slang::ast::SymbolKind::StatementBlock:
            // Block-local variables. Collected so that they get readable names;
            // anything missed here is still created on demand when it is referenced.
            if (!collectScope(member.as<ast::StatementBlockSymbol>(), prefix)) return false;
            break;

        case slang::ast::SymbolKind::Subroutine:
            // A subroutine's static variables are storage of this module, which a call from
            // another instance, `dev.is_family(name)`, reaches by reference, as it reaches the
            // module's own: declared here, whether or not this module's own code calls it.
            declareStatics(member.as<ast::SubroutineSymbol>());
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

void ModuleBuilder::declareStatics(const ast::Scope& scope) {
    for (const ast::Symbol& member : scope.members()) {
        if (member.kind == slang::ast::SymbolKind::StatementBlock) {
            declareStatics(member.as<ast::StatementBlockSymbol>());
            continue;
        }
        if (member.kind != slang::ast::SymbolKind::Variable) continue;
        const auto& variable = member.as<ast::VariableSymbol>();

        // One of a type Enzan cannot hold is reported only where it is used, as the rest of what
        // an unused subroutine holds is.
        if (staticSubroutineOf(variable) == nullptr || !convertType(variable.getType()).has_value())
            continue;
        objectFor(variable);
    }
}

void ModuleBuilder::collectSpecifyBlock(const ast::SpecifyBlockSymbol& block) {
    for (const ast::Symbol& member : block.members()) {
        switch (member.kind) {
        case slang::ast::SymbolKind::TimingPath:
            modulePaths.push_back(&member.as<ast::TimingPathSymbol>());
            break;

        case slang::ast::SymbolKind::SystemTimingCheck:
            if (!definitionValue.firstTimingCheck.valid())
                definitionValue.firstTimingCheck = convertLocation(member.location);
            break;

        case slang::ast::SymbolKind::Specparam: {
            // Every other specparam is a constant, which an expression naming one folds. slang
            // marks only the form of two limits a pulse limit; one limit is both.
            const auto& specparam = member.as<ast::SpecparamSymbol>();
            if (!specparam.name.starts_with("PATHPULSE$")) break;
            const bool zero = specparam.isPathPulse
                                  ? specparam.getPulseRejectLimit().isFalse()
                                        && specparam.getPulseErrorLimit().isFalse()
                                  : specparam.getValue().isFalse();
            if (zero && specparam.name == "PATHPULSE$") {
                transportPaths = true;
            } else {
                pulseShapers.emplace_back(member.location,
                                          std::format("the pulse limit '{}'", specparam.name));
            }
            break;
        }

        case slang::ast::SymbolKind::PulseStyle:
            pulseShapers.emplace_back(member.location, "a pulse style or `showcancelled`");
            break;

        default: break;
        }
    }
}

std::optional<OutputPaths::Source> ModuleBuilder::pathTerminal(const ast::Expression& terminal) {
    const ast::Expression* base = &terminal;
    std::optional<std::int64_t> low;
    if (terminal.kind == ast::ExpressionKind::ElementSelect) {
        const auto& select = terminal.as<ast::ElementSelectExpression>();
        base = &select.value();
        ast::EvalContext context(*body);
        const slang::ConstantValue index = select.selector().eval(context);
        if (index.isInteger() && !index.integer().hasUnknown()) {
            const auto position = index.integer().as<std::int32_t>();
            if (position.has_value())
                low = std::int64_t{base->type->getFixedRange().translateIndex(*position)}
                      * terminal.type->getBitWidth();
        }
    } else if (terminal.kind == ast::ExpressionKind::RangeSelect) {
        const auto& select = terminal.as<ast::RangeSelectExpression>();
        base = &select.value();
        if (select.getSelectionKind() == ast::RangeSelectionKind::Simple) {
            ast::EvalContext context(*body);
            const slang::ConstantValue left = select.left().eval(context);
            const slang::ConstantValue right = select.right().eval(context);
            const slang::ConstantRange range = base->type->getFixedRange();
            if (left.isInteger() && right.isInteger()) {
                const auto first = left.integer().as<std::int32_t>();
                const auto second = right.integer().as<std::int32_t>();
                if (first.has_value() && second.has_value())
                    low = std::min(range.translateIndex(*first), range.translateIndex(*second));
            }
        }
    } else {
        low = 0;
    }
    if (base->kind != ast::ExpressionKind::NamedValue || !low.has_value()) {
        reportUnsupported(terminal.sourceRange.start(),
                          "a module path terminal other than a port or a constant select of one");
        return std::nullopt;
    }
    const std::optional<std::uint32_t> objectIndex =
        objectFor(base->as<ast::NamedValueExpression>().symbol);
    if (!objectIndex.has_value()) return std::nullopt;
    return OutputPaths::Source{.objectIndex = *objectIndex,
                               .lowBit = static_cast<std::uint32_t>(*low),
                               .width = terminal.type->getBitWidth()};
}

bool ModuleBuilder::translateModulePaths() {
    // The delays of a path, as constants: a path with every one zero changes nothing, though it
    // still takes part in choosing which path's delay applies.
    const auto valuesOf = [this](const ast::TimingPathSymbol& path) {
        std::vector<double> values;
        for (const ast::Expression* delay : path.getDelays()) {
            ast::EvalContext context(*body);
            const slang::ConstantValue value = delay->eval(context);
            double number = std::numeric_limits<double>::quiet_NaN();
            if (value.isReal()) number = static_cast<double>(value.real());
            if (value.isInteger()) number = value.integer().toDouble();
            values.push_back(number);
        }
        return values;
    };
    const auto zero = [](const std::vector<double>& values) {
        return std::ranges::all_of(values, [](double value) { return value == 0.0; });
    };
    if (std::ranges::all_of(
            modulePaths, [&](const ast::TimingPathSymbol* path) { return zero(valuesOf(*path)); }))
        return true;
    for (const auto& [location, what] : pulseShapers) reportUnsupported(location, what);
    if (!pulseShapers.empty()) return false;

    // One path of one input window to one bit of an output, as the bit's paths are compared.
    struct Arc {
        OutputPaths::Source source;
        const ast::TimingPathSymbol* path = nullptr;
        std::vector<double> delays;
    };

    std::map<std::uint32_t, std::map<std::uint32_t, std::vector<Arc>>> arcsByOutput;
    bool ok = true;
    for (const ast::TimingPathSymbol* path : modulePaths) {
        const std::vector<double> delays = valuesOf(*path);
        const std::size_t count = delays.size();
        // Twelve, which give the transitions to and from x their own, only when all one delay.
        const bool oneDelay =
            std::ranges::all_of(delays, [&](double value) { return value == delays.front(); });
        const bool modelled =
            count == 1 || count == 2 || count == 3 || count == 6 || (count == 12 && oneDelay);
        if (!modelled) {
            reportUnsupported(path->location, "a module path of twelve different delays");
            ok = false;
            continue;
        }
        if (path->getSyntax() != nullptr
            && path->getSyntax()->kind == slang::syntax::SyntaxKind::IfNonePathDeclaration
            && !zero(delays)) {
            reportUnsupported(path->location, "an `ifnone` module path with a delay");
            ok = false;
            continue;
        }
        for (const ast::Expression* output : path->getOutputs()) {
            const std::optional<OutputPaths::Source> to = pathTerminal(*output);
            if (!to.has_value()) {
                ok = false;
                continue;
            }
            for (const ast::Expression* input : path->getInputs()) {
                const std::optional<OutputPaths::Source> from = pathTerminal(*input);
                if (!from.has_value()) {
                    ok = false;
                    continue;
                }
                // A parallel path pairs the bits of its terminals; a full one joins each input
                // bit to every output bit, so a change of any is a change of the input.
                const bool parallel =
                    path->connectionKind == ast::TimingPathSymbol::ConnectionKind::Parallel;
                for (std::uint32_t bit = 0; bit < to->width; ++bit) {
                    OutputPaths::Source source = *from;
                    source.edge = path->edgeIdentifier;
                    if (parallel && from->width > 1) {
                        source.lowBit = from->lowBit + bit;
                        source.width = 1;
                    }
                    arcsByOutput[to->objectIndex][to->lowBit + bit].push_back(
                        Arc{.source = source, .path = path, .delays = delays});
                }
            }
        }
    }
    if (!ok) return false;

    // A process delaying `width` bits of the output from `lowBit` by `arcs`, the paths each of
    // them has, into the port from the body's copy of it; with no path, it copies them at once.
    const auto addPathProcess = [&](const eir::Object& port, std::uint32_t output,
                                    std::uint32_t internal, std::uint32_t lowBit,
                                    std::uint32_t width, const std::vector<Arc>& arcs,
                                    const ast::TimingPathSymbol& firstPath) {
        OutputPaths paths;
        paths.output = output;
        paths.internal = internal;
        paths.lowBit = lowBit;
        paths.width = width;
        paths.transport = transportPaths;
        for (const Arc& arc : arcs) {
            const auto found =
                std::ranges::find_if(paths.sources, [&](const OutputPaths::Source& known) {
                    return known.objectIndex == arc.source.objectIndex
                           && known.lowBit == arc.source.lowBit && known.width == arc.source.width
                           && known.edge == arc.source.edge;
                });
            const auto sourceIndex = static_cast<std::uint32_t>(found - paths.sources.begin());
            if (found == paths.sources.end()) paths.sources.push_back(arc.source);

            // The six transitions' delays from the one, two or three a path gives, as section
            // 30.5.1 extends them.
            const std::span<const ast::Expression* const> given = arc.path->getDelays();
            std::array<const ast::Expression*, 6> six{};
            switch (given.size()) {
            case 2: six = {given[0], given[1], given[0], given[0], given[1], given[1]}; break;
            case 3: six = {given[0], given[1], given[2], given[0], given[2], given[1]}; break;
            case 6: std::ranges::copy(given, six.begin()); break;
            default: six.fill(given[0]); break;
            }
            paths.paths.push_back(OutputPaths::Path{
                .source = sourceIndex, .condition = arc.path->getConditionExpr(), .delays = six});
        }

        const bool whole = lowBit == 0 && width == port.type.bitWidth();
        const std::string stem =
            whole ? port.name : std::format("{}[{}:{}]", port.name, lowBit + width - 1, lowBit);
        if (!arcs.empty()) {
            std::uint32_t seenWidth = 0;
            for (const OutputPaths::Source& source : paths.sources) seenWidth += source.width;
            const auto addState = [&](std::string_view suffix, eir::Type type) {
                eir::Object state;
                state.name = uniqueObjectName(std::format("{}${}", stem, suffix));
                state.type = type;
                state.location = port.location;
                return definitionValue.addObject(std::move(state));
            };
            paths.sourcesSeen = addState("seen", eir::Type::bits(seenWidth, false, true));
            paths.changedAt = addState(
                "changed", eir::Type::bits(static_cast<std::uint32_t>(64 * paths.sources.size()),
                                           false, false));
            paths.scheduled = addState(
                "scheduled",
                whole ? port.type : eir::Type::bits(width, false, port.type.isFourState()));
        }

        const ast::TimingPathSymbol& named = arcs.empty() ? firstPath : *arcs.front().path;
        ProcessTranslator translator(
            *this, eir::ProcessKind::ContinuousAssign,
            std::format("specify@{}:{}", shared->slangSources.getFileName(named.location),
                        shared->slangSources.getLineNumber(named.location)),
            convertLocation(named.location));
        if (!translator.translateModulePaths(paths)) return false;
        definitionValue.processes.push_back(translator.take());
        return true;
    };

    for (const auto& entry : arcsByOutput) {
        // Named rather than bound, since the lambdas below capture them.
        const std::uint32_t output = entry.first;
        const std::map<std::uint32_t, std::vector<Arc>>& arcsByBit = entry.second;

        // A copy, since the objects added below may move the table.
        const eir::Object port = definitionValue.objects[output];
        const bool hasDelay = std::ranges::any_of(arcsByBit, [&](const auto& entry) {
            return std::ranges::any_of(entry.second,
                                       [&](const Arc& arc) { return !zero(arc.delays); });
        });
        if (!hasDelay) continue;

        // Bits with the same paths share a process; a carry chain's bits each have their own,
        // and a parallel path between vectors gives each bit its own source.
        const auto sameArcs = [](const std::vector<Arc>& a, const std::vector<Arc>& b) {
            return std::ranges::equal(a, b, [](const Arc& x, const Arc& y) {
                return x.source.objectIndex == y.source.objectIndex
                       && x.source.lowBit == y.source.lowBit && x.source.width == y.source.width
                       && x.source.edge == y.source.edge && x.delays == y.delays
                       && (x.path->getConditionExpr() == nullptr)
                              == (y.path->getConditionExpr() == nullptr)
                       && (x.path->getConditionExpr() == nullptr
                           || x.path->getConditionExpr()->syntax->toString()
                                  == y.path->getConditionExpr()->syntax->toString());
            });
        };
        const ast::TimingPathSymbol& firstPath = *arcsByBit.begin()->second.front().path;
        const auto portIt =
            std::ranges::find(definitionValue.ports, output, &eir::Port::objectIndex);
        if (portIt == definitionValue.ports.end()
            || portIt->direction != eir::PortDirection::Output) {
            reportUnsupported(firstPath.location,
                              std::format("a module path with a delay to '{}', which is not an "
                                          "output",
                                          port.name));
            return false;
        }

        // The body's own writes of the output, its initial value among them, go to a copy
        // beside the port; the port is the processes' alone, and what everything else reads,
        // inside the body as well.
        eir::Object internal = port;
        internal.name = uniqueObjectName(std::format("{}$path", port.name));
        definitionValue.objects[output].initialValue.reset();
        const std::uint32_t internalIndex = definitionValue.addObject(std::move(internal));
        delayedOutputs.emplace(output, internalIndex);

        const std::vector<Arc> none;
        const auto arcsOf = [&](std::uint32_t bit) -> const std::vector<Arc>& {
            const auto found = arcsByBit.find(bit);
            return found == arcsByBit.end() ? none : found->second;
        };
        const std::uint32_t width = port.type.bitWidth();
        for (std::uint32_t low = 0; low < width;) {
            std::uint32_t end = low + 1;
            while (end < width && sameArcs(arcsOf(end), arcsOf(low))) ++end;
            if (!addPathProcess(port, output, internalIndex, low, end - low, arcsOf(low),
                                firstPath))
                return false;
            low = end;
        }
    }
    return true;
}

void ModuleBuilder::redirectDelayedOutputs(std::size_t firstPathProcess,
                                           std::size_t pathProcesses) {
    if (delayedOutputs.empty()) return;
    const auto redirected = [this](std::uint32_t objectIndex) {
        const auto found = delayedOutputs.find(objectIndex);
        return found == delayedOutputs.end() ? objectIndex : found->second;
    };
    for (std::size_t index = 0; index < definitionValue.processes.size(); ++index) {
        if (index >= firstPathProcess && index < firstPathProcess + pathProcesses) continue;
        eir::Process& process = definitionValue.processes[index];
        for (eir::WriteExtent& extent : process.writeSet)
            extent.objectIndex = redirected(extent.objectIndex);
        eir::normaliseWriteSet(process.writeSet);
        for (eir::BasicBlock& block : process.blocks) {
            for (eir::Instruction& instruction : block.instructions) {
                if (eir::isStore(instruction.opcode))
                    instruction.a = eir::Operand::object(redirected(instruction.a.index()));
            }
        }
        for (eir::SysTaskCall& call : process.sysCalls) {
            if (call.objectIndex != eir::SysTaskCall::NoObject)
                call.objectIndex = redirected(call.objectIndex);
        }
    }

    // What an instance drives of a port connected to the output is the body's too; what it
    // reads stays the port.
    for (eir::Instance& instance : definitionValue.instances) {
        for (eir::PortConnection& connection : instance.connections) {
            if (connection.connected()
                && connection.driveObjectIndex == eir::PortConnection::Unconnected
                && delayedOutputs.contains(connection.objectIndex))
                connection.driveObjectIndex = delayedOutputs.at(connection.objectIndex);
        }
    }
}

bool ModuleBuilder::collectDeclarations() {
    if (!collectScope(*body, "")) return false;

    for (const ast::Symbol* portSymbol : body->getPortList()) {
        if (portSymbol->kind == slang::ast::SymbolKind::InterfacePort) {
            interfacePorts.push_back(&portSymbol->as<ast::InterfacePortSymbol>());
            continue;
        }
        if (portSymbol->kind != slang::ast::SymbolKind::Port) {
            reportUnsupported(portSymbol->location, "multi-ports");
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
        if (definitionValue.objects[*objectIndex].type.isReal()) {
            reportUnsupported(port.location, std::format("the real port '{}'", port.name));
            continue;
        }
        if (definitionValue.objects[*objectIndex].holdsReals) {
            reportUnsupported(port.location,
                              std::format("the port '{}', which holds reals,", port.name));
            continue;
        }

        eir::PortDirection direction = eir::PortDirection::Input;
        switch (port.direction) {
        case slang::ast::ArgumentDirection::In: direction = eir::PortDirection::Input; break;
        case slang::ast::ArgumentDirection::Out: direction = eir::PortDirection::Output; break;
        case slang::ast::ArgumentDirection::InOut: direction = eir::PortDirection::InOut; break;
        case slang::ast::ArgumentDirection::Ref:
            reportUnsupported(port.location, "`ref` ports");
            continue;
        }

        portIndexByName.emplace(std::string(port.name),
                                static_cast<std::uint32_t>(definitionValue.ports.size()));
        definitionValue.ports.push_back(eir::Port{.name = std::string(port.name),
                                                  .direction = direction,
                                                  .objectIndex = *objectIndex,
                                                  .location = convertLocation(port.location)});
    }

    return !failed;
}

bool ModuleBuilder::translateProcesses() {
    // First, so that the body's own writes of an output translated after them can be sent where
    // the outputs' processes read them.
    const std::size_t firstPathProcess = definitionValue.processes.size();
    if (!translateModulePaths()) failed = true;
    const std::size_t pathProcesses = definitionValue.processes.size() - firstPathProcess;

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

        // A concurrent assertion the module declares is a process of its own that never ends,
        // checking its property at each tick of its clock; a labelled one is in a block of its
        // label's name.
        const ast::Statement* asserted = &block->getBody();
        if (asserted->kind == ast::StatementKind::Block)
            asserted = &asserted->as<ast::BlockStatement>().body;
        if (asserted->kind == ast::StatementKind::ConcurrentAssertion) {
            std::string assertionName =
                std::format("assert@{}", shared->slangSources.getFileName(block->location));
            if (block->location)
                assertionName +=
                    ':' + std::to_string(shared->slangSources.getLineNumber(block->location));
            ProcessTranslator translator(*this, eir::ProcessKind::Initial, std::move(assertionName),
                                         location);
            const ast::Scope* scope = block->getParentScope();
            if (scope == nullptr || !translator.translateConcurrentAssertion(*asserted, *scope)) {
                failed = true;
                continue;
            }
            definitionValue.processes.push_back(translator.take());
            continue;
        }

        ProcessTranslator translator(*this, kind, std::move(name), location);

        // An event control on the block header is the process's sensitivity, not a
        // statement inside it: the scheduler re-arms the process, so the body must start
        // after the wait rather than with it.
        const ast::Statement* bodyStatement = &block->getBody();
        if (bodyStatement->kind == ast::StatementKind::Timed) {
            const auto& timed = bodyStatement->as<ast::TimedStatement>();
            std::vector<eir::SensitivityEntry> sensitivity;
            switch (extractSensitivity(translator, timed.timing, sensitivity)) {
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
        const std::optional<eir::DriveStrength> strength =
            driveStrengthOf(assignment->getDriveStrength(), assignment->location);
        if (!strength) continue;

        const SourceLocation location = convertLocation(assignment->location);
        ProcessTranslator translator(
            *this, eir::ProcessKind::ContinuousAssign,
            std::format("assign@{}:{}", shared->slangSources.getFileName(assignment->location),
                        shared->slangSources.getLineNumber(assignment->location)),
            location);

        if (!translator.translateContinuousAssignment(assignment->getAssignment(),
                                                      assignment->getDelay())) {
            failed = true;
            continue;
        }

        definitionValue.processes.push_back(translator.take());
        definitionValue.processes.back().strength = *strength;
    }

    for (const ast::PrimitiveInstanceSymbol* gate : gates) {
        // `pullup` and `pulldown` drive at pull strength unless they say otherwise, and a MOS
        // switch at its input's.
        const std::string_view kind = gate->primitiveType.name;
        const bool pull = kind == "pullup" || kind == "pulldown";
        auto declared = gate->getDriveStrength();
        if (pull && !declared.first && !declared.second)
            declared = {ast::DriveStrength::Pull, ast::DriveStrength::Pull};
        const std::optional<eir::DriveStrength> strength =
            kind == "nmos" || kind == "pmos" || kind == "cmos"
                ? switchStrength(*gate)
                : driveStrengthOf(declared, gate->location);
        if (!strength) continue;

        ProcessTranslator translator(
            *this, eir::ProcessKind::ContinuousAssign,
            std::format("{}@{}:{}", gate->primitiveType.name,
                        shared->slangSources.getFileName(gate->location),
                        shared->slangSources.getLineNumber(gate->location)),
            convertLocation(gate->location));
        if (!translator.translateGate(*gate)) {
            failed = true;
            continue;
        }
        definitionValue.processes.push_back(translator.take());
        definitionValue.processes.back().strength = *strength;
    }

    for (const ast::PrimitiveInstanceSymbol* instance : primitives) {
        const std::optional<eir::DriveStrength> strength =
            driveStrengthOf(instance->getDriveStrength(), instance->location);
        if (!strength) continue;

        // A sequential primitive's inputs as it last took them, x until it first has, above its
        // state, which starts at its `initial` value or at x.
        std::optional<std::uint32_t> held;
        const ast::PrimitiveSymbol& primitive = instance->primitiveType;
        if (primitive.isSequential) {
            const auto width = static_cast<std::uint32_t>(instance->getPortConnections().size());
            eir::Object object;
            object.name = uniqueObjectName(std::format("{}$state", instance->name));
            object.type = eir::Type::bits(width, false, true);
            object.location = convertLocation(instance->location);
            eir::BitVector initial = eir::BitVector::filled(width, eir::Logic::Unknown);
            if (primitive.initVal != nullptr && primitive.initVal->isInteger()) {
                const slang::logic_t bit = primitive.initVal->integer()[0];
                if (!bit.isUnknown())
                    initial.set(0, bit.value != 0 ? eir::Logic::One : eir::Logic::Zero);
            }
            object.initialValue = std::move(initial);
            held = definitionValue.addObject(std::move(object));
        }

        ProcessTranslator translator(
            *this, eir::ProcessKind::ContinuousAssign,
            std::format("{}@{}:{}", primitive.name,
                        shared->slangSources.getFileName(instance->location),
                        shared->slangSources.getLineNumber(instance->location)),
            convertLocation(instance->location));
        if (!translator.translatePrimitive(*instance, held)) {
            failed = true;
            continue;
        }
        definitionValue.processes.push_back(translator.take());
        definitionValue.processes.back().strength = *strength;
    }

    // By index, because translating one initialiser may meet a net not yet seen and add it,
    // which would invalidate a range-for's iterators.
    // NOLINTNEXTLINE(modernize-loop-convert) — see above
    for (std::size_t index = 0; index < netAssignments.size(); ++index) {
        const ast::NetSymbol& net = *netAssignments[index];
        const std::uint32_t objectIndex = objectBySymbol.at(&net);
        ProcessTranslator translator(*this, eir::ProcessKind::ContinuousAssign,
                                     std::format("assign@{}:{}",
                                                 shared->slangSources.getFileName(net.location),
                                                 shared->slangSources.getLineNumber(net.location)),
                                     convertLocation(net.location));

        if (!translator.translateExpressionInto(objectIndex, *net.getInitializer())) {
            failed = true;
            continue;
        }

        // `tri (weak1, strong0) n = v` drives `n` with what the declaration says.
        definitionValue.processes.push_back(translator.take());
        if (const std::optional<eir::DriveStrength> strength =
                driveStrengthOf(net.getDriveStrength(), net.location))
            definitionValue.processes.back().strength = *strength;
    }

    redirectDelayedOutputs(firstPathProcess, pathProcesses);
    return !failed;
}

bool ModuleBuilder::bindInterface(const ast::InstanceSymbol& instance,
                                  const ast::PortConnection& connection, eir::Instance& result) {
    const ast::Symbol* connected = connection.getIfaceConn().first;
    std::string target;
    if (connected != nullptr && connected->kind == slang::ast::SymbolKind::Instance) {
        const auto& interface = connected->as<ast::InstanceSymbol>();
        const ast::InstanceBodySymbol* owner = nullptr;
        const std::optional<std::string> name = nameWithinBody(interface, owner);
        target = name.has_value() && owner == body ? *name : portReaching(*body, interface);
    } else if (connected != nullptr && connected->kind == slang::ast::SymbolKind::InstanceArray) {
        // A whole array, to an array port: what the elements are called here, or the port of
        // this body it arrived through. slang hands the connection over as an array of its own,
        // outside any scope, so the array is found through its elements.
        if (const ast::InstanceArraySymbol* array =
                declaredArrayOf(connected->as<ast::InstanceArraySymbol>())) {
            const ast::InstanceBodySymbol* owner = nullptr;
            const std::optional<std::string> name = nameWithinBody(*array, owner);
            if (name.has_value() && owner == body) {
                target = *name;
            } else {
                for (const ast::InterfacePortSymbol* port : interfacePorts) {
                    const ast::Symbol* reached = port->getConnection().first;
                    if (reached != nullptr && reached->kind == slang::ast::SymbolKind::InstanceArray
                        && declaredArrayOf(reached->as<ast::InstanceArraySymbol>()) == array)
                        target = std::format("@{}", port->name);
                }
            }
        }
    }
    if (target.empty()) {
        reportUnsupported(
            instance.location,
            std::format("the connection of interface port '{}' to {}", connection.port.name,
                        connected == nullptr
                            ? std::string("nothing")
                            : "a " + kindName(slang::ast::toString(connected->kind))));
        return false;
    }
    result.interfaceBindings.push_back(
        eir::InterfaceBinding{.port = std::string(connection.port.name), .target = target});
    return true;
}

std::optional<std::string> ModuleBuilder::pathTo(const ast::InstanceBodySymbol& from,
                                                 const ast::InstanceBodySymbol& owner,
                                                 const std::string& name) {
    // Reached through an interface port, the reference names the port rather than the
    // interface it happens to be connected to here: one definition serves every instance, and
    // each may be connected to a different one.
    if (owner.parentInstance != nullptr) {
        if (const std::string port = portReaching(from, *owner.parentInstance); !port.empty())
            return std::format("{}/{}", port, name);
    }

    const std::optional<std::vector<std::string>> here = instanceChain(from);
    const std::optional<std::vector<std::string>> there = instanceChain(owner);
    if (!here.has_value() || !there.has_value()) return std::nullopt;

    // Up from here to the instance both paths share, then down to the one declaring it.
    std::size_t common = 0;
    while (common < here->size() && common < there->size() && (*here)[common] == (*there)[common])
        ++common;

    // Into another top level, sharing none, from the root: no path climbs out of one top into
    // another, and one from the root is the same from every instance, at whatever depth it is.
    if (common == 0 && !there->empty()) {
        std::string path;
        for (const std::string& level : *there) path += "/" + level;
        return path + "/" + name;
    }

    std::string path;
    for (std::size_t level = common; level < here->size(); ++level) path += "../";
    for (std::size_t level = common; level < there->size(); ++level) path += (*there)[level] + "/";
    path += name;
    return path;
}

std::optional<std::uint32_t> ModuleBuilder::referenceThrough(const ast::ValueSymbol& symbol,
                                                             std::string path, eir::Type type) {
    eir::Object object;
    // A `$` keeps the made-up name out of the user's namespace, as a connection's is.
    object.name = uniqueObjectName(std::format("ref${}", path));
    object.type = type;
    object.kind = symbol.kind == slang::ast::SymbolKind::Net ? eir::ObjectKind::Net
                                                             : eir::ObjectKind::Variable;
    object.location = convertLocation(symbol.location);
    object.hierarchicalPath = std::move(path);
    object.subroutineStatic = staticSubroutineOf(symbol) != nullptr;

    const std::uint32_t index = definitionValue.addObject(std::move(object));
    objectBySymbol.emplace(&symbol, index);
    return index;
}

std::optional<eir::NetKind> ModuleBuilder::convertNetKind(ast::NetType::NetKind kind) {
    switch (kind) {
    case ast::NetType::Wire: return eir::NetKind::Wire;
    case ast::NetType::Tri: return eir::NetKind::Tri;
    case ast::NetType::WAnd: return eir::NetKind::WiredAnd;
    case ast::NetType::WOr: return eir::NetKind::WiredOr;
    case ast::NetType::TriAnd: return eir::NetKind::TriAnd;
    case ast::NetType::TriOr: return eir::NetKind::TriOr;
    case ast::NetType::Tri0: return eir::NetKind::Tri0;
    case ast::NetType::Tri1: return eir::NetKind::Tri1;
    case ast::NetType::Supply0: return eir::NetKind::Supply0;
    case ast::NetType::Supply1: return eir::NetKind::Supply1;
    case ast::NetType::UWire: return eir::NetKind::UWire;
    // A trireg stores charge, an interconnect has no type of its own, and a user-defined
    // nettype resolves with a function of its own; none of them is a table Enzan has.
    case ast::NetType::TriReg:
    case ast::NetType::Interconnect:
    case ast::NetType::UserDefined:
    case ast::NetType::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<eir::DriveStrength> ModuleBuilder::driveStrengthOf(
    std::pair<std::optional<ast::DriveStrength>, std::optional<ast::DriveStrength>> strength,
    slang::SourceLocation location) {
    bool highImpedance = false;
    const auto levelOf = [&highImpedance](std::optional<ast::DriveStrength> value) {
        switch (value.value_or(ast::DriveStrength::Strong)) {
        case ast::DriveStrength::Supply: return eir::Strength::Supply;
        case ast::DriveStrength::Strong: return eir::Strength::Strong;
        case ast::DriveStrength::Pull: return eir::Strength::Pull;
        case ast::DriveStrength::Weak: return eir::Strength::Weak;
        case ast::DriveStrength::HighZ: highImpedance = true; return eir::Strength::Strong;
        }
        return eir::Strength::Strong;
    };
    const eir::DriveStrength result{.zero = levelOf(strength.first),
                                    .one = levelOf(strength.second)};
    if (highImpedance) {
        reportUnsupported(location, "a drive strength of highz");
        return std::nullopt;
    }
    return result;
}

namespace {

/// The variables and nets `target` writes: the one a select or a member access is of, and each
/// a concatenation's operands write.
void writtenSymbols(const ast::Expression& target, std::vector<const ast::Symbol*>& into) {
    switch (target.kind) {
    case ast::ExpressionKind::NamedValue:
        into.push_back(&target.as<ast::NamedValueExpression>().symbol);
        break;
    case ast::ExpressionKind::ElementSelect:
        writtenSymbols(target.as<ast::ElementSelectExpression>().value(), into);
        break;
    case ast::ExpressionKind::RangeSelect:
        writtenSymbols(target.as<ast::RangeSelectExpression>().value(), into);
        break;
    case ast::ExpressionKind::MemberAccess:
        writtenSymbols(target.as<ast::MemberAccessExpression>().value(), into);
        break;
    case ast::ExpressionKind::Concatenation:
        for (const ast::Expression* operand : target.as<ast::ConcatenationExpression>().operands())
            writtenSymbols(*operand, into);
        break;
    case ast::ExpressionKind::Conversion:
        writtenSymbols(target.as<ast::ConversionExpression>().operand(), into);
        break;
    case ast::ExpressionKind::Assignment:
        writtenSymbols(target.as<ast::AssignmentExpression>().left(), into);
        break;
    default: break;
    }
}

/// True when `target` writes `symbol`.
bool writes(const ast::Expression* target, const ast::Symbol& symbol) {
    if (target == nullptr) return false;
    std::vector<const ast::Symbol*> written;
    writtenSymbols(*target, written);
    return std::ranges::find(written, &symbol) != written.end();
}

/// The terminals a built-in gate drives: every one but the last of `buf` and `not`, every one of
/// `pullup` and `pulldown`, and the first of the rest.
std::span<const ast::Expression* const> gateOutputs(const ast::PrimitiveInstanceSymbol& gate) {
    const std::span<const ast::Expression* const> terminals = gate.getPortConnections();
    const std::string_view name = gate.primitiveType.name;
    if (terminals.empty()) return terminals;
    if (name == "buf" || name == "not") return terminals.first(terminals.size() - 1);
    if (name == "pullup" || name == "pulldown") return terminals;
    return terminals.first(1);
}

} // namespace

std::optional<eir::DriveStrength>
ModuleBuilder::switchStrength(const ast::PrimitiveInstanceSymbol& gate) {
    const std::string_view name = gate.primitiveType.name;
    const std::span<const ast::Expression* const> terminals = gate.getPortConnections();
    const auto refuse = [&](std::string_view why) -> std::optional<eir::DriveStrength> {
        reportUnsupported(gate.location,
                          std::format("the switch '{}', since its data input {}", name, why));
        return std::nullopt;
    };

    // The input slang has converted to one bit, of a variable or a net, or a constant, which
    // drives as a continuous assignment of it would.
    const ast::Expression* data = terminals.size() > 1 ? terminals[1] : nullptr;
    while (data != nullptr && data->kind == ast::ExpressionKind::Conversion)
        data = &data->as<ast::ConversionExpression>().operand();
    while (data != nullptr
           && (data->kind == ast::ExpressionKind::ElementSelect
               || data->kind == ast::ExpressionKind::RangeSelect)) {
        data = data->kind == ast::ExpressionKind::ElementSelect
                   ? &data->as<ast::ElementSelectExpression>().value()
                   : &data->as<ast::RangeSelectExpression>().value();
    }
    if (data == nullptr || data->getConstant() != nullptr) return eir::DriveStrength{};
    if (data->kind != ast::ExpressionKind::NamedValue)
        return refuse("is no signal of this module, whose drivers it could know");
    const ast::ValueSymbol& input = data->as<ast::NamedValueExpression>().symbol;
    if (input.kind != slang::ast::SymbolKind::Net) return eir::DriveStrength{};

    for (const ast::Symbol* portSymbol : body->getPortList()) {
        if (portSymbol->kind == slang::ast::SymbolKind::Port
            && portSymbol->as<ast::PortSymbol>().internalSymbol == &input
            && portSymbol->as<ast::PortSymbol>().direction != ast::ArgumentDirection::Out)
            return refuse("is a port, which drivers outside the module may drive");
    }

    // Each driver's strengths, with supply read as strong; a highz one is reported where it is.
    using Declared =
        std::pair<std::optional<ast::DriveStrength>, std::optional<ast::DriveStrength>>;
    const auto level = [](std::optional<ast::DriveStrength> value) {
        switch (value.value_or(ast::DriveStrength::Strong)) {
        case ast::DriveStrength::Pull: return eir::Strength::Pull;
        case ast::DriveStrength::Weak: return eir::Strength::Weak;
        case ast::DriveStrength::Supply:
        case ast::DriveStrength::Strong:
        case ast::DriveStrength::HighZ: return eir::Strength::Strong;
        }
        return eir::Strength::Strong;
    };
    std::vector<eir::DriveStrength> found;
    const auto add = [&](const Declared& declared) {
        found.push_back(
            eir::DriveStrength{.zero = level(declared.first), .one = level(declared.second)});
    };
    const auto& net = input.as<ast::NetSymbol>();
    if (net.getInitializer() != nullptr) add(net.getDriveStrength());
    const ast::NetType::NetKind kind = net.netType.netKind;
    if (kind == ast::NetType::Tri0 || kind == ast::NetType::Tri1)
        add({ast::DriveStrength::Pull, ast::DriveStrength::Pull});
    for (const ast::ContinuousAssignSymbol* assignment : continuousAssignments) {
        if (writes(&assignment->getAssignment(), input)) add(assignment->getDriveStrength());
    }
    for (const ast::PrimitiveInstanceSymbol* other : gates) {
        if (other == &gate) continue;
        if (!std::ranges::any_of(gateOutputs(*other), [&input](const ast::Expression* output) {
                return writes(output, input);
            }))
            continue;
        const std::string_view otherName = other->primitiveType.name;
        if (otherName.ends_with("mos") || otherName.starts_with("tran")
            || otherName.starts_with("rtran"))
            return refuse("is driven by another switch, which passes on a strength of its own");
        const bool pull = otherName == "pullup" || otherName == "pulldown";
        auto declared = other->getDriveStrength();
        if (pull && !declared.first && !declared.second)
            declared = {ast::DriveStrength::Pull, ast::DriveStrength::Pull};
        add(declared);
    }
    for (const ast::PrimitiveInstanceSymbol* primitive : primitives) {
        const std::span<const ast::Expression* const> outputs = primitive->getPortConnections();
        if (!outputs.empty() && writes(outputs.front(), input)) add(primitive->getDriveStrength());
    }
    for (const CollectedInstance& instance : instances) {
        for (const ast::PortConnection* connection : instance.symbol->getPortConnections()) {
            if (connection->port.kind == slang::ast::SymbolKind::Port
                && connection->port.as<ast::PortSymbol>().direction != ast::ArgumentDirection::In
                && writes(connection->getExpression(), input))
                return refuse("is driven by an instance, whose strength this module cannot see");
        }
    }

    if (found.empty()) return eir::DriveStrength{};
    if (!std::ranges::all_of(found, [&found](const eir::DriveStrength& strength) {
            return strength == found.front();
        }))
        return refuse("has drivers of different strengths");
    return found.front();
}

ModuleBuilder::SensitivityForm
ModuleBuilder::extractSensitivity(ProcessTranslator& translator, const ast::TimingControl& timing,
                                  std::vector<eir::SensitivityEntry>& into) {
    switch (timing.kind) {
    case ast::TimingControlKind::SignalEvent: {
        const auto& event = timing.as<ast::SignalEventControl>();
        if (event.iffCondition != nullptr) return SensitivityForm::NotAnEventControl;

        // A signal, or a constant select of one. An event on any other expression — `@(a &
        // b)` — is legal and needs a temporary the scheduler can watch; it is left to the body,
        // which reports it.
        const std::optional<eir::SensitivityEntry> entry =
            translator.eventEntry(event.expr, event.edge);
        if (!entry.has_value()) return SensitivityForm::NotAnEventControl;
        into.push_back(*entry);
        return SensitivityForm::Explicit;
    }

    case ast::TimingControlKind::EventList: {
        for (const ast::TimingControl* event : timing.as<ast::EventListControl>().events) {
            if (extractSensitivity(translator, *event, into) != SensitivityForm::Explicit)
                return SensitivityForm::NotAnEventControl;
        }
        return SensitivityForm::Explicit;
    }

    case ast::TimingControlKind::ImplicitEvent: return SensitivityForm::Implicit;

    default: return SensitivityForm::NotAnEventControl;
    }
}

bool ModuleBuilder::hasPortWidth(const ast::PortSymbol& port, const eir::Type& type) {
    if (port.internalSymbol == nullptr || !ast::ValueSymbol::isKind(port.internalSymbol->kind))
        return true;
    const std::optional<eir::Type> portType =
        convertType(port.internalSymbol->as<ast::ValueSymbol>().getType());
    return !portType.has_value() || portType->bitWidth() == type.bitWidth();
}

std::optional<std::uint32_t>
ModuleBuilder::materialiseConnection(const ast::InstanceSymbol& instance,
                                     const ast::PortSymbol& port,
                                     const ast::Expression& expression) {
    // An `inout` would need the assignment to run both ways at once, which is driver
    // resolution and therefore M5. Inputs and outputs each need it in one direction only.
    if (port.direction == slang::ast::ArgumentDirection::InOut) {
        reportUnsupported(expression.sourceRange.start(),
                          "an inout port connected to an expression, a hierarchical reference "
                          "or a signal of another width");
        return std::nullopt;
    }

    const bool isInput = port.direction == slang::ast::ArgumentDirection::In;

    const std::optional<eir::Type> type =
        convertType(port.internalSymbol->as<ast::ValueSymbol>().getType());
    if (!type.has_value()) {
        reportUnsupported(port.location, "the type of this port");
        return std::nullopt;
    }

    // The object is what the port declares, since the port collapses onto it: an output the
    // child writes procedurally is a variable, X until its first write, where a net would be
    // `z`.
    eir::Object object;
    object.name = uniqueObjectName(std::format("{}${}", instance.name, port.name));
    object.type = *type;
    object.kind = eir::ObjectKind::Variable;
    if (port.internalSymbol->kind == slang::ast::SymbolKind::Net) {
        const std::optional<eir::NetKind> netKind =
            convertNetKind(port.internalSymbol->as<ast::NetSymbol>().netType.netKind);
        if (!netKind.has_value()) {
            reportUnsupported(port.location, "the net type of this port");
            return std::nullopt;
        }
        object.kind = eir::ObjectKind::Net;
        object.netKind = *netKind;
    }
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

bool ModuleBuilder::addInstance(
    const ast::InstanceSymbol& instance, const std::string& prefix, const std::string& targetName,
    const std::map<std::string, std::uint32_t, std::less<>>& targetPorts) {
    // Named with the generate blocks it is inside, as an object is: two iterations of a loop
    // each instantiating `u` are otherwise two instances with one name, and one path.
    eir::Instance result;
    result.name = prefix + instanceName(instance);
    result.definitionName = targetName;
    result.location = convertLocation(instance.location);

    for (const ast::PortConnection* connection : instance.getPortConnections()) {
        if (connection->port.kind == slang::ast::SymbolKind::InterfacePort) {
            if (!bindInterface(instance, *connection, result)) return false;
            continue;
        }

        const auto portIndex = targetPorts.find(connection->port.name);
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
            // A hierarchical reference has no storage until elaboration binds it, so a port
            // cannot be collapsed onto it; it is connected through an assignment instead. So is
            // a signal of another width than the port, which elaboration can only collapse onto
            // a port as wide as it: the assignment extends or truncates as one written out would
            // (IEEE 1800 section 23.3.3.7).
            if (objectIndex.has_value()
                && (!definitionValue.objects[*objectIndex].hierarchicalPath.empty()
                    || !hasPortWidth(port, definitionValue.objects[*objectIndex].type)))
                objectIndex = materialiseConnection(instance, port, reference);
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

/// Returns the EIR name for one parameterisation of a module.
///
/// slang caches one body per distinct parameterisation, so a parameterised module
/// produces several. Each becomes its own EIR definition — which is what keeps parameters
/// out of EIR entirely (see eir/Module.h) — and a suffix distinguishes them.
///
/// **The suffix is a function of the parameter values, and of the interfaces the body's
/// ports are connected to, not a counter.** A counter numbers the parameterisations in the
/// order *this* compilation happened to walk them, which makes the name meaningless outside
/// it: `counter$1` from one analysis and `counter$1` from another would be different
/// modules wearing one name, and a design assembled from two libraries would resolve an
/// instance to the wrong body with nothing to say so. Deriving it from the values instead
/// gives a specialisation the same name wherever it is produced, which is what lets a
/// library be instantiated with parameters chosen by whoever uses it.
std::string specialisedModuleName(const ast::InstanceBodySymbol& body) {
    std::string base(body.getDefinition().name);

    // Only the parameters set from outside: a localparam is derived inside the module from
    // those and from its interfaces', both of which the name covers, so including it would make
    // the name longer without making it more specific.
    std::string values;
    for (const ast::ParameterSymbolBase* parameter : body.getParameters()) {
        if (parameter->isLocalParam()) continue;

        const ast::Symbol& symbol = parameter->symbol;
        std::string rendered;
        if (symbol.kind == ast::SymbolKind::Parameter) {
            rendered = symbol.as<ast::ParameterSymbol>().getValue().toString();
        } else if (symbol.kind == ast::SymbolKind::TypeParameter) {
            rendered = symbol.as<ast::TypeParameterSymbol>()
                           .targetType.getType()
                           .getCanonicalType()
                           .toString();
        } else {
            continue;
        }

        if (!values.empty()) values += ',';
        values += symbol.name;
        values += '=';
        values += rendered;
    }

    // And what each interface port is connected to, where that can change the body. A body
    // reads an interface's parameters through the port, `localparam W = bus.W`, so two
    // instances agreeing on their own parameters can still disagree on every width that
    // depends on the interface's; and a generic port, or one taking an array, reaches different
    // members or a different number of elements by what it is given.
    for (const ast::Symbol* portSymbol : body.getPortList()) {
        if (portSymbol->kind != slang::ast::SymbolKind::InterfacePort) continue;
        const auto& port = portSymbol->as<ast::InterfacePortSymbol>();
        const ast::Symbol* connected = port.getConnection().first;
        if (connected == nullptr) continue;

        std::string rendered;
        if (connected->kind == slang::ast::SymbolKind::Instance) {
            const ast::InstanceBodySymbol& interface = connected->as<ast::InstanceSymbol>().body;
            rendered = specialisedModuleName(interface);
            if (!port.isGeneric && rendered == interface.getDefinition().name) continue;
        } else if (connected->kind == slang::ast::SymbolKind::InstanceArray) {
            const auto& array = connected->as<ast::InstanceArraySymbol>();
            const ast::InstanceSymbol* element = firstElement(array);
            if (element == nullptr) continue;
            rendered =
                std::format("{}[{}]", specialisedModuleName(element->body), countElements(array));
        } else {
            continue;
        }

        if (!values.empty()) values += ',';
        values += port.name;
        values += '=';
        values += rendered;
    }

    if (values.empty()) return base;

    // Readable while it is short, which is what a `lib list` and a waveform hierarchy want; a
    // digest once it is not, so that a module with many or wide parameters does not produce a
    // name nothing can display. Either way it is a function of the values alone.
    std::string suffix;
    if (values.size() <= 32) {
        suffix = values;
        for (char& c : suffix) {
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                              || (c >= '0' && c <= '9') || c == '_';
            if (!keep) c = '_';
        }
    } else {
        suffix = digestOf(values);
    }

    return std::format("{}${}", base, suffix);
}

/// What the names in an instance's body that reach past its own objects lead to, from that
/// instance: for each, in the order slang holds them, the path a reference to it takes and the
/// kind and type of what it reaches, or the object's own name when a hierarchical name comes back
/// to the instance itself. A subroutine called by a name found upwards is inlined, so it is named
/// by its module's parameterisation, and what its own body names is followed from here too.
///
/// Instances of one parameterisation translate alike only when these agree. A reference is a
/// path from the instance holding it, so `tb.clk` read from two depths is `../clk` from one and
/// `../../clk` from the other; `tb.u_seen.mark` is an object of `u_seen`'s own from `u_seen` and
/// a reference from its sibling; and `../bump` is whichever ancestor's task of that name is
/// nearest. slang keeps the names that leave an instance, but not where anything outside it can
/// read them, so they are found here, by a walk of the body that a translation does not make.
class ReachedNames : public ast::ASTVisitor<ReachedNames, ast::VisitFlags::AllGood> {
public:
    explicit ReachedNames(const ast::InstanceBodySymbol& from) : body(&from) {}

    // A child's body is its own definition's; only its connections are written here.
    void handle(const ast::InstanceSymbol& instance) { instance.visitExprs(*this); }

    // A branch a generate condition rules out is never translated.
    void handle(const ast::GenerateBlockSymbol& block) {
        if (!block.isUninstantiated) visitDefault(block);
    }

    void handle(const ast::HierarchicalValueExpression& expression) {
        note(expression.symbol, true);
    }

    void handle(const ast::NamedValueExpression& expression) { note(expression.symbol, false); }

    // A subroutine found by its name upwards is inlined, and reaches its own module's objects
    // from wherever it is called.
    void handle(const ast::CallExpression& call) {
        if (!call.isSystemCall()) {
            const ast::SubroutineSymbol& subroutine = *std::get<0>(call.subroutine);
            const ast::InstanceBodySymbol* owner = nullptr;
            const std::optional<std::string> name = nameWithinBody(subroutine, owner);
            if (name.has_value() && owner != nullptr && owner != body) {
                reached.push_back(
                    std::format("{} {}", ModuleBuilder::pathTo(*body, *owner, *name).value_or("?"),
                                specialisedModuleName(*owner)));
                if (entered.insert(&subroutine).second) subroutine.visit(*this);
            }
        }
        visitDefault(call);
    }

    std::vector<std::string> reached;

private:
    void note(const ast::ValueSymbol& symbol, bool hierarchical) {
        const ast::ValueSymbol* target = &symbol;
        if (target->kind == slang::ast::SymbolKind::ModportPort) {
            const ast::Symbol* internal = target->as<ast::ModportPortSymbol>().internalSymbol;
            if (internal == nullptr || !ast::ValueSymbol::isKind(internal->kind)) return;
            target = &internal->as<ast::ValueSymbol>();
        }
        // Only what has storage becomes a reference; a package's variable is at one absolute
        // path from everywhere.
        if (target->kind != slang::ast::SymbolKind::Variable
            && target->kind != slang::ast::SymbolKind::Net)
            return;
        if (ModuleBuilder::packageOf(*target) != nullptr) return;

        const ast::InstanceBodySymbol* owner = nullptr;
        const std::optional<std::string> name = nameWithinBody(*target, owner);
        if (!name.has_value() || owner == nullptr) return;
        if (owner == body) {
            if (hierarchical) reached.push_back(*name);
            return;
        }
        reached.push_back(std::format("{} {} {}",
                                      ModuleBuilder::pathTo(*body, *owner, *name).value_or("?"),
                                      target->kind == slang::ast::SymbolKind::Net ? "net" : "var",
                                      target->getType().toString()));
    }

    const ast::InstanceBodySymbol* body;

    /// The subroutines of other modules already followed, so that a recursive one ends.
    std::set<const ast::SubroutineSymbol*> entered;
};

/// Walks the elaborated hierarchy, translating each distinct module body once.
class Walker {
public:
    Walker(eir::Design& target, ModuleBuilder::Shared sharedState)
        : design(&target), shared(sharedState) {
        shared.packageVariables = &packageVariables;
    }

    /// Translates `instance`'s body if it has not been seen, and returns its EIR name.
    std::optional<std::string> translate(const ast::InstanceSymbol& instance);

    /// Makes an instance of one of `units` ask for it into `requests` rather than be translated.
    void standIn(const std::vector<eir::UnitInterface>& units,
                 std::vector<eir::UnitRequest>& requests) {
        interfaces = &units;
        asked = &requests;
    }

    /// Adds a definition for each package whose variables the translated modules refer to.
    bool translatePackages();

    [[nodiscard]] bool ok() const noexcept { return succeeded; }

private:
    struct BodyInfo {
        std::string name;
        std::map<std::string, std::uint32_t, std::less<>> portIndices;
    };

    eir::Design* design;
    ModuleBuilder::Shared shared;
    /// Bodies already translated, by the EIR name they were given.
    ///
    /// Keyed by name and not by `InstanceBodySymbol*`: slang hands out a separate body symbol
    /// per instance even when the parameterisation is identical, so a pointer never matches and
    /// every instance of a module was translated again and then discarded by the design's
    /// replace-by-name. The name is a function of the definition, the parameters set on it and
    /// the interfaces it is given (`specialisedModuleName`), so two bodies sharing a name are the
    /// same module and one translation serves every instance of it.
    ///
    /// This is only sound because `BodyInfo::portIndices` is keyed by port name. A map keyed on
    /// slang's port symbols belongs to the one body it came from, and reusing it for another
    /// binds no connections at all — quietly, with the design still elaborating and simulating
    /// to the wrong answer.
    std::map<std::string, BodyInfo, std::less<>> bodies;

    /// Whether the body of each parameterisation names anything past its own objects, by the
    /// name `specialisedModuleName` gives it, as the first instance of it showed. Only an
    /// instance of one that does has its names followed (`ReachedNames`), so that the others
    /// cost nothing more than a lookup.
    std::map<std::string, bool, std::less<>> reachesPast;
    std::vector<const ast::ValueSymbol*> packageVariables;
    bool succeeded = true;

    /// The request `instance` of `unit` makes, as its placeholder.
    std::optional<std::string> askFor(const ast::InstanceSymbol& instance,
                                      const eir::UnitInterface& unit);

    const std::vector<eir::UnitInterface>* interfaces = nullptr;
    std::vector<eir::UnitRequest>* asked = nullptr;
};

bool Walker::translatePackages() {
    // Packages in the order a module first used one of their variables, which is the order the
    // modules were walked in, so the definitions come out the same on every run.
    std::vector<const ast::PackageSymbol*> packages;
    for (const ast::ValueSymbol* variable : packageVariables) {
        const ast::PackageSymbol* package = ModuleBuilder::packageOf(*variable);
        if (std::ranges::find(packages, package) == packages.end()) packages.push_back(package);
    }

    for (const ast::PackageSymbol* package : packages) {
        ModuleBuilder builder(shared, *package);
        for (const ast::ValueSymbol* variable : packageVariables) {
            if (ModuleBuilder::packageOf(*variable) != package) continue;
            if (!builder.objectFor(*variable).has_value()) return false;
        }
        design->addModule(builder.take());
    }
    return true;
}

std::optional<std::string> Walker::translate(const ast::InstanceSymbol& instance) {
    const ast::InstanceBodySymbol& body = instance.body;

    // An interface instance is an instance like any other: its body becomes a definition, its
    // signals get storage of their own, and what reaches them through a port is a reference.
    if (!instance.isModule() && !instance.isInterface()) {
        shared.diagnostics.report(DiagCode::UnsupportedConstruct, SourceLocation(),
                                  "program instances");
        succeeded = false;
        return std::nullopt;
    }

    // A unit of the other language is asked for, not translated.
    if (interfaces != nullptr) {
        const auto unit =
            std::ranges::find(*interfaces, body.getDefinition().name, &eir::UnitInterface::name);
        if (unit != interfaces->end()) return askFor(instance, *unit);
    }

    std::string name = specialisedModuleName(body);

    // An instance whose names lead out of it, to objects it reaches by a path from where it is,
    // is a definition of its own unless another instance's lead to the same places, and is named
    // for where they lead. One whose names stay inside keeps the parameterisation's name, which
    // every such instance can share: a name leading into the instance holding it leads there
    // from every instance of one parameterisation.
    const auto [past, first] = reachesPast.try_emplace(name, false);
    if (first || past->second) {
        ReachedNames names(body);
        body.visit(names);
        if (first) past->second = !names.reached.empty();
        if (std::ranges::any_of(names.reached,
                                [](const std::string& path) { return path.starts_with("../"); })) {
            std::string joined;
            for (const std::string& path : names.reached) joined += path + '\n';
            name = std::format("{}${}", name, digestOf(joined));
        }
    }

    const auto existing = bodies.find(name);
    if (existing != bodies.end()) return existing->second.name;
    ModuleBuilder builder(shared, name, body);

    if (!builder.collectDeclarations()) {
        succeeded = false;
        return std::nullopt;
    }

    // The name is reserved before recursing so that a hierarchy which reaches the same body
    // again — directly or through a cycle slang did not reject — terminates here rather
    // than recursing until the stack runs out.
    bodies.emplace(name, BodyInfo{.name = name, .portIndices = builder.portIndices()});

    for (const ModuleBuilder::CollectedInstance& collected : builder.instances) {
        const ast::InstanceSymbol* child = collected.symbol;
        const std::optional<std::string> childName = translate(*child);
        if (!childName.has_value()) {
            succeeded = false;
            return std::nullopt;
        }

        const auto childInfo = bodies.find(*childName);
        if (childInfo == bodies.end()) {
            succeeded = false;
            return std::nullopt;
        }

        if (!builder.addInstance(*child, collected.prefix, *childName,
                                 childInfo->second.portIndices)) {
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

/// slang's buffers of `options.files`, each mapped to the buffer of Enzan's holding its text;
/// false, having reported why, when a file cannot be read.
///
/// Every file is read through Enzan's source manager and the text is then handed to slang, so that
/// a diagnostic from slang and one from elaboration quote the same bytes and the same snippet.
/// Reading each file twice would be slower and could disagree.
bool slangBuffersOf(const Options& options, SourceManager& sources, DiagnosticEngine& diagnostics,
                    slang::SourceManager& slangSources,
                    std::map<slang::BufferID, BufferId>& buffers,
                    std::vector<slang::SourceBuffer>& slangBuffers) {
    for (std::size_t index = 0; index < options.files.size(); ++index) {
        const std::filesystem::path& path = options.files[index];

        BufferId text = index < options.buffers.size() ? options.buffers[index] : BufferId();
        if (!text.valid()) {
            const FileReadResult result = sources.readFile(path);
            if (!result.ok()) {
                reportFileReadFailure(diagnostics, path, result);
                return false;
            }
            text = result.buffer;
        }

        const slang::SourceBuffer buffer =
            slangSources.assignText(path.string(), sources.bufferText(text));
        buffers.emplace(buffer.id, text);
        slangBuffers.push_back(buffer);
    }
    return true;
}

/// Maps each file slang opened for an `include to a buffer of Enzan's holding the same text, so
/// that a diagnostic or a location in one points into it as one in a source given does. The text
/// is slang's, not read again, so that the two cannot disagree.
void mapIncludedBuffers(const slang::SourceManager& slangSources, SourceManager& sources,
                        std::map<slang::BufferID, BufferId>& buffers) {
    for (const slang::BufferID buffer : slangSources.getAllBuffers()) {
        if (buffers.contains(buffer) || !slangSources.getIncludedFrom(buffer).valid()) continue;

        // slang ends a buffer's text with a null, which the file does not hold.
        std::string_view text = slangSources.getSourceText(buffer);
        if (!text.empty() && text.back() == '\0') text.remove_suffix(1);
        buffers.emplace(buffer, sources.addBuffer(slangSources.getFullPath(buffer).string(),
                                                  std::string(text)));
    }
}

slang::parsing::PreprocessorOptions preprocessorOptionsOf(const Options& options) {
    slang::parsing::PreprocessorOptions preprocessorOptions;
    preprocessorOptions.predefines = options.defines;
    preprocessorOptions.additionalIncludePaths = options.includeDirectories;
    return preprocessorOptions;
}

/// A module a source declares, and the parameters an instance of it may set.
struct DeclaredModule {
    std::string_view name;
    std::vector<std::string_view> parameters;

    /// The parameters, of `parameters`, that take a string: declared `string`, or with no type and
    /// a string as their default.
    std::vector<std::string_view> strings;
};

/// The modules `tree` declares outside any other, in order.
void collectModules(const slang::syntax::SyntaxTree& tree, std::vector<DeclaredModule>& found) {
    namespace syntax = slang::syntax;
    if (tree.root().kind != syntax::SyntaxKind::CompilationUnit) return;

    // The value parameters a declaration declares, unless they are local.
    const auto values = [](const syntax::ParameterDeclarationBaseSyntax& declaration,
                           DeclaredModule& module) {
        if (declaration.kind != syntax::SyntaxKind::ParameterDeclaration
            || declaration.keyword.kind == slang::parsing::TokenKind::LocalParamKeyword)
            return;
        const auto& parameter = declaration.as<syntax::ParameterDeclarationSyntax>();
        const syntax::SyntaxKind type = parameter.type->kind;
        for (const syntax::DeclaratorSyntax* declarator : parameter.declarators) {
            module.parameters.push_back(declarator->name.valueText());
            const bool stringDefault = type == syntax::SyntaxKind::ImplicitType
                                       && declarator->initializer != nullptr
                                       && declarator->initializer->expr->kind
                                              == syntax::SyntaxKind::StringLiteralExpression;
            if (type == syntax::SyntaxKind::StringType || stringDefault)
                module.strings.push_back(declarator->name.valueText());
        }
    };

    for (const syntax::MemberSyntax* member :
         tree.root().as<syntax::CompilationUnitSyntax>().members) {
        if (member->kind != syntax::SyntaxKind::ModuleDeclaration) continue;
        const auto& module = member->as<syntax::ModuleDeclarationSyntax>();
        DeclaredModule declared{
            .name = module.header->name.valueText(), .parameters = {}, .strings = {}};

        // With a parameter port list, a parameter of the body is local (IEEE 1800 section 6.20.1).
        if (module.header->parameters != nullptr) {
            for (const syntax::ParameterDeclarationBaseSyntax* declaration :
                 module.header->parameters->declarations)
                values(*declaration, declared);
        } else {
            for (const syntax::MemberSyntax* item : module.members) {
                if (item->kind == syntax::SyntaxKind::ParameterDeclarationStatement)
                    values(*item->as<syntax::ParameterDeclarationStatementSyntax>().parameter,
                           declared);
            }
        }
        if (!declared.name.empty()) found.push_back(std::move(declared));
    }
}

/// The one of `names` that is `wanted` exactly, or else the one that is in some case; nothing when
/// there is none, or several in other cases.
std::optional<std::string_view> nameMatching(std::span<const std::string_view> names,
                                             std::string_view wanted) {
    if (const auto exact = std::ranges::find(names, wanted); exact != names.end()) return *exact;
    std::optional<std::string_view> found;
    for (const std::string_view name : names) {
        if (!equalsIgnoringCase(name, wanted)) continue;
        if (found) return std::nullopt;
        found = name;
    }
    return found;
}

/// `name` escaped, which names what it names as its plain spelling does whatever characters it
/// holds (IEEE 1800 section 5.6.1).
std::string escapedName(std::string_view name) {
    return std::format("\\{} ", name);
}

/// `value` as a literal of the kind `kind` (`eir::UnitRequest`); nothing when it is not one.
std::optional<std::string> literalOf(const slang::ConstantValue& value,
                                     eir::UnitInterface::Kind kind) {
    using Kind = eir::UnitInterface::Kind;
    if (kind == Kind::Real) {
        if (!value.isReal() && !value.isInteger()) return std::nullopt;
        return eir::realLiteral(value.isReal() ? static_cast<double>(value.real())
                                               : value.integer().toDouble());
    }
    if (kind == Kind::String) {
        if (!value.isString()) return std::nullopt;
        std::string text = "\"";
        for (const char character : value.str()) {
            const auto code = static_cast<unsigned char>(character);
            if (character == '"' || character == '\\') {
                text += '\\';
                text += character;
            } else if (code >= 0x20 && code < 0x7f) {
                text += character;
            } else {
                text += std::format("\\{:03o}", code);
            }
        }
        return text + '"';
    }
    if (!value.isInteger()) return std::nullopt;
    const slang::SVInt& bits = value.integer();
    if (kind == Kind::Integer) {
        const std::optional<std::int64_t> number = bits.as<std::int64_t>();
        if (!number) return std::nullopt;
        if (*number >= std::numeric_limits<std::int32_t>::min()
            && *number <= std::numeric_limits<std::int32_t>::max())
            return std::to_string(*number);
        const std::uint64_t magnitude = *number < 0 ? 0 - static_cast<std::uint64_t>(*number)
                                                    : static_cast<std::uint64_t>(*number);
        return std::format("{}64'sd{}", *number < 0 ? "-" : "", magnitude);
    }
    const bool vector = kind == Kind::BitVector || kind == Kind::LogicVector;
    if (!vector && bits.getBitWidth() != 1) return std::nullopt;
    std::string text = std::format("{}'b", bits.getBitWidth());
    for (auto bit = static_cast<std::int32_t>(bits.getBitWidth()) - 1; bit >= 0; --bit) {
        const slang::logic_t one = bits[bit];
        if (one.value == slang::logic_t::z.value) {
            text += 'z';
        } else if (one.isUnknown()) {
            text += 'x';
        } else {
            text += one.value == 0 ? '0' : '1';
        }
    }
    return text;
}

/// What `instance`, an instance of `unit`, asks for: its unit with each parameter's value, and its
/// ports, which are the unit's in order. Nothing when a value is no literal of its parameter's
/// kind, `refused` receiving that parameter's name.
std::optional<eir::UnitRequest> requestOf(const ast::InstanceSymbol& instance,
                                          const eir::UnitInterface& unit, std::string& refused) {
    eir::UnitRequest request{.placeholder = {}, .unit = unit.name, .parameters = {}, .ports = {}};
    std::string values;
    for (const eir::UnitInterface::Parameter& parameter : unit.parameters) {
        const ast::Symbol* found = instance.body.find(parameter.name);
        std::optional<std::string> value;
        if (found != nullptr && found->kind == ast::SymbolKind::Parameter)
            value = literalOf(found->as<ast::ParameterSymbol>().getValue(), parameter.kind);
        if (!value) {
            refused = parameter.name;
            return std::nullopt;
        }
        values += std::format("{}{} => {}", values.empty() ? "" : ", ", parameter.name, *value);
        request.parameters.push_back({.name = parameter.name, .value = std::move(*value)});
    }
    for (const eir::UnitInterface::Port& port : unit.ports)
        request.ports.push_back({.name = port.name, .direction = port.direction});
    request.placeholder = std::format("{} generic map ({})", unit.name, values);
    return request;
}

/// Whether a row of `unit`'s widths is for the values `request` asks for.
bool hasRow(const eir::UnitInterface& unit, const eir::UnitRequest& request) {
    std::vector<std::string> values;
    values.reserve(request.parameters.size());
    for (const eir::UnitRequest::Parameter& parameter : request.parameters)
        values.push_back(parameter.value);
    return std::ranges::find(unit.widths, values, &eir::UnitInterface::Widths::values)
           != unit.widths.end();
}

/// Adds `request` to `asked` unless one asks for the same unit with the same values: once for each
/// set of values, in the order first met.
void addRequest(std::vector<eir::UnitRequest>& asked, eir::UnitRequest request) {
    const bool known = std::ranges::any_of(asked, [&request](const eir::UnitRequest& one) {
        return one.unit == request.unit && one.parameters == request.parameters;
    });
    if (!known) asked.push_back(std::move(request));
}

/// What every instance of one of `interfaces` in a design asks for, found without translating it.
class StandInInstances : public ast::ASTVisitor<StandInInstances, ast::VisitFlags::Symbols> {
public:
    explicit StandInInstances(const std::vector<eir::UnitInterface>& units) : interfaces(&units) {}

    void handle(const ast::InstanceSymbol& instance) {
        const auto unit = std::ranges::find(*interfaces, instance.body.getDefinition().name,
                                            &eir::UnitInterface::name);
        if (unit != interfaces->end()) {
            std::string refused;
            if (std::optional<eir::UnitRequest> request = requestOf(instance, *unit, refused)) {
                guessed = guessed || !hasRow(*unit, *request);
                addRequest(asked, std::move(*request));
            }
            return;
        }
        visitDefault(instance);
    }

    std::vector<eir::UnitRequest> asked;

    /// Whether an instance's values have no row of its unit's widths.
    bool guessed = false;

private:
    const std::vector<eir::UnitInterface>* interfaces;
};

/// The name of the local parameter of a unit's module that is its port `port`'s width.
std::string widthParameterName(std::size_t port) {
    return std::format("enzan$width${}", port);
}

/// The name of the local parameter of a unit's module that is its unpacked port `port`'s `which`
/// bound, `left` or `right`.
std::string boundParameterName(std::size_t port, std::string_view which) {
    return std::format("enzan${}${}", which, port);
}

/// A module standing for each of `interfaces`, with its parameters and its ports, each port as wide
/// as the row of its width table the instance's values select, or one bit wide when none does: what
/// instances of the unit are checked against, which is never translated (`eir::UnitInterface`).
std::string interfaceText(const std::vector<eir::UnitInterface>& interfaces) {
    using Kind = eir::UnitInterface::Kind;
    std::string text;
    for (const eir::UnitInterface& unit : interfaces) {
        std::vector<std::string> declarations;
        for (const eir::UnitInterface::Parameter& parameter : unit.parameters) {
            std::string type;
            switch (parameter.kind) {
            case Kind::Integer: type = "int "; break;
            case Kind::Bit: type = "bit "; break;
            case Kind::Logic: type = "logic "; break;
            case Kind::BitVector:
            case Kind::LogicVector:
                if (parameter.length != 0) {
                    type = std::format("{} [{}:0] ",
                                       parameter.kind == Kind::BitVector ? "bit" : "logic",
                                       parameter.length - 1);
                }
                break;
            case Kind::String: type = "string "; break;
            case Kind::Real: type = "real "; break;
            }
            declarations.push_back(std::format(
                "parameter {}{}{}", type, escapedName(parameter.name),
                parameter.value ? std::format("= {}", *parameter.value) : std::string()));
        }

        // Each row's values against the parameters', exactly: x and z as themselves.
        std::vector<std::string> conditions;
        for (const eir::UnitInterface::Widths& row : unit.widths) {
            std::string condition;
            for (std::size_t index = 0; index < unit.parameters.size(); ++index) {
                if (index >= row.values.size()) break;
                const eir::UnitInterface::Parameter& parameter = unit.parameters[index];
                if (!condition.empty()) condition += " && ";
                condition += std::format(
                    "{}{} {}", escapedName(parameter.name),
                    parameter.kind == Kind::String || parameter.kind == Kind::Real ? "==" : "===",
                    row.values[index]);
            }
            conditions.push_back(condition.empty() ? std::string("1") : condition);
        }
        // Each port's width, an unpacked one's element's, and an unpacked one's bounds.
        for (std::size_t port = 0; port < unit.ports.size(); ++port) {
            std::string bits;
            std::string left;
            std::string right;
            for (std::size_t row = 0; row < unit.widths.size(); ++row) {
                if (port >= unit.widths[row].ports.size()) continue;
                const eir::UnitInterface::PortWidth& width = unit.widths[row].ports[port];
                bits += std::format("({}) ? {} : ", conditions[row], width.bits);
                left += std::format("({}) ? {} : ", conditions[row], width.left);
                right += std::format("({}) ? {} : ", conditions[row], width.right);
            }
            declarations.push_back(
                std::format("localparam int {}= {}1", escapedName(widthParameterName(port)), bits));
            if (unit.ports[port].unpacked) {
                declarations.push_back(std::format(
                    "localparam int {}= {}0", escapedName(boundParameterName(port, "left")), left));
                declarations.push_back(std::format("localparam int {}= {}0",
                                                   escapedName(boundParameterName(port, "right")),
                                                   right));
            }
        }

        std::vector<std::string> ports;
        for (std::size_t index = 0; index < unit.ports.size(); ++index) {
            const eir::UnitInterface::Port& port = unit.ports[index];
            const char* direction = "input";
            if (port.direction == eir::PortDirection::Output) direction = "output";
            if (port.direction == eir::PortDirection::InOut) direction = "inout";
            const std::string dimension =
                port.unpacked
                    ? std::format(" [{}:{}]", escapedName(boundParameterName(index, "left")),
                                  escapedName(boundParameterName(index, "right")))
                    : std::string();
            ports.push_back(
                std::format("{} {}{}[{}- 1:0] {}{}", direction, port.fourState ? "logic " : "bit ",
                            port.isSigned ? "signed " : "", escapedName(widthParameterName(index)),
                            escapedName(port.name), dimension));
        }

        const auto joined = [](const std::vector<std::string>& parts) {
            std::string result;
            for (const std::string& part : parts)
                result += std::format("{}\n    {}", result.empty() ? "" : ",", part);
            return result;
        };
        text += std::format("module {}#({}\n) ({}\n);\nendmodule\n", escapedName(unit.name),
                            joined(declarations), joined(ports));
    }
    return text;
}

/// The names the sources declare and instantiate, from their syntax.
class NameCollector : public slang::syntax::SyntaxVisitor<NameCollector> {
public:
    void handle(const slang::syntax::ModuleDeclarationSyntax& declaration) {
        const std::string_view name = declaration.header->name.valueText();
        if (declaration.kind == slang::syntax::SyntaxKind::ModuleDeclaration) add(modules, name);
        add(declared, name);
        visitDefault(declaration);
    }

    void handle(const slang::syntax::UdpDeclarationSyntax& declaration) {
        add(declared, declaration.name.valueText());
    }

    void handle(const slang::syntax::HierarchyInstantiationSyntax& instantiation) {
        add(instantiated, instantiation.type.valueText());
        visitDefault(instantiation);
    }

    /// Modules, and modules, interfaces, programs and primitives, by their names, in order.
    std::vector<std::string> modules;
    std::vector<std::string> declared;
    std::vector<std::string> instantiated;

private:
    static void add(std::vector<std::string>& names, std::string_view name) {
        if (!name.empty() && std::ranges::find(names, name) == names.end())
            names.emplace_back(name);
    }
};

/// The module through which the request at `index` is specialised: one instantiating the module it
/// asks for, as `u`, with the values it asks for.
std::string requestModuleName(std::size_t index) {
    return std::format("enzan$request${}", index);
}

/// The text of each request's module (`requestModuleName`); nothing, having reported why, when one
/// asks for what no module of `declared` has.
std::optional<std::string> requestText(const std::vector<eir::UnitRequest>& requests,
                                       const std::vector<DeclaredModule>& declared,
                                       DiagnosticEngine& diagnostics) {
    std::vector<std::string_view> moduleNames;
    moduleNames.reserve(declared.size());
    for (const DeclaredModule& module : declared) moduleNames.push_back(module.name);

    std::string text;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const eir::UnitRequest& request = requests[index];
        const std::optional<std::string_view> name = nameMatching(moduleNames, request.unit);
        if (!name) {
            diagnostics.report(DiagCode::RequestedModuleMissing, SourceLocation(), request.unit);
            return std::nullopt;
        }
        const DeclaredModule& module = declared[static_cast<std::size_t>(
            std::ranges::find(moduleNames, *name) - moduleNames.begin())];

        std::string values;
        for (const eir::UnitRequest::Parameter& parameter : request.parameters) {
            const std::optional<std::string_view> parameterName =
                nameMatching(module.parameters, parameter.name);
            if (!parameterName && parameter.defaulted) continue;
            if (!parameterName) {
                diagnostics.report(DiagCode::RequestedParameterMissing, SourceLocation(), *name,
                                   parameter.name);
                return std::nullopt;
            }
            // A boolean a parameter takes as a string is that string, as a model written in
            // SystemVerilog spells one.
            std::string value = parameter.value;
            if (parameter.boolean
                && std::ranges::find(module.strings, *parameterName) != module.strings.end())
                value = parameter.value == "1'b1" ? "\"TRUE\"" : "\"FALSE\"";
            if (!values.empty()) values += ", ";
            values += std::format(".{}({})", escapedName(*parameterName), value);
        }
        text +=
            std::format("module {};\n  {}{} u ();\nendmodule\n", requestModuleName(index),
                        escapedName(*name), values.empty() ? "" : std::format(" #({})", values));
    }
    return text;
}

std::optional<std::string> Walker::askFor(const ast::InstanceSymbol& instance,
                                          const eir::UnitInterface& unit) {
    std::string refused;
    std::optional<eir::UnitRequest> request = requestOf(instance, unit, refused);
    if (!request) {
        shared.diagnostics.report(
            DiagCode::UnsupportedConstruct, SourceLocation(),
            std::format("the value of the parameter '{}' of '{}', which sets a generic of VHDL,",
                        refused, unit.name));
        succeeded = false;
        return std::nullopt;
    }
    std::string placeholder = request->placeholder;
    const bool known = hasRow(unit, *request);
    addRequest(*asked, std::move(*request));

    // Its connections index the request's ports, which are the unit's, in order. Until a row of
    // the unit's widths is for its values, its ports are one bit wide only to be checked against,
    // and connecting them would refuse what is right once they are as wide as they will be: it
    // connects nothing then, and is analysed again with the widths.
    if (!bodies.contains(placeholder)) {
        std::map<std::string, std::uint32_t, std::less<>> ports;
        for (std::uint32_t index = 0; known && index < unit.ports.size(); ++index)
            ports.emplace(unit.ports[index].name, index);
        bodies.emplace(placeholder, BodyInfo{.name = placeholder, .portIndices = std::move(ports)});
    }
    return placeholder;
}

} // namespace

ModuleNames moduleNames(const Options& options, SourceManager& sources,
                        DiagnosticEngine& diagnostics) {
    slang::SourceManager slangSources;
    std::map<slang::BufferID, BufferId> buffers;
    std::vector<slang::SourceBuffer> slangBuffers;
    if (!slangBuffersOf(options, sources, diagnostics, slangSources, buffers, slangBuffers))
        return {};

    slang::Bag bag;
    bag.set(preprocessorOptionsOf(options));
    NameCollector collector;
    for (const slang::SourceBuffer& buffer : slangBuffers) {
        const std::shared_ptr<slang::syntax::SyntaxTree> tree =
            slang::syntax::SyntaxTree::fromBuffer(buffer, slangSources, bag);
        tree->root().visit(collector);
    }

    ModuleNames names{.declared = std::move(collector.modules), .undeclared = {}};
    for (std::string& name : collector.instantiated) {
        if (std::ranges::find(collector.declared, name) == collector.declared.end())
            names.undeclared.push_back(std::move(name));
    }
    return names;
}

std::unique_ptr<eir::Design> analyse(const Options& options, SourceManager& sources,
                                     DiagnosticEngine& diagnostics) {
    if (options.files.empty()) {
        diagnostics.report(DiagCode::NoInputFiles, SourceLocation());
        return nullptr;
    }

    slang::SourceManager slangSources;
    std::map<slang::BufferID, BufferId> buffers;
    std::vector<slang::SourceBuffer> slangBuffers;
    if (!slangBuffersOf(options, sources, diagnostics, slangSources, buffers, slangBuffers))
        return nullptr;

    slang::Bag bag;
    bag.set(preprocessorOptionsOf(options));

    // A module another language's units instantiate is specialised through a top level of its own
    // instantiating it, which makes it no top level itself, as an instance from the other
    // language would.
    std::vector<std::string> requestModules;
    requestModules.reserve(options.requests.size());
    for (std::size_t index = 0; index < options.requests.size(); ++index)
        requestModules.push_back(requestModuleName(index));

    slang::ast::CompilationOptions compilationOptions;
    for (const std::string& top : options.topModules) compilationOptions.topModules.insert(top);
    if (!options.topModules.empty() || options.onlyRequested) {
        for (const std::string& top : requestModules) compilationOptions.topModules.insert(top);
    }
    compilationOptions.paramOverrides = options.parameterOverrides;
    if (const std::optional<slang::TimeUnit> unit = timeUnitOf(options.timeUnitExponent)) {
        const slang::TimeScaleValue value(*unit, slang::TimeScaleMagnitude::One);
        compilationOptions.defaultTimeScale = slang::TimeScale(value, value);
    }
    bag.set(compilationOptions);

    slang::ast::Compilation compilation(bag);
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> trees;
    trees.reserve(slangBuffers.size() + 1);
    // A tree whose defparams are moved onto their instances shares syntax with the one parsed,
    // which has to outlive it (see moveDefParams).
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> parsed;
    for (const slang::SourceBuffer& buffer : slangBuffers) {
        parsed.push_back(slang::syntax::SyntaxTree::fromBuffer(buffer, slangSources, bag));
        trees.push_back(moveDefParams(parsed.back()));
        // slang makes no module of a library unit a top level, nor checks one nothing
        // instantiates.
        const auto held = buffers.find(buffer.id);
        trees.back()->isLibraryUnit =
            held != buffers.end()
            && std::ranges::find(options.libraryUnits, held->second) != options.libraryUnits.end();
    }
    mapIncludedBuffers(slangSources, sources, buffers);

    slang::BufferID requestBuffer;
    if (!options.requests.empty() || !options.interfaces.empty()) {
        std::vector<DeclaredModule> declared;
        for (const std::shared_ptr<slang::syntax::SyntaxTree>& tree : trees)
            collectModules(*tree, declared);
        std::optional<std::string> text = requestText(options.requests, declared, diagnostics);
        if (!text) return nullptr;
        *text += interfaceText(options.interfaces);
        const std::string name = "<modules another language instantiates>";
        const BufferId held = sources.addBuffer(name, *text);
        const slang::SourceBuffer buffer = slangSources.assignText(name, sources.bufferText(held));
        buffers.emplace(buffer.id, held);
        requestBuffer = buffer.id;
        trees.push_back(slang::syntax::SyntaxTree::fromBuffer(buffer, slangSources, bag));
    }
    for (const std::shared_ptr<slang::syntax::SyntaxTree>& tree : trees)
        compilation.addSyntaxTree(tree);

    // slang's diagnostics are dramatically better than most commercial tools', and
    // preserving that quality end to end is a genuine selling point rather than polish.
    // They are translated into Enzan's engine so that the transcript is uniform
    // regardless of which phase produced a message.
    //
    // The warnings are slang's default set, as its own driver shows them, and a few more of its
    // checks that point at a likely mistake. Its other checks are of style, or of code that is
    // written on purpose and means what it says, which a design of other tools' writing has by
    // the thousand: they would bury the warnings that matter. A parameter in a specify block is
    // in slang's default set and in every vendor's cell library, and is simulated as a specparam.
    slang::DiagnosticEngine slangEngine(slangSources);
    static const std::vector<std::string> WarningOptions = {
        "width-trunc",     "port-width-trunc", "constant-conversion", "sign-compare",
        "sign-conversion", "unused-result",    "udp-coverage",        "no-specify-param",
    };
    slangEngine.setWarningOptions(WarningOptions);
    bool hasError = false;

    // Elaborated before anything is asked of the design, so that the dead code is known.
    const slang::Diagnostics& slangDiagnostics = compilation.getAllDiagnostics();
    const DeadCode deadCode(compilation.getRoot());
    const NonStandardPaths nonStandardPaths(
        trees, std::span(slangDiagnostics.data(), slangDiagnostics.size()));
    const FileNameArguments fileNameArguments(trees);

    // Where slang's location is in the sources, or nowhere for a buffer they do not hold.
    const auto locationOf = [&](slang::SourceLocation where) {
        if (!where) return SourceLocation();
        const auto found = buffers.find(where.buffer());
        if (found == buffers.end()) return SourceLocation();
        const SourceLocation start = sources.lineStart(
            found->second, static_cast<std::uint32_t>(slangSources.getLineNumber(where)));
        if (!start.valid()) return SourceLocation();
        const std::size_t column = slangSources.getColumnNumber(where);
        return start.advanced(static_cast<std::uint32_t>(column > 0 ? column - 1 : 0));
    };

    // A defparam slang would read at another width than its value's is refused, by name.
    for (const MistypedDefParam& defparam : mistypedDefParams(compilation.getRoot())) {
        diagnostics.report(DiagCode::UnsupportedConstruct, locationOf(defparam.location),
                           defparam.description);
        hasError = true;
    }

    // slang skips an encrypted envelope with a warning and reads on, so a module whose body is
    // encrypted would be translated without it and simulate. Such a file is refused, by one
    // error naming it: what slang says about the rest of it follows from the missing text, such
    // as an `ifndef left open or a header with no `endmodule`.
    std::set<slang::BufferID> encrypted;
    for (const slang::Diagnostic& diagnostic : slangDiagnostics) {
        if (diagnostic.code == slang::diag::ProtectedEnvelope && diagnostic.location)
            encrypted.insert(diagnostic.location.buffer());
    }
    std::set<slang::BufferID> encryptionReported;

    for (const slang::Diagnostic& diagnostic : slangDiagnostics) {
        const slang::DiagnosticSeverity severity =
            slangEngine.getSeverity(diagnostic.code, diagnostic.location);
        if (severity == slang::DiagnosticSeverity::Ignored) continue;

        // A request's module leaves the ports of what it instantiates open, which the instances
        // asking for it connect.
        if (diagnostic.location && diagnostic.location.buffer() == requestBuffer
            && severity != slang::DiagnosticSeverity::Error
            && severity != slang::DiagnosticSeverity::Fatal)
            continue;

        const std::string message = slangEngine.formatMessage(diagnostic);
        const SourceLocation location = locationOf(diagnostic.location);

        if (diagnostic.location && encrypted.contains(diagnostic.location.buffer())) {
            const slang::BufferID buffer = diagnostic.location.buffer();
            if (diagnostic.code == slang::diag::ProtectedEnvelope
                && encryptionReported.insert(buffer).second) {
                diagnostics.report(DiagCode::EncryptedSource, location,
                                   slangSources.getRawFileName(buffer));
                hasError = true;
            }
            continue;
        }

        // A degenerate construct a constant condition rules out is never translated, so it is
        // reported and accepted rather than refused (see DeadCode).
        if (severity == slang::DiagnosticSeverity::Error && deadCode.excuses(diagnostic)) {
            diagnostics.report(DiagCode::ErrorInDeadCode, location, message);
            continue;
        }

        // Two module paths the standard refuses and other simulators accept (see
        // NonStandardPaths).
        if (severity == slang::DiagnosticSeverity::Error) {
            if (const std::optional<DiagCode> code = nonStandardPaths.excuses(diagnostic)) {
                diagnostics.report(*code, location, message);
                continue;
            }
        }

        // A file name held in a vector, which the standard allows `$fopen` and slang refuses
        // (see FileNameArguments); the translation reads its characters.
        if (severity == slang::DiagnosticSeverity::Error && fileNameArguments.excuses(diagnostic))
            continue;

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

    // Collected whether or not the analysis succeeded: a build that failed because a header
    // was wrong still has to rebuild when that header is fixed.
    if (options.filesRead != nullptr) {
        // Sorted, because slang hands them back in whatever order it opened them and nothing
        // observable may depend on that. Duplicates dropped: one file included
        // twice is one prerequisite.
        //
        // Only the sources handed in and what they included are files. slang parses each `-G`
        // from text of its own, in a buffer it names `<unnamed_buffer0>`, and a prerequisite
        // that no file will ever satisfy makes `make` rebuild on every run.
        std::set<std::filesystem::path> opened;
        for (const slang::BufferID buffer : slangSources.getAllBuffers()) {
            if (buffer == requestBuffer) continue;
            if (!buffers.contains(buffer) && !slangSources.getIncludedFrom(buffer).valid())
                continue;
            const std::filesystem::path& path = slangSources.getFullPath(buffer);
            if (!path.empty()) opened.insert(path);
        }
        options.filesRead->assign(opened.begin(), opened.end());
    }

    // What failed may be only that an instance of another language's unit was checked against
    // ports one bit wide, its values having no row of widths yet: what each such instance asks for
    // is still said, for the caller to translate and analyse again.
    if (hasError) {
        if (!options.interfaces.empty() && options.asked != nullptr
            && options.unsettled != nullptr) {
            StandInInstances instances(options.interfaces);
            compilation.getRoot().visit(instances);
            if (instances.guessed) {
                *options.unsettled = true;
                *options.asked = std::move(instances.asked);
            }
        }
        return nullptr;
    }

    const slang::ast::RootSymbol& root = compilation.getRoot();

    // slang applies an override to whichever top level has the parameter and ignores one that
    // none has, which would let a misspelt `-G` change nothing without a word.
    for (const std::string& override : options.parameterOverrides) {
        const std::string_view name = std::string_view(override).substr(0, override.find('='));
        const bool used = std::ranges::any_of(
            root.topInstances, [name](const slang::ast::InstanceSymbol* instance) {
                const slang::ast::Symbol* found = instance->body.find(name);
                return found != nullptr && found->kind == slang::ast::SymbolKind::Parameter;
            });
        if (!used)
            diagnostics.report(DiagCode::UnusedParameterOverride, SourceLocation(), override);
    }

    if (root.topInstances.empty()) {
        diagnostics.report(DiagCode::NoTopLevelFound, SourceLocation());
        return nullptr;
    }

    // Every delay counts the run's unit, which a design asking for a finer precision than the
    // default refines unless something fixed it.
    std::int8_t runTimeUnit = options.fixedRunTimeUnit.value_or(options.timeUnitExponent);
    if (options.refineTimeUnit && !options.fixedRunTimeUnit) {
        FinestPrecision finest(runTimeUnit);
        root.visit(finest);
        for (const ast::PackageSymbol* package : compilation.getPackages())
            finest.consider(package->timeScale);
        for (const ast::CompilationUnitSymbol* unit : compilation.getCompilationUnits())
            finest.consider(unit->timeScale);
        runTimeUnit = finest.value();
    }
    if (options.runTimeUnit != nullptr) *options.runTimeUnit = runTimeUnit;

    auto design = std::make_unique<eir::Design>();
    const ModuleBuilder::Shared shared{.design = *design,
                                       .diagnostics = diagnostics,
                                       .sources = sources,
                                       .slangSources = slangSources,
                                       .buffers = buffers,
                                       .runTimeUnit = runTimeUnit};

    Walker walker(*design, shared);
    std::vector<eir::UnitRequest> asked;
    walker.standIn(options.interfaces, asked);
    for (const slang::ast::InstanceSymbol* instance : root.topInstances) {
        if (!walker.translate(*instance).has_value()) return nullptr;
    }

    if (!walker.ok() || !walker.translatePackages()) return nullptr;
    if (options.asked != nullptr) *options.asked = std::move(asked);

    // Each request is answered by the definition its module's instance became, and its module,
    // which is no part of the design, is dropped.
    if (!requestModules.empty()) {
        if (options.requestAnswers != nullptr) {
            options.requestAnswers->clear();
            for (const std::string& name : requestModules) {
                const eir::ModuleDefinition* found = design->findModule(name);
                options.requestAnswers->push_back(found == nullptr || found->instances.empty()
                                                      ? std::string()
                                                      : found->instances.front().definitionName);
            }
        }
        auto kept = std::make_unique<eir::Design>();
        for (const eir::ModuleDefinition& definition : design->modules()) {
            if (std::ranges::find(requestModules, definition.name) == requestModules.end())
                kept->addModule(definition);
        }
        design = std::move(kept);
    }

    // A frontend's output is verified before it is accepted. The core invariant — a process is a
    // pure function over a declared slice of the arena — is exactly the kind of property a
    // translator gets quietly wrong, and catching it here means the defect is a diagnostic rather
    // than a mysterious wrong value.
    if (!eir::verify(*design, diagnostics)) return nullptr;

    return design;
}

} // namespace enzan::frontend::sv
