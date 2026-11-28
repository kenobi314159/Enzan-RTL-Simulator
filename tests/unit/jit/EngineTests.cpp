// SPDX-License-Identifier: GPL-3.0-or-later

#include "jit/Engine.h"

#include "codegen/Bytecode.h"
#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "runtime/Execution.h"
#include "runtime/ExecutionHelpers.h"
#include "support/Literals.h"
#include "support/ProcessBuilder.h"
#include "support/Simulation.h"

#include <gtest/gtest.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace enzan;

class OrcTest : public ::testing::Test {
protected:
    OrcTest() : diagnostics(sources) { diagnostics.addConsumer(consumer); }

    SourceManager sources;
    DiagnosticEngine diagnostics;
    test::CollectingConsumer consumer;
};

llvm::orc::ThreadSafeModule returnBody(bool invalid = false,
                                       const char* helperName = "enzanExecCount") {
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("test", *context);
    llvm::IRBuilder<> builder(*context);
    auto* pointer = builder.getPtrTy();
    auto* countType =
        llvm::FunctionType::get(builder.getVoidTy(), {pointer, builder.getInt64Ty()}, false);
    auto count = module->getOrInsertFunction(helperName, countType);
    auto* functionType = llvm::FunctionType::get(builder.getInt32Ty(), {pointer}, false);
    auto* function =
        llvm::Function::Create(functionType, llvm::Function::ExternalLinkage, "test_body", *module);
    auto* entry = llvm::BasicBlock::Create(*context, "entry", function);
    builder.SetInsertPoint(entry);
    builder.CreateCall(count, {function->getArg(0), builder.getInt64(1)});
    if (!invalid) builder.CreateRet(builder.getInt32(0));
    return {std::move(module), std::move(context)};
}

TEST_F(OrcTest, RegistersOnlyExplicitRuntimeSymbols) {
    auto engine = jit::Engine::create(diagnostics);
    ASSERT_NE(engine, nullptr);
    EXPECT_NE(engine->lookup("enzanExecCount"), nullptr);
    EXPECT_EQ(engine->lookup("getpid"), nullptr);
    ASSERT_FALSE(consumer.messages.empty());
    EXPECT_EQ(consumer.codes.back(), DiagCode::JitPreparationFailed);
}

TEST_F(OrcTest, RejectsInvalidIrAndDuplicateSymbols) {
    auto engine = jit::Engine::create(diagnostics);
    ASSERT_NE(engine, nullptr);
    EXPECT_FALSE(engine->addModule(returnBody(true)));
    ASSERT_FALSE(consumer.messages.empty());
    EXPECT_EQ(consumer.codes.back(), DiagCode::JitPreparationFailed);
    EXPECT_TRUE(engine->addModule(returnBody()));
    EXPECT_FALSE(engine->addModule(returnBody()));
    EXPECT_EQ(consumer.codes.back(), DiagCode::JitPreparationFailed);
}

TEST_F(OrcTest, ReportsUnresolvedSymbolsDuringMaterialization) {
    auto engine = jit::Engine::create(diagnostics);
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->addModule(returnBody(false, "enzanMissingHelper")));
    EXPECT_EQ(engine->lookup("test_body"), nullptr);
    ASSERT_FALSE(consumer.messages.empty());
    EXPECT_EQ(consumer.codes.back(), DiagCode::JitPreparationFailed);
}

class NativeBackend final : public runtime::ExecutionBackend {
public:
    NativeBackend(std::unique_ptr<jit::Engine> jitEngine, jit::NativeBody entry)
        : engine(std::move(jitEngine)), body(entry) {}

    runtime::ExecutionResult run(runtime::ExecutionContext& context) override {
        return static_cast<runtime::ExecutionResult>(body(&context));
    }

    runtime::ExecutionResult call(runtime::ExecutionContext& context) override {
        return run(context);
    }

private:
    std::unique_ptr<jit::Engine> engine;
    jit::NativeBody body;
};

