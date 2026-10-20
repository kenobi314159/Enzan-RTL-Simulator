// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::eir {
class Design;
}

namespace enzan::elab {

/// Connects each port whose two sides are of different languages through processes that convert
/// between them (DESIGN_GUIDE.md section 3.5), in the definition instantiating it.
///
/// A port crossing from one language into the other is not joined to its actual as a port of one
/// language is, since the two hold their values differently: a std_ulogic is a byte of code and a
/// SystemVerilog bit is a bit of each plane (section 3.6). Each such connection gets an object of
/// the port's own representation, `<instance>$<port>$boundary`, which the port is joined to, and
/// a conversion each way its mode needs, every one costing exactly one delta cycle: into VHDL by a
/// transaction, as a VHDL signal assignment is, and into SystemVerilog through the non-blocking
/// region, then a continuous assignment, so that a net it drives still resolves. An inout port
/// drives the outer signal with what its own drivers make of it and reads the outer signal back
/// as its effective value (section 18, M7), never one side's value as the other's driver, which
/// would hold a value driven once for ever.
///
/// What converts is what the VHDL side's type is made of: std_ulogic codes to four-state bits,
/// `bit` and `boolean` codes to two-state ones, integers as they are, element by element and
/// field by field, the left element and the first field highest on both sides. Anything else, a
/// real, another enumeration, a physical type or a shape the two sides do not share, is refused
/// by name (ENZ3016).
///
/// Runs on the design about to be elaborated, in place, and again on the same design changes
/// nothing: a connection already made through a boundary object is left alone. A library stores
/// what analysis produced, never this. Returns false having reported.
bool bridgeLanguages(eir::Design& design, DiagnosticEngine& diagnostics);

} // namespace enzan::elab
