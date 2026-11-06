// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Elaborator.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include "support/Optionals.h"
#include "support/ProcessBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <initializer_list>
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
using enzan::eir::ObjectKind;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::Port;
using enzan::eir::PortConnection;
using enzan::eir::PortDirection;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::eir::WriteExtent;
using enzan::elab::ElaboratedDesign;
using enzan::elab::Elaborator;
using enzan::elab::Instance;
using enzan::elab::SlotRange;
using enzan::elab::SubprogramInstance;
using enzan::test::ProcessBuilder;
using enzan::test::require;
using enzan::test::SubprogramBuilder;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
        codes.push_back(diagnostic.code);
        for (const enzan::DiagnosticNote& note : diagnostic.notes) notes.push_back(note.message);
    }

    std::vector<std::string> messages;
    std::vector<enzan::DiagCode> codes;
    std::vector<std::string> notes;
};

/// Builds a process that copies one object to another, with the declared read and
/// write sets the verifier requires.
Process makeCopyProcess(ProcessKind kind, std::uint32_t from, std::uint32_t to, Type type) {
    Process process;
    process.kind = kind;
    process.name = "copy";
    process.registerTypes.push_back(type);
    process.readSet = {from};
    process.writeSet = {WriteExtent::whole(to, type.bitWidth())};

    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(from)));
    entry.instructions.push_back(
        Instruction::make(Opcode::Store, {}, Operand::object(to), Operand::reg(0)));
    entry.instructions.push_back(Instruction::make(Opcode::Return));
    process.blocks.push_back(std::move(entry));
    return process;
}

/// Builds a process that copies `from`, which is `width` bits, into bits `lowBit` upwards of
/// `to`, declaring exactly those bits as written.
Process makePartDriver(ProcessKind kind, std::uint32_t from, std::uint32_t to, std::uint32_t lowBit,
                       std::uint32_t width) {
    Process process;
    process.kind = kind;
    process.name = std::format("part{}", lowBit);
    process.registerTypes.push_back(Type::bits(width, false, true));
    process.registerTypes.push_back(Type::bits(32, false, false));
    process.constants.push_back(enzan::eir::BitVector::fromUInt64(lowBit, 32));
    process.readSet = {from};
    process.writeSet = {WriteExtent{.objectIndex = to, .lowBit = lowBit, .width = width}};

    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(
        Instruction::make(Opcode::ReadObject, Operand::reg(0), Operand::object(from)));
    entry.instructions.push_back(
        Instruction::make(Opcode::LoadConstant, Operand::reg(1), Operand::constant(0)));
    entry.instructions.push_back(Instruction::make(Opcode::StorePart, {}, Operand::object(to),
                                                   Operand::reg(0), Operand::reg(1)));
    entry.instructions.push_back(Instruction::make(Opcode::Return));
    process.blocks.push_back(std::move(entry));
    return process;
}

class ElaboratorTest : public ::testing::Test {
protected:
    ElaboratorTest() : engine(sources) { engine.addConsumer(consumer); }

    std::unique_ptr<ElaboratedDesign> elaborate(std::string_view top = "") {
        Elaborator elaborator(design, engine);
        return elaborator.elaborate(top);
    }

    [[nodiscard]] bool reportedCode(enzan::DiagCode code) const {
        return std::ranges::find(consumer.codes, code) != consumer.codes.end();
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

// ---------------------------------------------------------------------------
// Choosing a top level
// ---------------------------------------------------------------------------

TEST_F(ElaboratorTest, ElaboratesASingleModule) {
    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    top.addObject(Object{.name = "a", .type = Type::bits(8, false, true)});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("top");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    EXPECT_EQ(elaborated->topModuleName(), "top");
    ASSERT_EQ(elaborated->instances().size(), 1U);
    EXPECT_EQ(elaborated->instances()[0].path, "/top");
    EXPECT_EQ(elaborated->instances()[0].parent, Instance::NoParent);
    EXPECT_EQ(elaborated->slots().size(), 1U);
    EXPECT_EQ(elaborated->findSlot("/top/a"), 0U);
}

TEST_F(ElaboratorTest, AParameterisedTopIsNamedAsTheSourceNamesIt) {
    // Its definition is named for its parameter values; its instance, and so every path
    // under it, is named for what the user wrote.
    ModuleDefinition top;
    top.name = "counter$WIDTH_8";
    top.sourceName = "counter";
    top.addObject(Object{.name = "count", .type = Type::bits(8, false, true)});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("counter");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    EXPECT_EQ(elaborated->instances()[0].path, "/counter");
    EXPECT_EQ(elaborated->instances()[0].name, "counter");
    EXPECT_EQ(elaborated->topModuleName(), "counter") << "the name a window title shows";
    EXPECT_EQ(elaborated->findSlot("/counter/count"), 0U);
}

TEST_F(ElaboratorTest, ChoosesTheOnlyCandidateTopWhenNoneIsNamed) {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.sourceName = "leaf";
    design.addModule(std::move(leaf));

    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    top.instances.push_back(enzan::eir::Instance{.name = "u", .definitionName = "leaf"});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate();
    ASSERT_NE(elaborated, nullptr);
    EXPECT_EQ(elaborated->topModuleName(), "top");
}

TEST_F(ElaboratorTest, RefusesToGuessBetweenSeveralCandidateTops) {
    // Guessing a top level is the kind of convenience that costs an hour of confusion
    // the first time it guesses wrong, so the candidates are reported instead.
    ModuleDefinition one;
    one.name = "one";
    design.addModule(std::move(one));
    ModuleDefinition two;
    two.name = "two";
    design.addModule(std::move(two));

    EXPECT_EQ(elaborate(), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::NoTopModule));
    EXPECT_TRUE(reported("'one'")) << "the candidates must be listed";
    EXPECT_TRUE(reported("'two'"));
}

TEST_F(ElaboratorTest, SeveralTopLevelsAreSeveralRoots) {
    // As a design and AMD's `glbl` are, in the order given and each once.
    ModuleDefinition testbench;
    testbench.name = "tb";
    testbench.sourceName = "tb";
    testbench.addObject(Object{.name = "clk", .type = Type::bits(1, false, true)});
    design.addModule(std::move(testbench));
    ModuleDefinition glbl;
    glbl.name = "glbl$values";
    glbl.sourceName = "glbl";
    glbl.addObject(Object{.name = "GSR", .type = Type::bits(1, false, true)});
    design.addModule(std::move(glbl));

    const std::vector<std::string> tops{"tb", "glbl", "tb"};
    Elaborator elaborator(design, engine);
    const std::unique_ptr<ElaboratedDesign> elaborated =
        elaborator.elaborate(std::span<const std::string>(tops));
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    EXPECT_EQ(elaborated->topModuleName(), "tb") << "the first, the design's own";
    EXPECT_EQ(std::vector<std::string>(elaborated->topModuleNames().begin(),
                                       elaborated->topModuleNames().end()),
              (std::vector<std::string>{"tb", "glbl"}));
    ASSERT_EQ(elaborated->instances().size(), 2U);
    EXPECT_EQ(elaborated->instances()[1].path, "/glbl");
    EXPECT_EQ(elaborated->instances()[1].parent, Instance::NoParent);
    EXPECT_TRUE(elaborated->findSlot("/glbl/GSR").has_value());
}

TEST_F(ElaboratorTest, SaysGlblIsATopLevelOfItsOwnWhenNoTopIsNamed) {
    ModuleDefinition testbench;
    testbench.name = "tb";
    testbench.sourceName = "tb";
    design.addModule(std::move(testbench));
    ModuleDefinition glbl;
    glbl.name = "glbl$values";
    glbl.sourceName = "glbl";
    design.addModule(std::move(glbl));

    EXPECT_EQ(elaborate(), nullptr);
    EXPECT_TRUE(reported("'glbl', 'tb'")) << "by the names the source gives them";
    ASSERT_EQ(consumer.notes.size(), 1U);
    EXPECT_NE(consumer.notes.front().find("--top <design> --top glbl"), std::string::npos)
        << consumer.notes.front();
}

TEST_F(ElaboratorTest, ReportsAnUnknownTopModuleAndListsWhatExists) {
    ModuleDefinition top;
    top.name = "counter";
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("cuonter"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::TopModuleNotFound));
    EXPECT_TRUE(reported("'counter'")) << "a typo is only fixable if the real names are shown";
}

