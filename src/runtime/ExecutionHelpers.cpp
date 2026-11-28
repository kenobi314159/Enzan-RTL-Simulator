// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/ExecutionHelpers.h"

#include "codegen/Bytecode.h"
#include "common/Assert.h"
#include "eir/BitOps.h"
#include "eir/BitVector.h"
#include "eir/Module.h"
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

    static std::uint64_t* counter(Kernel& kernel) noexcept { return &kernel.instructionCount; }
};

extern "C" std::uint32_t enzanExecResumePc(const ExecutionContext* context) noexcept {
    return context->resumePc();
}

extern "C" std::uint64_t* enzanExecFrame(ExecutionContext* context) noexcept {
    const Activation& activation = context->activation();
    if (activation.body->frameWords == 0) return nullptr;
    return context->kernel().frameAt(activation.frameOffset, 1).aval();
}

extern "C" std::uint64_t* enzanExecFrameAndCounter(ExecutionContext* context,
                                                   std::uint64_t** counter) noexcept {
    *counter = ExecutionHelperAccess::counter(context->kernel());
    return enzanExecFrame(context);
}

extern "C" void enzanExecRequestCall(ExecutionContext* context, std::uint32_t callIndex,
                                     std::uint32_t resumePc) noexcept {
    context->requestCall(callIndex, resumePc);
}

extern "C" void enzanExecExtend(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                                std::uint32_t sourceWidth, std::uint32_t signExtend) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    eir::bitops::extend(kernel.frameAt(frame + destinationWordOffset, destinationWidth),
                        kernel.frameAt(frame + sourceWordOffset, sourceWidth), signExtend != 0);
}

extern "C" void enzanExecPresent(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                 std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                                 std::uint32_t sourceWidth, std::uint32_t encoding) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    eir::present(static_cast<eir::Encoding>(encoding),
                 kernel.frameAt(frame + sourceWordOffset, sourceWidth),
                 kernel.frameAt(frame + destinationWordOffset, destinationWidth));
}

extern "C" void enzanExecEncode(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                                std::uint32_t sourceWidth, std::uint32_t encoding,
                                std::uint32_t weak) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    eir::encode(static_cast<eir::Encoding>(encoding),
                kernel.frameAt(frame + sourceWordOffset, sourceWidth),
                kernel.frameAt(frame + destinationWordOffset, destinationWidth),
                static_cast<std::uint8_t>(weak));
}

namespace {
template<auto Operation>
void select(ExecutionContext* context, std::uint32_t destinationWordOffset,
            std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
            std::uint32_t sourceWidth, std::uint32_t indexWordOffset, std::uint32_t indexWidth) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    Operation(kernel.frameAt(frame + destinationWordOffset, destinationWidth),
              kernel.frameAt(frame + sourceWordOffset, sourceWidth),
              kernel.frameAt(frame + indexWordOffset, indexWidth));
}
} // namespace

extern "C" void enzanExecBitSelect(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                   std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                                   std::uint32_t sourceWidth, std::uint32_t indexWordOffset,
                                   std::uint32_t indexWidth) {
    select<eir::bitops::bitSelect>(context, destinationWordOffset, destinationWidth,
                                   sourceWordOffset, sourceWidth, indexWordOffset, indexWidth);
}

extern "C" void enzanExecPartSelect(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                    std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                                    std::uint32_t sourceWidth, std::uint32_t indexWordOffset,
                                    std::uint32_t indexWidth) {
    select<eir::bitops::partSelect>(context, destinationWordOffset, destinationWidth,
                                    sourceWordOffset, sourceWidth, indexWordOffset, indexWidth);
}

extern "C" void enzanExecSplicePart(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                    std::uint32_t destinationWidth, std::uint32_t sourceWordOffset,
                                    std::uint32_t sourceWidth, std::uint32_t indexWordOffset,
                                    std::uint32_t indexWidth) {
    select<eir::bitops::assignPart>(context, destinationWordOffset, destinationWidth,
                                    sourceWordOffset, sourceWidth, indexWordOffset, indexWidth);
}

