// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/Translator.h"

#include "common/Diagnostics.h"
#include "frontend/vhdl/Folder.h"
#include "frontend/vhdl/ProcessTranslator.h"
#include "frontend/vhdl/Types.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

/// One architecture with its generics bound: what one EIR definition is translated from.
struct Specialisation {
    tree_t entity = nullptr;
    tree_t architecture = nullptr;
    Folder::Generics generics;
    std::string name;
};

/// What NVC's `elab_find_arch_cb` gathers: the most recently analysed architecture of an entity.
struct ArchitectureSearch {
    ident_t entity = nullptr;
    ident_t chosen = nullptr;
    timestamp_t modified = 0;
    lib_t library = nullptr;
};

/// The architecture an entity's instance is bound to by default: the one analysed most recently
/// (IEEE 1076 section 7.3.3), breaking a tie of timestamps by the later position in one file, as
/// NVC does.
void considerArchitecture(lib_t library, ident_t name, int kind, void* context) {
    auto& search = *static_cast<ArchitectureSearch*>(context);
    if (kind != T_ARCH || ident_until(name, '-') != search.entity) return;

    const timestamp_t modified = lib_get_mtime(library, name);
    if (search.chosen == nullptr || modified > search.modified) {
        search.chosen = name;
        search.modified = modified;
        return;
    }
    if (modified != search.modified) return;
    tree_t previous = lib_get(library, search.chosen);
    tree_t candidate = lib_get(library, name);
    const bool later =
        previous == nullptr
        || (candidate != nullptr && tree_loc(candidate)->file_ref == tree_loc(previous)->file_ref
            && tree_loc(candidate)->first_line >= tree_loc(previous)->first_line);
    if (later) search.chosen = name;
}

/// `text` as a name is written into a definition's: letters, digits and underscores.
std::string sanitised(std::string text) {
    for (char& character : text) {
        const bool keep =
            std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
        if (!keep) character = '_';
    }
    return text;
}

class DesignTranslator;

/// Translates one architecture, with its entity and its generics, into an EIR definition.
class ArchitectureTranslator final : public ArchitectureScope {
public:
    ArchitectureTranslator(DesignTranslator& owner, const Specialisation& translated);

    eir::ModuleDefinition translate();

    [[nodiscard]] std::optional<std::uint32_t> objectFor(tree_t declaration) const override {
        const auto found = objects.find(declaration);
        if (found == objects.end()) return std::nullopt;
        return found->second;
    }

    [[nodiscard]] const Folder& folder() const override { return folding; }

    [[nodiscard]] SourceLocation locate(tree_t where) const override;

    void unsupported(tree_t where, std::string_view what) override;

    void reportNeverWaits(tree_t process) override;

    [[nodiscard]] const eir::ModuleDefinition& definition() const override { return result; }

    [[nodiscard]] std::int8_t timeUnitExponent() const override;

private:
    /// Adds the object holding a port or a signal, and returns false having reported why it
    /// could not.
    bool addObject(tree_t declaration);

    void declarations(tree_t container);
    void statements(tree_t container);
    void instance(tree_t statement);

    /// The index of the enumeration `type` is in this definition's table, adding it the first
    /// time, or `NoEnum`.
    std::uint32_t enumFor(type_t type);

    DesignTranslator& design;
    const Specialisation& specialisation;
    Folder folding;
    eir::ModuleDefinition result;
    std::map<tree_t, std::uint32_t> objects;
    std::map<type_t, std::uint32_t> enums;
};

/// Walks the hierarchy from the top, translating each architecture and set of generics it reaches
/// once.
class DesignTranslator {
public:
    DesignTranslator(const DesignRequest& translated, const Locate& locate,
                     DiagnosticEngine& diagnostics)
        : request(translated), locator(locate), engine(diagnostics) {}

    std::vector<eir::ModuleDefinition> run() {
        for (tree_t top : tops()) {
            tree_t architecture = architectureOf(top);
            if (architecture == nullptr) {
                engine.report(DiagCode::VhdlError, locate(top),
                              std::format("entity '{}' has no architecture to simulate",
                                          simpleName(tree_ident(top))));
                continue;
            }
            std::optional<Folder::Generics> generics = bindGenerics(top, nullptr, Folder());
            if (!generics) continue;
            static_cast<void>(definitionFor(top, architecture, std::move(*generics)));
        }

        // Breadth first, so that a deep hierarchy does not recurse as deep, and in the order
        // instances are met, so that names and diagnostics come out the same on every run.
        while (!pending.empty()) {
            const Specialisation next = std::move(pending.front());
            pending.pop_front();
            ArchitectureTranslator translator(*this, next);
            modules.push_back(translator.translate());
        }
        return std::move(modules);
    }

