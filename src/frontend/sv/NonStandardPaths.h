// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <set>
#include <span>

#include "common/Diagnostics.h"
#include "slang/text/SourceLocation.h"

namespace slang {
class Diagnostic;
} // namespace slang

namespace slang::syntax {
class SyntaxTree;
} // namespace slang::syntax

namespace enzan::frontend::sv {

/// The two module paths IEEE 1800 refuses that models written for other simulators rely on,
/// for the errors that are excused in them.
///
/// A parameter in a path's condition, where section 30.4.4 allows only specparams among the
/// constants, is a constant as a specparam is. A parallel path from one bit to a vector, which
/// section 30.4.5 allows only between terminals of one width, can only join that bit to every
/// bit of the output, as a full path does. Yosys's models of AMD's cells write both, and
/// Altera's the second; Icarus Verilog accepts the first and refuses the second. Enzan accepts
/// them as warnings, and translates them so. A parallel path between vectors of different
/// widths stays an error, since which bits it pairs is not plain.
///
/// Both are found from the diagnostics and the sources' syntax rather than the elaborated
/// design, since slang checks every module, those no instance names included.
class NonStandardPaths {
public:
    /// Records where `trees` declare parameters, and which of `diagnostics` name one in a
    /// path's condition.
    NonStandardPaths(std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees,
                     std::span<const slang::Diagnostic> diagnostics);

    /// The warning `diagnostic` is reported as, when it is one of the errors excused.
    [[nodiscard]] std::optional<DiagCode> excuses(const slang::Diagnostic& diagnostic) const;

private:
    [[nodiscard]] bool namesParameter(const slang::Diagnostic& diagnostic) const;

    std::set<slang::SourceLocation> parameters;
    std::set<slang::SourceLocation> excusedConditions;
};

} // namespace enzan::frontend::sv
