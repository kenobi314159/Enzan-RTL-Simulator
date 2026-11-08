// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end scheduling tests: EIR is built by hand, verified, elaborated, compiled and run.
// This is where the simulation semantics are pinned, and a change to them MUST come with
// tests that do exactly this.
//
// Building EIR by hand rather than parsing SystemVerilog is deliberate at this point: it
// tests the kernel without depending on the frontend, so a scheduling bug and a frontend
// bug cannot be confused for one another.

#include "runtime/Kernel.h"

#include "common/Diagnostics.h"
#include "eir/BitOps.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"

#include "support/Literals.h"
#include "support/ProcessBuilder.h"
#include "support/Simulation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// NOLINTBEGIN(misc-unused-using-decls)
using enzan::eir::BasicBlock;
using enzan::eir::BitVector;
using enzan::eir::Design;
using enzan::eir::EdgeKind;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ParameterMode;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Radix;
using enzan::eir::SensitivityEntry;
using enzan::eir::Subprogram;
using enzan::eir::SysTaskArgument;
using enzan::eir::SysTaskCall;
using enzan::eir::SysTaskKind;
using enzan::eir::Type;
using enzan::runtime::Kernel;
using enzan::runtime::NoTime;
using enzan::runtime::PliHooks;
using enzan::runtime::RunInterrupt;
using enzan::runtime::RunStatus;
using enzan::runtime::SimTime;
using enzan::test::literal;
using enzan::test::ProcessBuilder;
using enzan::test::Simulation;
using enzan::test::SubprogramBuilder;

// NOLINTEND(misc-unused-using-decls)

/// A module with two 8-bit objects and whatever processes the caller adds.
ModuleDefinition makeTop(std::vector<Process> processes, std::vector<Object> objects = {}) {
    ModuleDefinition definition;
    definition.name = "tb";
    definition.sourceName = "tb";

    if (objects.empty()) {
        definition.addObject(Object{.name = "a", .type = Type::bits(8, false, true)});
        definition.addObject(Object{.name = "b", .type = Type::bits(8, false, true)});
    } else {
        for (Object& object : objects) definition.addObject(std::move(object));
    }

    definition.processes = std::move(processes);
    return definition;
}

Design designOf(ModuleDefinition definition) {
    Design design;
    design.addModule(std::move(definition));
    return design;
}

// ---------------------------------------------------------------------------
// Time zero
// ---------------------------------------------------------------------------

TEST(Kernel, FourStateObjectsStartAsXAndTwoStateOnesAsZero) {
    // A four-state object that defaulted to zero would hide exactly the bugs
    // X exists to reveal.
    ModuleDefinition definition =
        makeTop({}, {Object{.name = "fourstate", .type = Type::bits(4, false, true)},
                     Object{.name = "twostate", .type = Type::bits(4, false, false)}});

    const Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready());

    EXPECT_EQ(simulation.value("/tb/fourstate"), "xxxx");
    EXPECT_EQ(simulation.value("/tb/twostate"), "0000");
}

TEST(Kernel, ADeclaredInitialValueIsApplied) {
    ModuleDefinition definition = makeTop({}, {Object{.name = "preset",
                                                      .type = Type::bits(8, false, true),
                                                      .initialValue = BitVector::parse("8'hA5")}});

    const Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready());
    EXPECT_EQ(simulation.value("/tb/preset", Radix::Hexadecimal), "a5");
}

TEST(Kernel, ARealStartsAtItsDeclaredInitialValue) {
    // A VHDL real signal is held as 64 bits are, and starts at the double its declaration gives,
    // 2.5 here, rather than at zero.
    ModuleDefinition definition = makeTop(
        {}, {Object{.name = "ratio",
                    .type = Type::real(),
                    .initialValue = BitVector::fromUInt64(std::bit_cast<std::uint64_t>(2.5), 64)}});

    const Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready());
    EXPECT_EQ(simulation.value("/tb/ratio", Radix::Hexadecimal), "4004000000000000");
}

TEST(Kernel, AlwaysCombRunsAtTimeZero) {
    // IEEE 1800 defines always_comb to evaluate once at time zero so that combinational
    // outputs are consistent before any input has moved.
    ProcessBuilder builder(ProcessKind::AlwaysComb, "copy");
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    builder.block("entry");
    builder.readObject(value, 0);
    builder.store(1, value);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.objects[0].initialValue = BitVector::parse("8'h3C");

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "3c");
}

TEST(Kernel, AlwaysFfDoesNotRunAtTimeZero) {
    // A flip-flop whose clock has not edged has done nothing, and running its body at
    // time zero would load a value without a clock ever having arrived.
    ProcessBuilder builder(ProcessKind::AlwaysFf, "ff");
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    builder.sensitive(0, EdgeKind::Posedge);
    builder.block("entry");
    builder.readObject(value, 0);
    builder.store(1, value);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.objects[0].initialValue = BitVector::parse("8'h3C");

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/b"), "xxxxxxxx") << "nothing clocked it";
}

// ---------------------------------------------------------------------------
// Blocking and non-blocking assignment
// ---------------------------------------------------------------------------

TEST(Kernel, ABlockingAssignmentIsVisibleImmediatelyWithinAProcess) {
    ProcessBuilder builder(ProcessKind::Initial, "blocking");
    const std::uint32_t temporary = builder.reg(Type::bits(8, false, true));
    const std::uint32_t constant = builder.constant("8'h11");
    builder.block("entry");
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(temporary),
                                   Operand::constant(constant)));
    builder.store(0, temporary);
    // Reading `a` back must see the value just written, not the old one.
    builder.readObject(temporary, 0);
    builder.store(1, temporary);
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "11");
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "11");
}

TEST(Kernel, NonBlockingAssignmentsSwapRatherThanChain) {
    // The canonical test of the NBA region: `a <= b; b <= a;` in one time slot must swap
    // the two, because both right-hand sides are read before either update is applied. A
    // simulator that applied them immediately would copy one value into both.
    ProcessBuilder builder(ProcessKind::Initial, "swap");
    const std::uint32_t fromA = builder.reg(Type::bits(8, false, true));
    const std::uint32_t fromB = builder.reg(Type::bits(8, false, true));
    builder.block("entry");
    builder.readObject(fromA, 0);
    builder.readObject(fromB, 1);
    builder.storeNonBlocking(0, fromB);
    builder.storeNonBlocking(1, fromA);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.objects[0].initialValue = BitVector::parse("8'h11");
    definition.objects[1].initialValue = BitVector::parse("8'h22");

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "22");
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "11");
}

TEST(Kernel, TwoPartialNonBlockingUpdatesBothTakeEffect) {
    // Two assignments to different parts of one signal in the same time slot must both
    // land; collapsing them would let the later one discard the earlier one's bits.
    ProcessBuilder builder(ProcessKind::Initial, "parts");
    const std::uint32_t nibble = builder.reg(Type::bits(4, false, true));
    const std::uint32_t index = builder.reg(Type::bits(8, false, true));
    const std::uint32_t lowValue = builder.constant("4'hF");
    const std::uint32_t highValue = builder.constant("4'h3");
    const std::uint32_t zero = builder.constant("8'd0");
    const std::uint32_t four = builder.constant("8'd4");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(nibble), Operand::constant(lowValue)));
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(index), Operand::constant(zero)));
    builder.storePartNonBlocking(0, nibble, index, 8);
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(nibble),
                                   Operand::constant(highValue)));
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(index), Operand::constant(four)));
    builder.storePartNonBlocking(0, nibble, index, 8);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.objects[0].initialValue = BitVector::parse("8'h00");

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "3f");
}

// ---------------------------------------------------------------------------
// Delays and time
// ---------------------------------------------------------------------------

TEST(Kernel, ADelayAdvancesTime) {
    ProcessBuilder builder(ProcessKind::Initial, "delayed");
    const std::uint32_t delay = builder.reg(Type::bits(32, false, false));
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    const std::uint32_t ten = builder.constant("32'd10");
    const std::uint32_t one = builder.constant("8'h01");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(ten)));
    builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    builder.block("after");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(one)));
    builder.store(0, value);
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.time(), 10U);
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "01");
}

TEST(Kernel, RunStopsAtATimeLimitAndResumes) {
    ProcessBuilder builder(ProcessKind::Initial, "delayed");
    const std::uint32_t delay = builder.reg(Type::bits(32, false, false));
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    const std::uint32_t hundred = builder.constant("32'd100");
    const std::uint32_t one = builder.constant("8'h01");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(hundred)));
    builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    builder.block("after");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(one)));
    builder.store(0, value);
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(50), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/a"), "xxxxxxxx") << "the delay has not expired";

    // `until` is inclusive, so a second run continues rather than skipping the slot.
    EXPECT_EQ(simulation.run(100), RunStatus::Quiescent);
    EXPECT_EQ(simulation.time(), 100U);
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "01");
}

TEST(Kernel, AZeroDelayRunsAfterEverythingAlreadyActive) {
    // `#0` resumes in this timestamp but after the Active region drains, which is exactly
    // what the Inactive region is for. The second process writes last, so its value is
    // the one that survives.
    ProcessBuilder first(ProcessKind::Initial, "zero-delay");
    const std::uint32_t firstDelay = first.reg(Type::bits(32, false, false));
    const std::uint32_t firstValue = first.reg(Type::bits(8, false, true));
    const std::uint32_t zero = first.constant("32'd0");
    const std::uint32_t oneValue = first.constant("8'h01");
    first.block("entry");
    first.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(firstDelay), Operand::constant(zero)));
    first.emit(
        Instruction::make(Opcode::WaitTime, {}, Operand::reg(firstDelay), Operand::block(1)));
    first.block("after");
    first.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(firstValue),
                                 Operand::constant(oneValue)));
    first.store(0, firstValue);
    first.emit(Instruction::make(Opcode::Return));

    ProcessBuilder second(ProcessKind::Initial, "immediate");
    const std::uint32_t secondValue = second.reg(Type::bits(8, false, true));
    const std::uint32_t twoValue = second.constant("8'h02");
    second.block("entry");
    second.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(secondValue),
                                  Operand::constant(twoValue)));
    second.store(0, secondValue);
    second.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({first.finish(), second.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.time(), 0U) << "a zero delay does not advance time";
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "01")
        << "the deferred write happened after the immediate one";
}

TEST(Kernel, TheClockGeneratorIdiomToggles) {
    // `always #5 clk = ~clk;` is the single most common construct in any testbench. It
    // has no sensitivity list, so it must start at time zero and go round again after
    // completing.
    ProcessBuilder builder(ProcessKind::Always, "clockgen");
    const std::uint32_t delay = builder.reg(Type::bits(32, false, false));
    const std::uint32_t value = builder.reg(Type::logic());
    // A distinct destination: no operation but a move, a width change or a splice may
    // write the register it reads.
    const std::uint32_t inverted = builder.reg(Type::logic());
    const std::uint32_t five = builder.constant("32'd5");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(five)));
    builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    builder.block("toggle");
    builder.readObject(value, 0);
    builder.emit(Instruction::make(Opcode::Not, Operand::reg(inverted), Operand::reg(value)));
    builder.store(0, inverted);
    builder.emit(Instruction::make(Opcode::Jump, {}, Operand::block(0)));

    ModuleDefinition definition = makeTop(
        {builder.finish()},
        {Object{.name = "clk", .type = Type::logic(), .initialValue = BitVector::parse("1'b0")}});

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(5), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), "1");

    EXPECT_EQ(simulation.run(10), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), "0");

    EXPECT_EQ(simulation.run(15), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), "1");
}

// ---------------------------------------------------------------------------
// Sensitivity and activity-based scheduling
// ---------------------------------------------------------------------------

/// A design with a clock driven by an `initial` block and a flip-flop watching it.
Design makeClockedDesign(EdgeKind edge, std::string_view firstValue, std::string_view secondValue) {
    ProcessBuilder driver(ProcessKind::Initial, "driver");
    const std::uint32_t delay = driver.reg(Type::bits(32, false, false));
    const std::uint32_t bit = driver.reg(Type::logic());
    const std::uint32_t ten = driver.constant("32'd10");
    const std::uint32_t first = driver.constant(firstValue);
    const std::uint32_t second = driver.constant(secondValue);

    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(bit), Operand::constant(first)));
    driver.store(0, bit);
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(ten)));
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    driver.block("second");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(bit), Operand::constant(second)));
    driver.store(0, bit);
    driver.emit(Instruction::make(Opcode::Return));

    ProcessBuilder watcher(ProcessKind::AlwaysFf, "watcher");
    const std::uint32_t counterValue = watcher.reg(Type::bits(8, false, true));
    const std::uint32_t oneRegister = watcher.reg(Type::bits(8, false, true));
    // A distinct destination: an arithmetic operation may not alias its sources, because
    // the implementation writes the destination as it goes.
    const std::uint32_t sum = watcher.reg(Type::bits(8, false, true));
    const std::uint32_t one = watcher.constant("8'd1");

    watcher.sensitive(0, edge);
    watcher.block("entry");
    watcher.readObject(counterValue, 1);
    watcher.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(oneRegister), Operand::constant(one)));
    watcher.emit(Instruction::make(Opcode::Add, Operand::reg(sum), Operand::reg(counterValue),
                                   Operand::reg(oneRegister)));
    watcher.store(1, sum);
    watcher.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({driver.finish(), watcher.finish()},
                                          {Object{.name = "clk", .type = Type::logic()},
                                           Object{.name = "count",
                                                  .type = Type::bits(8, false, true),
                                                  .initialValue = BitVector::parse("8'd0")}});

    return designOf(std::move(definition));
}

