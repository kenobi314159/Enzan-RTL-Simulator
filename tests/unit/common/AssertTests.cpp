// SPDX-License-Identifier: GPL-3.0-or-later
//
// The assertion macros are tested through death tests: what matters about them is
// that a violated invariant terminates the process and says enough for the
// failure to be diagnosed from a bug report alone.

#include "common/Assert.h"

#include "support/DeathTests.h"

#include <gtest/gtest.h>

namespace {

TEST(Assert, AlwaysAssertPassesWhenTheConditionHolds) {
    int calls = 0;
    const auto condition = [&calls] {
        ++calls;
        return true;
    };

    ENZAN_ALWAYS_ASSERT(condition(), "must not fire");
    EXPECT_EQ(calls, 1);
}

TEST(Assert, AlwaysAssertAbortsAndExplainsItself) {
    ENZAN_EXPECT_DEATH(ENZAN_ALWAYS_ASSERT(1 == 2, "one is not two"), "internal assertion failed");
}

TEST(Assert, AlwaysAssertReportsTheConditionAndTheReason) {
    // Both halves matter: the condition text says what was checked, the message
    // says why it was expected to hold.
    ENZAN_EXPECT_DEATH(ENZAN_ALWAYS_ASSERT(1 == 2, "one is not two"), "1 == 2");
    ENZAN_EXPECT_DEATH(ENZAN_ALWAYS_ASSERT(1 == 2, "one is not two"), "one is not two");
}

TEST(Assert, AlwaysAssertSaysTheDefectIsInEnzan) {
    // A user whose design triggers an internal assertion must not be left
    // wondering whether their HDL is at fault.
    ENZAN_EXPECT_DEATH(ENZAN_ALWAYS_ASSERT(false, "why"), "defect in Enzan");
}

TEST(Assert, UnreachableAborts) {
    ENZAN_EXPECT_DEATH(ENZAN_UNREACHABLE("this enumerator is handled above"),
                       "unreachable code reached");
}

#ifdef ENZAN_ENABLE_ASSERTIONS

TEST(Assert, CheckedAssertFiresWhenAssertionsAreEnabled) {
    ENZAN_EXPECT_DEATH(ENZAN_ASSERT(false, "checked"), "internal assertion failed");
}

TEST(Assert, CheckedAssertPassesWhenTheConditionHolds) {
    ENZAN_ASSERT(true, "must not fire");
    SUCCEED();
}

#else

TEST(Assert, CheckedAssertIsCompiledOutWhenAssertionsAreDisabled) {
    // The condition must not even be evaluated, or a release build would pay for
    // checks it does not perform.
    bool evaluated = false;
    ENZAN_ASSERT((evaluated = true), "compiled out");
    EXPECT_FALSE(evaluated);
}

#endif

} // namespace