eir::Design returnDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    test::ProcessBuilder process(eir::ProcessKind::Initial, "entry");
    process.block("entry");
    process.emit(eir::Instruction::make(eir::Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design design;
    design.addModule(std::move(top));
    return design;
}

TEST_F(OrcTest, ExecutesNativeCodeThroughTheKernel) {
    auto engine = jit::Engine::create(diagnostics);
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->addModule(returnBody()));
    const auto address = engine->lookup("test_body");
    ASSERT_NE(address, nullptr);
    auto body = address;
    test::Simulation native(returnDesign(), "tb",
                            std::make_unique<NativeBackend>(std::move(engine), body));
    test::Simulation interpreted(returnDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(interpreted.ready());
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(interpreted.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.simulationKernel().executedInstructions(), 1U);
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              interpreted.simulationKernel().executedInstructions());
    EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
              interpreted.simulationKernel().totalDeltaCycles());
    EXPECT_TRUE(consumer.messages.empty());
}

eir::Design waitDesign() {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(eir::Object{.name = "source",
                              .type = eir::Type::bits(4, false, true),
                              .initialValue = test::literal("4'b1xz0")});
    top.addObject(eir::Object{.name = "output", .type = eir::Type::bits(4, false, true)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "driver");
    const auto value = process.reg(eir::Type::bits(4, false, true));
    const auto delay = process.reg(eir::Type::bits(32, false, true));
    const auto finalValue = process.reg(eir::Type::bits(4, false, true));
    const auto delayConstant = process.constant("32'd1");
    const auto finalConstant = process.constant("4'b0011");
    process.block("entry");
    process.emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(delay),
                                        eir::Operand::constant(delayConstant)));
    process.emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(finalValue),
                                        eir::Operand::constant(finalConstant)));
    process.readObject(value, 0);
    process.store(1, value);
    process.emit(eir::Instruction::make(eir::Opcode::WaitTime, {}, eir::Operand::reg(delay),
                                        eir::Operand::block(1)));
    process.block("resume");
    process.store(1, finalValue);
    process.emit(eir::Instruction::make(eir::Opcode::Return));
    top.processes.push_back(process.finish());
    eir::Design design;
    design.addModule(std::move(top));
    return design;
}

// A native body for this one known test process. The production bytecode lowering is still
// to come; taking the actual compiled operands here avoids assumptions about frame layout.
llvm::orc::ThreadSafeModule waitBody(const codegen::CompiledBody& body) {
    const auto& read = body.code[0];
    const auto& firstStore = body.code[1];
    const auto& wait = body.code[2];
    const auto& secondStore = body.code[3];
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("wait", *context);
    llvm::IRBuilder<> builder(*context);
    auto* ptr = builder.getPtrTy();
    auto* i32 = builder.getInt32Ty();
    auto* i64 = builder.getInt64Ty();
    auto helper = [&](const char* name, llvm::Type* result,
                      std::initializer_list<llvm::Type*> args) {
        return module->getOrInsertFunction(name, llvm::FunctionType::get(result, args, false));
    };
    const auto resume = helper("enzanExecResumePc", i32, {ptr});
    const auto frame = helper("enzanExecFrame", ptr, {ptr});
    const auto object = helper("enzanExecReadObject", ptr, {ptr, i32});
    const auto store = helper("enzanExecStore", builder.getVoidTy(), {ptr, i32, i32, i32, i32});
    const auto count = helper("enzanExecCount", builder.getVoidTy(), {ptr, i64});
    const auto suspend = helper("enzanExecWaitTime", i32, {ptr, i32, i32, i32});
    auto* function = llvm::Function::Create(llvm::FunctionType::get(i32, {ptr}, false),
                                            llvm::Function::ExternalLinkage, "test_wait", *module);
    auto* entry = llvm::BasicBlock::Create(*context, "entry", function);
    auto* start = llvm::BasicBlock::Create(*context, "start", function);
    auto* resumed = llvm::BasicBlock::Create(*context, "resume", function);
    auto* invalid = llvm::BasicBlock::Create(*context, "invalid", function);
    builder.SetInsertPoint(entry);
    auto* argument = function->getArg(0);
    auto* pc = builder.CreateCall(resume, {argument});
    auto* target = builder.CreateSwitch(pc, invalid, 2);
    target->addCase(builder.getInt32(0), start);
    target->addCase(builder.getInt32(wait.b), resumed);
    builder.SetInsertPoint(invalid);
    builder.CreateRet(
        builder.getInt32(static_cast<std::uint32_t>(runtime::ExecutionResult::Stopped)));
    builder.SetInsertPoint(start);
    auto* words = builder.CreateCall(frame, {argument});
    auto* source = builder.CreateCall(object, {argument, builder.getInt32(read.a)});
    for (std::uint32_t plane = 0; plane < 2; ++plane) {
        auto* input = builder.CreateConstGEP1_32(i64, source, plane);
        auto* output = builder.CreateConstGEP1_32(i64, words, read.dst + plane);
        builder.CreateStore(builder.CreateLoad(i64, input), output);
    }
    auto emitStore = [&](const codegen::Instruction& instruction) {
        builder.CreateCall(
            store, {argument, builder.getInt32(instruction.a), builder.getInt32(instruction.dst),
                    builder.getInt32(instruction.b), builder.getInt32(instruction.bWidth)});
    };
    builder.CreateCall(count, {argument, builder.getInt64(2)});
    emitStore(firstStore);
    builder.CreateCall(count, {argument, builder.getInt64(1)});
    builder.CreateRet(
        builder.CreateCall(suspend, {argument, builder.getInt32(wait.a),
                                     builder.getInt32(wait.aWidth), builder.getInt32(wait.b)}));
    builder.SetInsertPoint(resumed);
    builder.CreateCall(count, {argument, builder.getInt64(1)});
    emitStore(secondStore);
    builder.CreateCall(count, {argument, builder.getInt64(1)});
    builder.CreateRet(builder.getInt32(0));
    return {std::move(module), std::move(context)};
}

