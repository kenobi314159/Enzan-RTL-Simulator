// SPDX-License-Identifier: GPL-3.0-or-later
//
// VPI as an application sees it: the routines of vpi_user.h, called on a design built by hand
// and run by a real kernel. The test is the application here, calling the routines exactly as
// a loaded library would.

#include "pli/PliContext.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"

#include "support/Literals.h"
#include "support/ProcessBuilder.h"
#include "support/Simulation.h"

#include "sv_vpi_user.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using enzan::eir::Design;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::ObjectKind;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::pli::PliContext;
using enzan::runtime::RunStatus;
using enzan::test::literal;
using enzan::test::ProcessBuilder;
using enzan::test::Simulation;

/// `tb`, with a clock toggling every five units, one object of each kind VPI shows, a
/// generate block two deep, an object a frontend made up, a child whose port is collapsed
/// onto `data`, and a second child inside the generate block.
Design makeDesign() {
    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "in", .type = Type::bits(8, false, true)});
    child.ports.push_back(enzan::eir::Port{
        .name = "in", .direction = enzan::eir::PortDirection::Input, .objectIndex = 0});

    ProcessBuilder clock(ProcessKind::Always, "clockgen");
    const std::uint32_t delay = clock.reg(Type::bits(32, false, false));
    const std::uint32_t value = clock.reg(Type::logic());
    const std::uint32_t inverted = clock.reg(Type::logic());
    const std::uint32_t five = clock.constant("32'd5");
    clock.block("entry");
    clock.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(five)));
    clock.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    clock.block("toggle");
    clock.readObject(value, 0);
    clock.emit(Instruction::make(Opcode::Not, Operand::reg(inverted), Operand::reg(value)));
    clock.store(0, inverted);
    clock.emit(Instruction::make(Opcode::Jump, {}, Operand::block(0)));

    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(Object{.name = "clk", .type = Type::logic(), .initialValue = literal("1'b0")});
    top.addObject(Object{
        .name = "data", .type = Type::bits(8, false, true), .initialValue = literal("8'h5A")});
    top.addObject(Object{.name = "flag", .type = Type::bit()});
    top.addObject(
        Object{.name = "bus", .type = Type::bits(8, false, true), .kind = ObjectKind::Net});
    top.addObject(Object{.name = "gen[0].q", .type = Type::logic()});
    top.addObject(Object{.name = "gen[0].inner.r", .type = Type::bits(4, true, true)});
    top.addObject(Object{.name = "u_child$in", .type = Type::bits(8, false, true)});
    // `logic signed [7:0] mem [3:0]`, stored flat with the left element, mem[3], highest.
    Object memory{.name = "mem",
                  .type = Type::bits(32, false, true),
                  .initialValue = literal("32'h33221100")};
    memory.unpackedDimensions.push_back(enzan::eir::ArrayDimension{.left = 3, .right = 0});
    memory.elementWidth = 8;
    memory.elementSigned = true;
    top.addObject(std::move(memory));
    // `logic [3:0] grid [0:1][2:0]`: grid[0][2] highest, then grid[0][1], ... grid[1][0].
    Object grid{
        .name = "grid", .type = Type::bits(24, false, true), .initialValue = literal("24'h012345")};
    grid.unpackedDimensions.push_back(enzan::eir::ArrayDimension{.left = 0, .right = 1});
    grid.unpackedDimensions.push_back(enzan::eir::ArrayDimension{.left = 2, .right = 0});
    grid.elementWidth = 4;
    top.addObject(std::move(grid));
    // A package's variable, so that the package has an instance at the root beside `tb`.
    top.addObject(Object{.name = "ref$/config/mode",
                         .type = Type::bits(2, false, true),
                         .hierarchicalPath = "/config/mode"});
    top.parameters.push_back(
        enzan::eir::Parameter{.name = "WIDTH", .value = literal("32'd8"), .isLocal = false});
    top.parameters.push_back(
        enzan::eir::Parameter{.name = "DEPTH", .value = literal("32'd4"), .isLocal = true});
    top.parameters.push_back(
        enzan::eir::Parameter{.name = "gen[0].P", .value = literal("32'd1"), .isLocal = true});
    top.processes.push_back(clock.finish());

    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 1});
    top.instances.push_back(std::move(instance));

    enzan::eir::Instance generated;
    generated.name = "gen[0].u_gen";
    generated.definitionName = "child";
    generated.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 1});
    top.instances.push_back(std::move(generated));

    ModuleDefinition package;
    package.name = "config::";
    package.sourceName = "config";
    package.isPackage = true;
    package.addObject(Object{.name = "mode", .type = Type::bits(2, false, true)});

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(top));
    design.addModule(std::move(package));
    return design;
}

class Vpi : public ::testing::Test {
protected:
    Vpi()
        : simulation(makeDesign(), "tb"), engine(sources), context(engine, {"enzan", "+seed=3"}) {}