    /// The name of the definition `architecture` of `entity` is with `generics` bound, queueing
    /// it for translation the first time it is asked for.
    std::string definitionFor(tree_t entity, tree_t architecture, Folder::Generics generics) {
        std::vector<std::int64_t> values;
        values.reserve(generics.size());
        for (unsigned index = 0; index < tree_generics(entity); ++index) {
            const auto bound = generics.find(tree_generic(entity, index));
            values.push_back(bound == generics.end() ? 0 : bound->second);
        }
        auto key = std::pair(architecture, values);
        if (const auto known = names.find(key); known != names.end()) return known->second;

        std::string name = nameFor(entity, architecture, generics);
        names.emplace(std::move(key), name);
        pending.push_back(Specialisation{.entity = entity,
                                         .architecture = architecture,
                                         .generics = std::move(generics),
                                         .name = name});
        return name;
    }

    /// The architecture an instance of `entity` that names none is bound to.
    tree_t architectureOf(tree_t entity) {
        // One analysed in this run is the one the user just wrote, whatever an older one in the
        // library says about its time.
        for (tree_t unit : std::views::reverse(request.analysed)) {
            if (tree_kind(unit) == T_ARCH && tree_has_primary(unit) && tree_primary(unit) == entity)
                return unit;
        }

        ident_t name = tree_ident(entity);
        ident_t libraryName = ident_until(name, '.');
        lib_t library =
            libraryName == lib_name(request.work) ? request.work : lib_require(libraryName);
        if (library == nullptr) return nullptr;
        ArchitectureSearch search{
            .entity = name, .chosen = nullptr, .modified = 0, .library = library};
        lib_walk_index(library, considerArchitecture, &search);
        return search.chosen == nullptr ? nullptr : lib_get(library, search.chosen);
    }

    /// The values of `entity`'s generics for an instance: what `instance` maps, folded in its
    /// parent's scope, and each default for the rest, folded in the scope of the generics bound
    /// before it. Nothing, having reported why, when one is not a value the adapter can fold.
    std::optional<Folder::Generics> bindGenerics(tree_t entity, tree_t instance,
                                                 const Folder& parent) {
        Folder::Generics bound;
        const unsigned count = tree_generics(entity);
        for (unsigned index = 0; index < count; ++index) {
            tree_t generic = tree_generic(entity, index);
            if (tree_class(generic) != C_CONSTANT) {
                unsupported(generic, "a generic that is not a constant");
                return std::nullopt;
            }

            std::optional<std::int64_t> value;
            bool mapped = false;
            const unsigned maps = instance == nullptr ? 0 : tree_genmaps(instance);
            for (unsigned map = 0; map < maps && !mapped; ++map) {
                tree_t association = tree_genmap(instance, map);
                const bool positional = tree_subkind(association) == P_POS;
                const bool byName = !positional && tree_kind(tree_name(association)) == T_REF
                                    && tree_has_ref(tree_name(association))
                                    && tree_ref(tree_name(association)) == generic;
                if (positional ? tree_pos(association) != index : !byName) continue;
                mapped = true;
                if (tree_kind(tree_value(association)) != T_OPEN)
                    value = parent.integer(tree_value(association));
                else if (tree_has_value(generic))
                    value = Folder(bound).integer(tree_value(generic));
            }
            if (!mapped && tree_has_value(generic))
                value = Folder(bound).integer(tree_value(generic));

            if (!value) {
                unsupported(generic, std::format("the value of the generic '{}', which is not a "
                                                 "static integer, enumeration or time",
                                                 simpleName(tree_ident(generic))));
                return std::nullopt;
            }
            bound.emplace(generic, *value);
        }
        return bound;
    }

    [[nodiscard]] SourceLocation locate(tree_t where) const { return locator(tree_loc(where)); }

    void unsupported(tree_t where, std::string_view what) {
        engine.report(DiagCode::VhdlUnsupported, locate(where), what);
    }

    DiagnosticEngine& diagnostics() { return engine; }

