// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conversions a port crossing between SystemVerilog and VHDL is connected through, on
// hand-built EIR, so that what is tested is the boundary and not a frontend: each direction, an
// inout port that releases its bus, a record field by field, and the ports no rule converts.

#include "elab/Boundary.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"

#include "support/Optionals.h"
#include "support/ProcessBuilder.h"
#include "support/Simulation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::eir::BitVector;
using enzan::eir::Design;
using enzan::eir::EnumDefinition;
using enzan::eir::EnumMember;
using enzan::eir::Instance;
using enzan::eir::Instruction;
using enzan::eir::Language;
using enzan::eir::ModuleDefinition;
using enzan::eir::NetKind;
using enzan::eir::Object;
using enzan::eir::ObjectKind;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::Port;
using enzan::eir::PortConnection;
using enzan::eir::PortDirection;
using enzan::eir::ProcessKind;
using enzan::eir::Radix;
using enzan::eir::Type;
using enzan::eir::VhdlType;
using enzan::test::ProcessBuilder;
using enzan::test::require;
using enzan::test::Simulation;

/// std_ulogic's codes, the positions of its literals U X 0 1 Z W L H -.
constexpr std::uint8_t CodeU = 0;
constexpr std::uint8_t Code0 = 2;
constexpr std::uint8_t Code1 = 3;
constexpr std::uint8_t CodeZ = 4;
constexpr std::uint8_t CodeL = 6;
constexpr std::uint8_t CodeH = 7;

/// A value of codes, one byte each, the left element highest.
BitVector codes(std::initializer_list<std::uint8_t> elements) {
    std::uint64_t value = 0;
    for (const std::uint8_t element : elements) value = (value << 8U) | element;
    return BitVector::fromUInt64(value, static_cast<std::uint32_t>(8 * elements.size()));
}

/// The text of `codes`, as a constant the builder takes.
std::string codeText(std::initializer_list<std::uint8_t> elements) {
    return std::format("{}'h{}", 8 * elements.size(),
                       enzan::eir::toString(codes(elements).ref(), Radix::Hexadecimal));
}

/// The table of VHDL types the definitions here use: std_ulogic, bit and integer.
constexpr std::uint32_t StdUlogic = 0;
constexpr std::uint32_t BitType = 1;
constexpr std::uint32_t IntegerType = 2;

/// A VHDL definition with the type table above.
ModuleDefinition vhdlDefinition(std::string name) {
    ModuleDefinition definition;
    definition.name = name;
    definition.sourceName = std::move(name);
    definition.language = Language::Vhdl;

    EnumDefinition logic{.name = "STD_ULOGIC", .members = {}};
    for (const char* literal : {"'U'", "'X'", "'0'", "'1'", "'Z'", "'W'", "'L'", "'H'", "'-'"}) {
        logic.members.push_back(
            EnumMember{.name = literal, .value = BitVector::fromUInt64(logic.members.size(), 8)});
    }
    definition.enums.push_back(std::move(logic));
    EnumDefinition bit{.name = "BIT", .members = {}};
    for (const char* literal : {"'0'", "'1'"}) {
        bit.members.push_back(
            EnumMember{.name = literal, .value = BitVector::fromUInt64(bit.members.size(), 8)});
    }
    definition.enums.push_back(std::move(bit));

    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Enumeration,
                                            .name = "STD_ULOGIC",
                                            .width = 8,
                                            .encoding = enzan::eir::Encoding::VhdlLogic,
                                            .ranges = {},
                                            .enumIndex = 0});
    definition.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Enumeration,
                                            .name = "BIT",
                                            .width = 8,
                                            .encoding = enzan::eir::Encoding::VhdlBit,
                                            .ranges = {},
                                            .enumIndex = 1});
    definition.vhdlTypes.push_back(
        VhdlType{.kind = VhdlType::Kind::Integer, .name = "INTEGER", .width = 32});
    return definition;
}

/// A VHDL signal of `elements` std_ulogic codes, each starting at `initial`.
Object logicSignal(std::string name, std::uint32_t elements, std::uint8_t initial = CodeU) {
    BitVector value = BitVector::filled(8 * elements, enzan::eir::Logic::Zero);
    for (std::uint32_t element = 0; element < elements; ++element) {
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            value.mutableRef().set((8 * element) + bit, ((initial >> bit) & 1U) != 0
                                                            ? enzan::eir::Logic::One
                                                            : enzan::eir::Logic::Zero);
        }
    }
    return Object{.name = std::move(name),
                  .type = Type::bits(8 * elements, false, false),
                  .initialValue = std::move(value),
                  .encoding = enzan::eir::Encoding::VhdlLogic};
}

