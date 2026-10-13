// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Module.h"

#include "eir/BitOps.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace enzan::eir {

std::uint32_t presentedWidth(Encoding encoding, std::uint32_t width) noexcept {
    return encoding == Encoding::Bits || encoding == Encoding::Real ? width : width / 8;
}

void present(Encoding encoding, ConstBitRef stored, BitRef shown) noexcept {
    if (encoding == Encoding::Bits || encoding == Encoding::Real) {
        bitops::copy(shown, stored);
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

} // namespace enzan::eir
