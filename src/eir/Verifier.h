// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Design.h"

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::eir {

/// Checks that EIR is well formed, reporting every problem found.
///
/// **This is how the core invariant of DESIGN_GUIDE.md section 4.2 is enforced.**
/// A process must be expressible as a pure function over an explicit state arena
/// with a declared read set and write set; the verifier checks that every object an
/// instruction touches is declared, which is the part a frontend is most likely to
/// get quietly wrong. Code that lets a process reach arbitrary global state must be
/// rejected, and review is unreliable, so it is rejected here instead.
///
/// The verifier also checks the structural properties downstream components assume
/// and would otherwise read past the end of a vector discovering: block, register,
/// constant and object indices in range, one terminator at the end of every block,
/// operands matching each opcode's convention, and equal widths where an operation
/// requires them.
///
/// Every frontend's output SHOULD be verified before it is elaborated. The cost is
/// linear in the size of the design and is paid once per analysis, not per
/// simulation step, so there is no reason to skip it outside a measured hot path.
///
/// Returns true when no problem was found. Problems are reported as `ENZ9003`
/// diagnostics naming the module, process, block and instruction, because an EIR
/// defect is a defect in Enzan and the report has to be enough to find it.
bool verify(const Design& design, DiagnosticEngine& diagnostics);

/// Verifies one definition. `design` is used only to resolve instance references;
/// pass nullptr to skip cross-module checks, which is what a frontend unit test
/// wants when it builds a module in isolation.
bool verify(const ModuleDefinition& definition, const Design* design,
            DiagnosticEngine& diagnostics);

} // namespace enzan::eir