/// A register holding `text`.
std::uint32_t load(ProcessBuilder& builder, Type type, std::string_view text) {
    const std::uint32_t result = builder.reg(type);
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(result),
                                   Operand::constant(builder.constant(text))));
    return result;
}

/// A VHDL process that drives each of `values` into `object` in turn, 10 time units apart, and
/// then waits for ever.
enzan::eir::Process vhdlDriver(std::uint32_t object, std::uint32_t width,
                               const std::vector<std::string>& values) {
    ProcessBuilder builder(ProcessKind::Always, "driver");
    builder.block("entry");
    const std::uint32_t now = load(builder, Type::bits(64, true, false), "64'sd0");
    const std::uint32_t ten = load(builder, Type::bits(64, true, false), "64'sd10");
    for (const std::string& value : values) {
        builder.drive(object, load(builder, Type::bits(width, false, false), value), now);
        const std::uint32_t next = builder.nextBlock();
        builder.emit(
            Instruction::make(Opcode::WaitTime, {}, Operand::reg(ten), Operand::block(next)));
        builder.block("after");
    }
    builder.waitEvents({}, builder.nextBlock());
    builder.block("never");
    builder.emit(Instruction::make(Opcode::Return));
    enzan::eir::Process process = builder.finish();
    process.drivesSignals = true;
    return process;
}

/// A SystemVerilog `initial` that stores each of `values` into `object` in turn, 10 time units
/// apart.
enzan::eir::Process svDriver(std::uint32_t object, std::uint32_t width,
                             const std::vector<std::string>& values) {
    ProcessBuilder builder(ProcessKind::Initial, "driver");
    builder.block("entry");
    const std::uint32_t ten = load(builder, Type::bits(64, false, false), "64'd10");
    for (const std::string& value : values) {
        builder.store(object, load(builder, Type::bits(width, false, true), value));
        const std::uint32_t next = builder.nextBlock();
        builder.emit(
            Instruction::make(Opcode::WaitTime, {}, Operand::reg(ten), Operand::block(next)));
        builder.block("after");
    }
    builder.emit(Instruction::make(Opcode::Return));
    return builder.finish();
}

/// A one-port connection of `instance`'s port `port` to the parent's `object`.
PortConnection connect(std::uint32_t port, std::uint32_t object) {
    return PortConnection{.portIndex = port, .objectIndex = object};
}

class BoundaryTest : public ::testing::Test {
protected:
    BoundaryTest() : engine(sources) { engine.addConsumer(consumer); }

    /// Bridges `design` and simulates it from `top`.
    std::unique_ptr<Simulation> simulate(Design design, std::string_view top) {
        EXPECT_TRUE(enzan::elab::bridgeLanguages(design, engine))
            << (consumer.messages.empty() ? std::string() : consumer.messages.front());
        auto simulation = std::make_unique<Simulation>(std::move(design), top);
        EXPECT_TRUE(simulation->ready());
        return simulation;
    }

    [[nodiscard]] bool reported(DiagCode code) const {
        return std::ranges::find(consumer.codes, code) != consumer.codes.end();
    }

    enzan::SourceManager sources;
    enzan::DiagnosticEngine engine;
    enzan::test::CollectingConsumer consumer;
};

// ---------------------------------------------------------------------------
// SystemVerilog instantiating VHDL
// ---------------------------------------------------------------------------