// ---------------------------------------------------------------------------
// The instance tree
// ---------------------------------------------------------------------------

TEST_F(ElaboratorTest, BuildsAHierarchyWithSlashSeparatedPaths) {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.addObject(Object{.name = "q", .type = Type::logic()});
    design.addModule(std::move(leaf));

    ModuleDefinition middle;
    middle.name = "middle";
    middle.instances.push_back(enzan::eir::Instance{.name = "u_leaf", .definitionName = "leaf"});
    design.addModule(std::move(middle));

    ModuleDefinition top;
    top.name = "tb";
    top.instances.push_back(enzan::eir::Instance{.name = "u_mid", .definitionName = "middle"});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    EXPECT_EQ(elaborated->instances().size(), 3U);
    EXPECT_TRUE(elaborated->findInstance("/tb").has_value());
    EXPECT_TRUE(elaborated->findInstance("/tb/u_mid").has_value());
    EXPECT_TRUE(elaborated->findInstance("/tb/u_mid/u_leaf").has_value());
    EXPECT_TRUE(elaborated->findSlot("/tb/u_mid/u_leaf/q").has_value());
}

TEST_F(ElaboratorTest, TheSameModuleMayAppearManyTimes) {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.addObject(Object{.name = "q", .type = Type::logic()});
    design.addModule(std::move(leaf));

    ModuleDefinition top;
    top.name = "tb";
    top.instances.push_back(enzan::eir::Instance{.name = "u0", .definitionName = "leaf"});
    top.instances.push_back(enzan::eir::Instance{.name = "u1", .definitionName = "leaf"});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr);

    // Two instances, two distinct slots: instances share code, never storage.
    EXPECT_EQ(elaborated->instances().size(), 3U);
    const std::uint32_t first = require(elaborated->findSlot("/tb/u0/q"), "slot for /tb/u0/q");
    const std::uint32_t second = require(elaborated->findSlot("/tb/u1/q"), "slot for /tb/u1/q");
    EXPECT_NE(first, second);
}

TEST_F(ElaboratorTest, EnumerationsAreInternedOnceForTheWholeElaboration) {
    // Every instance of a module declares its enumerated signals again, and a design has far
    // more enumerated signals than it has enumerations. One table for the elaboration means a
    // display is told the names once however many slots share them.
    ModuleDefinition leaf;
    leaf.name = "leaf";
    const std::uint32_t leafEnum = leaf.addEnum(enzan::eir::EnumDefinition{
        .name = "state_t",
        .members = {
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            enzan::eir::EnumMember{.name = "A", .value = *enzan::eir::BitVector::parse("2'd0")},
            enzan::eir::EnumMember{.name = "B", .value = *enzan::eir::BitVector::parse("2'd1")}}});
    // NOLINTEND(bugprone-unchecked-optional-access)
    leaf.addObject(
        Object{.name = "state", .type = Type::bits(2, false, true), .enumIndex = leafEnum});
    leaf.addObject(
        Object{.name = "next", .type = Type::bits(2, false, true), .enumIndex = leafEnum});
    leaf.addObject(Object{.name = "plain", .type = Type::bits(2, false, true)});
    design.addModule(std::move(leaf));

    ModuleDefinition top;
    top.name = "tb";
    top.instances.push_back(enzan::eir::Instance{.name = "u0", .definitionName = "leaf"});
    top.instances.push_back(enzan::eir::Instance{.name = "u1", .definitionName = "leaf"});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr);

    ASSERT_EQ(elaborated->enumerations().size(), 1U)
        << "two instances and two signals each still describe one enumeration";
    EXPECT_EQ(elaborated->enumerations()[0].name, "state_t");

    const std::uint32_t first = require(elaborated->findSlot("/tb/u0/state"), "/tb/u0/state");
    const std::uint32_t second = require(elaborated->findSlot("/tb/u1/next"), "/tb/u1/next");
    EXPECT_EQ(elaborated->slots()[first].enumIndex, 0U);
    EXPECT_EQ(elaborated->slots()[second].enumIndex, 0U);

    // And a slot whose object has no enumeration says so, rather than pointing at whichever
    // entry happened to be first.
    const std::uint32_t plain = require(elaborated->findSlot("/tb/u0/plain"), "/tb/u0/plain");
    EXPECT_EQ(elaborated->slots()[plain].enumIndex, enzan::elab::Slot::NoEnum);
}

