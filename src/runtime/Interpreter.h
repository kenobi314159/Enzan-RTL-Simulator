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

/// Executes compiled bodies using switch dispatch.
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