/// A VHDL child taking a std_ulogic in and driving a four-element vector out: '1', what it takes,
/// 'Z' and 'H'.
ModuleDefinition vhdlChild() {
    ModuleDefinition child = vhdlDefinition("vchild");
    child.addObject(logicSignal("input", 1));
    child.addObject(logicSignal("output", 4));
    child.ports.push_back(
        Port{.name = "input", .direction = PortDirection::Input, .objectIndex = 0});
    child.ports.push_back(
        Port{.name = "output", .direction = PortDirection::Output, .objectIndex = 1});

    ProcessBuilder builder(ProcessKind::Always, "combine");
    builder.block("entry");
    const std::uint32_t loop = builder.nextBlock();
    builder.emit(Instruction::make(Opcode::Jump, {}, Operand::block(loop)));
    builder.block("loop");
    const std::uint32_t now = load(builder, Type::bits(64, true, false), "64'sd0");
    const std::uint32_t taken = builder.reg(Type::bits(8, false, false));
    builder.readObject(taken, 0);
    const std::uint32_t combined =
        load(builder, Type::bits(32, false, false), codeText({Code1, CodeU, CodeZ, CodeH}));
    builder.emit(
        Instruction::make(Opcode::SplicePart, Operand::reg(combined), Operand::reg(taken),
                          Operand::reg(load(builder, Type::bits(32, false, false), "32'd16"))));
    builder.drive(1, combined, now);
    builder.waitEvents({enzan::eir::SensitivityEntry{.objectIndex = 0}}, loop);
    enzan::eir::Process process = builder.finish();
    process.drivesSignals = true;
    child.processes.push_back(std::move(process));
    return child;
}

TEST_F(BoundaryTest, AStdLogicInAndAVectorOutCrossAsFourStateBits) {
    Design design;
    design.addModule(vhdlChild());

    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    top.addObject(Object{.name = "a", .type = Type::bits(1, false, true)});
    top.addObject(Object{.name = "y",
                         .type = Type::bits(4, false, true),
                         .kind = ObjectKind::Net,
                         .netKind = NetKind::Wire});
    top.instances.push_back(Instance{
        .name = "u", .definitionName = "vchild", .connections = {connect(0, 0), connect(1, 1)}});
    top.processes.push_back(svDriver(0, 1, {"1'b1", "1'b0", "1'bz"}));
    design.addModule(std::move(top));

    const std::unique_ptr<Simulation> simulation = simulate(std::move(design), "top");
    ASSERT_TRUE(simulation->ready());
    simulation->run(5);
    EXPECT_EQ(simulation->value("/top/y"), "11z1") << "'1', '1', 'Z' and 'H'";
    simulation->run(15);
    EXPECT_EQ(simulation->value("/top/y"), "10z1");
    simulation->run(25);
    EXPECT_EQ(simulation->value("/top/y"), "1zz1") << "z crosses as 'Z' and back";
}

TEST_F(BoundaryTest, ACrossingTakesOneDeltaCycleEachWay) {
    // At time 0 the child starts at its default, all 'U', and what its conversion brings in lands a
    // delta later: an observer reading y in the first delta of a time step that changes a sees
    // y's old value, and the new one once the step has settled.
    Design design;
    design.addModule(vhdlChild());

    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    top.addObject(Object{.name = "a", .type = Type::bits(1, false, true)});
    top.addObject(Object{.name = "y",
                         .type = Type::bits(4, false, true),
                         .kind = ObjectKind::Net,
                         .netKind = NetKind::Wire});
    top.addObject(Object{.name = "seen", .type = Type::bits(4, false, true)});
    top.instances.push_back(Instance{
        .name = "u", .definitionName = "vchild", .connections = {connect(0, 0), connect(1, 1)}});
    top.processes.push_back(svDriver(0, 1, {"1'b1", "1'b0"}));

    // Wakes when a changes and copies what y holds then, in the same delta.
    ProcessBuilder observer(ProcessKind::Always, "observer");
    observer.sensitive(0, enzan::eir::EdgeKind::AnyChange);
    observer.block("entry");
    const std::uint32_t y = observer.reg(Type::bits(4, false, true));
    observer.readObject(y, 1);
    observer.store(2, y);
    observer.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(observer.finish());
    design.addModule(std::move(top));

    const std::unique_ptr<Simulation> simulation = simulate(std::move(design), "top");
    ASSERT_TRUE(simulation->ready());
    simulation->run(15);
    EXPECT_EQ(simulation->value("/top/seen"), "11z1")
        << "at 10, a's change had not crossed when a changed";
    EXPECT_EQ(simulation->value("/top/y"), "10z1");
}

// ---------------------------------------------------------------------------
// VHDL instantiating SystemVerilog
// ---------------------------------------------------------------------------

