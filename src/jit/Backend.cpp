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

#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
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
    case Opcode::SysTask:
    case Opcode::WaitTime:
    case Opcode::WaitEdge:
    case Opcode::WaitEvents: return true;
    case Opcode::BranchIf: return instruction.aWidth != 0;
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
    case Opcode::Store:
    case Opcode::StoreNonBlocking: return instruction.bWidth != 0;
    case Opcode::Add:
    case Opcode::Subtract:
    case Opcode::Multiply:
        return !instruction.isReal && instruction.dstWidth != 0 && instruction.dstWidth <= 64
               && instruction.aWidth == instruction.dstWidth
               && instruction.bWidth == instruction.dstWidth;
    case Opcode::ShiftLeft:
    case Opcode::ShiftRightLogical:
    case Opcode::ShiftRightArithmetic:
        return !instruction.isReal && instruction.dstWidth != 0 && instruction.dstWidth <= 64
               && instruction.aWidth == instruction.dstWidth && instruction.bWidth != 0
               && instruction.bWidth <= 64;
    case Opcode::Equal:
    case Opcode::NotEqual:
    case Opcode::CaseEqual:
    case Opcode::CaseNotEqual:
    case Opcode::LessThan:
    case Opcode::LessEqual:
        return !instruction.isReal && instruction.dstWidth == 1 && instruction.aWidth != 0
               && instruction.aWidth <= 64 && instruction.aWidth == instruction.bWidth;
    default: return false;
    }
}

class NativeBackend final : public runtime::ExecutionBackend {
public:
    NativeBackend(std::unique_ptr<Engine> owner,
                  std::unordered_map<const CompiledBody*, NativeBody> entries)
        : engine(std::move(owner)), bodies(std::move(entries)) {}

    runtime::ExecutionResult run(runtime::ExecutionContext& context) override {
        const auto entry = bodies.find(context.activation().body);
        if (entry == bodies.end()) return runtime::ExecutionResult::Stopped;
        return static_cast<runtime::ExecutionResult>(entry->second(&context));
    }

    runtime::ExecutionResult call(runtime::ExecutionContext& context) override {
        return run(context);
    }

private:
    std::unique_ptr<Engine> engine;
    std::unordered_map<const CompiledBody*, NativeBody> bodies;
};

/// Emits one native function per body. Every bytecode instruction has its own block so that
/// branch and resume targets use bytecode indices, including those after eliminated constants.
class Lowerer {
public:
    Lowerer(llvm::LLVMContext& llvmContext, llvm::Module& llvmModule)
        : context(llvmContext), module(llvmModule), builder(llvmContext) {}

    void emit(const CompiledBody& body, std::uint32_t index) {
        auto* ptr = builder.getPtrTy();
        auto* type = llvm::FunctionType::get(builder.getInt32Ty(), {ptr}, false);
        auto* function =
            llvm::Function::Create(type, llvm::Function::ExternalLinkage, name(index), module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", function);
        auto* invalid = llvm::BasicBlock::Create(context, "invalid_resume", function);
        std::vector<llvm::BasicBlock*> blocks;
        blocks.reserve(body.code.size());
        for (std::size_t pc = 0; pc < body.code.size(); ++pc)
            blocks.push_back(
                llvm::BasicBlock::Create(context, "pc" + std::to_string(pc), function));

        builder.SetInsertPoint(entry);
        auto* argument = function->getArg(0);
        auto resume = helper("enzanExecResumePc", builder.getInt32Ty(), {ptr});
        auto frameFunction = helper("enzanExecFrame", ptr, {ptr});
        auto* frame = builder.CreateCall(frameFunction, {argument});
        auto* branch = builder.CreateSwitch(builder.CreateCall(resume, {argument}), invalid,
                                            static_cast<unsigned>(blocks.size()));
        for (std::size_t pc = 0; pc < blocks.size(); ++pc)
            branch->addCase(builder.getInt32(static_cast<std::uint32_t>(pc)), blocks[pc]);
        builder.SetInsertPoint(invalid);
        builder.CreateRet(result(runtime::ExecutionResult::Stopped));

        for (std::size_t pc = 0; pc < body.code.size(); ++pc) {
            builder.SetInsertPoint(blocks[pc]);
            const Instruction& op = body.code[pc];
            count(argument);
            const auto next = [&] { builder.CreateBr(blocks[pc + 1]); };
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
            case Opcode::ReadObject: {
                auto read = helper("enzanExecReadObject", ptr, {ptr, builder.getInt32Ty()});
                auto* source = builder.CreateCall(read, {argument, builder.getInt32(op.a)});
                copy(frame, op.dst, source, 0, eir::storageWordsFor(op.dstWidth));
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
            case Opcode::Add:
            case Opcode::Subtract:
            case Opcode::Multiply: {
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
            case Opcode::Equal:
            case Opcode::NotEqual:
            case Opcode::CaseEqual:
            case Opcode::CaseNotEqual:
            case Opcode::LessThan:
            case Opcode::LessEqual: {
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

    void count(llvm::Value* argument) {
        auto increment = helper("enzanExecCount", builder.getVoidTy(),
                                {builder.getPtrTy(), builder.getInt64Ty()});
        builder.CreateCall(increment, {argument, builder.getInt64(1)});
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
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("enzan", *context);
    Lowerer lowerer(*context, *module);
    for (const auto index : indices) lowerer.emit(design.body(index), index);
    if (!engine->addModule({std::move(module), std::move(context)})) return nullptr;
    std::unordered_map<const CompiledBody*, NativeBody> entries;
    for (const auto index : indices) {
        auto entry = engine->lookup(Lowerer::name(index));
        if (entry == nullptr) return nullptr;
        entries.emplace(&design.body(index), entry);
    }
    return std::make_unique<NativeBackend>(std::move(engine), std::move(entries));
}

} // namespace enzan::jit
