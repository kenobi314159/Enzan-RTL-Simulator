// SPDX-License-Identifier: GPL-3.0-or-later

#include "codegen/Compiler.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include "support/Literals.h"
#include "support/ProcessBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::codegen::CompiledDesign;
using enzan::codegen::Compiler;
using enzan::eir::BasicBlock;
using enzan::eir::Design;
using enzan::eir::Instruction;
using enzan::eir::Logic;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ParameterMode;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::eir::WriteExtent;
using enzan::test::literal;
using enzan::test::ProcessBuilder;
using enzan::test::SubprogramBuilder;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { messages.push_back(diagnostic.message); }

    std::vector<std::string> messages;
};

class CompilerTest : public ::testing::Test {
protected:
    CompilerTest() : engine(sources) { engine.addConsumer(consumer); }

    std::unique_ptr<CompiledDesign> compile() {
        Compiler compiler(engine);
        return compiler.compile(design);
    }

    [[nodiscard]] bool reported(std::string_view fragment) const {
        return std::ranges::any_of(consumer.messages, [fragment](const std::string& message) {
            return message.find(fragment) != std::string::npos;
        });
    }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    Design design;
};

/// A definition with one process, whose body the caller fills in.
ModuleDefinition makeModule(Process process) {
    ModuleDefinition definition;
    definition.name = "m";
    definition.sourceName = "m";
    definition.addObject(Object{.name = "a", .type = Type::bits(8, false, true)});
    definition.addObject(Object{.name = "b", .type = Type::bits(8, false, true)});
    definition.processes.push_back(std::move(process));
    return definition;
}

Process makeCopyProcess() {
    Process process;
    process.kind = ProcessKind::AlwaysComb;
    process.name = "copy";
    process.registerTypes.push_back(Type::bits(8, false, true));
    process.readSet = {0};
    process.writeSet = {WriteExtent::whole(1, 8)};

    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(0)));
    entry.instructions.push_back(
        Instruction::make(Opcode::Store, {}, Operand::object(1), Operand::reg(0)));
    entry.instructions.push_back(Instruction::make(Opcode::Return));
    process.blocks.push_back(std::move(entry));
    return process;
}

// ---------------------------------------------------------------------------
// Frame layout
// ---------------------------------------------------------------------------

TEST_F(CompilerTest, CompilesASimpleBody) {
    design.addModule(makeModule(makeCopyProcess()));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();
    ASSERT_EQ(compiled->bodies().size(), 1U);

    const auto& body = compiled->body(0);
    EXPECT_EQ(body.code.size(), 3U);
    EXPECT_EQ(body.code[0].opcode, Opcode::ReadObject);
    EXPECT_EQ(body.code[2].opcode, Opcode::Return);
    EXPECT_EQ(body.kind, ProcessKind::AlwaysComb);
    EXPECT_EQ(body.name, "copy");
    EXPECT_TRUE(body.startsAtTimeZero) << "always_comb evaluates once at time zero";
}

TEST_F(CompilerTest, RegistersBecomeFrameOffsetsAndCarryTheirWidths) {
    Process process = makeCopyProcess();
    // A 1-bit, a 64-bit and a 100-bit register: the frame must pack them in order,
    // two planes each.
    process.registerTypes = {Type::logic(), Type::bits(64, false, true),
                             Type::bits(100, false, true)};
    process.blocks[0].instructions[0] =
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(0));
    process.blocks[0].instructions[1] = Instruction::make(Opcode::Return);
    process.blocks[0].instructions.resize(2);

    ModuleDefinition definition = makeModule(std::move(process));
    definition.objects[0].type = Type::logic();
    design.addModule(std::move(definition));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();

    const auto& body = compiled->body(0);
    EXPECT_EQ(body.frameWords, 2U + 2U + 4U);
    EXPECT_EQ(body.code[0].dst, 0U) << "register 0 sits at the start of the frame";
    EXPECT_EQ(body.code[0].dstWidth, 1U);
}