TEST_F(BoundaryTest, AVhdlSignalDrivesASystemVerilogInputAndReadsItsOutput) {
    // The child copies its input to its output; the parent drives '1', then 'L', then 'Z', and
    // reads back the bits they stand for, as codes.
    Design design;
    ModuleDefinition child;
    child.name = "svbuf";
    child.sourceName = "svbuf";
    child.addObject(Object{.name = "i", .type = Type::bits(1, false, true)});
    child.addObject(Object{.name = "o", .type = Type::bits(1, false, true)});
    child.ports.push_back(Port{.name = "i", .direction = PortDirection::Input, .objectIndex = 0});
    child.ports.push_back(Port{.name = "o", .direction = PortDirection::Output, .objectIndex = 1});
    ProcessBuilder copy(ProcessKind::ContinuousAssign, "copy");
    copy.sensitive(0, enzan::eir::EdgeKind::AnyChange);
    copy.block("entry");
    const std::uint32_t value = copy.reg(Type::bits(1, false, true));
    copy.readObject(value, 0);
    copy.store(1, value);
    copy.emit(Instruction::make(Opcode::Return));
    child.processes.push_back(copy.finish());
    design.addModule(std::move(child));

    ModuleDefinition top = vhdlDefinition("vtop");
    top.addObject(logicSignal("s", 1));
    top.addObject(logicSignal("r", 1));
    top.instances.push_back(Instance{
        .name = "u", .definitionName = "svbuf", .connections = {connect(0, 0), connect(1, 1)}});
    top.processes.push_back(
        vhdlDriver(0, 8, {codeText({Code1}), codeText({CodeL}), codeText({CodeZ})}));
    design.addModule(std::move(top));

    const std::unique_ptr<Simulation> simulation = simulate(std::move(design), "vtop");
    ASSERT_TRUE(simulation->ready());
    simulation->run(5);
    EXPECT_EQ(simulation->value("/vtop/r", Radix::Hexadecimal), "03") << "'1'";
    simulation->run(15);
    EXPECT_EQ(simulation->value("/vtop/r", Radix::Hexadecimal), "02") << "'L' is 0, back as '0'";
    simulation->run(25);
    EXPECT_EQ(simulation->value("/vtop/r", Radix::Hexadecimal), "04") << "'Z'";
}

// ---------------------------------------------------------------------------
// Inout
// ---------------------------------------------------------------------------

TEST_F(BoundaryTest, AnInoutReleasedToZIsNotHeldByWhatItDroveBefore) {
    // The VHDL child drives its inout '1', then releases it to 'Z'; the SystemVerilog parent's
    // wire has a driver of its own that drives z and then 0. The wire resolves the child's
    // contribution with its own, and the child reads the wire back: had the child's '1' come back
    // to it as a driver, the bus would hold 1 for ever after.
    Design design;
    ModuleDefinition child = vhdlDefinition("pad");
    Object pad = logicSignal("p", 1);
    pad.resolution = enzan::eir::Resolution{.function = "IEEE.STD_LOGIC_1164.RESOLVED(Y)U",
                                            .elementBits = 8,
                                            .loneDriverIdentity = true};
    child.addObject(std::move(pad));
    child.ports.push_back(Port{.name = "p", .direction = PortDirection::InOut, .objectIndex = 0});
    child.processes.push_back(vhdlDriver(0, 8, {codeText({Code1}), codeText({CodeZ})}));
    design.addModule(std::move(child));

    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    top.addObject(Object{.name = "w",
                         .type = Type::bits(1, false, true),
                         .kind = ObjectKind::Net,
                         .netKind = NetKind::Wire});
    top.addObject(Object{.name = "own", .type = Type::bits(1, false, true)});
    top.instances.push_back(
        Instance{.name = "u", .definitionName = "pad", .connections = {connect(0, 0)}});
    ProcessBuilder assign(ProcessKind::ContinuousAssign, "assign");
    assign.sensitive(1, enzan::eir::EdgeKind::AnyChange);
    assign.block("entry");
    const std::uint32_t own = assign.reg(Type::bits(1, false, true));
    assign.readObject(own, 1);
    assign.store(0, own);
    assign.emit(Instruction::make(Opcode::Return));
    top.processes.push_back(assign.finish());
    top.processes.push_back(svDriver(1, 1, {"1'bz", "1'bz", "1'b0"}));
    design.addModule(std::move(top));

    const std::unique_ptr<Simulation> simulation = simulate(std::move(design), "top");
    ASSERT_TRUE(simulation->ready());
    simulation->run(5);
    EXPECT_EQ(simulation->value("/top/w"), "1");
    EXPECT_EQ(simulation->value("/top/u/p", Radix::Hexadecimal), "03");
    simulation->run(15);
    EXPECT_EQ(simulation->value("/top/w"), "z") << "released, and not held by its own '1'";
    EXPECT_EQ(simulation->value("/top/u/p", Radix::Hexadecimal), "04");
    simulation->run(25);
    EXPECT_EQ(simulation->value("/top/w"), "0");
    EXPECT_EQ(simulation->value("/top/u/p", Radix::Hexadecimal), "02")
        << "the child reads what the other driver makes of the bus";
}