TEST_F(ElaboratorTest, RejectsRecursiveInstantiation) {
    // An unbounded hierarchy cannot be elaborated, and recursing until the stack runs
    // out would report nothing useful.
    ModuleDefinition self;
    self.name = "loop";
    self.instances.push_back(enzan::eir::Instance{.name = "inner", .definitionName = "loop"});
    design.addModule(std::move(self));

    EXPECT_EQ(elaborate("loop"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::RecursiveInstantiation));
}

TEST_F(ElaboratorTest, ReportsAnInstanceOfAModuleThatWasNeverAnalysed) {
    ModuleDefinition top;
    top.name = "tb";
    top.instances.push_back(enzan::eir::Instance{.name = "u", .definitionName = "missing"});
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::UnresolvedInstance));
    EXPECT_TRUE(reported("missing"));
}

// ---------------------------------------------------------------------------
// The state arena
// ---------------------------------------------------------------------------

TEST_F(ElaboratorTest, AllocatesEverySlotInOneContiguousArena) {
    ModuleDefinition top;
    top.name = "top";
    top.addObject(Object{.name = "narrow", .type = Type::logic()});
    top.addObject(Object{.name = "wide", .type = Type::bits(100, false, true)});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("top");
    ASSERT_NE(elaborated, nullptr);

    // Two planes each: one Word pair for the 1-bit object, two for the 100-bit one.
    ASSERT_EQ(elaborated->slots().size(), 2U);
    EXPECT_EQ(elaborated->slots()[0].wordOffset, 0U);
    EXPECT_EQ(elaborated->slots()[0].type.storageWords(), 2U);
    EXPECT_EQ(elaborated->slots()[1].wordOffset, 2U);
    EXPECT_EQ(elaborated->slots()[1].type.storageWords(), 4U);
    EXPECT_EQ(elaborated->arenaWords(), 6U);
}

TEST_F(ElaboratorTest, SlotsCarryTheirHierarchicalPath) {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.addObject(Object{.name = "count", .type = Type::bits(4, false, true)});
    design.addModule(std::move(leaf));

    ModuleDefinition top;
    top.name = "tb";
    top.instances.push_back(enzan::eir::Instance{.name = "u_dut", .definitionName = "leaf"});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr);

    const std::uint32_t slot =
        require(elaborated->findSlot("/tb/u_dut/count"), "slot for /tb/u_dut/count");
    EXPECT_EQ(elaborated->slots()[slot].path, "/tb/u_dut/count");
}

// ---------------------------------------------------------------------------
// Port binding
// ---------------------------------------------------------------------------

/// A child with one input port, and a parent that drives it.
void buildPortedPair(Design& design, std::uint32_t width, std::uint32_t parentWidth) {
    ModuleDefinition child;
    child.name = "child";
    child.addObject(Object{.name = "in", .type = Type::bits(width, false, true)});
    child.ports.push_back(Port{.name = "in", .direction = PortDirection::Input, .objectIndex = 0});
    design.addModule(std::move(child));

    ModuleDefinition parent;
    parent.name = "parent";
    parent.addObject(Object{.name = "drive", .type = Type::bits(parentWidth, false, true)});
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(PortConnection{.portIndex = 0, .objectIndex = 0});
    parent.instances.push_back(std::move(instance));
    design.addModule(std::move(parent));
}

TEST_F(ElaboratorTest, ACollapsedPortIsOneSlotUnderTwoNames) {
    // A connection between two objects of the same width is the same storage under two
    // names, which is what makes a port cost nothing at run time.
    buildPortedPair(design, 8, 8);

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("parent");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    const std::uint32_t outer =
        require(elaborated->findSlot("/parent/drive"), "slot for /parent/drive");
    const std::uint32_t inner =
        require(elaborated->findSlot("/parent/u_child/in"), "slot for /parent/u_child/in");
    EXPECT_EQ(outer, inner);

    // One slot, so the arena holds one 8-bit object rather than two.
    EXPECT_EQ(elaborated->arenaWords(), 2U);

    const std::vector<std::string> names = elaborated->slotNames(outer);
    EXPECT_EQ(names, std::vector<std::string>({"/parent/drive", "/parent/u_child/in"}));
}

TEST_F(ElaboratorTest, TheChildsObjectSlotPointsAtTheParentsStorage) {
    buildPortedPair(design, 8, 8);

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("parent");
    ASSERT_NE(elaborated, nullptr);

    const std::uint32_t childIndex =
        require(elaborated->findInstance("/parent/u_child"), "instance /parent/u_child");
    const Instance& child = elaborated->instances()[childIndex];
    ASSERT_EQ(child.objectSlots.size(), 1U);
    EXPECT_EQ(child.objectSlots[0],
              require(elaborated->findSlot("/parent/drive"), "slot for /parent/drive"));
}

TEST_F(ElaboratorTest, APortsDriversDriveTheObjectItsConnectionNames) {
    // The middle instance reads the object it is connected to and drives the other one, and the
    // leaf's port collapsed onto the middle's does the same.
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.addObject(Object{.name = "io", .type = Type::bits(8, false, true)});
    leaf.ports.push_back(Port{.name = "io", .direction = PortDirection::InOut, .objectIndex = 0});
    design.addModule(std::move(leaf));

    ModuleDefinition middle;
    middle.name = "middle";
    middle.addObject(Object{.name = "io", .type = Type::bits(8, false, true)});
    middle.ports.push_back(Port{.name = "io", .direction = PortDirection::InOut, .objectIndex = 0});
    enzan::eir::Instance inner;
    inner.name = "u_leaf";
    inner.definitionName = "leaf";
    inner.connections.push_back(PortConnection{.portIndex = 0, .objectIndex = 0});
    middle.instances.push_back(std::move(inner));
    design.addModule(std::move(middle));

    ModuleDefinition top;
    top.name = "top";
    top.addObject(Object{.name = "effective", .type = Type::bits(8, false, true)});
    top.addObject(Object{.name = "driving", .type = Type::bits(8, false, true)});
    enzan::eir::Instance outer;
    outer.name = "u_middle";
    outer.definitionName = "middle";
    outer.connections.push_back(
        PortConnection{.portIndex = 0, .objectIndex = 0, .driveObjectIndex = 1});
    top.instances.push_back(std::move(outer));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("top");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    const std::uint32_t effective =
        require(elaborated->findSlot("/top/effective"), "slot for /top/effective");
    const std::uint32_t driving =
        require(elaborated->findSlot("/top/driving"), "slot for /top/driving");
    for (const std::string_view path : {"/top/u_middle", "/top/u_middle/u_leaf"}) {
        const Instance& instance = elaborated->instances()[require(
            elaborated->findInstance(std::string(path)), std::string(path))];
        EXPECT_EQ(instance.objectSlots[0], effective) << path;
        EXPECT_EQ(instance.objectDriveSlots[0], driving) << path;
    }
}

