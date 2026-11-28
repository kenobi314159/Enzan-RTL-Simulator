// SPDX-License-Identifier: GPL-3.0-or-later

#include "jit/Backend.h"

#include "jit/Engine.h"

#include "codegen/Bytecode.h"
#include "common/Diagnostics.h"
#include "eir/BitOps.h"
#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "runtime/Execution.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"

#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace enzan::jit {

namespace {

using codegen::CompiledBody;
using codegen::Instruction;
using eir::Opcode;

bool supported(const Instruction& instruction) {
    switch (instruction.opcode) {
    case Opcode::Nop:
    case Opcode::Jump:
    case Opcode::Return:
    case Opcode::Call:
    case Opcode::SysTask:
    case Opcode::WaitTime:
    case Opcode::WaitEdge:
    case Opcode::WaitEvents: return true;
    case Opcode::BranchIf: return instruction.aWidth != 0;
    case Opcode::Present:
    case Opcode::Encode: return instruction.dstWidth != 0 && instruction.aWidth != 0;
    case Opcode::Drive:
    case Opcode::DriveTransport:
    case Opcode::DrivePart: return instruction.bWidth != 0 && instruction.cWidth != 0;
    case Opcode::DrivePartAfter:
    case Opcode::DriveTransportPart: return instruction.bWidth != 0 && instruction.cWidth == 128;
    case Opcode::BitSelect:
        return instruction.dstWidth == 1 && instruction.aWidth != 0 && instruction.bWidth != 0;
    case Opcode::Conditional:
        return instruction.dstWidth != 0 && instruction.aWidth != 0
               && instruction.bWidth == instruction.dstWidth
               && instruction.cWidth == instruction.dstWidth;
    case Opcode::PartSelect:
    case Opcode::SplicePart:
        return instruction.dstWidth != 0 && instruction.aWidth != 0 && instruction.bWidth != 0;
    case Opcode::LoadConstant: return instruction.dstWidth != 0;
    case Opcode::Move:
        return instruction.dstWidth != 0 && instruction.dstWidth == instruction.aWidth;
    case Opcode::Extend:
    case Opcode::Truncate:
        return !instruction.isReal && instruction.dstWidth != 0 && instruction.aWidth != 0;
    case Opcode::CoerceTwoState:
    case Opcode::Not:
        return instruction.dstWidth != 0 && instruction.dstWidth == instruction.aWidth;
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor:
    case Opcode::Xnor:
        return instruction.dstWidth != 0 && instruction.dstWidth == instruction.aWidth
               && instruction.dstWidth == instruction.bWidth;
    case Opcode::ReadObject: return instruction.dstWidth != 0;
    case Opcode::ReadObjectPart: return instruction.dstWidth != 0 && instruction.bWidth != 0;
    case Opcode::ReadEvent:
    case Opcode::ReadActive: return instruction.dstWidth == 1;
    case Opcode::ReadLastValue:
    case Opcode::ReadTime:
    case Opcode::SysFunction: return instruction.dstWidth != 0;
    case Opcode::Store:
    case Opcode::StoreNonBlocking: return instruction.bWidth != 0;
    case Opcode::StorePart:
    case Opcode::StorePartNonBlocking: return instruction.bWidth != 0 && instruction.cWidth != 0;
    case Opcode::Add:
    case Opcode::Subtract:
    case Opcode::Multiply:
        return instruction.dstWidth != 0 && (!instruction.isReal || instruction.dstWidth == 64)
               && instruction.aWidth == instruction.dstWidth
               && instruction.bWidth == instruction.dstWidth;
    case Opcode::Negate:
        return instruction.dstWidth != 0 && instruction.aWidth == instruction.dstWidth
               && (!instruction.isReal || instruction.dstWidth == 64);
    case Opcode::Divide:
    case Opcode::Modulo:
        return instruction.dstWidth != 0 && instruction.aWidth == instruction.dstWidth
               && instruction.bWidth == instruction.dstWidth
               && (!instruction.isReal
                   || (instruction.opcode == Opcode::Divide && instruction.dstWidth == 64));
    case Opcode::ShiftLeft:
    case Opcode::ShiftRightLogical:
    case Opcode::ShiftRightArithmetic:
        return !instruction.isReal && instruction.dstWidth != 0
               && instruction.aWidth == instruction.dstWidth && instruction.bWidth != 0;
    case Opcode::Equal:
    case Opcode::NotEqual:
    case Opcode::CaseEqual:
    case Opcode::CaseNotEqual:
    case Opcode::LessThan:
    case Opcode::LessEqual:
    case Opcode::WildcardEqual:
    case Opcode::WildcardNotEqual:
    case Opcode::CaseZMatch:
    case Opcode::CaseXMatch:
        return instruction.dstWidth == 1 && instruction.aWidth != 0
               && instruction.aWidth == instruction.bWidth
               && (!instruction.isReal || instruction.aWidth == 64);
    default: return false;
    }
}

/// Generated code accounts before an operation can stop, re-enter the kernel, or transfer
/// control. Counting a straight-line run once preserves the logical bytecode count without
/// a helper call for every instruction.
bool endsCountRun(Opcode opcode) {
    switch (opcode) {
    case Opcode::Jump:
    case Opcode::BranchIf:
    case Opcode::Return:
    case Opcode::Call:
    case Opcode::Store:
    case Opcode::StoreNonBlocking:
    case Opcode::StorePart:
    case Opcode::StorePartNonBlocking:
    case Opcode::Drive:
    case Opcode::DriveTransport:
    case Opcode::DrivePart:
    case Opcode::DrivePartAfter:
    case Opcode::DriveTransportPart:
    case Opcode::SysTask:
    case Opcode::SysFunction:
    case Opcode::WaitTime:
    case Opcode::WaitEdge:
    case Opcode::WaitEvents: return true;
    default: return false;
    }
}

struct CountRuns {
    std::vector<bool> leaders;
    std::vector<std::uint64_t> countAt;
};

CountRuns countRuns(const CompiledBody& body) {
    CountRuns result{.leaders = std::vector<bool>(body.code.size(), false),
                     .countAt = std::vector<std::uint64_t>(body.code.size(), 0)};
    if (body.code.empty()) return result;
    result.leaders[0] = true;
    for (std::size_t pc = 0; pc < body.code.size(); ++pc) {
        const auto& instruction = body.code[pc];
        switch (instruction.opcode) {
        case Opcode::Jump: result.leaders[instruction.a] = true; break;
        case Opcode::BranchIf:
            result.leaders[instruction.b] = true;
            result.leaders[instruction.c] = true;
            break;
        case Opcode::WaitTime:
        case Opcode::WaitEvents: result.leaders[instruction.b] = true; break;
        case Opcode::WaitEdge: result.leaders[instruction.c] = true; break;
        default: break;
        }
        if (endsCountRun(instruction.opcode) && pc + 1 < body.code.size())
            result.leaders[pc + 1] = true;
    }
    std::uint64_t count = 0;
    for (std::size_t pc = 0; pc < body.code.size(); ++pc) {
        ++count;
        if (endsCountRun(body.code[pc].opcode) || pc + 1 == body.code.size()
            || result.leaders[pc + 1]) {
            result.countAt[pc] = count;
            count = 0;
        }
    }
    return result;
}

class NativeBackend final : public runtime::ExecutionBackend {
public:
    NativeBackend(std::unique_ptr<Engine> owner, std::vector<NativeBody> entries)
        : engine(std::move(owner)), bodies(std::move(entries)) {}

