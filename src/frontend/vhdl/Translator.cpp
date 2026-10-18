// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/Translator.h"

#include "common/Diagnostics.h"
#include "eir/BitOps.h"
#include "frontend/vhdl/BodyTranslator.h"
#include "frontend/vhdl/Folder.h"
#include "frontend/vhdl/Types.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

/// One architecture with its generics bound: what one EIR definition is translated from.
struct Specialisation {
    tree_t entity = nullptr;
    tree_t architecture = nullptr;
    Folder generics;
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
class ArchitectureTranslator final : public DefinitionScope {
public:
    ArchitectureTranslator(DesignTranslator& owner, const Specialisation& translated);

    eir::ModuleDefinition translate();

    [[nodiscard]] std::optional<std::uint32_t> objectFor(tree_t declaration) const override {
        if (const auto found = objects.find(declaration); found != objects.end())
            return found->second;
        // An alias of a whole signal is that signal, so a port associated with one is collapsed
        // onto it as onto the signal.
        if (tree_kind(declaration) == T_ALIAS && tree_has_value(declaration)) {
            tree_t value = tree_value(declaration);
            if (tree_kind(value) == T_REF && tree_has_ref(value)) return objectFor(tree_ref(value));
        }
        return std::nullopt;
    }

    [[nodiscard]] const Folder& folder() const override { return folding; }

    [[nodiscard]] SourceLocation locate(tree_t where) const override;

    void unsupported(tree_t where, std::string_view what) override;

    void reportNeverWaits(tree_t process) override;

    [[nodiscard]] const eir::ModuleDefinition& definition() const override { return result; }

    [[nodiscard]] std::int8_t timeUnitExponent() const override;

    [[nodiscard]] std::optional<Callee> callee(tree_t declaration, const Shape& shape,
                                               tree_t where) override;

    /// Adds a subprogram specialised for a call from this architecture.
    void addSubprogram(eir::Subprogram translated) {
        result.subprograms.push_back(std::move(translated));
    }

private:
    /// Adds the object holding a port or a signal, and returns false having reported why it
    /// could not.
    bool addObject(tree_t declaration);

    /// Records a constant the architecture declares, with its value folded, as a parameter local
    /// to the definition, which VHPI shows as a constant as it shows a generic (DESIGN_GUIDE.md
    /// section 9.7). One whose value does not fold is computed where it is used, and shows as
    /// nothing.
    void addConstant(tree_t declaration);

    void declarations(tree_t container);
    void statements(tree_t container);
    void instance(tree_t statement);

    /// The index among `entity`'s ports of the one `formalPort`, a port of the entity or of the
    /// component instantiated, is: as `binding`'s port map associates them, or else by name.
    static std::optional<unsigned> entityPortFor(tree_t entity, tree_t binding, tree_t formalPort);

    /// True when `port` is of an unconstrained array type, whose bounds its actual gives it.
    static bool unconstrained(tree_t port);

    /// True when `type` is a record with a field of a resolved subtype, at any depth.
    static bool hasResolvedField(type_t type);

    /// Makes each port of mode out, inout or buffer a source of its actual where nothing in the
    /// architecture drives it, as every such port is (IEEE 1076 section 14.7.3.2): its bits
    /// that no process drives, and no instance's port, are driven at its default for ever.
    void addPortSources(tree_t entity);

    /// The bounds `port`, of an unconstrained array type, takes from `associations` (IEEE 1076
    /// section 6.5.6.3): its actual's as a whole, or those its formals' indices span when it is
    /// associated in parts, or its default's, folded in `portScope`, when it has no actual.
    /// Nothing, having reported why, when they are not static.
    std::optional<Folder::Range> actualBounds(tree_t port, const std::vector<tree_t>& associations,
                                              tree_t where, const Folder& portScope);

    /// The objects of this definition a port connected through stands for: the one it reads,
    /// and for an inout port the one its drivers drive, which is otherwise `Unconnected`.
    struct Through {
        std::uint32_t object = 0;
        std::uint32_t driven = eir::PortConnection::Unconnected;
    };

    /// Connects `port` through an object of this definition named `objectName` standing for it,
    /// kept equal to its actuals by a process for each of `associations`: for a port associated in
    /// parts, or with part of a signal, a conversion or an expression (DESIGN_GUIDE.md section
    /// 5.2). An inout port has a second object for its driving value, and a process for each
    /// association each way. The associations' formals name `formalPort`, the port itself or a
    /// component's of its name. Nothing, having reported why, when the port cannot be connected.
    std::optional<Through> connectThrough(std::string objectName, tree_t port, tree_t formalPort,
                                          const std::vector<tree_t>& associations,
                                          const Folder& childGenerics);

    /// The index among `formals`' ports of the one `association` associates, whole, in part or
    /// converted; nothing when it names none of them.
    static std::optional<unsigned> formalIndex(tree_t association, tree_t formals);

    /// The statements of a generate statement's chosen alternative, or of each of its loop's
    /// iterations, or of a block, flattened into this definition.
    void ifGenerate(tree_t statement);
    void forGenerate(tree_t statement);
    void block(tree_t statement);

    /// Translates the declarations and statements of `container` with what they declare named
    /// inside `name`, as `name.` begins each, and forgets them after: an iteration of a loop
    /// declares them again.
    void nested(tree_t container, const std::string& name);

    /// The index of the enumeration `type` is in this definition's table, adding it the first
    /// time, or `NoEnum`.
    std::uint32_t enumFor(type_t type);

    /// The index of `type`, laid out as `layout`, in this definition's table of VHDL types,
    /// adding it and the types it is made of the first time; VhdlType::None for one the table
    /// cannot describe.
    std::uint32_t vhdlTypeFor(type_t type, const Layout& layout);

    DesignTranslator& design;
    const Specialisation& specialisation;
    Folder folding;
    eir::ModuleDefinition result;
    std::map<tree_t, std::uint32_t> objects;
    std::map<type_t, std::uint32_t> enums;

    /// The objects associated as a whole with a port of an instance that is a source of them, of
    /// mode out, inout or buffer, which drives them whatever this architecture does.
    std::set<std::uint32_t> sourcedByInstances;

    /// What a name declared where translation is begins with: the generate blocks and blocks it
    /// is inside, each followed by a dot, as SystemVerilog's are (DESIGN_GUIDE.md section 5.1).
    std::string prefix;
};

/// The definition a package's subprograms are translated into: one with no objects, which a call
/// needs no instance of (DESIGN_GUIDE.md section 4.1).
class PackageScope final : public DefinitionScope {
public:
    PackageScope(DesignTranslator& owner, eir::ModuleDefinition& package)
        : design(owner), module(package) {}

    [[nodiscard]] std::optional<std::uint32_t> objectFor(tree_t /*declaration*/) const override {
        return std::nullopt;
    }

    [[nodiscard]] const Folder& folder() const override { return folding; }

    [[nodiscard]] SourceLocation locate(tree_t where) const override;

    void unsupported(tree_t where, std::string_view what) override;

    void reportNeverWaits(tree_t process) override;

    [[nodiscard]] const eir::ModuleDefinition& definition() const override { return module; }

    [[nodiscard]] std::int8_t timeUnitExponent() const override;

    [[nodiscard]] std::optional<Callee> callee(tree_t declaration, const Shape& shape,
                                               tree_t where) override;

    void addSubprogram(eir::Subprogram translated) {
        module.subprograms.push_back(std::move(translated));
    }

private:
    DesignTranslator& design;
    eir::ModuleDefinition& module;
    Folder folding;
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
            std::optional<Folder> generics = bindGenerics(top, nullptr, Folder());
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
        return takePackages();
    }

    /// Each requested resolution function, specialised to its driver count: an array parameter
    /// as long as that, as `driversRange` gives its bounds.
    std::vector<eir::ModuleDefinition> resolve(const std::vector<ResolutionRequest>& requests,
                                               std::vector<ResolutionAnswer>& answers) {
        for (const ResolutionRequest& wanted : requests) {
            tree_t declaration = resolutionFunction(wanted.function);
            if (declaration == nullptr) {
                engine.report(DiagCode::VhdlError, SourceLocation(),
                              std::format("the resolution function {} could not be found again",
                                          wanted.function));
                continue;
            }
            const std::optional<Folder::Range> bounds = driversRange(declaration, wanted.drivers);
            if (!bounds) {
                unsupported(declaration, "a resolution function whose parameter's index does "
                                         "not fold");
                continue;
            }
            const Shape shape{.bounds = {*bounds}, .values = {std::nullopt}};
            const std::optional<Callee> made = callee(declaration, shape, declaration, nullptr);
            if (!made) continue;
            answers.push_back(
                ResolutionAnswer{.definition = made->definition, .subprogram = made->name});
        }
        return takePackages();
    }