// ---------------------------------------------------------------------------
// Records, two-state targets and refusals
// ---------------------------------------------------------------------------

TEST_F(BoundaryTest, ARecordCrossesFieldByFieldIntoATwoStateStruct) {
    // A record of a std_ulogic, an integer and a bit, the first field highest, into a two-state
    // packed struct of one bit, 32 and one: 'H' is 1, 42 is 42, '1' is 1, and the struct's
    // two-state bits take an unknown as 0.
    Design design;
    ModuleDefinition child = vhdlDefinition("vrec");
    child.vhdlTypes.push_back(
        VhdlType{.kind = VhdlType::Kind::Record,
                 .name = "R",
                 .width = 48,
                 .fields = {VhdlType::Field{.name = "L", .type = StdUlogic, .offset = 40},
                            VhdlType::Field{.name = "N", .type = IntegerType, .offset = 8},
                            VhdlType::Field{.name = "B", .type = BitType, .offset = 0}}});
    child.addObject(Object{.name = "q",
                           .type = Type::bits(48, false, false),
                           .initialValue = BitVector::filled(48, enzan::eir::Logic::Zero),
                           .vhdlType = 3});
    child.ports.push_back(Port{.name = "q", .direction = PortDirection::Output, .objectIndex = 0});
    child.processes.push_back(vhdlDriver(0, 48, {"48'h070000002a01", "48'h010000000700"}));
    design.addModule(std::move(child));

    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    top.addObject(Object{.name = "s", .type = Type::bits(34, false, false)});
    top.instances.push_back(
        Instance{.name = "u", .definitionName = "vrec", .connections = {connect(0, 0)}});
    design.addModule(std::move(top));

    const std::unique_ptr<Simulation> simulation = simulate(std::move(design), "top");
    ASSERT_TRUE(simulation->ready());
    simulation->run(5);
    EXPECT_EQ(simulation->value("/top/s", Radix::Hexadecimal), "200000055") << "1, 42, 1";
    simulation->run(15);
    EXPECT_EQ(simulation->value("/top/s", Radix::Hexadecimal), "00000000e") << "X as 0, 7, 0";
}

TEST_F(BoundaryTest, ARealIsRefusedByName) {
    Design design;
    ModuleDefinition child = vhdlDefinition("vreal");
    child.vhdlTypes.push_back(
        VhdlType{.kind = VhdlType::Kind::Floating, .name = "REAL", .width = 64});
    child.addObject(Object{.name = "r",
                           .type = Type::bits(64, false, false),
                           .encoding = enzan::eir::Encoding::Real,
                           .vhdlType = 3});
    child.ports.push_back(Port{.name = "r", .direction = PortDirection::Input, .objectIndex = 0});
    design.addModule(std::move(child));

    ModuleDefinition top;
    top.name = "top";
    top.addObject(Object{.name = "x", .type = Type::bits(64, false, true)});
    top.instances.push_back(
        Instance{.name = "u", .definitionName = "vreal", .connections = {connect(0, 0)}});
    design.addModule(std::move(top));

    EXPECT_FALSE(enzan::elab::bridgeLanguages(design, engine));
    EXPECT_TRUE(reported(DiagCode::UnconvertiblePort));
}

TEST_F(BoundaryTest, SidesOfDifferentShapesAreRefused) {
    // Four std_ulogic codes are four bits on the SystemVerilog side, not eight.
    Design design;
    design.addModule(vhdlChild());
    ModuleDefinition top;
    top.name = "top";
    top.addObject(Object{.name = "a", .type = Type::bits(1, false, true)});
    top.addObject(Object{.name = "y", .type = Type::bits(8, false, true)});
    top.instances.push_back(Instance{
        .name = "u", .definitionName = "vchild", .connections = {connect(0, 0), connect(1, 1)}});
    design.addModule(std::move(top));

    EXPECT_FALSE(enzan::elab::bridgeLanguages(design, engine));
    EXPECT_TRUE(reported(DiagCode::UnconvertiblePort));
}