TEST(Kernel, PosedgeFiresOnATransitionTowardsOne) {
    // IEEE 1364 defines posedge as a transition towards 1, so x to 1 counts. That matters
    // on the first edge of a clock that starts out X, which is every clock at time zero.
    Simulation simulation(makeClockedDesign(EdgeKind::Posedge, "1'b0", "1'b1"), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    // x -> 0 at time 0 is a negedge, 0 -> 1 at time 10 is a posedge.
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "1");
}

TEST(Kernel, PosedgeFiresFromUnknownToOne) {
    Simulation simulation(makeClockedDesign(EdgeKind::Posedge, "1'bx", "1'b1"), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "1") << "x to 1 is a positive edge";
}

TEST(Kernel, NegedgeFiresOnATransitionTowardsZero) {
    Simulation simulation(makeClockedDesign(EdgeKind::Negedge, "1'b1", "1'b0"), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "1");
}

TEST(Kernel, AWriteOfTheSameValueWakesNothing) {
    // Value-change filtering before scheduling, one of the largest single performance wins
    // available.
    Simulation simulation(makeClockedDesign(EdgeKind::AnyChange, "1'b1", "1'b1"), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    // x -> 1 at time 0 is a change; writing 1 again at time 10 is not.
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "1");
}

TEST(Kernel, WaitEdgeSuspendsUntilTheEdgeArrives) {
    ProcessBuilder driver(ProcessKind::Initial, "driver");
    const std::uint32_t delay = driver.reg(Type::bits(32, false, false));
    const std::uint32_t bit = driver.reg(Type::logic());
    const std::uint32_t ten = driver.constant("32'd10");
    const std::uint32_t zeroBit = driver.constant("1'b0");
    const std::uint32_t oneBit = driver.constant("1'b1");

    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(bit), Operand::constant(zeroBit)));
    driver.store(0, bit);
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(ten)));
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    driver.block("raise");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(bit), Operand::constant(oneBit)));
    driver.store(0, bit);
    driver.emit(Instruction::make(Opcode::Return));

    ProcessBuilder waiter(ProcessKind::Initial, "waiter");
    const std::uint32_t marker = waiter.reg(Type::bits(8, false, true));
    const std::uint32_t markerValue = waiter.constant("8'hAB");
    waiter.block("entry");
    waiter.waitEdge(0, EdgeKind::Posedge, 1);
    waiter.block("resumed");
    waiter.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(marker),
                                  Operand::constant(markerValue)));
    waiter.store(1, marker);
    waiter.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition =
        makeTop({driver.finish(), waiter.finish()},
                {Object{.name = "clk", .type = Type::logic()},
                 Object{.name = "marker", .type = Type::bits(8, false, true)}});

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(5), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/marker"), "xxxxxxxx") << "no edge yet";

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/marker", Radix::Hexadecimal), "ab");
    EXPECT_EQ(simulation.time(), 10U);
}

/// A process that waits on `@(posedge clk or posedge rst)` and counts each time it resumes,
/// once when `forever` is false and in a loop when it is true.
Process makeEventListCounter(bool forever) {
    ProcessBuilder counter(ProcessKind::Initial, "counter");
    const std::uint32_t countValue = counter.reg(Type::bits(16, false, true));
    const std::uint32_t oneRegister = counter.reg(Type::bits(16, false, true));
    const std::uint32_t sum = counter.reg(Type::bits(16, false, true));
    const std::uint32_t one = counter.constant("16'd1");

    counter.block("entry");
    counter.waitEvents({SensitivityEntry{.objectIndex = 0, .edge = EdgeKind::Posedge},
                        SensitivityEntry{.objectIndex = 1, .edge = EdgeKind::Posedge}},
                       1);
    counter.block("resumed");
    counter.readObject(countValue, 2);
    counter.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(oneRegister), Operand::constant(one)));
    counter.emit(Instruction::make(Opcode::Add, Operand::reg(sum), Operand::reg(countValue),
                                   Operand::reg(oneRegister)));
    counter.store(2, sum);
    counter.emit(forever ? Instruction::make(Opcode::Jump, {}, Operand::block(0))
                         : Instruction::make(Opcode::Return));
    return counter.finish();
}

std::vector<Object> eventListObjects() {
    return {Object{.name = "clk", .type = Type::logic(), .initialValue = BitVector::parse("1'b0")},
            Object{.name = "rst", .type = Type::logic(), .initialValue = BitVector::parse("1'b0")},
            Object{.name = "count",
                   .type = Type::bits(16, false, true),
                   .initialValue = BitVector::parse("16'd0")}};
}

TEST(Kernel, AnEventListResumesOnceWhicheverOfItsEntriesHappen) {
    // `clk` rises first and resumes the process, which then ends. The subscription on `rst` is
    // stale from then on, and when `rst` rises later it must not resume the process again.
    ProcessBuilder driver(ProcessKind::Initial, "driver");
    const std::uint32_t delay = driver.reg(Type::bits(32, false, false));
    const std::uint32_t bit = driver.reg(Type::logic());
    const std::uint32_t ten = driver.constant("32'd10");
    const std::uint32_t oneBit = driver.constant("1'b1");

    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(ten)));
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(bit), Operand::constant(oneBit)));
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    driver.block("clock");
    driver.store(0, bit);
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(2)));
    driver.block("reset");
    driver.store(1, bit);
    driver.emit(Instruction::make(Opcode::Return));

    Simulation simulation(
        designOf(makeTop({driver.finish(), makeEventListCounter(false)}, eventListObjects())),
        "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.time(), 20U);
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "1");
}

TEST(Kernel, AnEventListEntryWhoseSignalNeverChangesDoesNotPileUp) {
    // `forever @(posedge clk or posedge rst)` with a reset that never rises: every wait leaves
    // a stale subscription on `rst`, which nothing else ever removes.
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

    Simulation simulation(
        designOf(makeTop({clock.finish(), makeEventListCounter(true)}, eventListObjects())), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(10'000), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "1000");
    EXPECT_LE(simulation.simulationKernel().armedWaits(simulation.signal("/tb/rst")), 16U)
        << "a stale subscription on a signal that never changes stayed for the rest of the run";
}

// ---------------------------------------------------------------------------
// System tasks
// ---------------------------------------------------------------------------

TEST(Kernel, DisplayWritesToTheTranscript) {
    ProcessBuilder builder(ProcessKind::Initial, "talker");
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    const std::uint32_t now = builder.reg(Type::bits(64, false, false));
    const std::uint32_t constant = builder.constant("8'd42");
    const std::uint32_t format = builder.string("value = %0d at %0t in %m");

    // `%t` formats an argument rather than reading the clock, which is what
    // `$display("%t", $time)` does, so the time is passed in like any other value.
    SysTaskCall call;
    call.kind = SysTaskKind::Display;
    call.formatIndex = format;
    call.arguments.push_back(
        SysTaskArgument{.value = Operand::reg(value), .type = Type::bits(8, false, true)});
    call.arguments.push_back(
        SysTaskArgument{.value = Operand::reg(now), .type = Type::bits(64, false, false)});
    const std::uint32_t descriptor = builder.sysCall(std::move(call));

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(constant)));
    builder.emit(Instruction::make(Opcode::ReadTime, Operand::reg(now)));
    builder.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.transcript(), "value = 42 at 0ps in tb\n");
}

/// `write(l, 42, left, 5); write(l, -7); writeline(output, l)`, and the handle writeline leaves in
/// `l` stored in `after`.
Design makeLineWriter() {
    ProcessBuilder builder(ProcessKind::Initial, "writer");
    const Type handle = Type::bits(64, false, false);
    const Type integer = Type::bits(32, true, false);
    const std::uint32_t line = builder.reg(handle);
    const std::uint32_t left = builder.reg(Type::bits(8, false, false));
    const std::uint32_t right = builder.reg(Type::bits(8, false, false));
    const std::uint32_t field = builder.reg(integer);
    const std::uint32_t none = builder.reg(integer);
    const std::uint32_t first = builder.reg(integer);
    const std::uint32_t second = builder.reg(integer);
    const std::uint32_t output = builder.reg(Type::bits(32, false, false));
    const std::uint32_t written = builder.reg(handle);
    const std::uint32_t decimal = builder.string("%0d");

    const auto argument = [](std::uint32_t reg, Type type) {
        return SysTaskArgument{.value = Operand::reg(reg), .type = type};
    };
    const auto write = [&](std::uint32_t justified, std::uint32_t width, std::uint32_t value) {
        SysTaskCall call;
        call.kind = SysTaskKind::LineWrite;
        call.formatIndex = decimal;
        call.arguments = {argument(line, handle), argument(justified, Type::bits(8, false, false)),
                          argument(width, integer), argument(value, integer)};
        return builder.sysCall(std::move(call));
    };
    const std::uint32_t writeFirst = write(left, field, first);
    const std::uint32_t writeSecond = write(right, none, second);
    SysTaskCall writeLine;
    writeLine.kind = SysTaskKind::LineWriteLine;
    writeLine.arguments = {argument(output, Type::bits(32, false, false)), argument(line, handle)};
    const std::uint32_t printLine = builder.sysCall(std::move(writeLine));

    const auto load = [&](std::uint32_t reg, std::string_view literal) {
        builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(reg),
                                       Operand::constant(builder.constant(literal))));
    };
    builder.block("entry");
    load(line, "64'd0");
    load(left, "8'd1");
    load(right, "8'd0");
    load(field, "32'sd5");
    load(none, "32'sd0");
    load(first, "32'sd42");
    load(second, "32'hfffffff9");
    load(output, "32'h80000001");
    builder.emit(
        Instruction::make(Opcode::SysFunction, Operand::reg(line), Operand::sysCall(writeFirst)));
    builder.emit(
        Instruction::make(Opcode::SysFunction, Operand::reg(line), Operand::sysCall(writeSecond)));
    builder.emit(
        Instruction::make(Opcode::SysFunction, Operand::reg(written), Operand::sysCall(printLine)));
    builder.store(0, written);
    builder.emit(Instruction::make(Opcode::Return));

    return designOf(makeTop({builder.finish()}, {Object{.name = "after", .type = handle}}));
}

TEST(Kernel, TextioLinesLiveInTheHeapAndAreWrittenWhole) {
    // The first write allocates a line, the second appends to it, and writeline prints it and
    // leaves a new empty one, the handle the first freed.
    Simulation simulation(makeLineWriter(), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.transcript(), "42   -7\n");
    EXPECT_EQ(simulation.value("/tb/after", Radix::Decimal), "1");
}

TEST(Kernel, FinishEndsTheRun) {
    ProcessBuilder builder(ProcessKind::Initial, "quitter");
    SysTaskCall call;
    call.kind = SysTaskKind::Finish;
    const std::uint32_t descriptor = builder.sysCall(std::move(call));

    builder.block("entry");
    builder.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Finished);
    EXPECT_EQ(simulation.simulationKernel().exitStatus(), 0);
}

TEST(Kernel, FatalReportsAndStopsWithAFailureStatus) {
    ProcessBuilder builder(ProcessKind::Initial, "failer");
    const std::uint32_t format = builder.string("mismatch detected");
    SysTaskCall call;
    call.kind = SysTaskKind::Fatal;
    call.formatIndex = format;
    const std::uint32_t descriptor = builder.sysCall(std::move(call));

    builder.block("entry");
    builder.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_EQ(simulation.simulationKernel().exitStatus(), 1);
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::UserFatal));
}

TEST(Kernel, SeverityTasksReportThroughTheDiagnosticEngine) {
    // A self-checking testbench's `$error` must reach the same diagnostic engine as a
    // compile error, so that one transcript shows everything.
    ProcessBuilder builder(ProcessKind::Initial, "checker");
    const std::uint32_t format = builder.string("check failed");
    SysTaskCall call;
    call.kind = SysTaskKind::Error;
    call.formatIndex = format;
    const std::uint32_t descriptor = builder.sysCall(std::move(call));

    builder.block("entry");
    builder.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::UserError));
    EXPECT_TRUE(simulation.transcript().empty()) << "$error is a diagnostic, not output";
}

TEST(Kernel, AReportsSeverityChoosesItsDiagnostic) {
    // VHDL's `report` and failed `assert`: note, warning and error are reported and the run goes
    // on, and failure stops it, as GHDL's default does.
    ProcessBuilder builder(ProcessKind::Initial, "reporter");
    const std::uint32_t severity = builder.reg(Type::bits(8, false, false));
    std::vector<std::uint32_t> constants;
    std::vector<std::uint32_t> descriptors;
    for (const std::string_view level : {"8'd0", "8'd1", "8'd2", "8'd3"}) {
        constants.push_back(builder.constant(level));
        SysTaskCall call;
        call.kind = SysTaskKind::Report;
        call.formatIndex = builder.string(std::string("severity ") + std::string(level));
        call.arguments.push_back(
            SysTaskArgument{.value = Operand::reg(severity), .type = Type::bits(8, false, false)});
        descriptors.push_back(builder.sysCall(std::move(call)));
    }

    builder.block("entry");
    for (std::size_t index = 0; index < constants.size(); ++index) {
        builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(severity),
                                       Operand::constant(constants[index])));
        builder.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptors[index])));
    }
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_EQ(simulation.simulationKernel().exitStatus(), 1);
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::UserInfo));
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::UserWarning));
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::UserError));
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::AssertionFailure));
    // The severity is not part of the message.
    EXPECT_EQ(simulation.messages().back(), "severity 8'd3");
}