    void SetUp() override {
        ASSERT_TRUE(simulation.ready());
        context.attach(simulation.designImage(), simulation.simulationKernel());
    }

    static vpiHandle byName(const char* name, vpiHandle scope = nullptr) {
        return vpi_handle_by_name(const_cast<PLI_BYTE8*>(name), scope);
    }

    static std::string nameOf(vpiHandle handle, PLI_INT32 property = vpiFullName) {
        const char* text = vpi_get_str(property, handle);
        return text != nullptr ? text : "<null>";
    }

    static std::string hexOf(vpiHandle handle) {
        s_vpi_value value{};
        value.format = vpiHexStrVal;
        vpi_get_value(handle, &value);
        return value.value.str != nullptr ? value.value.str : "<null>";
    }

    static void write(vpiHandle handle, PLI_INT32 integer, PLI_INT32 flags, PLI_UINT32 delay = 0) {
        s_vpi_value value{};
        value.format = vpiIntVal;
        value.value.integer = integer;
        s_vpi_time time{};
        time.type = vpiSimTime;
        time.low = delay;
        vpi_put_value(handle, &value, &time, flags);
    }

    /// The full names an iteration yields, in order.
    static std::vector<std::string> names(PLI_INT32 type, vpiHandle reference) {
        std::vector<std::string> found;
        vpiHandle iterator = vpi_iterate(type, reference);
        if (iterator == nullptr) return found;
        while (vpiHandle item = vpi_scan(iterator)) found.push_back(nameOf(item));
        return found;
    }

    static PLI_INT32 lastErrorLevel() { return vpi_chk_error(nullptr); }

    Simulation simulation;
    enzan::SourceManager sources;
    enzan::DiagnosticEngine engine;
    PliContext context;
};

// ---------------------------------------------------------------------------
// The object model
// ---------------------------------------------------------------------------

TEST_F(Vpi, TheTopLevelIsFoundFromTheRoot) {
    vpiHandle iterator = vpi_iterate(vpiModule, nullptr);
    ASSERT_NE(iterator, nullptr);

    vpiHandle top = vpi_scan(iterator);
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(nameOf(top), "tb");
    EXPECT_EQ(vpi_get(vpiType, top), vpiModule);
    EXPECT_EQ(vpi_get(vpiTopModule, top), 1);
    EXPECT_EQ(nameOf(top, vpiDefName), "tb");

    // The package `config` is at the root too, but it is no top-level module.
    EXPECT_EQ(vpi_scan(iterator), nullptr);
    EXPECT_EQ(vpi_free_object(iterator), 0) << "the scan that exhausted it freed it";
    EXPECT_EQ(lastErrorLevel(), vpiError);
}

TEST(VpiTopLevels, EveryTopLevelIsFoundFromTheRoot) {
    // A design and AMD's `glbl`, which are two top levels, as other simulators show them.
    ModuleDefinition tb;
    tb.name = "tb";
    tb.sourceName = "tb";
    ModuleDefinition glbl;
    glbl.name = "glbl";
    glbl.sourceName = "glbl";
    Design design;
    design.addModule(std::move(tb));
    design.addModule(std::move(glbl));

    Simulation simulation(std::move(design), std::vector<std::string>{"tb", "glbl"});
    ASSERT_TRUE(simulation.ready());
    enzan::SourceManager sources;
    enzan::DiagnosticEngine engine(sources);
    PliContext context(engine, {"enzan"});
    context.attach(simulation.designImage(), simulation.simulationKernel());

    vpiHandle iterator = vpi_iterate(vpiModule, nullptr);
    ASSERT_NE(iterator, nullptr);
    std::vector<std::string> names;
    while (vpiHandle top = vpi_scan(iterator)) {
        names.push_back(vpi_get_str(vpiName, top));
        EXPECT_EQ(vpi_get(vpiTopModule, top), 1);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"tb", "glbl"}));
    context.detach();
}

TEST_F(Vpi, NamesAreDottedFromTheTop) {
    vpiHandle data = byName("tb.data");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(nameOf(data), "tb.data");
    EXPECT_EQ(nameOf(data, vpiName), "data");

    vpiHandle child = byName("tb.u_child");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(byName("in", child), byName("tb.u_child.in")) << "a name relative to a scope";
}

TEST_F(Vpi, GenerateBlocksAreScopesOfTheirOwn) {
    vpiHandle r = byName("tb.gen[0].inner.r");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(nameOf(r, vpiName), "r");

    vpiHandle inner = vpi_handle(vpiScope, r);
    EXPECT_EQ(nameOf(inner), "tb.gen[0].inner");
    EXPECT_EQ(vpi_get(vpiType, inner), vpiGenScope);
    EXPECT_EQ(nameOf(vpi_handle(vpiModule, r)), "tb") << "through every generate block";

    EXPECT_EQ(names(vpiInternalScope, byName("tb")),
              (std::vector<std::string>{"tb.gen[0]", "tb.u_child"}));
    EXPECT_EQ(names(vpiParameter, byName("tb.gen[0]")), std::vector<std::string>{"tb.gen[0].P"});
}

