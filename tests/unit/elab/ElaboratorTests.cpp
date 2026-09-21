// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Elaborator.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include "support/Optionals.h"

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
using enzan::eir::Port;
using enzan::eir::PortConnection;
using enzan::eir::PortDirection;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::elab::ElaboratedDesign;
using enzan::elab::Elaborator;
using enzan::elab::Instance;
using enzan::test::require;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
        codes.push_back(diagnostic.code);
    }

    std::vector<std::string> messages;
    std::vector<enzan::DiagCode> codes;
};

/// Builds a process that copies one object to another, with the declared read and
/// write sets the verifier requires.
Process makeCopyProcess(ProcessKind kind, std::uint32_t from, std::uint32_t to, Type type) {
    Process process;
    process.kind = kind;
    process.name = "copy";
    process.registerTypes.push_back(type);
    process.readSet = {from};
    process.writeSet = {to};

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
        for (const std::string& message : consumer.messages) {
            if (message.find(fragment) != std::string::npos) return true;
        }
        return false;
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

TEST_F(ElaboratorTest, RejectsAPortWidthMismatch) {
    // Width conversion on a port connection is not implemented, and collapsing a
    // mismatched pair onto one slot would silently read or write the wrong bits.
    buildPortedPair(design, 8, 4);

    EXPECT_EQ(elaborate("parent"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::PortWidthMismatch));
    EXPECT_TRUE(reported("not yet implemented"));
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
    // Two continuous drivers need resolution, which is an M5 feature. Catching it here
    // is the difference between a clear error and a design that simulates to a
    // plausible wrong answer.
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

TEST_F(ElaboratorTest, RejectsAContinuousAndAProceduralDriverOnOneObject) {
    ModuleDefinition top;
    top.name = "tb";
    top.addObject(Object{.name = "src", .type = Type::logic()});
    top.addObject(Object{.name = "net", .type = Type::logic()});
    top.processes.push_back(makeCopyProcess(ProcessKind::ContinuousAssign, 0, 1, Type::logic()));
    top.processes.push_back(makeCopyProcess(ProcessKind::AlwaysFf, 0, 1, Type::logic()));
    design.addModule(std::move(top));

    EXPECT_EQ(elaborate("tb"), nullptr);
    EXPECT_TRUE(reportedCode(enzan::DiagCode::MultipleContinuousDrivers));
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

} // namespace
