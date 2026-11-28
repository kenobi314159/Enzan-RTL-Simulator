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

/// Return the frame and instruction counter addresses, valid only during this activation.
/// Generated code must publish counts before a helper can stop or re-enter the kernel.
std::uint64_t* enzanExecFrameAndCounter(ExecutionContext* context,
                                        std::uint64_t** counter) noexcept;

/// Yield a subprogram call to the native backend's iterative dispatcher.
void enzanExecRequestCall(ExecutionContext* context, std::uint32_t callIndex,
                          std::uint32_t resumePc) noexcept;

/// Extend or truncate a frame value, preserving the bit planes and signedness.
void enzanExecExtend(ExecutionContext* context, std::uint32_t destinationWordOffset,
                     std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                     std::uint32_t sourceWidth, std::uint32_t signExtend);

/// Translate a VHDL value to or from its four-state presentation.
void enzanExecPresent(ExecutionContext* context, std::uint32_t destinationWordOffset,
                      std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                      std::uint32_t sourceWidth, std::uint32_t encoding);
void enzanExecEncode(ExecutionContext* context, std::uint32_t destinationWordOffset,
                     std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                     std::uint32_t sourceWidth, std::uint32_t encoding, std::uint32_t weak);

/// Select or update a part of a frame value, using the full four-state index.
void enzanExecBitSelect(ExecutionContext* context, std::uint32_t destinationWordOffset,
                        std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                        std::uint32_t sourceWidth, std::uint32_t indexWordOffset,
                        std::uint32_t indexWidth);
void enzanExecPartSelect(ExecutionContext* context, std::uint32_t destinationWordOffset,
                         std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                         std::uint32_t sourceWidth, std::uint32_t indexWordOffset,
                         std::uint32_t indexWidth);
void enzanExecSplicePart(ExecutionContext* context, std::uint32_t destinationWordOffset,
                         std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                         std::uint32_t sourceWidth, std::uint32_t indexWordOffset,
                         std::uint32_t indexWidth);

/// Compare two equally wide frame values, producing a single four-state bit.
void enzanExecCompare(ExecutionContext* context, std::uint32_t destinationWordOffset,
                      std::uint32_t leftWordOffset, std::uint32_t rightWordOffset,
                      std::uint32_t width, std::uint32_t opcode, std::uint32_t isSigned);

/// Divide or take the remainder of equally wide integer frame values.
void enzanExecDivRem(ExecutionContext* context, std::uint32_t destinationWordOffset,
                     std::uint32_t leftWordOffset, std::uint32_t rightWordOffset,
                     std::uint32_t width, std::uint32_t isSigned, std::uint32_t remainder);

/// Perform a wide integer arithmetic operation on frame values.
void enzanExecWideArithmetic(ExecutionContext* context, std::uint32_t destinationWordOffset,
                             std::uint32_t leftWordOffset, std::uint32_t rightWordOffset,
                             std::uint32_t width, std::uint32_t opcode);

/// Shift a frame value by a frame-held count of any width.
void enzanExecWideShift(ExecutionContext* context, std::uint32_t destinationWordOffset,
                        std::uint32_t sourceWordOffset, std::uint32_t width,
                        std::uint32_t amountWordOffset, std::uint32_t amountWidth,
                        std::uint32_t opcode);

/// Start of a bound object's two-plane value. The body supplies its width. The address is stable
/// during an activation, but callbacks and runtime helpers may change the value it points to.
const std::uint64_t* enzanExecReadObject(ExecutionContext* context,
                                         std::uint32_t objectIndex) noexcept;

/// Read an object window at a frame-held offset, with out-of-range bits read as X.
void enzanExecReadObjectPart(ExecutionContext* context, std::uint32_t destinationWordOffset,
                             std::uint32_t destinationWidth, std::uint32_t objectIndex,
                             std::uint32_t offsetWordOffset, std::uint32_t offsetWidth);

