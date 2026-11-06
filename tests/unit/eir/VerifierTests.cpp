// SPDX-License-Identifier: GPL-3.0-or-later
//
// The verifier is how the core invariant — a process is a pure function over a
// declared slice of the state arena — stops being a statement in a document and
// becomes a rule the build enforces. These tests are therefore as much about
// that invariant as about the verifier.

#include "eir/Verifier.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Instruction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::eir::BasicBlock;
using enzan::eir::CallSite;
using enzan::eir::Design;
using enzan::eir::EnumDefinition;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ParameterMode;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::SensitivityEntry;
using enzan::eir::Subprogram;
using enzan::eir::SubprogramParameter;
using enzan::eir::Type;
using enzan::eir::VhdlType;
using enzan::eir::WriteExtent;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { messages.push_back(diagnostic.message); }

    std::vector<std::string> messages;
};

/// A minimal but valid module: two objects and an `always_comb` that copies one to
/// the other. Tests break exactly one property of it and check that the verifier
/// notices, which keeps each test's intent obvious.
class VerifierTest : public ::testing::Test {
public:
    /// The valid module every test breaks one property of.
    static ModuleDefinition validModule() { return makeValidModule(); }

protected:
    VerifierTest() : engine(sources) { engine.addConsumer(consumer); }

    static ModuleDefinition makeValidModule() {
        ModuleDefinition definition;
        definition.name = "copier";
        definition.sourceName = "copier";
        definition.addObject(Object{.name = "source", .type = Type::bits(8, false, true)});
        definition.addObject(Object{.name = "sink", .type = Type::bits(8, false, true)});

        Process process;
        process.kind = ProcessKind::AlwaysComb;
        process.name = "always_comb@copier";
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

        definition.processes.push_back(std::move(process));
        return definition;
    }

    /// A function of one 8-bit parameter returning it doubled, as a module's subprogram.
    static Subprogram makeDoubler() {
        Subprogram doubler;
        doubler.name = "double";
        doubler.registerTypes = {Type::bits(8, false, true), Type::bits(8, false, true)};
        doubler.parameters.push_back(
            SubprogramParameter{.name = "value", .mode = ParameterMode::In, .registerIndex = 0});
        doubler.resultType = Type::bits(8, false, true);
        doubler.resultRegister = 1;

        BasicBlock entry;
        entry.label = "entry";
        entry.instructions.push_back(
            Instruction::make(Opcode::Add, Operand::reg(1), Operand::reg(0), Operand::reg(0)));
        entry.instructions.push_back(Instruction::make(Opcode::Return));
        doubler.blocks.push_back(std::move(entry));
        return doubler;
    }

    /// The valid module, with its process copying `double(source)` to `sink` through a call.
    static ModuleDefinition makeModuleWithCall() {
        ModuleDefinition definition = makeValidModule();
        definition.subprograms.push_back(makeDoubler());

        Process& process = definition.processes[0];
        process.registerTypes.push_back(Type::bits(8, false, true));
        process.calls.push_back(CallSite{.definition = "",
                                         .subprogram = "double",
                                         .arguments = {Operand::reg(0)},
                                         .result = Operand::reg(1),
                                         .location = {}});

        std::vector<Instruction>& code = process.blocks[0].instructions;
        code.insert(code.begin() + 1, Instruction::make(Opcode::Call, {}, Operand::call(0)));
        code[2].b = Operand::reg(1);
        return definition;
    }

    /// Verifies `definition` in isolation and returns whether it passed.
    bool check(const ModuleDefinition& definition) {
        return enzan::eir::verify(definition, nullptr, engine);
    }

    /// True when any reported message contains `fragment`.
    [[nodiscard]] bool reported(std::string_view fragment) const {
        return std::ranges::any_of(consumer.messages, [fragment](const std::string& message) {
            return message.find(fragment) != std::string::npos;
        });
    }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
};

// ---------------------------------------------------------------------------
// The happy path
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, AcceptsAWellFormedModule) {
    EXPECT_TRUE(check(makeValidModule()));
    EXPECT_TRUE(consumer.messages.empty()) << consumer.messages.front();
}