TEST_F(BoundaryTest, WhatAPortReadsStartsAsItsActualDoesConverted) {
    // A clock that starts at '1' is 1 in SystemVerilog before anything runs, so the first
    // conversion is no edge; and a SystemVerilog 0 is '0' in VHDL from the start, as a VHDL port
    // takes its actual's initial value.
    Design design;
    design.addModule(vhdlChild());
    ModuleDefinition svbuf;
    svbuf.name = "svbuf";
    svbuf.addObject(Object{.name = "i", .type = Type::bits(1, false, true)});
    svbuf.ports.push_back(Port{.name = "i", .direction = PortDirection::Input, .objectIndex = 0});
    design.addModule(std::move(svbuf));

    ModuleDefinition vtop = vhdlDefinition("vtop");
    vtop.addObject(logicSignal("clk", 1, Code1));
    vtop.instances.push_back(
        Instance{.name = "u", .definitionName = "svbuf", .connections = {connect(0, 0)}});
    design.addModule(std::move(vtop));

    ModuleDefinition top;
    top.name = "top";
    top.addObject(Object{.name = "a",
                         .type = Type::bits(1, false, true),
                         .initialValue = BitVector::fromUInt64(0, 1)});
    top.addObject(Object{.name = "y", .type = Type::bits(4, false, true)});
    top.instances.push_back(Instance{
        .name = "v", .definitionName = "vchild", .connections = {connect(0, 0), connect(1, 1)}});
    design.addModule(std::move(top));

    ASSERT_TRUE(enzan::elab::bridgeLanguages(design, engine));

    const ModuleDefinition& parent = *design.findModule("vtop");
    const Object& shown = parent.objects[require(parent.findObject("u$i$boundary"))];
    EXPECT_EQ(shown.initialValue, std::optional(BitVector::fromUInt64(1, 1)));

    const ModuleDefinition& svParent = *design.findModule("top");
    const Object& coded = svParent.objects[require(svParent.findObject("v$input$boundary"))];
    EXPECT_EQ(coded.initialValue, std::optional(codes({Code0})));
}

TEST_F(BoundaryTest, BridgingTwiceChangesNothingTheSecondTime) {
    Design design;
    design.addModule(vhdlChild());
    ModuleDefinition top;
    top.name = "top";
    top.addObject(Object{.name = "a", .type = Type::bits(1, false, true)});
    top.addObject(Object{.name = "y", .type = Type::bits(4, false, true)});
    top.instances.push_back(Instance{
        .name = "u", .definitionName = "vchild", .connections = {connect(0, 0), connect(1, 1)}});
    design.addModule(std::move(top));

    ASSERT_TRUE(enzan::elab::bridgeLanguages(design, engine));
    const ModuleDefinition once = *design.findModule("top");
    ASSERT_TRUE(enzan::elab::bridgeLanguages(design, engine));
    const ModuleDefinition& twice = *design.findModule("top");
    EXPECT_EQ(twice.objects.size(), once.objects.size());
    EXPECT_EQ(twice.processes.size(), once.processes.size());
}

TEST_F(BoundaryTest, APortOfOneLanguageIsLeftAsItIs) {
    Design design;
    ModuleDefinition child;
    child.name = "child";
    child.addObject(Object{.name = "i", .type = Type::bits(1, false, true)});
    child.ports.push_back(Port{.name = "i", .direction = PortDirection::Input, .objectIndex = 0});
    design.addModule(std::move(child));
    ModuleDefinition top;
    top.name = "top";
    top.addObject(Object{.name = "a", .type = Type::bits(1, false, true)});
    top.instances.push_back(
        Instance{.name = "u", .definitionName = "child", .connections = {connect(0, 0)}});
    design.addModule(std::move(top));

    ASSERT_TRUE(enzan::elab::bridgeLanguages(design, engine));
    EXPECT_EQ(design.findModule("top")->objects.size(), 1U);
    EXPECT_TRUE(design.findModule("top")->processes.empty());
}

} // namespace
