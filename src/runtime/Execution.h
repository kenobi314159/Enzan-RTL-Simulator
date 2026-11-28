// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace enzan::codegen {
struct CompiledBody;
}

namespace enzan::runtime {

class Kernel;
struct WriteRoute;

/// A compiled body bound to the frame, objects, routes and callees of one instance.
struct Activation {
    const codegen::CompiledBody* body = nullptr;
    /// Index of that body in the image's compiled design.
    std::uint32_t bodyIndex = 0xFFFF'FFFFU;
    /// Word offset into the kernel's frame storage, not a byte offset.
    std::uint32_t frameOffset = 0;
    /// Per-object signal indices for reads and for writes; port bindings can differ.
    std::span<const std::uint32_t> objectSignals;
    std::span<const std::uint32_t> writeSignals;
    /// Routes are indexed by the body's written-object positions, not by object index.
    std::span<const WriteRoute> writeRoutes;
    /// Bound subprogram indices, in the body's call-site order.
    std::span<const std::uint32_t> calls;
};

/// A synchronous invocation. Its activation and context live only until the backend returns.
///
/// A nested callback or resolution gets a distinct context, so it cannot overwrite the outer
/// activation. Generated code must use runtime helpers, not this class's C++ member layout.
class ExecutionContext {
public:
    [[nodiscard]] Kernel& kernel() const noexcept { return *owner; }

    [[nodiscard]] std::uint32_t processIndex() const noexcept { return process; }

    [[nodiscard]] const Activation& activation() const noexcept { return *bound; }

    [[nodiscard]] std::uint32_t resumePc() const noexcept { return resume; }

    /// A nested activation uses the same kernel and process, with its own frame and PC.
    [[nodiscard]] ExecutionContext withActivation(const Activation& activation,
                                                  std::uint32_t resumePc) const noexcept {
        return {*owner, process, activation, resumePc};
    }

    /// A native body yields a call to its backend's iterative dispatcher.
    void requestCall(std::uint32_t callIndex, std::uint32_t resumePc) noexcept {
        requestedCall = callIndex;
        callResume = resumePc;
    }

    [[nodiscard]] std::uint32_t callIndex() const noexcept { return requestedCall; }

    [[nodiscard]] std::uint32_t callResumePc() const noexcept { return callResume; }

private:
    friend class Kernel;

    ExecutionContext(Kernel& kernel, std::uint32_t processIndex, const Activation& activation,
                     std::uint32_t resumePc) noexcept
        : owner(&kernel), process(processIndex), bound(&activation), resume(resumePc) {}

    Kernel* owner;
    std::uint32_t process;
    const Activation* bound;
    std::uint32_t resume;
    std::uint32_t requestedCall = 0;
    std::uint32_t callResume = 0;
};

/// How executing a process body ended.
enum class ExecutionResult : std::uint8_t {
    /// The body ran to its `ret`. A repeating process re-arms; a one-shot one is done.
    Completed = 0,
    /// The body suspended on a delay or an event control and will resume later.
    Suspended = 1,
    /// The run must stop: `$finish`, `$fatal`, or a kernel error.
    Stopped = 2,
};

/// Executes bodies for one kernel. Calls may re-enter the backend through kernel callbacks.
///
/// The kernel owns the backend and destroys it before its own state. The image and compiled
/// bodies outlive the kernel. A backend must not use either after it is destroyed.
class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;

    /// Runs a process from its resume point until it completes, suspends or stops.
    virtual ExecutionResult run(ExecutionContext& context) = 0;

    /// Runs a bound subprogram the kernel calls on behalf of a process, such as resolution.
    /// The call is synchronous; the backend must not retain the activation reference.
    virtual ExecutionResult call(ExecutionContext& context) = 0;
};

/// Creates the default interpreter backend; each kernel owns its own backend.
[[nodiscard]] std::unique_ptr<ExecutionBackend> makeInterpreterBackend();

} // namespace enzan::runtime