extern "C" void enzanExecCompare(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                 std::uint32_t leftWordOffset, std::uint32_t rightWordOffset,
                                 std::uint32_t width, std::uint32_t opcode,
                                 std::uint32_t isSigned) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    const eir::BitRef destination = kernel.frameAt(frame + destinationWordOffset, 1);
    const eir::ConstBitRef left = kernel.frameAt(frame + leftWordOffset, width);
    const eir::ConstBitRef right = kernel.frameAt(frame + rightWordOffset, width);
    switch (static_cast<eir::Opcode>(opcode)) {
    case eir::Opcode::Equal: eir::bitops::equal(destination, left, right); break;
    case eir::Opcode::NotEqual: eir::bitops::notEqual(destination, left, right); break;
    case eir::Opcode::CaseEqual: eir::bitops::caseEqual(destination, left, right); break;
    case eir::Opcode::CaseNotEqual: eir::bitops::caseNotEqual(destination, left, right); break;
    case eir::Opcode::WildcardEqual: eir::bitops::wildcardEqual(destination, left, right); break;
    case eir::Opcode::WildcardNotEqual:
        eir::bitops::wildcardNotEqual(destination, left, right);
        break;
    case eir::Opcode::CaseZMatch: eir::bitops::caseZMatch(destination, left, right); break;
    case eir::Opcode::CaseXMatch: eir::bitops::caseXMatch(destination, left, right); break;
    case eir::Opcode::LessThan:
        eir::bitops::lessThan(destination, left, right, isSigned != 0);
        break;
    case eir::Opcode::LessEqual:
        eir::bitops::lessEqual(destination, left, right, isSigned != 0);
        break;
    default: ENZAN_UNREACHABLE("a native comparison must have a supported opcode");
    }
}

extern "C" void enzanExecDivRem(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                std::uint32_t leftWordOffset, std::uint32_t rightWordOffset,
                                std::uint32_t width, std::uint32_t isSigned,
                                std::uint32_t remainder) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    const eir::BitRef destination = kernel.frameAt(frame + destinationWordOffset, width);
    const eir::ConstBitRef left = kernel.frameAt(frame + leftWordOffset, width);
    const eir::ConstBitRef right = kernel.frameAt(frame + rightWordOffset, width);
    if (remainder != 0)
        eir::bitops::modulo(destination, left, right, isSigned != 0);
    else
        eir::bitops::divide(destination, left, right, isSigned != 0);
}

extern "C" void enzanExecWideArithmetic(ExecutionContext* context,
                                        std::uint32_t destinationWordOffset,
                                        std::uint32_t leftWordOffset, std::uint32_t rightWordOffset,
                                        std::uint32_t width, std::uint32_t opcode) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    const eir::BitRef destination = kernel.frameAt(frame + destinationWordOffset, width);
    const eir::ConstBitRef left = kernel.frameAt(frame + leftWordOffset, width);
    const auto operation = static_cast<eir::Opcode>(opcode);
    if (operation == eir::Opcode::Negate) {
        eir::bitops::negate(destination, left);
        return;
    }
    const eir::ConstBitRef right = kernel.frameAt(frame + rightWordOffset, width);
    switch (operation) {
    case eir::Opcode::Add: eir::bitops::add(destination, left, right); break;
    case eir::Opcode::Subtract: eir::bitops::subtract(destination, left, right); break;
    case eir::Opcode::Multiply: eir::bitops::multiply(destination, left, right); break;
    default: ENZAN_UNREACHABLE("a native arithmetic helper must have a supported opcode");
    }
}