TEST_F(ElaboratorTest, RejectsADrivenObjectOfAnotherWidthThanItsPort) {
    ModuleDefinition child;
    child.name = "child";
    child.addObject(Object{.name = "io", .type = Type::bits(8, false, true)});
    child.ports.push_back(Port{.name = "io", .direction = PortDirection::InOut, .objectIndex = 0});
    design.addModule(std::move(child));

    ModuleDefinition parent;
    parent.name = "parent";
    parent.addObject(Object{.name = "effective", .type = Type::bits(8, false, true)});
    parent.addObject(Object{.name = "narrow", .type = Type::bits(4, false, true)});
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(
        PortConnection{.portIndex = 0, .objectIndex = 0, .driveObjectIndex = 1});
    parent.instances.push_back(std::move(instance));
    design.addModule(std::move(parent));

    EXPECT_EQ(elaborate("parent"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::PortWidthMismatch));
}

TEST_F(ElaboratorTest, RejectsAPortOfAnotherEncodingThanItsObject) {
    // Eight std_ulogic codes and 64 four-state bits are as wide, and joined, one side would read
    // the other's codes as bits: a connection between languages has to convert its values, and
    // EIR asking for one that does not is refused.
    buildPortedPair(design, 64, 64);
    ModuleDefinition child = *design.findModule("child");
    child.objects[0].encoding = enzan::eir::Encoding::VhdlLogic;
    design.addModule(std::move(child));

    EXPECT_EQ(elaborate("parent"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::PortEncodingMismatch));
}

TEST_F(ElaboratorTest, RejectsAPortWidthMismatch) {
    // A port is collapsed onto the signal it is connected to, and collapsing a mismatched
    // pair onto one slot would silently read or write the wrong bits. A frontend converts the
    // width with an assignment instead, so EIR that asks for it is refused.
    buildPortedPair(design, 8, 4);

    EXPECT_EQ(elaborate("parent"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::PortWidthMismatch));
    EXPECT_TRUE(reported("joined only to a signal as wide as it"));
}

TEST_F(ElaboratorTest, WarnsAboutAnUnconnectedPortButContinues) {
    // Left to read as X rather than tied off: a silently tied-off input is how a
    // missing connection turns into a design that appears to work.
    ModuleDefinition child;
    child.name = "child";
    child.addObject(Object{.name = "in", .type = Type::logic()});
    child.ports.push_back(Port{.name = "in", .direction = PortDirection::Input, .objectIndex = 0});
    design.addModule(std::move(child));

    ModuleDefinition parent;
    parent.name = "parent";
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(PortConnection{.portIndex = 0});
    parent.instances.push_back(std::move(instance));
    design.addModule(std::move(parent));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("parent");
    ASSERT_NE(elaborated, nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::UnconnectedPort));

    // The port keeps its own storage, so the design still elaborates.
    EXPECT_TRUE(elaborated->findSlot("/parent/u_child/in").has_value());
}

TEST_F(ElaboratorTest, AnUnconnectedVhdlPortKeepsItsDefault) {
    // Which its object starts with, rather than X: a VHDL port left open from SystemVerilog.
    ModuleDefinition child;
    child.name = "child";
    child.language = enzan::eir::Language::Vhdl;
    child.addObject(Object{.name = "in", .type = Type::logic()});
    child.ports.push_back(Port{.name = "in", .direction = PortDirection::Input, .objectIndex = 0});
    design.addModule(std::move(child));

    ModuleDefinition parent;
    parent.name = "parent";
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(PortConnection{.portIndex = 0});
    parent.instances.push_back(std::move(instance));
    design.addModule(std::move(parent));

    ASSERT_NE(elaborate("parent"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::UnconnectedVhdlPort));
    EXPECT_FALSE(reportedCode(enzan::DiagCode::UnconnectedPort));
}

// ---------------------------------------------------------------------------
// Processes and drivers
// ---------------------------------------------------------------------------

TEST_F(ElaboratorTest, EveryInstancesProcessesAreScheduled) {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.addObject(Object{.name = "a", .type = Type::logic()});
    leaf.addObject(Object{.name = "b", .type = Type::logic()});
    leaf.processes.push_back(makeCopyProcess(ProcessKind::AlwaysComb, 0, 1, Type::logic()));
    design.addModule(std::move(leaf));

    ModuleDefinition top;
    top.name = "tb";
    top.instances.push_back(enzan::eir::Instance{.name = "u0", .definitionName = "leaf"});
    top.instances.push_back(enzan::eir::Instance{.name = "u1", .definitionName = "leaf"});
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    // One process per instance of the module, sharing one definition body.
    ASSERT_EQ(elaborated->processes().size(), 2U);
    EXPECT_EQ(elaborated->processes()[0].processIndex, 0U);
    EXPECT_EQ(elaborated->processes()[1].processIndex, 0U);
    EXPECT_NE(elaborated->processes()[0].instanceIndex, elaborated->processes()[1].instanceIndex);
}

TEST_F(ElaboratorTest, SeveralProceduralWritersOfOneObjectAreAllowed) {
    // A testbench with `initial clk = 0;` alongside `always #5 clk = ~clk;` has two
    // procedural writers of one variable, and the last write wins. Rejecting that
    // would reject almost every testbench ever written.
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "clk", .type = Type::logic()});
    top.processes.push_back(makeCopyProcess(ProcessKind::Initial, 0, 1, Type::logic()));
    top.processes.push_back(makeCopyProcess(ProcessKind::Always, 0, 1, Type::logic()));
    design.addModule(std::move(top));

    EXPECT_NE(elaborate("tb"), nullptr);
    EXPECT_FALSE(reportedCode(enzan::DiagCode::MultipleContinuousDrivers));
}

TEST_F(ElaboratorTest, RejectsTwoContinuousAssignmentsDrivingOneObject) {
    // A variable has at most one continuous driver per bit, and a second is an error rather
    // than something to resolve. Catching it here is the difference between a clear error
    // and a design that simulates to a plausible wrong answer.
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "net", .type = Type::logic()});
    top.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic()));
    top.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic()));
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MultipleContinuousDrivers));
    EXPECT_TRUE(reported("/tb/net")) << "the report must name the object";
}