    runtime::ExecutionResult run(runtime::ExecutionContext& context) override {
        const auto invoke = [&](runtime::ExecutionContext& invocation) {
            const auto index = invocation.activation().bodyIndex;
            if (index >= bodies.size() || bodies[index] == nullptr)
                return static_cast<std::uint32_t>(runtime::ExecutionResult::Stopped);
            return bodies[index](&invocation);
        };
        const auto first = invoke(context);
        if (first != NativeCallRequested) return static_cast<runtime::ExecutionResult>(first);

        runtime::Kernel& kernel = context.kernel();
        runtime::Activation current = context.activation();
        std::uint32_t callIndex = context.callIndex();
        std::uint32_t resume = context.callResumePc();
        std::size_t depth = 0;
        for (;;) {
            const auto callee =
                kernel.enterCall(context.processIndex(), current, callIndex, resume);
            if (!callee.has_value()) return runtime::ExecutionResult::Stopped;
            current = *callee;
            ++depth;
            resume = 0;
            for (;;) {
                auto invocation = context.withActivation(current, resume);
                const auto outcome = invoke(invocation);
                if (outcome == NativeCallRequested) {
                    callIndex = invocation.callIndex();
                    resume = invocation.callResumePc();
                    break;
                }
                if (outcome != static_cast<std::uint32_t>(runtime::ExecutionResult::Completed))
                    return static_cast<runtime::ExecutionResult>(outcome);
                if (depth == 0) return runtime::ExecutionResult::Completed;
                const auto caller = kernel.leaveCall(current);
                if (!caller.has_value()) return runtime::ExecutionResult::Completed;
                current = caller->first;
                resume = caller->second;
                --depth;
            }
        }
    }

    runtime::ExecutionResult call(runtime::ExecutionContext& context) override {
        return run(context);
    }

private:
    std::unique_ptr<Engine> engine;
    std::vector<NativeBody> bodies;
};

/// Emits one native function per body. Branch and resume targets retain their bytecode indices;
/// straight-line instructions share blocks, with a limit on block size for LLVM's code generator.
class Lowerer {
public:
    Lowerer(llvm::LLVMContext& llvmContext, llvm::Module& llvmModule)
        : context(llvmContext), module(llvmModule), builder(llvmContext) {}

