// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/Module.h"
#include "frontend/vhdl/NvcApi.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::frontend::vhdl {

/// Maps one of NVC's locations onto Enzan's buffer holding the same source, or an invalid location
/// when the source is not one Enzan read: a unit of `ieee`, say.
using Locate = std::function<SourceLocation(const loc_t*)>;

/// What the adapter translates, and from where.
struct DesignRequest {
    /// The entity the design is translated from, in any case. Empty means every entity of `work`
    /// that no architecture analysed in this run instantiates.
    std::string top;

    /// The unit EIR's delays count, as a power of ten of a second. VHDL's own values of `time`
    /// count femtoseconds, NVC's and the standard's resolution limit, and are divided down.
    std::int8_t timeUnitExponent = -15;

    /// The work library, and the units this run analysed into it in the order it analysed them.
    /// An architecture analysed here is preferred to one an earlier run left in the library,
    /// whatever their timestamps, since it is the one the user just wrote.
    lib_t work = nullptr;
    std::vector<tree_t> analysed;
};

/// Translates the design under `request.top` from NVC's analysed units into EIR (DESIGN_GUIDE.md
/// section 5.2).
///
/// One definition per architecture and set of generic values: generics are folded here, as
/// SystemVerilog's parameters are folded by its frontend, so EIR never holds one (section 4.4).
/// What is translated is what the hierarchy under the top reaches, not every unit analysed, so a
/// unit nothing instantiates cannot stop a design for using something the adapter lacks.
///
/// Reports what it cannot translate to `diagnostics` and returns what it could. A caller checks
/// the error count: a design with a construct left out simulates and gives a wrong answer, which
/// section 1.2 ranks worst, so nothing partial may be run.
std::vector<eir::ModuleDefinition>
translateDesign(const DesignRequest& request, const Locate& locate, DiagnosticEngine& diagnostics);

} // namespace enzan::frontend::vhdl