TEST(Kernel, AnEmptyEventListWaitsForEver) {
    // VHDL's `wait;`, which ends a testbench process: nothing wakes it, and what follows never
    // runs.
    ProcessBuilder builder(ProcessKind::Always, "waiter");
    SysTaskCall call;
    call.kind = SysTaskKind::Display;
    call.formatIndex = builder.string("woken");
    const std::uint32_t descriptor = builder.sysCall(std::move(call));

    builder.block("entry");
    builder.waitEvents({}, 1);
    builder.block("after");
    builder.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_TRUE(simulation.transcript().empty());
}

// ---------------------------------------------------------------------------
// VHDL drivers (IEEE 1076 section 10.5.2.2)
// ---------------------------------------------------------------------------

/// A VHDL-style process: `steps` run in order, each loading `value` into a register and driving
/// object 0 with it `delay` from now, `inertial` or not; a `wait` of `waitBefore` comes first when
/// it is not zero. Then it waits for ever.
struct DriveStep {
    std::uint64_t waitBefore = 0;
    std::uint8_t value = 0;
    std::uint64_t delay = 0;
    bool inertial = true;
};

Process makeDriver(const std::vector<DriveStep>& steps) {
    ProcessBuilder builder(ProcessKind::Always, "driver");
    const std::uint32_t value = builder.reg(Type::bits(8, false, false));
    const std::uint32_t delay = builder.reg(Type::bits(64, true, false));
    builder.block("entry");
    for (const DriveStep& step : steps) {
        if (step.waitBefore != 0) {
            builder.emit(Instruction::make(
                Opcode::LoadConstant, Operand::reg(delay),
                Operand::constant(builder.constant("64'd" + std::to_string(step.waitBefore)))));
            const auto resume = builder.nextBlock();
            builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay),
                                           Operand::block(resume)));
            builder.block("resumed");
        }
        builder.emit(Instruction::make(
            Opcode::LoadConstant, Operand::reg(value),
            Operand::constant(builder.constant("8'd" + std::to_string(step.value)))));
        builder.emit(Instruction::make(
            Opcode::LoadConstant, Operand::reg(delay),
            Operand::constant(builder.constant("64'd" + std::to_string(step.delay)))));
        builder.drive(0, value, delay, step.inertial);
    }
    const auto end = builder.nextBlock();
    builder.waitEvents({}, end);
    builder.block("end");
    builder.emit(Instruction::make(Opcode::Return));
    Process process = builder.finish();
    process.drivesSignals = true;
    return process;
}

ModuleDefinition driven(std::vector<Process> processes) {
    return makeTop(std::move(processes),
                   {Object{.name = "s", .type = Type::bits(8, false, false)},
                    Object{.name = "seen", .type = Type::bits(8, false, false)}});
}

TEST(Kernel, ADriveDeletesWhatItsDriverHadPendingFromItsTimeOn) {
    // s <= transport 1 after 10; s <= transport 2 after 5: the second deletes the first.
    Simulation simulation(
        designOf(driven({makeDriver({{.value = 1, .delay = 10, .inertial = false},
                                     {.value = 2, .delay = 5, .inertial = false}})})),
        "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/s", Radix::Decimal), "2");
    EXPECT_EQ(simulation.time(), 5U) << "the deleted transaction leaves no time step behind";
}

TEST(Kernel, OfTwoAssignmentsInOneDeltaTheLastHappens) {
    // s <= 1; s <= 2; in one run of a process: one transaction, so one event.
    Simulation simulation(designOf(driven({makeDriver({{.value = 1}, {.value = 2}})})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/s", Radix::Decimal), "2");
}

TEST(Kernel, AnInertialDriveRejectsAPulseShorterThanItsDelay) {
    // s <= 1 after 10; then at 2, s <= 0 after 10: the 1 would last less than the delay, so it is
    // rejected and s never leaves 0.
    Simulation simulation(
        designOf(driven(
            {makeDriver({{.value = 1, .delay = 10}, {.waitBefore = 2, .value = 0, .delay = 10}})})),
        "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(11), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/s", Radix::Decimal), "0");
}

TEST(Kernel, AnInertialDriveKeepsARunOfItsOwnValue) {
    // s <= 1 after 10; then at 2, s <= 1 after 10: the earlier 1 is part of the new one's run, so
    // s becomes 1 at 10 rather than at 12.
    Simulation simulation(
        designOf(driven(
            {makeDriver({{.value = 1, .delay = 10}, {.waitBefore = 2, .value = 1, .delay = 10}})})),
        "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(10), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/s", Radix::Decimal), "1");
}

TEST(Kernel, ATransportDriveRejectsNothing) {
    Simulation simulation(designOf(driven({makeDriver(
                              {{.value = 1, .delay = 10, .inertial = false},
                               {.waitBefore = 2, .value = 0, .delay = 10, .inertial = false}})})),
                          "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(10), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/s", Radix::Decimal), "1");
}

/// A process driving a 16-bit `s` as `steps` say: a whole drive when `offset` is absent, and a
/// part drive of the low or the high byte otherwise.
struct PartStep {
    std::uint16_t value = 0;
    std::optional<std::uint32_t> offset;
    std::uint64_t delay = 0;
    bool inertial = true;
};

Process makePartDriver(const std::vector<PartStep>& steps) {
    ProcessBuilder builder(ProcessKind::Always, "driver");
    const std::uint32_t whole = builder.reg(Type::bits(16, false, false));
    const std::uint32_t byte = builder.reg(Type::bits(8, false, false));
    const std::uint32_t delay = builder.reg(Type::bits(64, true, false));
    const std::uint32_t offset = builder.reg(Type::bits(32, false, false));
    builder.block("entry");
    for (const PartStep& step : steps) {
        if (step.offset) {
            builder.emit(Instruction::make(
                Opcode::LoadConstant, Operand::reg(byte),
                Operand::constant(builder.constant("8'd" + std::to_string(step.value)))));
            builder.emit(Instruction::make(
                Opcode::LoadConstant, Operand::reg(offset),
                Operand::constant(builder.constant("32'd" + std::to_string(*step.offset)))));
            builder.drivePart(0, byte, offset, 16);
            continue;
        }
        builder.emit(Instruction::make(
            Opcode::LoadConstant, Operand::reg(whole),
            Operand::constant(builder.constant("16'd" + std::to_string(step.value)))));
        builder.emit(Instruction::make(
            Opcode::LoadConstant, Operand::reg(delay),
            Operand::constant(builder.constant("64'd" + std::to_string(step.delay)))));
        builder.drive(0, whole, delay, step.inertial);
    }
    builder.waitEvents({}, builder.nextBlock());
    builder.block("end");
    builder.emit(Instruction::make(Opcode::Return));
    Process process = builder.finish();
    process.drivesSignals = true;
    return process;
}

ModuleDefinition partDriven(std::vector<Process> processes) {
    return makeTop(std::move(processes),
                   {Object{.name = "s", .type = Type::bits(16, false, false)}});
}

TEST(Kernel, AssignmentsToSeparatePartsOfASignalAreSeparateTransactions) {
    // s(0) <= ...; s(1) <= ...; in one run: neither deletes the other.
    Simulation simulation(designOf(partDriven({makePartDriver(
                              {{.value = 0x34, .offset = 0}, {.value = 0x12, .offset = 8}})})),
                          "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/s", Radix::Hexadecimal), "1234");
}

TEST(Kernel, APartAssignmentDeletesOnlyThePendingBitsItAssigns) {
    // s <= transport x"1111" after 10; s(low byte) <= x"22": what was pending for the low byte is
    // gone, and the high byte's part of it still happens at 10.
    Simulation simulation(
        designOf(partDriven({makePartDriver(
            {{.value = 0x1111, .delay = 10, .inertial = false}, {.value = 0x22, .offset = 0}})})),
        "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(5), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/s", Radix::Hexadecimal), "0022");
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/s", Radix::Hexadecimal), "1122");
    EXPECT_EQ(simulation.time(), 10U);
}

TEST(Kernel, InertialRejectionOverAPartlyPendingSignalIsRefused) {
    // Rejection is decided for each subelement, and a pending transaction for part of what an
    // inertial assignment assigns would need it decided for some of them and not others.
    Simulation simulation(designOf(partDriven({makePartDriver(
                              {{.value = 0x22, .offset = 0}, {.value = 0x1111, .delay = 10}})})),
                          "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::NotYetImplemented));
}

/// A process that waits for `s` (object 0) to change, then records whether `s` and `other`
/// (object 2) had an event, in objects 1 and 3, and what `s` was before, in object 4.
Process makeWatcher() {
    ProcessBuilder builder(ProcessKind::Always, "watcher");
    const std::uint32_t event = builder.reg(Type::bits(1, false, false));
    const std::uint32_t widened = builder.reg(Type::bits(8, false, false));
    const std::uint32_t otherEvent = builder.reg(Type::bits(1, false, false));
    const std::uint32_t otherWidened = builder.reg(Type::bits(8, false, false));
    const std::uint32_t before = builder.reg(Type::bits(8, false, false));
    builder.block("entry");
    builder.waitEvents({SensitivityEntry{.objectIndex = 0}}, builder.nextBlock());
    builder.block("woken");
    builder.readObject(before, 0);
    builder.readObject(otherWidened, 2);
    builder.emit(Instruction::make(Opcode::ReadEvent, Operand::reg(event), Operand::object(0)));
    builder.emit(
        Instruction::make(Opcode::ReadEvent, Operand::reg(otherEvent), Operand::object(2)));
    builder.emit(
        Instruction::make(Opcode::ReadLastValue, Operand::reg(before), Operand::object(0)));
    builder.emit(Instruction::make(Opcode::Extend, Operand::reg(widened), Operand::reg(event)));
    builder.emit(
        Instruction::make(Opcode::Extend, Operand::reg(otherWidened), Operand::reg(otherEvent)));
    builder.store(1, widened);
    builder.store(3, otherWidened);
    builder.store(4, before);
    builder.emit(Instruction::make(Opcode::Jump, {}, Operand::block(0)));
    return builder.finish();
}

ModuleDefinition watched(std::vector<Process> processes) {
    return makeTop(std::move(processes),
                   {Object{.name = "s", .type = Type::bits(8, false, false)},
                    Object{.name = "event", .type = Type::bits(8, false, false)},
                    Object{.name = "other", .type = Type::bits(8, false, false)},
                    Object{.name = "other_event", .type = Type::bits(8, false, false)},
                    Object{.name = "last", .type = Type::bits(8, false, false)}});
}

TEST(Kernel, AnEventIsAChangeInTheUpdateThisCycleBeganWith) {
    // VHDL's 'event: true of the signal that changed, false of one that did not.
    Simulation simulation(designOf(watched({makeDriver({{.value = 5}}), makeWatcher()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/event", Radix::Decimal), "1");
    EXPECT_EQ(simulation.value("/tb/other_event", Radix::Decimal), "0");
}

TEST(Kernel, ALastValueIsTheValueBeforeTheLastChange) {
    // s goes to 5, then at 10 to 7: when it does, its 'last_value is 5.
    Simulation simulation(
        designOf(
            watched({makeDriver({{.value = 5}, {.waitBefore = 10, .value = 7}}), makeWatcher()})),
        "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(5), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/last", Radix::Decimal), "0");
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/s", Radix::Decimal), "7");
    EXPECT_EQ(simulation.value("/tb/last", Radix::Decimal), "5");
}

TEST(Kernel, ATransactionTakesEffectBeforeProcessesResumeAtItsTime) {
    // A process waking from `wait for 5` at 5 sees what a transaction for 5 set, since VHDL
    // updates signals before it resumes processes (IEEE 1076 section 14.7.5).
    ProcessBuilder reader(ProcessKind::Initial, "reader");
    const std::uint32_t delay = reader.reg(Type::bits(64, true, false));
    const std::uint32_t value = reader.reg(Type::bits(8, false, false));
    reader.block("entry");
    reader.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(delay),
                                  Operand::constant(reader.constant("64'd5"))));
    reader.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    reader.block("after");
    reader.readObject(value, 0);
    reader.store(1, value);
    reader.emit(Instruction::make(Opcode::Return));

    Simulation simulation(
        designOf(driven({makeDriver({{.value = 7, .delay = 5}}), reader.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/seen", Radix::Decimal), "7");
}

TEST(Kernel, TimeIsReadableAsAValue) {
    ProcessBuilder builder(ProcessKind::Initial, "timer");
    const std::uint32_t delay = builder.reg(Type::bits(32, false, false));
    const std::uint32_t now = builder.reg(Type::bits(8, false, true));
    const std::uint32_t seven = builder.constant("32'd7");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(seven)));
    builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    builder.block("after");
    builder.emit(Instruction::make(Opcode::ReadTime, Operand::reg(now)));
    builder.store(0, now);
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/a", Radix::Decimal), "7");
}

TEST(Kernel, FinalBlocksRunWhenTheSimulationEnds) {
    ProcessBuilder initial(ProcessKind::Initial, "main");
    SysTaskCall finish;
    finish.kind = SysTaskKind::Finish;
    const std::uint32_t finishCall = initial.sysCall(std::move(finish));
    initial.block("entry");
    initial.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(finishCall)));
    initial.emit(Instruction::make(Opcode::Return));

    ProcessBuilder final(ProcessKind::Final, "summary");
    const std::uint32_t format = final.string("done");
    SysTaskCall display;
    display.kind = SysTaskKind::Display;
    display.formatIndex = format;
    const std::uint32_t displayCall = final.sysCall(std::move(display));
    final.block("entry");
    final.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(displayCall)));
    final.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({initial.finish(), final.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Finished);
    EXPECT_EQ(simulation.transcript(), "done\n") << "a final block sees the end of the run";
}

// ---------------------------------------------------------------------------
// The delta limit
// ---------------------------------------------------------------------------

TEST(Kernel, ADeltaLoopIsReportedRatherThanHanging) {
    // The limit must exist and the diagnostic must name what is still active, because a delta loop
    // with no diagnostic is one of the most frustrating failures a user can hit.
    //
    // The loop here is a bare `always` with no time control, which spins in the Active
    // region. Counting each immediate re-arm as a delta is what lets the same limit catch
    // it.
    ProcessBuilder builder(ProcessKind::Always, "spinner");
    builder.block("entry");
    builder.emit(Instruction::make(Opcode::Nop));
    builder.emit(Instruction::make(Opcode::Return));

    Simulation::deltaLimit = 32;
    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    Simulation::deltaLimit = 10'000;
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::DeltaLimitExceeded));

    bool named = false;
    for (const std::string& message : simulation.messages())
        named = named || message.find("spinner") != std::string::npos;
    EXPECT_TRUE(named) << "the report must name the process that is looping";
}