/// A net `net` of `kind` driven by one continuous assignment of each of `strengths`.
void addNetDrivers(Design& design, enzan::eir::NetKind kind,
                   std::initializer_list<enzan::eir::DriveStrength> strengths) {
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "net",
                         .type = Type::logic(),
                         .kind = enzan::eir::ObjectKind::Net,
                         .netKind = kind});
    for (const enzan::eir::DriveStrength strength : strengths) {
        Process process = makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic());
        process.strength = strength;
        top.processes.push_back(std::move(process));
    }
    design.addModule(std::move(top));
}

TEST_F(ElaboratorTest, ADriversStrengthIsHeldByTheDriverAndItsSegment) {
    using enzan::eir::Strength;
    addNetDrivers(design, enzan::eir::NetKind::Wire,
                  {{.zero = Strength::Strong, .one = Strength::Weak}, {}});
    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    ASSERT_EQ(elaborated->drivers().size(), 2U);
    EXPECT_EQ(elaborated->drivers()[0].strength.one, Strength::Weak);
    ASSERT_EQ(elaborated->segments().size(), 1U);
    EXPECT_TRUE(elaborated->segments()[0].weak);
}

TEST_F(ElaboratorTest, ANetAForceNamesKeepsItsLoneDriversValue) {
    // A lone driver of a wire writes straight into it, but one a force names is released back
    // to what its driver drives, so the driver keeps its value.
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "net", .type = Type::logic(), .kind = ObjectKind::Net});
    top.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic()));
    Process statement;
    statement.kind = ProcessKind::Initial;
    statement.overrideSet = {1};
    statement.blocks.push_back(
        BasicBlock{.label = "entry", .instructions = {Instruction::make(Opcode::Return)}});
    top.processes.push_back(std::move(statement));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    ASSERT_EQ(elaborated->segments().size(), 1U);
    EXPECT_EQ(elaborated->segments()[0].driverCount, 1U);
    EXPECT_EQ(elaborated->drivers().size(), 1U);
}

TEST_F(ElaboratorTest, RefusesStrengthsTheTwoClassesWouldResolveAsEquals) {
    // The standard has pull beat weak; two classes would resolve them to X.
    using enzan::eir::Strength;
    addNetDrivers(design, enzan::eir::NetKind::Wire,
                  {{.zero = Strength::Pull, .one = Strength::Pull},
                   {.zero = Strength::Weak, .one = Strength::Weak}});
    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::StrengthsIndistinguishable));
    EXPECT_TRUE(reported("/tb/net"));
}

TEST_F(ElaboratorTest, RefusesAWeakDriverOfANetThatPulls) {
    // A `tri1`'s own pull beats a weak 0.
    using enzan::eir::Strength;
    addNetDrivers(design, enzan::eir::NetKind::Tri1,
                  {{.zero = Strength::Weak, .one = Strength::Weak}});
    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reported("the tri1's own pull"));
}

TEST_F(ElaboratorTest, RefusesADriverOfASupplyNetAtSupplyStrength) {
    using enzan::eir::Strength;
    addNetDrivers(design, enzan::eir::NetKind::Supply0,
                  {{.zero = Strength::Supply, .one = Strength::Supply}});
    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::NotYetImplemented));
}

TEST_F(ElaboratorTest, AProcessThatDrivesItsSignalsIsADriverOfThem) {
    // Each VHDL process has a driver of its own for every signal it assigns, so two of them
    // assigning one unresolved signal is the error two continuous assignments are, not a race
    // the last write wins.
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::bits(8, false, false)});
    top.addObject(Object{.name = "sig", .type = Type::bits(8, false, false)});
    for (int copy = 0; copy < 2; ++copy) {
        Process process = makeCopyProcess(ProcessKind::Always, 0, 1, Type::bits(8, false, false));
        process.drivesSignals = true;
        top.processes.push_back(std::move(process));
    }
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MultipleContinuousDrivers));
}

/// `tb` with a 16-bit source and a 16-bit signal resolved sixteen bits at a time, as a VHDL
/// resolution function of composite values resolves, and `drivers` driving the signal.
ModuleDefinition makeResolvedWhole(std::vector<Process> drivers) {
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::bits(16, false, false)});
    Object resolved{.name = "sig", .type = Type::bits(16, false, false)};
    resolved.resolution = {.function = "WORK.PAIRS.COMBINE", .elementBits = 16};
    top.addObject(std::move(resolved));
    for (Process& driver : drivers) {
        driver.drivesSignals = true;
        top.processes.push_back(std::move(driver));
    }
    return top;
}

TEST_F(ElaboratorTest, DriversOfAValueResolvedWholeShareOneResolution) {
    design.addModule(makeResolvedWhole(
        {makeCopyProcess(ProcessKind::Always, 0, 1, Type::bits(16, false, false)),
         makeCopyProcess(ProcessKind::Always, 0, 1, Type::bits(16, false, false))}));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    ASSERT_EQ(elaborated->segments().size(), 1U);
    EXPECT_EQ(elaborated->segments()[0].driverCount, 2U);
    ASSERT_EQ(elaborated->resolutionCalls().size(), 1U);
    EXPECT_EQ(elaborated->resolutionCalls()[0].elementBits, 16U);
}

TEST_F(ElaboratorTest, RejectsAProcessDrivingPartOfAValueResolvedWhole) {
    // The resolution function is given each driver's whole value, and a process driving eight of
    // its sixteen bits would give it one no process assigned.
    design.addModule(
        makeResolvedWhole({makeCopyProcess(ProcessKind::Always, 0, 1, Type::bits(16, false, false)),
                           makePartDriver(ProcessKind::Always, 0, 1, 8, 8)}));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::NotYetImplemented));
    EXPECT_TRUE(reported("bits [15:8] of '/tb/sig'")) << consumer.messages.front();
}

TEST_F(ElaboratorTest, ASubroutinesStaticVariableIsNoContinuousDriversTarget) {
    // Every call of a static function writes its variables, and a call inlined into a
    // continuous assignment is no more their driver than one in an `always`: two assignments
    // and an always_comb calling one function are all legal.
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "f.scratch", .type = Type::logic(), .subroutineStatic = true});
    top.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic()));
    top.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic()));
    top.processes.push_back(makeCopyProcess(ProcessKind::AlwaysComb, 0, 1, Type::logic()));
    design.addModule(std::move(top));

    EXPECT_NE(elaborate("tb"), nullptr) << consumer.messages.front();
}

