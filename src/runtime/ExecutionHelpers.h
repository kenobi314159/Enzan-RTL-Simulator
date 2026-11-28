// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace enzan::runtime {

class ExecutionContext;

// Callable entry points for generated code. The context is opaque to generated code; indices,
// widths and frame-word offsets come from verified bytecode. The ABI is internal to one build,
// not a persistent native-code format. None of these helpers silently invokes the interpreter.
extern "C" {

/// Bytecode instruction index to resume, or zero for a fresh process or kernel call.
std::uint32_t enzanExecResumePc(const ExecutionContext* context) noexcept;

/// Start of this activation's frame, or null for a body with no frame words. Each register's
/// aval words precede its bval words. Use the pointer only during this activation; call-stack
/// frames can be reused after a return.
std::uint64_t* enzanExecFrame(ExecutionContext* context) noexcept;

/// Start of a bound object's two-plane value. The body supplies its width. The address is stable
/// during an activation, but callbacks and runtime helpers may change the value it points to.
const std::uint64_t* enzanExecReadObject(ExecutionContext* context,
                                         std::uint32_t objectIndex) noexcept;

/// Store a frame value through the bound object's write route, using ordinary kernel semantics.
/// `sourceWordOffset` is relative to this activation's frame; `writePosition` indexes its routes.
void enzanExecStore(ExecutionContext* context, std::uint32_t objectIndex,
                    std::uint32_t writePosition, std::uint32_t sourceWordOffset,
                    std::uint32_t sourceWidth);

/// Suspend on a frame-held delay, treating unknown bits as zero. Returns the numeric
/// ExecutionResult::Suspended or ExecutionResult::Stopped, as the interpreter does.
std::uint32_t enzanExecWaitTime(ExecutionContext* context, std::uint32_t delayWordOffset,
                                std::uint32_t delayWidth, std::uint32_t resumePc);

/// Perform a system task. Returns numeric ExecutionResult::Completed to continue or
/// ExecutionResult::Stopped after finish/error, unwinding any in-progress calls in that case.
std::uint32_t enzanExecSystemTask(ExecutionContext* context, std::uint32_t taskIndex);

/// Add logical bytecode instructions actually executed. A generated body must account before
/// operations that can stop or re-enter it, and must not count instructions it never reaches.
void enzanExecCount(ExecutionContext* context, std::uint64_t instructions) noexcept;

} // extern "C"

} // namespace enzan::runtime