TEST_F(CompilerTest, AStoreCarriesItsObjectsPositionAmongTheObjectsWritten) {
    // The runtime routes a store by this position (DESIGN_GUIDE.md section 3.7), so it counts
    // objects, not extents: object 1 is written in two ranges and is still position 0.
    Process process = makeCopyProcess();
    process.writeSet = {WriteExtent{.objectIndex = 1, .lowBit = 0, .width = 2},
                        WriteExtent{.objectIndex = 1, .lowBit = 4, .width = 4},
                        WriteExtent::whole(2, 8)};
    process.blocks[0].instructions.insert(
        process.blocks[0].instructions.begin() + 2,
        Instruction::make(Opcode::Store, {}, Operand::object(2), Operand::reg(0)));

    ModuleDefinition definition = makeModule(std::move(process));
    definition.addObject(Object{.name = "c", .type = Type::bits(8, false, true)});
    design.addModule(std::move(definition));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();

    const auto& body = compiled->body(0);
    ASSERT_EQ(body.code[1].opcode, Opcode::Store);
    EXPECT_EQ(body.code[1].dst, 0U);
    ASSERT_EQ(body.code[2].opcode, Opcode::Store);
    EXPECT_EQ(body.code[2].dst, 1U);
}

TEST_F(CompilerTest, NonRegisterOperandsKeepTheirIndexAndHaveNoWidth) {
    design.addModule(makeModule(makeCopyProcess()));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr);

    // An object operand stays an object index: the runtime resolves it per instance,
    // which is what lets every instance share one compiled body.
    const auto& body = compiled->body(0);
    EXPECT_EQ(body.code[0].a, 0U);
    EXPECT_EQ(body.code[0].aWidth, 0U);
    EXPECT_EQ(body.code[1].a, 1U) << "the store names object 1";
    EXPECT_EQ(body.code[1].bWidth, 8U) << "the value register keeps its width";
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

TEST_F(CompilerTest, ConstantsAreFlattenedIntoOnePoolInTwoPlaneForm) {
    Process process = makeCopyProcess();
    process.constants.push_back(literal("8'hA5"));
    process.constants.push_back(literal("100'h1"));
    process.blocks[0].instructions[0] =
        Instruction::make(Opcode::LoadConstant, Operand::reg(0), Operand::constant(0));

    design.addModule(makeModule(std::move(process)));
    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();

    const auto& body = compiled->body(0);
    ASSERT_EQ(body.constants.size(), 2U);

    // Two Words for the 8-bit value, four for the 100-bit one.
    EXPECT_EQ(body.constants[0].wordOffset, 0U);
    EXPECT_EQ(body.constants[0].bitWidth, 8U);
    EXPECT_EQ(body.constants[1].wordOffset, 2U);
    EXPECT_EQ(body.constants[1].bitWidth, 100U);
    EXPECT_EQ(body.constantPool.size(), 6U);

    // The view over the pool must read back the value that went in.
    EXPECT_EQ(enzan::eir::toString(body.constant(0), enzan::eir::Radix::Hexadecimal), "a5");
    EXPECT_EQ(body.constant(1).get(0), Logic::One);
}

TEST_F(CompilerTest, UnknownBitsSurviveTheConstantPool) {
    // The pool holds both planes, so a literal with X or Z in it must come back
    // unchanged rather than as its aval bits alone.
    Process process = makeCopyProcess();
    process.constants.push_back(literal("4'b10xz"));
    process.blocks[0].instructions[0] =
        Instruction::make(Opcode::LoadConstant, Operand::reg(0), Operand::constant(0));

    design.addModule(makeModule(std::move(process)));
    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr);

    EXPECT_EQ(enzan::eir::toString(compiled->body(0).constant(0), enzan::eir::Radix::Binary),
              "10xz");
}