TEST(Kernel, ADesignThatSettlesDoesNotTripTheDeltaLimit) {
    // A chain of combinational processes takes a few deltas per timestamp, which is
    // healthy: the limit is per timestamp and must not accumulate across a long run.
    ProcessBuilder first(ProcessKind::AlwaysComb, "stage0");
    const std::uint32_t firstValue = first.reg(Type::bits(8, false, true));
    first.block("entry");
    first.readObject(firstValue, 0);
    first.store(1, firstValue);
    first.emit(Instruction::make(Opcode::Return));

    ProcessBuilder second(ProcessKind::AlwaysComb, "stage1");
    const std::uint32_t secondValue = second.reg(Type::bits(8, false, true));
    second.block("entry");
    second.readObject(secondValue, 1);
    second.store(2, secondValue);
    second.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition =
        makeTop({first.finish(), second.finish()},
                {Object{.name = "a",
                        .type = Type::bits(8, false, true),
                        .initialValue = BitVector::parse("8'h5A")},
                 Object{.name = "b", .type = Type::bits(8, false, true)},
                 Object{.name = "c", .type = Type::bits(8, false, true)}});

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/c", Radix::Hexadecimal), "5a");
    EXPECT_FALSE(simulation.reportedCode(enzan::DiagCode::DeltaLimitExceeded));
}

TEST(Kernel, ADesignWithNothingToDoIsQuiescentImmediately) {
    Simulation simulation(designOf(makeTop({})), "tb");
    ASSERT_TRUE(simulation.ready());

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.time(), 0U);
}

// ---------------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------------

/// Records what it is told, so that delivery can be asserted without a waveform file.
class RecordingListener final : public enzan::runtime::ValueChangeListener {
public:
    void onValueChange(std::uint32_t signalIndex, enzan::eir::ConstBitRef /*value*/) override {
        changes.push_back(signalIndex);
    }

    void onTimeAdvance(enzan::runtime::SimTime time) override { times.push_back(time); }

    std::vector<std::uint32_t> changes;
    std::vector<enzan::runtime::SimTime> times;
};

TEST(Kernel, EveryRegisteredListenerIsNotified) {
    // A batch run writes a VCD and an FST from one simulation, and the GUI attaches while a run is
    // in progress. Both need more than one listener.
    ProcessBuilder builder(ProcessKind::Initial, "writer");
    const std::uint32_t delay = builder.reg(Type::bits(32, false, false));
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    const std::uint32_t ten = builder.constant("32'd10");
    const std::uint32_t one = builder.constant("8'h01");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(ten)));
    builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    builder.block("after");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(one)));
    builder.store(0, value);
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    RecordingListener first;
    RecordingListener second;
    simulation.simulationKernel().addListener(first);
    simulation.simulationKernel().addListener(second);

    simulation.run();

    EXPECT_FALSE(first.changes.empty());
    EXPECT_EQ(first.changes, second.changes) << "both listeners must see the same changes";
    EXPECT_EQ(first.times, second.times);
    EXPECT_FALSE(first.times.empty()) << "time advanced, so it must have been reported";
}

TEST(Kernel, ARemovedListenerStopsBeingNotified) {
    ProcessBuilder builder(ProcessKind::Initial, "writer");
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    const std::uint32_t one = builder.constant("8'h01");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(one)));
    builder.store(0, value);
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    RecordingListener listener;
    simulation.simulationKernel().addListener(listener);
    simulation.simulationKernel().removeListener(listener);

    simulation.run();
    EXPECT_TRUE(listener.changes.empty());
}

TEST(Kernel, RemovingAListenerThatWasNeverAddedIsHarmless) {
    Simulation simulation(designOf(makeTop({})), "tb");
    ASSERT_TRUE(simulation.ready());

    RecordingListener stranger;
    simulation.simulationKernel().removeListener(stranger);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

TEST(Kernel, ACollapsedPortKeepsTheDeclaredInitialValue) {
    // Elaboration collapses a connected port onto the parent's slot, so one slot is reached through
    // two objects — and a child's port declaration carries no initialiser. Initialising per object
    // rather than per slot let the child's default overwrite the parent's declared value with X,
    // depending only on which instance came last. The design then simulated and produced a
    // plausible wrong answer, the worst failure there is, and it is exactly what happened on the
    // first hierarchical design that was tried.
    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "in", .type = Type::bits(8, false, true)});
    child.ports.push_back(enzan::eir::Port{
        .name = "in", .direction = enzan::eir::PortDirection::Input, .objectIndex = 0});

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    parent.addObject(Object{.name = "drive",
                            .type = Type::bits(8, false, true),
                            .initialValue = BitVector::parse("8'h5A")});

    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0});
    parent.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(parent));

    const Simulation simulation(std::move(design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.value("/tb/drive", Radix::Hexadecimal), "5a");
    EXPECT_EQ(simulation.value("/tb/u_child/in", Radix::Hexadecimal), "5a");
}

TEST(Kernel, AnExplicitInitialiserWinsWhicheverSideDeclaresIt) {
    // The same slot reached from the child's side: the order instances are elaborated in must
    // not decide which value the design starts from.
    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "io",
                           .type = Type::bits(8, false, true),
                           .initialValue = BitVector::parse("8'h3C")});
    child.ports.push_back(enzan::eir::Port{
        .name = "io", .direction = enzan::eir::PortDirection::Output, .objectIndex = 0});

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    parent.addObject(Object{.name = "wire", .type = Type::bits(8, false, true)});

    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0});
    parent.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(parent));

    const Simulation simulation(std::move(design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    EXPECT_EQ(simulation.value("/tb/wire", Radix::Hexadecimal), "3c");
}

TEST(Kernel, ACollapsedPortIsOnePieceOfStorage) {
    // Writing the parent's object must be immediately visible through the child's port
    // name, because they are the same slot (see src/elab/README.md).
    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "in", .type = Type::bits(8, false, true)});
    child.ports.push_back(enzan::eir::Port{
        .name = "in", .direction = enzan::eir::PortDirection::Input, .objectIndex = 0});

    ProcessBuilder driver(ProcessKind::Initial, "driver");
    const std::uint32_t value = driver.reg(Type::bits(8, false, true));
    const std::uint32_t constant = driver.constant("8'h77");
    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(constant)));
    driver.store(0, value);
    driver.emit(Instruction::make(Opcode::Return));

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    parent.addObject(Object{.name = "drive", .type = Type::bits(8, false, true)});
    parent.processes.push_back(driver.finish());

    enzan::eir::Instance instance;
    instance.name = "u_child";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0});
    parent.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(parent));

    Simulation simulation(std::move(design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/drive", Radix::Hexadecimal), "77");
    EXPECT_EQ(simulation.value("/tb/u_child/in", Radix::Hexadecimal), "77");
}

TEST(Kernel, APortsDriversWriteTheObjectItsConnectionDrives) {
    // A VHDL inout port associated through a conversion reads one object and drives another
    // (eir::PortConnection::driveObjectIndex), and so does a port collapsed onto it below.
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.sourceName = "leaf";
    leaf.addObject(Object{.name = "io", .type = Type::bits(8, false, true)});
    leaf.ports.push_back(enzan::eir::Port{
        .name = "io", .direction = enzan::eir::PortDirection::InOut, .objectIndex = 0});
    ProcessBuilder driver(ProcessKind::Initial, "driver");
    const std::uint32_t value = driver.reg(Type::bits(8, false, true));
    const std::uint32_t constant = driver.constant("8'h77");
    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(constant)));
    driver.store(0, value);
    driver.emit(Instruction::make(Opcode::Return));
    leaf.processes.push_back(driver.finish());

    ModuleDefinition middle;
    middle.name = "middle";
    middle.sourceName = "middle";
    middle.addObject(Object{.name = "io", .type = Type::bits(8, false, true)});
    middle.ports.push_back(enzan::eir::Port{
        .name = "io", .direction = enzan::eir::PortDirection::InOut, .objectIndex = 0});
    enzan::eir::Instance inner;
    inner.name = "u_leaf";
    inner.definitionName = "leaf";
    inner.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0});
    middle.instances.push_back(std::move(inner));

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    parent.addObject(Object{.name = "effective",
                            .type = Type::bits(8, false, true),
                            .initialValue = BitVector::parse("8'h00")});
    parent.addObject(Object{.name = "driving",
                            .type = Type::bits(8, false, true),
                            .initialValue = BitVector::parse("8'h00")});
    enzan::eir::Instance outer;
    outer.name = "u_middle";
    outer.definitionName = "middle";
    outer.connections.push_back(
        enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0, .driveObjectIndex = 1});
    parent.instances.push_back(std::move(outer));

    Design design;
    design.addModule(std::move(leaf));
    design.addModule(std::move(middle));
    design.addModule(std::move(parent));

    Simulation simulation(std::move(design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/driving", Radix::Hexadecimal), "77");
    EXPECT_EQ(simulation.value("/tb/effective", Radix::Hexadecimal), "00");
    EXPECT_EQ(simulation.value("/tb/u_middle/u_leaf/io", Radix::Hexadecimal), "00")
        << "the port reads the object it is connected to";
}

// ---------------------------------------------------------------------------
// Interruption
// ---------------------------------------------------------------------------

/// Answers "stop" every `period` times it is asked, and counts the questions.
class CountingInterrupt final : public RunInterrupt {
public:
    explicit CountingInterrupt(unsigned everyNth) : period(everyNth) {}

    bool shouldInterrupt() override {
        ++asked;
        return period != 0 && asked % period == 0;
    }

    unsigned asked = 0;

private:
    unsigned period;
};

/// A clock that toggles every five time units and never stops.
Design freeRunningClock() {
    ProcessBuilder builder(ProcessKind::Initial, "clock");
    const std::uint32_t delay = builder.reg(Type::bits(32, false, false));
    const std::uint32_t value = builder.reg(Type::logic());
    const std::uint32_t inverted = builder.reg(Type::logic());
    const std::uint32_t five = builder.constant("32'd5");

    builder.block("wait");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(five)));
    builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    builder.block("toggle");
    builder.readObject(value, 0);
    builder.emit(Instruction::make(Opcode::Not, Operand::reg(inverted), Operand::reg(value)));
    builder.store(0, inverted);
    builder.emit(Instruction::make(Opcode::Jump, {}, Operand::block(0)));

    return designOf(makeTop(
        {builder.finish()},
        {Object{.name = "clk", .type = Type::logic(), .initialValue = BitVector::parse("1'b0")}}));
}