TEST_F(Vpi, AnInstanceInAGenerateBlockIsInsideIt) {
    vpiHandle instance = byName("tb.gen[0].u_gen");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(vpi_get(vpiType, instance), vpiModule);
    EXPECT_EQ(nameOf(instance, vpiName), "u_gen");
    EXPECT_EQ(vpi_handle(vpiScope, instance), byName("tb.gen[0]"));
    EXPECT_EQ(vpi_handle(vpiModule, instance), byName("tb")) << "through the generate block";
    EXPECT_EQ(names(vpiModule, byName("tb.gen[0]")), std::vector<std::string>{"tb.gen[0].u_gen"});
    EXPECT_EQ(byName("in", instance), byName("tb.gen[0].u_gen.in"));
}

TEST_F(Vpi, HandlesToTheDesignAreInterned) {
    EXPECT_EQ(byName("tb.clk"), byName("tb.clk"));
    EXPECT_EQ(vpi_free_object(byName("tb.clk")), 1);
    EXPECT_NE(vpi_get(vpiType, byName("tb.clk")), vpiUndefined) << "freeing it changed nothing";
}

TEST_F(Vpi, ObjectsAreTypedAsSection92Says) {
    EXPECT_EQ(vpi_get(vpiType, byName("tb.clk")), vpiReg);
    EXPECT_EQ(vpi_get(vpiType, byName("tb.flag")), vpiBitVar);
    EXPECT_EQ(vpi_get(vpiType, byName("tb.bus")), vpiNet);
    EXPECT_EQ(vpi_get(vpiNetType, byName("tb.bus")), vpiWire);
    EXPECT_EQ(vpi_get(vpiType, byName("tb.WIDTH")), vpiParameter);
    EXPECT_EQ(vpi_get(vpiLocalParam, byName("tb.WIDTH")), 0);
    EXPECT_EQ(vpi_get(vpiLocalParam, byName("tb.DEPTH")), 1);
    EXPECT_EQ(nameOf(byName("tb.clk"), vpiType), "vpiReg");
}

TEST_F(Vpi, WhatAFrontendMadeUpIsNotVisible) {
    EXPECT_EQ(byName("tb.u_child$in"), nullptr);
    EXPECT_EQ(names(vpiReg, byName("tb")), (std::vector<std::string>{"tb.clk", "tb.data"}));
}

TEST_F(Vpi, IterationSelectsByKind) {
    vpiHandle top = byName("tb");
    EXPECT_EQ(names(vpiNet, top), std::vector<std::string>{"tb.bus"});
    EXPECT_EQ(names(vpiVariables, top),
              (std::vector<std::string>{"tb.clk", "tb.data", "tb.flag", "tb.mem", "tb.grid"}));
    EXPECT_EQ(names(vpiRegArray, top), (std::vector<std::string>{"tb.mem", "tb.grid"}));
    EXPECT_EQ(names(vpiParameter, top), (std::vector<std::string>{"tb.WIDTH", "tb.DEPTH"}));
    EXPECT_EQ(names(vpiModule, top), std::vector<std::string>{"tb.u_child"});
    EXPECT_EQ(vpi_iterate(vpiNetArray, top), nullptr) << "nothing to iterate is no iterator";
}

TEST_F(Vpi, AVectorDescribesItselfAndItsRange) {
    vpiHandle data = byName("tb.data");
    EXPECT_EQ(vpi_get(vpiSize, data), 8);
    EXPECT_EQ(vpi_get(vpiVector, data), 1);
    EXPECT_EQ(vpi_get(vpiSigned, data), 0);
    EXPECT_EQ(vpi_get(vpiSigned, byName("tb.gen[0].inner.r")), 1);

    s_vpi_value bound{};
    bound.format = vpiIntVal;
    vpi_get_value(vpi_handle(vpiLeftRange, data), &bound);
    EXPECT_EQ(bound.value.integer, 7);
    vpi_get_value(vpi_handle(vpiRightRange, data), &bound);
    EXPECT_EQ(bound.value.integer, 0);

    EXPECT_EQ(vpi_handle(vpiLeftRange, byName("tb.clk")), nullptr) << "a scalar has no range";
}

TEST_F(Vpi, ACollapsedPortIsOneSignalUnderTwoNames) {
    write(byName("tb.u_child.in"), 0x33, vpiNoDelay);
    EXPECT_EQ(hexOf(byName("tb.data")), "33");
}

TEST_F(Vpi, AParameterHasAValueAndCannotBeWritten) {
    EXPECT_EQ(hexOf(byName("tb.WIDTH")), "00000008");
    write(byName("tb.WIDTH"), 9, vpiNoDelay);
    EXPECT_EQ(lastErrorLevel(), vpiError);
    EXPECT_EQ(hexOf(byName("tb.WIDTH")), "00000008");
}