extern "C" void enzanExecWideShift(ExecutionContext* context, std::uint32_t destinationWordOffset,
                                   std::uint32_t sourceWordOffset, std::uint32_t width,
                                   std::uint32_t amountWordOffset, std::uint32_t amountWidth,
                                   std::uint32_t opcode) {
    Kernel& kernel = context->kernel();
    const std::uint32_t frame = context->activation().frameOffset;
    const eir::BitRef destination = kernel.frameAt(frame + destinationWordOffset, width);
    const eir::ConstBitRef source = kernel.frameAt(frame + sourceWordOffset, width);
    const eir::ConstBitRef amount = kernel.frameAt(frame + amountWordOffset, amountWidth);
    switch (static_cast<eir::Opcode>(opcode)) {
    case eir::Opcode::ShiftLeft: eir::bitops::shiftLeft(destination, source, amount); break;
    case eir::Opcode::ShiftRightLogical:
        eir::bitops::shiftRightLogical(destination, source, amount);
        break;
    case eir::Opcode::ShiftRightArithmetic:
        eir::bitops::shiftRightArithmetic(destination, source, amount);
        break;
    default: ENZAN_UNREACHABLE("a native shift helper must have a supported opcode");
    }
}

extern "C" const std::uint64_t* enzanExecReadObject(ExecutionContext* context,
                                                    std::uint32_t objectIndex) noexcept {
    Kernel& kernel = context->kernel();
    const std::uint32_t signalIndex = context->activation().objectSignals[objectIndex];
    const Signal& signal = kernel.image().signals()[signalIndex];
    return kernel.arena().at(signal.wordOffset, signal.bitWidth).aval();
}

extern "C" void enzanExecReadObjectPart(ExecutionContext* context,
                                        std::uint32_t destinationWordOffset,
                                        std::uint32_t destinationWidth, std::uint32_t objectIndex,
                                        std::uint32_t offsetWordOffset, std::uint32_t offsetWidth) {
    Kernel& kernel = context->kernel();
    const Activation& activation = context->activation();
    const auto destination =
        kernel.frameAt(activation.frameOffset + destinationWordOffset, destinationWidth);
    const eir::ConstBitRef offset =
        kernel.frameAt(activation.frameOffset + offsetWordOffset, offsetWidth);
    if (offset.hasUnknown()) {
        eir::bitops::setUnknown(destination);
        return;
    }
    const Signal& signal = kernel.image().signals()[activation.objectSignals[objectIndex]];
    eir::bitops::partSelectAt(destination, kernel.arena().at(signal.wordOffset, signal.bitWidth),
                              offset.toUInt64());
}

extern "C" std::uint32_t enzanExecSignalAttribute(ExecutionContext* context,
                                                  std::uint32_t objectIndex,
                                                  std::uint32_t active) noexcept {
    const Kernel& kernel = context->kernel();
    const std::uint32_t signal = context->activation().objectSignals[objectIndex];
    return static_cast<std::uint32_t>(active != 0 ? kernel.activeThisCycle(signal)
                                                  : kernel.changedThisCycle(signal));
}

extern "C" const std::uint64_t* enzanExecLastValue(ExecutionContext* context,
                                                   std::uint32_t objectIndex) noexcept {
    const std::uint32_t signal = context->activation().objectSignals[objectIndex];
    return context->kernel().lastValue(signal).aval();
}