TEST_F(ElaboratorTest, RejectsAContinuousAndAProceduralDriverOnOneObject) {
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "net", .type = Type::logic()});
    top.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic()));
    top.processes.push_back(makeCopyProcess(ProcessKind::AlwaysFf, 0, 1, Type::logic()));
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MixedDriverKinds));
}

TEST_F(ElaboratorTest, ACollapsedPortDrivenFromBothSidesIsDetected) {
    // The conflict is only visible after collapsing: two processes in two different
    // modules, writing what turns out to be one slot.
    ModuleDefinition child;
    child.name = "child";
    child.addObject(Object{.name = "io", .type = Type::logic()});
    child.addObject(Object{.name = "src", .type = Type::logic()});
    child.ports.push_back(Port{.name = "io", .direction = PortDirection::Output, .objectIndex = 0});
    child.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 1, 0, Type::logic()));
    design.addModule(std::move(child));

    ModuleDefinition parent;
    parent.name = "parent";
    parent.addObject(Object{.name = "wire", .type = Type::logic()});
    parent.addObject(Object{.name = "src", .type = Type::logic()});
    parent.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 1, 0, Type::logic()));
    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(PortConnection{.portIndex = 0, .objectIndex = 0});
    parent.instances.push_back(std::move(instance));
    design.addModule(std::move(parent));

    EXPECT_EQ(elaborate("parent"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MultipleContinuousDrivers));
}

// ---------------------------------------------------------------------------
// Drivers per bit
// ---------------------------------------------------------------------------

/// A module `tb` with a one-bit source and an eight-bit target of the given kind.
ModuleDefinition partTarget(ObjectKind kind) {
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "x", .type = Type::bits(8, false, true), .kind = kind});
    return top;
}

TEST_F(ElaboratorTest, ContinuousDriversOfDisjointBitsOfOneVariableAreAllowed) {
    // Overlap, not sharing a signal, is what multiple driving means.
    ModuleDefinition top = partTarget(ObjectKind::Variable);
    for (std::uint32_t bit = 0; bit < 8; ++bit)
        top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 0, 1, bit, 1));
    design.addModule(std::move(top));

    EXPECT_NE(elaborate("tb"), nullptr) << consumer.messages.front();
    EXPECT_TRUE(consumer.codes.empty());
}

TEST_F(ElaboratorTest, ContinuousDriversOverlappingOnOneBitOfAVariableAreRefused) {
    ModuleDefinition top = partTarget(ObjectKind::Variable);
    top.addObject(Object{.name = "pair", .type = Type::bits(2, false, true)});
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 0, 2));
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 1, 2));
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MultipleContinuousDrivers));
    // The overlap, not the whole of either driver, and both drivers by name and instance.
    EXPECT_TRUE(reported("'/tb/x' has bit 1 driven by both part0 in /tb and part1 in /tb"))
        << consumer.messages.front();
}

TEST_F(ElaboratorTest, AWideOverlapIsReportedAsARangeOnce) {
    ModuleDefinition top = partTarget(ObjectKind::Variable);
    top.addObject(Object{.name = "nibble", .type = Type::bits(4, false, true)});
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 0, 4));
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 2, 4));
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 3, 4));
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reported("bits [3:2]")) << consumer.messages.front();
    EXPECT_EQ(consumer.codes.size(), 1U) << "one diagnostic per signal, for its lowest overlap";
}

TEST_F(ElaboratorTest, AProceduralWriterOfBitsNoContinuousDriverOwnsIsAllowed) {
    ModuleDefinition top = partTarget(ObjectKind::Variable);
    top.addObject(Object{.name = "nibble", .type = Type::bits(4, false, true)});
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 4, 4));
    top.processes.push_back(makePartDriver(ProcessKind::AlwaysComb, 2, 1, 0, 4));
    design.addModule(std::move(top));

    EXPECT_NE(elaborate("tb"), nullptr) << consumer.messages.front();
}

TEST_F(ElaboratorTest, AProceduralWriterOfAContinuouslyDrivenBitIsRefused) {
    ModuleDefinition top = partTarget(ObjectKind::Variable);
    top.addObject(Object{.name = "nibble", .type = Type::bits(4, false, true)});
    top.processes.push_back(makePartDriver(ProcessKind::AlwaysComb, 2, 1, 0, 4));
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 3, 4));
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MixedDriverKinds));
    EXPECT_TRUE(reported("'/tb/x' has bit 3 driven by the continuous assignment part3 in /tb and "
                         "also written by part0 in /tb"))
        << consumer.messages.front();
}

TEST_F(ElaboratorTest, OverlappingDriversOfANetAreResolvedRatherThanRefused) {
    ModuleDefinition top = partTarget(ObjectKind::Net);
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 0, 1, 5, 1));
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 0, 1, 5, 1));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    // Each driver has its own value in the arena, past every slot.
    ASSERT_EQ(elaborated->drivers().size(), 2U);
    const std::uint32_t slot = require(elaborated->findSlot("/tb/x"));
    for (const enzan::elab::Driver& driver : elaborated->drivers()) {
        EXPECT_EQ(driver.slot, slot);
        EXPECT_EQ(driver.lowBit, 5U);
        EXPECT_EQ(driver.width, 1U);
        EXPECT_GE(driver.wordOffset, elaborated->slots().back().wordOffset + 2);
    }
    EXPECT_NE(elaborated->drivers()[0].wordOffset, elaborated->drivers()[1].wordOffset);

    ASSERT_EQ(elaborated->segments().size(), 1U);
    EXPECT_EQ(elaborated->segments()[0].lowBit, 5U);
    EXPECT_EQ(elaborated->segments()[0].driverCount, 2U);
}

TEST_F(ElaboratorTest, PartlyOverlappingDriversSplitIntoSegments) {
    // Bits [3:0] and [5:2]: bits 1 and 0 have the first alone, 3 and 2 have both, and 5 and 4
    // the second alone. Bits 7 and 6 have none and are in no segment.
    ModuleDefinition top = partTarget(ObjectKind::Net);
    top.addObject(Object{.name = "nibble", .type = Type::bits(4, false, true)});
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 0, 4));
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 2, 1, 2, 4));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    const auto segments = elaborated->segments();
    ASSERT_EQ(segments.size(), 3U);
    EXPECT_EQ(segments[0].lowBit, 0U);
    EXPECT_EQ(segments[0].width, 2U);
    EXPECT_EQ(segments[0].driverCount, 1U);
    EXPECT_EQ(segments[1].lowBit, 2U);
    EXPECT_EQ(segments[1].width, 2U);
    EXPECT_EQ(segments[1].driverCount, 2U);
    EXPECT_EQ(segments[2].lowBit, 4U);
    EXPECT_EQ(segments[2].width, 2U);
    EXPECT_EQ(segments[2].driverCount, 1U);

    // Each driver covers exactly the segments inside its range.
    const auto drivers = elaborated->drivers();
    ASSERT_EQ(drivers.size(), 2U);
    EXPECT_EQ(drivers[0].firstSegment, 0U);
    EXPECT_EQ(drivers[0].segmentCount, 2U);
    EXPECT_EQ(drivers[1].firstSegment, 1U);
    EXPECT_EQ(drivers[1].segmentCount, 2U);
    EXPECT_EQ(elaborated->widestDriver(), 4U);
}