TEST_F(VerifierTest, AcceptsAWholeDesign) {
    Design design;
    design.addModule(makeValidModule());
    EXPECT_TRUE(enzan::eir::verify(design, engine));
}

TEST_F(VerifierTest, AcceptsVhdlTypesThatNameWhatTheModuleHas) {
    ModuleDefinition definition = makeValidModule();
    definition.enums.push_back(EnumDefinition{.name = "BIT", .members = {}});
    definition.vhdlTypes.push_back(
        VhdlType{.kind = VhdlType::Kind::Enumeration, .name = "BIT", .width = 1, .enumIndex = 0});
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                            .name = "BIT_VECTOR",
                                            .width = 8,
                                            .ranges = {{.left = 7, .right = 0, .ascending = false}},
                                            .element = 0});
    definition.objects[0].vhdlType = 1;
    EXPECT_TRUE(check(definition)) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAVhdlTypeIndexOutsideTheTable) {
    // VHPI follows these indices to answer what an object is; one past the table would be read
    // as whatever memory follows it.
    ModuleDefinition definition = makeValidModule();
    definition.vhdlTypes.push_back(
        VhdlType{.kind = VhdlType::Kind::Array, .name = "BIT_VECTOR", .width = 8, .element = 5});
    definition.objects[0].vhdlType = 3;

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("is of VHDL type 3")) << consumer.messages.front();
    EXPECT_TRUE(reported("names an element, a field or an enumeration"));
}

// ---------------------------------------------------------------------------
// The process purity invariant
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, RejectsAReadOfAnUndeclaredObject) {
    // The invariant that matters most: a process that touches an object it did not
    // declare is not a pure function over a known slice of the arena, and every
    // optimisation built on read and write sets would be unsound.
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("not in the declared read set")) << consumer.messages.front();
    EXPECT_TRUE(reported("source"));
}

TEST_F(VerifierTest, RejectsAWriteToAnUndeclaredObject) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].writeSet.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("not in the declared write set"));
}

TEST_F(VerifierTest, RejectsReadingAnObjectDeclaredOnlyAsWritten) {
    // A read set and a write set are not interchangeable: confusing them would make
    // the dependency graph point the wrong way.
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet = {1};
    definition.processes[0].writeSet = {WriteExtent::whole(0, 8)};

    EXPECT_FALSE(check(definition));
}

TEST_F(VerifierTest, RejectsAnUnsortedDeclaredSet) {
    // Sortedness is relied on by the binary search here and by set intersection in
    // the scheduler, and it makes iteration order reproducible.
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet = {1, 0};
    definition.processes[0].writeSet = {WriteExtent::whole(1, 8)};
    definition.processes[0].blocks[0].instructions[0].a = Operand::object(1);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("not sorted"));
}

TEST_F(VerifierTest, RejectsADuplicateInADeclaredSet) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet = {0, 0};

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("duplicate"));
}

/// The valid module, with an `initial` forcing `sink` to `source`, held by an override process
/// that writes it again: the processes are the copier, the holder and the statement.
ModuleDefinition makeModuleWithForce() {
    ModuleDefinition definition = VerifierTest::validModule();

    Process holder;
    holder.kind = ProcessKind::Override;
    holder.name = "force@copier";
    holder.registerTypes.push_back(Type::bits(8, false, true));
    holder.readSet = {0};
    holder.overrideSet = {1};
    BasicBlock again;
    again.label = "entry";
    again.instructions.push_back(
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(0)));
    again.instructions.push_back(
        Instruction::make(Opcode::Override, {}, Operand::object(1), Operand::reg(0)));
    again.instructions.push_back(Instruction::make(Opcode::Return));
    holder.blocks.push_back(std::move(again));
    definition.processes.push_back(std::move(holder));

    Process statement;
    statement.kind = ProcessKind::Initial;
    statement.name = "initial@copier";
    statement.registerTypes.push_back(Type::bits(8, false, true));
    statement.readSet = {0};
    statement.overrideSet = {1};
    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(0)));
    entry.instructions.push_back(Instruction::make(Opcode::Force, {}, Operand::object(1),
                                                   Operand::reg(0), Operand::immediate(1)));
    entry.instructions.push_back(Instruction::make(Opcode::Release, {}, Operand::object(1)));
    entry.instructions.push_back(Instruction::make(Opcode::Return));
    statement.blocks.push_back(std::move(entry));
    definition.processes.push_back(std::move(statement));
    return definition;
}

