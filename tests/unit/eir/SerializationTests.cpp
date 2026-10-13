// SPDX-License-Identifier: GPL-3.0-or-later
//
// A library entry that decodes into something subtly different from what was encoded is the
// worst failure this component can have: it simulates, and it gives the wrong answer. So the
// tests here compare a decoded unit against the original field by field rather than checking
// that decoding "succeeded".

#include "eir/Serialization.h"

#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using enzan::BufferId;
using enzan::SourceLocation;
using enzan::SourceManager;
using enzan::eir::BasicBlock;
using enzan::eir::BitVector;
using enzan::eir::CallSite;
using enzan::eir::EdgeKind;
using enzan::eir::Encoding;
using enzan::eir::EnumDefinition;
using enzan::eir::EnumMember;
using enzan::eir::Instance;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::NetKind;
using enzan::eir::Object;
using enzan::eir::ObjectKind;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::Parameter;
using enzan::eir::ParameterMode;
using enzan::eir::Port;
using enzan::eir::PortConnection;
using enzan::eir::PortDirection;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::SensitivityEntry;
using enzan::eir::Subprogram;
using enzan::eir::SubprogramParameter;
using enzan::eir::SysTaskArgument;
using enzan::eir::SysTaskCall;
using enzan::eir::SysTaskKind;
using enzan::eir::Type;
using enzan::eir::UnpackedDimension;
using enzan::eir::WriteExtent;
using enzan::test::literal;

class SerializationTest : public ::testing::Test {
protected:
    SerializationTest() {
        buffer = sources.addBuffer("counter.sv", "module counter;\nendmodule\n");
    }

    /// Encodes `modules` and decodes them again, resolving names against `sources`.
    std::vector<ModuleDefinition> roundTrip(const std::vector<ModuleDefinition>& modules) {
        const std::string bytes = enzan::eir::encodeModules(modules, sources);

        std::vector<ModuleDefinition> decoded;
        EXPECT_TRUE(enzan::eir::decodeModules(bytes, resolver(), decoded));
        return decoded;
    }

    [[nodiscard]] std::function<BufferId(std::string_view)> resolver() const {
        return [this](std::string_view name) -> BufferId {
            return name == sources.bufferName(buffer) ? buffer : BufferId();
        };
    }

    SourceManager sources;
    BufferId buffer;
};

