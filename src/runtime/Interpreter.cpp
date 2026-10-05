// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Interpreter.h"

#include "codegen/Bytecode.h"
#include "common/Assert.h"
#include "eir/BitOps.h"
#include "runtime/Kernel.h"

namespace enzan::runtime {

namespace bitops = eir::bitops;

Interpreter::Interpreter(Kernel& owner) noexcept : kernel(owner) {}

ExecutionResult Interpreter::run(std::uint32_t processIndex) {
    ProcessState& state = kernel.processes[processIndex];
    const codegen::CompiledProcess& body =
        kernel.designImage->compiledDesign().body(state.bodyIndex);

    // Register and object access are the two innermost operations, so each is a single
    // helper that the compiler can inline into every case below.
    const std::uint32_t frameBase = state.frameOffset;
    const auto reg = [&](std::uint32_t offset, std::uint32_t width) {
        return kernel.frameAt(frameBase + offset, width);
    };
    const auto objectValue = [&](std::uint32_t objectIndex) {
        const std::uint32_t signalIndex = state.objectSignals[objectIndex];
        const Signal& signal = kernel.designImage->signals()[signalIndex];
        return kernel.stateArena.at(signal.wordOffset, signal.bitWidth);
    };

    std::uint32_t pc = state.resumePc;
    state.resumePc = 0;

    while (pc < body.code.size()) {
        const codegen::Instruction& instruction = body.code[pc];
        ++kernel.instructionCount;
        ++pc;

        switch (instruction.opcode) {
        case eir::Opcode::Nop: break;

        case eir::Opcode::Jump: pc = instruction.a; break;

        case eir::Opcode::BranchIf: {
            // An unknown condition takes the false branch. IEEE 1800 defines an `if`
            // on an unknown expression to take the else path, which is the
            // pessimistic reading and the one every simulator implements.
            const eir::Logic truth = bitops::truthValue(reg(instruction.a, instruction.aWidth));
            pc = truth == eir::Logic::One ? instruction.b : instruction.c;
            break;
        }

        case eir::Opcode::Return: return ExecutionResult::Completed;

        case eir::Opcode::LoadConstant:
            bitops::extend(reg(instruction.dst, instruction.dstWidth), body.constant(instruction.a),
                           instruction.isSigned);
            break;

        case eir::Opcode::Move:
            bitops::copy(reg(instruction.dst, instruction.dstWidth),
                         reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::Extend:
        case eir::Opcode::Truncate:
            bitops::extend(reg(instruction.dst, instruction.dstWidth),
                           reg(instruction.a, instruction.aWidth), instruction.isSigned);
            break;

        case eir::Opcode::CoerceTwoState: {
            // Assigning an unknown value into a two-state object converts X and Z to
            // zero, which is what the standard requires and the only place in the
            // engine where information is deliberately discarded.
            const eir::BitRef destination = reg(instruction.dst, instruction.dstWidth);
            const eir::ConstBitRef source = reg(instruction.a, instruction.aWidth);
            for (std::uint32_t bit = 0; bit < destination.width(); ++bit) {
                const eir::Logic value = source.get(bit);
                destination.set(bit, value == eir::Logic::One ? eir::Logic::One : eir::Logic::Zero);
            }
            break;
        }

        case eir::Opcode::ReadObject:
            bitops::copy(reg(instruction.dst, instruction.dstWidth), objectValue(instruction.a));
            break;

        case eir::Opcode::ReadObjectPart: {
            // A window of the object at a run-time offset, read straight from the arena: an
            // element of a memory costs its own width, not the memory's.
            const eir::BitRef destination = reg(instruction.dst, instruction.dstWidth);
            const eir::ConstBitRef offset = reg(instruction.b, instruction.bWidth);
            if (offset.hasUnknown()) {
                bitops::setUnknown(destination);
                break;
            }
            bitops::partSelectAt(destination, objectValue(instruction.a), offset.toUInt64());
            break;
        }

        case eir::Opcode::Store: {
            // A store goes straight into the signal unless what it writes is resolved with
            // other drivers, in which case it goes through this process's driver (section
            // 3.7). One load and a compare on the common path.
            const WriteRoute& route = state.writeRoutes[instruction.dst];
            if (route.direct()) {
                kernel.writeSignal(state.objectSignals[instruction.a],
                                   reg(instruction.b, instruction.bWidth));
            } else {
                kernel.writeThroughDrivers(route, reg(instruction.b, instruction.bWidth), 0);
            }
            break;
        }

        case eir::Opcode::StorePart: {
            const eir::ConstBitRef index = reg(instruction.c, instruction.cWidth);
            if (index.hasUnknown()) {
                // The target of the write is unknown, so nothing may be written.
                break;
            }
            const WriteRoute& route = state.writeRoutes[instruction.dst];
            if (route.direct()) {
                kernel.writeSignalPart(state.objectSignals[instruction.a],
                                       reg(instruction.b, instruction.bWidth), index.toUInt64());
            } else {
                kernel.writeThroughDrivers(route, reg(instruction.b, instruction.bWidth),
                                           index.toUInt64());
            }
            break;
        }

        case eir::Opcode::StoreDelayed:
        case eir::Opcode::StoreNonBlockingDelayed: {
            // An unknown delay is zero, as for `#` (see WaitTime).
            const eir::ConstBitRef delay = reg(instruction.c, instruction.cWidth);
            kernel.scheduleDelayedWrite(
                processIndex, instruction.dst, state.objectSignals[instruction.a],
                reg(instruction.b, instruction.bWidth), delay.hasUnknown() ? 0 : delay.toUInt64(),
                instruction.opcode == eir::Opcode::StoreNonBlockingDelayed);
            if (kernel.errorRequested) return ExecutionResult::Stopped;
            break;
        }

        case eir::Opcode::StoreNonBlocking:
            kernel.scheduleNonBlocking(state.objectSignals[instruction.a],
                                       reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::StorePartNonBlocking: {
            const eir::ConstBitRef index = reg(instruction.c, instruction.cWidth);
            if (index.hasUnknown()) break;
            kernel.scheduleNonBlockingPart(state.objectSignals[instruction.a],
                                           reg(instruction.b, instruction.bWidth),
                                           index.toUInt64());
            break;
        }

        case eir::Opcode::And:
            bitops::bitwiseAnd(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth),
                               reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Or:
            bitops::bitwiseOr(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth),
                              reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Xor:
            bitops::bitwiseXor(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth),
                               reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Xnor:
            bitops::bitwiseXnor(reg(instruction.dst, instruction.dstWidth),
                                reg(instruction.a, instruction.aWidth),
                                reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Not:
            bitops::bitwiseNot(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::ReduceAnd:
            bitops::reduceAnd(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::ReduceOr:
            bitops::reduceOr(reg(instruction.dst, instruction.dstWidth),
                             reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::ReduceXor:
            bitops::reduceXor(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::LogicalAnd:
            bitops::logicalAnd(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth),
                               reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::LogicalOr:
            bitops::logicalOr(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth),
                              reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::LogicalNot:
            bitops::logicalNot(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::Add:
            bitops::add(reg(instruction.dst, instruction.dstWidth),
                        reg(instruction.a, instruction.aWidth),
                        reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Subtract:
            bitops::subtract(reg(instruction.dst, instruction.dstWidth),
                             reg(instruction.a, instruction.aWidth),
                             reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Multiply:
            bitops::multiply(reg(instruction.dst, instruction.dstWidth),
                             reg(instruction.a, instruction.aWidth),
                             reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Divide:
            bitops::divide(reg(instruction.dst, instruction.dstWidth),
                           reg(instruction.a, instruction.aWidth),
                           reg(instruction.b, instruction.bWidth), instruction.isSigned);
            break;

        case eir::Opcode::Modulo:
            bitops::modulo(reg(instruction.dst, instruction.dstWidth),
                           reg(instruction.a, instruction.aWidth),
                           reg(instruction.b, instruction.bWidth), instruction.isSigned);
            break;

        case eir::Opcode::Negate:
            bitops::negate(reg(instruction.dst, instruction.dstWidth),
                           reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::ShiftLeft:
            bitops::shiftLeft(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth),
                              reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::ShiftRightLogical:
            bitops::shiftRightLogical(reg(instruction.dst, instruction.dstWidth),
                                      reg(instruction.a, instruction.aWidth),
                                      reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::ShiftRightArithmetic:
            bitops::shiftRightArithmetic(reg(instruction.dst, instruction.dstWidth),
                                         reg(instruction.a, instruction.aWidth),
                                         reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Equal:
            bitops::equal(reg(instruction.dst, instruction.dstWidth),
                          reg(instruction.a, instruction.aWidth),
                          reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::NotEqual:
            bitops::notEqual(reg(instruction.dst, instruction.dstWidth),
                             reg(instruction.a, instruction.aWidth),
                             reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::CaseEqual:
            bitops::caseEqual(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth),
                              reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::CaseNotEqual:
            bitops::caseNotEqual(reg(instruction.dst, instruction.dstWidth),
                                 reg(instruction.a, instruction.aWidth),
                                 reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::WildcardEqual:
            bitops::wildcardEqual(reg(instruction.dst, instruction.dstWidth),
                                  reg(instruction.a, instruction.aWidth),
                                  reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::WildcardNotEqual:
            bitops::wildcardNotEqual(reg(instruction.dst, instruction.dstWidth),
                                     reg(instruction.a, instruction.aWidth),
                                     reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::CeilLog2:
            bitops::ceilLog2(reg(instruction.dst, instruction.dstWidth),
                             reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::CountOnes:
            bitops::countOnes(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth));
            break;

        case eir::Opcode::Power:
            // The third operand is an immediate saying whether the exponent is signed, which
            // the one signedness flag of an instruction cannot also carry.
            bitops::power(
                reg(instruction.dst, instruction.dstWidth), reg(instruction.a, instruction.aWidth),
                reg(instruction.b, instruction.bWidth), instruction.isSigned, instruction.c != 0);
            break;

        case eir::Opcode::CaseZMatch:
            bitops::caseZMatch(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth),
                               reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::CaseXMatch:
            bitops::caseXMatch(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth),
                               reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::LessThan:
            bitops::lessThan(reg(instruction.dst, instruction.dstWidth),
                             reg(instruction.a, instruction.aWidth),
                             reg(instruction.b, instruction.bWidth), instruction.isSigned);
            break;

        case eir::Opcode::LessEqual:
            bitops::lessEqual(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth),
                              reg(instruction.b, instruction.bWidth), instruction.isSigned);
            break;

        case eir::Opcode::BitSelect:
            bitops::bitSelect(reg(instruction.dst, instruction.dstWidth),
                              reg(instruction.a, instruction.aWidth),
                              reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::PartSelect:
            bitops::partSelect(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth),
                               reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::SplicePart:
            // The destination is read as well as written: this is how the compiler
            // builds a concatenation and how it writes through a part select.
            bitops::assignPart(reg(instruction.dst, instruction.dstWidth),
                               reg(instruction.a, instruction.aWidth),
                               reg(instruction.b, instruction.bWidth));
            break;

        case eir::Opcode::Conditional:
            bitops::conditional(
                reg(instruction.dst, instruction.dstWidth), reg(instruction.a, instruction.aWidth),
                reg(instruction.b, instruction.bWidth), reg(instruction.c, instruction.cWidth));
            break;

        case eir::Opcode::ReadTime: {
            const eir::BitRef destination = reg(instruction.dst, instruction.dstWidth);
            const eir::BitVector value =
                eir::BitVector::fromUInt64(kernel.currentTime, destination.width());
            bitops::copy(destination, value.ref());
            break;
        }

        case eir::Opcode::SysTask:
            kernel.runSystemTask(processIndex, instruction.a);
            if (kernel.finishRequested || kernel.errorRequested) return ExecutionResult::Stopped;
            break;

        case eir::Opcode::WaitTime: {
            const eir::ConstBitRef delay = reg(instruction.a, instruction.aWidth);
            // An unknown delay is treated as zero. IEEE 1800 leaves it
            // implementation-defined; zero keeps the process alive so that the design
            // continues and the X shows up in the waveform, rather than silently
            // deadlocking a testbench.
            const SimTime amount = delay.hasUnknown() ? 0 : delay.toUInt64();
            kernel.suspendForTime(processIndex, amount, instruction.b);
            return kernel.errorRequested ? ExecutionResult::Stopped : ExecutionResult::Suspended;
        }

        case eir::Opcode::WaitEvents:
            kernel.suspendForEvents(processIndex, instruction.a, instruction.b);
            return ExecutionResult::Suspended;

        case eir::Opcode::WaitEdge:
            kernel.suspendForEdge(processIndex, state.objectSignals[instruction.a],
                                  static_cast<eir::EdgeKind>(instruction.b), instruction.c);
            return ExecutionResult::Suspended;
        }
    }

    // Running off the end of the code is possible only if a block's terminator is
    // missing, which the verifier rejects. Treated as completion rather than as a crash,
    // because aborting a simulation over a malformed body would lose the transcript that
    // shows what led to it.
    ENZAN_ASSERT(false, "a process body ran past its last instruction; run the EIR verifier");
    return ExecutionResult::Completed;
}

} // namespace enzan::runtime