    [[nodiscard]] std::int8_t timeUnitExponent() const { return request.timeUnitExponent; }

private:
    /// The entities the design is translated from.
    std::vector<tree_t> tops() {
        if (!request.top.empty()) {
            std::string wanted = request.top;
            for (char& character : wanted)
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            ident_t name = ident_prefix(lib_name(request.work), ident_new(wanted.c_str()), '.');
            for (tree_t unit : request.analysed) {
                if (tree_kind(unit) == T_ENTITY && tree_ident(unit) == name) return {unit};
            }
            if (tree_t unit = lib_get(request.work, name);
                unit != nullptr && tree_kind(unit) == T_ENTITY)
                return {unit};
            // Left to elaboration, which says what the design does have.
            return {};
        }

        // Every entity this run analysed that no architecture of it instantiates.
        std::set<tree_t> instantiated;
        for (tree_t unit : request.analysed) {
            if (tree_kind(unit) != T_ARCH) continue;
            tree_visit_only(
                unit,
                [](tree_t instance, void* context) {
                    if (!tree_has_ref(instance)) return;
                    tree_t target = tree_ref(instance);
                    if (tree_kind(target) == T_ARCH && tree_has_primary(target))
                        target = tree_primary(target);
                    static_cast<std::set<tree_t>*>(context)->insert(target);
                },
                &instantiated, T_INSTANCE);
        }
        std::vector<tree_t> roots;
        for (tree_t unit : request.analysed) {
            if (tree_kind(unit) == T_ENTITY && !instantiated.contains(unit)) roots.push_back(unit);
        }
        return roots;
    }

    /// The entity's name, with the architecture when another of the entity's architectures has
    /// the plain name, and the generics' values when it has any, as the SystemVerilog frontend
    /// names a parameterisation.
    std::string nameFor(tree_t entity, tree_t architecture, const Folder::Generics& generics) {
        std::string base = simpleName(tree_ident(entity));
        const auto owner = baseOwners.try_emplace(base, architecture).first;
        if (owner->second != architecture) base += "$" + simpleName(tree_ident(architecture));

        std::string values;
        for (unsigned index = 0; index < tree_generics(entity); ++index) {
            tree_t generic = tree_generic(entity, index);
            if (!values.empty()) values += ',';
            values += simpleName(tree_ident(generic));
            values += '=';
            values += std::to_string(generics.at(generic));
        }
        if (values.empty()) return uniqueName(base);

        std::string suffix;
        if (values.size() <= 32) {
            suffix = sanitised(values);
        } else {
            // FNV-1a, as the SystemVerilog frontend uses: stable across runs and machines.
            std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
            for (const char character : values) {
                hash ^= static_cast<unsigned char>(character);
                hash *= 0x0000'0100'0000'01b3ULL;
            }
            suffix = std::format("{:016x}", hash);
        }
        return uniqueName(base + "$" + suffix);
    }

    std::string uniqueName(std::string preferred) {
        if (taken.insert(preferred).second) return preferred;
        for (unsigned suffix = 1;; ++suffix) {
            std::string attempt = std::format("{}${}", preferred, suffix);
            if (taken.insert(attempt).second) return attempt;
        }
    }

    const DesignRequest& request;
    const Locate& locator;
    DiagnosticEngine& engine;

    std::deque<Specialisation> pending;
    std::vector<eir::ModuleDefinition> modules;
    std::map<std::pair<tree_t, std::vector<std::int64_t>>, std::string> names;
    std::map<std::string, tree_t> baseOwners;
    std::set<std::string> taken;
};

ArchitectureTranslator::ArchitectureTranslator(DesignTranslator& owner,
                                               const Specialisation& translated)
    : design(owner), specialisation(translated), folding(translated.generics) {}

SourceLocation ArchitectureTranslator::locate(tree_t where) const {
    return design.locate(where);
}

void ArchitectureTranslator::unsupported(tree_t where, std::string_view what) {
    design.unsupported(where, what);
}

void ArchitectureTranslator::reportNeverWaits(tree_t process) {
    design.diagnostics().report(DiagCode::VhdlProcessNeverWaits, locate(process));
}

std::int8_t ArchitectureTranslator::timeUnitExponent() const {
    return design.timeUnitExponent();
}