TEST_F(VerifierTest, RejectsAPrimitiveWhoseTableIsNotOfItsInputs) {
    // Eight bits a field, a field per input and two more a row: a table for two inputs is
    // 32 bits a row, so one of 24 bits is a table for one input, not two.
    ModuleDefinition definition = makeValidModule();
    Process& process = definition.processes[0];
    process.registerTypes = {Type::bits(2, false, true), Type::logic()};
    process.constants = {enzan::eir::BitVector::filled(24, enzan::eir::Logic::Zero)};
    process.writeSet.clear();
    std::vector<Instruction>& code = process.blocks[0].instructions;
    code = {Instruction::make(Opcode::UdpCombinational, Operand::reg(1), Operand::reg(0),
                              Operand::constant(0)),
            Instruction::make(Opcode::Return)};
    process.readSet.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("rows of 2 inputs"));

    process.constants = {enzan::eir::BitVector::filled(32, enzan::eir::Logic::Zero)};
    consumer.messages.clear();
    EXPECT_TRUE(check(definition)) << consumer.messages.front();
}

TEST_F(VerifierTest, AcceptsAForceHeldByAnOverrideProcess) {
    EXPECT_TRUE(check(makeModuleWithForce())) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAnOverrideOfAnObjectNotInTheOverrideSet) {
    // Not in the write set either, which an override does not go through: what it names is
    // declared beside it.
    ModuleDefinition definition = makeModuleWithForce();
    definition.processes[2].overrideSet.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("not in the declared override set"));
}

TEST_F(VerifierTest, RejectsAForceWhoseHolderIsNotAnOverrideProcess) {
    // The kernel wakes the holder when a release re-establishes an assign, so it must be the
    // process that evaluates the value again, not whatever the index happens to reach.
    ModuleDefinition definition = makeModuleWithForce();
    definition.processes[2].blocks[0].instructions[1].c = Operand::immediate(0);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("not an override process"));
}

TEST_F(VerifierTest, RejectsAnOverrideInASubprogram) {
    // A subprogram has no process of its own to hold an override.
    ModuleDefinition definition = makeValidModule();
    Subprogram releaser;
    releaser.name = "let_go";
    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(Instruction::make(Opcode::Release, {}, Operand::object(1)));
    entry.instructions.push_back(Instruction::make(Opcode::Return));
    releaser.blocks.push_back(std::move(entry));
    definition.subprograms.push_back(std::move(releaser));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("only a process may"));
}

TEST_F(VerifierTest, AcceptsAWriteSetOfSeveralDisjointExtentsOfOneObject) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].writeSet = {WriteExtent{.objectIndex = 1, .lowBit = 0, .width = 2},
                                        WriteExtent{.objectIndex = 1, .lowBit = 4, .width = 4}};

    EXPECT_TRUE(check(definition)) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAnExtentPastTheEndOfItsObject) {
    // An extent claiming bits the object does not have would be a driver of nothing, and
    // elaboration would compare it against drivers of real bits.
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].writeSet = {WriteExtent{.objectIndex = 1, .lowBit = 4, .width = 5}};

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("bits [8:4] of 'sink'"));
}

TEST_F(VerifierTest, RejectsAnEmptyExtent) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].writeSet = {WriteExtent{.objectIndex = 1, .lowBit = 0, .width = 0}};

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("empty extent"));
}

TEST_F(VerifierTest, RejectsTouchingExtentsOfOneObject) {
    // Normalisation merges them, so an unmerged pair is a write set nobody normalised, and
    // elaboration would take one process for two drivers.
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].writeSet = {WriteExtent{.objectIndex = 1, .lowBit = 0, .width = 4},
                                        WriteExtent{.objectIndex = 1, .lowBit = 4, .width = 4}};

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("not normalised"));
}

