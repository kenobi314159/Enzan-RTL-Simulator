// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end scheduling tests: EIR is built by hand, verified, elaborated, compiled and
// run. This is where the semantics of DESIGN_GUIDE.md section 3 are pinned, and section
// 3 says changes there MUST be accompanied by tests that do exactly this.
//
// Building EIR by hand rather than parsing SystemVerilog is deliberate at this point: it
// tests the kernel without depending on the frontend, so a scheduling bug and a frontend
// bug cannot be confused for one another.

#include "runtime/Kernel.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Verifier.h"
#include "elab/Elaborator.h"
#include "runtime/Image.h"

#include "support/Literals.h"
#include "support/ProcessBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// NOLINTBEGIN(misc-unused-using-decls)
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::codegen::CompiledDesign;
using enzan::codegen::Compiler;
using enzan::eir::BasicBlock;
using enzan::eir::BitVector;
using enzan::eir::Design;
using enzan::eir::EdgeKind;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::Process;
using enzan::eir::ProcessKind;
using enzan::eir::Radix;
using enzan::eir::SensitivityEntry;
using enzan::eir::SysTaskArgument;
using enzan::eir::SysTaskCall;
using enzan::eir::SysTaskKind;
using enzan::eir::Type;
using enzan::elab::ElaboratedDesign;
using enzan::elab::Elaborator;
using enzan::runtime::Image;
using enzan::runtime::Kernel;
using enzan::runtime::KernelOptions;
using enzan::runtime::NoTime;
using enzan::runtime::RunInterrupt;
using enzan::runtime::RunStatus;
using enzan::runtime::SimTime;
using enzan::test::literal;
using enzan::test::ProcessBuilder;

// NOLINTEND(misc-unused-using-decls)

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
        codes.push_back(diagnostic.code);
    }

    std::vector<std::string> messages;
    std::vector<enzan::DiagCode> codes;
};

/// Verifies, elaborates, compiles and runs a one-module design.
class Simulation {
public:
    Simulation(Design design, std::string_view topName)
        : engine(sources), analysed(std::move(design)) {
        engine.addConsumer(consumer);

        // Verifying here means every kernel test also exercises the verifier, and that a
        // test whose hand-built EIR is malformed says so instead of misbehaving.
        valid = enzan::eir::verify(analysed, engine);

        Elaborator elaborator(analysed, engine);
        elaborated = elaborator.elaborate(topName);
        if (elaborated == nullptr) return;

        Compiler compiler(engine);
        compiled = compiler.compile(analysed);
        if (compiled == nullptr) return;

        image = std::make_unique<Image>(analysed, *elaborated, *compiled);

        KernelOptions options;
        options.transcript = &transcriptStream;
        options.deltaLimit = deltaLimit;
        kernel = std::make_unique<Kernel>(*image, engine, options);
        kernel->initialize();
    }

    [[nodiscard]] bool ready() const noexcept { return kernel != nullptr && valid; }

    RunStatus run(SimTime until = NoTime) { return kernel->run(until); }

    [[nodiscard]] SimTime time() const { return kernel->time(); }

    /// Renders a signal's current value, so that assertions read as the design does.
    [[nodiscard]] std::string value(std::string_view path, Radix radix = Radix::Binary) const {
        const std::uint32_t signalIndex = image->findSignal(path);
        EXPECT_NE(signalIndex, Image::NoSignal) << "no signal named " << path;
        if (signalIndex == Image::NoSignal) return "<missing>";

        const auto& signal = image->signals()[signalIndex];
        return enzan::eir::toString(kernel->arena().at(signal.wordOffset, signal.bitWidth), radix);
    }

    /// The index of a signal by path, failing the test if the design has no such signal.
    [[nodiscard]] std::uint32_t signal(std::string_view path) const {
        const std::uint32_t signalIndex = image->findSignal(path);
        EXPECT_NE(signalIndex, Image::NoSignal) << "no signal named " << path;
        return signalIndex;
    }

    [[nodiscard]] std::string transcript() const { return transcriptStream.str(); }

    [[nodiscard]] bool reportedCode(enzan::DiagCode code) const {
        return std::ranges::find(consumer.codes, code) != consumer.codes.end();
    }

    [[nodiscard]] const std::vector<std::string>& messages() const { return consumer.messages; }

    Kernel& simulationKernel() { return *kernel; }

    /// Delta limit applied to the kernel. Set before constructing to test the limit
    /// without waiting for ten thousand iterations.
    static std::uint32_t deltaLimit;

private:
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    std::ostringstream transcriptStream;

    Design analysed;
    std::unique_ptr<ElaboratedDesign> elaborated;
    std::unique_ptr<CompiledDesign> compiled;
    std::unique_ptr<Image> image;
    std::unique_ptr<Kernel> kernel;
    bool valid = false;
};

std::uint32_t Simulation::deltaLimit = 10'000;

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
    // Section 3.9. A four-state object that defaulted to zero would hide exactly the bugs
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
    builder.storePartNonBlocking(0, nibble, index);
    builder.emit(Instruction::make(Opcode::LoadConstant, Operand::reg(nibble),
                                   Operand::constant(highValue)));
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(index), Operand::constant(four)));
    builder.storePartNonBlocking(0, nibble, index);
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
    // what the Inactive region is for (section 3.3). The second process writes last, so
    // its value is the one that survives.
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
    // Value-change filtering before scheduling. Section 6.2 calls this one of the largest
    // single performance wins available and requires it from the start.
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
    EXPECT_EQ(simulation.transcript(), "value = 42 at 0ps in /tb\n");
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
    // compile error, so that one transcript shows everything (section 10.6).
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
    // Section 3.2: the limit must exist and the diagnostic must name what is still
    // active, because a delta loop with no diagnostic is one of the most frustrating
    // failures a user can hit.
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
    // A batch run writes a VCD and an FST from one simulation, and section 2.2 requires the GUI to
    // attach while a run is in progress. Both need more than one listener.
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
    // Elaboration collapses a connected port onto the parent's slot, so one slot is reached
    // through two objects — and a child's port declaration carries no initialiser. Initialising
    // per object rather than per slot let the child's default overwrite the parent's declared
    // value with X, depending only on which instance came last. The design then simulated and
    // produced a plausible wrong answer, which is the failure mode section 1.2 ranks above all
    // others, and it is exactly what happened on the first hierarchical design that was tried.
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
    // The invariant the interrupt exists to preserve. Section 3.8 requires results to be
    // bit-identical across runs of the same build on the same inputs, and a GUI that could
    // change a simulation by pausing it would break that in the least debuggable way
    // imaginable: the waveform would depend on when the user clicked.
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
// examine, deposit, force and release (section 10.4)
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
// restart (section 8.1)
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

} // namespace