/// Read a bound signal's event/activity flag or its value before its last change.
std::uint32_t enzanExecSignalAttribute(ExecutionContext* context, std::uint32_t objectIndex,
                                       std::uint32_t active) noexcept;
const std::uint64_t* enzanExecLastValue(ExecutionContext* context,
                                        std::uint32_t objectIndex) noexcept;

/// Read the kernel's current simulation time.
std::uint64_t enzanExecTime(ExecutionContext* context) noexcept;

/// Store a frame value through the bound object's write route, using ordinary kernel semantics.
/// `sourceWordOffset` is relative to this activation's frame; `writePosition` indexes its routes.
void enzanExecStore(ExecutionContext* context, std::uint32_t objectIndex,
                    std::uint32_t writePosition, std::uint32_t sourceWordOffset,
                    std::uint32_t sourceWidth);

/// Enqueue a whole nonblocking write using the activation's bound write signal.
void enzanExecStoreNonBlocking(ExecutionContext* context, std::uint32_t objectIndex,
                               std::uint32_t sourceWordOffset, std::uint32_t sourceWidth);

/// Apply or enqueue a partial write; an unknown offset writes nothing.
void enzanExecStorePart(ExecutionContext* context, std::uint32_t objectIndex,
                        std::uint32_t writePosition, std::uint32_t sourceWordOffset,
                        std::uint32_t sourceWidth, std::uint32_t offsetWordOffset,
                        std::uint32_t offsetWidth, std::uint32_t nonBlocking);

/// Schedule a whole VHDL driver transaction through the kernel's projected waveform.
/// Returns Completed or Stopped, including on time overflow.
std::uint32_t enzanExecDrive(ExecutionContext* context, std::uint32_t objectIndex,
                             std::uint32_t writePosition, std::uint32_t valueWordOffset,
                             std::uint32_t valueWidth, std::uint32_t delayWordOffset,
                             std::uint32_t delayWidth, std::uint32_t inertial);

/// Schedule a partial VHDL transaction; the offset and optional delay are frame values.
std::uint32_t enzanExecDrivePart(ExecutionContext* context, std::uint32_t objectIndex,
                                 std::uint32_t writePosition, std::uint32_t valueWordOffset,
                                 std::uint32_t valueWidth, std::uint32_t argumentWordOffset,
                                 std::uint32_t argumentWidth, std::uint32_t opcode);

/// Suspend on a frame-held delay, treating unknown bits as zero. Returns the numeric
/// ExecutionResult::Suspended or ExecutionResult::Stopped, as the interpreter does.
std::uint32_t enzanExecWaitTime(ExecutionContext* context, std::uint32_t delayWordOffset,
                                std::uint32_t delayWidth, std::uint32_t resumePc);

/// Suspend on one edge or an event list. The edge value is an `eir::EdgeKind`.
std::uint32_t enzanExecWaitEdge(ExecutionContext* context, std::uint32_t objectIndex,
                                std::uint32_t edge, std::uint32_t resumePc);
std::uint32_t enzanExecWaitEvents(ExecutionContext* context, std::uint32_t listIndex,
                                  std::uint32_t resumePc);

/// Perform a system task. Returns numeric ExecutionResult::Completed to continue or
/// ExecutionResult::Stopped after finish/error, unwinding any in-progress calls in that case.
std::uint32_t enzanExecSystemTask(ExecutionContext* context, std::uint32_t taskIndex);

/// Perform a system function, writing its result into the activation's frame.
std::uint32_t enzanExecSystemFunction(ExecutionContext* context, std::uint32_t functionIndex,
                                      std::uint32_t destinationWordOffset,
                                      std::uint32_t destinationWidth);

/// Add logical bytecode instructions actually executed. A generated body must account before
/// operations that can stop or re-enter it, and must not count instructions it never reaches.
void enzanExecCount(ExecutionContext* context, std::uint64_t instructions) noexcept;

} // extern "C"

} // namespace enzan::runtime
