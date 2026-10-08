// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/ParameterExpression.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::session {

/// Runs the constant functions a generic unit's parameters call, for elaboration
/// (DESIGN_GUIDE.md section 18, M5.1): a function is a unit of the design
/// (`eir::ModuleDefinition::isFunction`), and running it is simulating that unit on its own,
/// with its arguments as its inputs' initial values, until nothing is left to do. The VM that
/// runs a design runs the function, so a function means what its statements mean and nothing
/// here evaluates a statement.
///
/// Before a function is trusted with arguments slang never saw, it is run with the ones it did
/// (the unit's initial values), and a result that differs from slang's is reported and fails
/// every call of it: a translation that is wrong for one call is wrong for the widths derived
/// from any.
class ConstantFunctionRunner final : public eir::ConstantFunctions {
public:
    /// `design` holds the functions; it, `sources` and `diagnostics` must outlive the runner.
    ConstantFunctionRunner(const eir::Design& design, const SourceManager& sources,
                           DiagnosticEngine& diagnostics) noexcept;

    std::optional<eir::BitVector> call(std::string_view function,
                                       const std::optional<eir::BitVector>& arguments,
                                       std::uint32_t width) override;

private:
    /// What `unit` computes from its inputs' initial values, or nothing when it does not run to
    /// the end.
    std::optional<eir::BitVector> run(const eir::ModuleDefinition& unit) const;

    /// True when `unit` reproduces the call slang evaluated, reporting it once when it does not.
    bool trusted(const eir::ModuleDefinition& unit);

    const eir::Design* design;
    const SourceManager* sources;
    DiagnosticEngine* diagnostics;

    /// What each function returned for each set of arguments, so that a function the hierarchy
    /// calls with the same arguments a hundred times runs once.
    std::map<std::pair<std::string, std::string>, eir::BitVector, std::less<>> results;
    /// Functions checked against slang, and whether each passed.
    std::map<std::string, bool, std::less<>> checked;
};

} // namespace enzan::session