    /// Every package definition translated so far, which a run hands over at its end.
    std::vector<eir::ModuleDefinition> takePackages() {
        for (auto& [name, package] : packages) modules.push_back(std::move(package.module));
        return std::move(modules);
    }

    /// The function a resolution names, `IEEE.STD_LOGIC_1164.RESOLVED(Y)U`: declared by the
    /// package, the entity or the architecture its first two parts name, or null.
    [[nodiscard]] tree_t resolutionFunction(const std::string& function) const {
        const std::size_t library = function.find('.');
        const std::size_t unit =
            library == std::string::npos ? std::string::npos : function.find('.', library + 1);
        if (unit == std::string::npos) return nullptr;
        ident_t libraryName = ident_new(function.substr(0, library).c_str());
        lib_t found =
            libraryName == lib_name(request.work) ? request.work : lib_require(libraryName);
        if (found == nullptr) return nullptr;
        tree_t declarer = lib_get(found, ident_new(function.substr(0, unit).c_str()));
        if (declarer == nullptr) return nullptr;
        const tree_kind_t kind = tree_kind(declarer);
        if (kind != T_PACKAGE && kind != T_ENTITY && kind != T_ARCH) return nullptr;
        ident_t wanted = ident_new(function.c_str());
        for (unsigned index = 0; index < tree_decls(declarer); ++index) {
            tree_t declaration = tree_decl(declarer, index);
            const tree_kind_t declared = tree_kind(declaration);
            if ((declared == T_FUNC_DECL || declared == T_FUNC_BODY)
                && tree_ident2(declaration) == wanted)
                return declaration;
        }
        return nullptr;
    }

    /// The subprogram `declaration` specialised for `shape` (DESIGN_GUIDE.md section 4.1): from
    /// `architecture` when it is one of that architecture's, and translated the first time.
    std::optional<Callee> callee(tree_t declaration, const Shape& shape, tree_t where,
                                 ArchitectureTranslator* architecture) {
        tree_t body = subprogramBody(declaration);
        if (body == nullptr) {
            unsupported(where, std::format("a call of {}, whose body has not been analysed",
                                           istr(tree_ident2(declaration))));
            return std::nullopt;
        }
        // A package's subprograms are a definition of their own, and so are an entity's or an
        // architecture's when no architecture is translating them: a resolution function's, which
        // elaboration asks for alone (DESIGN_GUIDE.md section 3.7).
        tree_t unit = tree_container(body);
        const tree_kind_t unitKind = tree_kind(unit);
        const bool inPackage =
            unitKind == T_PACK_BODY || unitKind == T_PACKAGE
            || (architecture == nullptr && (unitKind == T_ENTITY || unitKind == T_ARCH));
        if (!inPackage && architecture == nullptr) {
            unsupported(where, "a call from a package of a subprogram outside it");
            return std::nullopt;
        }

        // The values a specialisation depends on: the bounds of its unconstrained parameters, and
        // the scalars its own bounds are computed from.
        const std::vector<bool> needed = shapeParameters(body);
        SpecialisationKey key{.body = body,
                              .definition =
                                  inPackage ? std::string() : architecture->definition().name,
                              .bounds = {},
                              .values = {}};
        for (unsigned index = 0; index < tree_ports(body); ++index) {
            const std::optional<Folder::Range> bounds =
                index < shape.bounds.size() ? shape.bounds[index] : std::nullopt;
            key.bounds.push_back(
                bounds ? std::optional(std::tuple(bounds->left, bounds->right, bounds->ascending))
                       : std::nullopt);
            const std::optional<std::int64_t> value =
                index < shape.values.size() ? shape.values[index] : std::nullopt;
            if (needed[index] && !value) {
                unsupported(where,
                            std::format("a call of {} whose parameter '{}', which its bounds "
                                        "depend on, is not static",
                                        simpleName(tree_ident(body)),
                                        simpleName(tree_ident(tree_port(body, index)))));
                return std::nullopt;
            }
            key.values.push_back(needed[index] ? value : std::nullopt);
        }
        // One that failed has said why where it failed, and a call of it again is no recursion.
        if (failedCallees.contains(key)) return std::nullopt;
        if (const auto known = callees.find(key); known != callees.end()) {
            if (!known->second) {
                unsupported(where, std::format("a recursive call of {} whose result's bounds are "
                                               "not known before it returns",
                                               simpleName(tree_ident(body))));
            }
            return known->second;
        }
        callees.emplace(key, std::nullopt);

        // Each parameter's layout, and the bindings that make the body's bounds static.
        DefinitionScope& scope = inPackage ? static_cast<DefinitionScope&>(packageScope(unit))
                                           : static_cast<DefinitionScope&>(*architecture);
        SubprogramBindings bindings{.folder = scope.folder(), .parameters = {}};
        Callee callee;
        for (unsigned index = 0; index < tree_ports(body); ++index) {
            tree_t parameter = tree_port(body, index);
            type_t type = tree_type(parameter);
            std::optional<Layout> layout = layoutOf(type, bindings.folder);
            const auto& given = key.bounds[index];
            if (!layout && given) {
                const auto& [left, right, ascending] = *given;
                const Folder::Range bounds{.left = left, .right = right, .ascending = ascending};
                bindings.folder.bindBounds(parameter, bounds);
                layout = arrayLayout(bounds, type_elem(type), bindings.folder);
            }
            if (!layout) {
                unsupported(parameter, std::format("a parameter of type {}", type_pp(type)));
                failedCallees.insert(key);
                return std::nullopt;
            }
            if (const std::optional<std::int64_t>& value = key.values[index])
                bindings.folder.bind(parameter, *value);
            bindings.parameters.push_back(*layout);
            callee.parameters.push_back(*layout);
            eir::ParameterMode mode = eir::ParameterMode::In;
            if (tree_subkind(parameter) == PORT_OUT) mode = eir::ParameterMode::Out;
            if (tree_subkind(parameter) == PORT_INOUT) mode = eir::ParameterMode::InOut;
            callee.modes.push_back(mode);
        }

        callee.definition = inPackage ? packageName(unit) : architecture->definition().name;
        callee.name = specialisationName(body, key);
        BodyTranslator translator(scope, body, callee.name, std::move(bindings));
        std::optional<BodyTranslator::Translated> translated = translator.translateSubprogram();
        if (!translated) {
            failedCallees.insert(key);
            return std::nullopt;
        }
        callee.result = translated->result;
        if (inPackage) {
            packages.at(packageName(unit)).scope->addSubprogram(std::move(translated->subprogram));
        } else {
            architecture->addSubprogram(std::move(translated->subprogram));
        }
        callees[key] = callee;
        return callee;
    }

    /// The value `generics` binds the generic `generic` to, as a name and a key show it: a
    /// scalar's in decimal, and an array's bits in binary.
    static std::string rendered(const Folder& generics, tree_t generic) {
        if (const eir::BitVector* bits = generics.compositeOf(generic))
            return renderedArray(*bits, tree_type(generic));
        const auto scalar = generics.bindings().find(generic);
        if (scalar != generics.bindings().end()) return std::to_string(scalar->second);
        return "null";
    }

    /// An array generic's value as its elements' literals, left to right: `1010` for a vector of
    /// bits, `U01` for one of std_logic. Elements of any other type show as their bits.
    static std::string renderedArray(const eir::BitVector& bits, type_t type) {
        type_t element = type;
        while (type_is_array(element)) element = type_elem(element);
        const std::optional<eir::Type> storage = scalarStorage(element);
        if (!type_is_enum(element) || !storage || bits.width() % storage->bitWidth() != 0)
            return eir::toString(bits.ref(), eir::Radix::Binary);

        type_t base = type_base_recur(element);
        const std::uint32_t width = storage->bitWidth();
        std::string text;
        for (std::uint32_t at = bits.width(); at >= width; at -= width) {
            eir::BitVector one(width);
            eir::bitops::partSelectAt(one.mutableRef(), bits.ref(), at - width);
            const std::uint64_t code = one.ref().toUInt64();
            if (code >= type_enum_literals(base))
                return eir::toString(bits.ref(), eir::Radix::Binary);
            tree_t literal = type_enum_literal(base, static_cast<unsigned>(code));
            const std::string_view name = istr(tree_ident(literal));
            text += name.starts_with('\'') ? std::string(name.substr(1, 1))
                                           : simpleName(tree_ident(literal));
        }
        return text;
    }

