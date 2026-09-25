// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace enzan::detail {

/// Reports a failed assertion on the standard error stream and aborts.
///
/// Not intended to be called directly; use the ENZAN_ASSERT macros. It is
/// declared `[[noreturn]]` so that code after a failing assertion is understood
/// to be unreachable by both the compiler and the reader.
[[noreturn]] void assertionFailed(const char* expression, const char* message, const char* file,
                                  unsigned line, const char* function);

} // namespace enzan::detail

/// Asserts an internal invariant, unconditionally and in every build.
///
/// Reserved for invariants whose violation means the process is already in an
/// unrecoverable state — a corrupt state arena, an out-of-range arena offset —
/// where continuing would produce wrong simulation results rather than a crash.
/// Wrong results are the worst failure mode this project has (DESIGN_GUIDE.md
/// section 1.2), so paying for the check is the right trade even in release.
///
/// This is not the mechanism for reporting user errors: a malformed design or a
/// bad command line is a diagnostic (see Diagnostics.h), never an assertion.
#define ENZAN_ALWAYS_ASSERT(condition, message) \
    ((condition)                                \
         ? void(0)                              \
         : ::enzan::detail::assertionFailed(#condition, (message), __FILE__, __LINE__, __func__))

/// Asserts an internal invariant in builds that enable assertions.
///
/// Compiled out entirely when ENZAN_ENABLE_ASSERTIONS is not defined, which the
/// build sets for Debug, RelWithDebInfo and sanitizer configurations but not for
/// Release. Use this for the ordinary precondition and bounds checks that sit on
/// the simulation hot path, where the cost in a release build is not acceptable.
#ifdef ENZAN_ENABLE_ASSERTIONS
#define ENZAN_ASSERT(condition, message) ENZAN_ALWAYS_ASSERT(condition, message)
#else
#define ENZAN_ASSERT(condition, message) void(0)
#endif

/// Marks a point the program must never reach.
///
/// Always aborts with a diagnostic rather than becoming undefined behaviour,
/// because a switch that silently falls through an unhandled enumerator is the
/// kind of defect that surfaces as a mysterious wrong value many time steps
/// later.
#define ENZAN_UNREACHABLE(message)                                                              \
    ::enzan::detail::assertionFailed("unreachable code reached", (message), __FILE__, __LINE__, \
                                     __func__)
