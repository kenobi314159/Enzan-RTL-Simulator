// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Module.h"

#include "eir/BitOps.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <utility>

namespace enzan::eir {

namespace {

// The bytes of a plane's words are its elements' in order only on a little-endian machine.
static_assert(std::endian::native == std::endian::little, "VHDL codes are read as bytes");

/// A std_ulogic's code, a byte of its object's aval plane, as the two planes of the four-state bit
/// it shows as: bit 0 the aval and bit 1 the bval, which is a Logic's value. The positions of its
/// literals are U X 0 1 Z W L H -; a byte that is none of them is unknown.
constexpr std::array<std::uint8_t, 256> LogicOfCode = [] {
    std::array<std::uint8_t, 256> table{};
    for (auto& entry : table) entry = static_cast<std::uint8_t>(Logic::Unknown);
    table[2] = static_cast<std::uint8_t>(Logic::Zero);
    table[3] = static_cast<std::uint8_t>(Logic::One);
    table[4] = static_cast<std::uint8_t>(Logic::HighZ);
    table[6] = static_cast<std::uint8_t>(Logic::Zero);
    table[7] = static_cast<std::uint8_t>(Logic::One);
    return table;
}();

/// `present` of std_ulogic codes with no unknown bit among them, which is every VHDL value and
/// holds the codes in the bytes of its aval plane, as a loop over bytes rather than over bits:
/// what a vector operator of numeric_std costs is mostly this and its inverse.
void presentLogicCodes(ConstBitRef stored, BitRef shown) noexcept {
    const std::uint32_t elements = shown.width();
    const auto* codes = reinterpret_cast<const std::uint8_t*>(stored.aval());
    Word* aval = shown.aval();
    Word* bval = shown.bval();
    // A word of each plane at a time, held in registers while its 64 elements are looked up.
    for (std::uint32_t word = 0; word < shown.words(); ++word) {
        const std::uint32_t first = word * BitsPerWord;
        const std::uint32_t count = std::min(elements - first, BitsPerWord);
        Word known = 0;
        Word unknown = 0;
        for (std::uint32_t bit = 0; bit < count; ++bit) {
            const std::uint8_t logic = LogicOfCode[codes[first + bit]];
            known |= static_cast<Word>(logic & 1U) << bit;
            unknown |= static_cast<Word>(logic >> 1U) << bit;
        }
        aval[word] = known;
        bval[word] = unknown;
    }
}

/// `encode` of std_ulogic with `codes`, the codes of a 0, a 1, a high-impedance bit and an unknown
/// one in that order, into bytes of the aval plane of `stored`.
void encodeLogicCodes(ConstBitRef shown, BitRef stored,
                      const std::array<std::uint8_t, 4>& codes) noexcept {
    const std::uint32_t elements = shown.width();
    const Word* known = shown.aval();
    const Word* unknown = shown.bval();
    Word* aval = stored.aval();
    Word* bval = stored.bval();
    for (std::uint32_t word = 0; word < stored.words(); ++word) {
        aval[word] = 0;
        bval[word] = 0;
    }
    auto* bytes = reinterpret_cast<std::uint8_t*>(aval);
    for (std::uint32_t word = 0; word < shown.words(); ++word) {
        const std::uint32_t first = word * BitsPerWord;
        const std::uint32_t count = std::min(elements - first, BitsPerWord);
        const Word a = known[word];
        const Word b = unknown[word];
        for (std::uint32_t bit = 0; bit < count; ++bit) {
            const auto logic =
                static_cast<std::uint32_t>(((a >> bit) & 1U) | (((b >> bit) & 1U) << 1U));
            bytes[first + bit] = codes[logic];
        }
    }
}

} // namespace

std::uint32_t presentedWidth(Encoding encoding, std::uint32_t width) noexcept {
    return encoding == Encoding::Bits || encoding == Encoding::Real ? width : width / 8;
}

void present(Encoding encoding, ConstBitRef stored, BitRef shown) noexcept {
    if (encoding == Encoding::Bits || encoding == Encoding::Real) {
        bitops::copy(shown, stored);
        return;
    }
    if (encoding == Encoding::VhdlLogic && stored.isTwoState()) {
        presentLogicCodes(stored, shown);
        return;
    }
    // The positions of std_ulogic's literals: U X 0 1 Z W L H -.
    static constexpr Logic Logic9[] = {Logic::Unknown, Logic::Unknown, Logic::Zero,
                                       Logic::One,     Logic::HighZ,   Logic::Unknown,
                                       Logic::Zero,    Logic::One,     Logic::Unknown};
    for (std::uint32_t element = 0; element < shown.width(); ++element) {
        std::uint32_t code = 0;
        bool known = true;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            const Logic value = stored.get((element * 8) + bit);
            if (value == Logic::One) code |= 1U << bit;
            known = known && (value == Logic::Zero || value == Logic::One);
        }
        Logic shownBit = Logic::Unknown;
        if (known && encoding == Encoding::VhdlLogic && code < std::size(Logic9))
            shownBit = Logic9[code];
        if (known && encoding == Encoding::VhdlBit && code < 2)
            shownBit = code == 0 ? Logic::Zero : Logic::One;
        shown.set(element, shownBit);
    }
}