TEST(Kernel, AnInterruptStopsTheRunWhereItStands) {
    Simulation simulation(freeRunningClock(), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    CountingInterrupt always(1);
    simulation.simulationKernel().setInterrupt(&always);

    EXPECT_EQ(simulation.run(100), RunStatus::Interrupted);
    EXPECT_EQ(simulation.time(), 0U) << "the stop was asked for before any slot ran";
    EXPECT_EQ(always.asked, 1U);
}

TEST(Kernel, AnInterruptIsAskedOncePerTimeSlot) {
    Simulation simulation(freeRunningClock(), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    CountingInterrupt never(0);
    simulation.simulationKernel().setInterrupt(&never);

    EXPECT_EQ(simulation.run(100), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.time(), 100U);

    // Twenty toggles plus time zero and the final poll: the exact number matters less than
    // that the kernel asks often enough for a stop to take effect promptly.
    EXPECT_GE(never.asked, 20U);
}

TEST(Kernel, ClearingTheInterruptLetsTheRunFinish) {
    Simulation simulation(freeRunningClock(), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    CountingInterrupt always(1);
    simulation.simulationKernel().setInterrupt(&always);
    EXPECT_EQ(simulation.run(100), RunStatus::Interrupted);

    simulation.simulationKernel().setInterrupt(nullptr);
    EXPECT_EQ(simulation.run(100), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.time(), 100U);
}

TEST(Kernel, ResumingAfterAnInterruptMatchesAnUninterruptedRun) {
    // The invariant the interrupt exists to preserve. Results are bit-identical across runs of the
    // same build on the same inputs, and a GUI that could change a simulation by pausing it would
    // break that in the least debuggable way imaginable: the waveform would depend on when the user
    // clicked.
    Simulation straight(freeRunningClock(), "tb");
    ASSERT_TRUE(straight.ready()) << straight.messages().front();
    EXPECT_EQ(straight.run(100), RunStatus::TimeLimitReached);

    Simulation chopped(freeRunningClock(), "tb");
    ASSERT_TRUE(chopped.ready()) << chopped.messages().front();

    CountingInterrupt everyThird(3);
    chopped.simulationKernel().setInterrupt(&everyThird);

    RunStatus status = RunStatus::Interrupted;
    unsigned attempts = 0;
    while (status == RunStatus::Interrupted && attempts < 1000) {
        status = chopped.run(100);
        ++attempts;
    }

    EXPECT_EQ(status, RunStatus::TimeLimitReached);
    EXPECT_GT(attempts, 1U) << "the interrupt never fired, so nothing was tested";

    EXPECT_EQ(chopped.time(), straight.time());
    EXPECT_EQ(chopped.value("/tb/clk"), straight.value("/tb/clk"));

    // Not merely the same final value: the same amount of work, which is what says the run
    // was continued rather than partly redone or partly skipped.
    EXPECT_EQ(chopped.simulationKernel().executedInstructions(),
              straight.simulationKernel().executedInstructions());
    EXPECT_EQ(chopped.simulationKernel().totalDeltaCycles(),
              straight.simulationKernel().totalDeltaCycles());
}

// ---------------------------------------------------------------------------
// examine, deposit, force and release
// ---------------------------------------------------------------------------

/// The clock-generator design again, so that a force has something to fight with.
Design makeTogglingDesign() {
    ProcessBuilder builder(ProcessKind::Always, "clockgen");
    const std::uint32_t delay = builder.reg(Type::bits(32, false, false));
    const std::uint32_t value = builder.reg(Type::logic());
    const std::uint32_t inverted = builder.reg(Type::logic());
    const std::uint32_t five = builder.constant("32'd5");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(five)));
    builder.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    builder.block("toggle");
    builder.readObject(value, 0);
    builder.emit(Instruction::make(Opcode::Not, Operand::reg(inverted), Operand::reg(value)));
    builder.store(0, inverted);
    builder.emit(Instruction::make(Opcode::Jump, {}, Operand::block(0)));

    return designOf(makeTop(
        {builder.finish()},
        {Object{.name = "clk", .type = Type::logic(), .initialValue = BitVector::parse("1'b0")}}));
}

TEST(Kernel, ADepositLandsImmediately) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t clk = simulation.signal("/tb/clk");
    const BitVector one = literal("1'b1");
    simulation.simulationKernel().deposit(clk, one);

    EXPECT_EQ(simulation.value("/tb/clk"), "1");
}

TEST(Kernel, ADepositWakesWhatWatchesTheSignal) {
    Simulation simulation(makeClockedDesign(EdgeKind::Posedge, "1'b0", "1'b0"), "tb");
    ASSERT_TRUE(simulation.ready());

    ASSERT_EQ(simulation.run(0), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "0");

    const BitVector one = literal("1'b1");
    simulation.simulationKernel().deposit(simulation.signal("/tb/clk"), one);
    ASSERT_EQ(simulation.run(0), RunStatus::TimeLimitReached);

    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "1");
}

TEST(Kernel, ADepositIsOverwrittenByTheNextDrive) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    const BitVector one = literal("1'b1");
    simulation.simulationKernel().deposit(simulation.signal("/tb/clk"), one);
    ASSERT_EQ(simulation.run(5), RunStatus::TimeLimitReached);

    // The generator inverted what it found, which is the deposited value.
    EXPECT_EQ(simulation.value("/tb/clk"), "0");
}

TEST(Kernel, AForcedSignalIgnoresWhatDrivesIt) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t clk = simulation.signal("/tb/clk");
    const BitVector one = literal("1'b1");
    simulation.simulationKernel().force(clk, one);
    EXPECT_TRUE(simulation.simulationKernel().isForced(clk));
    EXPECT_EQ(simulation.value("/tb/clk"), "1");

    ASSERT_EQ(simulation.run(20), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), "1");
}

TEST(Kernel, AForcedSignalIgnoresADeposit) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t clk = simulation.signal("/tb/clk");
    const BitVector one = literal("1'b1");
    const BitVector zero = literal("1'b0");
    simulation.simulationKernel().force(clk, one);
    simulation.simulationKernel().deposit(clk, zero);

    EXPECT_EQ(simulation.value("/tb/clk"), "1");
}

TEST(Kernel, ForcingAgainChangesTheHeldValue) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t clk = simulation.signal("/tb/clk");
    const BitVector one = literal("1'b1");
    const BitVector zero = literal("1'b0");
    simulation.simulationKernel().force(clk, one);
    simulation.simulationKernel().force(clk, zero);

    EXPECT_EQ(simulation.value("/tb/clk"), "0");
    EXPECT_TRUE(simulation.simulationKernel().isForced(clk));
}

TEST(Kernel, AReleasedSignalIsDrivenAgain) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t clk = simulation.signal("/tb/clk");
    const BitVector one = literal("1'b1");
    simulation.simulationKernel().force(clk, one);
    ASSERT_EQ(simulation.run(20), RunStatus::TimeLimitReached);

    EXPECT_TRUE(simulation.simulationKernel().release(clk));
    EXPECT_FALSE(simulation.simulationKernel().isForced(clk));

    // The value stays where the force left it until something drives the signal, which the
    // generator does at the next multiple of five.
    EXPECT_EQ(simulation.value("/tb/clk"), "1");
    ASSERT_EQ(simulation.run(25), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), "0");
}

TEST(Kernel, ReleasingASignalThatWasNeverForcedSaysSo) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    EXPECT_FALSE(simulation.simulationKernel().release(simulation.signal("/tb/clk")));
}

TEST(Kernel, NothingIsForcedBeforeAnythingIsForced) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    EXPECT_FALSE(simulation.simulationKernel().isForced(simulation.signal("/tb/clk")));
}

TEST(Kernel, AForceDoesNotSurviveReinitialisation) {
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t clk = simulation.signal("/tb/clk");
    const BitVector one = literal("1'b1");
    simulation.simulationKernel().force(clk, one);

    simulation.simulationKernel().initialize();
    EXPECT_FALSE(simulation.simulationKernel().isForced(clk));
}

TEST(Kernel, AForcedSignalIgnoresANonBlockingUpdate) {
    Simulation simulation(makeClockedDesign(EdgeKind::Posedge, "1'b0", "1'b1"), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t count = simulation.signal("/tb/count");
    const BitVector held = literal("8'd99");
    simulation.simulationKernel().force(count, held);

    ASSERT_EQ(simulation.run(20), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/count", Radix::Decimal), "99");
}

// ---------------------------------------------------------------------------
// restart
// ---------------------------------------------------------------------------

TEST(Kernel, ReinitialisingRunsTheDesignAgainIdentically) {
    // `restart` is `initialize` called a second time. A design put back to time zero must
    // behave exactly as it did the first time, which means nothing the first run left in the
    // time wheel, in a process's resume point or in an edge subscription may survive.
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    ASSERT_EQ(simulation.run(12), RunStatus::TimeLimitReached);
    const std::string afterFirst = simulation.value("/tb/clk");

    simulation.simulationKernel().initialize();
    EXPECT_EQ(simulation.time(), 0U);
    EXPECT_EQ(simulation.value("/tb/clk"), "0") << "the declared initial value came back";

    ASSERT_EQ(simulation.run(12), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), afterFirst);
}

TEST(Kernel, ReinitialisingClearsAFinishedRun) {
    ProcessBuilder builder(ProcessKind::Initial, "stopper");
    SysTaskCall call;
    call.kind = SysTaskKind::Finish;
    const std::uint32_t descriptor = builder.sysCall(std::move(call));

    builder.block("entry");
    builder.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready());
    ASSERT_EQ(simulation.run(), RunStatus::Finished);

    simulation.simulationKernel().initialize();
    EXPECT_EQ(simulation.run(), RunStatus::Finished) << "the design stopped a second time";
}

TEST(Kernel, ReinitialisingEmptiesTheHeap) {
    // A line the first run left allocated is gone with the run, so the second allocates the same
    // handles the first did.
    Simulation simulation(makeLineWriter(), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    ASSERT_EQ(simulation.value("/tb/after", Radix::Decimal), "1");

    simulation.simulationKernel().initialize();
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/after", Radix::Decimal), "1");
}

TEST(Kernel, ReinitialisingAfterASuspensionDoesNotResumeTheOldRun) {
    // The clock generator suspends inside `WaitTime`. If its resume point survived the
    // restart it would continue mid-body, toggling on a schedule the new run never set up.
    Simulation simulation(makeTogglingDesign(), "tb");
    ASSERT_TRUE(simulation.ready());

    ASSERT_EQ(simulation.run(7), RunStatus::TimeLimitReached);
    simulation.simulationKernel().initialize();

    // The first edge is at five, not at whatever was left over from before.
    ASSERT_EQ(simulation.run(4), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), "0");
    ASSERT_EQ(simulation.run(5), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.value("/tb/clk"), "1");
}

// ---------------------------------------------------------------------------
// PLI hooks
// ---------------------------------------------------------------------------

/// A PLI application reduced to what the kernel sees of one: queues of things to run at the
/// points the kernel's hooks name, plus a log of when each point was reached.
class ScriptedHooks final : public PliHooks {
public:
    using Action = std::function<void()>;

    void valueChanged(std::uint32_t signalIndex) override {
        if (onValueChange) onValueChange(signalIndex);
    }

    [[nodiscard]] SimTime nextTimedEvent() const override {
        return timed.empty() ? NoTime : timed.begin()->first;
    }

    void timeAdvanced(SimTime time) override { log.push_back("advance " + std::to_string(time)); }

    void runTimedEvents(SimTime time) override {
        // Taken out before running, so that one scheduling another for the same time is
        // run on the kernel's next pass rather than inside this one.
        std::vector<Action> due;
        while (!timed.empty() && timed.begin()->first <= time) {
            due.push_back(std::move(timed.begin()->second));
            timed.erase(timed.begin());
        }
        for (const Action& action : due) action();
    }

    [[nodiscard]] bool hasReadWrite() const override { return !readWrite.empty(); }

    void runReadWrite() override { runAll(readWrite); }

    [[nodiscard]] bool hasReadOnly() const override { return !readOnly.empty(); }

    void runReadOnly() override { runAll(readOnly); }

    std::function<void(std::uint32_t)> onValueChange;
    std::multimap<SimTime, Action> timed;
    std::vector<Action> readWrite;
    std::vector<Action> readOnly;
    std::vector<std::string> log;

private:
    static void runAll(std::vector<Action>& actions) {
        std::vector<Action> taken;
        taken.swap(actions);
        for (const Action& action : taken) action();
    }
};

/// A process that copies `a` into `b` whenever `a` changes.
Process copyAToB() {
    ProcessBuilder builder(ProcessKind::AlwaysComb, "copy");
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    builder.block("entry");
    builder.readObject(value, 0);
    builder.store(1, value);
    builder.emit(Instruction::make(Opcode::Return));
    return builder.finish();
}

TEST(KernelPli, APendingTimedEventKeepsAQuietDesignRunning) {
    // A design that schedules nothing of its own is exactly what a Python testbench drives,
    // and it must not be declared finished while the testbench still has a timer running.
    Simulation simulation(designOf(makeTop({})), "tb");
    ASSERT_TRUE(simulation.ready());

    ScriptedHooks hooks;
    const std::uint32_t a = simulation.signal("/tb/a");
    hooks.timed.emplace(40, [&] { simulation.simulationKernel().deposit(a, literal("8'h5A")); });
    simulation.simulationKernel().setPliHooks(&hooks);

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.time(), 40U);
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "5a");
}

