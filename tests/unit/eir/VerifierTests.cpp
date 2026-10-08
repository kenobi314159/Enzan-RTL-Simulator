// SPDX-License-Identifier: GPL-3.0-or-later
//
// The verifier is how the core invariant of DESIGN_GUIDE.md section 4.2 — a process
// is a pure function over a declared slice of the state arena — stops being a
// statement in a document and becomes a rule the build enforces. These tests are
// therefore as much about that invariant as about the verifier.

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
using enzan::eir::Design;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::SensitivityEntry;
using enzan::eir::Type;
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

    /// `makeValidModule` made generic over one formal, `W`, which both objects' widths are.
    static ModuleDefinition makeGenericModule() {
        ModuleDefinition definition = makeValidModule();
        const std::uint32_t width = definition.expressions.formal(0, 32, true);
        definition.formals.push_back(enzan::eir::ParameterFormal{
            .name = "W",
            .value =
                definition.expressions.constant(enzan::eir::BitVector::fromInt64(8, 32), true)});
        for (Object& object : definition.objects) object.type = object.type.withWidthExpression(width);
        definition.processes[0].registerTypes[0] =
            definition.processes[0].registerTypes[0].withWidthExpression(width);
        definition.processes[0].writeSet[0].widthExpression = width;
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

TEST_F(VerifierTest, AcceptsAConstantFunctionOfItsShape) {
    // Arguments in, one result out, one initial process computing it: what running it relies
    // on to know where the arguments go and where the result is.
    ModuleDefinition function = makeValidModule();
    function.isFunction = true;
    function.processes[0].kind = ProcessKind::Initial;
    function.ports = {enzan::eir::Port{.name = "source", .objectIndex = 0},
                      enzan::eir::Port{.name = "sink",
                                       .direction = enzan::eir::PortDirection::Output,
                                       .objectIndex = 1}};
    EXPECT_TRUE(check(function));
    EXPECT_TRUE(consumer.messages.empty()) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAConstantFunctionOfAnotherShape) {
    ModuleDefinition function = makeValidModule();
    function.isFunction = true;
    function.ports = {enzan::eir::Port{.name = "sink", .objectIndex = 1}};
    EXPECT_FALSE(check(function));
    EXPECT_TRUE(reported("then one output"));
    EXPECT_TRUE(reported("one initial process"));
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
    // the scheduler, and it makes iteration order reproducible (section 3.8).
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
// Generic units (DESIGN_GUIDE.md section 18, M5.1)
// ---------------------------------------------------------------------------

TEST_F(VerifierTest, AcceptsAGenericUnit) {
    EXPECT_TRUE(check(makeGenericModule()));
    EXPECT_TRUE(consumer.messages.empty()) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAnAnnotationNamingNoExpression) {
    ModuleDefinition definition = makeGenericModule();
    definition.objects[1].type = definition.objects[1].type.withWidthExpression(42);
    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("object 'sink' names expression 42")) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAnnotationsOfSomeOfAProcesssConstants) {
    // The annotations are by constant, so a shorter list would leave the specialiser reading
    // another constant's.
    ModuleDefinition definition = makeGenericModule();
    definition.processes[0].constants = {enzan::eir::BitVector::fromUInt64(0, 8),
                                         enzan::eir::BitVector::fromUInt64(1, 8)};
    definition.processes[0].constantExpressions = {enzan::eir::NoExpression};
    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("annotates 1 of its 2 constants")) << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAFormalDefinedByItself) {
    // Elaboration evaluates formals in order, so a default naming its own formal, or a later
    // one, would read a value that does not exist yet.
    ModuleDefinition definition = makeGenericModule();
    definition.formals[0].value = definition.expressions.formal(0, 32, true);
    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("formal 'W' is defined by a formal at or after itself"))
        << consumer.messages.front();
}

TEST_F(VerifierTest, RejectsAnExpressionUsingAFormalTheUnitLacks) {
    ModuleDefinition definition = makeGenericModule();
    definition.objects[0].type =
        definition.objects[0].type.withWidthExpression(definition.expressions.formal(3, 32, true));
    EXPECT_FALSE(check(definition));
    EXPECT_TRUE(reported("uses formal 3, but the unit has 1 formals")) << consumer.messages.front();
}

} // namespace