    /// The name of the definition `architecture` of `entity` is with `generics` bound, and with the
    /// bounds `generics` binds its ports of unconstrained types to, queueing it for translation the
    /// first time it is asked for.
    std::string definitionFor(tree_t entity, tree_t architecture, Folder generics) {
        std::vector<std::string> values;
        values.reserve(tree_generics(entity));
        for (unsigned index = 0; index < tree_generics(entity); ++index) {
            tree_t generic = tree_generic(entity, index);
            values.push_back(
                std::format("{}={}", simpleName(tree_ident(generic)), rendered(generics, generic)));
        }
        for (unsigned index = 0; index < tree_ports(entity); ++index) {
            tree_t port = tree_port(entity, index);
            if (const std::optional<Folder::Range> bounds = generics.boundOf(port)) {
                values.push_back(std::format("{}={}{}{}", simpleName(tree_ident(port)),
                                             bounds->left, bounds->ascending ? "to" : "downto",
                                             bounds->right));
            }
        }
        auto key = std::pair(architecture, values);
        if (const auto known = names.find(key); known != names.end()) return known->second;

        std::string name = nameFor(entity, architecture, values);
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
    std::optional<Folder> bindGenerics(tree_t entity, tree_t instance, const Folder& parent) {
        Folder bound;
        const unsigned count = tree_generics(entity);
        for (unsigned index = 0; index < count; ++index) {
            tree_t generic = tree_generic(entity, index);

            // The actual the instance maps, folded where the instance is; or the default, folded
            // where the generics before it are bound.
            tree_t actual = nullptr;
            bool fromParent = false;
            const unsigned maps = instance == nullptr ? 0 : tree_genmaps(instance);
            for (unsigned map = 0; map < maps && actual == nullptr; ++map) {
                tree_t association = tree_genmap(instance, map);
                const bool positional = tree_subkind(association) == P_POS;
                const bool byName = !positional && tree_kind(tree_name(association)) == T_REF
                                    && tree_has_ref(tree_name(association))
                                    && tree_ref(tree_name(association)) == generic;
                if (positional ? tree_pos(association) != index : !byName) continue;
                if (tree_kind(tree_value(association)) != T_OPEN) {
                    actual = tree_value(association);
                    fromParent = true;
                }
            }
            if (actual == nullptr && tree_has_value(generic)) actual = tree_value(generic);
            if (!bindGeneric(bound, generic, actual, fromParent ? parent : bound))
                return std::nullopt;
        }
        return bound;
    }

    /// The values of `entity`'s generics where it is bound to `component` by default, the
    /// component's own being `local`: each the value of the component's generic of its name, or
    /// its default when the component has none (IEEE 1076 section 7.3.3).
    std::optional<Folder> bindThrough(tree_t entity, tree_t component, const Folder& local) {
        Folder bound;
        for (unsigned index = 0; index < tree_generics(entity); ++index) {
            tree_t generic = tree_generic(entity, index);
            tree_t same = nullptr;
            for (unsigned other = 0; other < tree_generics(component); ++other) {
                if (tree_ident(tree_generic(component, other)) == tree_ident(generic))
                    same = tree_generic(component, other);
            }
            if (same != nullptr) {
                // A null array's bounds are all it holds, so it binds as well as anything does.
                if (!bound.bindAs(generic, local, same)) {
                    unsupported(generic, std::format("the value of the generic '{}'",
                                                     simpleName(tree_ident(generic))));
                    return std::nullopt;
                }
                continue;
            }
            tree_t actual = tree_has_value(generic) ? tree_value(generic) : nullptr;
            if (!bindGeneric(bound, generic, actual, bound)) return std::nullopt;
        }
        return bound;
    }

    /// The entity a component is bound to by default: the one of its name in the library of
    /// `unit`, where it is instantiated, or else in the work library (IEEE 1076 section 7.3.3).
    /// Null when there is none.
    tree_t boundEntity(tree_t component, tree_t unit) {
        ident_t simple = ident_rfrom(tree_ident(component), '.');
        std::vector<lib_t> libraries;
        if (unit != nullptr) {
            ident_t libraryName = ident_until(tree_ident(unit), '.');
            if (lib_t own =
                    libraryName == lib_name(request.work) ? request.work : lib_require(libraryName))
                libraries.push_back(own);
        }
        libraries.push_back(request.work);
        for (lib_t library : libraries) {
            ident_t name = ident_prefix(lib_name(library), simple, '.');
            for (tree_t analysed : std::views::reverse(request.analysed)) {
                if (tree_kind(analysed) == T_ENTITY && tree_ident(analysed) == name)
                    return analysed;
            }
            if (tree_t found = lib_get(library, name);
                found != nullptr && tree_kind(found) == T_ENTITY)
                return found;
        }
        return nullptr;
    }

    /// Binds `generic` to the value of `actual`, folded in `scope`, and returns false having
    /// reported why when it is not one the adapter can fold.
    bool bindGeneric(Folder& bound, tree_t generic, tree_t actual, const Folder& scope) {
        if (tree_class(generic) != C_CONSTANT) {
            unsupported(generic, "a generic that is not a constant");
            return false;
        }
        type_t type = tree_type(generic);
        if (actual != nullptr && type_is_array(type)) {
            // An array generic of an unconstrained type takes its actual's bounds. A null one,
            // `""` most often, has no bits to hold, and its bounds are all a design can ask of it.
            std::optional<Layout> layout = layoutOf(type, bound);
            if (!layout) {
                if (const std::optional<Folder::Range> bounds =
                        constantBounds(actual, type, scope)) {
                    bound.bindBounds(generic, *bounds);
                    if (bounds->length() == 0) return true;
                    layout = arrayLayout(*bounds, type_elem(type), bound);
                }
            }
            if (layout) {
                if (std::optional<eir::BitVector> bits = constantBits(actual, *layout, scope)) {
                    bound.bindComposite(generic, std::move(*bits));
                    return true;
                }
            }
        } else if (actual != nullptr && type_is_record(type)) {
            // A record's value is bound as its bits, the first field highest, as an array's is.
            if (const std::optional<Layout> layout = layoutOf(type, bound)) {
                if (std::optional<eir::BitVector> bits = constantBits(actual, *layout, scope)) {
                    bound.bindComposite(generic, std::move(*bits));
                    return true;
                }
            }
        } else if (actual != nullptr) {
            if (const std::optional<std::int64_t> value = scope.integer(actual)) {
                bound.bind(generic, *value);
                return true;
            }
        }
        unsupported(generic, std::format("the value of the generic '{}', which is not a static "
                                         "value of a scalar, array or record type",
                                         simpleName(tree_ident(generic))));
        return false;
    }

    [[nodiscard]] SourceLocation locate(tree_t where) const { return locator(tree_loc(where)); }

    void unsupported(tree_t where, std::string_view what) {
        DiagnosticBuilder diagnostic =
            engine.report(DiagCode::VhdlUnsupported, locate(where), what);

        // A unit of another library, `ieee` say, has no buffer here, so its position is a note,
        // as NVC's own diagnostics there are.
        const loc_t* location = tree_loc(where);
        if (!locate(where).valid() && location != nullptr && !loc_invalid_p(location)) {
            diagnostic.note({},
                            std::format("at {}:{}", loc_file_str(location), location->first_line));
        }
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
        // A component stands for the entity it is bound to.
        for (tree_t target : std::vector<tree_t>(instantiated.begin(), instantiated.end())) {
            if (tree_kind(target) != T_COMPONENT) continue;
            if (tree_t entity = boundEntity(target, nullptr)) instantiated.insert(entity);
        }
        std::vector<tree_t> roots;
        for (tree_t unit : request.analysed) {
            if (tree_kind(unit) == T_ENTITY && !instantiated.contains(unit)) roots.push_back(unit);
        }
        return roots;
    }

    /// The entity's name, with the architecture when another of the entity's architectures has
    /// the plain name, and the generics' values and its unconstrained ports' bounds, `bound`, when
    /// it has any, as the SystemVerilog frontend names a parameterisation.
    std::string nameFor(tree_t entity, tree_t architecture, const std::vector<std::string>& bound) {
        std::string base = simpleName(tree_ident(entity));
        const auto owner = baseOwners.try_emplace(base, architecture).first;
        if (owner->second != architecture) base += "$" + simpleName(tree_ident(architecture));

        std::string values;
        for (const std::string& value : bound) {
            if (!values.empty()) values += ',';
            values += value;
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

    /// The definition holding the subprograms of the package `unit` declares or defines, named
    /// as a call names it: `<library>.<package>::`, a package's definition being `<package>::` in
    /// its library, as a SystemVerilog package's is.
    static std::string packageName(tree_t unit) {
        ident_t name = tree_ident(unit);
        if (tree_kind(unit) == T_PACK_BODY) name = ident_until(name, '-');
        return std::format("{}.{}::", simpleName(ident_until(name, '.')), simpleName(name));
    }

    /// The scope of the package `unit` declares or defines, made with its definition the first
    /// time.
    PackageScope& packageScope(tree_t unit) {
        const std::string name = packageName(unit);
        auto found = packages.find(name);
        if (found == packages.end()) {
            found = packages.emplace(name, Package{}).first;
            eir::ModuleDefinition& module = found->second.module;
            ident_t identifier = tree_ident(unit);
            if (tree_kind(unit) == T_PACK_BODY) identifier = ident_until(identifier, '-');
            module.name = simpleName(identifier) + "::";
            module.library = simpleName(ident_until(identifier, '.'));
            module.sourceName = simpleName(identifier);
            module.isPackage = true;
            module.location = locate(unit);
            found->second.scope = std::make_unique<PackageScope>(*this, module);
        }
        return *found->second.scope;
    }

    /// What identifies a subprogram's specialisation: its body, the architecture definition it is
    /// translated into when it is not a package's, and its shape.
    struct SpecialisationKey {
        tree_t body = nullptr;
        std::string definition;
        std::vector<std::optional<std::tuple<std::int64_t, std::int64_t, bool>>> bounds;
        std::vector<std::optional<std::int64_t>> values;

        friend auto operator<=>(const SpecialisationKey&, const SpecialisationKey&) = default;
    };

    /// The name of `body` specialised for `key`: its own name and signature, as NVC mangles the
    /// signature to tell overloads apart, then each parameter's bounds or value, `_` for one the
    /// shape leaves free: `resolved(Y)U$0to1`. Made of what the specialisation is, so that two
    /// translations of one name one subprogram, as the second that elaboration asks for
    /// resolution functions does (DESIGN_GUIDE.md section 3.7).
    [[nodiscard]] static std::string specialisationName(tree_t body, const SpecialisationKey& key) {
        std::string name = simpleName(tree_ident(body));
        const std::string_view mangled = istr(tree_ident2(body));
        if (const std::size_t signature = mangled.find('('); signature != std::string_view::npos)
            name += mangled.substr(signature);
        name += '$';
        for (std::size_t index = 0; index < key.bounds.size(); ++index) {
            if (index != 0) name += ',';
            if (const auto& bounds = key.bounds[index]) {
                const auto& [left, right, ascending] = *bounds;
                name += std::format("{}{}{}", left, ascending ? "to" : "downto", right);
            } else if (const std::optional<std::int64_t>& value = key.values[index]) {
                name += std::format("={}", *value);
            } else {
                name += '_';
            }
        }
        return name;
    }

    /// A package's definition, and the scope its subprograms are translated in. The definition
    /// never moves once made, since the scope refers to it.
    struct Package {
        eir::ModuleDefinition module;
        std::unique_ptr<PackageScope> scope;
    };

    const DesignRequest& request;
    const Locate& locator;
    DiagnosticEngine& engine;

    std::map<std::string, Package> packages;

    /// Each specialisation translated, or being translated when it holds nothing yet; and each
    /// whose translation failed.
    std::map<SpecialisationKey, std::optional<Callee>> callees;
    std::set<SpecialisationKey> failedCallees;

    std::deque<Specialisation> pending;
    std::vector<eir::ModuleDefinition> modules;
    std::map<std::pair<tree_t, std::vector<std::string>>, std::string> names;
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

std::optional<Callee> ArchitectureTranslator::callee(tree_t declaration, const Shape& shape,
                                                     tree_t where) {
    return design.callee(declaration, shape, where, this);
}

SourceLocation PackageScope::locate(tree_t where) const {
    return design.locate(where);
}

void PackageScope::unsupported(tree_t where, std::string_view what) {
    design.unsupported(where, what);
}

void PackageScope::reportNeverWaits(tree_t process) {
    design.diagnostics().report(DiagCode::VhdlProcessNeverWaits, locate(process));
}

std::int8_t PackageScope::timeUnitExponent() const {
    return design.timeUnitExponent();
}

std::optional<Callee> PackageScope::callee(tree_t declaration, const Shape& shape, tree_t where) {
    return design.callee(declaration, shape, where, nullptr);
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

std::uint32_t ArchitectureTranslator::vhdlTypeFor(type_t type, const Layout& layout) {
    if (type == nullptr) return eir::VhdlType::None;
    type_t base = type_base_recur(type);
    // Names as VHPI gives a basic identifier's, in upper case.
    const auto upperName = [](ident_t identifier) {
        std::string name = simpleName(identifier);
        std::ranges::transform(name, name.begin(), [](unsigned char letter) {
            return static_cast<char>(std::toupper(letter));
        });
        return name;
    };
    eir::VhdlType described;
    // A named subtype is known by its own name, `NATURAL`; an anonymous one by its base type's.
    described.name = upperName(type_has_ident(type) ? type_ident(type) : type_ident(base));
    // A null array's placeholder bit is no part of it.
    described.width =
        layout.isArray() && layout.index.length() == 0 ? 0 : layout.storage.bitWidth();
    const auto rangeOf = [](const Folder::Range& range) {
        return eir::VhdlType::Range{
            .left = range.left, .right = range.right, .ascending = range.ascending};
    };

    if (layout.isRecord()) {
        described.kind = eir::VhdlType::Kind::Record;
        for (std::size_t index = 0; index < layout.fields.size(); ++index) {
            tree_t field = type_field(layout.recordType, static_cast<unsigned>(index));
            const std::uint32_t fieldType = vhdlTypeFor(tree_type(field), layout.fields[index]);
            if (fieldType == eir::VhdlType::None) return eir::VhdlType::None;
            described.fields.push_back(eir::VhdlType::Field{.name = upperName(tree_ident(field)),
                                                            .type = fieldType,
                                                            .offset = layout.fieldOffsets[index]});
        }
    } else if (layout.isArray()) {
        described.kind = eir::VhdlType::Kind::Array;
        described.ranges.push_back(rangeOf(layout.index));
        for (const Folder::Range& inner : layout.inner) described.ranges.push_back(rangeOf(inner));
        const std::optional<Layout> element = layoutOf(layout.elementType, folding);
        if (!element) return eir::VhdlType::None;
        described.element = vhdlTypeFor(layout.elementType, *element);
        if (described.element == eir::VhdlType::None) return eir::VhdlType::None;
    } else {
        switch (type_kind(base)) {
        case T_ENUM:
            described.kind = eir::VhdlType::Kind::Enumeration;
            described.enumIndex = enumFor(type);
            if (described.enumIndex == eir::Object::NoEnum) return eir::VhdlType::None;
            break;
        case T_INTEGER: described.kind = eir::VhdlType::Kind::Integer; break;
        case T_PHYSICAL: described.kind = eir::VhdlType::Kind::Physical; break;
        case T_REAL: described.kind = eir::VhdlType::Kind::Floating; break;
        default: return eir::VhdlType::None;
        }
        if (described.kind != eir::VhdlType::Kind::Floating) {
            if (const std::optional<Folder::Range> range = folding.rangeOf(type))
                described.ranges.push_back(rangeOf(*range));
        }
    }

    // One entry for each type there is, however many objects are of it.
    const auto found = std::ranges::find(result.vhdlTypes, described);
    if (found != result.vhdlTypes.end())
        return static_cast<std::uint32_t>(found - result.vhdlTypes.begin());
    result.vhdlTypes.push_back(std::move(described));
    return static_cast<std::uint32_t>(result.vhdlTypes.size() - 1);
}

bool ArchitectureTranslator::addObject(tree_t declaration) {
    type_t type = tree_type(declaration);
    std::optional<Layout> layout = declaredLayout(declaration, folding);

    // A signal of a null range holds nothing, and is declared all the same, with a placeholder
    // bit, since a bit vector has at least one: what names its bounds reads them from its type,
    // or from its actual for a port of an unconstrained type, and VHPI shows it as the empty array
    // it is (DESIGN_GUIDE.md section 9.7). Nothing else of it is translated.
    std::optional<Folder::Range> bounds;
    if (!layout && type_is_array(type) && dimension_of(type) == 1) {
        bounds = folding.boundOf(declaration);
        if (!bounds) bounds = folding.rangeOf(type);
    }
    const Folder::Range index = bounds.value_or(Folder::Range{});
    const bool null = bounds.has_value() && index.length() == 0;
    if (null) {
        const std::optional<Layout> element = layoutOf(type_elem(type), folding);
        if (element) {
            layout = Layout{.storage = eir::Type::bits(1, false, false),
                            .array = true,
                            .index = index,
                            .elementBits = element->storage.bitWidth(),
                            .elementType = type_elem(type)};
        }
    }
    if (!layout) {
        unsupported(declaration, std::format("a signal of type {}", type_pp(type)));
        return false;
    }
    // Left unresolved, its lone driver's value would be its value, which the function need not
    // return.
    if (resolutionNotHeld(type, folding)) {
        unsupported(declaration,
                    std::format("the resolution of a signal of type {}", type_pp(type)));
        return false;
    }

    // Every VHDL object has a value from the start: its own initial value, or its type's left
    // bound (IEEE 1076 section 14.4.2.5). A port's is the default it declares, which is what an
    // unconnected port reads; a connected one is the signal it connects.
    std::optional<eir::BitVector> initial;
    if (null) {
        initial = eir::BitVector::filled(1, eir::Logic::Zero);
    } else if (tree_has_value(declaration)) {
        initial = constantBits(tree_value(declaration), *layout, folding.elaborating());
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
    object.name = prefix + simpleName(tree_ident(declaration));
    object.type = layout->storage;
    object.kind = eir::ObjectKind::Variable;
    object.location = locate(declaration);
    object.initialValue = std::move(initial);
    object.enumIndex = layout->isArray() ? eir::Object::NoEnum : enumFor(type);
    object.vhdlType = vhdlTypeFor(type, *layout);
    object.holdsNothing = null;
    object.encoding = encodingOf(type);
    object.resolution =
        null ? eir::Resolution{} : resolutionOf(type, folding).value_or(eir::Resolution{});
    objects.emplace(declaration, result.addObject(std::move(object)));
    return true;
}

void ArchitectureTranslator::addConstant(tree_t declaration) {
    tree_t value = constantValue(declaration);
    if (value == nullptr) return;
    type_t type = tree_type(declaration);
    std::optional<Layout> layout = declaredLayout(declaration, folding);
    if (!layout && type_is_array(type) && dimension_of(type) == 1) {
        if (const std::optional<Folder::Range> bounds = constantBounds(value, type, folding);
            bounds && bounds->length() > 0)
            layout = arrayLayout(*bounds, type_elem(type), folding);
    }
    if (!layout) return;
    std::optional<eir::BitVector> bits = constantBits(value, *layout, folding);
    if (!bits) return;
    result.parameters.push_back(eir::Parameter{.name = prefix + simpleName(tree_ident(declaration)),
                                               .value = std::move(*bits),
                                               .isLocal = true,
                                               .location = locate(declaration),
                                               .vhdlType = vhdlTypeFor(type, *layout)});
}

eir::ModuleDefinition ArchitectureTranslator::translate() {
    tree_t entity = specialisation.entity;
    result.name = specialisation.name;
    result.sourceName = simpleName(tree_ident(entity));
    result.location = locate(specialisation.architecture);

    for (unsigned index = 0; index < tree_generics(entity); ++index) {
        tree_t generic = tree_generic(entity, index);
        std::optional<eir::BitVector> value;
        if (const eir::BitVector* bits = folding.compositeOf(generic)) {
            value = *bits;
        } else if (const std::optional<eir::Type> storage = scalarStorage(tree_type(generic))) {
            const auto scalar = folding.bindings().find(generic);
            if (scalar != folding.bindings().end()) value = encode(scalar->second, *storage);
        }
        if (!value) continue;
        // A generic of an unconstrained array type, `string`, has its value's bounds.
        std::optional<Layout> layout = layoutOf(tree_type(generic), folding);
        if (!layout && type_is_array(tree_type(generic))) {
            if (const std::optional<Folder::Range> bounds = folding.boundOf(generic))
                layout = arrayLayout(*bounds, type_elem(tree_type(generic)), folding);
        }
        result.parameters.push_back(eir::Parameter{
            .name = simpleName(tree_ident(generic)),
            .value = std::move(*value),
            .isLocal = false,
            .location = locate(generic),
            .vhdlType = layout ? vhdlTypeFor(tree_type(generic), *layout) : eir::VhdlType::None});
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
    addPortSources(entity);
    return std::move(result);
}

void ArchitectureTranslator::addPortSources(tree_t entity) {
    for (unsigned index = 0; index < tree_ports(entity); ++index) {
        tree_t port = tree_port(entity, index);
        const auto mode = static_cast<port_mode_t>(tree_subkind(port));
        if (mode != PORT_OUT && mode != PORT_INOUT && mode != PORT_BUFFER) continue;
        const auto found = objects.find(port);
        if (found == objects.end() || sourcedByInstances.contains(found->second)) continue;
        // A record's fields are not resolved yet, so a second source of a resolved one could
        // only be refused.
        if (hasResolvedField(tree_type(port))) continue;
        const std::uint32_t objectIndex = found->second;
        const std::uint32_t width = result.objects[objectIndex].type.bitWidth();
        if (result.objects[objectIndex].holdsNothing) continue;

        // The bits the architecture's processes drive, merged; the rest have no source in it.
        std::vector<eir::WriteExtent> driven;
        for (const eir::Process& process : result.processes) {
            for (const eir::WriteExtent& extent : process.writeSet)
                if (extent.objectIndex == objectIndex) driven.push_back(extent);
        }
        eir::normaliseWriteSet(driven);
        std::vector<eir::WriteExtent> undriven;
        std::uint32_t next = 0;
        for (const eir::WriteExtent& extent : driven) {
            if (extent.lowBit > next) {
                undriven.push_back(eir::WriteExtent{
                    .objectIndex = objectIndex, .lowBit = next, .width = extent.lowBit - next});
            }
            next = std::max(next, extent.endBit());
        }
        if (next < width) {
            undriven.push_back(eir::WriteExtent{
                .objectIndex = objectIndex, .lowBit = next, .width = width - next});
        }
        if (undriven.empty()) continue;

        // A process that drives those bits and never runs again: its drivers start at the port's
        // default and keep it.
        eir::Process source;
        source.name = simpleName(tree_ident(port)) + "$default";
        source.kind = eir::ProcessKind::Always;
        source.drivesSignals = true;
        source.location = locate(port);
        source.writeSet = std::move(undriven);
        source.eventLists.emplace_back();
        eir::BasicBlock entry;
        entry.label = "entry";
        entry.instructions.push_back(eir::Instruction::make(
            eir::Opcode::WaitEvents, {}, eir::Operand::immediate(0), eir::Operand::block(0)));
        source.blocks.push_back(std::move(entry));
        result.processes.push_back(std::move(source));
    }
}

void ArchitectureTranslator::declarations(tree_t container) {
    const unsigned count = tree_decls(container);
    for (unsigned index = 0; index < count; ++index) {
        tree_t declaration = tree_decl(container, index);
        switch (tree_kind(declaration)) {
        case T_SIGNAL_DECL: static_cast<void>(addObject(declaration)); break;
        case T_CONST_DECL: addConstant(declaration); break;

        // Folded where they are used, so they need nothing of their own; a generate loop's
        // parameter among them, bound to each iteration's value. A configuration specification
        // is read at the instances it binds, and an alias is what it names where it is used.
        case T_ALIAS:
        case T_SPEC:
        case T_GENERIC_DECL:
        case T_TYPE_DECL:
        case T_SUBTYPE_DECL:
        case T_COMPONENT:
        case T_ATTR_DECL:
        case T_ATTR_SPEC:
        case T_USE:
        case T_FUNC_DECL:
        case T_PROC_DECL:
        case T_FUNC_BODY:
        case T_PROC_BODY: break;

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
            BodyTranslator translator(*this, statement, prefix + simpleName(tree_ident(statement)));
            if (std::optional<eir::Process> translated = translator.translate())
                result.processes.push_back(std::move(*translated));
            break;
        }

        case T_INSTANCE: instance(statement); break;
        case T_IF_GENERATE: ifGenerate(statement); break;
        case T_FOR_GENERATE: forGenerate(statement); break;
        case T_BLOCK: block(statement); break;

        default:
            unsupported(statement, std::format("the concurrent statement {}",
                                               tree_kind_str(tree_kind(statement))));
            break;
        }
    }
}

void ArchitectureTranslator::nested(tree_t container, const std::string& name) {
    const std::string outer = prefix;
    prefix += name + '.';
    declarations(container);
    statements(container);
    for (unsigned index = 0; index < tree_decls(container); ++index)
        objects.erase(tree_decl(container, index));
    prefix = outer;
}

void ArchitectureTranslator::ifGenerate(tree_t statement) {
    // The first alternative whose condition holds is the one elaborated, and the others are not
    // (IEEE 1076 section 14.5.3); an `else` has no condition and always holds.
    for (unsigned index = 0; index < tree_conds(statement); ++index) {
        tree_t alternative = tree_cond(statement, index);
        if (tree_has_value(alternative)) {
            const std::optional<std::int64_t> holds = folding.integer(tree_value(alternative));
            if (!holds) {
                unsupported(tree_value(alternative), "a generate condition that is not static");
                return;
            }
            if (*holds == 0) continue;
        }
        nested(alternative, simpleName(tree_ident(statement)));
        return;
    }
}

void ArchitectureTranslator::forGenerate(tree_t statement) {
    tree_t parameter = nullptr;
    for (unsigned index = 0; index < tree_decls(statement) && parameter == nullptr; ++index) {
        if (tree_kind(tree_decl(statement, index)) == T_GENERIC_DECL)
            parameter = tree_decl(statement, index);
    }
    const std::optional<Folder::Range> folded = folding.range(tree_range(statement, 0));
    if (parameter == nullptr || !folded) {
        unsupported(statement, "a generate loop whose range is not static");
        return;
    }
    const Folder::Range range = *folded;

    // Each iteration is a block of its own, named with its parameter's value as VHDL's path
    // names have it: `g(0)`, or `g(red)` for one over an enumeration.
    type_t base = type_base_recur(tree_type(parameter));
    const std::string label = simpleName(tree_ident(statement));
    for (std::int64_t offset = 0; offset < range.length(); ++offset) {
        const std::int64_t value = range.ascending ? range.left + offset : range.left - offset;
        folding.bind(parameter, value);
        std::string shown = std::to_string(value);
        if (type_is_enum(base) && value >= 0 && std::cmp_less(value, type_enum_literals(base)))
            shown = simpleName(tree_ident(type_enum_literal(base, static_cast<unsigned>(value))));
        const std::string iteration = std::format("{}({})", label, shown);

        // The parameter is a constant of the iteration, which VHPI shows as one.
        if (const std::optional<eir::Type> storage = scalarStorage(tree_type(parameter))) {
            result.parameters.push_back(eir::Parameter{
                .name =
                    std::format("{}{}.{}", prefix, iteration, simpleName(tree_ident(parameter))),
                .value = encode(value, *storage),
                .isLocal = true,
                .location = locate(parameter),
                .vhdlType = vhdlTypeFor(tree_type(parameter), Layout{.storage = *storage})});
        }
        nested(statement, iteration);
    }
}

void ArchitectureTranslator::block(tree_t statement) {
    // A block's generics are constants its statements fold with, bound by its generic map as an
    // instance's are; each of its ports is a signal of its own, connected to its actuals as an
    // instance's port is (IEEE 1076 section 11.2), which a waveform shows inside the block as
    // GHDL's does. An inout port, which a connection does not yet carry both ways, is the signal
    // associated with it as a whole instead.
    const Folder outer = folding;
    if (tree_generics(statement) != 0) {
        const std::optional<Folder> bound = design.bindGenerics(statement, statement, folding);
        if (!bound) return;
        folding.absorb(*bound);
    }

    const std::string name = simpleName(tree_ident(statement));
    const std::string outerPrefix = prefix;
    prefix += name + '.';
    std::vector<std::vector<tree_t>> byPort(tree_ports(statement));
    for (unsigned index = 0; index < tree_params(statement); ++index) {
        tree_t association = tree_param(statement, index);
        const std::optional<unsigned> port = formalIndex(association, statement);
        if (!port) {
            unsupported(association, "this port association");
            continue;
        }
        byPort[*port].push_back(association);
    }
    for (unsigned portNumber = 0; portNumber < tree_ports(statement); ++portNumber) {
        tree_t port = tree_port(statement, portNumber);
        const std::vector<tree_t>& associations = byPort[portNumber];
        tree_t first = associations.empty() ? nullptr : associations.front();
        tree_t actual = first == nullptr ? nullptr : tree_value(first);
        const bool whole =
            associations.size() == 1
            && (tree_subkind(first) == P_POS || tree_kind(tree_name(first)) == T_REF);
        if (unconstrained(port)) {
            const std::optional<Folder::Range> bounds =
                actualBounds(port, associations, statement, folding);
            if (!bounds) continue;
            folding.bindBounds(port, *bounds);
        }
        if (whole && tree_subkind(port) == PORT_INOUT && tree_kind(actual) == T_REF
            && tree_has_ref(actual)) {
            if (const std::optional<std::uint32_t> object = objectFor(tree_ref(actual))) {
                objects.insert_or_assign(port, *object);
                continue;
            }
        }
        if (associations.empty() || (whole && tree_kind(actual) == T_OPEN)) {
            addObject(port);
            continue;
        }
        // The block's own statements, being in this definition, would drive what the port reads.
        if (tree_subkind(port) == PORT_INOUT) {
            unsupported(first, "associating a block's inout port with this");
            continue;
        }
        if (const std::optional<Through> through = connectThrough(
                prefix + simpleName(tree_ident(port)), port, port, associations, folding))
            objects.insert_or_assign(port, through->object);
    }

    declarations(statement);
    statements(statement);
    for (unsigned index = 0; index < tree_decls(statement); ++index)
        objects.erase(tree_decl(statement, index));
    for (unsigned index = 0; index < tree_ports(statement); ++index)
        objects.erase(tree_port(statement, index));
    prefix = outerPrefix;
    folding = outer;
}

std::optional<unsigned> ArchitectureTranslator::formalIndex(tree_t association, tree_t formals) {
    if (tree_subkind(association) == P_POS) return tree_pos(association);
    // A part of the port, or a conversion of an output's value, `f(port) => s` (IEEE 1076 section
    // 6.5.7.1), names the port inside it.
    tree_t formal = tree_name(association);
    for (;;) {
        const tree_kind_t kind = tree_kind(formal);
        if (kind == T_ARRAY_REF || kind == T_ARRAY_SLICE || kind == T_RECORD_REF
            || kind == T_TYPE_CONV || kind == T_CONV_FUNC) {
            formal = tree_value(formal);
        } else if (kind == T_FCALL && tree_params(formal) == 1) {
            formal = tree_value(tree_param(formal, 0));
        } else {
            break;
        }
    }
    for (unsigned port = 0; port < tree_ports(formals); ++port) {
        if (tree_kind(formal) == T_REF && tree_has_ref(formal)
            && tree_port(formals, port) == tree_ref(formal))
            return port;
    }
    return std::nullopt;
}

void ArchitectureTranslator::instance(tree_t statement) {
    tree_t target = tree_ref(statement);
    tree_t entity = nullptr;
    tree_t architecture = nullptr;
    tree_t component = nullptr;
    tree_t binding = nullptr;
    switch (tree_kind(target)) {
    case T_ENTITY:
        entity = target;
        architecture = design.architectureOf(entity);
        break;
    case T_ARCH:
        architecture = target;
        entity = tree_primary(target);
        break;
    case T_COMPONENT:
        component = target;
        if (tree_has_spec(statement)) {
            // A configuration specification, `for u : c use entity work.e(a);`, names the entity
            // and maybe the architecture, and maps the entity's generics and ports to the
            // component's, by name where it does not say otherwise (IEEE 1076 section 7.3.2).
            binding = tree_value(tree_spec(statement));
            if (binding == nullptr || tree_kind(binding) != T_BINDING || !tree_has_ref(binding)) {
                unsupported(statement, "an instance its configuration specification leaves open");
                return;
            }
            tree_t bound = tree_ref(binding);
            if (tree_kind(bound) == T_ARCH) {
                architecture = bound;
                entity = tree_primary(bound);
            } else if (tree_kind(bound) == T_ENTITY) {
                entity = bound;
                architecture = design.architectureOf(entity);
            } else {
                unsupported(statement, "a binding to a configuration");
                return;
            }
            break;
        }
        // With no entity to bind by default, the instance is unbound, which is legal: it has no
        // behaviour, and its actuals are driven by nothing in it.
        entity = design.boundEntity(component, specialisation.architecture);
        if (entity == nullptr) {
            design.diagnostics().report(DiagCode::VhdlUnboundComponent, locate(statement),
                                        simpleName(tree_ident(component)));
            return;
        }
        architecture = design.architectureOf(entity);
        break;
    default: unsupported(statement, "an instance of a configuration"); return;
    }
    if (architecture == nullptr) {
        design.diagnostics().report(
            DiagCode::VhdlError, locate(statement),
            std::format("entity '{}' has no architecture", simpleName(tree_ident(entity))));
        return;
    }

    // A component's generics are bound by the instance, and the entity's by the component's
    // (IEEE 1076 section 7.3.3). A formal names one of the component's ports then, whose bounds
    // fold with the component's generics.
    std::optional<Folder> generics;
    Folder childGenerics;
    if (component != nullptr) {
        const std::optional<Folder> local = design.bindGenerics(component, statement, folding);
        if (!local) return;
        generics = binding != nullptr ? design.bindGenerics(entity, binding, *local)
                                      : design.bindThrough(entity, component, *local);
        if (!generics) return;
        childGenerics = *generics;
        childGenerics.absorb(*local);
    } else {
        generics = design.bindGenerics(entity, statement, folding);
        if (!generics) return;
        childGenerics = *generics;
    }
    tree_t formals = component != nullptr ? component : entity;

    // Each port's associations, in order: one, or one for each part of a port associated in
    // parts (IEEE 1076 section 6.5.7.1). A component's port is the entity's of its name.
    std::vector<std::vector<tree_t>> byPort(tree_ports(entity));
    std::vector<tree_t> formalPorts(tree_ports(entity), nullptr);
    for (unsigned index = 0; index < tree_params(statement); ++index) {
        tree_t association = tree_param(statement, index);
        const unsigned formalNumber =
            formalIndex(association, formals).value_or(tree_ports(formals));
        if (formalNumber >= tree_ports(formals)) {
            unsupported(association, "this port association");
            continue;
        }
        tree_t formalPort = tree_port(formals, formalNumber);
        const std::optional<unsigned> portIndex = entityPortFor(entity, binding, formalPort);
        const unsigned portNumber = portIndex.value_or(tree_ports(entity));
        if (portNumber >= tree_ports(entity)) {
            design.diagnostics().report(
                DiagCode::VhdlError, locate(association),
                std::format("entity '{}' has no port '{}' for component '{}' to be bound to",
                            simpleName(tree_ident(entity)), simpleName(tree_ident(formalPort)),
                            simpleName(tree_ident(formals))));
            continue;
        }
        byPort[portNumber].push_back(association);
        formalPorts[portNumber] = formalPort;
    }

    // A port of an unconstrained type takes its bounds from its actual (IEEE 1076 section
    // 6.5.6.3), and the entity is translated for them as for its generics' values. A component's
    // port gives the entity's its bounds, its own or, unconstrained too, its actual's.
    for (unsigned portNumber = 0; portNumber < tree_ports(entity); ++portNumber) {
        tree_t port = tree_port(entity, portNumber);
        tree_t formalPort = formalPorts[portNumber];
        const bool formalTakesBounds =
            formalPort != nullptr && formalPort != port && unconstrained(formalPort);
        if (!unconstrained(port) && !formalTakesBounds) continue;
        std::optional<Folder::Range> bounds = formalPort == nullptr || formalTakesBounds
                                                  ? std::nullopt
                                                  : childGenerics.rangeOf(tree_type(formalPort));
        if (!bounds) bounds = actualBounds(port, byPort[portNumber], statement, *generics);
        if (!bounds) return;
        if (unconstrained(port)) {
            generics->bindBounds(port, *bounds);
            childGenerics.bindBounds(port, *bounds);
        }
        if (formalTakesBounds) childGenerics.bindBounds(formalPort, *bounds);
    }

    eir::Instance created;
    created.name = prefix + simpleName(tree_ident(statement));
    created.definitionName = design.definitionFor(entity, architecture, std::move(*generics));
    created.location = locate(statement);

    for (unsigned portNumber = 0; portNumber < tree_ports(entity); ++portNumber) {
        const std::vector<tree_t>& associations = byPort[portNumber];
        if (associations.empty()) continue;
        tree_t port = tree_port(entity, portNumber);
        tree_t first = associations.front();
        const bool whole =
            associations.size() == 1
            && (tree_subkind(first) == P_POS || tree_kind(tree_name(first)) == T_REF);

        // An open port keeps its own storage and its default, so it needs no connection at all;
        // a whole signal is the port's storage itself.
        if (whole) {
            tree_t actual = tree_value(first);
            if (tree_kind(actual) == T_OPEN) continue;
            if (tree_kind(actual) == T_REF && tree_has_ref(actual)) {
                if (const std::optional<std::uint32_t> object = objectFor(tree_ref(actual))) {
                    created.connections.push_back(
                        eir::PortConnection{.portIndex = portNumber, .objectIndex = *object});
                    if (tree_subkind(port) != PORT_IN) sourcedByInstances.insert(*object);
                    continue;
                }
            }
        }
        if (const std::optional<Through> through =
                connectThrough(std::format("{}${}", created.name, simpleName(tree_ident(port))),
                               port, formalPorts[portNumber], associations, childGenerics)) {
            created.connections.push_back(eir::PortConnection{.portIndex = portNumber,
                                                              .objectIndex = through->object,
                                                              .driveObjectIndex = through->driven});
        }
    }
    result.instances.push_back(std::move(created));
}

std::optional<unsigned> ArchitectureTranslator::entityPortFor(tree_t entity, tree_t binding,
                                                              tree_t formalPort) {
    // The port a binding's port map associates with this one of the component's.
    if (binding != nullptr) {
        for (unsigned index = 0; index < tree_params(binding); ++index) {
            tree_t association = tree_param(binding, index);
            tree_t actual = tree_value(association);
            if (tree_kind(actual) != T_REF || !tree_has_ref(actual)
                || tree_ref(actual) != formalPort)
                continue;
            if (tree_subkind(association) == P_POS) return tree_pos(association);
            tree_t formal = tree_name(association);
            for (unsigned port = 0; port < tree_ports(entity); ++port) {
                if (tree_kind(formal) == T_REF && tree_has_ref(formal)
                    && tree_ref(formal) == tree_port(entity, port))
                    return port;
            }
        }
        return std::nullopt;
    }

    // An entity's own port, or under default binding the entity's of the component's name.
    for (unsigned port = 0; port < tree_ports(entity); ++port) {
        if (tree_port(entity, port) == formalPort
            || tree_ident(tree_port(entity, port)) == tree_ident(formalPort))
            return port;
    }
    return std::nullopt;
}

bool ArchitectureTranslator::hasResolvedField(type_t type) {
    if (!type_is_record(type)) return false;
    type_t base = type_base_recur(type);
    for (unsigned index = 0; index < type_fields(base); ++index) {
        type_t field = tree_type(type_field(base, index));
        if (resolutionOf(field, Folder()) || resolutionNotHeld(field, Folder())
            || hasResolvedField(field))
            return true;
    }
    return false;
}

bool ArchitectureTranslator::unconstrained(tree_t port) {
    type_t type = tree_type(port);
    return type_is_array(type) && dimension_of(type) == 1 && type_is_unconstrained(type);
}

std::optional<Folder::Range>
ArchitectureTranslator::actualBounds(tree_t port, const std::vector<tree_t>& associations,
                                     tree_t where, const Folder& portScope) {
    tree_t first = associations.empty() ? nullptr : associations.front();
    const bool whole = associations.size() == 1
                       && (tree_subkind(first) == P_POS || tree_kind(tree_name(first)) == T_REF);

    // An input left open is its default, which NVC requires it to have.
    if (first == nullptr || (whole && tree_kind(tree_value(first)) == T_OPEN)) {
        std::optional<Folder::Range> bounds;
        if (tree_has_value(port))
            bounds = constantBounds(tree_value(port), tree_type(port), portScope);
        if (!bounds) {
            unsupported(where, std::format("leaving open the port '{}' of an unconstrained type "
                                           "with no static default",
                                           simpleName(tree_ident(port))));
        }
        return bounds;
    }

    if (whole) {
        // A conversion to an unconstrained type has its operand's bounds.
        tree_t actual = tree_value(first);
        std::optional<Folder::Range> bounds = folding.boundsOf(actual);
        while (!bounds && tree_kind(actual) == T_TYPE_CONV) {
            actual = tree_value(actual);
            bounds = folding.boundsOf(actual);
        }
        if (!bounds) unsupported(first, "taking a port's bounds from this actual");
        return bounds;
    }

    // Associated in parts, the port runs the way its index subtype does, from the lowest index
    // its formals name to the highest (IEEE 1076 section 5.3.2.2).
    std::optional<std::int64_t> low;
    std::optional<std::int64_t> high;
    for (tree_t association : associations) {
        tree_t formal = tree_subkind(association) == P_POS ? nullptr : tree_name(association);
        std::optional<Folder::Range> part;
        if (formal != nullptr && tree_kind(formal) == T_ARRAY_REF && tree_params(formal) == 1) {
            if (const std::optional<std::int64_t> index =
                    folding.integer(tree_value(tree_param(formal, 0))))
                part = Folder::Range{.left = *index, .right = *index, .ascending = true};
        } else if (formal != nullptr && tree_kind(formal) == T_ARRAY_SLICE) {
            part = folding.range(tree_range(formal, 0));
        }
        if (!part || part->length() == 0 || tree_kind(tree_value(formal)) != T_REF) {
            unsupported(association, "associating this part of a port of an unconstrained type");
            return std::nullopt;
        }
        low = std::min(low.value_or(part->low()), part->low());
        high = std::max(high.value_or(part->high()), part->high());
    }
    const std::optional<Folder::Range> index = folding.rangeOf(index_type_of(tree_type(port), 0));
    if (!index || !low || !high) {
        unsupported(first, "associating in parts a port of an unconstrained type");
        return std::nullopt;
    }
    return index->ascending ? Folder::Range{.left = *low, .right = *high, .ascending = true}
                            : Folder::Range{.left = *high, .right = *low, .ascending = false};
}

std::optional<ArchitectureTranslator::Through>
ArchitectureTranslator::connectThrough(std::string objectName, tree_t port, tree_t formalPort,
                                       const std::vector<tree_t>& associations,
                                       const Folder& childGenerics) {
    const std::optional<Layout> layout = declaredLayout(port, childGenerics);
    if (!layout) {
        unsupported(associations.front(), "associating a port of this type with this");
        return std::nullopt;
    }
    if (resolutionNotHeld(tree_type(port), childGenerics)) {
        unsupported(port,
                    std::format("the resolution of a port of type {}", type_pp(tree_type(port))));
        return std::nullopt;
    }
    const auto mode = static_cast<port_mode_t>(tree_subkind(port));
    const bool input = mode == PORT_IN;
    const bool inout = mode == PORT_INOUT;
    if (!input && !inout && mode != PORT_OUT && mode != PORT_BUFFER) {
        unsupported(associations.front(), "associating a port of this mode with this");
        return std::nullopt;
    }

    // An object standing for the port in this definition: its default to start with, which an
    // input's actuals overwrite where their initial values are known.
    eir::Object through;
    through.name = std::move(objectName);
    through.type = layout->storage;
    through.location = locate(associations.front());
    through.initialValue = tree_has_value(port)
                               ? constantBits(tree_value(port), *layout, childGenerics)
                               : defaultBits(tree_type(port), *layout, childGenerics);
    through.enumIndex = layout->isComposite() ? eir::Object::NoEnum : enumFor(tree_type(port));
    through.vhdlType = vhdlTypeFor(tree_type(port), *layout);
    through.encoding = encodingOf(tree_type(port));

    // An inout port's drivers drive an object of their own, its driving value, which goes out to
    // its actuals as one of their sources, while it reads its effective value, its actuals
    // resolved and brought back in (IEEE 1076 section 14.7.3). Both start at its default.
    std::uint32_t drivenIndex = eir::PortConnection::Unconnected;
    if (inout) {
        eir::Object driving = through;
        driving.name += "$driving";
        driving.resolution =
            resolutionOf(tree_type(port), childGenerics).value_or(eir::Resolution{});
        drivenIndex = result.addObject(std::move(driving));
    }
    const std::uint32_t throughIndex = result.addObject(std::move(through));

    // `name` without the conversion of an association around it, `f(x)` or `t(x)`.
    const auto unconverted = [](tree_t name) {
        const tree_kind_t kind = name == nullptr ? T_LAST_TREE_KIND : tree_kind(name);
        if (kind == T_CONV_FUNC || kind == T_TYPE_CONV) return tree_value(name);
        if (kind == T_FCALL && tree_params(name) == 1) return tree_value(tree_param(name, 0));
        return name;
    };

    // A formal names the port, which is that object here, and its bounds fold with the instance's
    // generics.
    const Folder outer = folding;
    folding.absorb(childGenerics);
    objects.insert_or_assign(formalPort, throughIndex);
    std::optional<eir::BitVector> start = result.objects[throughIndex].initialValue;
    for (std::size_t index = 0; index < associations.size(); ++index) {
        tree_t association = associations[index];
        tree_t actual = tree_value(association);
        if (tree_kind(actual) == T_OPEN) continue;
        tree_t formal = tree_subkind(association) == P_POS ? nullptr : tree_name(association);
        if (formal != nullptr && tree_kind(formal) == T_REF) formal = nullptr;

        std::string name = result.objects[throughIndex].name;
        if (associations.size() > 1) name += std::format("${}", index);

        // Each way through its own process: the actual, converted, into the effective value, and
        // the driving value, converted, out to the actual.
        if (inout) {
            tree_t formalPart = unconverted(formal);
            if (formalPart != nullptr && tree_kind(formalPart) == T_REF) formalPart = nullptr;
            BodyTranslator in(*this, association, name + "$in");
            std::optional<eir::Process> inward =
                in.translateAssociation(formalPart, actual, throughIndex, *layout, &start);
            if (inward) result.processes.push_back(std::move(*inward));

            objects.insert_or_assign(formalPort, drivenIndex);
            BodyTranslator out(*this, association, name + "$out");
            std::optional<eir::Process> outward = out.translateAssociation(
                unconverted(actual), formal, drivenIndex, *layout, nullptr);
            if (outward) result.processes.push_back(std::move(*outward));
            objects.insert_or_assign(formalPort, throughIndex);
            continue;
        }

        // A whole input tied to a static value is that value from the start, and nothing drives
        // it after.
        if (input && formal == nullptr) {
            if (std::optional<eir::BitVector> bits = constantBits(actual, *layout, folding)) {
                start = std::move(bits);
                continue;
            }
        }

        BodyTranslator translator(*this, association, std::move(name));
        std::optional<eir::Process> process =
            input ? translator.translateAssociation(formal, actual, throughIndex, *layout, &start)
                  : translator.translateAssociation(actual, formal, throughIndex, *layout, nullptr);
        if (process) result.processes.push_back(std::move(*process));
    }
    result.objects[throughIndex].initialValue = std::move(start);
    objects.erase(formalPort);
    folding = outer;
    return Through{.object = throughIndex, .driven = drivenIndex};
}

} // namespace

std::vector<eir::ModuleDefinition>
translateDesign(const DesignRequest& request, const Locate& locate, DiagnosticEngine& diagnostics) {
    setIeeeWarnings(request.ieeeWarnings);
    DesignTranslator translator(request, locate, diagnostics);
    return translator.run();
}

std::vector<eir::ModuleDefinition>
translateResolutions(const DesignRequest& request, const std::vector<ResolutionRequest>& requests,
                     std::vector<ResolutionAnswer>& answers, const Locate& locate,
                     DiagnosticEngine& diagnostics) {
    setIeeeWarnings(request.ieeeWarnings);
    DesignTranslator translator(request, locate, diagnostics);
    return translator.resolve(requests, answers);
}

} // namespace enzan::frontend::vhdl
