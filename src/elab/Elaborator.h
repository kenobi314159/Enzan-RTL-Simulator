// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Design.h"
#include "elab/ElaboratedDesign.h"

#include <memory>
#include <string_view>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::elab {

/// Builds an executable image's structure from analysed design units.
///
/// Elaboration takes a named top level plus the analysed library, expands the
/// instance tree, binds ports and computes the flat state layout (DESIGN_GUIDE.md
/// section 2.1). It is inherently per-design and cannot be cached across top levels
/// or parameterisations, which is why it is a separate phase from analysis rather
/// than something a frontend does.
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
    std::unique_ptr<ElaboratedDesign> elaborate(std::string_view topModuleName);

private:
    class Builder;

    const eir::Design& design;
    DiagnosticEngine& diagnostics;
};

} // namespace enzan::elab
