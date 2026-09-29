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
///
/// **One name may be defined by more than one library.** A design assembled from
/// several libraries (8.2) keeps every such definition rather than letting the last
/// one loaded win, and an unqualified lookup of a name two libraries define finds
/// nothing and reports both. A caller that means a particular one says so by
/// qualifying the name as `<library>.<unit>`.
class Design {
public:
    /// Adds a definition and returns its index.
    ///
    /// A definition whose name *and library* are already present replaces that one and
    /// returns its index. Replacement rather than rejection is deliberate:
    /// re-analysing a changed source file must update the library, and the analysis
    /// driver is the right place to decide whether a redefinition deserves a
    /// diagnostic. The same name from a *different* library is a separate definition
    /// and is kept alongside.
    std::uint32_t addModule(ModuleDefinition module);

    /// Returns the definition called `name`, or nullptr.
    ///
    /// `name` may be qualified as `<library>.<unit>`, which selects that library's
    /// definition and nothing else.
    ///
    /// An unqualified name that more than one library defines is **not** resolved to
    /// whichever came first. This returns nullptr and, when `ambiguity` is given,
    /// fills it with the libraries that define the name, in the order they were added
    /// — which is the order 8.2 says libraries are searched, and is what a diagnostic
    /// lists. Silently choosing one is the failure mode section 1.2 ranks worst.
    [[nodiscard]] const ModuleDefinition*
    findModule(std::string_view name, std::vector<std::string_view>* ambiguity = nullptr) const;

    /// Returns the index of the definition called `name`, or nothing. See `findModule`.
    [[nodiscard]] std::optional<std::uint32_t>
    findModuleIndex(std::string_view name,
                    std::vector<std::string_view>* ambiguity = nullptr) const;

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

    /// Name to the indices defining it, one per library, in the order they were added.
    /// Ordered by name, not hashed: see the class comment.
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> byName;
};

} // namespace enzan::eir