TEST(KernelPli, CallbacksFireWhereSection93PutsThem) {
    // At one timestamp: time advances, the timed event runs before the process released at
    // that time, the clock changes, and only then do read-write and read-only run.
    Simulation simulation(freeRunningClock(), "tb");
    ASSERT_TRUE(simulation.ready());

    ScriptedHooks hooks;
    const std::uint32_t clk = simulation.signal("/tb/clk");
    hooks.onValueChange = [&](std::uint32_t signal) {
        if (signal == clk) hooks.log.push_back("change clk=" + simulation.value("/tb/clk"));
    };
    hooks.timed.emplace(5, [&] {
        hooks.log.push_back("timed clk=" + simulation.value("/tb/clk"));
        hooks.readWrite.emplace_back(
            [&] { hooks.log.emplace_back("read-write clk=" + simulation.value("/tb/clk")); });
        hooks.readOnly.emplace_back([&] { hooks.log.emplace_back("read-only"); });
    });
    simulation.simulationKernel().setPliHooks(&hooks);

    ASSERT_EQ(simulation.run(5), RunStatus::TimeLimitReached);
    const std::vector<std::string> expected = {"advance 5", "timed clk=0", "change clk=1",
                                               "read-write clk=1", "read-only"};
    EXPECT_EQ(hooks.log, expected);
}

TEST(KernelPli, WhatAReadWriteCallbackWritesSettlesBeforeReadOnly) {
    Simulation simulation(designOf(makeTop({copyAToB()})), "tb");
    ASSERT_TRUE(simulation.ready());

    ScriptedHooks hooks;
    const std::uint32_t a = simulation.signal("/tb/a");
    std::string seenByReadOnly;
    hooks.readWrite.emplace_back(
        [&] { simulation.simulationKernel().deposit(a, literal("8'h42")); });
    hooks.readOnly.emplace_back(
        [&] { seenByReadOnly = simulation.value("/tb/b", Radix::Hexadecimal); });
    simulation.simulationKernel().setPliHooks(&hooks);

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(seenByReadOnly, "42") << "the copy ran before the read-only phase";
}

TEST(KernelPli, ReadWriteAskingForReadWriteForeverIsADeltaLoop) {
    Simulation::deltaLimit = 16;
    Simulation simulation(designOf(makeTop({})), "tb");
    Simulation::deltaLimit = 10'000;
    ASSERT_TRUE(simulation.ready());

    ScriptedHooks hooks;
    std::function<void()> again = [&] { hooks.readWrite.push_back(again); };
    hooks.readWrite.push_back(again);
    simulation.simulationKernel().setPliHooks(&hooks);

    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::DeltaLimitExceeded));
}

TEST(KernelPli, AnUpdateScheduledWhileUpdatesAreAppliedJoinsTheNextPass) {
    // A value-change callback runs while the NBA batch is being applied, and one that writes
    // with an inertial delay appends to the very list being applied. The second write must
    // land, and the first batch must not be disturbed by it.
    ProcessBuilder builder(ProcessKind::Initial, "nba");
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    const std::uint32_t constant = builder.constant("8'h11");
    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(constant)));
    builder.storeNonBlocking(0, value);
    builder.emit(Instruction::make(Opcode::Return));

    Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    ScriptedHooks hooks;
    const std::uint32_t a = simulation.signal("/tb/a");
    const std::uint32_t b = simulation.signal("/tb/b");
    const BitVector written = literal("8'h22");
    hooks.onValueChange = [&](std::uint32_t signal) {
        if (signal == a) simulation.simulationKernel().scheduleNonBlocking(b, written);
    };
    simulation.simulationKernel().setPliHooks(&hooks);

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "11");
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "22");
}

TEST(KernelPli, AFinishFromACallbackEndsTheRunThere) {
    Simulation simulation(freeRunningClock(), "tb");
    ASSERT_TRUE(simulation.ready());

    ScriptedHooks hooks;
    hooks.timed.emplace(12, [&] { simulation.simulationKernel().requestFinish(); });
    simulation.simulationKernel().setPliHooks(&hooks);

    EXPECT_EQ(simulation.run(100), RunStatus::Finished);
    EXPECT_EQ(simulation.time(), 12U);
    EXPECT_EQ(simulation.simulationKernel().exitStatus(), 0);
}

TEST(KernelPli, AFinishAskedForBeforeTheRunEndsItBeforeAnythingRuns) {
    // What an application does from `cbStartOfSimulation` when it has nothing to test.
    Simulation simulation(freeRunningClock(), "tb");
    ASSERT_TRUE(simulation.ready());

    simulation.simulationKernel().requestFinish();
    EXPECT_EQ(simulation.run(100), RunStatus::Finished);
    EXPECT_EQ(simulation.time(), 0U);
    EXPECT_EQ(simulation.simulationKernel().executedInstructions(), 0U);
}

TEST(KernelPli, AStopFromACallbackPausesTheRunAndResumingMatchesAStraightRun) {
    Simulation straight(freeRunningClock(), "tb");
    ASSERT_TRUE(straight.ready());
    ASSERT_EQ(straight.run(100), RunStatus::TimeLimitReached);

    Simulation stopped(freeRunningClock(), "tb");
    ASSERT_TRUE(stopped.ready());

    ScriptedHooks hooks;
    hooks.timed.emplace(12, [&] { stopped.simulationKernel().requestStop(); });
    stopped.simulationKernel().setPliHooks(&hooks);

    EXPECT_EQ(stopped.run(100), RunStatus::Interrupted);
    EXPECT_LT(stopped.time(), 100U);
    EXPECT_GE(stopped.time(), 12U);

    EXPECT_EQ(stopped.run(100), RunStatus::TimeLimitReached);
    EXPECT_EQ(stopped.value("/tb/clk"), straight.value("/tb/clk"));
    EXPECT_EQ(stopped.simulationKernel().executedInstructions(),
              straight.simulationKernel().executedInstructions());
}

TEST(KernelPli, AStopTheRunEndedBeforeReachingDoesNotStopTheNextRun) {
    // Asked for at 12, with the next edge at 15 past the limit: the run ends at its limit, and
    // the stop it never reached ends with it rather than cutting the next run short at once.
    Simulation simulation(freeRunningClock(), "tb");
    ASSERT_TRUE(simulation.ready());

    ScriptedHooks hooks;
    hooks.timed.emplace(12, [&] { simulation.simulationKernel().requestStop(); });
    simulation.simulationKernel().setPliHooks(&hooks);

    ASSERT_EQ(simulation.run(14), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.run(30), RunStatus::TimeLimitReached);
    EXPECT_EQ(simulation.time(), 30U);
}

TEST(KernelPli, AZeroDelayEventFromReadOnlyRunsInAFurtherSlotAtTheSameTime) {
    // Read-only is the end of a slot, so something scheduled from it with no delay cannot run
    // in that slot. It runs in another at the same time, and time is not said to advance.
    Simulation simulation(designOf(makeTop({})), "tb");
    ASSERT_TRUE(simulation.ready());

    ScriptedHooks hooks;
    hooks.timed.emplace(10, [&] {
        hooks.readOnly.emplace_back([&] {
            hooks.log.emplace_back("read-only");
            hooks.timed.emplace(10, [&] { hooks.log.emplace_back("after read-only"); });
        });
    });
    simulation.simulationKernel().setPliHooks(&hooks);

    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.time(), 10U);
    const std::vector<std::string> expected = {"advance 10", "read-only", "after read-only"};
    EXPECT_EQ(hooks.log, expected);
}

TEST(Kernel, AProcessIsNotWokenByWhatItWritesWhileItRuns) {
    // `always @* begin t = 0; t = x; y = t; end`: the inferred sensitivity includes the
    // temporary, which the process changes twice on every run. A process is not waiting on its
    // event control while it runs, so those changes must not schedule it again; if they did,
    // the time slot would never end.
    ProcessBuilder builder(ProcessKind::AlwaysComb, "temporaries");
    const std::uint32_t value = builder.reg(Type::bits(8, false, true));
    const std::uint32_t zero = builder.constant("8'd0");
    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(zero)));
    builder.store(1, value);
    builder.readObject(value, 0);
    builder.store(1, value);
    builder.readObject(value, 1);
    builder.store(2, value);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop(
        {builder.finish()},
        {Object{.name = "x", .type = Type::bits(8, false, true), .initialValue = literal("8'd7")},
         Object{.name = "t", .type = Type::bits(8, false, true)},
         Object{.name = "y", .type = Type::bits(8, false, true)}});

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/y", Radix::Decimal), "7");

    // It runs once at time zero, and once more for each change of what it reads from outside.
    const std::uint64_t once = simulation.simulationKernel().executedInstructions();
    simulation.simulationKernel().deposit(simulation.signal("/tb/x"), literal("8'd9"));
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/y", Radix::Decimal), "9");
    EXPECT_EQ(simulation.simulationKernel().executedInstructions(), 2 * once);
}

// ---------------------------------------------------------------------------
// Reading a window of an object
// ---------------------------------------------------------------------------

/// `result = source[offset +: 8]`, read with ReadObjectPart, with `offset` as given.
Simulation readWindowAt(std::string_view offset) {
    ProcessBuilder builder(ProcessKind::Initial, "window");
    const std::uint32_t window = builder.reg(Type::bits(8, false, true));
    const std::uint32_t at = builder.reg(Type::bits(32, false, true));
    const std::uint32_t offsetConstant = builder.constant(offset);
    builder.block("entry");
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(at),
                                   Operand::constant(offsetConstant)));
    builder.readObjectPart(window, 0, at);
    builder.store(1, window);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition =
        makeTop({builder.finish()}, {Object{.name = "source",
                                            .type = Type::bits(16, false, true),
                                            .initialValue = literal("16'hA55A")},
                                     Object{.name = "result", .type = Type::bits(8, false, true)}});
    return {designOf(std::move(definition)), "tb"};
}

TEST(Kernel, AWindowOfAnObjectIsReadAtItsOffset) {
    Simulation simulation = readWindowAt("32'd4");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/result", Radix::Hexadecimal), "55");
}

TEST(Kernel, AWindowPastTheEndOfAnObjectReadsXThere) {
    Simulation simulation = readWindowAt("32'd12");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/result"), "xxxx1010");
}

TEST(Kernel, AWindowAtAnUnknownOffsetIsX) {
    Simulation simulation = readWindowAt("32'bx");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/result"), "xxxxxxxx");
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

/// A continuous assignment of all of one one-bit object to another.
Process continuousCopy(std::uint32_t from, std::uint32_t to) {
    ProcessBuilder builder(ProcessKind::ContinuousAssign, "assign");
    const std::uint32_t value = builder.reg(Type::logic());
    builder.block("entry");
    builder.readObject(value, from);
    builder.store(to, value);
    builder.emit(Instruction::make(Opcode::Return));
    return builder.finish();
}

/// `wire n; assign n = a; assign n = b;`, with `a` and `b` starting at the given values.
Simulation twoDrivers(std::string_view a, std::string_view b) {
    ModuleDefinition definition =
        makeTop({continuousCopy(0, 2), continuousCopy(1, 2)},
                {Object{.name = "a", .type = Type::logic(), .initialValue = literal(a)},
                 Object{.name = "b", .type = Type::logic(), .initialValue = literal(b)},
                 Object{.name = "n", .type = Type::logic(), .kind = enzan::eir::ObjectKind::Net}});
    return {designOf(std::move(definition)), "tb"};
}

TEST(Kernel, ADrivenNetIsXUntilItsDriverRunsAndAnUndrivenOneIsItsDefault) {
    // Before anything runs: a driven bit holds no value the design gave it, so X, as a variable
    // would; `z` would claim nothing drives it.
    const Simulation driven = twoDrivers("1'b1", "1'bz");
    ASSERT_TRUE(driven.ready()) << driven.messages().front();
    EXPECT_EQ(driven.value("/tb/n"), "x");

    ModuleDefinition definition = makeTop({}, {Object{.name = "n",
                                                      .type = Type::bits(4, false, true),
                                                      .kind = enzan::eir::ObjectKind::Net}});
    const Simulation undriven(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(undriven.ready());
    EXPECT_EQ(undriven.value("/tb/n"), "zzzz");
}

TEST(Kernel, TwoDriversOfANetResolveAndReResolveWhenEitherChanges) {
    Simulation simulation = twoDrivers("1'b1", "1'bz");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();

    simulation.run();
    EXPECT_EQ(simulation.value("/tb/n"), "1") << "z yields to the other driver";

    simulation.simulationKernel().deposit(simulation.signal("/tb/b"), literal("1'b0"));
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/n"), "x") << "two strong drivers disagreeing are X";

    simulation.simulationKernel().deposit(simulation.signal("/tb/a"), literal("1'bz"));
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/n"), "0");
}

TEST(Kernel, ADepositOnAResolvedNetHoldsOnlyUntilADriverChanges) {
    // A deposit writes the net, not a driver, so the next driver update resolves it again from
    // what the drivers drive. That is what `vpi_put_value` without a force means, and what a
    // cocotb test writing to a bus that the design also drives sees.
    Simulation simulation = twoDrivers("1'b1", "1'bz");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();

    simulation.simulationKernel().deposit(simulation.signal("/tb/n"), literal("1'b0"));
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/n"), "0");

    simulation.simulationKernel().deposit(simulation.signal("/tb/b"), literal("1'b1"));
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/n"), "1");
}

TEST(Kernel, AReleaseResolvesAgainWhatTheDriversOfAForcedNetDrive) {
    // The drivers keep their values while the force stands, so a release from outside resolves
    // them at once, and the net does not wait for one of them to change.
    Simulation simulation = twoDrivers("1'b1", "1'bz");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();

    Kernel& kernel = simulation.simulationKernel();
    const std::uint32_t net = simulation.signal("/tb/n");
    kernel.force(net, literal("1'b0"));
    kernel.deposit(simulation.signal("/tb/a"), literal("1'bz"));
    kernel.deposit(simulation.signal("/tb/b"), literal("1'b1"));
    simulation.run();
    EXPECT_EQ(simulation.value("/tb/n"), "0");

    EXPECT_TRUE(kernel.release(net));
    EXPECT_EQ(simulation.value("/tb/n"), "1");
}

TEST(Kernel, AReleasedUndrivenNetGoesBackToItsKindsValue) {
    ModuleDefinition definition = makeTop({}, {Object{.name = "n",
                                                      .type = Type::bits(2, false, true),
                                                      .kind = enzan::eir::ObjectKind::Net,
                                                      .netKind = enzan::eir::NetKind::Tri1}});
    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready());

    const std::uint32_t net = simulation.signal("/tb/n");
    simulation.simulationKernel().force(net, literal("2'b00"));
    EXPECT_EQ(simulation.value("/tb/n"), "00");
    EXPECT_TRUE(simulation.simulationKernel().release(net));
    EXPECT_EQ(simulation.value("/tb/n"), "11");
}

// ---------------------------------------------------------------------------
// Calls of subprograms
// ---------------------------------------------------------------------------

/// Every message a simulation reported, one per line, for a failed assertion to show.
std::string reportedMessages(const Simulation& simulation) {
    std::string text;
    for (const std::string& message : simulation.messages()) text += message + '\n';
    return text;
}

/// `double(value)`: an 8-bit function returning its parameter twice over.
Subprogram makeDoubler() {
    SubprogramBuilder builder("double");
    const std::uint32_t value = builder.parameter("value", Type::bits(8, false, true));
    const std::uint32_t result = builder.result(Type::bits(8, false, true));
    builder.block("entry");
    builder.emit(Instruction::make(Opcode::Add, Operand::reg(result), Operand::reg(value),
                                   Operand::reg(value)));
    builder.emit(Instruction::make(Opcode::Return));
    return builder.finish();
}

TEST(Kernel, AFunctionsResultReachesItsCaller) {
    ProcessBuilder builder(ProcessKind::Initial, "caller");
    const std::uint32_t argument = builder.reg(Type::bits(8, false, true));
    const std::uint32_t result = builder.reg(Type::bits(8, false, true));
    const std::uint32_t value = builder.constant("8'h15");
    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(argument), Operand::constant(value)));
    builder.call("", "double", {Operand::reg(argument)}, Operand::reg(result));
    builder.store(1, result);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.subprograms.push_back(makeDoubler());

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "2a");
}

