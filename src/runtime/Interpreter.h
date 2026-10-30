// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace enzan::runtime {

class Kernel;
struct Activation;

/// How executing a process body ended.
enum class ExecutionResult : std::uint8_t {
    /// The body ran to its `ret`. A repeating process re-arms; a one-shot one is done.
    Completed,
    /// The body suspended on a delay or an event control and will resume later.
    Suspended,
    /// The run must stop: `$finish`, `$fatal`, or a kernel error.
    Stopped,
};

/// Executes compiled bodies.
///
/// A bytecode interpreter, and deliberately the *only* execution engine until M8. The
/// interpreter must exist before the JIT both to validate the semantics and to serve as
/// the reference the JIT is differentially tested against, so anything clever belongs in
/// the JIT rather than here.
///
/// Dispatch is a `switch`. Computed goto is a recommendation rather than an invariant,
/// and nothing is optimised without a measurement (DESIGN_GUIDE.md section 13.4).
/// Threaded dispatch arrives with M8's profiling, when there is a benchmark to show
/// what it buys; the `switch` is what keeps the semantics readable while they are still
/// being pinned down by tests.
class Interpreter {
public:
    /// `kernel` must outlive the interpreter.
    explicit Interpreter(Kernel& owner) noexcept;

    /// Runs process `processIndex` from its resume point until it completes, suspends or
    /// stops the simulation.
    ExecutionResult run(std::uint32_t processIndex);

    /// Runs `callee`, a subprogram the kernel calls rather than a body, as it does a VHDL
    /// resolution function, to its return, on behalf of process `processIndex`, whose name a
    /// diagnostic gives.
    ExecutionResult call(std::uint32_t processIndex, const Activation& callee);

private:
    /// Runs `current` from `pc` until it returns at `baseDepth` calls in progress, suspends or
    /// stops the simulation.
    ExecutionResult execute(std::uint32_t processIndex, Activation current, std::uint32_t pc,
                            std::size_t baseDepth);

    Kernel& kernel;
};

} // namespace enzan::runtime
