// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only helper for death tests that keeps a deliberate abort out of the system crash log.
//
// A death test works by killing a real child process, so every EXPECT_DEATH leaves a real SIGABRT
// behind and systemd-coredump records it like any other crash. The assertion suite alone files six
// per run per build configuration, which buries the one crash that mattered under dozens that were
// the whole point of the test.
//
// Marking the child non-dumpable is narrower than capping RLIMIT_CORE for the test binary as a
// whole: a test that crashes for real — anywhere outside the statement under test — still dumps
// core and can still be debugged afterwards.

#ifndef ENZAN_TESTS_UNIT_SUPPORT_DEATHTESTS_H
#define ENZAN_TESTS_UNIT_SUPPORT_DEATHTESTS_H

#include <gtest/gtest.h>

#include <sys/prctl.h>

namespace enzan::test {

/// Marks the calling process non-dumpable, so that an abort immediately afterwards is not recorded
/// as a crash by the system's core dump handler.
///
/// Belongs in the death-test child and never in the parent: the flag is per-process, so it must be
/// set after the fork that EXPECT_DEATH performs, and it must not outlive the child that dies. The
/// result is deliberately ignored — a kernel too old to know the option leaves the dump enabled,
/// which is the behaviour this suite had before and is no reason to fail a test.
inline void suppressCoreDump() {
    static_cast<void>(::prctl(PR_SET_DUMPABLE, 0));
}

} // namespace enzan::test

/// EXPECT_DEATH, without leaving a core dump behind. `statement` and `matcher` carry exactly the
/// meanings they have there, and a statement that fails to die still fails the test.
///
/// Preferred over EXPECT_DEATH throughout the suite. Calling suppressCoreDump() by hand at each
/// site would work as well and would be forgotten by the next death test written, which is the
/// failure this macro exists to make impossible.
#define ENZAN_EXPECT_DEATH(statement, matcher) \
    EXPECT_DEATH(                              \
        {                                      \
            ::enzan::test::suppressCoreDump(); \
            statement;                         \
        },                                     \
        matcher)

#endif // ENZAN_TESTS_UNIT_SUPPORT_DEATHTESTS_H