/// A unit that uses every field the encoding carries.
ModuleDefinition richModule(BufferId buffer) {
    ModuleDefinition definition;
    definition.name = "counter$WIDTH=8";
    definition.sourceName = "counter";
    definition.location = SourceLocation(buffer, 3);

    EnumDefinition states;
    states.name = "state_e";
    states.members.push_back(EnumMember{.name = "S_IDLE", .value = literal("2'b00")});
    states.members.push_back(EnumMember{.name = "S_RUN", .value = literal("2'b01")});
    definition.enums.push_back(std::move(states));

    definition.addObject(Object{.name = "clk",
                                .type = Type::logic(),
                                .kind = ObjectKind::Net,
                                .netKind = NetKind::Tri1,
                                .location = SourceLocation(buffer, 11),
                                .unpackedDimensions = {UnpackedDimension{.left = 0, .right = 3},
                                                       UnpackedDimension{.left = 7, .right = -1}},
                                .elementWidth = 1,
                                .elementSigned = true,
                                .subroutineStatic = true});
    definition.addObject(Object{.name = "count",
                                .type = Type::bits(8, true, true),
                                .kind = ObjectKind::Variable,
                                .location = SourceLocation(buffer, 17),
                                .initialValue = literal("8'hA5"),
                                .isAlias = true,
                                .encoding = Encoding::VhdlLogic,
                                .enumIndex = 0});

    definition.ports.push_back(Port{.name = "clk",
                                    .direction = PortDirection::Input,
                                    .objectIndex = 0,
                                    .location = SourceLocation(buffer, 5)});
    definition.ports.push_back(Port{.name = "count",
                                    .direction = PortDirection::InOut,
                                    .objectIndex = 1,
                                    .location = SourceLocation(buffer, 9)});

    Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.location = SourceLocation(buffer, 21);
    instance.connections.push_back(PortConnection{.portIndex = 0, .objectIndex = 1});
    instance.connections.push_back(
        PortConnection{.portIndex = 1, .objectIndex = PortConnection::Unconnected});
    definition.instances.push_back(std::move(instance));

    Process process;
    process.kind = ProcessKind::AlwaysFf;
    process.drivesSignals = true;
    process.name = "always_ff@counter.sv:14";
    process.location = SourceLocation(buffer, 14);
    process.registerTypes = {Type::logic(), Type::bits(8, false, false), Type::real(),
                             Type::string(), Type::voidType()};
    process.constants = {literal("8'd1"), literal("4'b10xz")};
    process.strings = {"value = %d", ""};

    // Instruction locations in the shape the frontend produces them: a run sharing the
    // statement that emitted them, then a change, then a block boundary that continues the
    // previous statement. That is what the encoding's repeat flag has to reproduce, and it is
    // what a source-line breakpoint is resolved against.
    BasicBlock entry;
    entry.label = "entry";
    entry.instructions.push_back(
        Instruction::make(Opcode::LoadConstant, Operand::reg(1), Operand::constant(0)));
    entry.instructions.back().location = SourceLocation(buffer, 20);
    entry.instructions.push_back(
        Instruction::makeSigned(Opcode::Add, Operand::reg(1), Operand::reg(1), Operand::reg(0)));
    entry.instructions.back().location = SourceLocation(buffer, 20);
    entry.instructions.push_back(Instruction::make(Opcode::Jump, {}, Operand::block(1)));
    entry.instructions.back().location = SourceLocation(buffer, 31);
    process.blocks.push_back(std::move(entry));

    BasicBlock second;
    second.label = "body";
    second.instructions.push_back(Instruction::make(Opcode::Return));
    second.instructions.back().location = SourceLocation(buffer, 31);
    process.blocks.push_back(std::move(second));

    SysTaskCall call;
    call.kind = SysTaskKind::Display;
    call.formatIndex = 0;
    call.location = SourceLocation(buffer, 15);
    call.arguments.push_back(
        SysTaskArgument{.value = Operand::reg(1), .type = Type::bits(8, true, true)});
    process.sysCalls.push_back(std::move(call));

    SysTaskCall unformatted;
    unformatted.kind = SysTaskKind::Finish;
    process.sysCalls.push_back(std::move(unformatted));

    process.sensitivity.push_back(SensitivityEntry{.objectIndex = 0, .edge = EdgeKind::Posedge});
    process.readSet = {0, 1};
    // Two extents of one object, with every field different, so that a field written in the
    // wrong order or an extent dropped shows up.
    process.writeSet = {WriteExtent{.objectIndex = 1, .lowBit = 0, .width = 2},
                        WriteExtent{.objectIndex = 1, .lowBit = 4, .width = 3}};
    process.calls.push_back(CallSite{.definition = "arith::",
                                     .subprogram = "accumulate$8",
                                     .arguments = {Operand::reg(1), Operand::constant(0)},
                                     .result = Operand::reg(1),
                                     .location = SourceLocation(buffer, 22)});
    definition.processes.push_back(std::move(process));

    // A subprogram, with a parameter of each mode but one, a result, and a call of its own: a
    // body is written once for both kinds, so this is what shows the subprogram's half of it.
    Subprogram accumulate;
    accumulate.name = "accumulate$8";
    accumulate.location = SourceLocation(buffer, 30);
    accumulate.registerTypes = {Type::bits(8, false, true), Type::bits(8, false, true),
                                Type::bits(8, false, true)};
    accumulate.parameters = {
        SubprogramParameter{.name = "total", .mode = ParameterMode::InOut, .registerIndex = 0},
        SubprogramParameter{.name = "step", .mode = ParameterMode::In, .registerIndex = 1}};
    accumulate.resultType = Type::bits(8, false, true);
    accumulate.resultRegister = 2;
    accumulate.readSet = {1};
    accumulate.calls.push_back(CallSite{.definition = "",
                                        .subprogram = "accumulate$8",
                                        .arguments = {Operand::reg(0), Operand::reg(1)},
                                        .result = Operand::reg(2),
                                        .location = SourceLocation(buffer, 31)});
    BasicBlock accumulateEntry;
    accumulateEntry.label = "entry";
    accumulateEntry.instructions.push_back(Instruction::make(Opcode::Call, {}, Operand::call(0)));
    accumulateEntry.instructions.push_back(Instruction::make(Opcode::Return));
    accumulate.blocks.push_back(std::move(accumulateEntry));
    definition.subprograms.push_back(std::move(accumulate));

    definition.parameters.push_back(Parameter{.name = "WIDTH",
                                              .value = literal("32'd8"),
                                              .isLocal = false,
                                              .location = SourceLocation(buffer, 1)});
    definition.parameters.push_back(Parameter{.name = "DEPTH",
                                              .value = literal("32'd16"),
                                              .isLocal = true,
                                              .location = SourceLocation(buffer, 2)});
    return definition;
}