TEST_F(Vpi, ABitOfAVectorCanBeSelectedReadAndWritten) {
    vpiHandle data = byName("tb.data");
    vpiHandle bit = vpi_handle_by_index(data, 1);
    ASSERT_NE(bit, nullptr);
    EXPECT_EQ(nameOf(bit), "tb.data[1]");
    EXPECT_EQ(nameOf(bit, vpiName), "data[1]");
    EXPECT_EQ(vpi_get(vpiType, bit), vpiRegBit);
    EXPECT_EQ(vpi_get(vpiSize, bit), 1);
    EXPECT_EQ(vpi_handle(vpiParent, bit), data);
    EXPECT_EQ(vpi_handle_by_index(data, 1), bit) << "interned like any design handle";

    s_vpi_value index{};
    index.format = vpiIntVal;
    vpi_get_value(vpi_handle(vpiIndex, bit), &index);
    EXPECT_EQ(index.value.integer, 1);

    EXPECT_EQ(hexOf(bit), "1") << "8'h5A has bit 1 set";
    write(bit, 0, vpiNoDelay);
    EXPECT_EQ(hexOf(data), "58") << "and only that bit changed";
}

TEST_F(Vpi, ANetsBitIsANetBit) {
    EXPECT_EQ(vpi_get(vpiType, vpi_handle_by_index(byName("tb.bus"), 0)), vpiNetBit);
}

TEST_F(Vpi, AnIndexOutsideTheRangeOrOfAScalarSelectsNothing) {
    EXPECT_EQ(vpi_handle_by_index(byName("tb.data"), 8), nullptr);
    EXPECT_EQ(lastErrorLevel(), vpiError);
    EXPECT_EQ(vpi_handle_by_index(byName("tb.data"), -1), nullptr);
    EXPECT_EQ(vpi_handle_by_index(byName("tb.clk"), 0), nullptr);
}

TEST_F(Vpi, TheBitsOfAVectorIterateLeftToRight) {
    const std::vector<std::string> bits = names(vpiBit, byName("tb.data"));
    ASSERT_EQ(bits.size(), 8U);
    EXPECT_EQ(bits.front(), "tb.data[7]");
    EXPECT_EQ(bits.back(), "tb.data[0]");
}

TEST_F(Vpi, AnArrayDescribesItselfAndHasElementsRatherThanAValue) {
    vpiHandle memory = byName("tb.mem");
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(vpi_get(vpiType, memory), vpiRegArray);
    EXPECT_EQ(vpi_get(vpiSize, memory), 4) << "an array's size counts its elements";
    EXPECT_EQ(vpi_get(vpiArray, memory), 1);

    s_vpi_value bound{};
    bound.format = vpiIntVal;
    vpi_get_value(vpi_handle(vpiLeftRange, memory), &bound);
    EXPECT_EQ(bound.value.integer, 3);
    vpi_get_value(vpi_handle(vpiRightRange, memory), &bound);
    EXPECT_EQ(bound.value.integer, 0);

    s_vpi_value value{};
    value.format = vpiHexStrVal;
    vpi_get_value(memory, &value);
    EXPECT_EQ(lastErrorLevel(), vpiError);
}

TEST_F(Vpi, AnElementOfAnArrayIsSelectedByItsDeclaredIndex) {
    vpiHandle memory = byName("tb.mem");
    vpiHandle element = vpi_handle_by_index(memory, 2);
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(nameOf(element), "tb.mem[2]");
    EXPECT_EQ(vpi_get(vpiType, element), vpiReg);
    EXPECT_EQ(vpi_get(vpiSize, element), 8);
    EXPECT_EQ(vpi_handle(vpiParent, element), memory);
    EXPECT_EQ(hexOf(element), "22");

    s_vpi_value index{};
    index.format = vpiIntVal;
    vpi_get_value(vpi_handle(vpiIndex, element), &index);
    EXPECT_EQ(index.value.integer, 2);

    // Writing it changes that element alone.
    write(element, 0xAB, vpiNoDelay);
    EXPECT_EQ(hexOf(vpi_handle_by_index(memory, 3)), "33");
    EXPECT_EQ(hexOf(element), "ab");
    EXPECT_EQ(hexOf(vpi_handle_by_index(memory, 1)), "11");

    EXPECT_EQ(vpi_handle_by_index(memory, 4), nullptr);
    EXPECT_EQ(lastErrorLevel(), vpiError);
}

TEST_F(Vpi, AnElementOfAnArrayIsAsSignedAsItsDeclaredType) {
    // The array is stored as one unsigned vector, so its element type is what says this.
    vpiHandle element = vpi_handle_by_index(byName("tb.mem"), 1);
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(vpi_get(vpiSigned, element), 1);

    write(element, -2, vpiNoDelay);
    s_vpi_value value{};
    value.format = vpiIntVal;
    vpi_get_value(element, &value);
    EXPECT_EQ(value.value.integer, -2);
}

