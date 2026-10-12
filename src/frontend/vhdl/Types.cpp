// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/Types.h"

#include <cctype>
#include <limits>
#include <string_view>

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

} // namespace

std::optional<eir::Type> storageOf(type_t type) {
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

    default: return std::nullopt;
    }
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

    const eir::Type storage = storageOf(base).value_or(eir::Type::bits(8, false, false));
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

} // namespace enzan::frontend::vhdl
