// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Design.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::elab {

/// Makes the concrete units a top level needs out of a design that may hold generic ones
/// (DESIGN_GUIDE.md section 18, M5.1).
///
/// A generic unit is specialised once for each distinct set of values its instances give it,
/// and named for them as section 8.2 names a specialisation; every annotated number becomes
/// its expression's value. A unit that is not generic is taken as it is. Either way its
/// instances are renamed to the specialisations they instantiate, so the result holds nothing
/// generic and nothing downstream has to know that generic units exist.
///
/// Only units reachable from the top are in the result, so a library of a hundred modules
/// costs elaboration the ones a design uses.
class Specialiser {
public:
    /// `analysed` and `diagnostics` must outlive the specialiser, and `functions`, which runs
    /// the constant functions parameters call, too when given.
    Specialiser(const eir::Design& analysed, DiagnosticEngine& diagnostics,
                eir::ConstantFunctions* functions = nullptr) noexcept;

    /// The units under `top`, an index into the analysed design, with the top's own
    /// specialisation at `units->module(top)`; or nothing, having reported why.
    struct Result {
        std::unique_ptr<eir::Design> units;
        std::uint32_t top = 0;
    };

    [[nodiscard]] std::optional<Result> specialise(std::uint32_t top);

    /// What an interface port of a unit is connected to: the specialisation of the interface,
    /// and the values of its parameters, which the unit may read as its own.
    struct Interface {
        std::string unit;
        std::map<std::string, eir::ParameterValue, std::less<>> parameters;
        /// For a port taking an array, how many elements the array holds; 0 for one interface.
        std::uint32_t elements = 0;
    };

private:
    /// Specialises definition `index` for an instance at `instancePath`, given the actuals it
    /// sets (already evaluated, by formal name) and the interfaces its ports reach. Returns the
    /// specialisation's name, or nothing having reported why.
    std::optional<std::string>
    unitFor(std::uint32_t index, const std::string& instancePath,
            const std::map<std::string, eir::ParameterValue, std::less<>>& actuals,
            const std::map<std::string, Interface, std::less<>>& interfaces);

    /// Makes every connection of instance `instanceIndex` of `unit` whose two sides differ in
    /// width an assignment through an object of the port's width. False, having reported it,
    /// for an `inout`, which no one-way assignment can join.
    ///
    /// Only for a connection a generic unit is on either side of: anywhere else a frontend saw
    /// both widths and converted them itself, and a mismatch is its defect, which elaboration
    /// reports rather than hides.
    bool adaptPorts(eir::ModuleDefinition& unit, std::size_t instanceIndex);

    const eir::Design& analysed;
    DiagnosticEngine& diagnostics;
    eir::ConstantFunctions* functions;
    std::unique_ptr<eir::Design> units;
    /// The parameter values of each specialisation made so far, by name, for an interface
    /// whose parameters a unit it is connected to reads.
    std::map<std::string, std::map<std::string, eir::ParameterValue, std::less<>>, std::less<>>
        parametersOf;
    /// Names being specialised, outermost first, to report a hierarchy that contains itself.
    std::vector<std::string> inProgress;
};

} // namespace enzan::elab