TEST_F(ElaboratorTest, ANetWithOneDriverPerBitHasNoDrivers) {
    // The common case bypasses resolution as a whole: its stores go straight into the slot.
    ModuleDefinition top = partTarget(ObjectKind::Net);
    for (std::uint32_t bit = 0; bit < 8; ++bit)
        top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 0, 1, bit, 1));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    EXPECT_TRUE(elaborated->drivers().empty());
    EXPECT_TRUE(elaborated->segments().empty());
}

TEST_F(ElaboratorTest, PortsOfManyInstancesDrivingBitsOfOneArrayAreAllowed) {
    // The pattern drivers of bit ranges exist for, after collapsing: each instance's output is
    // its own slot, copied into one bit of the parent's array by its own connection.
    ModuleDefinition child;
    child.name = "channel";
    child.addObject(Object{.name = "ready", .type = Type::logic()});
    child.addObject(Object{.name = "src", .type = Type::logic()});
    child.ports.push_back(
        Port{.name = "ready", .direction = PortDirection::Output, .objectIndex = 0});
    child.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 1, 0, Type::logic()));
    design.addModule(std::move(child));

    ModuleDefinition parent = partTarget(ObjectKind::Variable);
    parent.name = "tb";
    for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const std::uint32_t port =
            parent.addObject(Object{.name = std::format("u{}$ready", bit), .type = Type::logic()});
        parent.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, port, 1, bit, 1));

        enzan::eir::Instance instance;
        instance.name = std::format("u{}", bit);
        instance.definitionName = "channel";
        instance.connections.push_back(PortConnection{.portIndex = 0, .objectIndex = port});
        parent.instances.push_back(std::move(instance));
    }
    design.addModule(std::move(parent));

    EXPECT_NE(elaborate("tb"), nullptr) << consumer.messages.front();
}

TEST_F(ElaboratorTest, OverlappingDriversOfAUwireAreAnErrorAsForAVariable) {
    ModuleDefinition top = partTarget(ObjectKind::Net);
    top.objects[1].netKind = enzan::eir::NetKind::UWire;
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 0, 1, 2, 1));
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 0, 1, 2, 1));
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MultipleContinuousDrivers));
    EXPECT_TRUE(reported("uwire net '/tb/x' has bit 2")) << consumer.messages.front();
}

TEST_F(ElaboratorTest, APulledNetResolvesEvenASingleDriver) {
    // A tri0 driven to z reads 0, so its one driver is resolved like several would be.
    ModuleDefinition top = partTarget(ObjectKind::Net);
    top.objects[1].netKind = enzan::eir::NetKind::Tri0;
    top.processes.push_back(makePartDriver(ProcessKind::ContinuousAssign, 0, 1, 0, 1));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    ASSERT_EQ(elaborated->drivers().size(), 1U);
    ASSERT_EQ(elaborated->segments().size(), 1U);
    EXPECT_EQ(elaborated->segments()[0].netKind, enzan::eir::NetKind::Tri0);
}

TEST_F(ElaboratorTest, AnUndrivenPulledNetNeedsNoResolution) {
    ModuleDefinition top = partTarget(ObjectKind::Net);
    top.objects[1].netKind = enzan::eir::NetKind::Supply1;
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    const std::uint32_t slot = require(elaborated->findSlot("/tb/x"));
    EXPECT_EQ(elaborated->slots()[slot].kind, ObjectKind::Net);
    EXPECT_EQ(elaborated->slots()[slot].netKind, enzan::eir::NetKind::Supply1);
}

// ---------------------------------------------------------------------------
// Hierarchical references
// ---------------------------------------------------------------------------

/// `tb` holding `left` and `right`, both of `leaf`, whose one object is `value`; `right` also
/// declares a reference to `../left/value`.
void addReferencingDesign(Design& design, std::string_view reference) {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.addObject(Object{.name = "value", .type = Type::bits(4, false, true)});
    design.addModule(std::move(leaf));

    ModuleDefinition peer;
    peer.name = "peer";
    peer.addObject(Object{.name = "value", .type = Type::bits(4, false, true)});
    peer.addObject(Object{.name = "ref$value",
                          .type = Type::bits(4, false, true),
                          .hierarchicalPath = std::string(reference)});
    design.addModule(std::move(peer));

    ModuleDefinition top;
    top.name = "tb";
    for (const auto& [name, definition] : {std::pair{"left", "leaf"}, std::pair{"right", "peer"}}) {
        enzan::eir::Instance instance;
        instance.name = name;
        instance.definitionName = definition;
        top.instances.push_back(std::move(instance));
    }
    design.addModule(std::move(top));
}

TEST_F(ElaboratorTest, AHierarchicalReferenceIsTheStorageItNames) {
    addReferencingDesign(design, "../left/value");

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    const std::uint32_t target = require(elaborated->findSlot("/tb/left/value"));
    const Instance& right = elaborated->instances()[require(elaborated->findInstance("/tb/right"))];
    EXPECT_EQ(right.objectSlots[1], target);
    // A reference has no storage and no name of its own.
    EXPECT_FALSE(elaborated->findSlot("/tb/right/ref$value").has_value());
}

TEST_F(ElaboratorTest, AHierarchicalReferenceToNothingIsReported) {
    addReferencingDesign(design, "../middle/value");

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::UnresolvedHierarchicalReference));
    EXPECT_TRUE(reported("'../middle/value' in '/tb/right'")) << consumer.messages.front();
}

// ---------------------------------------------------------------------------
// Packages
// ---------------------------------------------------------------------------