TEST_F(CompilerTest, ARegisterHoldingOneConstantStartsWithItAndIsNotLoaded) {
    // A register whose only writer is one constant load holds the constant in the frame's
    // starting image, extended as the load would extend it, and the load is left out. One
    // written again keeps its load, which has to run where it stands.
    ProcessBuilder builder(ProcessKind::Initial, "constants");
    const std::uint32_t mask = builder.reg(Type::bits(8, false, true));
    const std::uint32_t widened = builder.reg(Type::bits(16, true, false));
    const std::uint32_t counter = builder.reg(Type::bits(8, false, true));
    const std::uint32_t a5 = builder.constant("8'hA5");
    const std::uint32_t minus128 = builder.constant("8'sh80");
    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(mask), Operand::constant(a5)));
    builder.emit(Instruction::makeSigned(Opcode::LoadConstant, Operand::reg(widened),
                                         Operand::constant(minus128)));
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(counter), Operand::constant(a5)));
    builder.emit(Instruction::make(Opcode::Add, Operand::reg(counter), Operand::reg(counter),
                                   Operand::reg(mask)));
    builder.store(1, counter);
    builder.emit(Instruction::make(Opcode::Return));
    design.addModule(makeModule(builder.finish()));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();
    const auto& body = compiled->body(0);
    ASSERT_EQ(body.code.size(), 4U);
    EXPECT_EQ(body.code[0].opcode, Opcode::LoadConstant);
    EXPECT_EQ(body.code[0].dst, 4U) << "the counter's load stays";
    EXPECT_EQ(body.code[1].opcode, Opcode::Add);

    ASSERT_EQ(body.frameImage.size(), body.frameWords);
    EXPECT_EQ(body.frameImage, (std::vector<enzan::eir::Word>{0xA5, 0, 0xFF80, 0, 0, 0}))
        << "the mask's and the widened value's planes, and the counter's zero";
}

TEST_F(CompilerTest, AParameterOrACallsArgumentIsNotAConstantRegister) {
    // A caller writes a parameter as the call starts, and a call writes its out arguments back
    // as it returns, so a constant load into either has to run where it stands.
    SubprogramBuilder callee("reset");
    const std::uint32_t value =
        callee.parameter("value", Type::bits(8, false, true), ParameterMode::InOut);
    const std::uint32_t zero = callee.constant("8'h00");
    callee.block("entry");
    callee.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(zero)));
    callee.emit(Instruction::make(Opcode::Return));

    ProcessBuilder caller(ProcessKind::Initial, "caller");
    const std::uint32_t argument = caller.reg(Type::bits(8, false, true));
    const std::uint32_t seven = caller.constant("8'h07");
    caller.block("entry");
    caller.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(argument), Operand::constant(seven)));
    caller.call("", "reset", {Operand::reg(argument)});
    caller.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeModule(caller.finish());
    definition.subprograms.push_back(callee.finish());
    design.addModule(std::move(definition));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();
    const auto& calleeBody = compiled->body(compiled->subprogramBodyIndex(0, 0));
    ASSERT_FALSE(calleeBody.code.empty());
    EXPECT_EQ(calleeBody.code[0].opcode, Opcode::LoadConstant);
    const auto& callerBody = compiled->body(compiled->bodyIndex(0, 0));
    ASSERT_FALSE(callerBody.code.empty());
    EXPECT_EQ(callerBody.code[0].opcode, Opcode::LoadConstant);
}

// ---------------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------------

