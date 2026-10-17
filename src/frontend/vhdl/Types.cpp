// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/Types.h"

#include "eir/BitOps.h"

#include <cctype>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

std::string simpleName(ident_t identifier) {
    std::string_view text = istr(identifier);

    // `WORK.COUNTER-RTL` is an architecture's name; `WORK.PKG.CONSTANT` an object's in a package.
    // An extended identifier may contain either character, so the search stops at its backslash.
    if (const std::size_t extended = text.find('\\'); extended != std::string_view::npos) {
        const std::size_t dot = text.rfind('.', extended);
        return std::string(dot == std::string_view::npos ? text : text.substr(dot + 1));
    }
    if (const std::size_t dot = text.rfind('.'); dot != std::string_view::npos)
        text = text.substr(dot + 1);

    std::string lowered(text);
    for (char& character : lowered)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lowered;
}

namespace {

/// The bits of storage needed for `count` positions: whole bytes, so that a code reads in a
/// waveform and in a debugger as the position it is.
std::uint32_t enumerationBits(std::uint32_t count) {
    if (count <= 0x100U) return 8;
    if (count <= 0x1'0000U) return 16;
    return 32;
}

/// What a static array value's elements are: their type and how each is laid out.
struct Elements {
    type_t type = nullptr;
    Layout layout;
};

/// The elements of a static array value, left to right; nothing when the value is not static.
/// `length` is the context's, which an aggregate with `others` needs.
std::optional<std::vector<eir::BitVector>> elementsOf(tree_t expression, const Elements& element,
                                                      std::optional<std::int64_t> length,
                                                      const Folder& folder);

/// Places `elements` into the bits of an array laid out as `layout`, or nothing when there are not
/// as many as it holds.
std::optional<eir::BitVector> pack(const std::vector<eir::BitVector>& elements,
                                   const Layout& layout) {
    if (static_cast<std::int64_t>(elements.size()) != layout.index.length()) return std::nullopt;
    eir::BitVector bits = eir::BitVector::filled(layout.storage.bitWidth(), eir::Logic::Zero);
    for (std::size_t position = 0; position < elements.size(); ++position) {
        const auto step = static_cast<std::int64_t>(position);
        const std::int64_t index =
            layout.index.ascending ? layout.index.left + step : layout.index.left - step;
        eir::bitops::assignPartAt(bits.mutableRef(), elements[position].ref(),
                                  static_cast<std::uint64_t>(layout.bitOffset(index)));
    }
    return bits;
}

std::optional<std::vector<eir::BitVector>> aggregateElements(tree_t aggregate,
                                                             const Elements& element,
                                                             std::optional<std::int64_t> length,
                                                             const Folder& folder) {
    // A named association names an index, so it needs the bounds of the type the aggregate has;
    // a positional one counts from the left.
    const std::optional<Folder::Range> bounds = folder.rangeOf(tree_type(aggregate));
    const auto positionOf = [&bounds](std::int64_t index) -> std::optional<std::size_t> {
        if (!bounds) return std::nullopt;
        const std::int64_t offset = bounds->ascending ? index - bounds->left : bounds->left - index;
        if (offset < 0 || offset >= bounds->length()) return std::nullopt;
        return static_cast<std::size_t>(offset);
    };

    std::vector<std::optional<eir::BitVector>> slots;
    const auto place = [&slots](std::size_t position, eir::BitVector value) {
        if (slots.size() <= position) slots.resize(position + 1);
        slots[position] = std::move(value);
    };

    std::size_t next = 0;
    tree_t others = nullptr;
    const unsigned count = tree_assocs(aggregate);
    for (unsigned index = 0; index < count; ++index) {
        tree_t association = tree_assoc(aggregate, index);
        tree_t value = tree_value(association);
        switch (tree_subkind(association)) {
        case A_POS: {
            std::optional<eir::BitVector> bits = constantBits(value, element.layout, folder);
            if (!bits) return std::nullopt;
            place(next++, std::move(*bits));
            break;
        }
        case A_CONCAT: {
            std::optional<std::vector<eir::BitVector>> pieces =
                elementsOf(value, element, std::nullopt, folder);
            if (!pieces) return std::nullopt;
            for (eir::BitVector& piece : *pieces) place(next++, std::move(piece));
            break;
        }
        case A_NAMED: {
            const std::optional<std::int64_t> at = folder.integer(tree_name(association));
            const std::optional<std::size_t> position = at ? positionOf(*at) : std::nullopt;
            std::optional<eir::BitVector> bits = constantBits(value, element.layout, folder);
            if (!position || !bits) return std::nullopt;
            place(*position, std::move(*bits));
            break;
        }
        case A_RANGE: {
            const std::optional<Folder::Range> range = folder.range(tree_range(association, 0));
            const std::optional<eir::BitVector> bits = constantBits(value, element.layout, folder);
            if (!range || !bits) return std::nullopt;
            const Folder::Range span = *range;
            const eir::BitVector& one = *bits;
            for (std::int64_t at = span.low(); at <= span.high(); ++at) {
                const std::optional<std::size_t> position = positionOf(at);
                if (!position) return std::nullopt;
                place(*position, one);
            }
            break;
        }
        case A_OTHERS: others = value; break;
        default: return std::nullopt;
        }
    }

    if (others != nullptr) {
        if (!length) return std::nullopt;
        const std::optional<eir::BitVector> bits = constantBits(others, element.layout, folder);
        if (!bits) return std::nullopt;
        slots.resize(static_cast<std::size_t>(*length));
        for (std::optional<eir::BitVector>& slot : slots) {
            if (!slot) slot = *bits;
        }
    }

    std::vector<eir::BitVector> elements;
    elements.reserve(slots.size());
    for (std::optional<eir::BitVector>& slot : slots) {
        if (!slot) return std::nullopt;
        elements.push_back(std::move(*slot));
    }
    return elements;
}

std::optional<std::vector<eir::BitVector>> elementsOf(tree_t expression, const Elements& element,
                                                      std::optional<std::int64_t> length,
                                                      const Folder& folder) {
    switch (tree_kind(expression)) {
    case T_STRING: {
        std::vector<eir::BitVector> elements;
        const unsigned count = tree_chars(expression);
        for (unsigned index = 0; index < count; ++index) {
            tree_t character = tree_char(expression, index);
            if (!tree_has_ref(character) || tree_kind(tree_ref(character)) != T_ENUM_LIT)
                return std::nullopt;
            elements.push_back(encode(tree_pos(tree_ref(character)), element.layout.storage));
        }
        return elements;
    }

    case T_AGGREGATE: return aggregateElements(expression, element, length, folder);

    case T_QUALIFIED:
    case T_TYPE_CONV: return elementsOf(tree_value(expression), element, length, folder);

    case T_REF: {
        if (!tree_has_ref(expression)) return std::nullopt;
        tree_t declaration = tree_ref(expression);
        if ((tree_kind(declaration) == T_CONST_DECL || tree_kind(declaration) == T_ALIAS)
            && tree_has_value(declaration))
            return elementsOf(tree_value(declaration), element, length, folder);

        // A null array, a parameter given `""` say, is bound to its bounds alone.
        if (const std::optional<Folder::Range> bounds = folder.boundsOf(expression);
            bounds && bounds->length() == 0 && folder.compositeOf(declaration) == nullptr)
            return std::vector<eir::BitVector>{};

        // An array generic's value is bound as its bits, left element highest.
        if (const eir::BitVector* bits = folder.compositeOf(declaration)) {
            const std::uint32_t width = element.layout.storage.bitWidth();
            if (width == 0 || bits->width() % width != 0) return std::nullopt;
            std::vector<eir::BitVector> elements;
            const std::uint32_t count = bits->width() / width;
            for (std::uint32_t position = 0; position < count; ++position) {
                eir::BitVector one(width);
                eir::bitops::partSelectAt(one.mutableRef(), bits->ref(),
                                          static_cast<std::uint64_t>(count - 1 - position) * width);
                elements.push_back(std::move(one));
            }
            return elements;
        }
        return std::nullopt;
    }

    // The image of a static scalar: an integer in decimal, an enumeration value as its literal,
    // and a physical value in its primary unit (IEEE 1076 section 16.2.2).
    case T_ATTR_REF: {
        if (tree_subkind(expression) != ATTR_IMAGE || tree_params(expression) != 1)
            return std::nullopt;
        tree_t operand = tree_value(tree_param(expression, 0));
        type_t type = tree_type(operand);
        const std::optional<std::int64_t> value = folder.integer(operand);
        if (!value) return std::nullopt;
        std::string text;
        type_t base = type_base_recur(type);
        if (type_is_enum(base)) {
            if (*value < 0 || std::cmp_greater_equal(*value, type_enum_literals(base)))
                return std::nullopt;
            tree_t literal = type_enum_literal(base, static_cast<unsigned>(*value));
            const std::string_view name = istr(tree_ident(literal));
            text = name.starts_with('\'') ? std::string(name) : simpleName(tree_ident(literal));
        } else if (type_is_integer(base)) {
            text = std::to_string(*value);
        } else if (type_is_physical(base) && type_units(base) > 0) {
            text = std::to_string(*value) + " " + simpleName(tree_ident(type_unit(base, 0)));
        } else {
            return std::nullopt;
        }
        std::vector<eir::BitVector> elements;
        elements.reserve(text.size());
        for (const char character : text) {
            elements.push_back(
                encode(static_cast<unsigned char>(character), element.layout.storage));
        }
        return elements;
    }

    // A slice of a static array: its elements in the slice's range.
    case T_ARRAY_SLICE: {
        tree_t prefix = tree_value(expression);
        const std::optional<Folder::Range> whole = folder.boundsOf(prefix);
        const std::optional<Folder::Range> slice = folder.range(tree_range(expression, 0));
        if (!whole || !slice) return std::nullopt;
        if (slice->length() == 0) return std::vector<eir::BitVector>{};
        if (slice->ascending != whole->ascending || slice->low() < whole->low()
            || slice->high() > whole->high())
            return std::nullopt;
        std::optional<std::vector<eir::BitVector>> all =
            elementsOf(prefix, element, whole->length(), folder);
        if (!all || static_cast<std::int64_t>(all->size()) != whole->length()) return std::nullopt;
        const std::int64_t first =
            whole->ascending ? slice->left - whole->left : whole->left - slice->left;
        const auto begin = all->begin() + first;
        return std::vector<eir::BitVector>(begin, begin + slice->length());
    }

    case T_FCALL: {
        // A constant function's result, folded from what it returns.
        if (tree_has_ref(expression)
            && hasVhdlBody(static_cast<subprogram_kind_t>(tree_subkind(tree_ref(expression))))) {
            std::optional<std::vector<eir::BitVector>> result;
            folder.returned(expression, [&](tree_t value, const Folder& frame) {
                result = elementsOf(value, element, length, frame);
                return result.has_value();
            });
            return result;
        }

        // A concatenation of static pieces: arrays, and single elements.
        if (!tree_has_ref(expression) || tree_subkind(tree_ref(expression)) != S_CONCAT)
            return std::nullopt;
        std::vector<eir::BitVector> elements;
        for (unsigned index = 0; index < tree_params(expression); ++index) {
            tree_t operand = tree_value(tree_param(expression, index));
            if (type_eq(tree_type(operand), element.type)) {
                std::optional<eir::BitVector> bits = constantBits(operand, element.layout, folder);
                if (!bits) return std::nullopt;
                elements.push_back(std::move(*bits));
                continue;
            }
            std::optional<std::vector<eir::BitVector>> pieces =
                elementsOf(operand, element, std::nullopt, folder);
            if (!pieces) return std::nullopt;
            for (eir::BitVector& piece : *pieces) elements.push_back(std::move(piece));
        }
        return elements;
    }

    default: return std::nullopt;
    }
}

} // namespace

std::optional<eir::Type> scalarStorage(type_t type) {
    if (type == nullptr) return std::nullopt;
    type_t base = type_base_recur(type);
    switch (type_kind(base)) {
    case T_ENUM: return eir::Type::bits(enumerationBits(type_enum_literals(base)), false, false);

    case T_INTEGER: {
        // The base type's range, not the subtype's: `natural` is held as `integer` is.
        std::int64_t low = 0;
        std::int64_t high = 0;
        const bool narrow = type_dims(base) > 0 && folded_bounds(range_of(base, 0), &low, &high)
                            && low >= std::numeric_limits<std::int32_t>::min()
                            && high <= std::numeric_limits<std::int32_t>::max();
        return eir::Type::bits(narrow ? 32 : 64, true, false);
    }

    case T_PHYSICAL: return eir::Type::bits(64, true, false);

    // An IEEE 754 double, as a real's code is its bits (DESIGN_GUIDE.md section 3.6).
    case T_REAL: return eir::Type::real();

    // An access to a string, std.textio's line, is a handle into the runtime's heap, and 0 is
    // `null` (section 4.3). Accesses to anything else are not held yet.
    case T_ACCESS: {
        type_t designated = type_designated(base);
        if (!type_is_array(designated)) return std::nullopt;
        type_t element = type_base_recur(type_elem(designated));
        if (!type_is_enum(element)
            || std::string_view(istr(type_ident(element))) != "STD.STANDARD.CHARACTER")
            return std::nullopt;
        return eir::Type::bits(64, false, false);
    }

    default: return std::nullopt;
    }
}

std::optional<Layout> arrayLayout(const Folder::Range& index, type_t elementType,
                                  const Folder& folder, const std::vector<Folder::Range>& inner) {
    // A row of the further dimensions is laid out as an array of its own.
    std::optional<Layout> element;
    if (inner.empty()) {
        element = layoutOf(elementType, folder);
    } else {
        element = arrayLayout(inner.front(), elementType, folder,
                              std::vector<Folder::Range>(inner.begin() + 1, inner.end()));
    }
    if (!element || index.length() <= 0) return std::nullopt;
    const std::int64_t bits = index.length() * element->storage.bitWidth();
    if (std::cmp_greater(bits, std::numeric_limits<std::uint32_t>::max())) return std::nullopt;
    return Layout{.storage = eir::Type::bits(static_cast<std::uint32_t>(bits), false, false),
                  .array = true,
                  .index = index,
                  .elementBits = element->storage.bitWidth(),
                  .elementType = elementType,
                  .inner = inner};
}

std::optional<Layout> elementLayout(const Layout& layout, const Folder& folder) {
    if (layout.inner.empty()) return layoutOf(layout.elementType, folder);
    return arrayLayout(layout.inner.front(), layout.elementType, folder,
                       std::vector<Folder::Range>(layout.inner.begin() + 1, layout.inner.end()));
}

std::optional<Layout> layoutOf(type_t type, const Folder& folder) {
    if (type == nullptr) return std::nullopt;
    if (type_is_record(type)) {
        type_t base = type_base_recur(type);
        Layout layout{.storage = {}, .recordType = base};
        std::uint64_t total = 0;
        for (unsigned index = 0; index < type_fields(base); ++index) {
            std::optional<Layout> field = layoutOf(tree_type(type_field(base, index)), folder);
            if (!field) return std::nullopt;
            total += field->storage.bitWidth();
            layout.fields.push_back(std::move(*field));
        }
        if (total == 0 || total > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        // The first field takes the highest bits, so each sits above every field after it.
        std::uint64_t below = total;
        for (const Layout& field : layout.fields) {
            below -= field.storage.bitWidth();
            layout.fieldOffsets.push_back(static_cast<std::uint32_t>(below));
        }
        layout.storage = eir::Type::bits(static_cast<std::uint32_t>(total), false, false);
        return layout;
    }
    if (type_is_array(type)) {
        const unsigned dimensions = dimension_of(type);
        std::vector<Folder::Range> ranges;
        for (unsigned dimension = 0; dimension < dimensions; ++dimension) {
            const std::optional<Folder::Range> range = folder.rangeOf(type, dimension);
            if (!range) return std::nullopt;
            ranges.push_back(*range);
        }
        if (ranges.empty()) return std::nullopt;
        return arrayLayout(ranges.front(), type_elem(type), folder,
                           std::vector<Folder::Range>(ranges.begin() + 1, ranges.end()));
    }
    const std::optional<eir::Type> storage = scalarStorage(type);
    if (!storage) return std::nullopt;
    return Layout{.storage = *storage};
}

std::optional<Layout> declaredLayout(tree_t declaration, const Folder& folder) {
    type_t type = tree_type(declaration);
    if (std::optional<Layout> layout = layoutOf(type, folder)) return layout;
    if (!type_is_array(type) || dimension_of(type) != 1) return std::nullopt;
    const std::optional<Folder::Range> bounds = folder.boundOf(declaration);
    if (!bounds) return std::nullopt;
    return arrayLayout(*bounds, type_elem(type), folder);
}

eir::Encoding encodingOf(type_t type) {
    while (type != nullptr && type_is_array(type)) type = type_elem(type);
    if (type == nullptr) return eir::Encoding::Bits;
    type_t base = type_base_recur(type);
    if (!type_has_ident(base)) return eir::Encoding::Bits;
    const std::string_view name = istr(type_ident(base));
    if (name == "IEEE.STD_LOGIC_1164.STD_ULOGIC") return eir::Encoding::VhdlLogic;
    if (name == "STD.STANDARD.BIT" || name == "STD.STANDARD.BOOLEAN") return eir::Encoding::VhdlBit;
    if (type_is_real(base)) return eir::Encoding::Real;
    return eir::Encoding::Bits;
}

std::optional<eir::Resolution> resolutionOf(type_t type) {
    if (type == nullptr) return std::nullopt;
    // Only a subtype has a resolution, the nearest of those it is declared through.
    for (type_t subtype = type; type_kind(subtype) == T_SUBTYPE; subtype = type_base(subtype)) {
        if (!type_has_resolution(subtype)) continue;

        // `(resolved) std_ulogic_vector` resolves each element; the function is innermost.
        tree_t name = type_resolution(subtype);
        type_t element = subtype;
        while (tree_kind(name) == T_ELEM_RESOLUTION) {
            if (!type_is_array(element) || tree_assocs(name) != 1) return std::nullopt;
            name = tree_value(tree_assoc(name, 0));
            element = type_elem(element);
        }
        const std::optional<eir::Type> storage = scalarStorage(element);
        if (tree_kind(name) != T_REF || !tree_has_ref(name) || !storage) return std::nullopt;

        // Proven to return a lone driver's value by running it on each value of an
        // enumeration of sixteen or fewer (DESIGN_GUIDE.md section 3.7).
        tree_t function = tree_ref(name);
        bool identity = false;
        type_t base = type_base_recur(element);
        if (type_is_enum(base) && type_enum_literals(base) <= 16) {
            identity = true;
            const Folder folder;
            for (std::int64_t code = 0; identity && std::cmp_less(code, type_enum_literals(base));
                 ++code) {
                const std::optional<std::int64_t> resolved =
                    folder.applyToArray(function, encode(code, *storage),
                                        Folder::Range{.left = 0, .right = 0, .ascending = true});
                identity = resolved == code;
            }
        }
        return eir::Resolution{.function = std::string(istr(tree_ident2(function))),
                               .elementBits = storage->bitWidth(),
                               .loneDriverIdentity = identity};
    }
    // An array of a resolved subtype, `array (...) of std_logic`.
    if (type_is_array(type)) return resolutionOf(type_elem(type));
    return std::nullopt;
}

bool isSignedScalar(type_t type) {
    if (type == nullptr) return false;
    const type_kind_t kind = type_kind(type_base_recur(type));
    return kind == T_INTEGER || kind == T_PHYSICAL;
}

std::optional<eir::EnumDefinition> enumerationOf(type_t type) {
    if (type == nullptr) return std::nullopt;
    type_t base = type_base_recur(type);
    if (type_kind(base) != T_ENUM) return std::nullopt;

    const eir::Type storage = scalarStorage(base).value_or(eir::Type::bits(8, false, false));
    eir::EnumDefinition definition;
    definition.name = type_has_ident(base) ? simpleName(type_ident(base)) : std::string();
    const unsigned count = type_enum_literals(base);
    definition.members.reserve(count);
    for (unsigned position = 0; position < count; ++position) {
        // A character literal keeps its case and its quotes: '0' and 'a' are what a reader
        // expects to see, and 'A' is another value.
        tree_t literal = type_enum_literal(base, position);
        const std::string_view text = istr(tree_ident(literal));
        definition.members.push_back(eir::EnumMember{
            .name = text.starts_with('\'') ? std::string(text) : simpleName(tree_ident(literal)),
            .value = encode(position, storage)});
    }
    return definition;
}

eir::BitVector encode(std::int64_t value, eir::Type type) {
    return type.isSigned()
               ? eir::BitVector::fromInt64(value, type.bitWidth())
               : eir::BitVector::fromUInt64(static_cast<std::uint64_t>(value), type.bitWidth());
}

namespace {

/// The bits of a static record value laid out as `layout`: an aggregate of its fields, by position
/// or by name, or a constant or a generic holding one.
std::optional<eir::BitVector> recordBits(tree_t expression, const Layout& layout,
                                         const Folder& folder) {
    switch (tree_kind(expression)) {
    case T_QUALIFIED:
    case T_TYPE_CONV: return recordBits(tree_value(expression), layout, folder);
    case T_REF: {
        if (!tree_has_ref(expression)) return std::nullopt;
        tree_t declaration = tree_ref(expression);
        if ((tree_kind(declaration) == T_CONST_DECL || tree_kind(declaration) == T_ALIAS)
            && tree_has_value(declaration))
            return recordBits(tree_value(declaration), layout, folder);
        if (const eir::BitVector* bits = folder.compositeOf(declaration)) return *bits;
        return std::nullopt;
    }
    case T_AGGREGATE: break;
    default: return std::nullopt;
    }

    const std::size_t count = layout.fields.size();
    std::vector<std::optional<eir::BitVector>> values(count);
    tree_t others = nullptr;
    std::size_t next = 0;
    for (unsigned index = 0; index < tree_assocs(expression); ++index) {
        tree_t association = tree_assoc(expression, index);
        std::optional<std::size_t> field;
        switch (tree_subkind(association)) {
        case A_POS: field = next++; break;
        case A_NAMED: {
            tree_t name = tree_name(association);
            if (tree_kind(name) != T_REF || !tree_has_ref(name)
                || tree_kind(tree_ref(name)) != T_FIELD_DECL)
                return std::nullopt;
            field = tree_pos(tree_ref(name));
            break;
        }
        case A_OTHERS: others = tree_value(association); break;
        default: return std::nullopt;
        }
        if (!field) continue;
        if (*field >= count) return std::nullopt;
        values[*field] = constantBits(tree_value(association), layout.fields[*field], folder);
        if (!values[*field]) return std::nullopt;
    }

    eir::BitVector bits = eir::BitVector::filled(layout.storage.bitWidth(), eir::Logic::Zero);
    for (std::size_t field = 0; field < count; ++field) {
        std::optional<eir::BitVector> value = values[field];
        if (!value && others != nullptr) value = constantBits(others, layout.fields[field], folder);
        if (!value) return std::nullopt;
        eir::bitops::assignPartAt(bits.mutableRef(), value->ref(), layout.fieldOffsets[field]);
    }
    return bits;
}

} // namespace

std::optional<eir::BitVector> constantBits(tree_t expression, const Layout& layout,
                                           const Folder& folder) {
    if (layout.isRecord()) return recordBits(expression, layout, folder);
    if (!layout.isArray()) {
        const std::optional<std::int64_t> value = folder.integer(expression);
        if (!value) return std::nullopt;
        return encode(*value, layout.storage);
    }
    const std::optional<Layout> element = elementLayout(layout, folder);
    if (!element) return std::nullopt;
    const std::optional<std::vector<eir::BitVector>> elements = elementsOf(
        expression,
        Elements{.type = layout.inner.empty() ? layout.elementType : nullptr, .layout = *element},
        layout.index.length(), folder);
    if (!elements) return std::nullopt;
    return pack(*elements, layout);
}

std::optional<Folder::Range> constantBounds(tree_t expression, type_t type, const Folder& folder) {
    if (std::optional<Folder::Range> bounds = folder.boundsOf(expression)) return bounds;
    if (!type_is_array(type) || dimension_of(type) != 1) return std::nullopt;
    type_t elementType = type_elem(type);
    const std::optional<eir::Type> storage = scalarStorage(elementType);
    if (!storage) return std::nullopt;
    const std::optional<std::vector<eir::BitVector>> elements =
        elementsOf(expression, Elements{.type = elementType, .layout = Layout{.storage = *storage}},
                   std::nullopt, folder);
    if (!elements) return std::nullopt;

    // From the left of the index subtype, the way it runs, as a concatenation's result is when
    // its left operand gives it none (IEEE 1076 section 9.2.5).
    const std::optional<Folder::Range> index = folder.rangeOf(index_type_of(type, 0));
    if (!index) return std::nullopt;
    const auto count = static_cast<std::int64_t>(elements->size());
    return Folder::Range{.left = index->left,
                         .right =
                             index->ascending ? index->left + count - 1 : index->left - count + 1,
                         .ascending = index->ascending};
}

std::optional<eir::BitVector> defaultBits(type_t type, const Layout& layout, const Folder& folder) {
    if (layout.isRecord()) {
        eir::BitVector bits = eir::BitVector::filled(layout.storage.bitWidth(), eir::Logic::Zero);
        for (std::size_t field = 0; field < layout.fields.size(); ++field) {
            type_t fieldType =
                tree_type(type_field(layout.recordType, static_cast<unsigned>(field)));
            const std::optional<eir::BitVector> value =
                defaultBits(fieldType, layout.fields[field], folder);
            if (!value) return std::nullopt;
            eir::bitops::assignPartAt(bits.mutableRef(), value->ref(), layout.fieldOffsets[field]);
        }
        return bits;
    }
    if (!layout.isArray()) {
        // An access starts `null`.
        if (type != nullptr && type_is_access(type)) return encode(0, layout.storage);
        const std::optional<std::int64_t> left = folder.left(type);
        if (!left) return std::nullopt;
        return encode(*left, layout.storage);
    }
    const std::optional<Layout> element = elementLayout(layout, folder);
    if (!element) return std::nullopt;
    const std::optional<eir::BitVector> one =
        element->isArray() ? defaultBits(nullptr, *element, folder)
                           : defaultBits(layout.elementType, *element, folder);
    if (!one) return std::nullopt;
    return pack(std::vector<eir::BitVector>(static_cast<std::size_t>(layout.index.length()), *one),
                layout);
}

} // namespace enzan::frontend::vhdl