bool storeShown(Encoding encoding, ConstBitRef shown, BitRef stored) noexcept {
    for (std::uint32_t element = 0; element < shown.width(); ++element) {
        const Logic value = shown.get(element);
        if (encoding == Encoding::VhdlBit && value != Logic::Zero && value != Logic::One)
            return false;
    }
    encode(encoding, shown, stored);
    return true;
}

void encode(Encoding encoding, ConstBitRef shown, BitRef stored, std::uint8_t weak) noexcept {
    if (encoding == Encoding::Bits || encoding == Encoding::Real) {
        bitops::copy(stored, shown);
        return;
    }
    // The codes of a bit's 0 and 1, every other value its 0; or the positions of std_ulogic's
    // '0', '1', 'Z' and 'X', weak values its 'L', 'H' and 'W'.
    std::uint32_t zero = 0;
    std::uint32_t one = 1;
    std::uint32_t highZ = 0;
    std::uint32_t unknown = 0;
    if (encoding == Encoding::VhdlLogic) {
        const bool weakZero = (weak & WeakZero) != 0;
        const bool weakOne = (weak & WeakOne) != 0;
        zero = weakZero ? 6 : 2;
        one = weakOne ? 7 : 3;
        highZ = 4;
        unknown = weakZero && weakOne ? 5 : 1;
    }
    if (encoding == Encoding::VhdlLogic) {
        encodeLogicCodes(shown, stored,
                         {static_cast<std::uint8_t>(zero), static_cast<std::uint8_t>(one),
                          static_cast<std::uint8_t>(highZ), static_cast<std::uint8_t>(unknown)});
        return;
    }
    for (std::uint32_t element = 0; element < shown.width(); ++element) {
        std::uint32_t code = 0;
        switch (shown.get(element)) {
        case Logic::Zero: code = zero; break;
        case Logic::One: code = one; break;
        case Logic::HighZ: code = highZ; break;
        case Logic::Unknown: code = unknown; break;
        }
        for (std::uint32_t bit = 0; bit < 8; ++bit)
            stored.set((element * 8) + bit, ((code >> bit) & 1U) != 0 ? Logic::One : Logic::Zero);
    }
}

std::string_view toString(Encoding encoding) noexcept {
    switch (encoding) {
    case Encoding::Bits: return "four-state bits";
    case Encoding::VhdlLogic: return "std_ulogic codes";
    case Encoding::VhdlBit: return "bit or boolean codes";
    case Encoding::Real: return "a real";
    }
    return "bits";
}

std::string_view toString(ObjectKind kind) noexcept {
    switch (kind) {
    case ObjectKind::Net: return "net";
    case ObjectKind::Variable: return "var";
    }
    return "var";
}

std::string_view toString(NetKind kind) noexcept {
    switch (kind) {
    case NetKind::Wire: return "wire";
    case NetKind::Tri: return "tri";
    case NetKind::WiredAnd: return "wand";
    case NetKind::WiredOr: return "wor";
    case NetKind::TriAnd: return "triand";
    case NetKind::TriOr: return "trior";
    case NetKind::Tri0: return "tri0";
    case NetKind::Tri1: return "tri1";
    case NetKind::Supply0: return "supply0";
    case NetKind::Supply1: return "supply1";
    case NetKind::UWire: return "uwire";
    }
    return "wire";
}

Logic undrivenValue(NetKind kind) noexcept {
    switch (kind) {
    case NetKind::Tri0:
    case NetKind::Supply0: return Logic::Zero;
    case NetKind::Tri1:
    case NetKind::Supply1: return Logic::One;
    case NetKind::Wire:
    case NetKind::Tri:
    case NetKind::WiredAnd:
    case NetKind::WiredOr:
    case NetKind::TriAnd:
    case NetKind::TriOr:
    case NetKind::UWire: return Logic::HighZ;
    }
    return Logic::HighZ;
}

