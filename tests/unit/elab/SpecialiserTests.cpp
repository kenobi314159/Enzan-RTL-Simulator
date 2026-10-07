// SPDX-License-Identifier: GPL-3.0-or-later
//
// The specialiser turns generic units into the concrete ones a top level needs (DESIGN_GUIDE.md
// section 18, M5.1). The generic units here are built by hand, so that what is tested is the
// specialiser's reading of the annotations rather than a frontend's choice of them.

#include "elab/Specialiser.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/ParameterExpression.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "elab/Elaborator.h"

#include "support/Optionals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::eir::BasicBlock;
using enzan::eir::BitVector;
using enzan::eir::Design;
using enzan::eir::FormalKind;
using enzan::eir::Instance;
using enzan::eir::Instruction;
using enzan::eir::InterfaceBinding;
using enzan::eir::ModuleDefinition;
using enzan::eir::NoExpression;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ParameterActual;
using enzan::eir::ParameterFormal;
using enzan::eir::ParameterNode;
using enzan::eir::ParameterNodeKind;
using enzan::eir::Port;
using enzan::eir::PortDirection;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::eir::WriteExtent;
using enzan::elab::Specialiser;
using enzan::test::require;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
        codes.push_back(diagnostic.code);
    }

    std::vector<std::string> messages;
    std::vector<DiagCode> codes;
};

BitVector intBits(std::int64_t value) {
    return BitVector::fromInt64(value, 32);
}

/// A generic `leaf` over one `int` formal `W`, defaulting to 8: an output `q` of `W` bits that an
/// `initial` block sets to all ones, the constant being `{W{1'b1}}`. Its representative is
/// `W = 8`, so every number the annotations replace is 8 as it stands.
ModuleDefinition genericLeaf() {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.sourceName = "leaf";

    enzan::eir::ParameterExpressions& table = leaf.expressions;
    const std::uint32_t w = table.formal(0, 32, true);
    const std::uint32_t one = table.constant(BitVector::fromUInt64(1, 1), false);
    const std::uint32_t ones =
        table.add(ParameterNode{.kind = ParameterNodeKind::Replicate, .a = w, .b = one});
    leaf.formals.push_back(ParameterFormal{.name = "W", .value = table.constant(intBits(8), true)});
    leaf.parameters.push_back(enzan::eir::Parameter{.name = "W", .value = intBits(8)});

    leaf.addObject(Object{.name = "q", .type = Type::bits(8, false, true), .widthExpression = w});
    leaf.ports.push_back(Port{.name = "q", .direction = PortDirection::Output, .objectIndex = 0});

    Process process;
    process.kind = ProcessKind::Initial;
    process.name = "initial@leaf";
    process.registerTypes = {Type::bits(8, false, true)};
    process.registerWidthExpressions = {w};
    process.constants = {BitVector::fromUInt64(0xff, 8)};
    process.constantExpressions = {ones};
    process.writeSet = {
        WriteExtent{.objectIndex = 0, .lowBit = 0, .width = 8, .widthExpression = w}};
    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(
        Instruction::make(Opcode::LoadConstant, Operand::reg(0), Operand::constant(0)));
    entry.instructions.push_back(
        Instruction::make(Opcode::Store, {}, Operand::object(0), Operand::reg(0)));
    entry.instructions.push_back(Instruction::make(Opcode::Return));
    process.blocks.push_back(std::move(entry));
    leaf.processes.push_back(std::move(process));
    return leaf;
}

/// The unit `name` of `units`, which the test requires to exist.
const ModuleDefinition& unitNamed(const Design& units, std::string_view name) {
    const ModuleDefinition* unit = units.findModule(name);
    EXPECT_NE(unit, nullptr) << "no unit named " << name;
    static const ModuleDefinition missing;
    return unit != nullptr ? *unit : missing;
}

/// A top instantiating `leaf` once per entry of `widths`, connected to nothing; an absent width
/// leaves `W` at its default.
ModuleDefinition topOf(const std::vector<std::optional<std::int64_t>>& widths) {
    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    for (std::size_t index = 0; index < widths.size(); ++index) {
        Instance instance;
        instance.name = "u" + std::to_string(index);
        instance.definitionName = "leaf";
        if (widths[index].has_value()) {
            instance.parameterActuals.push_back(ParameterActual{
                .formal = "W", .value = top.expressions.constant(intBits(*widths[index]), true)});
        }
        top.instances.push_back(std::move(instance));
    }
    return top;
}

class SpecialiserTest : public ::testing::Test {
protected:
    SpecialiserTest() : engine(sources) { engine.addConsumer(consumer); }

    std::optional<Specialiser::Result> specialise(const Design& design,
                                                  std::string_view top = "top") {
        Specialiser specialiser(design, engine);
        return specialiser.specialise(require(design.findModuleIndex(top)));
    }

