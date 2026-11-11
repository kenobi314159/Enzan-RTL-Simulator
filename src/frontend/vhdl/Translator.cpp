// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/Translator.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "frontend/vhdl/BodyTranslator.h"
#include "frontend/vhdl/Folder.h"
#include "frontend/vhdl/Types.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

/// The actual `instance` maps to `generic`, the one at `index` of what it instantiates; null when
/// it leaves the generic alone. NVC's analysis maps one left alone to `open`, or where its default
/// is a literal to the default's own tree.
tree_t mappedActual(tree_t instance, tree_t generic, unsigned index) {
    const unsigned maps = instance == nullptr ? 0 : tree_genmaps(instance);
    for (unsigned map = 0; map < maps; ++map) {
        tree_t association = tree_genmap(instance, map);
        const bool positional = tree_subkind(association) == P_POS;
        const bool byName = !positional && tree_kind(tree_name(association)) == T_REF
                            && tree_has_ref(tree_name(association))
                            && tree_ref(tree_name(association)) == generic;
        if (positional ? tree_pos(association) != index : !byName) continue;
        tree_t actual = tree_value(association);
        if (tree_kind(actual) == T_OPEN) continue;
        return tree_has_value(generic) && actual == tree_value(generic) ? nullptr : actual;
    }
    return nullptr;
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

/// The double a real literal is; nothing when the text is no number.
std::optional<double> realOf(std::string_view text) {
    double value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || text.empty())
        return std::nullopt;
    return value;
}

/// The value `generics` binds the generic `generic` to as a SystemVerilog literal, which sets the
/// parameter of its name of the module its component is bound to: an integer in decimal, a `bit`, a
/// `boolean` or a `std_ulogic` as the four-state bit its code stands for, an array of them as that
/// many bits, the left element highest, and a string as a string. Nothing for a value of another
/// type.
std::optional<std::string> systemVerilogLiteral(const Folder& generics, tree_t generic) {
    // std_ulogic's codes, U X 0 1 Z W L H -, as four-state bits.
    static constexpr std::string_view Logic = "xx01zx01x";
    type_t type = tree_type(generic);

    if (const eir::BitVector* bits = generics.compositeOf(generic)) {
        if (!type_is_array(type) || dimension_of(type) != 1) return std::nullopt;
        type_t element = type_elem(type);
        type_t base = type_base_recur(element);
        const bool characters =
            type_has_ident(base)
            && std::string_view(istr(type_ident(base))) == "STD.STANDARD.CHARACTER";
        const eir::Encoding encoding = encodingOf(element);
        const std::optional<eir::Type> storage = scalarStorage(element);
        if (!storage || bits->width() == 0 || bits->width() % storage->bitWidth() != 0)
            return std::nullopt;
        if (!characters && encoding != eir::Encoding::VhdlLogic
            && encoding != eir::Encoding::VhdlBit)
            return std::nullopt;

        const std::uint32_t width = storage->bitWidth();
        std::string text =
            characters ? std::string("\"") : std::format("{}'b", bits->width() / width);
        for (std::uint32_t at = bits->width(); at >= width; at -= width) {
            eir::BitVector one(width);
            eir::bitops::partSelectAt(one.mutableRef(), bits->ref(), at - width);
            const std::uint64_t code = one.ref().toUInt64();
            if (characters) {
                if (code == '"' || code == '\\') {
                    text += '\\';
                    text += static_cast<char>(code);
                } else if (code >= 0x20 && code < 0x7f) {
                    text += static_cast<char>(code);
                } else {
                    text += std::format("\\{:03o}", code & 0xffU);
                }
            } else if (encoding == eir::Encoding::VhdlLogic) {
                if (code >= Logic.size()) return std::nullopt;
                text += Logic[code];
            } else {
                text += code == 0 ? '0' : '1';
            }
        }
        if (characters) text += '"';
        return text;
    }

    const std::optional<std::int64_t> value = generics.boundValue(generic);
    if (!value) return std::nullopt;
    if (type_is_real(type)) return eir::realLiteral(std::bit_cast<double>(*value));
    if (type_is_integer(type)) {
        // Wider than an unsized literal's 32 bits, sized and signed.
        if (*value >= std::numeric_limits<std::int32_t>::min()
            && *value <= std::numeric_limits<std::int32_t>::max())
            return std::to_string(*value);
        const std::uint64_t magnitude = *value < 0 ? 0 - static_cast<std::uint64_t>(*value)
                                                   : static_cast<std::uint64_t>(*value);
        return std::format("{}64'sd{}", *value < 0 ? "-" : "", magnitude);
    }
    const eir::Encoding encoding = encodingOf(type);
    if (encoding == eir::Encoding::VhdlLogic && *value >= 0 && std::cmp_less(*value, Logic.size()))
        return std::format("1'b{}", Logic[static_cast<std::size_t>(*value)]);
    if (encoding == eir::Encoding::VhdlBit) return *value != 0 ? "1'b1" : "1'b0";
    return std::nullopt;
}

/// Whether two names are one in any case.
bool sameName(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a))
               == std::tolower(static_cast<unsigned char>(b));
    });
}

/// `text` in lower case, as a source most often writes what NVC prints in upper case.
std::string lowered(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return lower;
}

/// `name`, a VHDL simple name as `simpleName` gives it, as SystemVerilog names what it names: an
/// extended identifier, `\next\`, by what it holds, which is how VHDL names a port or a generic
/// whose SystemVerilog name is one of VHDL's reserved words.
std::string systemVerilogName(std::string name) {
    if (name.size() < 2 || !name.starts_with('\\') || !name.ends_with('\\')) return name;
    std::string held;
    for (std::size_t at = 1; at + 1 < name.size(); ++at) {
        held += name[at];
        if (name[at] == '\\' && name[at + 1] == '\\') ++at;
    }
    return held;
}

/// The integer a literal (`eir::UnitRequest`) is, `-5` or `64'sd5000000000`; nothing when it is not
/// one.
std::optional<std::int64_t> integerLiteral(std::string_view text) {
    const bool negative = text.starts_with('-');
    if (negative) text.remove_prefix(1);
    if (text.starts_with("64'sd")) text.remove_prefix(5);
    std::uint64_t magnitude = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), magnitude);
    if (error != std::errc() || end != text.data() + text.size() || text.empty())
        return std::nullopt;
    const std::uint64_t limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1 : 0);
    if (magnitude > limit) return std::nullopt;
    if (!negative) return static_cast<std::int64_t>(magnitude);
    return magnitude == limit ? std::numeric_limits<std::int64_t>::min()
                              : -static_cast<std::int64_t>(magnitude);
}

/// The bits a literal of one bit or a vector is, `1'bx` or `4'b01xz`, left first, each `0`, `1`,
/// `x` or `z`; nothing when it is not one.
std::optional<std::string> bitsLiteral(std::string_view text) {
    const std::size_t base = text.find("'b");
    if (base == std::string_view::npos) return std::nullopt;
    std::uint64_t count = 0;
    const std::string_view size = text.substr(0, base);
    const auto [end, error] = std::from_chars(size.data(), size.data() + size.size(), count);
    if (error != std::errc() || end != size.data() + size.size()) return std::nullopt;
    std::string bits(text.substr(base + 2));
    if (bits.size() != count || !std::ranges::all_of(bits, [](char bit) {
            return std::string_view("01xz").find(bit) != std::string_view::npos;
        }))
        return std::nullopt;
    return bits;
}