TEST_F(VerifierTest, RejectsUnorderedExtents) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet = {};
    definition.processes[0].blocks[0].instructions.erase(
        definition.processes[0].blocks[0].instructions.begin());
    definition.processes[0].blocks[0].instructions[0].b = Operand::reg(0);
    definition.processes[0].writeSet = {WriteExtent::whole(1, 8), WriteExtent::whole(0, 8)};

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("not normalised"));
}

TEST_F(VerifierTest, RejectsADeclaredSetNamingANonexistentObject) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet = {0, 99};

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("the module has 2 objects"));
}

TEST_F(VerifierTest, RejectsSensitivityToAnObjectNotRead) {
    // A process triggered by a signal depends on that signal, so it belongs in the
    // read set; otherwise activity-based scheduling would draw the graph wrongly.
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].sensitivity.push_back(
        SensitivityEntry{.objectIndex = 1, .edge = enzan::eir::EdgeKind::AnyChange});

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("does not declare it in its read set"));
}

TEST_F(VerifierTest, AcceptsSensitivityToAnObjectThatIsRead) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].sensitivity.push_back(
        SensitivityEntry{.objectIndex = 0, .edge = enzan::eir::EdgeKind::Posedge});

    EXPECT_TRUE(check(definition));
}

// ---------------------------------------------------------------------------
// Structural well-formedness
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, RejectsAProcessWithNoBlocks) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("no basic blocks"));
}

TEST_F(VerifierTest, RejectsAnEmptyBlock) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("is empty"));
}

TEST_F(VerifierTest, RejectsABlockThatDoesNotEndInATerminator) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions.pop_back();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("is not a terminator but ends its block"));
}

TEST_F(VerifierTest, RejectsATerminatorInTheMiddleOfABlock) {
    ModuleDefinition definition = makeValidModule();
    auto& instructions = definition.processes[0].blocks[0].instructions;
    instructions.insert(instructions.begin(), Instruction::make(Opcode::Return));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("in the middle of its block"));
}

TEST_F(VerifierTest, RejectsABranchToANonexistentBlock) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions.back() =
        Instruction::make(Opcode::Jump, {}, Operand::block(7));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("branches to block 7"));
}

TEST_F(VerifierTest, RejectsAnOutOfRangeRegister) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions[0].dst = Operand::reg(5);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("register 5"));
}

TEST_F(VerifierTest, RejectsAnOutOfRangeConstant) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions[0] =
        Instruction::make(Opcode::LoadConstant, Operand::reg(0), Operand::constant(3));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("constant 3"));
}

TEST_F(VerifierTest, RejectsAMissingDestination) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions[0].dst = Operand::none();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("missing a destination"));
}

TEST_F(VerifierTest, RejectsADestinationOnAnOperationThatHasNoResult) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions[1].dst = Operand::reg(0);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("must not have a destination"));
}

TEST_F(VerifierTest, RejectsTheWrongNumberOfSources) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].blocks[0].instructions[0].b = Operand::reg(0);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("expects 1 source operands"));
}

// ---------------------------------------------------------------------------
// Width rules
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, RejectsMixedWidthsInABinaryOperation) {
    // The frontend inserts the extensions SystemVerilog's context-determined width
    // rules call for. A mismatch here means those rules were applied wrongly, which
    // would otherwise surface as a wrong value rather than as a crash.
    ModuleDefinition definition = makeValidModule();
    Process& process = definition.processes[0];
    process.registerTypes.push_back(Type::bits(4, false, true));
    process.registerTypes.push_back(Type::bits(8, false, true));

    auto& instructions = process.blocks[0].instructions;
    instructions.insert(
        instructions.begin() + 1,
        Instruction::make(Opcode::Add, Operand::reg(2), Operand::reg(0), Operand::reg(1)));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("requires all three to be equal"));
}