bool resolvesSingleDrivers(NetKind kind) noexcept {
    switch (kind) {
    case NetKind::Tri0:
    case NetKind::Tri1:
    case NetKind::Supply0:
    case NetKind::Supply1: return true;
    case NetKind::Wire:
    case NetKind::Tri:
    case NetKind::WiredAnd:
    case NetKind::WiredOr:
    case NetKind::TriAnd:
    case NetKind::TriOr:
    case NetKind::UWire: return false;
    }
    return false;
}

std::string_view toString(PortDirection direction) noexcept {
    switch (direction) {
    case PortDirection::Input: return "input";
    case PortDirection::Output: return "output";
    case PortDirection::InOut: return "inout";
    }
    return "input";
}

std::optional<std::uint32_t> ModuleDefinition::findObject(std::string_view wanted) const {
    const auto found = std::ranges::find_if(
        objects, [wanted](const Object& object) { return object.name == wanted; });
    if (found == objects.end()) return std::nullopt;

    return static_cast<std::uint32_t>(found - objects.begin());
}

std::optional<std::uint32_t> ModuleDefinition::findSubprogram(std::string_view wanted) const {
    const auto found = std::ranges::find_if(
        subprograms, [wanted](const Subprogram& subprogram) { return subprogram.name == wanted; });
    if (found == subprograms.end()) return std::nullopt;

    return static_cast<std::uint32_t>(found - subprograms.begin());
}

std::uint32_t ModuleDefinition::addObject(Object object) {
    const auto index = static_cast<std::uint32_t>(objects.size());
    objects.push_back(std::move(object));
    return index;
}

std::string_view EnumDefinition::nameOf(const BitVector& value) const {
    for (const EnumMember& member : members) {
        if (member.value == value) return member.name;
    }
    return {};
}

std::uint32_t ModuleDefinition::addEnum(EnumDefinition definition) {
    const auto same = [&definition](const EnumDefinition& existing) {
        if (existing.name != definition.name) return false;
        if (existing.members.size() != definition.members.size()) return false;

        return std::ranges::equal(existing.members, definition.members,
                                  [](const EnumMember& left, const EnumMember& right) {
                                      return left.name == right.name && left.value == right.value;
                                  });
    };

    const auto found = std::ranges::find_if(enums, same);
    if (found != enums.end()) return static_cast<std::uint32_t>(found - enums.begin());

    const auto index = static_cast<std::uint32_t>(enums.size());
    enums.push_back(std::move(definition));
    return index;
}

bool isIntroducedName(std::string_view objectName) noexcept {
    return objectName.find('$') != std::string_view::npos;
}

std::vector<ArrayDimension> declaredDimensions(const ModuleDefinition& definition,
                                               const Object& object) {
    if (object.holdsNothing) return {};

    std::vector<ArrayDimension> dimensions;
    if (object.vhdlType == VhdlType::None) {
        dimensions = object.unpackedDimensions;
        dimensions.insert(dimensions.end(), object.packedDimensions.begin(),
                          object.packedDimensions.end());
    } else {
        // An array of arrays is dimensions of the first and then of its element, as an array
        // of two dimensions is: both are indexed `a(i)(j)` or `a(i, j)` down to the same bits.
        for (std::uint32_t type = object.vhdlType;
             type < definition.vhdlTypes.size()
             && definition.vhdlTypes[type].kind == VhdlType::Kind::Array;
             type = definition.vhdlTypes[type].element) {
            for (const VhdlType::Range& range : definition.vhdlTypes[type].ranges) {
                const bool null =
                    range.ascending ? range.left > range.right : range.left < range.right;
                if (null || !std::in_range<std::int32_t>(range.left)
                    || !std::in_range<std::int32_t>(range.right))
                    return {};
                dimensions.push_back(
                    ArrayDimension{.left = static_cast<std::int32_t>(range.left),
                                   .right = static_cast<std::int32_t>(range.right)});
            }
        }
    }

    std::uint64_t elements = 1;
    for (const ArrayDimension& dimension : dimensions) {
        elements *= dimension.size();
        if (elements > object.type.bitWidth()) return {};
    }
    const std::uint32_t shown = presentedWidth(object.encoding, object.type.bitWidth());
    if (dimensions.empty() || shown == 0 || shown % elements != 0) return {};
    return dimensions;
}

} // namespace enzan::eir