TEST_F(SerializationTest, AnEmptyLibraryRoundTrips) {
    const std::vector<ModuleDefinition> decoded = roundTrip({});
    EXPECT_TRUE(decoded.empty());
}

TEST_F(SerializationTest, ModuleIdentityRoundTrips) {
    const std::vector<ModuleDefinition> decoded = roundTrip({richModule(buffer)});
    ASSERT_EQ(decoded.size(), 1U);

    EXPECT_EQ(decoded.front().name, "counter$WIDTH=8");
    EXPECT_EQ(decoded.front().sourceName, "counter");
    EXPECT_EQ(decoded.front().location, SourceLocation(buffer, 3));
}

TEST_F(SerializationTest, ObjectsRoundTrip) {
    const std::vector<ModuleDefinition> decoded = roundTrip({richModule(buffer)});
    ASSERT_EQ(decoded.size(), 1U);
    const ModuleDefinition& definition = decoded.front();

    ASSERT_EQ(definition.objects.size(), 2U);
    EXPECT_EQ(definition.objects[0].name, "clk");
    EXPECT_EQ(definition.objects[0].kind, ObjectKind::Net);
    EXPECT_EQ(definition.objects[0].netKind, NetKind::Tri1);
    EXPECT_EQ(definition.objects[0].unpackedDimensions,
              (std::vector<UnpackedDimension>{UnpackedDimension{.left = 0, .right = 3},
                                              UnpackedDimension{.left = 7, .right = -1}}));
    EXPECT_EQ(definition.objects[0].elementWidth, 1U);
    EXPECT_TRUE(definition.objects[0].elementSigned);
    EXPECT_TRUE(definition.objects[0].subroutineStatic);
    EXPECT_FALSE(definition.objects[0].initialValue.has_value());
    EXPECT_EQ(definition.objects[0].enumIndex, Object::NoEnum);

    EXPECT_EQ(definition.objects[1].name, "count");
    EXPECT_EQ(definition.objects[1].type.bitWidth(), 8U);
    EXPECT_TRUE(definition.objects[1].type.isSigned());
    EXPECT_TRUE(definition.objects[1].type.isFourState());
    EXPECT_TRUE(definition.objects[1].isAlias);
    EXPECT_EQ(definition.objects[1].enumIndex, 0U);
    EXPECT_EQ(definition.objects[1].encoding, Encoding::VhdlLogic);
    EXPECT_EQ(definition.objects[0].encoding, Encoding::Bits);
    ASSERT_TRUE(definition.objects[1].initialValue.has_value());
    // Read through `value_or` rather than dereferenced: the assertion above already returns
    // from the test on an empty optional, but clang-tidy follows an `if` and not a GoogleTest
    // assertion, and a stand-in that can never be compared equal says the same thing.
    EXPECT_EQ(definition.objects[1].initialValue.value_or(literal("1'bx")), literal("8'hA5"));
    EXPECT_EQ(definition.objects[1].location, SourceLocation(buffer, 17));
}

