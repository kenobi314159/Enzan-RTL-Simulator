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
