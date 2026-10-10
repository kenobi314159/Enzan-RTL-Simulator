// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Design.h"

#include "common/Assert.h"

#include <algorithm>
#include <set>
#include <utility>

namespace enzan::eir {

namespace {

/// Splits `<library>.<unit>` into its parts. The library is empty when unqualified.
///
/// On the first dot, not the last: a mangled parameterisation is `leaf$1` and carries
/// no dot of its own, so the first is the qualifier's.
struct QualifiedName {
    std::string_view library;
    std::string_view unit;
};

QualifiedName splitQualified(std::string_view name) {
    const std::size_t dot = name.find('.');
    if (dot == std::string_view::npos) return {.library = {}, .unit = name};
    return {.library = name.substr(0, dot), .unit = name.substr(dot + 1)};
}

} // namespace

std::uint32_t Design::addModule(ModuleDefinition module) {
    std::vector<std::uint32_t>& indices = byName[module.name];
    for (const std::uint32_t existing : indices) duplicates.erase(existing);

    // Replacing the definition from the *same* library, because re-analysing a changed
    // source must update what is stored. A same-named unit from another library is a
    // different definition and is kept beside this one.
    for (const std::uint32_t existing : indices) {
        if (definitions[existing].library != module.library) continue;
        definitions[existing] = std::move(module);
        return existing;
    }

    const auto index = static_cast<std::uint32_t>(definitions.size());
    indices.push_back(index);
    definitions.push_back(std::move(module));
    return index;
}

const ModuleDefinition* Design::findModule(std::string_view name,
                                           std::vector<std::string_view>* ambiguity) const {
    const std::optional<std::uint32_t> index = findModuleIndex(name, ambiguity);
    return index.has_value() ? &definitions[*index] : nullptr;
}

std::optional<std::uint32_t>
Design::findModuleIndex(std::string_view name, std::vector<std::string_view>* ambiguity) const {
    const QualifiedName wanted = splitQualified(name);

    const auto found = byName.find(wanted.unit);
    if (found == byName.end()) return std::nullopt;

    if (!wanted.library.empty()) {
        for (const std::uint32_t index : found->second) {
            if (definitions[index].library == wanted.library) return index;
        }
        return std::nullopt;
    }

    const auto distinct = std::ranges::count_if(
        found->second, [this](std::uint32_t index) { return !duplicates.contains(index); });
    if (distinct == 1) return found->second.front();

    // More than one library defines it, so there is no answer to an unqualified
    // question. Reported rather than resolved: see the header.
    if (ambiguity != nullptr) {
        ambiguity->clear();
        for (const std::uint32_t index : found->second)
            ambiguity->push_back(definitions[index].library);
    }
    return std::nullopt;
}

const ModuleDefinition& Design::module(std::uint32_t index) const {
    ENZAN_ALWAYS_ASSERT(index < definitions.size(), "module index out of range");
    return definitions[index];
}

std::vector<std::uint32_t> Design::findSpecialisations(std::string_view sourceName) const {
    std::vector<std::uint32_t> found;
    for (std::uint32_t index = 0; index < definitions.size(); ++index) {
        if (definitions[index].sourceName == sourceName && !definitions[index].isPackage
            && !duplicates.contains(index))
            found.push_back(index);
    }
    return found;
}

void Design::mergeIdenticalDefinitions(
    const std::function<std::string(const ModuleDefinition&)>& contentOf) {
    duplicates.clear();
    for (const auto& [name, indices] : byName) {
        if (indices.size() < 2) continue;

        // Compared with every distinct copy before it, not only the first: a copy may differ
        // from the first library's and still agree with the second's.
        std::vector<std::pair<std::uint32_t, std::string>> distinct;
        for (const std::uint32_t index : indices) {
            std::string content = contentOf(definitions[index]);
            const bool seen = std::ranges::any_of(
                distinct, [&](const auto& earlier) { return earlier.second == content; });
            if (seen) {
                duplicates.insert(index);
            } else {
                distinct.emplace_back(index, std::move(content));
            }
        }
    }
}

std::vector<std::string> Design::candidateTopModules() const {
    // An ordered set, so that the result is the same on every run even though it is
    // built by iterating instances (section 3.8).
    std::set<std::string, std::less<>> instantiated;
    for (const ModuleDefinition& definition : definitions) {
        for (const Instance& instance : definition.instances)
            instantiated.insert(instance.definitionName);
    }

    // A package is never instantiated and never a top level either.
    std::vector<std::string> result;
    for (const auto& [name, indices] : byName) {
        if (!instantiated.contains(name) && !definitions[indices.front()].isPackage)
            result.push_back(name);
    }
    return result;
}

} // namespace enzan::eir