TEST_F(OrcTest, NativeWaitAndFourStateStoresMatchInterpreter) {
    auto design = waitDesign();
    codegen::Compiler compiler(diagnostics);
    auto compiled = compiler.compile(design);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(compiled->bodies().size(), 1U);
    const auto& body = compiled->body(0);
    ASSERT_EQ(body.code.size(), 5U);
    ASSERT_EQ(body.code[0].opcode, eir::Opcode::ReadObject);
    ASSERT_EQ(body.code[1].opcode, eir::Opcode::Store);
    ASSERT_EQ(body.code[2].opcode, eir::Opcode::WaitTime);
    ASSERT_EQ(body.code[3].opcode, eir::Opcode::Store);
    ASSERT_EQ(body.code[4].opcode, eir::Opcode::Return);
    auto engine = jit::Engine::create(diagnostics);
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->addModule(waitBody(body)));
    const auto address = engine->lookup("test_wait");
    ASSERT_NE(address, nullptr);
    test::Simulation native(waitDesign(), "tb",
                            std::make_unique<NativeBackend>(std::move(engine), address));
    test::Simulation interpreted(waitDesign(), "tb");
    ASSERT_TRUE(native.ready());
    ASSERT_TRUE(interpreted.ready());
    EXPECT_EQ(native.run(0), runtime::RunStatus::TimeLimitReached);
    EXPECT_EQ(interpreted.run(0), runtime::RunStatus::TimeLimitReached);
    EXPECT_EQ(native.value("/tb/output"), "1xz0");
    EXPECT_EQ(native.value("/tb/output"), interpreted.value("/tb/output"));
    EXPECT_EQ(native.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(interpreted.run(), runtime::RunStatus::Quiescent);
    EXPECT_EQ(native.value("/tb/output"), "0011");
    EXPECT_EQ(native.value("/tb/output"), interpreted.value("/tb/output"));
    EXPECT_EQ(native.time(), interpreted.time());
    EXPECT_EQ(native.simulationKernel().executedInstructions(),
              interpreted.simulationKernel().executedInstructions());
    EXPECT_EQ(native.simulationKernel().totalDeltaCycles(),
              interpreted.simulationKernel().totalDeltaCycles());
    EXPECT_TRUE(consumer.messages.empty());
}

} // namespace
