// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Module.h"

#include <algorithm>
#include <utility>

namespace enzan::eir {

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