TEST_F(CompilerTest, BlocksAreLaidOutInOrderAndBranchesResolveToInstructionIndices) {
    Process process;
    process.kind = ProcessKind::Initial;
    process.name = "branchy";
    process.registerTypes.push_back(Type::logic());
    process.readSet = {0};
    process.writeSet = {};

    // entry: read a; br a -> then, else
    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(0)));
    entry.instructions.push_back(Instruction::make(Opcode::BranchIf, {}, Operand::reg(0),
                                                   Operand::block(1), Operand::block(2)));
    process.blocks.push_back(std::move(entry));

    BasicBlock thenBlock;
    thenBlock.label = "then";
    thenBlock.instructions.push_back(Instruction::make(Opcode::Jump, {}, Operand::block(2)));
    process.blocks.push_back(std::move(thenBlock));

    BasicBlock exitBlock;
    exitBlock.label = "exit";
    exitBlock.instructions.push_back(Instruction::make(Opcode::Return));
    process.blocks.push_back(std::move(exitBlock));

    ModuleDefinition definition = makeModule(std::move(process));
    definition.objects[0].type = Type::logic();
    design.addModule(std::move(definition));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();

    const auto& body = compiled->body(0);
    ASSERT_EQ(body.blockStarts.size(), 3U);
    EXPECT_EQ(body.blockStarts[0], 0U);
    EXPECT_EQ(body.blockStarts[1], 2U);
    EXPECT_EQ(body.blockStarts[2], 3U);

    // A block index becomes the instruction index that block starts at, which can only
    // be known once every earlier block has been emitted.
    EXPECT_EQ(body.code[1].b, 2U) << "then-target";
    EXPECT_EQ(body.code[1].c, 3U) << "else-target";
    EXPECT_EQ(body.code[2].a, 3U) << "the jump in the then-block";
}

TEST_F(CompilerTest, ABranchBackwardsResolvesToo) {
    // A loop's back edge names a block that was emitted earlier, so it exercises the
    // opposite direction from a forward branch.
    Process process;
    process.kind = ProcessKind::Always;
    process.name = "loop";
    process.registerTypes.push_back(Type::logic());
    process.readSet = {};
    process.writeSet = {};

    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(Instruction::make(Opcode::Jump, {}, Operand::block(1)));
    process.blocks.push_back(std::move(entry));

    BasicBlock body;
    body.label = "body";
    body.instructions.push_back(Instruction::make(Opcode::Nop));
    body.instructions.push_back(Instruction::make(Opcode::Jump, {}, Operand::block(0)));
    process.blocks.push_back(std::move(body));

    design.addModule(makeModule(std::move(process)));
    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();

    EXPECT_EQ(compiled->body(0).code[0].a, 1U);
    EXPECT_EQ(compiled->body(0).code[2].a, 0U) << "the back edge";
}

// ---------------------------------------------------------------------------
// System tasks
// ---------------------------------------------------------------------------

TEST_F(CompilerTest, SystemTaskArgumentsResolveToFrameOffsetsAndConstantIndices) {
    Process process = makeCopyProcess();
    process.strings.emplace_back("count = %0d");
    process.constants.push_back(literal("8'd7"));

    enzan::eir::SysTaskCall call;
    call.kind = enzan::eir::SysTaskKind::Display;
    call.formatIndex = 0;
    call.arguments.push_back(
        enzan::eir::SysTaskArgument{.value = Operand::reg(0), .type = Type::bits(8, true, true)});
    call.arguments.push_back(enzan::eir::SysTaskArgument{.value = Operand::constant(0),
                                                         .type = Type::bits(8, false, true)});
    process.sysCalls.push_back(std::move(call));

    process.blocks[0].instructions[1] = Instruction::make(Opcode::SysTask, {}, Operand::sysCall(0));
    process.writeSet.clear();

    design.addModule(makeModule(std::move(process)));
    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();

    const auto& body = compiled->body(0);
    ASSERT_EQ(body.sysCalls.size(), 1U);
    const auto& descriptor = body.sysCalls[0];

    EXPECT_TRUE(descriptor.hasFormat());
    EXPECT_EQ(body.strings[descriptor.formatIndex], "count = %0d");
    ASSERT_EQ(descriptor.arguments.size(), 2U);

    EXPECT_FALSE(descriptor.arguments[0].isConstant);
    EXPECT_EQ(descriptor.arguments[0].location, 0U);
    EXPECT_EQ(descriptor.arguments[0].bitWidth, 8U);
    EXPECT_TRUE(descriptor.arguments[0].isSigned) << "signedness comes from the type, not the "
                                                     "format string";

    EXPECT_TRUE(descriptor.arguments[1].isConstant);
    EXPECT_EQ(descriptor.arguments[1].location, 0U) << "constant pool index";
}