TEST_F(VerifierTest, RejectsAnOperationMixingARealWithBits) {
    // Adding a real to 64 bits would read the bits as a double, or the double as bits; which
    // was meant is the frontend's to say with a Convert.
    ModuleDefinition definition = makeValidModule();
    Process& process = definition.processes[0];
    process.registerTypes.push_back(Type::real());
    process.registerTypes.push_back(Type::bits(64, true, false));
    process.registerTypes.push_back(Type::real());

    auto& instructions = process.blocks[0].instructions;
    instructions.insert(
        instructions.begin() + 1,
        Instruction::make(Opcode::Add, Operand::reg(3), Operand::reg(1), Operand::reg(2)));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("mixes reals with bits"));
    EXPECT_EQ(consumer.messages.size(), 1U);
}

TEST_F(VerifierTest, RejectsAConversionWithARealOnNeitherSide) {
    ModuleDefinition definition = makeValidModule();
    Process& process = definition.processes[0];
    process.registerTypes.push_back(Type::bits(64, true, false));
    process.registerTypes.push_back(Type::bits(64, true, false));

    auto& instructions = process.blocks[0].instructions;
    instructions.insert(instructions.begin() + 1,
                        Instruction::make(Opcode::Convert, Operand::reg(1), Operand::reg(2)));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("converts between two bits"));
    EXPECT_EQ(consumer.messages.size(), 1U);
}

/// The valid module with a register of 64 bits of codes and one of their 8 bits, and `opcode`
/// converting between them with `encoding` as its second operand.
ModuleDefinition withConversion(Opcode opcode, Operand encoding, std::uint32_t bits) {
    ModuleDefinition definition = VerifierTest::validModule();
    Process& process = definition.processes[0];
    process.registerTypes.push_back(Type::bits(64, false, false));
    process.registerTypes.push_back(Type::bits(bits, false, true));
    const bool present = opcode == Opcode::Present;
    auto& instructions = process.blocks[0].instructions;
    instructions.insert(instructions.begin() + 1,
                        Instruction::make(opcode, Operand::reg(present ? 2 : 1),
                                          Operand::reg(present ? 1 : 2), encoding));
    return definition;
}

TEST_F(VerifierTest, AcceptsCodesPresentedAsBitsAndBitsEncodedAsCodes) {
    const Operand logic =
        Operand::immediate(static_cast<std::uint32_t>(enzan::eir::Encoding::VhdlLogic));
    EXPECT_TRUE(check(withConversion(Opcode::Present, logic, 8))) << consumer.messages.front();
    EXPECT_TRUE(check(withConversion(Opcode::Encode, logic, 8))) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsCodesThatAreNotAByteForEachBit) {
    const Operand logic =
        Operand::immediate(static_cast<std::uint32_t>(enzan::eir::Encoding::VhdlLogic));
    EXPECT_FALSE(check(withConversion(Opcode::Present, logic, 4)));
    EXPECT_TRUE(reported("eight times as wide"));
}

TEST_F(VerifierTest, RejectsAConversionNamingNoEncodingOfCodes) {
    const Operand bits = Operand::immediate(static_cast<std::uint32_t>(enzan::eir::Encoding::Bits));
    EXPECT_FALSE(check(withConversion(Opcode::Encode, bits, 8)));
    EXPECT_TRUE(reported("names no encoding of codes"));
}

TEST_F(VerifierTest, RejectsAWidePredicateDestination) {
    ModuleDefinition definition = makeValidModule();
    Process& process = definition.processes[0];

    auto& instructions = process.blocks[0].instructions;
    instructions.insert(
        instructions.begin() + 1,
        Instruction::make(Opcode::Equal, Operand::reg(0), Operand::reg(0), Operand::reg(0)));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("produces a single bit"));
}

TEST_F(VerifierTest, AcceptsAOneBitPredicateDestination) {
    ModuleDefinition definition = makeValidModule();
    Process& process = definition.processes[0];
    process.registerTypes.push_back(Type::logic());

    auto& instructions = process.blocks[0].instructions;
    instructions.insert(
        instructions.begin() + 1,
        Instruction::make(Opcode::Equal, Operand::reg(1), Operand::reg(0), Operand::reg(0)));

    EXPECT_TRUE(check(definition));
}