TEST_F(Vpi, AnArrayOfTwoDimensionsIsAnArrayOfArrays) {
    vpiHandle grid = byName("tb.grid");
    EXPECT_EQ(vpi_get(vpiType, grid), vpiRegArray);
    EXPECT_EQ(vpi_get(vpiMultiArray, grid), 1);
    EXPECT_EQ(vpi_get(vpiSize, grid), 2);
    EXPECT_EQ(names(vpiRegArray, grid), (std::vector<std::string>{"tb.grid[0]", "tb.grid[1]"}));

    vpiHandle row = vpi_handle_by_index(grid, 1);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(vpi_get(vpiType, row), vpiRegArray);
    EXPECT_EQ(vpi_get(vpiSize, row), 3);
    EXPECT_EQ(vpi_handle(vpiParent, row), grid);
    EXPECT_EQ(names(vpiReg, row),
              (std::vector<std::string>{"tb.grid[1][2]", "tb.grid[1][1]", "tb.grid[1][0]"}));

    vpiHandle cell = vpi_handle_by_index(row, 0);
    ASSERT_NE(cell, nullptr);
    EXPECT_EQ(vpi_get(vpiType, cell), vpiReg);
    EXPECT_EQ(vpi_handle(vpiParent, cell), row);
    EXPECT_EQ(hexOf(cell), "5");
    EXPECT_EQ(hexOf(vpi_handle_by_index(vpi_handle_by_index(grid, 0), 2)), "0");

    write(cell, 0xC, vpiNoDelay);
    EXPECT_EQ(hexOf(cell), "c");
    EXPECT_EQ(hexOf(vpi_handle_by_index(row, 1)), "4") << "and only that cell changed";
}

TEST_F(Vpi, TheElementsOfAnArrayIterateLeftToRight) {
    EXPECT_EQ(names(vpiReg, byName("tb.mem")),
              (std::vector<std::string>{"tb.mem[3]", "tb.mem[2]", "tb.mem[1]", "tb.mem[0]"}));
}

TEST_F(Vpi, AnInertialWriteToABitLandsInTheNbaRegion) {
    write(vpi_handle_by_index(byName("tb.data"), 0), 1, vpiInertialDelay);
    EXPECT_EQ(hexOf(byName("tb.data")), "5a");
    simulation.run(0);
    EXPECT_EQ(hexOf(byName("tb.data")), "5b");
}