extern "C" std::uint64_t enzanExecTime(ExecutionContext* context) noexcept {
    return context->kernel().time();
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

extern "C" void enzanExecStorePart(ExecutionContext* context, std::uint32_t objectIndex,
                                   std::uint32_t writePosition, std::uint32_t sourceWordOffset,
                                   std::uint32_t sourceWidth, std::uint32_t offsetWordOffset,
                                   std::uint32_t offsetWidth, std::uint32_t nonBlocking) {
    Kernel& kernel = context->kernel();
    const Activation& activation = context->activation();
    const eir::ConstBitRef offset =
        kernel.frameAt(activation.frameOffset + offsetWordOffset, offsetWidth);
    if (offset.hasUnknown()) return;
    const eir::ConstBitRef source =
        kernel.frameAt(activation.frameOffset + sourceWordOffset, sourceWidth);
    if (nonBlocking != 0) {
        kernel.scheduleNonBlockingPart(activation.writeSignals[objectIndex], source,
                                       offset.toUInt64());
    } else {
        const WriteRoute& route = activation.writeRoutes[writePosition];
        if (route.direct())
            kernel.writeSignalPart(activation.writeSignals[objectIndex], source, offset.toUInt64());
        else
            kernel.writeThroughDrivers(route, source, offset.toUInt64());
    }
}

extern "C" std::uint32_t enzanExecDrive(ExecutionContext* context, std::uint32_t objectIndex,
                                        std::uint32_t writePosition, std::uint32_t valueWordOffset,
                                        std::uint32_t valueWidth, std::uint32_t delayWordOffset,
                                        std::uint32_t delayWidth, std::uint32_t inertial) {
    Kernel& kernel = context->kernel();
    const Activation& activation = context->activation();
    const eir::ConstBitRef delay =
        kernel.frameAt(activation.frameOffset + delayWordOffset, delayWidth);
    kernel.drive(context->processIndex(), writePosition, activation.writeSignals[objectIndex],
                 kernel.frameAt(activation.frameOffset + valueWordOffset, valueWidth), 0,
                 delay.hasUnknown() ? 0 : delay.toUInt64(), inertial != 0);
    return static_cast<std::uint32_t>(ExecutionHelperAccess::error(kernel)
                                          ? ExecutionResult::Stopped
                                          : ExecutionResult::Completed);
}

extern "C" std::uint32_t enzanExecDrivePart(ExecutionContext* context, std::uint32_t objectIndex,
                                            std::uint32_t writePosition,
                                            std::uint32_t valueWordOffset, std::uint32_t valueWidth,
                                            std::uint32_t argumentWordOffset,
                                            std::uint32_t argumentWidth, std::uint32_t opcode) {
    Kernel& kernel = context->kernel();
    const Activation& activation = context->activation();
    const eir::ConstBitRef argument =
        kernel.frameAt(activation.frameOffset + argumentWordOffset, argumentWidth);
    if (argument.hasUnknown()) return static_cast<std::uint32_t>(ExecutionResult::Completed);
    const auto operation = static_cast<eir::Opcode>(opcode);
    const bool delayed = operation != eir::Opcode::DrivePart;
    const std::uint64_t delay = delayed ? argument.avalWords()[1] : 0;
    const bool inertial = operation != eir::Opcode::DriveTransportPart;
    kernel.drive(context->processIndex(), writePosition, activation.writeSignals[objectIndex],
                 kernel.frameAt(activation.frameOffset + valueWordOffset, valueWidth),
                 static_cast<std::uint32_t>(argument.toUInt64()), delay, inertial);
    return static_cast<std::uint32_t>(ExecutionHelperAccess::error(kernel)
                                          ? ExecutionResult::Stopped
                                          : ExecutionResult::Completed);
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

extern "C" std::uint32_t enzanExecSystemFunction(ExecutionContext* context,
                                                 std::uint32_t functionIndex,
                                                 std::uint32_t destinationWordOffset,
                                                 std::uint32_t destinationWidth) {
    Kernel& kernel = context->kernel();
    const Activation& activation = context->activation();
    kernel.runSystemFunction(
        context->processIndex(), activation, functionIndex,
        kernel.frameAt(activation.frameOffset + destinationWordOffset, destinationWidth));
    if (!ExecutionHelperAccess::stopping(kernel))
        return static_cast<std::uint32_t>(ExecutionResult::Completed);
    kernel.unwindCalls();
    return static_cast<std::uint32_t>(ExecutionResult::Stopped);
}

extern "C" void enzanExecCount(ExecutionContext* context, std::uint64_t instructions) noexcept {
    ExecutionHelperAccess::count(context->kernel(), instructions);
}

} // namespace enzan::runtime