/// The characters a string literal is, `"a\"b"`, its escapes undone; nothing when it is not one.
std::optional<std::string> stringLiteral(std::string_view text) {
    if (text.size() < 2 || !text.starts_with('"') || !text.ends_with('"')) return std::nullopt;
    text = text.substr(1, text.size() - 2);
    std::string characters;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] != '\\') {
            characters += text[at];
            continue;
        }
        if (at + 1 < text.size() && (text[at + 1] == '"' || text[at + 1] == '\\')) {
            characters += text[++at];
            continue;
        }
        if (at + 3 >= text.size()) return std::nullopt;
        unsigned code = 0;
        const auto [end, error] =
            std::from_chars(text.data() + at + 1, text.data() + at + 4, code, 8);
        if (error != std::errc() || end != text.data() + at + 4 || code > 0xffU)
            return std::nullopt;
        characters += static_cast<char>(code);
        at += 3;
    }
    return characters;
}

/// The position among the values of the enumeration `type` of `bit`, a literal's `0`, `1`, `x` or
/// `z`: a `boolean`'s false or true for 0 or 1, and otherwise its character literal of that bit,
/// `'0'`, `'1'`, `'X'` or `'Z'`.
std::optional<std::uint64_t> positionOf(type_t type, char bit) {
    type_t base = type_base_recur(type);
    if (type_has_ident(base)
        && std::string_view(istr(type_ident(base))) == "STD.STANDARD.BOOLEAN") {
        if (bit == '0' || bit == '1') return bit == '1' ? 1U : 0U;
        return std::nullopt;
    }
    const char wanted = static_cast<char>(std::toupper(static_cast<unsigned char>(bit)));
    for (unsigned index = 0; index < type_enum_literals(base); ++index) {
        const std::string_view name = istr(tree_ident(type_enum_literal(base, index)));
        if (name.size() == 3 && name[0] == '\'' && name[1] == wanted && name[2] == '\'')
            return index;
    }
    return std::nullopt;
}

/// The name of `type`'s base type, `STD.STANDARD.BIT`, or nothing when it has none.
std::string_view baseName(type_t type) {
    type_t base = type_base_recur(type);
    return type_has_ident(base) ? std::string_view(istr(type_ident(base))) : std::string_view();
}

/// What a generic of `type` is as a SystemVerilog parameter, or nothing when it is none.
std::optional<eir::UnitInterface::Kind> kindOf(type_t type) {
    using Kind = eir::UnitInterface::Kind;
    if (type_is_integer(type)) return Kind::Integer;
    if (type_is_real(type) && !type_is_array(type)) return Kind::Real;
    const auto scalar = [](type_t of) -> std::optional<Kind> {
        const std::string_view name = baseName(of);
        if (name == "STD.STANDARD.BIT" || name == "STD.STANDARD.BOOLEAN") return Kind::Bit;
        if (name == "IEEE.STD_LOGIC_1164.STD_ULOGIC") return Kind::Logic;
        return std::nullopt;
    };
    if (!type_is_array(type)) return scalar(type);
    if (dimension_of(type) != 1) return std::nullopt;
    type_t element = type_elem(type);
    if (baseName(element) == "STD.STANDARD.CHARACTER") return Kind::String;
    const std::optional<Kind> elements = scalar(element);
    if (!elements) return std::nullopt;
    return *elements == Kind::Bit ? Kind::BitVector : Kind::LogicVector;
}

/// Whether a value of `type` has a `std_ulogic` in it, which makes it four-state in SystemVerilog.
bool holdsLogic(type_t type) {
    if (type_is_array(type)) return holdsLogic(type_elem(type));
    if (type_is_record(type)) {
        type_t base = type_base_recur(type);
        for (unsigned index = 0; index < type_fields(base); ++index) {
            if (holdsLogic(tree_type(type_field(base, index)))) return true;
        }
        return false;
    }
    return encodingOf(type) == eir::Encoding::VhdlLogic;
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

    /// Reports that computing `what` is not supported, as `DesignTranslator::uncomputed` does.
    void uncomputed(tree_t where, std::string_view what, const FoldTrace& trace);

    void warn(tree_t where, std::string_view what) override;

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
    /// to the definition, which VHPI shows as a constant as it shows a generic. One whose value
    /// does not fold is computed where it is used, and shows as nothing.
    void addConstant(tree_t declaration);

    void declarations(tree_t container);
    void statements(tree_t container);
    void instance(tree_t statement);

    /// An instance of `component` bound to the SystemVerilog module `module`, which names the
    /// placeholder of a request for it and connects the request's ports (`eir::UnitRequest`).
    void moduleInstance(tree_t statement, tree_t component, const std::string& module);

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
    /// parts, or with part of a signal, a conversion or an expression. An inout port has a second
    /// object for its driving value, and a process for each association each way. The
    /// associations' formals name `formalPort`, the port itself or a component's of its name.
    /// Nothing, having reported why, when the port cannot be connected.
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
    /// is inside, each followed by a dot, as SystemVerilog's are.
    std::string prefix;
};