/// A package `counters` with one variable, and a top level named `topName` whose instance of
/// `user` and itself both refer to it.
void addPackageDesign(Design& design, std::string_view topName) {
    ModuleDefinition package;
    package.name = "counters::";
    package.sourceName = "counters";
    package.isPackage = true;
    package.addObject(Object{.name = "events", .type = Type::bits(32, true, false)});
    design.addModule(std::move(package));

    const auto reference = [] {
        return Object{.name = "ref$/counters/events",
                      .type = Type::bits(32, true, false),
                      .hierarchicalPath = "/counters/events"};
    };

    ModuleDefinition user;
    user.name = "user";
    user.addObject(reference());
    design.addModule(std::move(user));

    ModuleDefinition top;
    top.name = std::string(topName);
    top.addObject(reference());
    top.instances.push_back(enzan::eir::Instance{.name = "u_user", .definitionName = "user"});
    design.addModule(std::move(top));
}

TEST_F(ElaboratorTest, APackageVariableIsOneVariableForEveryModule) {
    addPackageDesign(design, "tb");

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

    // The package is an instance of its own at the root, beside the top level, not under it.
    const Instance& package =
        elaborated->instances()[require(elaborated->findInstance("/counters"))];
    EXPECT_EQ(package.parent, Instance::NoParent);
    const std::uint32_t events = require(elaborated->findSlot("/counters/events"));
    const Instance& top = elaborated->instances()[require(elaborated->findInstance("/tb"))];
    const Instance& user = elaborated->instances()[require(elaborated->findInstance("/tb/u_user"))];
    EXPECT_EQ(top.objectSlots[0], events);
    EXPECT_EQ(user.objectSlots[0], events);
}

TEST_F(ElaboratorTest, AnUnusedPackageHasNoInstance) {
    ModuleDefinition package;
    package.name = "counters::";
    package.isPackage = true;
    package.addObject(Object{.name = "events", .type = Type::bits(32, true, false)});
    design.addModule(std::move(package));
    ModuleDefinition top;
    top.name = "tb";
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("tb");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    EXPECT_FALSE(elaborated->findInstance("/counters").has_value());
}

TEST_F(ElaboratorTest, APackageNamedAsTheTopLevelIsReported) {
    // Its variables would be at the top level's path, where a reference could find the top
    // level's own object of the same name instead.
    addPackageDesign(design, "counters");

    EXPECT_EQ(elaborate("counters"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::PackageNamedAsTop));
}

// ---------------------------------------------------------------------------
// Calls of subprograms
// ---------------------------------------------------------------------------

/// A process of `definition` that calls `subprogram` of `callee` with no arguments.
Process makeCaller(std::string callee, std::string subprogram) {
    ProcessBuilder builder(ProcessKind::Initial, "caller");
    builder.block("entry");
    builder.call(std::move(callee), std::move(subprogram), {});
    builder.emit(Instruction::make(Opcode::Return));
    return builder.finish();
}

TEST_F(ElaboratorTest, BindsAModulesSubprogramToEachInstanceThatCallsIt) {
    // One compiled body, two bindings: each instance's call reaches its own `x`.
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.addObject(Object{.name = "x", .type = Type::bits(8, false, true)});
    SubprogramBuilder touch("touch");
    const std::uint32_t value = touch.reg(Type::bits(8, false, true));
    touch.block("entry");
    touch.readObject(value, 0);
    touch.emit(Instruction::make(Opcode::Return));
    leaf.subprograms.push_back(touch.finish());
    leaf.processes.push_back(makeCaller("", "touch"));

    ModuleDefinition top;
    top.name = "top";
    top.instances.push_back(enzan::eir::Instance{.name = "u0", .definitionName = "leaf"});
    top.instances.push_back(enzan::eir::Instance{.name = "u1", .definitionName = "leaf"});

    design.addModule(std::move(leaf));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("top");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    ASSERT_EQ(elaborated->subprograms().size(), 2U);
    ASSERT_EQ(elaborated->processes().size(), 2U);

    for (const enzan::elab::ProcessInstance& process : elaborated->processes()) {
        ASSERT_EQ(process.calls.size(), 1U);
        const SubprogramInstance& bound = elaborated->subprograms()[process.calls.front()];
        EXPECT_EQ(bound.instanceIndex, process.instanceIndex);

        const std::string& path = elaborated->instances()[process.instanceIndex].path;
        EXPECT_EQ(process.calleeReads,
                  std::vector<std::uint32_t>{require(elaborated->findSlot(path + "/x"))});
    }
}

TEST_F(ElaboratorTest, BindsAPackagesSubprogramToThePackagesOneInstance) {
    ModuleDefinition package;
    package.name = "pkg::";
    package.sourceName = "pkg";
    package.isPackage = true;
    package.addObject(Object{.name = "state", .type = Type::bits(8, false, true)});
    SubprogramBuilder set("set");
    const std::uint32_t value = set.reg(Type::bits(8, false, true));
    set.block("entry");
    set.store(0, value);
    set.emit(Instruction::make(Opcode::Return));
    package.subprograms.push_back(set.finish());

    ModuleDefinition top;
    top.name = "top";
    top.processes.push_back(makeCaller("pkg::", "set"));

    design.addModule(std::move(package));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("top");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    ASSERT_EQ(elaborated->subprograms().size(), 1U);
    EXPECT_EQ(elaborated->subprograms().front().instanceIndex,
              require(elaborated->findInstance("/pkg")));

    const std::uint32_t state = require(elaborated->findSlot("/pkg/state"));
    EXPECT_EQ(elaborated->processes().front().calleeWrites,
              (std::vector<SlotRange>{SlotRange{.slot = state, .lowBit = 0, .width = 8}}));
}

TEST_F(ElaboratorTest, APackageHoldingNoObjectsIsGivenNoInstance) {
    // Its subprograms touch only their parameters and registers, so there is nothing for an
    // instance to hold.
    ModuleDefinition package;
    package.name = "pure::";
    package.sourceName = "pure";
    package.isPackage = true;
    SubprogramBuilder nothing("nothing");
    nothing.block("entry");
    nothing.emit(Instruction::make(Opcode::Return));
    package.subprograms.push_back(nothing.finish());

    ModuleDefinition top;
    top.name = "top";
    top.processes.push_back(makeCaller("pure::", "nothing"));

    design.addModule(std::move(package));
    design.addModule(std::move(top));

    const std::unique_ptr<ElaboratedDesign> elaborated = elaborate("top");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    EXPECT_FALSE(elaborated->findInstance("/pure").has_value());
    ASSERT_EQ(elaborated->subprograms().size(), 1U);
    EXPECT_EQ(elaborated->subprograms().front().instanceIndex, SubprogramInstance::NoInstance);
}

} // namespace
