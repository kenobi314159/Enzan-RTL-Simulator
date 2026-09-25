// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"

#include <cstdint>
#include <string_view>

namespace slang {
class SVInt;
}

namespace enzan::frontend::sv {

/// Converts a slang integer into an EIR bit vector.
///
/// **The two encodings differ for unknown bits, and the difference is not obvious.** Both
/// are two planes of the same shape, but they disagree on what the first plane means where
/// the second is set:
///
/// | value | slang (value, unknown) | Enzan (aval, bval) |
/// |---|---|---|
/// | `0` | 0, 0 | 0, 0 |
/// | `1` | 1, 0 | 1, 0 |
/// | `X` | 0, 1 | 1, 1 |
/// | `Z` | 1, 1 | 0, 1 |
///
/// Copying the planes straight across therefore turns every X into a Z and every Z into an X,
/// which makes `===` against an `x` literal fail while `%b` still prints something plausible —
/// a defect that survives casual testing and is caught only by comparing against another
/// simulator.
///
/// The implementation asks slang for each bit rather than reading its storage, so it does not
/// depend on that layout at all; the table is kept here because it is what makes the bug
/// recognisable if anyone is tempted to write the fast version.
eir::BitVector toBitVector(const slang::SVInt& value);

/// Packs a string literal into a bit vector, most significant byte first, which is how
/// SystemVerilog stores a string in a vector.
eir::BitVector toBitVector(std::string_view text);

} // namespace enzan::frontend::sv
