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
        for (auto& [name, package] : packages) modules.push_back(std::move(package.module));
        return std::move(modules);
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
        tree_t unit = tree_container(body);
        const bool inPackage = tree_kind(unit) == T_PACK_BODY || tree_kind(unit) == T_PACKAGE;
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
        callee.name = std::format("{}${}", simpleName(tree_ident(body)), callees.size());
        BodyTranslator translator(scope, body, callee.name, std::move(bindings));
        std::optional<BodyTranslator::Translated> translated = translator.translateSubprogram();
        if (!translated) return std::nullopt;
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

    /// The name of the definition `architecture` of `entity` is with `generics` bound, queueing
    /// it for translation the first time it is asked for.
    std::string definitionFor(tree_t entity, tree_t architecture, Folder generics) {
        std::vector<std::string> values;
        values.reserve(tree_generics(entity));
        for (unsigned index = 0; index < tree_generics(entity); ++index)
            values.push_back(rendered(generics, tree_generic(entity, index)));
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
            if (tree_class(generic) != C_CONSTANT) {
                unsupported(generic, "a generic that is not a constant");
                return std::nullopt;
            }

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
            const Folder& scope = fromParent ? parent : bound;

            type_t type = tree_type(generic);
            if (actual != nullptr && type_is_array(type)) {
                // An array generic of an unconstrained type takes its actual's bounds. A null one,
                // `""` most often, has no bits to hold, and its bounds are all a design can ask
                // of it.
                std::optional<Layout> layout = layoutOf(type, bound);
                if (!layout) {
                    if (const std::optional<Folder::Range> bounds = scope.boundsOf(actual)) {
                        bound.bindBounds(generic, *bounds);
                        if (bounds->length() == 0) continue;
                        layout = arrayLayout(*bounds, type_elem(type), bound);
                    }
                }
                if (layout) {
                    if (std::optional<eir::BitVector> bits = constantBits(actual, *layout, scope)) {
                        bound.bindComposite(generic, std::move(*bits));
                        continue;
                    }
                }
            } else if (actual != nullptr) {
                if (const std::optional<std::int64_t> value = scope.integer(actual)) {
                    bound.bind(generic, *value);
                    continue;
                }
            }
            unsupported(generic,
                        std::format("the value of the generic '{}', which is not a static value "
                                    "of a scalar type or an array of one",
                                    simpleName(tree_ident(generic))));
            return std::nullopt;
        }
        return bound;
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
        std::vector<tree_t> roots;
        for (tree_t unit : request.analysed) {
            if (tree_kind(unit) == T_ENTITY && !instantiated.contains(unit)) roots.push_back(unit);
        }
        return roots;
    }

    /// The entity's name, with the architecture when another of the entity's architectures has
    /// the plain name, and the generics' values when it has any, as the SystemVerilog frontend
    /// names a parameterisation.
    std::string nameFor(tree_t entity, tree_t architecture, const std::vector<std::string>& bound) {
        std::string base = simpleName(tree_ident(entity));
        const auto owner = baseOwners.try_emplace(base, architecture).first;
        if (owner->second != architecture) base += "$" + simpleName(tree_ident(architecture));

        std::string values;
        for (unsigned index = 0; index < tree_generics(entity); ++index) {
            if (!values.empty()) values += ',';
            values += simpleName(tree_ident(tree_generic(entity, index)));
            values += '=';
            values += bound[index];
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

    /// Each specialisation translated, or being translated when it holds nothing yet.
    std::map<SpecialisationKey, std::optional<Callee>> callees;

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
    object.encoding = encodingOf(type);
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
        std::optional<eir::BitVector> value;
        if (const eir::BitVector* bits = folding.compositeOf(generic)) {
            value = *bits;
        } else if (const std::optional<eir::Type> storage = scalarStorage(tree_type(generic))) {
            const auto scalar = folding.bindings().find(generic);
            if (scalar != folding.bindings().end()) value = encode(scalar->second, *storage);
        }
        if (!value) continue;
        result.parameters.push_back(eir::Parameter{.name = simpleName(tree_ident(generic)),
                                                   .value = std::move(*value),
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
            BodyTranslator translator(*this, statement, simpleName(tree_ident(statement)));
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

    std::optional<Folder> generics = design.bindGenerics(entity, statement, folding);
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
            tied.encoding = encodingOf(tree_type(port));
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