// ---------------------------------------------------------------------------
// Ports and instances
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, RejectsAPortNamingANonexistentObject) {
    ModuleDefinition definition = makeValidModule();
    definition.ports.push_back(enzan::eir::Port{
        .name = "p", .direction = enzan::eir::PortDirection::Input, .objectIndex = 42});

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("names object 42"));
}

TEST_F(VerifierTest, RejectsAnInstanceConnectingToANonexistentObject) {
    ModuleDefinition definition = makeValidModule();
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 42});
    definition.instances.push_back(std::move(instance));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("connects a port to object 42"));
}

TEST_F(VerifierTest, RejectsAnInstanceDrivingANonexistentObject) {
    ModuleDefinition definition = makeValidModule();
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(
        enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0, .driveObjectIndex = 42});
    definition.instances.push_back(std::move(instance));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("drives a port's object 42, but the module has"));
}

TEST_F(VerifierTest, RejectsAnUnconnectedPortThatDrivesAnObject) {
    ModuleDefinition definition = makeValidModule();
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(
        enzan::eir::PortConnection{.portIndex = 0, .driveObjectIndex = 0});
    definition.instances.push_back(std::move(instance));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("but the port is unconnected"));
}

TEST_F(VerifierTest, RejectsAnInstanceBindingAPortThatDoesNotExist) {
    Design design;

    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "in", .type = Type::logic()});
    child.ports.push_back(enzan::eir::Port{
        .name = "in", .direction = enzan::eir::PortDirection::Input, .objectIndex = 0});
    design.addModule(std::move(child));

    ModuleDefinition parent = makeValidModule();
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 3, .objectIndex = 0});
    parent.instances.push_back(std::move(instance));
    design.addModule(std::move(parent));

    EXPECT_FALSE(enzan::eir::verify(design, engine));
    EXPECT_TRUE(reported("binds port index 3"));
}

TEST_F(VerifierTest, AnUnresolvedInstanceReferenceIsNotAVerifierError) {
    // Analysis is per-design-unit and cacheable, so a module may be analysed before
    // the thing it instantiates exists in any library. Elaboration is where an
    // unresolved reference becomes an error.
    Design design;
    ModuleDefinition parent = makeValidModule();
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "not_analysed_yet";
    parent.instances.push_back(std::move(instance));
    design.addModule(std::move(parent));

    EXPECT_TRUE(enzan::eir::verify(design, engine));
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, ReportsEveryProblemRatherThanStoppingAtTheFirst) {
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet.clear();
    definition.processes[0].writeSet.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_GE(consumer.messages.size(), 2U);
}

TEST_F(VerifierTest, ReportsNameTheModuleAndTheProcess) {
    // An EIR defect is a defect in Enzan, so the report has to be enough to find it.
    ModuleDefinition definition = makeValidModule();
    definition.processes[0].readSet.clear();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("module 'copier'"));
    EXPECT_TRUE(reported("process 'always_comb@copier'"));
}

// ---------------------------------------------------------------------------
// Subprograms and calls
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, AcceptsAFunctionAndACallOfIt) {
    EXPECT_TRUE(check(makeModuleWithCall()));
    EXPECT_TRUE(consumer.messages.empty()) << consumer.messages.front();
}

TEST_F(VerifierTest, HoldsASubprogramToItsOwnDeclaredSets) {
    // The core invariant is a body's, whichever kind of body it is.
    ModuleDefinition definition = makeModuleWithCall();
    Subprogram& doubler = definition.subprograms[0];
    doubler.registerTypes.push_back(Type::bits(8, false, true));
    std::vector<Instruction>& code = doubler.blocks[0].instructions;
    code.insert(code.begin(),
                Instruction::make(Opcode::ReadObject, Operand::reg(2), Operand::object(0)));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("subprogram 'double'"));
    EXPECT_TRUE(reported("not in the declared read set"));
}

TEST_F(VerifierTest, RejectsASubprogramThatSuspends) {
    // Its frame lives on the kernel's one stack, which a suspension would leave holding it.
    ModuleDefinition definition = makeModuleWithCall();
    Subprogram& doubler = definition.subprograms[0];
    std::vector<Instruction>& code = doubler.blocks[0].instructions;
    code.back() = Instruction::make(Opcode::WaitTime, {}, Operand::reg(0), Operand::block(1));
    BasicBlock resumed;
    resumed.label = "resumed";
    resumed.instructions.push_back(Instruction::make(Opcode::Return));
    doubler.blocks.push_back(std::move(resumed));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("which a subprogram may not; one that waits is inlined"));
}