TEST_F(CompilerTest, ASystemTaskWithNoFormatStringIsAllowed) {
    Process process = makeCopyProcess();
    enzan::eir::SysTaskCall call;
    call.kind = enzan::eir::SysTaskKind::Finish;
    process.sysCalls.push_back(std::move(call));
    process.blocks[0].instructions[1] = Instruction::make(Opcode::SysTask, {}, Operand::sysCall(0));
    process.writeSet.clear();

    design.addModule(makeModule(std::move(process)));
    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();
    EXPECT_FALSE(compiled->body(0).sysCalls[0].hasFormat());
}

// ---------------------------------------------------------------------------
// Indexing and error paths
// ---------------------------------------------------------------------------

TEST_F(CompilerTest, BodiesAreFoundByDefinitionAndProcessIndex) {
    design.addModule(makeModule(makeCopyProcess()));

    ModuleDefinition second = makeModule(makeCopyProcess());
    second.name = "n";
    second.processes.push_back(makeCopyProcess());
    design.addModule(std::move(second));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr);

    EXPECT_EQ(compiled->bodies().size(), 3U);
    EXPECT_EQ(compiled->bodyIndex(0, 0), 0U);
    EXPECT_EQ(compiled->bodyIndex(1, 0), 1U);
    EXPECT_EQ(compiled->bodyIndex(1, 1), 2U);
    EXPECT_EQ(compiled->bodyIndex(2, 0), CompiledDesign::NotCompiled);
    EXPECT_EQ(compiled->bodyIndex(0, 5), CompiledDesign::NotCompiled);
}

TEST_F(CompilerTest, ADesignWithNoProcessesCompilesToNothing) {
    ModuleDefinition definition;
    definition.name = "empty";
    design.addModule(std::move(definition));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(compiled->bodies().empty());
}

TEST_F(CompilerTest, RejectsAnOutOfRangeRegisterAndSaysTheVerifierWasSkipped) {
    // A compiler that quietly tolerates bad input is how a frontend bug becomes a
    // runtime crash, so the message names the step that was missed.
    Process process = makeCopyProcess();
    process.blocks[0].instructions[0].dst = Operand::reg(9);

    design.addModule(makeModule(std::move(process)));
    EXPECT_EQ(compile(), nullptr);
    EXPECT_TRUE(reported("register 9"));
    EXPECT_TRUE(reported("EIR verifier"));
}

TEST_F(CompilerTest, RejectsABranchToANonexistentBlock) {
    Process process = makeCopyProcess();
    process.blocks[0].instructions.back() = Instruction::make(Opcode::Jump, {}, Operand::block(4));

    design.addModule(makeModule(std::move(process)));
    EXPECT_EQ(compile(), nullptr);
    EXPECT_TRUE(reported("branch to block 4"));
}

TEST_F(CompilerTest, RejectsASystemTaskArgumentThatIsNeitherRegisterNorConstant) {
    Process process = makeCopyProcess();
    enzan::eir::SysTaskCall call;
    call.kind = enzan::eir::SysTaskKind::Display;
    call.arguments.push_back(
        enzan::eir::SysTaskArgument{.value = Operand::object(0), .type = Type::logic()});
    process.sysCalls.push_back(std::move(call));
    process.blocks[0].instructions[1] = Instruction::make(Opcode::SysTask, {}, Operand::sysCall(0));
    process.writeSet.clear();

    design.addModule(makeModule(std::move(process)));
    EXPECT_EQ(compile(), nullptr);
    EXPECT_TRUE(reported("neither a register nor a constant"));
}

