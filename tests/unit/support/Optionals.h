// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only helper for reading a std::optional that the test requires to hold a value.
//
// The obvious spelling is ASSERT_TRUE(maybe.has_value()) followed by *maybe, and it is correct:
// the assertion returns from the test, so the dereference is never reached without a value. It is
// not, however, provable by clang-tidy — bugprone-unchecked-optional-access understands an `if`
// but not a GoogleTest assertion, so every such pair is reported as unchecked access.
//
// Rather than suppress a check that catches a real mistake elsewhere, or contort each test into an
// `if`, the check happens once, here, in a form the analysis can follow: value_or never accesses
// an empty optional, so there is nothing left to prove.

#ifndef ENZAN_TESTS_UNIT_SUPPORT_OPTIONALS_H
#define ENZAN_TESTS_UNIT_SUPPORT_OPTIONALS_H

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace enzan::test {

/// Returns the value `maybe` holds, failing the test and yielding a default-constructed value when
/// it is empty.
///
/// `what` names what was being looked up, so that the failure says which lookup came back empty
/// rather than only that one did. The test continues afterwards — deliberately, so that a lookup
/// failing does not hide whatever else the test would have reported — which is why a
/// default-constructed stand-in is returned rather than nothing.
template<typename T>
T require(const std::optional<T>& maybe, std::string_view what = "value") {
    EXPECT_TRUE(maybe.has_value()) << "expected a " << what << ", but the optional was empty";
    return maybe.value_or(T{});
}

} // namespace enzan::test

#endif // ENZAN_TESTS_UNIT_SUPPORT_OPTIONALS_H