TEST(Kernel, OutParametersAreCopiedBackAndInOutOnesInAsWell) {
    // `accumulate(total, step)` adds to an inout parameter, so what it starts from is the
    // caller's; `produce(value)` sets an out one, so the caller's value never reaches it.
    SubprogramBuilder accumulate("accumulate");
    const std::uint32_t total =
        accumulate.parameter("total", Type::bits(8, false, true), ParameterMode::InOut);
    const std::uint32_t step = accumulate.parameter("step", Type::bits(8, false, true));
    const std::uint32_t sum = accumulate.reg(Type::bits(8, false, true));
    accumulate.block("entry");
    accumulate.emit(
        Instruction::make(Opcode::Add, Operand::reg(sum), Operand::reg(total), Operand::reg(step)));
    accumulate.emit(Instruction::make(Opcode::Move, Operand::reg(total), Operand::reg(sum)));
    accumulate.emit(Instruction::make(Opcode::Return));

    SubprogramBuilder produce("produce");
    const std::uint32_t produced =
        produce.parameter("value", Type::bits(8, false, true), ParameterMode::Out);
    const std::uint32_t nine = produce.constant("8'h09");
    produce.block("entry");
    produce.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(produced), Operand::constant(nine)));
    produce.emit(Instruction::make(Opcode::Return));

    ProcessBuilder builder(ProcessKind::Initial, "caller");
    const std::uint32_t running = builder.reg(Type::bits(8, false, true));
    const std::uint32_t other = builder.reg(Type::bits(8, false, true));
    const std::uint32_t five = builder.constant("8'h05");
    const std::uint32_t three = builder.constant("8'h03");
    const std::uint32_t junk = builder.constant("8'h55");
    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(running), Operand::constant(five)));
    builder.call("", "accumulate", {Operand::reg(running), Operand::constant(three)});
    builder.call("", "accumulate", {Operand::reg(running), Operand::constant(three)});
    builder.store(0, running);
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(other), Operand::constant(junk)));
    builder.call("", "produce", {Operand::reg(other)});
    builder.store(1, other);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.subprograms.push_back(accumulate.finish());
    definition.subprograms.push_back(produce.finish());

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/a", Radix::Hexadecimal), "0b");
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "09");
}

/// `factorial(n)`, recursively, in 16 bits.
Subprogram makeFactorial() {
    SubprogramBuilder builder("factorial");
    const Type word = Type::bits(16, false, true);
    const std::uint32_t n = builder.parameter("n", word);
    const std::uint32_t result = builder.result(word);
    const std::uint32_t one = builder.reg(word);
    const std::uint32_t atMostOne = builder.reg(Type::bits(1, false, true));
    const std::uint32_t smaller = builder.reg(word);
    const std::uint32_t product = builder.reg(word);
    const std::uint32_t oneValue = builder.constant("16'd1");

    builder.block("entry");
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(one), Operand::constant(oneValue)));
    builder.emit(Instruction::make(Opcode::LessEqual, Operand::reg(atMostOne), Operand::reg(n),
                                   Operand::reg(one)));
    builder.emit(Instruction::make(Opcode::BranchIf, {}, Operand::reg(atMostOne), Operand::block(1),
                                   Operand::block(2)));

    builder.block("base");
    builder.emit(Instruction::make(Opcode::Move, Operand::reg(result), Operand::reg(one)));
    builder.emit(Instruction::make(Opcode::Return));

    builder.block("recurse");
    builder.emit(Instruction::make(Opcode::Subtract, Operand::reg(smaller), Operand::reg(n),
                                   Operand::reg(one)));
    builder.call("", "factorial", {Operand::reg(smaller)}, Operand::reg(product));
    builder.emit(Instruction::make(Opcode::Multiply, Operand::reg(result), Operand::reg(n),
                                   Operand::reg(product)));
    builder.emit(Instruction::make(Opcode::Return));
    return builder.finish();
}

TEST(Kernel, ARecursiveFunctionCallsItself) {
    ProcessBuilder builder(ProcessKind::Initial, "caller");
    const std::uint32_t n = builder.reg(Type::bits(16, false, true));
    const std::uint32_t result = builder.reg(Type::bits(16, false, true));
    const std::uint32_t five = builder.constant("16'd5");
    builder.block("entry");
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(n), Operand::constant(five)));
    builder.call("", "factorial", {Operand::reg(n)}, Operand::reg(result));
    builder.store(1, result);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition =
        makeTop({builder.finish()}, {Object{.name = "a", .type = Type::bits(16, false, true)},
                                     Object{.name = "b", .type = Type::bits(16, false, true)}});
    definition.subprograms.push_back(makeFactorial());

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/b", Radix::Decimal), "120");
}

TEST(Kernel, RealsComputeAsDoublesAndRoundHalfwayAwayFromZero) {
    // 1.5 + real(2) is 3.5, which rounds to 4 and its negation to -4; 2.0 ** 3 is 8.0; and 1.5 is
    // less than 2.0.
    ProcessBuilder builder(ProcessKind::Initial, "reals");
    const Type real = Type::real();
    const Type integer = Type::bits(32, true, false);
    const std::uint32_t oneAndAHalf = builder.reg(real);
    const std::uint32_t two = builder.reg(integer);
    const std::uint32_t three = builder.reg(integer);
    const std::uint32_t twoReal = builder.reg(real);
    const std::uint32_t sum = builder.reg(real);
    const std::uint32_t negated = builder.reg(real);
    const std::uint32_t cubed = builder.reg(real);
    const std::uint32_t rounded = builder.reg(integer);
    const std::uint32_t roundedNegated = builder.reg(integer);
    const std::uint32_t cubedInteger = builder.reg(integer);
    const std::uint32_t less = builder.reg(Type::bits(1, false, false));
    const std::uint32_t oneAndAHalfValue = builder.constant("64'h3FF8000000000000");
    const std::uint32_t twoValue = builder.constant("32'sd2");
    const std::uint32_t threeValue = builder.constant("32'sd3");

    builder.block("entry");
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(oneAndAHalf),
                                   Operand::constant(oneAndAHalfValue)));
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(two), Operand::constant(twoValue)));
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(three),
                                   Operand::constant(threeValue)));
    Instruction toReal =
        Instruction::make(Opcode::Convert, Operand::reg(twoReal), Operand::reg(two));
    toReal.isSigned = true;
    builder.emit(toReal);
    builder.emit(Instruction::make(Opcode::Add, Operand::reg(sum), Operand::reg(oneAndAHalf),
                                   Operand::reg(twoReal)));
    builder.emit(Instruction::make(Opcode::Negate, Operand::reg(negated), Operand::reg(sum)));
    builder.emit(Instruction::make(Opcode::Power, Operand::reg(cubed), Operand::reg(twoReal),
                                   Operand::reg(three), Operand::immediate(1)));
    builder.emit(Instruction::make(Opcode::Convert, Operand::reg(rounded), Operand::reg(sum)));
    builder.emit(
        Instruction::make(Opcode::Convert, Operand::reg(roundedNegated), Operand::reg(negated)));
    builder.emit(
        Instruction::make(Opcode::Convert, Operand::reg(cubedInteger), Operand::reg(cubed)));
    builder.emit(Instruction::make(Opcode::LessThan, Operand::reg(less), Operand::reg(oneAndAHalf),
                                   Operand::reg(twoReal)));
    builder.store(0, rounded);
    builder.store(1, roundedNegated);
    builder.store(2, cubedInteger);
    builder.store(3, less);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition =
        makeTop({builder.finish()},
                {Object{.name = "a", .type = integer}, Object{.name = "b", .type = integer},
                 Object{.name = "c", .type = integer},
                 Object{.name = "d", .type = Type::bits(1, false, false)}});
    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/a", Radix::Decimal), "4");
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "fffffffc") << "-4";
    EXPECT_EQ(simulation.value("/tb/c", Radix::Decimal), "8");
    EXPECT_EQ(simulation.value("/tb/d", Radix::Decimal), "1");
}

TEST(Kernel, CodesArePresentedAsBitsAndBitsEncodedAsCodes) {
    // What crosses a language boundary: std_ulogic's '1', '0', 'H' and 'Z' as the bits 1 0 1 z,
    // and the bits 1 0 x z as std_ulogic's '1', '0', 'X' and 'Z' and as bits' 1, 0, 0 and 0; and
    // driven weakly, as 'H', 'L', 'W' and 'Z', or with only the 1 weak, as 'H', '0', 'X' and 'Z'.
    ProcessBuilder builder(ProcessKind::Initial, "boundary");
    const Type codes = Type::bits(32, false, false);
    const Type bits = Type::bits(4, false, true);
    const std::uint32_t logicCodes = builder.reg(codes);
    const std::uint32_t presented = builder.reg(bits);
    const std::uint32_t shown = builder.reg(bits);
    const std::uint32_t asLogic = builder.reg(codes);
    const std::uint32_t asBit = builder.reg(codes);
    const std::uint32_t asWeak = builder.reg(codes);
    const std::uint32_t asWeakOne = builder.reg(codes);
    const auto encoding = [](enzan::eir::Encoding which) {
        return Operand::immediate(static_cast<std::uint32_t>(which));
    };

    builder.block("entry");
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(logicCodes),
                                   Operand::constant(builder.constant("32'h03020704"))));
    builder.emit(Instruction::make(Opcode::Present, Operand::reg(presented),
                                   Operand::reg(logicCodes),
                                   encoding(enzan::eir::Encoding::VhdlLogic)));
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(shown),
                                   Operand::constant(builder.constant("4'b10xz"))));
    builder.emit(Instruction::make(Opcode::Encode, Operand::reg(asLogic), Operand::reg(shown),
                                   encoding(enzan::eir::Encoding::VhdlLogic),
                                   Operand::immediate(0)));
    builder.emit(Instruction::make(Opcode::Encode, Operand::reg(asBit), Operand::reg(shown),
                                   encoding(enzan::eir::Encoding::VhdlBit), Operand::immediate(0)));
    builder.emit(Instruction::make(Opcode::Encode, Operand::reg(asWeak), Operand::reg(shown),
                                   encoding(enzan::eir::Encoding::VhdlLogic),
                                   Operand::immediate(enzan::eir::WeakZero | enzan::eir::WeakOne)));
    builder.emit(Instruction::make(Opcode::Encode, Operand::reg(asWeakOne), Operand::reg(shown),
                                   encoding(enzan::eir::Encoding::VhdlLogic),
                                   Operand::immediate(enzan::eir::WeakOne)));
    builder.store(0, presented);
    builder.store(1, asLogic);
    builder.store(2, asBit);
    builder.store(3, asWeak);
    builder.store(4, asWeakOne);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop(
        {builder.finish()}, {Object{.name = "a", .type = bits}, Object{.name = "b", .type = codes},
                             Object{.name = "c", .type = codes}, Object{.name = "d", .type = codes},
                             Object{.name = "e", .type = codes}});
    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/a", Radix::Binary), "101z");
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "03020104");
    EXPECT_EQ(simulation.value("/tb/c", Radix::Hexadecimal), "01000000");
    EXPECT_EQ(simulation.value("/tb/d", Radix::Hexadecimal), "07060504");
    EXPECT_EQ(simulation.value("/tb/e", Radix::Hexadecimal), "07020104");
}

