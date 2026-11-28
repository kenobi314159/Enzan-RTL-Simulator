// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "runtime/Execution.h"

#include <cstddef>
#include <cstdint>

namespace enzan::runtime {

class Kernel;
struct Activation;

/// Executes compiled bodies.
///
/// The default bytecode interpreter and the reference for a future JIT's differential tests.
/// Each invocation keeps its state on the stack so callbacks can re-enter the backend.
///
/// Dispatch is a `switch`. Computed goto is a recommendation rather than an invariant,
/// and nothing is optimised without a measurement (DESIGN_GUIDE.md section 13.4).
/// Threaded dispatch is considered only if profiling and benchmarks justify it; the `switch`
/// keeps the reference semantics readable.
class Interpreter {
public:
    /// `kernel` must outlive the interpreter.
    explicit Interpreter(Kernel& owner) noexcept;

    /// Runs the bound process from its resume point until it completes, suspends or stops.
    ExecutionResult run(const ExecutionContext& context);

    /// Runs a kernel-invoked subprogram, such as a VHDL resolution function, to its return.
    ExecutionResult call(const ExecutionContext& context);

private:
    /// Runs `current` from `pc` until it returns at `baseDepth` calls in progress, suspends or
    /// stops the simulation.
    ExecutionResult execute(std::uint32_t processIndex, Activation current, std::uint32_t pc,
                            std::size_t baseDepth);

    Kernel& kernel;
};

} // namespace enzan::runtime