    void emit(const CompiledBody& body, std::uint32_t index) {
        const CountRuns counts = countRuns(body);
        auto* ptr = builder.getPtrTy();
        auto* type = llvm::FunctionType::get(builder.getInt32Ty(), {ptr}, false);
        auto* function =
            llvm::Function::Create(type, llvm::Function::ExternalLinkage, name(index), module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", function);
        auto* invalid = llvm::BasicBlock::Create(context, "invalid_resume", function);
        constexpr std::size_t MaxInstructionsPerBlock = 64;
        std::vector<bool> blockLeaders = counts.leaders;
        std::size_t blockSize = 0;
        for (std::size_t pc = 0; pc < body.code.size(); ++pc) {
            if (blockLeaders[pc] || blockSize == MaxInstructionsPerBlock) {
                blockLeaders[pc] = true;
                blockSize = 0;
            }
            ++blockSize;
        }
        std::vector<llvm::BasicBlock*> blocks(body.code.size(), nullptr);
        for (std::size_t pc = 0; pc < body.code.size(); ++pc)
            if (blockLeaders[pc])
                blocks[pc] = llvm::BasicBlock::Create(context, "pc" + std::to_string(pc), function);

        builder.SetInsertPoint(entry);
        auto* argument = function->getArg(0);
        auto resume = helper("enzanExecResumePc", builder.getInt32Ty(), {ptr});
        auto* counterSlot = builder.CreateAlloca(ptr);
        auto frameFunction = helper("enzanExecFrameAndCounter", ptr, {ptr, ptr});
        auto* frame = builder.CreateCall(frameFunction, {argument, counterSlot});
        auto* counter = builder.CreateLoad(ptr, counterSlot);
        auto* branch = builder.CreateSwitch(builder.CreateCall(resume, {argument}), invalid,
                                            static_cast<unsigned>(blocks.size()));
        for (std::size_t pc = 0; pc < blocks.size(); ++pc)
            if (counts.leaders[pc])
                branch->addCase(builder.getInt32(static_cast<std::uint32_t>(pc)), blocks[pc]);
        builder.SetInsertPoint(invalid);
        builder.CreateRet(result(runtime::ExecutionResult::Stopped));

        for (std::size_t pc = 0; pc < body.code.size(); ++pc) {
            if (blockLeaders[pc]) builder.SetInsertPoint(blocks[pc]);
            const Instruction& op = body.code[pc];
            if (counts.countAt[pc] != 0) count(counter, counts.countAt[pc]);
            const auto next = [&] {
                if (blockLeaders[pc + 1]) builder.CreateBr(blocks[pc + 1]);
            };
            switch (op.opcode) {
            case Opcode::Nop: next(); break;
            case Opcode::Jump: builder.CreateBr(blocks[op.a]); break;
            case Opcode::BranchIf: {
                llvm::Value* any = builder.getInt64(0);
                const auto words = eir::wordsFor(op.aWidth);
                for (std::uint32_t word = 0; word < words; ++word) {
                    auto* aval = load(frame, op.a + word);
                    auto* bval = load(frame, op.a + words + word);
                    any = builder.CreateOr(any, builder.CreateAnd(aval, builder.CreateNot(bval)));
                }
                builder.CreateCondBr(builder.CreateICmpNE(any, builder.getInt64(0)), blocks[op.b],
                                     blocks[op.c]);
                break;
            }
            case Opcode::Return:
                builder.CreateRet(result(runtime::ExecutionResult::Completed));
                break;
            case Opcode::Call: {
                auto request = helper("enzanExecRequestCall", builder.getVoidTy(),
                                      {ptr, builder.getInt32Ty(), builder.getInt32Ty()});
                builder.CreateCall(request, {argument, builder.getInt32(op.a),
                                             builder.getInt32(static_cast<std::uint32_t>(pc + 1))});
                builder.CreateRet(builder.getInt32(NativeCallRequested));
                break;
            }
            case Opcode::LoadConstant: {
                eir::BitVector value = eir::BitVector::filled(op.dstWidth, eir::Logic::Zero);
                eir::bitops::extend(value.mutableRef(), body.constant(op.a), op.isSigned);
                const auto bits = value.ref();
                for (std::uint32_t word = 0; word < bits.words(); ++word) {
                    write(frame, op.dst + word, builder.getInt64(bits.aval()[word]));
                    write(frame, op.dst + bits.words() + word, builder.getInt64(bits.bval()[word]));
                }
                next();
                break;
            }
            case Opcode::Move:
                copy(frame, op.dst, frame, op.a, eir::storageWordsFor(op.dstWidth));
                next();
                break;
            case Opcode::Extend:
            case Opcode::Truncate: {
                if (op.dstWidth <= 64 && op.aWidth <= 64) {
                    auto* aval = load(frame, op.a);
                    auto* bval = load(frame, op.a + 1);
                    auto extendPlane = [&](llvm::Value* value) -> llvm::Value* {
                        if (op.isSigned && op.dstWidth > op.aWidth) {
                            auto* sign = builder.CreateAnd(builder.CreateLShr(value, op.aWidth - 1),
                                                           builder.getInt64(1));
                            auto* extension = builder.CreateSelect(
                                builder.CreateICmpNE(sign, builder.getInt64(0)),
                                builder.getInt64(~eir::topWordMask(op.aWidth)),
                                builder.getInt64(0));
                            value = builder.CreateOr(value, extension);
                        }
                        return builder.CreateAnd(value,
                                                 builder.getInt64(eir::topWordMask(op.dstWidth)));
                    };
                    aval = extendPlane(aval);
                    bval = extendPlane(bval);
                    write(frame, op.dst, aval);
                    write(frame, op.dst + 1, bval);
                } else {
                    auto extend =
                        helper("enzanExecExtend", builder.getVoidTy(),
                               {ptr, builder.getInt32Ty(), builder.getInt32Ty(),
                                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()});
                    builder.CreateCall(extend, {argument, builder.getInt32(op.dst),
                                                builder.getInt32(op.dstWidth),
                                                builder.getInt32(op.a), builder.getInt32(op.aWidth),
                                                builder.getInt32(op.isSigned ? 1 : 0)});
                }
                next();
                break;
            }
            case Opcode::CoerceTwoState:
            case Opcode::Not:
            case Opcode::And:
            case Opcode::Or:
            case Opcode::Xor:
            case Opcode::Xnor: {
                const auto words = eir::wordsFor(op.dstWidth);
                for (std::uint32_t word = 0; word < words; ++word) {
                    auto* left = load(frame, op.a + word);
                    auto* leftUnknown = load(frame, op.a + words + word);
                    llvm::Value* one = nullptr;
                    llvm::Value* unknown = nullptr;
                    if (op.opcode == Opcode::CoerceTwoState) {
                        one = builder.CreateAnd(left, builder.CreateNot(leftUnknown));
                        unknown = builder.getInt64(0);
                    } else if (op.opcode == Opcode::Not) {
                        one = builder.CreateAnd(builder.CreateNot(left),
                                                builder.CreateNot(leftUnknown));
                        unknown = leftUnknown;
                    } else {
                        auto* right = load(frame, op.b + word);
                        auto* rightUnknown = load(frame, op.b + words + word);
                        if (op.opcode == Opcode::Xor || op.opcode == Opcode::Xnor) {
                            unknown = builder.CreateOr(leftUnknown, rightUnknown);
                            auto* difference = builder.CreateXor(left, right);
                            if (op.opcode == Opcode::Xnor)
                                difference = builder.CreateNot(difference);
                            one = builder.CreateAnd(difference, builder.CreateNot(unknown));
                        } else {
                            auto* leftOne = builder.CreateAnd(left, builder.CreateNot(leftUnknown));
                            auto* rightOne =
                                builder.CreateAnd(right, builder.CreateNot(rightUnknown));
                            auto* leftZero = builder.CreateAnd(builder.CreateNot(left),
                                                               builder.CreateNot(leftUnknown));
                            auto* rightZero = builder.CreateAnd(builder.CreateNot(right),
                                                                builder.CreateNot(rightUnknown));
                            one = op.opcode == Opcode::And ? builder.CreateAnd(leftOne, rightOne)
                                                           : builder.CreateOr(leftOne, rightOne);
                            auto* zero = op.opcode == Opcode::And
                                             ? builder.CreateOr(leftZero, rightZero)
                                             : builder.CreateAnd(leftZero, rightZero);
                            unknown = builder.CreateNot(builder.CreateOr(one, zero));
                        }
                    }
                    auto* mask = builder.getInt64(word + 1 == words ? eir::topWordMask(op.dstWidth)
                                                                    : ~eir::Word{0});
                    write(frame, op.dst + word,
                          builder.CreateAnd(builder.CreateOr(one, unknown), mask));
                    write(frame, op.dst + words + word, builder.CreateAnd(unknown, mask));
                }
                next();
                break;
            }
            case Opcode::Present:
            case Opcode::Encode: {
                auto* i32 = builder.getInt32Ty();
                if (op.opcode == Opcode::Present) {
                    auto present = helper("enzanExecPresent", builder.getVoidTy(),
                                          {ptr, i32, i32, i32, i32, i32});
                    builder.CreateCall(present,
                                       {argument, builder.getInt32(op.dst),
                                        builder.getInt32(op.dstWidth), builder.getInt32(op.a),
                                        builder.getInt32(op.aWidth), builder.getInt32(op.b)});
                } else {
                    auto encode = helper("enzanExecEncode", builder.getVoidTy(),
                                         {ptr, i32, i32, i32, i32, i32, i32});
                    builder.CreateCall(encode, {argument, builder.getInt32(op.dst),
                                                builder.getInt32(op.dstWidth),
                                                builder.getInt32(op.a), builder.getInt32(op.aWidth),
                                                builder.getInt32(op.b), builder.getInt32(op.c)});
                }
                next();
                break;
            }
            case Opcode::BitSelect:
            case Opcode::PartSelect:
            case Opcode::SplicePart: {
                if (op.opcode != Opcode::SplicePart && op.dstWidth <= 64 && op.aWidth <= 64
                    && op.bWidth <= 64) {
                    auto* position = load(frame, op.b);
                    auto* known = builder.CreateICmpEQ(load(frame, op.b + 1), builder.getInt64(0));
                    auto* inRange = builder.CreateICmpULT(position, builder.getInt64(op.aWidth));
                    auto* valid = builder.CreateAnd(known, inRange);
                    // LLVM shifts by the word size or more are poison even on the unused path.
                    auto* shift = builder.CreateSelect(valid, position, builder.getInt64(0));
                    auto* selectedMask = builder.CreateAnd(
                        builder.CreateLShr(builder.getInt64(eir::topWordMask(op.aWidth)), shift),
                        builder.getInt64(eir::topWordMask(op.dstWidth)));
                    auto* mask = builder.CreateSelect(valid, selectedMask, builder.getInt64(0));
                    auto* missing =
                        builder.CreateXor(mask, builder.getInt64(eir::topWordMask(op.dstWidth)));
                    auto selectPlane = [&](std::uint32_t offset) {
                        auto* selected =
                            builder.CreateAnd(builder.CreateLShr(load(frame, offset), shift), mask);
                        return builder.CreateOr(selected, missing);
                    };
                    auto* aval = selectPlane(op.a);
                    auto* bval = selectPlane(op.a + 1);
                    write(frame, op.dst, aval);
                    write(frame, op.dst + 1, bval);
                } else {
                    const char* name = "enzanExecSplicePart";
                    if (op.opcode == Opcode::BitSelect) name = "enzanExecBitSelect";
                    if (op.opcode == Opcode::PartSelect) name = "enzanExecPartSelect";
                    auto* i32 = builder.getInt32Ty();
                    auto select =
                        helper(name, builder.getVoidTy(), {ptr, i32, i32, i32, i32, i32, i32});
                    builder.CreateCall(
                        select, {argument, builder.getInt32(op.dst), builder.getInt32(op.dstWidth),
                                 builder.getInt32(op.a), builder.getInt32(op.aWidth),
                                 builder.getInt32(op.b), builder.getInt32(op.bWidth)});
                }
                next();
                break;
            }
            case Opcode::Conditional: {
                const std::uint32_t conditionWords = eir::wordsFor(op.aWidth);
                llvm::Value* knownOnes = builder.getInt64(0);
                llvm::Value* unknownBits = builder.getInt64(0);
                for (std::uint32_t word = 0; word < conditionWords; ++word) {
                    auto* aval = load(frame, op.a + word);
                    auto* bval = load(frame, op.a + conditionWords + word);
                    knownOnes = builder.CreateOr(knownOnes,
                                                 builder.CreateAnd(aval, builder.CreateNot(bval)));
                    unknownBits = builder.CreateOr(unknownBits, bval);
                }
                auto* isTrue = builder.CreateICmpNE(knownOnes, builder.getInt64(0));
                auto* hasUnknown = builder.CreateICmpNE(unknownBits, builder.getInt64(0));
                const std::uint32_t words = eir::wordsFor(op.dstWidth);
                for (std::uint32_t word = 0; word < words; ++word) {
                    auto* trueAval = load(frame, op.b + word);
                    auto* trueBval = load(frame, op.b + words + word);
                    auto* falseAval = load(frame, op.c + word);
                    auto* falseBval = load(frame, op.c + words + word);
                    auto* different = builder.CreateOr(builder.CreateXor(trueAval, falseAval),
                                                       builder.CreateXor(trueBval, falseBval));
                    auto selectPlane = [&](llvm::Value* whenTrue, llvm::Value* whenFalse) {
                        auto* merged = builder.CreateOr(
                            builder.CreateAnd(whenTrue, builder.CreateNot(different)), different);
                        return builder.CreateSelect(
                            isTrue, whenTrue, builder.CreateSelect(hasUnknown, merged, whenFalse));
                    };
                    llvm::Value* aval = selectPlane(trueAval, falseAval);
                    llvm::Value* bval = selectPlane(trueBval, falseBval);
                    if (word + 1 == words) {
                        auto* mask = builder.getInt64(eir::topWordMask(op.dstWidth));
                        aval = builder.CreateAnd(aval, mask);
                        bval = builder.CreateAnd(bval, mask);
                    }
                    write(frame, op.dst + word, aval);
                    write(frame, op.dst + words + word, bval);
                }
                next();
                break;
            }
            case Opcode::ReadObject: {
                auto read = helper("enzanExecReadObject", ptr, {ptr, builder.getInt32Ty()});
                auto* source = builder.CreateCall(read, {argument, builder.getInt32(op.a)});
                copy(frame, op.dst, source, 0, eir::storageWordsFor(op.dstWidth));
                next();
                break;
            }
            case Opcode::ReadObjectPart: {
                auto* i32 = builder.getInt32Ty();
                auto read = helper("enzanExecReadObjectPart", builder.getVoidTy(),
                                   {ptr, i32, i32, i32, i32, i32});
                builder.CreateCall(read, {argument, builder.getInt32(op.dst),
                                          builder.getInt32(op.dstWidth), builder.getInt32(op.a),
                                          builder.getInt32(op.b), builder.getInt32(op.bWidth)});
                next();
                break;
            }
            case Opcode::ReadEvent:
            case Opcode::ReadActive: {
                auto* i32 = builder.getInt32Ty();
                auto attribute = helper("enzanExecSignalAttribute", i32, {ptr, i32, i32});
                auto* value = builder.CreateCall(
                    attribute, {argument, builder.getInt32(op.a),
                                builder.getInt32(op.opcode == Opcode::ReadActive ? 1 : 0)});
                write(frame, op.dst, builder.CreateZExt(value, builder.getInt64Ty()));
                write(frame, op.dst + 1, builder.getInt64(0));
                next();
                break;
            }
            case Opcode::ReadLastValue: {
                auto last = helper("enzanExecLastValue", ptr, {ptr, builder.getInt32Ty()});
                auto* source = builder.CreateCall(last, {argument, builder.getInt32(op.a)});
                copy(frame, op.dst, source, 0, eir::storageWordsFor(op.dstWidth));
                next();
                break;
            }
            case Opcode::ReadTime: {
                auto time = helper("enzanExecTime", builder.getInt64Ty(), {ptr});
                llvm::Value* value = builder.CreateCall(time, {argument});
                if (op.dstWidth < 64)
                    value =
                        builder.CreateAnd(value, builder.getInt64(eir::topWordMask(op.dstWidth)));
                const std::uint32_t words = eir::wordsFor(op.dstWidth);
                write(frame, op.dst, value);
                for (std::uint32_t word = 1; word < words; ++word)
                    write(frame, op.dst + word, builder.getInt64(0));
                for (std::uint32_t word = 0; word < words; ++word)
                    write(frame, op.dst + words + word, builder.getInt64(0));
                next();
                break;
            }
            case Opcode::Store: {
                auto store = helper("enzanExecStore", builder.getVoidTy(),
                                    {ptr, builder.getInt32Ty(), builder.getInt32Ty(),
                                     builder.getInt32Ty(), builder.getInt32Ty()});
                builder.CreateCall(store,
                                   {argument, builder.getInt32(op.a), builder.getInt32(op.dst),
                                    builder.getInt32(op.b), builder.getInt32(op.bWidth)});
                next();
                break;
            }
            case Opcode::StoreNonBlocking: {
                auto store =
                    helper("enzanExecStoreNonBlocking", builder.getVoidTy(),
                           {ptr, builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()});
                builder.CreateCall(store, {argument, builder.getInt32(op.a), builder.getInt32(op.b),
                                           builder.getInt32(op.bWidth)});
                next();
                break;
            }
            case Opcode::StorePart:
            case Opcode::StorePartNonBlocking: {
                auto* i32 = builder.getInt32Ty();
                auto store = helper("enzanExecStorePart", builder.getVoidTy(),
                                    {ptr, i32, i32, i32, i32, i32, i32, i32});
                builder.CreateCall(
                    store, {argument, builder.getInt32(op.a), builder.getInt32(op.dst),
                            builder.getInt32(op.b), builder.getInt32(op.bWidth),
                            builder.getInt32(op.c), builder.getInt32(op.cWidth),
                            builder.getInt32(op.opcode == Opcode::StorePartNonBlocking ? 1 : 0)});
                next();
                break;
            }
            case Opcode::Drive:
            case Opcode::DriveTransport: {
                auto* i32 = builder.getInt32Ty();
                auto drive =
                    helper("enzanExecDrive", i32, {ptr, i32, i32, i32, i32, i32, i32, i32});
                auto* outcome = builder.CreateCall(
                    drive, {argument, builder.getInt32(op.a), builder.getInt32(op.dst),
                            builder.getInt32(op.b), builder.getInt32(op.bWidth),
                            builder.getInt32(op.c), builder.getInt32(op.cWidth),
                            builder.getInt32(op.opcode == Opcode::Drive ? 1 : 0)});
                auto* stopped = llvm::BasicBlock::Create(context, "drive_stop", function);
                builder.CreateCondBr(
                    builder.CreateICmpEQ(outcome, result(runtime::ExecutionResult::Completed)),
                    blocks[pc + 1], stopped);
                builder.SetInsertPoint(stopped);
                builder.CreateRet(outcome);
                break;
            }
            case Opcode::DrivePart:
            case Opcode::DrivePartAfter:
            case Opcode::DriveTransportPart: {
                auto* i32 = builder.getInt32Ty();
                auto drive =
                    helper("enzanExecDrivePart", i32, {ptr, i32, i32, i32, i32, i32, i32, i32});
                auto* outcome = builder.CreateCall(
                    drive, {argument, builder.getInt32(op.a), builder.getInt32(op.dst),
                            builder.getInt32(op.b), builder.getInt32(op.bWidth),
                            builder.getInt32(op.c), builder.getInt32(op.cWidth),
                            builder.getInt32(static_cast<std::uint32_t>(op.opcode))});
                auto* stopped = llvm::BasicBlock::Create(context, "drive_part_stop", function);
                builder.CreateCondBr(
                    builder.CreateICmpEQ(outcome, result(runtime::ExecutionResult::Completed)),
                    blocks[pc + 1], stopped);
                builder.SetInsertPoint(stopped);
                builder.CreateRet(outcome);
                break;
            }
            case Opcode::Add:
            case Opcode::Subtract:
            case Opcode::Multiply: {
                if (op.isReal) {
                    auto* left = builder.CreateBitCast(load(frame, op.a), builder.getDoubleTy());
                    auto* right = builder.CreateBitCast(load(frame, op.b), builder.getDoubleTy());
                    llvm::Value* value = nullptr;
                    if (op.opcode == Opcode::Add)
                        value = builder.CreateFAdd(left, right);
                    else if (op.opcode == Opcode::Subtract)
                        value = builder.CreateFSub(left, right);
                    else
                        value = builder.CreateFMul(left, right);
                    write(frame, op.dst, builder.CreateBitCast(value, builder.getInt64Ty()));
                    write(frame, op.dst + 1, builder.getInt64(0));
                    next();
                    break;
                }
                if (op.dstWidth > 64) {
                    auto* i32 = builder.getInt32Ty();
                    auto arithmetic = helper("enzanExecWideArithmetic", builder.getVoidTy(),
                                             {ptr, i32, i32, i32, i32, i32});
                    builder.CreateCall(arithmetic,
                                       {argument, builder.getInt32(op.dst), builder.getInt32(op.a),
                                        builder.getInt32(op.b), builder.getInt32(op.dstWidth),
                                        builder.getInt32(static_cast<std::uint32_t>(op.opcode))});
                    next();
                    break;
                }
                auto* left = load(frame, op.a);
                auto* right = load(frame, op.b);
                auto* unknown = builder.CreateOr(load(frame, op.a + 1), load(frame, op.b + 1));
                auto* hasUnknown = builder.CreateICmpNE(unknown, builder.getInt64(0));
                llvm::Value* value = nullptr;
                if (op.opcode == Opcode::Add)
                    value = builder.CreateAdd(left, right);
                else if (op.opcode == Opcode::Subtract)
                    value = builder.CreateSub(left, right);
                else
                    value = builder.CreateMul(left, right);
                auto* mask = builder.getInt64(eir::topWordMask(op.dstWidth));
                value = builder.CreateAnd(value, mask);
                write(frame, op.dst, builder.CreateSelect(hasUnknown, mask, value));
                write(frame, op.dst + 1,
                      builder.CreateSelect(hasUnknown, mask, builder.getInt64(0)));
                next();
                break;
            }
            case Opcode::Negate: {
                if (op.isReal) {
                    auto* value = builder.CreateBitCast(load(frame, op.a), builder.getDoubleTy());
                    write(frame, op.dst,
                          builder.CreateBitCast(builder.CreateFNeg(value), builder.getInt64Ty()));
                    write(frame, op.dst + 1, builder.getInt64(0));
                } else if (op.dstWidth > 64) {
                    auto* i32 = builder.getInt32Ty();
                    auto arithmetic = helper("enzanExecWideArithmetic", builder.getVoidTy(),
                                             {ptr, i32, i32, i32, i32, i32});
                    builder.CreateCall(arithmetic,
                                       {argument, builder.getInt32(op.dst), builder.getInt32(op.a),
                                        builder.getInt32(0), builder.getInt32(op.dstWidth),
                                        builder.getInt32(static_cast<std::uint32_t>(op.opcode))});
                } else {
                    auto* hasUnknown =
                        builder.CreateICmpNE(load(frame, op.a + 1), builder.getInt64(0));
                    auto* mask = builder.getInt64(eir::topWordMask(op.dstWidth));
                    auto* value = builder.CreateAnd(builder.CreateNeg(load(frame, op.a)), mask);
                    write(frame, op.dst, builder.CreateSelect(hasUnknown, mask, value));
                    write(frame, op.dst + 1,
                          builder.CreateSelect(hasUnknown, mask, builder.getInt64(0)));
                }
                next();
                break;
            }
            case Opcode::Divide:
            case Opcode::Modulo: {
                if (op.isReal) {
                    auto* left = builder.CreateBitCast(load(frame, op.a), builder.getDoubleTy());
                    auto* right = builder.CreateBitCast(load(frame, op.b), builder.getDoubleTy());
                    auto* quotient = builder.CreateFDiv(left, right);
                    write(frame, op.dst, builder.CreateBitCast(quotient, builder.getInt64Ty()));
                    write(frame, op.dst + 1, builder.getInt64(0));
                    next();
                    break;
                }
                if (op.dstWidth > 64) {
                    auto* i32 = builder.getInt32Ty();
                    auto divide = helper("enzanExecDivRem", builder.getVoidTy(),
                                         {ptr, i32, i32, i32, i32, i32, i32});
                    builder.CreateCall(divide,
                                       {argument, builder.getInt32(op.dst), builder.getInt32(op.a),
                                        builder.getInt32(op.b), builder.getInt32(op.dstWidth),
                                        builder.getInt32(op.isSigned ? 1 : 0),
                                        builder.getInt32(op.opcode == Opcode::Modulo ? 1 : 0)});
                    next();
                    break;
                }
                auto* left = load(frame, op.a);
                auto* right = load(frame, op.b);
                auto* unknown = builder.CreateOr(load(frame, op.a + 1), load(frame, op.b + 1));
                auto* undefinedResult =
                    builder.CreateOr(builder.CreateICmpNE(unknown, builder.getInt64(0)),
                                     builder.CreateICmpEQ(right, builder.getInt64(0)));
                auto* mask = builder.getInt64(eir::topWordMask(op.dstWidth));
                llvm::Value* leftNegative = builder.getFalse();
                llvm::Value* rightNegative = builder.getFalse();
                llvm::Value* leftMagnitude = left;
                llvm::Value* rightMagnitude = right;
                if (op.isSigned) {
                    auto sign = [&](llvm::Value* value) {
                        return builder.CreateICmpNE(
                            builder.CreateAnd(builder.CreateLShr(value, op.dstWidth - 1),
                                              builder.getInt64(1)),
                            builder.getInt64(0));
                    };
                    auto magnitude = [&](llvm::Value* value, llvm::Value* negative) {
                        auto* negated = builder.CreateAnd(builder.CreateNeg(value), mask);
                        return builder.CreateSelect(negative, negated, value);
                    };
                    leftNegative = sign(left);
                    rightNegative = sign(right);
                    leftMagnitude = magnitude(left, leftNegative);
                    rightMagnitude = magnitude(right, rightNegative);
                }
                // LLVM division by zero is poison, including on a later discarded path.
                auto* safeDivisor =
                    builder.CreateSelect(undefinedResult, builder.getInt64(1), rightMagnitude);
                llvm::Value* value = op.opcode == Opcode::Modulo
                                         ? builder.CreateURem(leftMagnitude, safeDivisor)
                                         : builder.CreateUDiv(leftMagnitude, safeDivisor);
                if (op.isSigned) {
                    auto* negative = op.opcode == Opcode::Modulo
                                         ? leftNegative
                                         : builder.CreateXor(leftNegative, rightNegative);
                    value = builder.CreateSelect(negative, builder.CreateNeg(value), value);
                }
                value = builder.CreateAnd(value, mask);
                write(frame, op.dst, builder.CreateSelect(undefinedResult, mask, value));
                write(frame, op.dst + 1,
                      builder.CreateSelect(undefinedResult, mask, builder.getInt64(0)));
                next();
                break;
            }
            case Opcode::Equal:
            case Opcode::NotEqual:
            case Opcode::CaseEqual:
            case Opcode::CaseNotEqual:
            case Opcode::LessThan:
            case Opcode::LessEqual:
            case Opcode::WildcardEqual:
            case Opcode::WildcardNotEqual:
            case Opcode::CaseZMatch:
            case Opcode::CaseXMatch: {
                const bool realComparison =
                    op.isReal
                    && (op.opcode == Opcode::Equal || op.opcode == Opcode::NotEqual
                        || op.opcode == Opcode::LessThan || op.opcode == Opcode::LessEqual);
                if (realComparison) {
                    auto* left = builder.CreateBitCast(load(frame, op.a), builder.getDoubleTy());
                    auto* right = builder.CreateBitCast(load(frame, op.b), builder.getDoubleTy());
                    llvm::Value* value = nullptr;
                    switch (op.opcode) {
                    case Opcode::Equal: value = builder.CreateFCmpOEQ(left, right); break;
                    case Opcode::NotEqual: value = builder.CreateFCmpUNE(left, right); break;
                    case Opcode::LessThan: value = builder.CreateFCmpOLT(left, right); break;
                    case Opcode::LessEqual: value = builder.CreateFCmpOLE(left, right); break;
                    default: break;
                    }
                    write(frame, op.dst, builder.CreateZExt(value, builder.getInt64Ty()));
                    write(frame, op.dst + 1, builder.getInt64(0));
                    next();
                    break;
                }
                if (op.aWidth > 64 || op.opcode == Opcode::WildcardEqual
                    || op.opcode == Opcode::WildcardNotEqual || op.opcode == Opcode::CaseZMatch
                    || op.opcode == Opcode::CaseXMatch) {
                    auto* i32 = builder.getInt32Ty();
                    auto compare = helper("enzanExecCompare", builder.getVoidTy(),
                                          {ptr, i32, i32, i32, i32, i32, i32});
                    builder.CreateCall(compare,
                                       {argument, builder.getInt32(op.dst), builder.getInt32(op.a),
                                        builder.getInt32(op.b), builder.getInt32(op.aWidth),
                                        builder.getInt32(static_cast<std::uint32_t>(op.opcode)),
                                        builder.getInt32(op.isSigned ? 1 : 0)});
                    next();
                    break;
                }
                auto* left = load(frame, op.a);
                auto* right = load(frame, op.b);
                auto* leftUnknown = load(frame, op.a + 1);
                auto* rightUnknown = load(frame, op.b + 1);
                auto* unknownBits = builder.CreateOr(leftUnknown, rightUnknown);
                auto* hasUnknown = builder.CreateICmpNE(unknownBits, builder.getInt64(0));
                llvm::Value* one = nullptr;
                llvm::Value* unknown = builder.getFalse();
                if (op.opcode == Opcode::CaseEqual || op.opcode == Opcode::CaseNotEqual) {
                    one = builder.CreateAnd(builder.CreateICmpEQ(left, right),
                                            builder.CreateICmpEQ(leftUnknown, rightUnknown));
                    if (op.opcode == Opcode::CaseNotEqual) one = builder.CreateNot(one);
                } else if (op.opcode == Opcode::Equal || op.opcode == Opcode::NotEqual) {
                    auto* knownDifference = builder.CreateAnd(builder.CreateXor(left, right),
                                                              builder.CreateNot(unknownBits));
                    auto* mismatch = builder.CreateICmpNE(knownDifference, builder.getInt64(0));
                    unknown = builder.CreateAnd(builder.CreateNot(mismatch), hasUnknown);
                    one = op.opcode == Opcode::Equal
                              ? builder.CreateAnd(builder.CreateNot(mismatch),
                                                  builder.CreateNot(hasUnknown))
                              : mismatch;
                } else {
                    if (op.isSigned) {
                        const auto shift = 64U - op.aWidth;
                        left = builder.CreateAShr(builder.CreateShl(left, shift), shift);
                        right = builder.CreateAShr(builder.CreateShl(right, shift), shift);
                    }
                    if (op.opcode == Opcode::LessThan)
                        one = op.isSigned ? builder.CreateICmpSLT(left, right)
                                          : builder.CreateICmpULT(left, right);
                    else
                        one = op.isSigned ? builder.CreateICmpSLE(left, right)
                                          : builder.CreateICmpULE(left, right);
                    unknown = hasUnknown;
                }
                write(frame, op.dst,
                      builder.CreateZExt(builder.CreateOr(one, unknown), builder.getInt64Ty()));
                write(frame, op.dst + 1, builder.CreateZExt(unknown, builder.getInt64Ty()));
                next();
                break;
            }
            case Opcode::ShiftLeft:
            case Opcode::ShiftRightLogical:
            case Opcode::ShiftRightArithmetic: {
                if (op.dstWidth > 64 || op.bWidth > 64) {
                    auto* i32 = builder.getInt32Ty();
                    auto shift = helper("enzanExecWideShift", builder.getVoidTy(),
                                        {ptr, i32, i32, i32, i32, i32, i32});
                    builder.CreateCall(shift,
                                       {argument, builder.getInt32(op.dst), builder.getInt32(op.a),
                                        builder.getInt32(op.dstWidth), builder.getInt32(op.b),
                                        builder.getInt32(op.bWidth),
                                        builder.getInt32(static_cast<std::uint32_t>(op.opcode))});
                    next();
                    break;
                }
                auto* aval = load(frame, op.a);
                auto* bval = load(frame, op.a + 1);
                auto* amount = load(frame, op.b);
                auto* unknown = builder.CreateICmpNE(load(frame, op.b + 1), builder.getInt64(0));
                auto* tooLarge = builder.CreateICmpUGE(amount, builder.getInt64(op.dstWidth));
                // LLVM shifts by the word width or more are poison, even if a later select
                // discards them. Clamp first, then select the language-defined result.
                auto* safeAmount = builder.CreateSelect(tooLarge, builder.getInt64(0), amount);
                auto* mask = builder.getInt64(eir::topWordMask(op.dstWidth));
                auto shiftPlane = [&](llvm::Value* value) -> llvm::Value* {
                    llvm::Value* shifted = nullptr;
                    llvm::Value* outside = builder.getInt64(0);
                    if (op.opcode == Opcode::ShiftLeft) {
                        shifted = builder.CreateShl(value, safeAmount);
                    } else if (op.opcode == Opcode::ShiftRightLogical) {
                        shifted = builder.CreateLShr(value, safeAmount);
                    } else {
                        const auto signShift = 64U - op.dstWidth;
                        auto* signedValue =
                            builder.CreateAShr(builder.CreateShl(value, signShift), signShift);
                        shifted = builder.CreateAShr(signedValue, safeAmount);
                        outside = builder.CreateAShr(signedValue, 63);
                    }
                    auto* inRange = builder.CreateSelect(tooLarge, outside, shifted);
                    return builder.CreateSelect(unknown, mask, builder.CreateAnd(inRange, mask));
                };
                auto* resultA = shiftPlane(aval);
                auto* resultB = shiftPlane(bval);
                write(frame, op.dst, resultA);
                write(frame, op.dst + 1, resultB);
                next();
                break;
            }
            case Opcode::SysTask: {
                auto task = helper("enzanExecSystemTask", builder.getInt32Ty(),
                                   {ptr, builder.getInt32Ty()});
                auto* outcome = builder.CreateCall(task, {argument, builder.getInt32(op.a)});
                auto* stopped = llvm::BasicBlock::Create(context, "task_stop", function);
                builder.CreateCondBr(
                    builder.CreateICmpEQ(outcome, result(runtime::ExecutionResult::Completed)),
                    blocks[pc + 1], stopped);
                builder.SetInsertPoint(stopped);
                builder.CreateRet(outcome);
                break;
            }
            case Opcode::SysFunction: {
                auto* i32 = builder.getInt32Ty();
                auto functionCall = helper("enzanExecSystemFunction", i32, {ptr, i32, i32, i32});
                auto* outcome = builder.CreateCall(functionCall, {argument, builder.getInt32(op.a),
                                                                  builder.getInt32(op.dst),
                                                                  builder.getInt32(op.dstWidth)});
                auto* stopped = llvm::BasicBlock::Create(context, "function_stop", function);
                builder.CreateCondBr(
                    builder.CreateICmpEQ(outcome, result(runtime::ExecutionResult::Completed)),
                    blocks[pc + 1], stopped);
                builder.SetInsertPoint(stopped);
                builder.CreateRet(outcome);
                break;
            }
            case Opcode::WaitTime: {
                auto wait =
                    helper("enzanExecWaitTime", builder.getInt32Ty(),
                           {ptr, builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()});
                builder.CreateRet(builder.CreateCall(wait, {argument, builder.getInt32(op.a),
                                                            builder.getInt32(op.aWidth),
                                                            builder.getInt32(op.b)}));
                break;
            }
            case Opcode::WaitEdge: {
                auto wait =
                    helper("enzanExecWaitEdge", builder.getInt32Ty(),
                           {ptr, builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()});
                builder.CreateRet(
                    builder.CreateCall(wait, {argument, builder.getInt32(op.a),
                                              builder.getInt32(op.b), builder.getInt32(op.c)}));
                break;
            }
            case Opcode::WaitEvents: {
                auto wait = helper("enzanExecWaitEvents", builder.getInt32Ty(),
                                   {ptr, builder.getInt32Ty(), builder.getInt32Ty()});
                builder.CreateRet(builder.CreateCall(
                    wait, {argument, builder.getInt32(op.a), builder.getInt32(op.b)}));
                break;
            }
            default: break; // Preparation refuses this before emitting any module.
            }
        }
    }

    static std::string name(std::uint32_t index) { return std::format("enzan_body_{}", index); }

private:
    llvm::FunctionCallee helper(const char* name, llvm::Type* result,
                                std::initializer_list<llvm::Type*> args) {
        return module.getOrInsertFunction(name, llvm::FunctionType::get(result, args, false));
    }

    llvm::Value* pointer(llvm::Value* base, std::uint32_t offset) {
        return builder.CreateConstGEP1_32(builder.getInt64Ty(), base, offset);
    }

    llvm::Value* load(llvm::Value* base, std::uint32_t offset) {
        return builder.CreateLoad(builder.getInt64Ty(), pointer(base, offset));
    }

    void write(llvm::Value* base, std::uint32_t offset, llvm::Value* value) {
        builder.CreateStore(value, pointer(base, offset));
    }

    void copy(llvm::Value* dst, std::uint32_t destination, llvm::Value* src, std::uint32_t source,
              std::uint32_t words) {
        for (std::uint32_t word = 0; word < words; ++word)
            write(dst, destination + word, load(src, source + word));
    }

    void count(llvm::Value* counter, std::uint64_t instructions) {
        // A helper may re-enter native code and update the same counter between runs.
        auto* previous = builder.CreateLoad(builder.getInt64Ty(), counter);
        builder.CreateStore(builder.CreateAdd(previous, builder.getInt64(instructions)), counter);
    }

    llvm::ConstantInt* result(runtime::ExecutionResult value) {
        return builder.getInt32(static_cast<std::uint32_t>(value));
    }

    llvm::LLVMContext& context;
    llvm::Module& module;
    llvm::IRBuilder<> builder;
};

} // namespace

std::unique_ptr<runtime::ExecutionBackend> makeBackend(const runtime::Image& image,
                                                       DiagnosticEngine& diagnostics) {
    std::vector<std::uint32_t> indices;
    for (const auto& process : image.processes()) indices.push_back(process.bodyIndex);
    for (const auto& subprogram : image.subprograms()) indices.push_back(subprogram.bodyIndex);
    std::ranges::sort(indices);
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    const auto& design = image.compiledDesign();
    for (const auto index : indices) {
        const auto& body = design.body(index);
        for (std::size_t pc = 0; pc < body.code.size(); ++pc) {
            const auto& instruction = body.code[pc];
            if (!supported(instruction)) {
                diagnostics.report(DiagCode::JitPreparationFailed, body.location,
                                   std::format("{} at instruction {} in '{}' is not supported",
                                               eir::mnemonic(instruction.opcode), pc, body.name));
                return nullptr;
            }
        }
    }
    auto engine = Engine::create(diagnostics);
    if (!engine) return nullptr;
    std::vector<NativeBody> entries(design.bodies().size(), nullptr);
    // Materialize bounded modules before emitting the next one, so LLVM can release each
    // module's IR and code-generation state before the kernel starts. A body stays intact.
    constexpr std::size_t MaxBytecodePerModule = 4096;
    for (std::size_t first = 0; first < indices.size();) {
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>(std::format("enzan_{}", first), *context);
        Lowerer lowerer(*context, *module);
        std::size_t count = 0;
        std::size_t last = first;
        while (last < indices.size()) {
            const auto index = indices[last];
            const auto& body = design.body(index);
            const std::size_t instructions = body.code.size();
            if (last != first
                && (count >= MaxBytecodePerModule || instructions > MaxBytecodePerModule - count))
                break;
            lowerer.emit(body, index);
            count += instructions;
            ++last;
        }
        if (!engine->addModule({std::move(module), std::move(context)})) return nullptr;
        for (std::size_t current = first; current < last; ++current) {
            const auto index = indices[current];
            auto entry = engine->lookup(Lowerer::name(index));
            if (entry == nullptr) return nullptr;
            entries[index] = entry;
        }
        first = last;
    }
    return std::make_unique<NativeBackend>(std::move(engine), std::move(entries));
}

} // namespace enzan::jit