TEST(Kernel, OneWordOperationsMatchTheGeneralRoutines) {
    // The interpreter computes the commonest operations on values of one word itself and leaves
    // the rest to eir::bitops, so each must give what the other would. Every operation runs on
    // operands around the edges of their widths, and each result is held against what bitops
    // computes from the same operands.
    namespace bitops = enzan::eir::bitops;
    ProcessBuilder builder(ProcessKind::Initial, "operations");
    builder.block("entry");
    std::vector<Object> results;
    std::vector<BitVector> expected;

    const auto load = [&builder](const BitVector& value) {
        const std::uint32_t loaded = builder.reg(Type::bits(value.width(), false, true));
        const std::string text =
            std::format("{}'b{}", value.width(), toString(value.ref(), enzan::eir::Radix::Binary));
        builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(loaded),
                                       Operand::constant(builder.constant(text))));
        return loaded;
    };
    // Runs `opcode` into a register of `value`'s width and stores it, expecting `value`.
    const auto check = [&](Opcode opcode, bool isSigned, BitVector value,
                           std::initializer_list<std::uint32_t> operands) {
        const std::uint32_t destination = builder.reg(Type::bits(value.width(), false, true));
        std::vector<Operand> sources;
        for (const std::uint32_t operand : operands) sources.push_back(Operand::reg(operand));
        Instruction instruction =
            sources.size() == 1
                ? Instruction::make(opcode, Operand::reg(destination), sources[0])
                : Instruction::make(opcode, Operand::reg(destination), sources[0], sources[1]);
        instruction.isSigned = isSigned;
        builder.emit(instruction);
        const auto index = static_cast<std::uint32_t>(results.size());
        results.push_back(Object{.name = std::format("r{}", index),
                                 .type = Type::bits(value.width(), false, true)});
        builder.store(index, destination);
        expected.push_back(std::move(value));
    };

    for (const std::uint32_t width : {1U, 5U, 32U, 63U, 64U}) {
        const std::uint64_t all = width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
        const std::uint64_t signBit = std::uint64_t{1} << (width - 1);
        std::vector<BitVector> values;
        for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, all, signBit,
                                          signBit - 1, std::uint64_t{0x5A3C96E1F00F7B2D}})
            values.push_back(BitVector::fromUInt64(value & all, width));

        for (const BitVector& a : values) {
            for (const BitVector& b : values) {
                const std::uint32_t left = load(a);
                const std::uint32_t right = load(b);
                for (const bool isSigned : {false, true}) {
                    const auto computed = [&](auto operation, std::uint32_t resultWidth) {
                        BitVector value(resultWidth);
                        operation(value.mutableRef());
                        return value;
                    };
                    const auto arithmetic = [&](Opcode opcode, auto operation) {
                        check(opcode, isSigned, computed(operation, width), {left, right});
                    };
                    const auto comparison = [&](Opcode opcode, auto operation) {
                        check(opcode, isSigned, computed(operation, 1), {left, right});
                    };
                    arithmetic(Opcode::Add, [&](auto dst) { bitops::add(dst, a.ref(), b.ref()); });
                    arithmetic(Opcode::Subtract,
                               [&](auto dst) { bitops::subtract(dst, a.ref(), b.ref()); });
                    arithmetic(Opcode::Multiply,
                               [&](auto dst) { bitops::multiply(dst, a.ref(), b.ref()); });
                    arithmetic(Opcode::And,
                               [&](auto dst) { bitops::bitwiseAnd(dst, a.ref(), b.ref()); });
                    arithmetic(Opcode::Or,
                               [&](auto dst) { bitops::bitwiseOr(dst, a.ref(), b.ref()); });
                    arithmetic(Opcode::Xor,
                               [&](auto dst) { bitops::bitwiseXor(dst, a.ref(), b.ref()); });
                    arithmetic(Opcode::Divide,
                               [&](auto dst) { bitops::divide(dst, a.ref(), b.ref(), isSigned); });
                    arithmetic(Opcode::Modulo,
                               [&](auto dst) { bitops::modulo(dst, a.ref(), b.ref(), isSigned); });
                    comparison(Opcode::Equal,
                               [&](auto dst) { bitops::equal(dst, a.ref(), b.ref()); });
                    comparison(Opcode::NotEqual,
                               [&](auto dst) { bitops::notEqual(dst, a.ref(), b.ref()); });
                    comparison(Opcode::LessThan, [&](auto dst) {
                        bitops::lessThan(dst, a.ref(), b.ref(), isSigned);
                    });
                    comparison(Opcode::LessEqual, [&](auto dst) {
                        bitops::lessEqual(dst, a.ref(), b.ref(), isSigned);
                    });
                }
            }

            const std::uint32_t source = load(a);
            check(Opcode::Move, false, a, {source});
            for (const std::uint32_t to : {1U, 17U, 64U}) {
                if (to == width) continue;
                for (const bool isSigned : {false, true}) {
                    BitVector value(to);
                    bitops::extend(value.mutableRef(), a.ref(), isSigned);
                    check(to > width ? Opcode::Extend : Opcode::Truncate, isSigned, value,
                          {source});
                }
            }
        }
    }

    // A part of any source, unknown bits and all, at an offset in it or past it.
    for (const std::string_view text :
         {"64'h0123456789abcdef", "100'hf_0123_4567_89ab_cdef_x0z1_5a5a",
          "130'h3_ffff_0000_ffff_0000_1234_xxxx_8765_4321"}) {
        const BitVector source = literal(text);
        const std::uint32_t sourceRegister = load(source);
        for (const std::uint32_t width : {1U, 8U, 64U}) {
            for (const std::uint64_t low : {0U, 3U, 36U, 60U, 64U, 66U, 99U, 129U, 140U}) {
                const std::uint32_t offset = load(BitVector::fromUInt64(low, 32));
                BitVector value(width);
                bitops::partSelectAt(value.mutableRef(), source.ref(), low);
                check(Opcode::PartSelect, false, value, {sourceRegister, offset});
            }
        }
    }
    builder.emit(Instruction::make(Opcode::Return));

    const std::size_t count = results.size();
    Simulation simulation(designOf(makeTop({builder.finish()}, std::move(results))), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    for (std::size_t index = 0; index < count; ++index) {
        EXPECT_EQ(simulation.value(std::format("/tb/r{}", index)),
                  toString(expected[index].ref(), enzan::eir::Radix::Binary))
            << "result " << index;
    }
}

TEST(Kernel, ARecursionWithoutEndStopsAtTheCallDepthLimit) {
    SubprogramBuilder forever("forever");
    const std::uint32_t value = forever.parameter("value", Type::bits(8, false, true));
    const std::uint32_t result = forever.result(Type::bits(8, false, true));
    forever.block("entry");
    forever.call("", "forever", {Operand::reg(value)}, Operand::reg(result));
    forever.emit(Instruction::make(Opcode::Return));

    ProcessBuilder builder(ProcessKind::Initial, "caller");
    const std::uint32_t argument = builder.reg(Type::bits(8, false, true));
    const std::uint32_t returned = builder.reg(Type::bits(8, false, true));
    builder.block("entry");
    builder.call("", "forever", {Operand::reg(argument)}, Operand::reg(returned));
    builder.store(1, returned);
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.subprograms.push_back(forever.finish());

    const std::uint32_t previous = Simulation::callDepthLimit;
    Simulation::callDepthLimit = 50;
    Simulation simulation(designOf(std::move(definition)), "tb");
    Simulation::callDepthLimit = previous;
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);

    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::CallDepthExceeded))
        << reportedMessages(simulation);
}

TEST(Kernel, APackagesSubprogramReachesThePackagesOwnObjects) {
    // One instance of the package, at the root, holds `count`, so two calls see one count.
    ModuleDefinition package;
    package.name = "counter_pkg::";
    package.sourceName = "counter_pkg";
    package.isPackage = true;
    package.addObject(Object{.name = "count",
                             .type = Type::bits(8, false, true),
                             .initialValue = BitVector::parse("8'h00")});

    SubprogramBuilder bump("bump");
    const std::uint32_t current = bump.reg(Type::bits(8, false, true));
    const std::uint32_t one = bump.reg(Type::bits(8, false, true));
    const std::uint32_t oneValue = bump.constant("8'h01");
    const std::uint32_t result = bump.result(Type::bits(8, false, true));
    bump.block("entry");
    bump.readObject(current, 0);
    bump.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(one), Operand::constant(oneValue)));
    bump.emit(Instruction::make(Opcode::Add, Operand::reg(result), Operand::reg(current),
                                Operand::reg(one)));
    bump.store(0, result);
    bump.emit(Instruction::make(Opcode::Return));
    package.subprograms.push_back(bump.finish());

    ProcessBuilder builder(ProcessKind::Initial, "caller");
    const std::uint32_t first = builder.reg(Type::bits(8, false, true));
    const std::uint32_t second = builder.reg(Type::bits(8, false, true));
    builder.block("entry");
    builder.call("counter_pkg::", "bump", {}, Operand::reg(first));
    builder.call("counter_pkg::", "bump", {}, Operand::reg(second));
    builder.store(1, second);
    builder.emit(Instruction::make(Opcode::Return));

    Design design;
    design.addModule(std::move(package));
    design.addModule(makeTop({builder.finish()}));

    Simulation simulation(std::move(design), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.value("/tb/b", Radix::Hexadecimal), "02");
    EXPECT_EQ(simulation.value("/counter_pkg/count", Radix::Hexadecimal), "02");
}

TEST(Kernel, ASystemTaskInASubprogramReadsTheSubprogramsRegisters) {
    SubprogramBuilder show("show");
    const std::uint32_t value = show.parameter("value", Type::bits(8, false, true));
    const std::uint32_t format = show.string("shown %0d");
    SysTaskCall display;
    display.kind = SysTaskKind::Display;
    display.formatIndex = format;
    display.arguments.push_back(
        SysTaskArgument{.value = Operand::reg(value), .type = Type::bits(8, false, true)});
    const std::uint32_t descriptor = show.sysCall(std::move(display));
    show.block("entry");
    show.emit(Instruction::make(Opcode::SysTask, {}, Operand::sysCall(descriptor)));
    show.emit(Instruction::make(Opcode::Return));

    ProcessBuilder builder(ProcessKind::Initial, "caller");
    const std::uint32_t seven = builder.constant("8'd7");
    builder.block("entry");
    builder.call("", "show", {Operand::constant(seven)});
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({builder.finish()});
    definition.subprograms.push_back(show.finish());

    Simulation simulation(designOf(std::move(definition)), "tb");
    ASSERT_TRUE(simulation.ready()) << reportedMessages(simulation);
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.transcript(), "shown 7\n");
}

TEST(Kernel, ASubprogramMayNotWriteASignalSomethingDrives) {
    // Its writes are immediate and go through no driver, so `a`, which a continuous
    // assignment drives, would be written past its driver.
    ProcessBuilder assign(ProcessKind::ContinuousAssign, "assign");
    const std::uint32_t copied = assign.reg(Type::bits(8, false, true));
    assign.block("entry");
    assign.readObject(copied, 1);
    assign.store(0, copied);
    assign.emit(Instruction::make(Opcode::Return));

    SubprogramBuilder clobber("clobber");
    const std::uint32_t value = clobber.reg(Type::bits(8, false, true));
    const std::uint32_t constant = clobber.constant("8'h3c");
    clobber.block("entry");
    clobber.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(constant)));
    clobber.store(0, value);
    clobber.emit(Instruction::make(Opcode::Return));

    ProcessBuilder caller(ProcessKind::Initial, "caller");
    caller.block("entry");
    caller.call("", "clobber", {});
    caller.emit(Instruction::make(Opcode::Return));

    ModuleDefinition definition = makeTop({assign.finish(), caller.finish()});
    definition.subprograms.push_back(clobber.finish());

    const Simulation simulation(designOf(std::move(definition)), "tb");
    EXPECT_FALSE(simulation.ready());
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::SubprogramWritesDrivenSignal))
        << reportedMessages(simulation);
}

TEST(Kernel, ACallOfASubprogramNoUnitDeclaresIsAnElaborationError) {
    ProcessBuilder builder(ProcessKind::Initial, "caller");
    builder.block("entry");
    builder.call("elsewhere::", "missing", {});
    builder.emit(Instruction::make(Opcode::Return));

    const Simulation simulation(designOf(makeTop({builder.finish()})), "tb");
    EXPECT_FALSE(simulation.ready());
    EXPECT_TRUE(simulation.reportedCode(enzan::DiagCode::UnresolvedSubprogram))
        << reportedMessages(simulation);
}

} // namespace
