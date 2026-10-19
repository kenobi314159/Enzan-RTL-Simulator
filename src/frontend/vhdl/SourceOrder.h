// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan {
class SourceManager;
} // namespace enzan

namespace enzan::frontend::vhdl {

/// `sources` in an order that analyses each one after those declaring the units of the work
/// library it names, keeping the order given wherever nothing requires another, so that a list
/// already in an order VHDL can analyse stays as it is.
///
/// A unit is analysed against the units already analysed (IEEE 1076 section 13.1), so a package
/// has to come before what uses it, an entity before its architectures and a package before its
/// body; and a file list need not say them in that order, as GHDL's `-i` and `-m` and Questa's
/// `-autoorder` let it not. Each source is scanned, not analysed, for the primary units it
/// declares, `entity`, `package`, `configuration` and `context`, and for those it names: a
/// `work.x`, or the work library's own name `workLibrary`, in a use clause, an instantiation or a
/// selected name; and the entity of an architecture or a configuration, the package of a body.
/// Sources that name each other's units, which no order of whole files can satisfy, stay in the
/// order given.
[[nodiscard]] std::vector<BufferId> dependencyOrder(std::span<const BufferId> sources,
                                                    const SourceManager& manager,
                                                    std::string_view workLibrary);

/// The primary units `text` declares, as `dependencyOrder` scans for them, in lower case.
[[nodiscard]] std::vector<std::string> primaryUnits(std::string_view text);

} // namespace enzan::frontend::vhdl