TEST_F(SerializationTest, PortsAndInstancesRoundTrip) {
    const std::vector<ModuleDefinition> decoded = roundTrip({richModule(buffer)});
    const ModuleDefinition& definition = decoded.front();

    ASSERT_EQ(definition.ports.size(), 2U);
    EXPECT_EQ(definition.ports[1].direction, PortDirection::InOut);
    EXPECT_EQ(definition.ports[1].objectIndex, 1U);

    ASSERT_EQ(definition.instances.size(), 1U);
    EXPECT_EQ(definition.instances[0].name, "u_child");
    EXPECT_EQ(definition.instances[0].definitionName, "child");
    ASSERT_EQ(definition.instances[0].connections.size(), 2U);
    EXPECT_TRUE(definition.instances[0].connections[0].connected());
    EXPECT_FALSE(definition.instances[0].connections[1].connected());
}

TEST_F(SerializationTest, ProcessesRoundTrip) {
    const std::vector<ModuleDefinition> decoded = roundTrip({richModule(buffer)});
    ASSERT_EQ(decoded.front().processes.size(), 1U);
    const Process& process = decoded.front().processes.front();

    EXPECT_EQ(process.kind, ProcessKind::AlwaysFf);
    EXPECT_TRUE(process.drivesSignals);
    EXPECT_EQ(process.name, "always_ff@counter.sv:14");
    EXPECT_EQ(process.location, SourceLocation(buffer, 14));

    ASSERT_EQ(process.registerTypes.size(), 5U);
    EXPECT_EQ(process.registerTypes[2].kind(), Type::Kind::Real);
    EXPECT_EQ(process.registerTypes[3].kind(), Type::Kind::String);
    EXPECT_EQ(process.registerTypes[4].kind(), Type::Kind::Void);

    ASSERT_EQ(process.constants.size(), 2U);
    EXPECT_EQ(process.constants[1], literal("4'b10xz")) << "X and Z survive the round trip";

    EXPECT_EQ(process.strings, (std::vector<std::string>{"value = %d", ""}));

    ASSERT_EQ(process.blocks.size(), 2U);
    EXPECT_EQ(process.blocks[0].label, "entry");
    ASSERT_EQ(process.blocks[0].instructions.size(), 3U);
    EXPECT_EQ(process.blocks[0].instructions[1].opcode, Opcode::Add);
    EXPECT_TRUE(process.blocks[0].instructions[1].isSigned);
    EXPECT_EQ(process.blocks[0].instructions[2].a, Operand::block(1));
    EXPECT_FALSE(process.blocks[1].instructions[0].dst.valid());

    ASSERT_EQ(process.sysCalls.size(), 2U);
    EXPECT_EQ(process.sysCalls[0].kind, SysTaskKind::Display);
    EXPECT_TRUE(process.sysCalls[0].hasFormat());
    ASSERT_EQ(process.sysCalls[0].arguments.size(), 1U);
    EXPECT_EQ(process.sysCalls[0].arguments[0].value, Operand::reg(1));
    EXPECT_TRUE(process.sysCalls[0].arguments[0].type.isSigned());
    EXPECT_FALSE(process.sysCalls[1].hasFormat());

    ASSERT_EQ(process.sensitivity.size(), 1U);
    EXPECT_EQ(process.sensitivity[0].edge, EdgeKind::Posedge);
    EXPECT_EQ(process.readSet, (std::vector<std::uint32_t>{0, 1}));
    EXPECT_EQ(process.writeSet,
              (std::vector<WriteExtent>{WriteExtent{.objectIndex = 1, .lowBit = 0, .width = 2},
                                        WriteExtent{.objectIndex = 1, .lowBit = 4, .width = 3}}));
}