TEST_F(CompilerTest, RejectsAFormatStringIndexOutOfRange) {
    Process process = makeCopyProcess();
    enzan::eir::SysTaskCall call;
    call.kind = enzan::eir::SysTaskKind::Display;
    call.formatIndex = 3;
    process.sysCalls.push_back(std::move(call));
    process.blocks[0].instructions[1] = Instruction::make(Opcode::SysTask, {}, Operand::sysCall(0));
    process.writeSet.clear();

    design.addModule(makeModule(std::move(process)));
    EXPECT_EQ(compile(), nullptr);
    EXPECT_TRUE(reported("names string 3"));
}

TEST_F(CompilerTest, ACallKeepsTheCallersHalfAndTheCalleeKeepsItsOwn) {
    // A call resolves only what the caller knows — where its arguments are and where the
    // result goes. Where the callee keeps its parameters is on the callee, since which
    // binding of it a call reaches is elaboration's to say.
    SubprogramBuilder callee("scale");
    const std::uint32_t wide = callee.reg(Type::bits(128, false, true));
    const std::uint32_t factor =
        callee.parameter("factor", Type::bits(8, false, true), ParameterMode::InOut);
    const std::uint32_t result = callee.result(Type::bits(8, false, true));
    callee.block("entry");
    callee.emit(Instruction::make(Opcode::Move, Operand::reg(result), Operand::reg(factor)));
    callee.emit(Instruction::make(Opcode::Return));
    static_cast<void>(wide);

    ProcessBuilder caller(ProcessKind::Initial, "caller");
    const std::uint32_t padding = caller.reg(Type::bits(64, false, true));
    const std::uint32_t argument = caller.reg(Type::bits(8, false, true));
    const std::uint32_t returned = caller.reg(Type::bits(8, false, true));
    caller.block("entry");
    caller.call("", "scale", {Operand::reg(argument)}, Operand::reg(returned));
    caller.emit(Instruction::make(Opcode::Return));
    static_cast<void>(padding);

    ModuleDefinition definition = makeModule(caller.finish());
    definition.subprograms.push_back(callee.finish());
    design.addModule(std::move(definition));

    const std::unique_ptr<CompiledDesign> compiled = compile();
    ASSERT_NE(compiled, nullptr) << consumer.messages.front();

    const std::uint32_t calleeIndex = compiled->subprogramBodyIndex(0, 0);
    ASSERT_NE(calleeIndex, CompiledDesign::NotCompiled);
    const enzan::codegen::CompiledBody& calleeBody = compiled->body(calleeIndex);
    // A 128-bit register takes four Words, both planes of two, before the parameter's.
    ASSERT_EQ(calleeBody.parameters.size(), 1U);
    EXPECT_EQ(calleeBody.parameters[0].offset, 4U);
    EXPECT_EQ(calleeBody.parameters[0].bitWidth, 8U);
    EXPECT_EQ(calleeBody.parameters[0].mode, ParameterMode::InOut);
    EXPECT_EQ(calleeBody.resultOffset, 6U);
    EXPECT_EQ(calleeBody.resultWidth, 8U);

    const enzan::codegen::CompiledBody& callerBody = compiled->body(compiled->bodyIndex(0, 0));
    ASSERT_EQ(callerBody.calls.size(), 1U);
    const enzan::codegen::CallDescriptor& call = callerBody.calls[0];
    ASSERT_EQ(call.arguments.size(), 1U);
    EXPECT_FALSE(call.arguments[0].isConstant);
    EXPECT_EQ(call.arguments[0].location, 2U);
    EXPECT_TRUE(call.hasResult);
    EXPECT_EQ(call.resultLocation, 4U);
    EXPECT_EQ(callerBody.code[0].opcode, Opcode::Call);
    EXPECT_EQ(callerBody.code[0].a, 0U);
}

} // namespace