std::uint32_t ArchitectureTranslator::enumFor(type_t type) {
    type_t base = type_base_recur(type);
    if (const auto known = enums.find(base); known != enums.end()) return known->second;
    std::optional<eir::EnumDefinition> enumeration = enumerationOf(base);
    if (!enumeration) return eir::Object::NoEnum;
    const auto index = static_cast<std::uint32_t>(result.enums.size());
    result.enums.push_back(std::move(*enumeration));
    enums.emplace(base, index);
    return index;
}

bool ArchitectureTranslator::addObject(tree_t declaration) {
    type_t type = tree_type(declaration);
    const std::optional<Layout> layout = layoutOf(type, folding);
    if (!layout) {
        unsupported(declaration, std::format("a signal of type {}", type_pp(type)));
        return false;
    }

    // Every VHDL object has a value from the start: its own initial value, or its type's left
    // bound (IEEE 1076 section 14.4.2.5). A port's is the default it declares, which is what an
    // unconnected port reads; a connected one is the signal it connects.
    std::optional<eir::BitVector> initial;
    if (tree_has_value(declaration)) {
        initial = constantBits(tree_value(declaration), *layout, folding);
        if (!initial) {
            unsupported(tree_value(declaration), "an initial value that is not static");
            return false;
        }
    } else {
        initial = defaultBits(type, *layout, folding);
        if (!initial) {
            unsupported(declaration, std::format("the initial value of a {}", type_pp(type)));
            return false;
        }
    }

    eir::Object object;
    object.name = simpleName(tree_ident(declaration));
    object.type = layout->storage;
    object.kind = eir::ObjectKind::Variable;
    object.location = locate(declaration);
    object.initialValue = std::move(initial);
    object.enumIndex = layout->isArray() ? eir::Object::NoEnum : enumFor(type);
    objects.emplace(declaration, result.addObject(std::move(object)));
    return true;
}

eir::ModuleDefinition ArchitectureTranslator::translate() {
    tree_t entity = specialisation.entity;
    result.name = specialisation.name;
    result.sourceName = simpleName(tree_ident(entity));
    result.location = locate(specialisation.architecture);

    for (unsigned index = 0; index < tree_generics(entity); ++index) {
        tree_t generic = tree_generic(entity, index);
        const std::optional<eir::Type> storage = scalarStorage(tree_type(generic));
        if (!storage) continue;
        result.parameters.push_back(
            eir::Parameter{.name = simpleName(tree_ident(generic)),
                           .value = encode(specialisation.generics.at(generic), *storage),
                           .isLocal = false,
                           .location = locate(generic)});
    }

    for (unsigned index = 0; index < tree_ports(entity); ++index) {
        tree_t port = tree_port(entity, index);
        if (!addObject(port)) continue;
        const std::uint32_t objectIndex = objects.at(port);

        eir::PortDirection direction = eir::PortDirection::Input;
        switch (tree_subkind(port)) {
        case PORT_IN: direction = eir::PortDirection::Input; break;
        case PORT_OUT:
        case PORT_BUFFER: direction = eir::PortDirection::Output; break;
        case PORT_INOUT: direction = eir::PortDirection::InOut; break;
        default: unsupported(port, "a port of this mode"); continue;
        }
        result.ports.push_back(eir::Port{.name = simpleName(tree_ident(port)),
                                         .direction = direction,
                                         .objectIndex = objectIndex,
                                         .location = locate(port)});
    }

    declarations(entity);
    declarations(specialisation.architecture);
    statements(entity);
    statements(specialisation.architecture);
    return std::move(result);
}

void ArchitectureTranslator::declarations(tree_t container) {
    const unsigned count = tree_decls(container);
    for (unsigned index = 0; index < count; ++index) {
        tree_t declaration = tree_decl(container, index);
        switch (tree_kind(declaration)) {
        case T_SIGNAL_DECL: static_cast<void>(addObject(declaration)); break;

        // Folded where they are used, so they need nothing of their own.
        case T_CONST_DECL:
        case T_TYPE_DECL:
        case T_SUBTYPE_DECL:
        case T_COMPONENT:
        case T_ATTR_DECL:
        case T_ATTR_SPEC:
        case T_USE:
        case T_FUNC_DECL:
        case T_PROC_DECL: break;

        default:
            unsupported(declaration,
                        std::format("the declaration {}", tree_kind_str(tree_kind(declaration))));
            break;
        }
    }
}

