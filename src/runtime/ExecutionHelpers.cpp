// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/ExecutionHelpers.h"

#include "codegen/Bytecode.h"
#include "eir/BitOps.h"
#include "eir/BitVector.h"
#include "runtime/Execution.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"

#include <cstdint>

namespace enzan::runtime {

/// Access to kernel-owned bookkeeping without exposing its member layout to generated code.
struct ExecutionHelperAccess {
    static bool error(const Kernel& kernel) noexcept { return kernel.errorRequested; }

    static bool stopping(const Kernel& kernel) noexcept { return kernel.stopping(); }

    static void count(Kernel& kernel, std::uint64_t instructions) noexcept {
        kernel.instructionCount += instructions;
    }
};

extern "C" std::uint32_t enzanExecResumePc(const ExecutionContext* context) noexcept {
    return context->resumePc();
}

extern "C" std::uint64_t* enzanExecFrame(ExecutionContext* context) noexcept {
    const Activation& activation = context->activation();
    if (activation.body->frameWords == 0) return nullptr;
    return context->kernel().frameAt(activation.frameOffset, 1).aval();
}

extern "C" void enzanExecExtend(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                                std::uint32_t sourceWidth, std::uint32_t signExtend) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    eir::bitops::extend(kernel.frameAt(frame + destinationWordOffset, destinationWidth),
                        kernel.frameAt(frame + sourceWordOffset, sourceWidth), signExtend != 0);
}

extern "C" const std::uint64_t* enzanExecReadObject(ExecutionContext* context,
                                                    std::uint32_t objectIndex) noexcept {
    Kernel& kernel = context->kernel();
    const std::uint32_t signalIndex = context->activation().objectSignals[objectIndex];
    const Signal& signal = kernel.image().signals()[signalIndex];
    return kernel.arena().at(signal.wordOffset, signal.bitWidth).aval();
}

extern "C" void enzanExecStore(ExecutionContext* context, std::uint32_t objectIndex,
                               std::uint32_t writePosition, std::uint32_t sourceWordOffset,
                               std::uint32_t sourceWidth) {
    Kernel& kernel = context->kernel();
    const Activation& activation = context->activation();
    kernel.storeBound(activation, objectIndex, writePosition,
                      kernel.frameAt(activation.frameOffset + sourceWordOffset, sourceWidth));
}

extern "C" void enzanExecStoreNonBlocking(ExecutionContext* context, std::uint32_t objectIndex,
                                          std::uint32_t sourceWordOffset,
                                          std::uint32_t sourceWidth) {
    Kernel& kernel = context->kernel();
    const Activation& activation = context->activation();
    kernel.scheduleNonBlocking(
        activation.writeSignals[objectIndex],
        kernel.frameAt(activation.frameOffset + sourceWordOffset, sourceWidth));
}

extern "C" std::uint32_t enzanExecWaitTime(ExecutionContext* context, std::uint32_t delayWordOffset,
                                           std::uint32_t delayWidth, std::uint32_t resumePc) {
    Kernel& kernel = context->kernel();
    const eir::ConstBitRef delay =
        kernel.frameAt(context->activation().frameOffset + delayWordOffset, delayWidth);
    kernel.suspendForTime(context->processIndex(), delay.hasUnknown() ? 0 : delay.toUInt64(),
                          resumePc);
    return static_cast<std::uint32_t>(ExecutionHelperAccess::error(kernel)
                                          ? ExecutionResult::Stopped
                                          : ExecutionResult::Suspended);
}

extern "C" std::uint32_t enzanExecWaitEdge(ExecutionContext* context, std::uint32_t objectIndex,
                                           std::uint32_t edge, std::uint32_t resumePc) {
    context->kernel().suspendForEdge(context->processIndex(),
                                     context->activation().objectSignals[objectIndex],
                                     static_cast<eir::EdgeKind>(edge), resumePc);
    return static_cast<std::uint32_t>(ExecutionResult::Suspended);
}

extern "C" std::uint32_t enzanExecWaitEvents(ExecutionContext* context, std::uint32_t listIndex,
                                             std::uint32_t resumePc) {
    context->kernel().suspendForEvents(context->processIndex(), listIndex, resumePc);
    return static_cast<std::uint32_t>(ExecutionResult::Suspended);
}

extern "C" std::uint32_t enzanExecSystemTask(ExecutionContext* context, std::uint32_t taskIndex) {
    Kernel& kernel = context->kernel();
    kernel.runSystemTask(context->processIndex(), context->activation(), taskIndex);
    if (!ExecutionHelperAccess::stopping(kernel))
        return static_cast<std::uint32_t>(ExecutionResult::Completed);
    kernel.unwindCalls();
    return static_cast<std::uint32_t>(ExecutionResult::Stopped);
}

extern "C" void enzanExecCount(ExecutionContext* context, std::uint64_t instructions) noexcept {
    ExecutionHelperAccess::count(context->kernel(), instructions);
}

} // namespace enzan::runtime
