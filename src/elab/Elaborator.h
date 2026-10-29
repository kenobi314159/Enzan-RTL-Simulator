// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Design.h"
#include "elab/ElaboratedDesign.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::elab {

/// Where each translated VHDL resolution function is, by its name and its number of drivers: the
/// definition and the subprogram.
using ResolutionAnswers =
    std::map<std::pair<std::string, std::uint32_t>, std::pair<std::string, std::string>>;

/// Builds an executable image's structure from analysed design units.
///
/// Elaboration takes a named top level plus the analysed library, expands the
/// instance tree, binds ports and computes the flat state layout. It is inherently
/// per-design and cannot be cached across top levels or parameterisations, which is
/// why it is a separate phase from analysis rather than something a frontend does.
///
/// What this phase deliberately does **not** do: evaluate parameters or expand
/// generate constructs. Both happen in the frontend, which emits one EIR definition
/// per distinct parameterisation, so that an analysed unit is reusable without
/// re-evaluating anything.
class Elaborator {
public:
    /// `design` and `diagnostics` must outlive the elaborator.
    Elaborator(const eir::Design& analysedDesign, DiagnosticEngine& engine) noexcept;

    /// Elaborates `topModuleName`, or returns nullptr having reported why.
    ///
    /// Passing an empty name asks the elaborator to choose: it succeeds when the
    /// design has exactly one module nothing else instantiates, and otherwise reports
    /// the candidates rather than picking one. Guessing a top level is the kind of
    /// convenience that costs an hour of confusion the first time it guesses wrong.
    ///
    /// `resolutions` says where each VHDL resolution function already translated is, by its name
    /// and its number of drivers; a segment resolving with one it does not have is left with an
    /// unanswered call for the caller to have translated and to elaborate again with.
    std::unique_ptr<ElaboratedDesign> elaborate(std::string_view topModuleName,
                                                const ResolutionAnswers& resolutions = {});

private:
    class Builder;

    const eir::Design& design;
    DiagnosticEngine& diagnostics;
};

} // namespace enzan::elab