    [[nodiscard]] bool reported(DiagCode code) const {
        return std::ranges::find(consumer.codes, code) != consumer.codes.end();
    }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
};

TEST_F(SpecialiserTest, EachSetOfValuesIsOneConcreteUnit) {
    Design design;
    design.addModule(genericLeaf());
    design.addModule(topOf({4, 16, std::nullopt, 4}));

    const std::optional<Specialiser::Result> result = specialise(design);
    ASSERT_TRUE(result.has_value()) << consumer.messages.front();
    const Design& units = *result->units;

    // Three distinct values, three units, each named for its value; the two instances setting 4
    // share one.
    const ModuleDefinition& top = units.module(result->top);
    std::vector<std::string> instantiated;
    for (const Instance& instance : top.instances) instantiated.push_back(instance.definitionName);
    EXPECT_EQ(instantiated,
              (std::vector<std::string>{"leaf$W_4", "leaf$W_16", "leaf$W_8", "leaf$W_4"}));
    EXPECT_EQ(units.size(), 4U);

    for (const auto& [name, width, ones] :
         std::vector<std::tuple<std::string, std::uint32_t, std::uint64_t>>{
             {"leaf$W_4", 4, 0xf}, {"leaf$W_16", 16, 0xffff}, {"leaf$W_8", 8, 0xff}}) {
        const ModuleDefinition& leaf = unitNamed(units, name);
        if (leaf.processes.empty()) continue;
        EXPECT_FALSE(leaf.isGeneric());
        EXPECT_EQ(leaf.sourceName, "leaf");
        EXPECT_EQ(leaf.objects[0].type, Type::bits(width, false, true)) << name;
        const Process& process = leaf.processes[0];
        EXPECT_EQ(process.registerTypes[0], Type::bits(width, false, true)) << name;
        EXPECT_EQ(process.constants[0], BitVector::fromUInt64(ones, width)) << name;
        EXPECT_EQ(process.writeSet[0].width, width) << name;
        EXPECT_TRUE(process.registerWidthExpressions.empty());
        EXPECT_TRUE(process.constantExpressions.empty());
        ASSERT_EQ(leaf.parameters.size(), 1U);
        EXPECT_EQ(leaf.parameters[0].value, intBits(width)) << "shown as what it resolved to";
    }
}

TEST_F(SpecialiserTest, OnlyWhatTheTopReachesIsSpecialised) {
    Design design;
    design.addModule(genericLeaf());
    design.addModule(topOf({4}));
    ModuleDefinition unused;
    unused.name = "unused";
    design.addModule(std::move(unused));

    const std::optional<Specialiser::Result> result = specialise(design);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->units->size(), 2U);
    EXPECT_EQ(result->units->findModule("unused"), nullptr);
}

TEST_F(SpecialiserTest, AParameterWithNoValueIsReported) {
    ModuleDefinition leaf = genericLeaf();
    leaf.formals[0].value = NoExpression;
    Design design;
    design.addModule(std::move(leaf));
    design.addModule(topOf({std::nullopt}));

    EXPECT_FALSE(specialise(design).has_value());
    EXPECT_TRUE(reported(DiagCode::ParameterHasNoValue));
    EXPECT_NE(consumer.messages.front().find("/top/u0"), std::string::npos)
        << "the instance that left it unset is named: " << consumer.messages.front();
}

TEST_F(SpecialiserTest, AnActualNamingNoFormalIsReported) {
    ModuleDefinition top = topOf({4});
    top.instances[0].parameterActuals[0].formal = "DEPTH";
    Design design;
    design.addModule(genericLeaf());
    design.addModule(std::move(top));

    EXPECT_FALSE(specialise(design).has_value());
    EXPECT_TRUE(reported(DiagCode::UnknownParameterActual));
}

TEST_F(SpecialiserTest, AWidthThatComesOutZeroIsReported) {
    Design design;
    design.addModule(genericLeaf());
    design.addModule(topOf({0}));

    EXPECT_FALSE(specialise(design).has_value());
    EXPECT_TRUE(reported(DiagCode::ParameterEvaluationFailed));
}

TEST_F(SpecialiserTest, AnUntypedParameterGivenAnotherTypeIsRefused) {
    // Every width depending on an untyped parameter was translated with the representative's
    // type; a 16-bit value would change them all, which only analysis can redo.
    ModuleDefinition leaf = genericLeaf();
    leaf.formals[0].isTyped = false;
    ModuleDefinition top = topOf({std::nullopt});
    top.instances[0].parameterActuals.push_back(ParameterActual{
        .formal = "W", .value = top.expressions.constant(BitVector::fromUInt64(4, 16), false)});
    Design design;
    design.addModule(std::move(leaf));
    design.addModule(std::move(top));

    EXPECT_FALSE(specialise(design).has_value());
    EXPECT_TRUE(reported(DiagCode::UntypedParameterType));
    EXPECT_NE(consumer.messages.front().find("16-bit unsigned value"), std::string::npos)
        << consumer.messages.front();
}

