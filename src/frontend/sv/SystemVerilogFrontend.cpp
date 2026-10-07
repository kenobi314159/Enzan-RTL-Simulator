// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/SystemVerilogFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "eir/Module.h"
#include "eir/Verifier.h"
#include "frontend/sv/DeadCode.h"
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
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/numeric/Time.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
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

/// The name an instance has in the module holding it: its own, or its array's with its index.
std::string instanceName(const ast::InstanceSymbol& instance) {
    if (instance.arrayPath.empty()) return std::string(instance.name);
    return std::string(instance.getArrayName()) + arraySuffixOf(instance);
}

/// The name `symbol` has in the module that declares it, its generate blocks joined to it with
/// dots as `collectScope` names a hoisted object or an instance (DESIGN_GUIDE.md section 5.1),
/// and the body that module's instance has; null for the top level's instance, which no body
/// holds. Nothing when the symbol is somewhere a name cannot reach, a subroutine or a package.
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

    /// The package that declares `symbol` directly, if one does.
    static const ast::PackageSymbol* packageOf(const ast::ValueSymbol& symbol);

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

private:
    /// Walks a scope, collecting declarations and remembering what to translate later.
    bool collectScope(const ast::Scope& scope, const std::string& prefix);

    /// The EIR kind of a slang net kind, or nothing for one Enzan cannot resolve.
    static std::optional<eir::NetKind> convertNetKind(ast::NetType::NetKind kind);

    /// Creates the object a hierarchical reference to `symbol`, declared in `owner` as `name`,
    /// is: one with no storage and a path from this instance to that object.
    std::optional<std::uint32_t> referenceTo(const ast::ValueSymbol& symbol,
                                             const ast::InstanceBodySymbol& owner,
                                             const std::string& name, eir::Type type);

    /// Records what `connection`, an interface port of `instance`, is connected to here.
    bool bindInterface(const ast::InstanceSymbol& instance, const ast::PortConnection& connection,
                       eir::Instance& result);

    /// Creates the object for a reference whose path is already known.
    std::optional<std::uint32_t> referenceThrough(const ast::ValueSymbol& symbol, std::string path,
                                                  eir::Type type);

    /// Reports a drive strength other than the default and returns false.
    ///
    /// Enzan resolves drivers without a strength model (DESIGN_GUIDE.md section 3.7), which
    /// gives the right answer only when every driver is strong: a `weak` driver contending with
    /// a strong one would resolve to X where the strong one should win.
    bool checkDriveStrength(
        std::pair<std::optional<ast::DriveStrength>, std::optional<ast::DriveStrength>> strength,
        slang::SourceLocation location);

    /// Collects the elements of an instance array, and of the arrays nested in it.
    void collectInstanceArray(const ast::InstanceArraySymbol& array, const std::string& prefix) {
        for (const ast::Symbol* element : array.elements) {
            if (element->kind == slang::ast::SymbolKind::InstanceArray) {
                collectInstanceArray(element->as<ast::InstanceArraySymbol>(), prefix);
            } else if (element->kind == slang::ast::SymbolKind::Instance) {
                instances.push_back(CollectedInstance{
                    .prefix = prefix, .symbol = &element->as<ast::InstanceSymbol>()});
            }
        }
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
    std::map<std::string, std::uint32_t, std::less<>> portIndexByName;

    std::vector<std::pair<std::string, const ast::ProceduralBlockSymbol*>> proceduralBlocks;
    std::vector<const ast::ContinuousAssignSymbol*> continuousAssignments;

    /// The interface ports of this body, in declaration order.
    std::vector<const ast::InterfacePortSymbol*> interfacePorts;

    /// How this body reaches `instance` through one of its interface ports: `@port`, or
    /// `@port[2]` when the port is an array and the instance one of its elements. Empty when no
    /// interface port reaches it.
    [[nodiscard]] std::string portReaching(const ast::InstanceSymbol& instance) const {
        for (const ast::InterfacePortSymbol* port : interfacePorts) {
            const ast::Symbol* connected = port->getConnection().first;
            if (connected == &instance) return std::format("@{}", port->name);
            if (connected != nullptr && connected->kind == slang::ast::SymbolKind::InstanceArray
                && containsElement(connected->as<ast::InstanceArraySymbol>(), instance)) {
                return std::format("@{}{}", port->name, arraySuffixOf(instance));
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

    static std::size_t countElements(const ast::InstanceArraySymbol& array) {
        std::size_t count = 0;
        for (const ast::Symbol* element : array.elements) {
            count += element->kind == slang::ast::SymbolKind::InstanceArray
                         ? countElements(element->as<ast::InstanceArraySymbol>())
                         : 1;
        }
        return count;
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
    // Packed types — vectors, packed structs, packed arrays, enums — are all integral to
    // slang and all behave as bit vectors, so they convert for free.
    if (type.isIntegral()) {
        const slang::bitwidth_t width = type.getBitWidth();
        if (width == 0) return std::nullopt;
        return eir::Type::bits(width, type.isSigned(), type.isFourState());
    }

    // A fixed-size unpacked array and an unpacked struct are stored flat, as the vector of all
    // their bits (DESIGN_GUIDE.md section 4.3): every operation the language allows on one as
    // a whole — assignment, equality, a port connection — is then the same operation on bits.
    // Reals, strings, unions, dynamic arrays and classes are none of these, and are reported
    // where they are used, so that the diagnostic points at the user's line.
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

const ast::PackageSymbol* ModuleBuilder::packageOf(const ast::ValueSymbol& symbol) {
    const ast::Scope* scope = symbol.getParentScope();
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
    const ast::PackageSymbol* const declaringPackage = packageOf(symbol);
    if (declaringPackage != nullptr && declaringPackage != package) {
        std::vector<const ast::ValueSymbol*>& used = *shared->packageVariables;
        if (std::ranges::find(used, &symbol) == used.end()) used.push_back(&symbol);
        return referenceThrough(symbol, std::format("/{}/{}", declaringPackage->name, symbol.name),
                                *type);
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
        if (owner != body) {
            // Reached through an interface port, the reference names the port rather than the
            // interface it happens to be connected to here: one definition serves every
            // instance, and each may be connected to a different one.
            if (owner->parentInstance != nullptr) {
                if (const std::string port = portReaching(*owner->parentInstance); !port.empty())
                    return referenceThrough(symbol, std::format("{}/{}", port, *nameThere), *type);
            }
            return referenceTo(symbol, *owner, *nameThere, *type);
        }
    }

    eir::Object object;
    object.name = uniqueObjectName(symbol.name);
    object.type = *type;
    object.kind = symbol.kind == slang::ast::SymbolKind::Net ? eir::ObjectKind::Net
                                                             : eir::ObjectKind::Variable;
    object.location = convertLocation(symbol.location);
    object.enumIndex = enumFor(symbol.getType());

    // An unpacked array keeps its declared dimensions, outermost first, for what reads it as an
    // array rather than as the bits it is stored as: VPI, and the waveform database.
    for (const ast::Type* level = &symbol.getType().getCanonicalType();
         level->kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType;) {
        const auto& array = level->as<ast::FixedSizeUnpackedArrayType>();
        object.unpackedDimensions.push_back(
            eir::UnpackedDimension{.left = array.range.left, .right = array.range.right});
        level = &array.elementType.getCanonicalType();
        if (level->kind != slang::ast::SymbolKind::FixedSizeUnpackedArrayType) {
            if (const std::optional<eir::Type> element = convertType(*level)) {
                object.elementWidth = element->bitWidth();
                object.elementSigned = element->isSigned();
            }
        }
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
        if (!checkDriveStrength(net.getDriveStrength(), symbol.location)) return std::nullopt;
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
            instances.push_back(
                CollectedInstance{.prefix = prefix, .symbol = &member.as<ast::InstanceSymbol>()});
            break;

        case slang::ast::SymbolKind::InstanceArray:
            // Each element is an instance of its own, named with its index.
            collectInstanceArray(member.as<ast::InstanceArraySymbol>(), prefix);
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
        if (!checkDriveStrength(assignment->getDriveStrength(), assignment->location)) continue;

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

        definitionValue.processes.push_back(translator.take());
    }

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
        target = name.has_value() && owner == body ? *name : portReaching(interface);
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
                        connected == nullptr ? std::string_view("nothing")
                                             : slang::ast::toString(connected->kind)));
        return false;
    }
    result.interfaceBindings.push_back(
        eir::InterfaceBinding{.port = std::string(connection.port.name), .target = target});
    return true;
}

std::optional<std::uint32_t> ModuleBuilder::referenceTo(const ast::ValueSymbol& symbol,
                                                        const ast::InstanceBodySymbol& owner,
                                                        const std::string& name, eir::Type type) {
    const std::optional<std::vector<std::string>> here = instanceChain(*body);
    const std::optional<std::vector<std::string>> there = instanceChain(owner);
    if (!here.has_value() || !there.has_value()) {
        reportUnsupported(symbol.location,
                          std::format("the hierarchical reference to '{}'", symbol.name));
        return std::nullopt;
    }

    // Up from here to the instance both paths share, then down to the one declaring it.
    std::size_t common = 0;
    while (common < here->size() && common < there->size() && (*here)[common] == (*there)[common])
        ++common;
    std::string path;
    for (std::size_t level = common; level < here->size(); ++level) path += "../";
    for (std::size_t level = common; level < there->size(); ++level) path += (*there)[level] + "/";
    path += name;

    return referenceThrough(symbol, std::move(path), type);
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

bool ModuleBuilder::checkDriveStrength(
    std::pair<std::optional<ast::DriveStrength>, std::optional<ast::DriveStrength>> strength,
    slang::SourceLocation location) {
    const auto isDefault = [](std::optional<ast::DriveStrength> value) {
        return !value.has_value() || *value == ast::DriveStrength::Strong;
    };
    if (isDefault(strength.first) && isDefault(strength.second)) return true;

    reportUnsupported(location, "a drive strength other than strong");
    return false;
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

/// Walks the elaborated hierarchy, translating each distinct module body once.
class Walker {
public:
    Walker(eir::Design& target, ModuleBuilder::Shared sharedState)
        : design(&target), shared(sharedState) {
        shared.packageVariables = &packageVariables;
    }

    /// Translates `instance`'s body if it has not been seen, and returns its EIR name.
    std::optional<std::string> translate(const ast::InstanceSymbol& instance);

    /// Adds a definition for each package whose variables the translated modules refer to.
    bool translatePackages();

    [[nodiscard]] bool ok() const noexcept { return succeeded; }

private:
    struct BodyInfo {
        std::string name;
        std::map<std::string, std::uint32_t, std::less<>> portIndices;
    };

    /// Returns the EIR name for one parameterisation of a module.
    ///
    /// slang caches one body per distinct parameterisation, so a parameterised module
    /// produces several. Each becomes its own EIR definition — which is what keeps parameters
    /// out of EIR entirely (see eir/Module.h) — and a suffix distinguishes them.
    ///
    /// **The suffix is a function of the parameter values, not a counter.** A counter numbers
    /// the parameterisations in the order *this* compilation happened to walk them, which makes
    /// the name meaningless outside it: `counter$1` from one analysis and `counter$1` from
    /// another would be different modules wearing one name, and a design assembled from two
    /// libraries (DESIGN_GUIDE.md section 8.2) would resolve an instance to the wrong body with
    /// nothing to say so. Deriving it from the values instead gives a specialisation the same
    /// name wherever it is produced, which is what lets a library be instantiated with
    /// parameters chosen by whoever uses it.
    static std::string specialisedModuleName(const ast::InstanceBodySymbol& body);

    eir::Design* design;
    ModuleBuilder::Shared shared;
    /// Bodies already translated, by the EIR name they were given.
    ///
    /// Keyed by name and not by `InstanceBodySymbol*`: slang hands out a separate body symbol
    /// per instance even when the parameterisation is identical, so a pointer never matches and
    /// every instance of a module was translated again and then discarded by the design's
    /// replace-by-name. The name is a function of the definition and the parameters set on it
    /// (`specialisedModuleName`), so two bodies sharing a name are the same module and one
    /// translation serves every instance of it.
    ///
    /// This is only sound because `BodyInfo::portIndices` is keyed by port name. A map keyed on
    /// slang's port symbols belongs to the one body it came from, and reusing it for another
    /// binds no connections at all — quietly, with the design still elaborating and simulating
    /// to the wrong answer.
    std::map<std::string, BodyInfo, std::less<>> bodies;
    std::vector<const ast::ValueSymbol*> packageVariables;
    bool succeeded = true;
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

std::string Walker::specialisedModuleName(const ast::InstanceBodySymbol& body) {
    std::string base(body.getDefinition().name);

    // Only the parameters set from outside: a localparam is derived inside the module from
    // those, so two instances agreeing on the ports agree on it, and including it would make
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
        // FNV-1a over the rendered values. Stable across runs, builds and machines, which a
        // std::hash is not required to be.
        std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
        for (const char c : values) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 0x0000'0100'0000'01b3ULL;
        }
        suffix = std::format("{:016x}", hash);
    }

    return std::format("{}${}", base, suffix);
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

    std::string name = specialisedModuleName(body);

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

    for (std::size_t index = 0; index < options.files.size(); ++index) {
        const std::filesystem::path& path = options.files[index];

        BufferId text = index < options.buffers.size() ? options.buffers[index] : BufferId();
        if (!text.valid()) {
            const FileReadResult result = sources.readFile(path);
            if (!result.ok()) {
                reportFileReadFailure(diagnostics, path, result);
                return nullptr;
            }
            text = result.buffer;
        }

        const slang::SourceBuffer buffer =
            slangSources.assignText(path.string(), sources.bufferText(text));
        buffers.emplace(buffer.id, text);
        slangBuffers.push_back(buffer);
    }

    slang::Bag bag;
    slang::parsing::PreprocessorOptions preprocessorOptions;
    preprocessorOptions.predefines = options.defines;
    preprocessorOptions.additionalIncludePaths = options.includeDirectories;
    bag.set(preprocessorOptions);

    slang::ast::CompilationOptions compilationOptions;
    for (const std::string& top : options.topModules) compilationOptions.topModules.insert(top);
    compilationOptions.paramOverrides = options.parameterOverrides;
    if (const std::optional<slang::TimeUnit> unit = timeUnitOf(options.timeUnitExponent)) {
        const slang::TimeScaleValue value(*unit, slang::TimeScaleMagnitude::One);
        compilationOptions.defaultTimeScale = slang::TimeScale(value, value);
    }
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

    // Elaborated before anything is asked of the design, so that the dead code is known.
    const slang::Diagnostics& slangDiagnostics = compilation.getAllDiagnostics();
    const DeadCode deadCode(compilation.getRoot());

    for (const slang::Diagnostic& diagnostic : slangDiagnostics) {
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

        // A degenerate construct a constant condition rules out is never translated, so it is
        // reported and accepted rather than refused (see DeadCode).
        if (severity == slang::DiagnosticSeverity::Error && deadCode.excuses(diagnostic)) {
            diagnostics.report(DiagCode::ErrorInDeadCode, location, message);
            continue;
        }

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
        // Sorted, because slang hands them back in whatever order it opened them and section
        // 3.8 puts observable ordering beyond that. Duplicates dropped: one file included
        // twice is one prerequisite.
        std::set<std::filesystem::path> opened;
        for (const slang::BufferID buffer : slangSources.getAllBuffers()) {
            const std::filesystem::path& path = slangSources.getFullPath(buffer);
            if (!path.empty()) opened.insert(path);
        }
        options.filesRead->assign(opened.begin(), opened.end());
    }

    if (hasError) return nullptr;

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

    if (!walker.ok() || !walker.translatePackages()) return nullptr;

    // A frontend's output is verified before it is accepted. Section 4.2's core invariant —
    // a process is a pure function over a declared slice of the arena — is exactly the kind
    // of property a translator gets quietly wrong, and catching it here means the defect is
    // a diagnostic rather than a mysterious wrong value.
    if (!eir::verify(*design, diagnostics)) return nullptr;

    return design;
}

} // namespace enzan::frontend::sv