TEST_F(SerializationTest, SubprogramsAndCallsRoundTrip) {
    const std::vector<ModuleDefinition> decoded = roundTrip({richModule(buffer)});
    const ModuleDefinition& definition = decoded.front();

    ASSERT_EQ(definition.processes.front().calls.size(), 1U);
    const CallSite& call = definition.processes.front().calls.front();
    EXPECT_EQ(call.definition, "arith::");
    EXPECT_EQ(call.subprogram, "accumulate$8");
    EXPECT_EQ(call.arguments, (std::vector<Operand>{Operand::reg(1), Operand::constant(0)}));
    EXPECT_EQ(call.result, Operand::reg(1));
    EXPECT_EQ(call.location, SourceLocation(buffer, 22));

    ASSERT_EQ(definition.subprograms.size(), 1U);
    const Subprogram& subprogram = definition.subprograms.front();
    EXPECT_EQ(subprogram.name, "accumulate$8");
    EXPECT_EQ(subprogram.location, SourceLocation(buffer, 30));
    EXPECT_EQ(subprogram.registerTypes.size(), 3U);
    EXPECT_EQ(
        subprogram.parameters,
        (std::vector<SubprogramParameter>{
            SubprogramParameter{.name = "total", .mode = ParameterMode::InOut, .registerIndex = 0},
            SubprogramParameter{.name = "step", .mode = ParameterMode::In, .registerIndex = 1}}));
    EXPECT_EQ(subprogram.resultType, Type::bits(8, false, true));
    EXPECT_EQ(subprogram.resultRegister, 2U);
    EXPECT_EQ(subprogram.readSet, (std::vector<std::uint32_t>{1}));
    ASSERT_EQ(subprogram.calls.size(), 1U);
    EXPECT_TRUE(subprogram.calls.front().definition.empty());
    EXPECT_FALSE(subprogram.calls.front().arguments.empty());
    ASSERT_EQ(subprogram.blocks.size(), 1U);
    EXPECT_EQ(subprogram.blocks.front().instructions.front().a, Operand::call(0));
}

TEST_F(SerializationTest, AProcedureRoundTripsWithoutAResult) {
    ModuleDefinition definition = richModule(buffer);
    definition.subprograms.front().resultType = Type::voidType();

    const std::vector<ModuleDefinition> decoded = roundTrip({definition});
    EXPECT_FALSE(decoded.front().subprograms.front().isFunction());
}

TEST_F(SerializationTest, InstructionLocationsRoundTrip) {
    // Without these a unit served from the library decodes cleanly, simulates identically, and
    // silently cannot be debugged: every breakpoint on it is refused as a line that compiled to
    // no instructions (DESIGN_GUIDE.md section 10.4).
    const std::vector<ModuleDefinition> decoded = roundTrip({richModule(buffer)});
    ASSERT_EQ(decoded.front().processes.size(), 1U);
    const Process& process = decoded.front().processes.front();

    ASSERT_EQ(process.blocks.size(), 2U);
    ASSERT_EQ(process.blocks[0].instructions.size(), 3U);

    // The run that shares one statement, which the encoding stores once.
    EXPECT_EQ(process.blocks[0].instructions[0].location, SourceLocation(buffer, 20));
    EXPECT_EQ(process.blocks[0].instructions[1].location, SourceLocation(buffer, 20));

    // The change, and the block boundary that continues it.
    EXPECT_EQ(process.blocks[0].instructions[2].location, SourceLocation(buffer, 31));
    EXPECT_EQ(process.blocks[1].instructions[0].location, SourceLocation(buffer, 31));
}

