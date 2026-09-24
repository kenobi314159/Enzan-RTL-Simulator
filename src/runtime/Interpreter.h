// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace enzan::runtime {

class Kernel;

/// How executing a process body ended.
enum class ExecutionResult : std::uint8_t {
    /// The body ran to its `ret`. A repeating process re-arms; a one-shot one is done.
    Completed,
    /// The body suspended on a delay or an event control and will resume later.
    Suspended,
    /// The run must stop: `$finish`, `$fatal`, or a kernel error.
    Stopped,
    /// Execution reached a breakpoint, or completed a source line while stepping.
    ///
    /// The body is left resumable at the instruction it stopped *before*, so continuing runs
    /// it and nothing has happened twice. Distinct from `Suspended` because nothing will wake
    /// this process: the kernel puts it back at the head of its region and stops the run.
    Paused,
};

/// Executes compiled bodies.
///
/// A bytecode interpreter, and deliberately the *only* execution engine until M8. The
/// interpreter must exist before the JIT both to validate the semantics and to serve as
/// the reference the JIT is differentially tested against (DESIGN_GUIDE.md section 6.3),
/// so anything clever belongs in the JIT rather than here.
///
/// Dispatch is a `switch`. Section 6.1 says the loop SHOULD use computed goto, which is
/// a recommendation rather than an invariant, and section 13.4 forbids optimising
/// without a measurement. Threaded dispatch arrives with M8's profiling, when there is a
/// benchmark to show what it buys; the `switch` is what keeps the semantics readable
/// while they are still being pinned down by tests.
class Interpreter {
public:
    /// `kernel` must outlive the interpreter.
    explicit Interpreter(Kernel& owner) noexcept;

    /// Runs process `processIndex` from its resume point until it completes, suspends or
    /// stops the simulation.
    ExecutionResult run(std::uint32_t processIndex);

private:
    Kernel& kernel;
};

} // namespace enzan::runtime
