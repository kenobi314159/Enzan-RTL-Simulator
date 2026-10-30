// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/UnitRequest.h"

#include <span>
#include <string>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::eir {
class Design;
}

namespace enzan::elab {

/// Links each instance of `design` naming one of `requests`' placeholders as its definition to the
/// definition `answers` names for that request, in the same order (`eir::UnitRequest`).
///
/// A connection's port index, an index into the request's ports, becomes the index of the
/// definition's port of that name: the one of exactly that name, or else the one of it in another
/// case. A port the definition does not have, or has in another direction than the instance
/// connects it in, is refused by name (ENZ3017, ENZ3018), as a component port with no entity port
/// to be bound to is.
///
/// Changes the design in place, and returns false having reported.
bool linkRequests(eir::Design& design, std::span<const eir::UnitRequest> requests,
                  std::span<const std::string> answers, DiagnosticEngine& diagnostics);

} // namespace enzan::elab