TEST_F(SpecialiserTest, ATypedParameterConvertsWhatItIsGiven) {
    // `parameter int W` given `16'd12` is the int 12, as an assignment would make it.
    ModuleDefinition top = topOf({std::nullopt});
    top.instances[0].parameterActuals.push_back(ParameterActual{
        .formal = "W", .value = top.expressions.constant(BitVector::fromUInt64(12, 16), false)});
    Design design;
    design.addModule(genericLeaf());
    design.addModule(std::move(top));

    const std::optional<Specialiser::Result> result = specialise(design);
    ASSERT_TRUE(result.has_value()) << consumer.messages.front();
    EXPECT_NE(result->units->findModule("leaf$W_12"), nullptr);
}

TEST_F(SpecialiserTest, AnInterfacesParametersAreReadThroughThePort) {
    // `sink` reads `W` from the interface its port `b` is connected to, as `localparam W = b.W`
    // does, and declares `copy` that wide. Two buses of two widths give two sinks.
    ModuleDefinition bus;
    bus.name = "bus_if";
    bus.sourceName = "bus_if";
    bus.formals.push_back(
        ParameterFormal{.name = "W", .value = bus.expressions.constant(intBits(8), true)});
    bus.parameters.push_back(enzan::eir::Parameter{.name = "W", .value = intBits(8)});

    ModuleDefinition sink;
    sink.name = "sink";
    sink.sourceName = "sink";
    const std::uint32_t w = sink.expressions.formal(0, 32, true);
    sink.formals.push_back(
        ParameterFormal{.name = "W", .kind = FormalKind::InterfaceParameter, .port = "b"});
    sink.addObject(
        Object{.name = "copy", .type = Type::bits(8, false, true), .widthExpression = w});

    ModuleDefinition top;
    top.name = "top";
    top.sourceName = "top";
    for (const auto& [name, width] :
         std::vector<std::pair<std::string, std::int64_t>>{{"narrow", 8}, {"wide", 16}}) {
        Instance interface;
        interface.name = name;
        interface.definitionName = "bus_if";
        interface.parameterActuals.push_back(ParameterActual{
            .formal = "W", .value = top.expressions.constant(intBits(width), true)});
        top.instances.push_back(std::move(interface));
    }
    for (const std::string target : {"narrow", "wide"}) {
        Instance instance;
        instance.name = "u_" + target;
        instance.definitionName = "sink";
        instance.interfaceBindings.push_back(InterfaceBinding{.port = "b", .target = target});
        top.instances.push_back(std::move(instance));
    }

    Design design;
    design.addModule(std::move(bus));
    design.addModule(std::move(sink));
    design.addModule(std::move(top));

    const std::optional<Specialiser::Result> result = specialise(design);
    ASSERT_TRUE(result.has_value()) << consumer.messages.front();
    const ModuleDefinition& concreteTop = result->units->module(result->top);
    const ModuleDefinition& narrow =
        unitNamed(*result->units, concreteTop.instances[2].definitionName);
    const ModuleDefinition& wide =
        unitNamed(*result->units, concreteTop.instances[3].definitionName);
    ASSERT_FALSE(narrow.objects.empty() || wide.objects.empty());
    EXPECT_NE(narrow.name, wide.name);
    EXPECT_EQ(narrow.objects[0].type.bitWidth(), 8U);
    EXPECT_EQ(wide.objects[0].type.bitWidth(), 16U);
}

TEST_F(SpecialiserTest, AGenericDesignElaboratesToItsSpecialisations) {
    // Through the elaborator, which is how a design meets the specialiser: every instance is of
    // a concrete unit, and its slots are as wide as its own parameterisation says.
    Design design;
    design.addModule(genericLeaf());
    design.addModule(topOf({4, 16}));

    enzan::elab::Elaborator elaborator(design, engine);
    const std::unique_ptr<enzan::elab::ElaboratedDesign> elaborated = elaborator.elaborate("top");
    ASSERT_NE(elaborated, nullptr) << consumer.messages.front();
    EXPECT_EQ(elaborated->slots()[require(elaborated->findSlot("/top/u0/q"))].type.bitWidth(), 4U);
    EXPECT_EQ(elaborated->slots()[require(elaborated->findSlot("/top/u1/q"))].type.bitWidth(), 16U);
    for (const enzan::elab::Instance& instance : elaborated->instances())
        EXPECT_FALSE(elaborated->units().module(instance.definitionIndex).isGeneric());
}

} // namespace