void ArchitectureTranslator::statements(tree_t container) {
    const unsigned count = tree_stmts(container);
    for (unsigned index = 0; index < count; ++index) {
        tree_t statement = tree_stmt(container, index);
        switch (tree_kind(statement)) {
        case T_PROCESS: {
            ProcessTranslator translator(*this, statement, simpleName(tree_ident(statement)));
            if (std::optional<eir::Process> translated = translator.translate())
                result.processes.push_back(std::move(*translated));
            break;
        }

        case T_INSTANCE: instance(statement); break;

        default:
            unsupported(statement, std::format("the concurrent statement {}",
                                               tree_kind_str(tree_kind(statement))));
            break;
        }
    }
}

void ArchitectureTranslator::instance(tree_t statement) {
    tree_t target = tree_ref(statement);
    tree_t entity = nullptr;
    tree_t architecture = nullptr;
    switch (tree_kind(target)) {
    case T_ENTITY:
        entity = target;
        architecture = design.architectureOf(entity);
        break;
    case T_ARCH:
        architecture = target;
        entity = tree_primary(target);
        break;
    default: unsupported(statement, "an instance of a component or a configuration"); return;
    }
    if (architecture == nullptr) {
        design.diagnostics().report(
            DiagCode::VhdlError, locate(statement),
            std::format("entity '{}' has no architecture", simpleName(tree_ident(entity))));
        return;
    }

    std::optional<Folder::Generics> generics = design.bindGenerics(entity, statement, folding);
    if (!generics) return;

    eir::Instance created;
    created.name = simpleName(tree_ident(statement));
    created.definitionName = design.definitionFor(entity, architecture, std::move(*generics));
    created.location = locate(statement);

    const unsigned count = tree_params(statement);
    for (unsigned index = 0; index < count; ++index) {
        tree_t association = tree_param(statement, index);

        std::optional<std::uint32_t> portIndex;
        if (tree_subkind(association) == P_POS) {
            portIndex = tree_pos(association);
        } else {
            tree_t formal = tree_name(association);
            if (tree_kind(formal) != T_REF || !tree_has_ref(formal)) {
                unsupported(formal, "associating part of a port");
                continue;
            }
            for (unsigned port = 0; port < tree_ports(entity); ++port) {
                if (tree_port(entity, port) == tree_ref(formal)) portIndex = port;
            }
        }
        const std::uint32_t portNumber = portIndex.value_or(tree_ports(entity));
        if (portNumber >= tree_ports(entity)) {
            unsupported(association, "this port association");
            continue;
        }

        // An open port keeps its own storage and its default, so it needs no connection at all.
        tree_t actual = tree_value(association);
        if (tree_kind(actual) == T_OPEN) continue;

        std::uint32_t objectIndex = eir::PortConnection::Unconnected;
        if (tree_kind(actual) == T_REF && tree_has_ref(actual))
            objectIndex = objectFor(tree_ref(actual)).value_or(eir::PortConnection::Unconnected);

        // A static actual, as an input tied to a constant is: an object holding the value, which
        // nothing drives, connects as a signal does.
        if (objectIndex == eir::PortConnection::Unconnected) {
            tree_t port = tree_port(entity, portNumber);
            const std::optional<Layout> layout = layoutOf(tree_type(port), folding);
            std::optional<eir::BitVector> value;
            if (layout) value = constantBits(actual, *layout, folding);
            if (!layout || !value || tree_subkind(port) != PORT_IN) {
                unsupported(actual, "a port actual that is not a signal or a static value");
                continue;
            }
            eir::Object tied;
            tied.name = std::format("{}${}", created.name, simpleName(tree_ident(port)));
            tied.type = layout->storage;
            tied.location = locate(actual);
            tied.initialValue = std::move(value);
            tied.enumIndex = layout->isArray() ? eir::Object::NoEnum : enumFor(tree_type(port));
            objectIndex = result.addObject(std::move(tied));
        }

        created.connections.push_back(
            eir::PortConnection{.portIndex = portNumber, .objectIndex = objectIndex});
    }
    result.instances.push_back(std::move(created));
}

} // namespace

std::vector<eir::ModuleDefinition>
translateDesign(const DesignRequest& request, const Locate& locate, DiagnosticEngine& diagnostics) {
    DesignTranslator translator(request, locate, diagnostics);
    return translator.run();
}

} // namespace enzan::frontend::vhdl