TEST_F(Vpi, ABitCannotBeForcedOnItsOwn) {
    write(vpi_handle_by_index(byName("tb.data"), 0), 1, vpiForceFlag);
    EXPECT_EQ(lastErrorLevel(), vpiError);
    EXPECT_EQ(hexOf(byName("tb.data")), "5a");
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

TEST_F(Vpi, ANoDelayWriteLandsNow) {
    EXPECT_EQ(hexOf(byName("tb.data")), "5a");
    write(byName("tb.data"), 0x21, vpiNoDelay);
    EXPECT_EQ(hexOf(byName("tb.data")), "21");
}

TEST_F(Vpi, AnInertialWriteWithNoDelayLandsInTheNbaRegion) {
    write(byName("tb.data"), 0x21, vpiInertialDelay);
    EXPECT_EQ(hexOf(byName("tb.data")), "5a") << "not yet: it is a non-blocking update";

    simulation.run(0);
    EXPECT_EQ(hexOf(byName("tb.data")), "21");
}

TEST_F(Vpi, ADelayedWriteLandsAtItsTime) {
    write(byName("tb.data"), 0x21, vpiTransportDelay, 7);
    ASSERT_EQ(simulation.run(6), RunStatus::TimeLimitReached);
    EXPECT_EQ(hexOf(byName("tb.data")), "5a");
    ASSERT_EQ(simulation.run(7), RunStatus::TimeLimitReached);
    EXPECT_EQ(hexOf(byName("tb.data")), "21");
}

TEST_F(Vpi, AForceHoldsAgainstTheDesignUntilReleased) {
    vpiHandle clk = byName("tb.clk");
    write(clk, 1, vpiForceFlag);
    simulation.run(20);
    EXPECT_EQ(hexOf(clk), "1") << "the clock process could not move it";

    s_vpi_value kept{};
    kept.format = vpiBinStrVal;
    vpi_put_value(clk, &kept, nullptr, vpiReleaseFlag);
    EXPECT_STREQ(kept.value.str, "1") << "release reports the value the signal keeps";

    simulation.run(25);
    EXPECT_EQ(hexOf(clk), "0") << "driven again";
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

/// What a test's callbacks write down.
struct Record {
    std::vector<std::string> events;
    vpiHandle handle = nullptr;
};

Record& recordOf(p_cb_data data) {
    return *reinterpret_cast<Record*>(data->user_data);
}

vpiHandle registerCallback(PLI_INT32 reason, PLI_INT32 (*routine)(p_cb_data), Record& record,
                           vpiHandle object = nullptr, PLI_UINT32 delay = 0,
                           PLI_INT32 valueFormat = vpiSuppressVal) {
    s_vpi_time time{};
    time.type = vpiSimTime;
    time.low = delay;
    s_vpi_value value{};
    value.format = valueFormat;

    s_cb_data data{};
    data.reason = reason;
    data.cb_rtn = routine;
    data.obj = object;
    data.time = &time;
    data.value = &value;
    data.user_data = reinterpret_cast<PLI_BYTE8*>(&record);
    return vpi_register_cb(&data);
}

std::string at(p_cb_data data, const std::string& what) {
    return std::to_string(data->time->low) + " " + what;
}

TEST_F(Vpi, AValueChangeReportsTheNewValueAndTime) {
    Record record;
    record.handle = registerCallback(
        cbValueChange,
        [](p_cb_data data) -> PLI_INT32 {
            recordOf(data).events.push_back(at(data, data->value->value.str));
            return 0;
        },
        record, byName("tb.clk"), 0, vpiBinStrVal);
    ASSERT_NE(record.handle, nullptr);

    simulation.run(20);
    EXPECT_EQ(record.events, (std::vector<std::string>{"5 1", "10 0", "15 1", "20 0"}));

    EXPECT_EQ(vpi_remove_cb(record.handle), 1);
    simulation.run(40);
    EXPECT_EQ(record.events.size(), 4U) << "removed, so it fired no more";
}

TEST_F(Vpi, AValueChangeOnABitFiresOnlyWhenThatBitMoves) {
    // The kernel reports changes of whole signals; a bit's watcher must not hear about a
    // change somewhere else in its vector, or RisingEdge(dut.bus[0]) fires on every count.
    Record record;
    registerCallback(
        cbValueChange,
        [](p_cb_data data) -> PLI_INT32 {
            recordOf(data).events.emplace_back(data->value->value.str);
            return 0;
        },
        record, vpi_handle_by_index(byName("tb.data"), 0), 0, vpiBinStrVal);

    write(byName("tb.data"), 0x5B, vpiNoDelay);
    write(byName("tb.data"), 0x7B, vpiNoDelay);
    write(byName("tb.data"), 0x7A, vpiNoDelay);
    EXPECT_EQ(record.events, (std::vector<std::string>{"1", "0"}));
}

TEST_F(Vpi, AValueChangeOnAnElementFiresOnlyWhenThatElementMoves) {
    Record record;
    registerCallback(
        cbValueChange,
        [](p_cb_data data) -> PLI_INT32 {
            recordOf(data).events.emplace_back(data->value->value.str);
            return 0;
        },
        record, vpi_handle_by_index(byName("tb.mem"), 0), 0, vpiHexStrVal);

    write(vpi_handle_by_index(byName("tb.mem"), 1), 0x99, vpiNoDelay);
    write(vpi_handle_by_index(byName("tb.mem"), 0), 0x42, vpiNoDelay);
    EXPECT_EQ(record.events, std::vector<std::string>{"42"});
}

TEST_F(Vpi, ACallbackMayRemoveItselfWhileItRuns) {
    Record record;
    record.handle = registerCallback(
        cbValueChange,
        [](p_cb_data data) -> PLI_INT32 {
            Record& self = recordOf(data);
            self.events.emplace_back("changed");
            vpi_remove_cb(self.handle);
            return 0;
        },
        record, byName("tb.clk"));

    simulation.run(30);
    EXPECT_EQ(record.events.size(), 1U);
}

TEST_F(Vpi, ACallbackMayRemoveAnotherBeingDeliveredTheSameChange) {
    // The second is in the list being walked when the first removes it; it must be skipped
    // rather than fired, and freed only once the walk is over.
    Record record;
    registerCallback(
        cbValueChange,
        [](p_cb_data data) -> PLI_INT32 {
            Record& self = recordOf(data);
            self.events.emplace_back("first");
            if (self.handle != nullptr) vpi_remove_cb(self.handle);
            self.handle = nullptr;
            return 0;
        },
        record, byName("tb.clk"));
    record.handle = registerCallback(
        cbValueChange,
        [](p_cb_data data) -> PLI_INT32 {
            recordOf(data).events.emplace_back("second");
            return 0;
        },
        record, byName("tb.clk"));

    simulation.run(10);
    EXPECT_EQ(record.events, (std::vector<std::string>{"first", "first"}));
}

TEST_F(Vpi, AValueChangeRegisteredDuringAChangeWaitsForTheNext) {
    Record record;
    registerCallback(
        cbValueChange,
        [](p_cb_data data) -> PLI_INT32 {
            Record& self = recordOf(data);
            self.events.emplace_back("outer");
            registerCallback(
                cbValueChange,
                [](p_cb_data inner) -> PLI_INT32 {
                    recordOf(inner).events.emplace_back("inner");
                    return 0;
                },
                self, byName("tb.data"));
            return 0;
        },
        record, byName("tb.data"));

    write(byName("tb.data"), 0x01, vpiNoDelay);
    write(byName("tb.data"), 0x02, vpiNoDelay);
    EXPECT_EQ(record.events, (std::vector<std::string>{"outer", "outer", "inner"}));
}

TEST_F(Vpi, ReadOnlyRegisteredFromReadOnlyRunsInTheNextSlot) {
    Record record;
    registerCallback(
        cbReadOnlySynch,
        [](p_cb_data data) -> PLI_INT32 {
            Record& self = recordOf(data);
            self.events.push_back(at(data, "first"));
            registerCallback(
                cbReadOnlySynch,
                [](p_cb_data inner) -> PLI_INT32 {
                    recordOf(inner).events.push_back(at(inner, "second"));
                    return 0;
                },
                self);
            return 0;
        },
        record);

    simulation.run(12);
    EXPECT_EQ(record.events, (std::vector<std::string>{"0 first", "5 second"}));
}

TEST_F(Vpi, ADelayedWriteLandsBeforeTheSlotsAfterDelay) {
    write(byName("tb.data"), 0x21, vpiTransportDelay, 7);

    Record record;
    registerCallback(
        cbAfterDelay,
        [](p_cb_data data) -> PLI_INT32 {
            recordOf(data).events.emplace_back(hexOf(byName("tb.data")));
            return 0;
        },
        record, nullptr, 7);

    simulation.run(10);
    EXPECT_EQ(record.events, std::vector<std::string>{"21"});
}

TEST_F(Vpi, AStartOfSimTimeAlreadyPastIsRefused) {
    ASSERT_EQ(simulation.run(10), RunStatus::TimeLimitReached);

    Record record;
    EXPECT_EQ(registerCallback(
                  cbAtStartOfSimTime, [](p_cb_data /*data*/) -> PLI_INT32 { return 0; }, record,
                  nullptr, 3),
              nullptr);
    EXPECT_EQ(lastErrorLevel(), vpiError);
}

TEST_F(Vpi, AfterDelayFiresAtItsTime) {
    Record record;
    registerCallback(
        cbAfterDelay,
        [](p_cb_data data) -> PLI_INT32 {
            s_vpi_time now{};
            now.type = vpiSimTime;
            vpi_get_time(nullptr, &now);
            recordOf(data).events.push_back(std::to_string(now.low));
            return 0;
        },
        record, nullptr, 12);

    simulation.run(30);
    EXPECT_EQ(record.events, std::vector<std::string>{"12"});
}

TEST_F(Vpi, ReadWriteWritesAreSettledByReadOnlyWhichMayNotWrite) {
    Record record;
    registerCallback(
        cbReadWriteSynch,
        [](p_cb_data /*data*/) -> PLI_INT32 {
            write(byName("tb.data"), 0x44, vpiInertialDelay);
            return 0;
        },
        record);
    registerCallback(
        cbReadOnlySynch,
        [](p_cb_data data) -> PLI_INT32 {
            recordOf(data).events.emplace_back(hexOf(byName("tb.data")));
            write(byName("tb.data"), 0x55, vpiNoDelay);
            recordOf(data).events.emplace_back(lastErrorLevel() == vpiError ? "refused"
                                                                            : "written");
            return 0;
        },
        record);

    simulation.run(0);
    EXPECT_EQ(record.events, (std::vector<std::string>{"44", "refused"}));
    EXPECT_EQ(hexOf(byName("tb.data")), "44");
}

TEST_F(Vpi, ReadWriteRegisteredFromReadWriteRunsInTheNextPass) {
    Record record;
    registerCallback(
        cbReadWriteSynch,
        [](p_cb_data data) -> PLI_INT32 {
            Record& self = recordOf(data);
            self.events.emplace_back("first");
            registerCallback(
                cbReadWriteSynch,
                [](p_cb_data inner) -> PLI_INT32 {
                    recordOf(inner).events.emplace_back("second");
                    return 0;
                },
                self);
            self.events.emplace_back("first returned");
            return 0;
        },
        record);

    simulation.run(0);
    EXPECT_EQ(record.events, (std::vector<std::string>{"first", "first returned", "second"}));
}

TEST_F(Vpi, NextSimTimeFiresWhenTimeMoves) {
    Record record;
    registerCallback(
        cbNextSimTime,
        [](p_cb_data data) -> PLI_INT32 {
            recordOf(data).events.push_back(at(data, "next"));
            return 0;
        },
        record);

    simulation.run(12);
    EXPECT_EQ(record.events, std::vector<std::string>{"5 next"}) << "once, at the first advance";
}

TEST_F(Vpi, StartAndEndOfSimulationFireOnce) {
    Record record;
    const auto routine = [](p_cb_data data) -> PLI_INT32 {
        recordOf(data).events.emplace_back(data->reason == cbStartOfSimulation ? "start" : "end");
        return 0;
    };
    registerCallback(cbStartOfSimulation, routine, record);
    registerCallback(cbEndOfSimulation, routine, record);

    context.startOfSimulation();
    context.startOfSimulation();
    context.endOfSimulation();
    context.endOfSimulation();
    EXPECT_EQ(record.events, (std::vector<std::string>{"start", "end"}));
}

TEST_F(Vpi, FinishFromACallbackEndsTheRun) {
    Record record;
    registerCallback(
        cbAfterDelay,
        [](p_cb_data /*data*/) -> PLI_INT32 {
            vpi_control(vpiFinish, 1);
            return 0;
        },
        record, nullptr, 12);

    EXPECT_EQ(simulation.run(100), RunStatus::Finished);
    EXPECT_EQ(simulation.time(), 12U);
}

TEST_F(Vpi, AFinishBeforeThereIsADesignEndsTheFirstRunOnly) {
    context.detach();
    vpi_control(vpiFinish, 1);

    context.attach(simulation.designImage(), simulation.simulationKernel());
    EXPECT_EQ(simulation.run(100), RunStatus::Finished);
    EXPECT_EQ(simulation.time(), 0U);

    context.detach();
    simulation.simulationKernel().initialize();
    context.attach(simulation.designImage(), simulation.simulationKernel());
    EXPECT_EQ(simulation.run(100), RunStatus::TimeLimitReached);
}

TEST_F(Vpi, AValueChangeOnAParameterIsRefused) {
    Record record;
    EXPECT_EQ(registerCallback(
                  cbValueChange, [](p_cb_data /*data*/) -> PLI_INT32 { return 0; }, record,
                  byName("tb.WIDTH")),
              nullptr);
    EXPECT_EQ(lastErrorLevel(), vpiError);
}

TEST_F(Vpi, AnUnsupportedReasonIsRefused) {
    Record record;
    EXPECT_EQ(registerCallback(
                  cbStmt, [](p_cb_data /*data*/) -> PLI_INT32 { return 0; }, record),
              nullptr);
    EXPECT_EQ(lastErrorLevel(), vpiError);
}

// ---------------------------------------------------------------------------
// The simulator, and errors
// ---------------------------------------------------------------------------

TEST_F(Vpi, TheCommandLineAndProductAreReported) {
    s_vpi_vlog_info info{};
    ASSERT_EQ(vpi_get_vlog_info(&info), 1);
    EXPECT_STREQ(info.product, "Enzan");
    ASSERT_EQ(info.argc, 2);
    EXPECT_STREQ(info.argv[1], "+seed=3") << "where an application finds its plusargs";
}

TEST_F(Vpi, TimeIsCountedInTheKernelsPrecision) {
    EXPECT_EQ(vpi_get(vpiTimePrecision, nullptr),
              simulation.simulationKernel().timeScale().precisionExponent());
}

TEST_F(Vpi, AHandleNeverIssuedIsAnErrorRatherThanACrash) {
    std::uint32_t stray = 0;
    EXPECT_EQ(vpi_get(vpiType, &stray), vpiUndefined);

    s_vpi_error_info info{};
    EXPECT_EQ(vpi_chk_error(&info), vpiError);
    EXPECT_NE(std::string(info.message).find("handle"), std::string::npos);
}

TEST_F(Vpi, ASuccessfulCallClearsTheLastError) {
    std::uint32_t stray = 0;
    static_cast<void>(vpi_get(vpiType, &stray));
    ASSERT_EQ(lastErrorLevel(), vpiError);

    static_cast<void>(vpi_get(vpiType, byName("tb.clk")));
    EXPECT_EQ(lastErrorLevel(), 0);
}

TEST_F(Vpi, AnUnsupportedRoutineSaysWhichItIs) {
    s_vpi_systf_data task{};
    EXPECT_EQ(vpi_register_systf(&task), nullptr);

    s_vpi_error_info info{};
    ASSERT_EQ(vpi_chk_error(&info), vpiError);
    EXPECT_NE(std::string(info.message).find("vpi_register_systf"), std::string::npos);
}

TEST_F(Vpi, DetachingMakesEveryDesignHandleStale) {
    vpiHandle clk = byName("tb.clk");
    context.detach();
    EXPECT_EQ(vpi_get(vpiType, clk), vpiUndefined);
    EXPECT_EQ(lastErrorLevel(), vpiError);
}

TEST(VpiWithoutAContext, EveryRoutineFailsQuietly) {
    // What an application's own clean-up meets when it runs after the session has gone.
    EXPECT_EQ(PliContext::current(), nullptr);
    EXPECT_EQ(vpi_handle_by_name(const_cast<PLI_BYTE8*>("tb"), nullptr), nullptr);
    EXPECT_EQ(vpi_get(vpiType, nullptr), vpiUndefined);
    EXPECT_EQ(vpi_chk_error(nullptr), 0);
}

} // namespace