/// The definition a package's subprograms are translated into: one with no objects, which a call
/// needs no instance of.
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

    void warn(tree_t where, std::string_view what) override;

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
        // A `-G` naming no generic of a top level is reported, since a misspelt one otherwise
        // changes nothing without a word; but only once the tops' generics have all been bound.
        std::vector<bool> used(request.generics.size(), false);
        bool allBound = true;
        const std::vector<tree_t> chosen = request.onlyRequested ? std::vector<tree_t>() : tops();
        for (tree_t top : chosen) {
            tree_t architecture = architectureOf(top);
            if (architecture == nullptr) {
                engine.report(DiagCode::VhdlError, locate(top),
                              std::format("entity '{}' has no architecture to simulate",
                                          simpleName(tree_ident(top))));
                allBound = false;
                continue;
            }
            std::optional<Folder> generics = bindTopGenerics(top, used);
            if (!generics) {
                allBound = false;
                continue;
            }
            static_cast<void>(definitionFor(top, architecture, std::move(*generics)));
        }
        if (!chosen.empty() && allBound) {
            for (std::size_t index = 0; index < used.size(); ++index) {
                if (!used[index])
                    engine.report(DiagCode::UnusedParameterOverride, SourceLocation(),
                                  request.generics[index]);
            }
        }

        // What SystemVerilog asks of the design: an entity it instantiates for its values, and the
        // interface of each it may.
        for (const eir::UnitRequest& asked : request.requests) answered.push_back(answer(asked));
        for (const std::string& name : request.described) {
            if (std::optional<eir::UnitInterface> unit = describe(name))
                described.push_back(std::move(*unit));
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
                unsupported(declaration, "a resolution function whose parameter's index range "
                                         "is not static");
                continue;
            }
            const Shape shape{.bounds = {bounds}, .values = {std::nullopt}};
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

    /// The subprogram `declaration` specialised for `shape`: from `architecture` when it is one
    /// of that architecture's, and translated the first time.
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
        // elaboration asks for alone.
        tree_t unit = tree_container(body);
        const tree_kind_t unitKind = tree_kind(unit);
        const bool inPackage =
            unitKind == T_PACK_BODY || unitKind == T_PACKAGE
            || (architecture == nullptr && (unitKind == T_ENTITY || unitKind == T_ARCH));
        if (!inPackage && architecture == nullptr) {
            unsupported(where, "a call from a package of a subprogram outside it");
            return std::nullopt;
        }

        // The values a specialisation depends on: the bounds of its unconstrained parameters, the
        // scalars its own bounds are computed from, and those that reach the bounds of what it
        // calls where the call has them.
        const ShapeParameters& shaped = shapeParameters(body, shapes);
        SpecialisationKey key{.body = body,
                              .definition =
                                  inPackage ? std::string() : architecture->definition().name,
                              .bounds = {},
                              .elementBounds = {},
                              .values = {},
                              .runTimeStrings = {}};
        const auto asKey = [](const std::optional<Folder::Range>& range) {
            return range ? std::optional(std::tuple(range->left, range->right, range->ascending))
                         : std::nullopt;
        };
        for (unsigned index = 0; index < tree_ports(body); ++index) {
            key.bounds.push_back(
                asKey(index < shape.bounds.size() ? shape.bounds[index] : std::nullopt));
            key.elementBounds.push_back(asKey(
                index < shape.elementBounds.size() ? shape.elementBounds[index] : std::nullopt));
            key.runTimeStrings.push_back(
                index < shape.runTimeStrings.size() ? shape.runTimeStrings[index] : std::nullopt);
            const std::optional<std::int64_t> value =
                index < shape.values.size() ? shape.values[index] : std::nullopt;
            if (shaped.required[index] && !value) {
                unsupported(where,
                            std::format("a call of {} whose parameter '{}', which its bounds "
                                        "depend on, is not static",
                                        simpleName(tree_ident(body)),
                                        simpleName(tree_ident(tree_port(body, index)))));
                return std::nullopt;
            }
            key.values.push_back(shaped.required[index] || shaped.wanted[index] ? value
                                                                                : std::nullopt);
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
            // A string whose length only the run knows arrives as its handle into scratch.
            if (const std::optional<bool>& runTime = key.runTimeStrings[index]; !layout && runTime)
                layout = runTimeStringLayout(type, *runTime);
            const auto& given = key.bounds[index];
            if (!layout && given) {
                const auto& [left, right, ascending] = *given;
                const Folder::Range bounds{.left = left, .right = right, .ascending = ascending};
                bindings.folder.bindBounds(parameter, bounds);
                // Each element's bounds too, where the element type leaves them to the actual.
                std::optional<Folder::Range> elements;
                if (const auto& element = key.elementBounds[index]) {
                    const auto& [low, high, up] = *element;
                    elements = Folder::Range{.left = low, .right = high, .ascending = up};
                    bindings.folder.bindElementBounds(parameter, *elements);
                }
                layout = arrayLayout(bounds, type_elem(type), bindings.folder, {}, elements);
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

    /// The library of the package declaring `component`, or null when no package does: the one
    /// `instance` names it through, `alib.apkg.acomp`, or one a use clause of `unit` names, whose
    /// declarations hold it.
    ident_t declaringLibrary(tree_t component, tree_t instance, tree_t unit) const {
        if (tree_has_ident2(instance)) {
            ident_t written = tree_ident2(instance);
            ident_t prefix = ident_runtil(written, '.');
            if (prefix != written && ident_until(prefix, '.') != prefix)
                return ident_until(prefix, '.');
        }
        if (unit == nullptr) return nullptr;

        std::vector<tree_t> scopes{unit};
        if (tree_kind(unit) == T_ARCH && tree_has_primary(unit))
            scopes.push_back(tree_primary(unit));
        for (tree_t scope : scopes) {
            for (unsigned index = 0; index < tree_contexts(scope); ++index) {
                tree_t item = tree_context(scope, index);
                if (tree_kind(item) != T_USE) continue;
                ident_t packageName = tree_ident(item);
                ident_t libraryName = ident_until(packageName, '.');
                if (libraryName == packageName) continue;
                if (libraryName == well_known(W_WORK))
                    libraryName = ident_until(tree_ident(unit), '.');
                lib_t library =
                    libraryName == lib_name(request.work) ? request.work : lib_require(libraryName);
                if (library == nullptr) continue;
                tree_t package = lib_get(
                    library, ident_prefix(lib_name(library), ident_rfrom(packageName, '.'), '.'));
                if (package == nullptr || tree_kind(package) != T_PACKAGE) continue;
                for (unsigned decl = 0; decl < tree_decls(package); ++decl) {
                    if (tree_decl(package, decl) == component) return lib_name(library);
                }
            }
        }
        return nullptr;
    }

    /// The entity called `simple` that a use clause of `unit`, or of the entity it is an
    /// architecture of, makes directly visible: `use alib.acomp;` or `use alib.all;` (IEEE 1076
    /// section 12.4). Null when none does.
    tree_t visibleEntity(ident_t simple, tree_t unit) {
        std::vector<tree_t> scopes{unit};
        if (tree_kind(unit) == T_ARCH && tree_has_primary(unit))
            scopes.push_back(tree_primary(unit));
        for (tree_t scope : scopes) {
            for (unsigned index = 0; index < tree_contexts(scope); ++index) {
                tree_t item = tree_context(scope, index);
                if (tree_kind(item) != T_USE || !tree_has_ident2(item)) continue;

                // A library's unit or all of them; a package's declarations are no entity.
                ident_t libraryName = tree_ident(item);
                if (ident_until(libraryName, '.') != libraryName) continue;
                ident_t wanted = tree_ident2(item);
                if (wanted != well_known(W_ALL) && !ident_casecmp(wanted, simple)) continue;

                if (libraryName == well_known(W_WORK))
                    libraryName = ident_until(tree_ident(unit), '.');
                lib_t library =
                    libraryName == lib_name(request.work) ? request.work : lib_require(libraryName);
                if (library == nullptr) continue;
                ident_t name = ident_prefix(lib_name(library), simple, '.');
                for (tree_t analysed : std::views::reverse(request.analysed)) {
                    if (tree_kind(analysed) == T_ENTITY && tree_ident(analysed) == name)
                        return analysed;
                }
                if (tree_t found = lib_get(library, name);
                    found != nullptr && tree_kind(found) == T_ENTITY)
                    return found;
            }
        }
        return nullptr;
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
            tree_t actual = mappedActual(instance, generic, index);
            const bool fromParent = actual != nullptr;
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

    /// The entity a component `instance` instantiates is bound to by default (IEEE 1076 section
    /// 7.3.3): the one of its name a use clause makes directly visible where it is instantiated,
    /// or else the one of its name in the library of the package declaring the component, or in
    /// the library of `unit`, or else in the work library. Null when there is none.
    tree_t boundEntity(tree_t component, tree_t instance, tree_t unit) {
        ident_t simple = ident_rfrom(tree_ident(component), '.');
        if (unit != nullptr) {
            if (tree_t visible = visibleEntity(simple, unit)) return visible;
        }
        std::vector<lib_t> libraries;

        // A component a package declares, `alib.apkg.acomp`, is bound in the package's library,
        // as NVC binds it and GHDL does but in its strict VHDL-93.
        if (ident_t libraryName = declaringLibrary(component, instance, unit)) {
            if (lib_t own =
                    libraryName == lib_name(request.work) ? request.work : lib_require(libraryName))
                libraries.push_back(own);
        }
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
        const FoldTrace trace;
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
        if (actual != nullptr
            && (type_is_scalar(type) || type_is_array(type) || type_is_record(type))) {
            uncomputed(
                generic,
                std::format("the value of the generic '{}'", simpleName(tree_ident(generic))),
                trace);
            return false;
        }
        unsupported(generic, std::format("the value of the generic '{}', which is not a static "
                                         "value of a scalar, array or record type",
                                         simpleName(tree_ident(generic))));
        return false;
    }

    [[nodiscard]] SourceLocation locate(tree_t where) const { return locator(tree_loc(where)); }

    void unsupported(tree_t where, std::string_view what) {
        static_cast<void>(refuse(where, what));
    }

    /// Reports that computing `what`, a value the language requires to be static, is not
    /// supported at `where`, and where the constant call it depends on stopped when `trace`
    /// knows. The value is static, which NVC's analysis checks, so it is the adapter that could
    /// not compute it, and the reader needs to know which call stopped rather than to be told
    /// the value is not static (`FoldStop`).
    void uncomputed(tree_t where, std::string_view what, const FoldTrace& trace) {
        DiagnosticBuilder diagnostic = refuse(where, std::format("computing {}", what));
        const std::optional<FoldStop>& stop = trace.stop();
        if (!stop) return;
        const std::string called =
            tree_has_ref(stop->call) ? declaredName(tree_ref(stop->call)) : std::string("it");
        note(diagnostic, stop->at != nullptr ? stop->at : stop->call, stopped(*stop, called));
    }

    /// What `stop` says of the call of `called`, as the note under a value that did not fold.
    static std::string stopped(const FoldStop& stop, const std::string& called) {
        switch (stop.reason) {
        case FoldStop::Reason::Statement: {
            const tree_kind_t kind = tree_kind(stop.at);
            const char* what = "statement";
            if (kind == T_PCALL || kind == T_PROT_PCALL) {
                what = "procedure call";
            } else if (kind == T_REPORT) {
                what = "report";
            }
            return std::format("computing '{}' stopped at this {}, which cannot be computed "
                               "before the simulation yet",
                               called, what);
        }
        case FoldStop::Reason::Declaration:
            return std::format("computing '{}' stopped at this declaration, which cannot be "
                               "computed before the simulation yet",
                               called);
        case FoldStop::Reason::Value:
            return std::format("computing '{}' stopped here, at a value that could not be "
                               "computed before the simulation",
                               called);
        case FoldStop::Reason::Assertion:
            return std::format("computing '{}' stopped at this assertion, which it could not show "
                               "to hold",
                               called);
        case FoldStop::Reason::VariableType:
            return std::format("computing '{}' stopped at this variable, whose type cannot be "
                               "computed with before the simulation yet",
                               called);
        case FoldStop::Reason::TooLong:
            return std::format("computing '{}' ran a million statements without returning", called);
        case FoldStop::Reason::Impure:
            return std::format("'{}' is impure, so it is not computed before the simulation",
                               called);
        case FoldStop::Reason::TooDeep:
            return std::format("computing '{}' nests calls more than 64 deep", called);
        case FoldStop::Reason::NoBody:
            return std::format("the body of '{}' has not been analysed", called);
        case FoldStop::Reason::ParameterType:
            return std::format("'{}' takes a parameter of a type that cannot be computed with "
                               "before the simulation yet",
                               called);
        case FoldStop::Reason::NoReturn:
            return std::format("'{}' ends without returning a value", called);
        }
        return {};
    }

    /// Reports `what` as not supported at `where`, and returns the report to add notes to.
    DiagnosticBuilder refuse(tree_t where, std::string_view what) {
        DiagnosticBuilder diagnostic =
            engine.report(DiagCode::VhdlUnsupported, locate(where), what);

        // A unit of another library, `ieee` say, has no buffer here, so its position is a note,
        // as NVC's own diagnostics there are.
        const loc_t* location = tree_loc(where);
        if (!locate(where).valid() && location != nullptr && !loc_invalid_p(location)) {
            diagnostic.note({},
                            std::format("at {}:{}", loc_file_str(location), location->first_line));
        }
        return diagnostic;
    }

    /// Adds `text` to `diagnostic` as a note at `where`, or naming its position where it has no
    /// buffer here.
    void note(DiagnosticBuilder& diagnostic, tree_t where, std::string text) const {
        if (locate(where).valid()) {
            diagnostic.note(locate(where), std::move(text));
            return;
        }
        const loc_t* location = tree_loc(where);
        if (location != nullptr && !loc_invalid_p(location))
            text += std::format(", at {}:{}", loc_file_str(location), location->first_line);
        diagnostic.note({}, std::move(text));
    }

    DiagnosticEngine& diagnostics() { return engine; }

    /// The SystemVerilog modules a component called `simple` may be bound to: those of its name in
    /// any case.
    [[nodiscard]] std::vector<std::string> modulesNamed(std::string_view simple) const {
        std::vector<std::string> found;
        for (const std::string& module : request.modules) {
            if (sameName(module, simple)) found.push_back(module);
        }
        return found;
    }

    /// The index of the request for `module` with `parameters`, made the first time it is asked
    /// for. Its placeholder is the module and the values, as no definition can be named. A value
    /// stays defaulted only while every instance asking leaves it so.
    std::uint32_t requestFor(const std::string& module,
                             std::vector<eir::UnitRequest::Parameter> parameters) {
        const auto sameValue = [](const eir::UnitRequest::Parameter& one,
                                  const eir::UnitRequest::Parameter& other) {
            return one.name == other.name && one.value == other.value
                   && one.boolean == other.boolean;
        };
        for (std::uint32_t index = 0; index < moduleRequests.size(); ++index) {
            std::vector<eir::UnitRequest::Parameter>& asked = moduleRequests[index].parameters;
            if (moduleRequests[index].unit != module
                || !std::ranges::equal(asked, parameters, sameValue))
                continue;
            for (std::size_t at = 0; at < asked.size(); ++at)
                asked[at].defaulted = asked[at].defaulted && parameters[at].defaulted;
            return index;
        }
        std::string values;
        for (const eir::UnitRequest::Parameter& parameter : parameters) {
            if (!values.empty()) values += ", ";
            values += std::format(".{}({})", parameter.name, parameter.value);
        }
        moduleRequests.push_back({.placeholder = std::format("{} #({})", module, values),
                                  .unit = module,
                                  .parameters = std::move(parameters),
                                  .ports = {}});
        return static_cast<std::uint32_t>(moduleRequests.size() - 1);
    }

    /// The index among request `index`'s ports of the one called `name`, added as connected in
    /// `direction` the first time.
    std::uint32_t requestPort(std::uint32_t index, const std::string& name,
                              eir::PortDirection direction) {
        std::vector<eir::UnitRequest::PortUse>& ports = moduleRequests[index].ports;
        for (std::uint32_t port = 0; port < ports.size(); ++port) {
            if (ports[port].name == name) return port;
        }
        ports.push_back({.name = name, .direction = direction});
        return static_cast<std::uint32_t>(ports.size() - 1);
    }

    [[nodiscard]] const std::string& placeholderOf(std::uint32_t index) const {
        return moduleRequests[index].placeholder;
    }

    /// What the design exchanges with SystemVerilog, once it is translated.
    Exchange takeExchange() {
        return Exchange{.asked = std::move(moduleRequests),
                        .answers = std::move(answered),
                        .interfaces = std::move(described)};
    }

    [[nodiscard]] std::int8_t timeUnitExponent() const { return request.timeUnitExponent; }

private:
    /// The entities the design is translated from: the top named, or else the one entity this run
    /// analysed that nothing instantiates. Nothing, having reported them, when there are several.
    std::vector<tree_t> tops() {
        if (!request.top.empty()) {
            // Left to elaboration, when there is none, which says what the design does have.
            tree_t top = entityIn(request.work, request.top);
            return top == nullptr ? std::vector<tree_t>() : std::vector<tree_t>{top};
        }

        // Every entity this run analysed that no architecture of it instantiates.
        // What each architecture instantiates, and each instance of a component with the unit it
        // is in, since a component stands for the entity it is bound to there.
        struct Collected {
            std::set<tree_t> instantiated;
            std::vector<std::pair<tree_t, tree_t>> components;
            tree_t unit = nullptr;
        } collected;

        for (tree_t unit : request.analysed) {
            if (tree_kind(unit) != T_ARCH) continue;
            collected.unit = unit;
            tree_visit_only(
                unit,
                [](tree_t instance, void* context) {
                    if (!tree_has_ref(instance)) return;
                    auto& into = *static_cast<Collected*>(context);
                    tree_t target = tree_ref(instance);
                    if (tree_kind(target) == T_ARCH && tree_has_primary(target))
                        target = tree_primary(target);
                    if (tree_kind(target) == T_COMPONENT)
                        into.components.emplace_back(instance, into.unit);
                    else
                        into.instantiated.insert(target);
                },
                &collected, T_INSTANCE);
        }
        std::set<tree_t>& instantiated = collected.instantiated;
        for (const auto& [instance, unit] : collected.components) {
            if (tree_t entity = specifiedEntity(instance)) {
                instantiated.insert(entity);
                continue;
            }
            if (tree_t entity = boundEntity(tree_ref(instance), instance, unit))
                instantiated.insert(entity);
        }
        // Nor is one SystemVerilog instantiates a top level.
        std::vector<tree_t> roots;
        for (tree_t unit : request.analysed) {
            if (tree_kind(unit) != T_ENTITY || instantiated.contains(unit)) continue;
            const std::string name = simpleName(tree_ident(unit));
            if (std::ranges::any_of(request.described,
                                    [&name](const std::string& fromSystemVerilog) {
                                        return sameName(fromSystemVerilog, name);
                                    }))
                continue;
            roots.push_back(unit);
        }
        if (roots.size() <= 1) return roots;

        // Choosing among them is safe only when the choice is forced, as elaboration has it of
        // modules: which one is the top is the user's to say, and an entity nothing instantiates
        // need not be one at its defaults at all, as a wrapper whose defaults overflow is not.
        std::vector<std::string> rootNames;
        rootNames.reserve(roots.size());
        for (tree_t root : roots) rootNames.push_back(declaredName(root));
        std::ranges::sort(rootNames);
        std::string candidates;
        for (const std::string& name : rootNames)
            candidates += std::format("{}'{}'", candidates.empty() ? "" : ", ", name);
        engine.report(DiagCode::NoTopModule, SourceLocation(), roots.size(), candidates);
        return {};
    }

    /// The entity a configuration specification binds the component `instance` to, `for u : c
    /// use entity work.e(a);`, or null when there is none or it binds a configuration.
    static tree_t specifiedEntity(tree_t instance) {
        if (!tree_has_spec(instance)) return nullptr;
        tree_t binding = tree_value(tree_spec(instance));
        if (binding == nullptr || tree_kind(binding) != T_BINDING || !tree_has_ref(binding))
            return nullptr;
        tree_t bound = tree_ref(binding);
        if (tree_kind(bound) == T_ARCH)
            return tree_has_primary(bound) ? tree_primary(bound) : nullptr;
        return tree_kind(bound) == T_ENTITY ? bound : nullptr;
    }

    /// The entity called `name`, in any case, in `library`: the last this run analysed, which
    /// replaced any before it of its name, as its architecture is the last analysed
    /// (`architectureOf`), or else the library's. Null when there is none.
    tree_t entityIn(lib_t library, std::string_view name) {
        std::string wanted(name);
        for (char& character : wanted)
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        ident_t qualified = ident_prefix(lib_name(library), ident_new(wanted.c_str()), '.');
        if (library == request.work) {
            for (tree_t unit : std::views::reverse(request.analysed)) {
                if (tree_kind(unit) == T_ENTITY && tree_ident(unit) == qualified) return unit;
            }
        }
        tree_t unit = lib_get(library, qualified);
        return unit != nullptr && tree_kind(unit) == T_ENTITY ? unit : nullptr;
    }

    /// The entity SystemVerilog names `name`: the work library's, or else the first of the other
    /// libraries the design is analysed against that has one. Null when none does.
    tree_t entityNamed(std::string_view name) {
        if (tree_t entity = entityIn(request.work, name)) return entity;
        for (const std::string& library : request.libraries) {
            std::string upper = library;
            for (char& character : upper)
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            lib_t found = lib_require(ident_new(upper.c_str()));
            if (found == nullptr) continue;
            if (tree_t entity = entityIn(found, name)) return entity;
        }
        return nullptr;
    }

    /// The name `declaration` was declared with, in the case written, which SystemVerilog names it
    /// in; its name in lower case when the source is not one this run read.
    [[nodiscard]] std::string declaredName(tree_t declaration) const {
        std::string lower = simpleName(tree_ident(declaration));
        const SourceLocation at = locate(declaration);
        if (request.sources == nullptr || !at.valid()) return lower;
        const std::string_view text = request.sources->bufferText(at.buffer());
        if (at.offset() > text.size() || text.size() - at.offset() < lower.size()) return lower;
        const std::string_view written = text.substr(at.offset(), lower.size());
        return sameName(written, lower) ? std::string(written) : lower;
    }

    /// The name of the definition the entity SystemVerilog asks for is with the values it asks for,
    /// or an empty one having reported why it cannot be translated.
    std::string answer(const eir::UnitRequest& asked) {
        tree_t entity = entityNamed(asked.unit);
        if (entity == nullptr) {
            engine.report(
                DiagCode::VhdlError, SourceLocation(),
                std::format("SystemVerilog instantiates '{}', which no entity is", asked.unit));
            return {};
        }
        tree_t architecture = architectureOf(entity);
        if (architecture == nullptr) {
            engine.report(DiagCode::VhdlError, locate(entity),
                          std::format("entity '{}' has no architecture to simulate",
                                      simpleName(tree_ident(entity))));
            return {};
        }

        // A port of an unconstrained type takes its bounds from its actual, which SystemVerilog
        // does not give.
        for (unsigned index = 0; index < tree_ports(entity); ++index) {
            type_t type = tree_type(tree_port(entity, index));
            if (type_is_array(type) && type_is_unconstrained(type)) {
                unsupported(tree_port(entity, index),
                            std::format("the port '{}' of an unconstrained type, which nothing in "
                                        "SystemVerilog gives bounds,",
                                        simpleName(tree_ident(tree_port(entity, index)))));
                return {};
            }
        }

        std::optional<Folder> generics = bindLiterals(entity, asked.parameters);
        if (!generics) return {};
        return definitionFor(entity, architecture, std::move(*generics));
    }

    /// The values of `entity`'s generics SystemVerilog sets, each by its name in any case, and
    /// each other's default. Nothing, having reported why, when one cannot be bound.
    std::optional<Folder> bindLiterals(tree_t entity,
                                       const std::vector<eir::UnitRequest::Parameter>& values) {
        Folder bound;
        for (unsigned index = 0; index < tree_generics(entity); ++index) {
            tree_t generic = tree_generic(entity, index);
            const std::string name = simpleName(tree_ident(generic));
            const auto given =
                std::ranges::find_if(values, [&name](const eir::UnitRequest::Parameter& value) {
                    return sameName(value.name, name);
                });
            if (given == values.end()) {
                if (!tree_has_value(generic)) {
                    engine.report(DiagCode::VhdlError, locate(generic),
                                  std::format("the generic '{}' of '{}' has no default, and the "
                                              "SystemVerilog instantiating it sets none",
                                              name, simpleName(tree_ident(entity))));
                    return std::nullopt;
                }
                if (!bindGeneric(bound, generic, tree_value(generic), bound)) return std::nullopt;
                continue;
            }
            if (!bindLiteral(bound, generic, given->value)) {
                unsupported(generic, std::format("setting the generic '{}' to SystemVerilog's {}",
                                                 name, given->value));
                return std::nullopt;
            }
        }
        return bound;
    }

    /// Binds `generic` to the value of `text`, a literal (`eir::UnitRequest`); false when it is not
    /// a value of the generic's type.
    static bool bindLiteral(Folder& bound, tree_t generic, std::string_view text) {
        type_t type = tree_type(generic);
        if (type_is_integer(type)) {
            const std::optional<std::int64_t> value = integerLiteral(text);
            if (value) bound.bind(generic, *value);
            return value.has_value();
        }
        if (type_is_real(type) && !type_is_array(type)) {
            // Folded as the bits of the double, as the folder holds a real.
            const std::optional<double> value = realOf(text);
            if (value) bound.bind(generic, std::bit_cast<std::int64_t>(*value));
            return value.has_value();
        }
        if (type_is_enum(type)) {
            const std::optional<std::string> bits = bitsLiteral(text);
            if (!bits || bits->size() != 1) return false;
            const std::optional<std::uint64_t> position = positionOf(type, bits->front());
            if (position) bound.bind(generic, static_cast<std::int64_t>(*position));
            return position.has_value();
        }
        if (!type_is_array(type) || dimension_of(type) != 1) return false;

        // An array's elements, left first, as positions of its element type's values: a string's
        // characters are their codes, as `character`'s positions are.
        type_t element = type_elem(type);
        std::vector<std::uint64_t> positions;
        if (text.starts_with('"')) {
            if (baseName(element) != "STD.STANDARD.CHARACTER") return false;
            const std::optional<std::string> characters = stringLiteral(text);
            if (!characters) return false;
            for (const char character : *characters)
                positions.push_back(static_cast<unsigned char>(character));
        } else {
            const std::optional<std::string> bits = bitsLiteral(text);
            if (!type_is_enum(element) || !bits) return false;
            for (const char bit : *bits) {
                const std::optional<std::uint64_t> position = positionOf(element, bit);
                if (!position) return false;
                positions.push_back(*position);
            }
        }
        return bindPositions(bound, generic, positions);
    }

    /// Binds `generic`, of a one-dimensional array type, to the elements at `positions` of its
    /// element type's values, left first; false when they do not fit its subtype.
    static bool bindPositions(Folder& bound, tree_t generic,
                              const std::vector<std::uint64_t>& positions) {
        type_t type = tree_type(generic);
        type_t element = type_elem(type);

        // Its subtype's bounds, or for an unconstrained type the literal's, from the left of its
        // index subtype in the direction it runs, as a string literal's are (IEEE 1076 section
        // 9.3.2).
        std::optional<Layout> layout = layoutOf(type, bound);
        if (!layout) {
            const std::optional<Folder::Range> index =
                Folder().rangeOf(type_index(type_base_recur(type), 0));
            if (!index) return false;
            const auto last = static_cast<std::int64_t>(positions.size()) - 1;
            const Folder::Range bounds{.left = index->left,
                                       .right = index->ascending ? index->left + last
                                                                 : index->left - last,
                                       .ascending = index->ascending};
            bound.bindBounds(generic, bounds);
            // A null one, `""` most often, holds nothing, and its bounds are all it has.
            if (positions.empty()) return true;
            layout = arrayLayout(bounds, element, bound);
        }
        if (!layout || !layout->isArray()
            || layout->index.length() != static_cast<std::int64_t>(positions.size()))
            return false;
        eir::BitVector bits = eir::BitVector::filled(layout->storage.bitWidth(), eir::Logic::Zero);
        for (std::size_t at = 0; at < positions.size(); ++at) {
            eir::bitops::assignPartAt(
                bits.mutableRef(),
                eir::BitVector::fromUInt64(positions[at], layout->elementBits).ref(),
                (positions.size() - 1 - at) * layout->elementBits);
        }
        bound.bindComposite(generic, std::move(bits));
        return true;
    }

    /// What became of a value the command line gives a generic.
    enum class Override : std::uint8_t { Bound, NotOfItsType, OutOfRange };

    /// Binds `generic` to `text`, read as NVC's and GHDL's `-g` read a value of its type: an
    /// integer, a real, a physical value with its unit, an enumeration literal, or the elements of
    /// an array of characters, a string or a bit string, quoted or not.
    static Override bindOverride(Folder& bound, tree_t generic, const std::string& text) {
        type_t type = tree_type(generic);
        parsed_value_t parsed{};
        if (!parse_value(type, text.c_str(), &parsed)) return Override::NotOfItsType;

        if (type_is_array(type)) {
            const std::span<const std::uint8_t> elements(parsed.enums->values, parsed.enums->count);
            const std::vector<std::uint64_t> positions(elements.begin(), elements.end());
            std::free(parsed.enums); // NOLINT(cppcoreguidelines-no-malloc): NVC allocated it.
            return dimension_of(type) == 1 && bindPositions(bound, generic, positions)
                       ? Override::Bound
                       : Override::NotOfItsType;
        }

        // Folded as the bits of the double, as the folder holds a real.
        if (type_is_real(type)) {
            bound.bind(generic, std::bit_cast<std::int64_t>(parsed.real));
            return Override::Bound;
        }

        // An integer, a physical value in its primary unit, or an enumeration literal's position,
        // checked against its subtype's range, which parse_value does not know.
        if (const std::optional<Folder::Range> range = Folder().rangeOf(type)) {
            if (parsed.integer < std::min(range->left, range->right)
                || parsed.integer > std::max(range->left, range->right))
                return Override::OutOfRange;
        }
        bound.bind(generic, parsed.integer);
        return Override::Bound;
    }

    /// The values of a top level's generics: each one `-G` sets, by its name in any case, and each
    /// other's default. `used` marks which of `request.generics` named one. Nothing, having
    /// reported why, when one cannot be bound.
    std::optional<Folder> bindTopGenerics(tree_t top, std::vector<bool>& used) {
        Folder bound;
        for (unsigned index = 0; index < tree_generics(top); ++index) {
            tree_t generic = tree_generic(top, index);
            const std::string name = simpleName(tree_ident(generic));
            const auto given =
                std::ranges::find_if(request.generics, [&name](std::string_view set) {
                    return sameName(set.substr(0, set.find('=')), name);
                });
            if (given != request.generics.end()) {
                used[static_cast<std::size_t>(given - request.generics.begin())] = true;
                const std::string value = given->substr(given->find('=') + 1);
                const std::string typeName = lowered(type_pp(tree_type(generic)));
                switch (bindOverride(bound, generic, value)) {
                case Override::Bound: continue;
                case Override::NotOfItsType:
                    engine.report(DiagCode::VhdlError, locate(generic),
                                  std::format("-G {}: '{}' is not a value of the type {} of the "
                                              "generic '{}'",
                                              *given, value, typeName, name));
                    return std::nullopt;
                case Override::OutOfRange:
                    engine.report(DiagCode::VhdlError, locate(generic),
                                  std::format("-G {}: {} is outside the range of the type {} of "
                                              "the generic '{}'",
                                              *given, value, typeName, name));
                    return std::nullopt;
                }
            }
            if (!tree_has_value(generic)) {
                engine.report(DiagCode::VhdlError, locate(generic),
                              std::format("the generic '{}' of the top level '{}' has no default "
                                          "value; give it one with -G {}=<value>",
                                          name, simpleName(tree_ident(top)), name));
                return std::nullopt;
            }
            if (!bindGeneric(bound, generic, tree_value(generic), bound)) return std::nullopt;
        }
        return bound;
    }

    /// The interface of the entity SystemVerilog names `name`, which it is checked against as a
    /// module of that name, or nothing when no entity has the name. A generic no SystemVerilog
    /// parameter can hold is left out, so that one can only take its default.
    std::optional<eir::UnitInterface> describe(const std::string& name) {
        tree_t entity = entityNamed(name);
        if (entity == nullptr) return std::nullopt;
        eir::UnitInterface unit{.name = name, .parameters = {}, .ports = {}, .widths = {}};

        // The defaults, each folded where the generics before it are bound, as an instance setting
        // none folds them.
        Folder defaults;
        bool folding = true;
        for (unsigned index = 0; index < tree_generics(entity); ++index) {
            tree_t generic = tree_generic(entity, index);
            type_t type = tree_type(generic);
            const std::optional<eir::UnitInterface::Kind> kind = kindOf(type);
            folding = folding && tree_has_value(generic)
                      && bindGeneric(defaults, generic, tree_value(generic), defaults);
            if (!kind) continue;

            eir::UnitInterface::Parameter parameter{
                .name = declaredName(generic), .kind = *kind, .length = 0, .value = std::nullopt};
            if (type_is_array(type) && *kind != eir::UnitInterface::Kind::String) {
                if (const std::optional<Layout> layout = layoutOf(type, Folder()))
                    parameter.length = static_cast<std::uint32_t>(layout->index.length());
            }
            if (folding) parameter.value = systemVerilogLiteral(defaults, generic);
            unit.parameters.push_back(std::move(parameter));
        }

        for (unsigned index = 0; index < tree_ports(entity); ++index) {
            tree_t port = tree_port(entity, index);
            eir::PortDirection direction = eir::PortDirection::Input;
            switch (static_cast<port_mode_t>(tree_subkind(port))) {
            case PORT_IN: break;
            case PORT_OUT:
            case PORT_BUFFER: direction = eir::PortDirection::Output; break;
            case PORT_INOUT: direction = eir::PortDirection::InOut; break;
            default: continue;
            }
            type_t type = tree_type(port);
            const bool unpacked =
                type_is_array(type) && dimension_of(type) == 1
                && (type_is_array(type_elem(type)) || type_is_record(type_elem(type)));
            unit.ports.push_back({.name = declaredName(port),
                                  .direction = direction,
                                  .fourState = holdsLogic(type),
                                  .isSigned = type_is_integer(type),
                                  .unpacked = unpacked});
        }
        return unit;
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
            module.language = eir::Language::Vhdl;
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
        std::vector<std::optional<std::tuple<std::int64_t, std::int64_t, bool>>> elementBounds;
        std::vector<std::optional<std::int64_t>> values;
        std::vector<std::optional<bool>> runTimeStrings;

        friend auto operator<=>(const SpecialisationKey&, const SpecialisationKey&) = default;
    };

    /// The name of `body` specialised for `key`: its own name and signature, as NVC mangles the
    /// signature to tell overloads apart, then each parameter's bounds, its elements' after them
    /// where the actual gives those too, or value, `*to*` or `*downto*` for a string of any length,
    /// `_` for one the shape leaves free: `resolved(Y)U$0to1`, `ser(...)$0to2(3downto0)`,
    /// `int_str_to_int(S)I$*to*`. Made of what the specialisation is, so that
    /// two translations of one name one subprogram, as the second that elaboration asks for
    /// resolution functions does.
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
                if (const auto& element = key.elementBounds[index]) {
                    const auto& [low, high, up] = *element;
                    name += std::format("({}{}{})", low, up ? "to" : "downto", high);
                }
            } else if (const std::optional<bool>& runTime = key.runTimeStrings[index]) {
                name += *runTime ? "*to*" : "*downto*";
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

    /// Which parameters of each subprogram called its specialisations are made for.
    ShapeCache shapes;

    std::deque<Specialisation> pending;
    std::vector<eir::ModuleDefinition> modules;
    std::vector<eir::UnitRequest> moduleRequests;
    std::vector<std::string> answered;
    std::vector<eir::UnitInterface> described;
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

void ArchitectureTranslator::uncomputed(tree_t where, std::string_view what,
                                        const FoldTrace& trace) {
    design.uncomputed(where, what, trace);
}

void ArchitectureTranslator::warn(tree_t where, std::string_view what) {
    design.diagnostics().report(DiagCode::VhdlWarning, locate(where), what);
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

void PackageScope::warn(tree_t where, std::string_view what) {
    design.diagnostics().report(DiagCode::VhdlWarning, locate(where), what);
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
        described.encoding = encodingOf(type);
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
    // bit, since a bit vector has at least one: what names its bounds reads them from its type, or
    // from its actual for a port of an unconstrained type, and VHPI shows it as the empty array it
    // is. Nothing else of it is translated.
    bool null = false;
    if (!layout) {
        layout = declaredNullLayout(declaration, folding);
        null = layout.has_value();
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
    result.language = eir::Language::Vhdl;
    // The library the entity was analysed into, which a design names it by.
    result.library = simpleName(ident_until(tree_ident(entity), '.'));
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

        default: unsupported(declaration, "this " + kindName(tree_kind(declaration))); break;
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

        default: unsupported(statement, "this " + statementName(tree_kind(statement))); break;
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
            const FoldTrace trace;
            const std::optional<std::int64_t> holds = folding.integer(tree_value(alternative));
            if (!holds) {
                uncomputed(tree_value(alternative), "this generate condition", trace);
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
    const FoldTrace trace;
    const std::optional<Folder::Range> folded = folding.range(tree_range(statement, 0));
    if (parameter == nullptr || !folded) {
        uncomputed(statement, "the range of this generate loop", trace);
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
        // With no entity to bind by default, the instance is bound to the SystemVerilog module of
        // its name, as commercial tools extend IEEE 1076 section 7.3.3 to the other language, or
        // else it is unbound, which is legal: it has no behaviour, and its actuals are driven by
        // nothing in it.
        entity = design.boundEntity(component, statement, specialisation.architecture);
        if (entity == nullptr) {
            const std::string simple = systemVerilogName(simpleName(tree_ident(component)));
            const std::vector<std::string> modules = design.modulesNamed(simple);
            if (modules.size() == 1) {
                moduleInstance(statement, component, modules.front());
                return;
            }
            if (modules.size() > 1) {
                std::string listed;
                for (const std::string& module : modules)
                    listed += std::format("{}'{}'", listed.empty() ? "" : ", ", module);
                design.diagnostics().report(
                    DiagCode::VhdlError, locate(statement),
                    std::format("component '{}' has no entity of its name, and the SystemVerilog "
                                "modules {} differ from it only in case, so which it is bound to "
                                "is not known",
                                simple, listed));
                return;
            }
            design.diagnostics().report(DiagCode::VhdlUnboundComponent, locate(statement), simple);
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

void ArchitectureTranslator::moduleInstance(tree_t statement, tree_t component,
                                            const std::string& module) {
    // The component's generics are the module's parameters of their names, and its ports'
    // bounds fold with them.
    const std::optional<Folder> local = design.bindGenerics(component, statement, folding);
    if (!local) return;
    std::vector<eir::UnitRequest::Parameter> parameters;
    for (unsigned index = 0; index < tree_generics(component); ++index) {
        tree_t generic = tree_generic(component, index);
        std::optional<std::string> value = systemVerilogLiteral(*local, generic);
        if (!value) {
            unsupported(generic,
                        std::format("setting the parameter '{}' of the SystemVerilog module '{}' "
                                    "from a generic of type {}",
                                    simpleName(tree_ident(generic)), module,
                                    type_pp(tree_type(generic))));
            return;
        }
        type_t type = type_base_recur(tree_type(generic));
        const bool boolean = type_has_ident(type)
                             && std::string_view(istr(type_ident(type))) == "STD.STANDARD.BOOLEAN";
        parameters.push_back({.name = systemVerilogName(simpleName(tree_ident(generic))),
                              .value = std::move(*value),
                              .boolean = boolean,
                              .defaulted = mappedActual(statement, generic, index) == nullptr});
    }
    Folder formals = *local;

    std::vector<std::vector<tree_t>> byPort(tree_ports(component));
    for (unsigned index = 0; index < tree_params(statement); ++index) {
        tree_t association = tree_param(statement, index);
        const unsigned formalNumber =
            formalIndex(association, component).value_or(tree_ports(component));
        if (formalNumber >= tree_ports(component)) {
            unsupported(association, "this port association");
            continue;
        }
        byPort[formalNumber].push_back(association);
    }

    // A port of an unconstrained type takes its bounds from its actual (IEEE 1076 section
    // 6.5.6.3).
    for (unsigned portNumber = 0; portNumber < tree_ports(component); ++portNumber) {
        tree_t port = tree_port(component, portNumber);
        if (!unconstrained(port)) continue;
        const std::optional<Folder::Range> bounds =
            actualBounds(port, byPort[portNumber], statement, *local);
        if (!bounds) return;
        formals.bindBounds(port, *bounds);
    }

    eir::Instance created;
    created.name = prefix + simpleName(tree_ident(statement));
    const std::uint32_t request = design.requestFor(module, std::move(parameters));
    created.definitionName = design.placeholderOf(request);
    created.location = locate(statement);

    for (unsigned portNumber = 0; portNumber < tree_ports(component); ++portNumber) {
        const std::vector<tree_t>& associations = byPort[portNumber];
        if (associations.empty()) continue;
        tree_t port = tree_port(component, portNumber);
        eir::PortDirection direction = eir::PortDirection::Input;
        switch (static_cast<port_mode_t>(tree_subkind(port))) {
        case PORT_IN: break;
        case PORT_OUT:
        case PORT_BUFFER: direction = eir::PortDirection::Output; break;
        case PORT_INOUT: direction = eir::PortDirection::InOut; break;
        default:
            unsupported(port, "a port of this mode bound to a SystemVerilog module's");
            continue;
        }
        // An open port is not connected, and a whole signal is connected as itself; anything else
        // is connected through an object of the component port's type standing for it, as an
        // entity's port is.
        tree_t first = associations.front();
        const bool whole =
            associations.size() == 1
            && (tree_subkind(first) == P_POS || tree_kind(tree_name(first)) == T_REF);
        if (whole && tree_kind(tree_value(first)) == T_OPEN) continue;
        const std::string portName = systemVerilogName(simpleName(tree_ident(port)));
        const std::uint32_t portIndex = design.requestPort(request, portName, direction);
        if (whole) {
            tree_t actual = tree_value(first);
            if (tree_kind(actual) == T_REF && tree_has_ref(actual)) {
                if (const std::optional<std::uint32_t> object = objectFor(tree_ref(actual))) {
                    created.connections.push_back(
                        eir::PortConnection{.portIndex = portIndex, .objectIndex = *object});
                    if (direction != eir::PortDirection::Input) sourcedByInstances.insert(*object);
                    continue;
                }
            }
        }
        if (const std::optional<Through> through = connectThrough(
                std::format("{}${}", created.name, portName), port, port, associations, formals)) {
            created.connections.push_back(eir::PortConnection{.portIndex = portIndex,
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
    // generics. An actual is read in this architecture, whose generics and ports a unit
    // instantiating itself declares again, so the two ends are kept apart.
    Folder formalFolding = folding;
    formalFolding.absorb(childGenerics);
    const BodyTranslator::FormalScope formalScope{.folder = &formalFolding, .port = formalPort};
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
            std::optional<eir::Process> inward = in.translateAssociation(
                formalPart, actual, throughIndex, *layout, &start, &formalScope, true);
            if (inward) result.processes.push_back(std::move(*inward));

            BodyTranslator out(*this, association, name + "$out");
            std::optional<eir::Process> outward = out.translateAssociation(
                unconverted(actual), formal, drivenIndex, *layout, nullptr, &formalScope, false);
            if (outward) result.processes.push_back(std::move(*outward));
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
            input ? translator.translateAssociation(formal, actual, throughIndex, *layout, &start,
                                                    &formalScope, true)
                  : translator.translateAssociation(actual, formal, throughIndex, *layout, nullptr,
                                                    &formalScope, false);
        if (process) result.processes.push_back(std::move(*process));
    }
    result.objects[throughIndex].initialValue = std::move(start);
    return Through{.object = throughIndex, .driven = drivenIndex};
}

} // namespace

std::vector<eir::ModuleDefinition> translateDesign(const DesignRequest& request,
                                                   const Locate& locate,
                                                   DiagnosticEngine& diagnostics,
                                                   Exchange& exchange) {
    setIeeeWarnings(request.ieeeWarnings);
    DesignTranslator translator(request, locate, diagnostics);
    std::vector<eir::ModuleDefinition> definitions = translator.run();
    exchange = translator.takeExchange();
    return definitions;
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
