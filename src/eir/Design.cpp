// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Design.h"

#include "common/Assert.h"

#include <set>
#include <utility>

namespace enzan::eir {

std::uint32_t Design::addModule(ModuleDefinition module) {
    const auto existing = byName.find(module.name);
    if (existing != byName.end()) {
        definitions[existing->second] = std::move(module);
        return existing->second;
    }

    const auto index = static_cast<std::uint32_t>(definitions.size());
    byName.emplace(module.name, index);
    definitions.push_back(std::move(module));
    return index;
}

const ModuleDefinition* Design::findModule(std::string_view name) const {
    const auto found = byName.find(name);
    return found == byName.end() ? nullptr : &definitions[found->second];
}

std::optional<std::uint32_t> Design::findModuleIndex(std::string_view name) const {
    const auto found = byName.find(name);
    return found == byName.end() ? std::nullopt : std::optional(found->second);
}

const ModuleDefinition& Design::module(std::uint32_t index) const {
    ENZAN_ALWAYS_ASSERT(index < definitions.size(), "module index out of range");
    return definitions[index];
}

std::vector<std::string> Design::candidateTopModules() const {
    // An ordered set, so that the result is the same on every run even though it is
    // built by iterating instances (section 3.8).
    std::set<std::string, std::less<>> instantiated;
    for (const ModuleDefinition& definition : definitions) {
        for (const Instance& instance : definition.instances)
            instantiated.insert(instance.definitionName);
    }

    std::vector<std::string> result;
    for (const auto& [name, index] : byName) {
        if (!instantiated.contains(name)) result.push_back(name);
    }
    return result;
}

} // namespace enzan::eir
