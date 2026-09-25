// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only helpers for turning Verilog-style literals into BitVectors.
//
// BitVector::parse returns an optional, because a literal read from a design may be malformed.
// A literal written into a test is different: it is part of the fixture, and if it does not parse
// the test is broken rather than the code under test. Dereferencing the optional directly says
// that, but says it as undefined behaviour — so every test that needed a literal grew its own
// copy of the same checked wrapper. This is that wrapper, written once.

#ifndef ENZAN_TESTS_UNIT_SUPPORT_LITERALS_H
#define ENZAN_TESTS_UNIT_SUPPORT_LITERALS_H

#include "eir/BitVector.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace enzan::test {

/// Parses `text` as a Verilog-style literal, for example "8'hA5" or "4'b10xz".
///
/// A literal that does not parse fails the test and yields a one-bit X, so that the failure is
/// reported where it happened instead of propagating as a confusing assertion later.
inline eir::BitVector literal(std::string_view text) {
    const std::optional<eir::BitVector> parsed = eir::BitVector::parse(text);
    EXPECT_TRUE(parsed.has_value()) << "failed to parse the literal '" << text << '\'';
    return parsed.value_or(eir::BitVector::filled(1, eir::Logic::Unknown));
}

} // namespace enzan::test

#endif // ENZAN_TESTS_UNIT_SUPPORT_LITERALS_H