TEST_F(VerifierTest, RejectsANonBlockingStoreInASubprogram) {
    ModuleDefinition definition = makeModuleWithCall();
    Subprogram& doubler = definition.subprograms[0];
    doubler.writeSet = {WriteExtent::whole(1, 8)};
    std::vector<Instruction>& code = doubler.blocks[0].instructions;
    code.insert(code.end() - 1, Instruction::make(Opcode::StoreNonBlocking, {}, Operand::object(1),
                                                  Operand::reg(1)));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("schedules a write, which a subprogram may not"));
}

TEST_F(VerifierTest, RejectsACallOfASubprogramTheDefinitionDoesNotDeclare) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.processes[0].calls[0].subprogram = "triple";

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("calls 'triple', which 'copier' does not declare"));
}

TEST_F(VerifierTest, RejectsAnArgumentOfAnotherWidthThanItsParameter) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.subprograms[0].registerTypes[0] = Type::bits(16, false, true);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("passes 8 bits to parameter 'value', which is 16 bits"));
}

TEST_F(VerifierTest, RejectsTheWrongNumberOfArguments) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.processes[0].calls[0].arguments.push_back(Operand::reg(0));

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("passes 2 arguments to 1 parameters"));
}

TEST_F(VerifierTest, RejectsAnOutArgumentWithNowhereToCopyItBack) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.subprograms[0].parameters[0].mode = ParameterMode::InOut;
    Process& process = definition.processes[0];
    process.constants.push_back(enzan::eir::BitVector::fromUInt64(3, 8));
    process.calls[0].arguments[0] = Operand::constant(0);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("passes its inout parameter 'value' something other than a register"));
}

TEST_F(VerifierTest, RejectsAFunctionWhoseResultIsDropped) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.processes[0].calls[0].result = Operand::none();

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("drops the result of a function"));
}

TEST_F(VerifierTest, RejectsAResultRegisterOfAnotherTypeThanTheResult) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.subprograms[0].resultType = Type::bits(8, true, true);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("but the register holding it is"));
}

TEST_F(VerifierTest, RejectsTwoSubprogramsOfOneName) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.subprograms.push_back(makeDoubler());

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("declares two subprograms called 'double'"));
}

TEST_F(VerifierTest, RejectsACallInstructionNamingNoCallSite) {
    ModuleDefinition definition = makeModuleWithCall();
    definition.processes[0].blocks[0].instructions[1].a = Operand::call(1);

    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("names a call site the process does not have"));
}

TEST_F(VerifierTest, HoldsACallIntoAnotherDefinitionToItsCallee) {
    // The callee is a package's, which the design gathers; with it in reach, the call is
    // checked as one into its own definition would be.
    ModuleDefinition package;
    package.name = "arith::";
    package.sourceName = "arith";
    package.isPackage = true;
    package.subprograms.push_back(makeDoubler());
    package.subprograms[0].registerTypes[0] = Type::bits(4, false, true);

    ModuleDefinition caller = makeModuleWithCall();
    caller.subprograms.clear();
    caller.processes[0].calls[0].definition = "arith::";

    Design design;
    design.addModule(std::move(package));
    design.addModule(std::move(caller));

    EXPECT_FALSE(enzan::eir::verify(design, engine));
    EXPECT_TRUE(reported("passes 8 bits to parameter 'value', which is 4 bits"));
}

TEST_F(VerifierTest, LeavesACallIntoAnUnknownDefinitionToElaboration) {
    // As an instance of a module no library has yet is: analysis is per unit, and the callee
    // may be in a library gathered later.
    ModuleDefinition definition = makeModuleWithCall();
    definition.subprograms.clear();
    definition.processes[0].calls[0].definition = "elsewhere::";

    EXPECT_TRUE(check(definition));
}

} // namespace
