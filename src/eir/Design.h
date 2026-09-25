// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Module.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::eir {

/// A collection of analysed design units.
///
/// This is what analysis produces and what a library directory stores (DESIGN_GUIDE.md
/// section 4.4). It is independent of any particular top level: the same Design can be
/// elaborated against several top modules, and elaborating one does not modify it.
///
/// Lookup is by name, because an instance refers to its definition by name (see
/// `Instance::definitionName`). The index is an ordered map so that iterating the
/// design — to dump it, to serialise it, or to report an error about it — produces
/// the same order on every run (section 3.8).
class Design {
public:
    /// Adds a definition and returns its index.
    ///
    /// Adding a definition whose name is already present replaces it and returns the
    /// existing index. Replacement rather than rejection is deliberate: re-analysing
    /// a changed source file must update the library, and the analysis driver is the
    /// right place to decide whether a redefinition deserves a diagnostic.
    std::uint32_t addModule(ModuleDefinition module);

    /// Returns the definition called `name`, or nullptr.
    [[nodiscard]] const ModuleDefinition* findModule(std::string_view name) const;

    /// Returns the index of the definition called `name`, or nothing.
    [[nodiscard]] std::optional<std::uint32_t> findModuleIndex(std::string_view name) const;

    [[nodiscard]] const ModuleDefinition& module(std::uint32_t index) const;

    [[nodiscard]] std::span<const ModuleDefinition> modules() const noexcept { return definitions; }

    [[nodiscard]] std::size_t size() const noexcept { return definitions.size(); }

    [[nodiscard]] bool empty() const noexcept { return definitions.empty(); }

    /// Returns the definitions that nothing else in this design instantiates.
    ///
    /// These are the candidate top levels. Reported in name order so that a driver
    /// choosing a default top, or a diagnostic listing the choices, is reproducible.
    [[nodiscard]] std::vector<std::string> candidateTopModules() const;

private:
    std::vector<ModuleDefinition> definitions;

    /// Name to index. Ordered, not hashed: see the class comment.
    std::map<std::string, std::uint32_t, std::less<>> byName;
};

} // namespace enzan::eir
