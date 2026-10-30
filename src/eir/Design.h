// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Module.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::eir {

/// A collection of analysed design units.
///
/// This is what analysis produces and what a library directory stores. It is
/// independent of any particular top level: the same Design can be elaborated against
/// several top modules, and elaborating one does not modify it.
///
/// Lookup is by name, because an instance refers to its definition by name (see
/// `Instance::definitionName`). The index is an ordered map so that iterating the
/// design — to dump it, to serialise it, or to report an error about it — produces
/// the same order on every run.
///
/// **One name may be defined by more than one library.** A design assembled from
/// several libraries keeps every such definition rather than letting the last one
/// loaded win, and an unqualified lookup of a name two libraries define finds nothing
/// and reports both. A caller that means a particular one says so by qualifying the
/// name as `<library>.<unit>`. Copies that are the same unit are not two definitions,
/// though (`mergeIdenticalDefinitions`).
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
    /// — which is the order libraries are searched in, and is what a diagnostic lists.
    /// Silently choosing one would be a wrong result with nothing to say so. Copies
    /// `mergeIdenticalDefinitions` found to be one unit are one definition, the first.
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

    /// Returns the indices of every specialisation whose *source* name is `sourceName`.
    ///
    /// A parameterised module becomes one definition per parameterisation, named for its
    /// parameter values (see `ModuleDefinition::name`), so a user naming a top level writes the
    /// name they wrote in the source and not the one analysis derived. More than one means the
    /// choice is not forced and has to be reported rather than guessed.
    ///
    /// In the order the definitions were added, so a diagnostic listing them is reproducible.
    /// A copy `mergeIdenticalDefinitions` found to be the same unit as an earlier one is left
    /// out, since it is not a second choice.
    [[nodiscard]] std::vector<std::uint32_t> findSpecialisations(std::string_view sourceName) const;

    /// Makes same-named definitions from different libraries one definition where
    /// `contentOf` gives them equal bytes.
    ///
    /// Two libraries that each needed a parameterisation of one module both store it, because
    /// each is analysed against its inputs alone; and a session that analyses a design against
    /// a library reproduces the units it reads from that library's sources. Such copies are
    /// one unit, and refusing them as two would make libraries that share an IP unusable
    /// together. A copy that differs in anything is a different unit, and the name stays
    /// ambiguous.
    ///
    /// The first copy added stands for the others. Each copy is kept, so a qualified name
    /// still finds its own library's. Recomputed from nothing on every call, and forgotten for
    /// a name `addModule` replaces a definition of, since the replacement may differ.
    void
    mergeIdenticalDefinitions(const std::function<std::string(const ModuleDefinition&)>& contentOf);

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

    /// Definitions that are the same unit as an earlier one of their name.
    std::set<std::uint32_t> duplicates;
};

} // namespace enzan::eir