TEST_F(SerializationTest, AnInstructionWithNoLocationStaysWithoutOne) {
    ModuleDefinition definition;
    definition.name = "m";
    definition.location = SourceLocation(buffer, 0);

    Process process;
    process.kind = ProcessKind::Initial;
    BasicBlock block;
    block.label = "entry";
    block.instructions.push_back(Instruction::make(Opcode::Return));
    process.blocks.push_back(std::move(block));
    definition.processes.push_back(std::move(process));

    const std::vector<ModuleDefinition> decoded = roundTrip({std::move(definition)});
    ASSERT_EQ(decoded.size(), 1U);
    ASSERT_EQ(decoded.front().processes.size(), 1U);
    ASSERT_EQ(decoded.front().processes[0].blocks.size(), 1U);
    ASSERT_EQ(decoded.front().processes[0].blocks[0].instructions.size(), 1U);

    // A frontend with no position for an instruction leaves it invalid, and the encoding must
    // not invent one: a breakpoint that matched there would stop somewhere the user never wrote.
    EXPECT_FALSE(decoded.front().processes[0].blocks[0].instructions[0].location.valid());
}

TEST_F(SerializationTest, EnumerationsAndParametersRoundTrip) {
    const std::vector<ModuleDefinition> decoded = roundTrip({richModule(buffer)});
    const ModuleDefinition& definition = decoded.front();

    ASSERT_EQ(definition.enums.size(), 1U);
    EXPECT_EQ(definition.enums[0].name, "state_e");
    ASSERT_EQ(definition.enums[0].members.size(), 2U);
    EXPECT_EQ(definition.enums[0].members[1].name, "S_RUN");
    EXPECT_EQ(definition.enums[0].members[1].value, literal("2'b01"));

    ASSERT_EQ(definition.parameters.size(), 2U);
    EXPECT_EQ(definition.parameters[0].name, "WIDTH");
    EXPECT_FALSE(definition.parameters[0].isLocal);
    EXPECT_EQ(definition.parameters[1].value, literal("32'd16"));
    EXPECT_TRUE(definition.parameters[1].isLocal);
}

TEST_F(SerializationTest, SeveralModulesRoundTripInOrder) {
    ModuleDefinition first;
    first.name = "first";
    first.location = SourceLocation(buffer, 0);
    ModuleDefinition second = richModule(buffer);
    ModuleDefinition third;
    third.name = "third";
    third.location = SourceLocation(buffer, 1);

    const std::vector<ModuleDefinition> decoded =
        roundTrip({std::move(first), std::move(second), std::move(third)});

    ASSERT_EQ(decoded.size(), 3U);
    EXPECT_EQ(decoded[0].name, "first");
    EXPECT_EQ(decoded[1].name, "counter$WIDTH=8");
    EXPECT_EQ(decoded[2].name, "third");
}

TEST_F(SerializationTest, ALocationInAFileThatCannotBeResolvedBecomesInvalid) {
    const BufferId other = sources.addBuffer("header.svh", "`define W 8\n");
    ModuleDefinition definition;
    definition.name = "m";
    definition.location = SourceLocation(other, 4);

    const std::string bytes = enzan::eir::encodeModules({&definition, 1}, sources);

    std::vector<ModuleDefinition> decoded;
    ASSERT_TRUE(enzan::eir::decodeModules(bytes, resolver(), decoded));
    ASSERT_EQ(decoded.size(), 1U);

    // A library may name an include file that has since been deleted. Losing the snippet is
    // the right price; refusing to load the design over it would not be.
    EXPECT_FALSE(decoded.front().location.valid());
}

TEST_F(SerializationTest, EncodingIsStableForTheSameInput) {
    // A library entry is named by a hash of what produced it, so two encodings of one design
    // have to be byte-identical or the store would never hit.
    const std::vector<ModuleDefinition> modules{richModule(buffer)};
    EXPECT_EQ(enzan::eir::encodeModules(modules, sources),
              enzan::eir::encodeModules(modules, sources));
}

TEST_F(SerializationTest, ContentLeavesOutWhereAUnitCameFrom) {
    // A unit a session analysed has its locations; the same unit read from a library has none.
    // They are one unit, and two libraries holding it must be usable together (section 8.2).
    const std::vector<ModuleDefinition> analysed{richModule(buffer)};
    std::vector<ModuleDefinition> stored;
    ASSERT_TRUE(enzan::eir::decodeModules(
        enzan::eir::encodeModules(analysed, sources), [](std::string_view) { return BufferId(); },
        stored));

    EXPECT_NE(enzan::eir::encodeModules(analysed, sources),
              enzan::eir::encodeModules(stored, sources));
    EXPECT_EQ(enzan::eir::encodeContent(analysed.front(), sources),
              enzan::eir::encodeContent(stored.front(), sources));
}

TEST_F(SerializationTest, ContentDiffersWhenTheUnitDoes) {
    const ModuleDefinition one = richModule(buffer);
    ModuleDefinition other = richModule(buffer);
    other.objects[1].type = Type::bits(9, true, true);

    EXPECT_NE(enzan::eir::encodeContent(one, sources), enzan::eir::encodeContent(other, sources));
}

// ---------------------------------------------------------------------------
// Refusing what is not a library of this version
// ---------------------------------------------------------------------------

TEST_F(SerializationTest, RefusesSomethingThatIsNotALibrary) {
    std::vector<ModuleDefinition> decoded;
    EXPECT_FALSE(enzan::eir::decodeModules("not a library at all", resolver(), decoded));
    EXPECT_FALSE(enzan::eir::decodeModules("", resolver(), decoded));
    EXPECT_TRUE(decoded.empty());
}

TEST_F(SerializationTest, RefusesAnotherFormatVersion) {
    std::string bytes = enzan::eir::encodeModules({}, sources);
    ASSERT_GT(bytes.size(), 12U);
    ++bytes[8]; // the low byte of the version

    std::vector<ModuleDefinition> decoded;
    EXPECT_FALSE(enzan::eir::decodeModules(bytes, resolver(), decoded));
}

TEST_F(SerializationTest, RefusesATruncatedLibrary) {
    const std::vector<ModuleDefinition> modules{richModule(buffer)};
    const std::string bytes = enzan::eir::encodeModules(modules, sources);

    for (const std::size_t keep : {bytes.size() / 4, bytes.size() / 2, bytes.size() - 1}) {
        std::vector<ModuleDefinition> decoded;
        EXPECT_FALSE(enzan::eir::decodeModules(bytes.substr(0, keep), resolver(), decoded))
            << "kept " << keep << " of " << bytes.size();
        EXPECT_TRUE(decoded.empty());
    }
}

TEST_F(SerializationTest, RefusesACorruptedLibrary) {
    const std::vector<ModuleDefinition> modules{richModule(buffer)};
    const std::string original = enzan::eir::encodeModules(modules, sources);

    // Every byte of the payload, so that no field is left where a flipped bit passes.
    for (std::size_t index = 28; index < original.size(); index += 37) {
        std::string bytes = original;
        bytes[index] = static_cast<char>(bytes[index] ^ 0x5A);

        std::vector<ModuleDefinition> decoded;
        EXPECT_FALSE(enzan::eir::decodeModules(bytes, resolver(), decoded))
            << "byte " << index << " survived corruption";
    }
}

TEST_F(SerializationTest, RefusesTrailingBytes) {
    std::string bytes = enzan::eir::encodeModules({}, sources);
    bytes += "extra";

    std::vector<ModuleDefinition> decoded;
    EXPECT_FALSE(enzan::eir::decodeModules(bytes, resolver(), decoded));
}

} // namespace
