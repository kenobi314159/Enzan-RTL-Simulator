// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the machinery both waveform writers share, through a test double that records the
// hooks it is called with. Driving the base class directly rather than through VCD or FST means a
// defect in the shared logic — which is where the subtle ones live — is reported once and named
// for what it is, instead of appearing twice as two format bugs.

#include "wavedb/WaveformWriter.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "elab/Elaborator.h"
#include "runtime/Arena.h"
#include "runtime/Image.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::codegen::CompiledDesign;
using enzan::codegen::Compiler;
using enzan::eir::BitVector;
using enzan::eir::Design;
using enzan::eir::Logic;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Type;
using enzan::elab::ElaboratedDesign;
using enzan::elab::Elaborator;
using enzan::runtime::Arena;
using enzan::runtime::Image;
using enzan::runtime::TimeScale;
using enzan::test::literal;
using enzan::wavedb::WaveformWriter;

class DiscardingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const enzan::Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
    }

    std::vector<std::string> messages;
};

/// A writer whose "format" is a list of the hooks it was asked to perform.
class RecordingWriter final : public WaveformWriter {
public:
    using WaveformWriter::WaveformWriter;

    ~RecordingWriter() override { close(); }

    /// Set before `open` to make it fail, so the error path can be exercised.
    bool refuseToOpen = false;

    std::vector<std::string> events;

    /// Exposed for the tests: a real writer reaches this from inside its own hooks, where it is
    /// protected, and a test asserting that two names share one record has to ask from outside.
    using WaveformWriter::NotRecorded;
    using WaveformWriter::recordIndexOf;

    // The hooks keep the visibility they have in the base class, as in the real writers.
protected:
    bool openOutput(const std::filesystem::path& path, std::string& error) override {
        if (refuseToOpen) {
            error = "refused for the test";
            return false;
        }
        events.push_back("open:" + path.filename().string());
        return true;
    }

    void writeHeader() override { events.emplace_back("header"); }

    void enterScope(std::string_view name) override {
        events.push_back("enter:" + std::string(name));
    }

    void leaveScope() override { events.emplace_back("leave"); }

    void declareSignal(std::uint32_t signalIndex, std::string_view leafName) override {
        events.push_back("declare:" + std::string(leafName) + "@"
                         + std::to_string(recordIndexOf(signalIndex)));
    }

    void endDeclarations() override { events.emplace_back("enddecl"); }

    void beginInitialDump() override { events.emplace_back("dumpstart"); }

    void endInitialDump() override { events.emplace_back("dumpend"); }

    void writeTimestamp(enzan::runtime::SimTime time) override {
        events.push_back("time:" + std::to_string(time));
    }

    void writeChange(std::uint32_t signalIndex) override {
        events.push_back("change:" + std::to_string(signalIndex) + "="
                         + enzan::eir::toString(valueOf(signalIndex), enzan::eir::Radix::Binary));
    }

    void closeOutput() override { events.emplace_back("close"); }
};

/// Elaborates a hand-built design and gives the test an arena to change.
class WaveformWriterTest : public ::testing::Test {
protected:
    WaveformWriterTest() : engine(sources) { engine.addConsumer(consumer); }

    void build(Design design, const std::string& top) {
        analysed = std::move(design);

        Elaborator elaborator(analysed, engine);
        elaborated = elaborator.elaborate(top);
        ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

        Compiler compiler(engine);
        compiled = compiler.compile(analysed);
        ASSERT_NE(compiled, nullptr);

        image = std::make_unique<Image>(analysed, *elaborated, *compiled);
        arena.resize(image->arenaWords());
        image->initializeState(arena);
        writer = std::make_unique<RecordingWriter>(*image, arena, TimeScale(-9));
    }

    [[nodiscard]] enzan::eir::BitRef signal(std::string_view path) {
        const std::uint32_t index = image->findSignal(path);
        EXPECT_NE(index, Image::NoSignal) << path;
        const auto& info = image->signals()[index];
        return arena.at(info.wordOffset, info.bitWidth);
    }

    [[nodiscard]] std::uint32_t signalIndex(std::string_view path) const {
        return image->findSignal(path);
    }

    /// Reports the change of `path` to the writer, as the kernel would.
    void report(std::string_view path) { writer->onValueChange(signalIndex(path), signal(path)); }

    [[nodiscard]] std::filesystem::path outputPath() const { return directory / "wave.out"; }

    [[nodiscard]] bool opened() {
        std::string error;
        return writer->open(outputPath(), error);
    }

    /// Index of the first event equal to `wanted`, or the event count when absent.
    [[nodiscard]] std::size_t indexOf(std::string_view wanted) const {
        for (std::size_t index = 0; index < writer->events.size(); ++index) {
            if (writer->events[index] == wanted) return index;
        }
        return writer->events.size();
    }

    [[nodiscard]] std::size_t countOf(std::string_view prefix) const {
        std::size_t total = 0;
        for (const std::string& event : writer->events) {
            if (event.starts_with(prefix)) ++total;
        }
        return total;
    }

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory =
            std::filesystem::temp_directory_path()
            / ("enzan-wave-" + std::string(info->name()) + '-' + std::to_string(::getpid()));
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        ASSERT_TRUE(std::filesystem::create_directories(directory, error)) << error.message();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    SourceManager sources;
    DiagnosticEngine engine;
    DiscardingConsumer consumer;

    Design analysed;
    std::unique_ptr<ElaboratedDesign> elaborated;
    std::unique_ptr<CompiledDesign> compiled;
    std::unique_ptr<Image> image;
    Arena arena;
    std::unique_ptr<RecordingWriter> writer;
    std::filesystem::path directory;
};

/// A flat module with a one-bit and an eight-bit object.
Design flatDesign() {
    ModuleDefinition definition;
    definition.name = "tb";
    definition.sourceName = "tb";
    definition.addObject(Object{.name = "clk", .type = Type::logic()});
    definition.addObject(Object{.name = "count", .type = Type::bits(8, false, true)});

    Design design;
    design.addModule(std::move(definition));
    return design;
}

/// A parent whose child's port is collapsed onto the parent's object.
Design hierarchicalDesign() {
    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "in", .type = Type::bits(4, false, true)});
    child.addObject(Object{.name = "internal", .type = Type::logic()});
    child.ports.push_back(enzan::eir::Port{
        .name = "in", .direction = enzan::eir::PortDirection::Input, .objectIndex = 0});

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    parent.addObject(Object{.name = "drive", .type = Type::bits(4, false, true)});
    enzan::eir::Instance instance;
    instance.name = "u_dut";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0});
    parent.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(parent));
    return design;
}

/// A flat module with a memory, `logic [7:0] memory [0:3]`, and vectors of 100 and 200 bits
/// beside a one-bit object.
Design wideDesign() {
    ModuleDefinition definition;
    definition.name = "tb";
    definition.sourceName = "tb";
    definition.addObject(Object{.name = "clk", .type = Type::logic()});
    definition.addObject(
        Object{.name = "memory",
               .type = Type::bits(32, false, true),
               .unpackedDimensions = {enzan::eir::ArrayDimension{.left = 0, .right = 3}},
               .elementWidth = 8});
    definition.addObject(Object{.name = "wide", .type = Type::bits(100, false, true)});
    definition.addObject(Object{.name = "wider", .type = Type::bits(200, false, true)});

    Design design;
    design.addModule(std::move(definition));
    return design;
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

TEST_F(WaveformWriterTest, OpenPerformsTheHooksInOrder) {
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());

    // The header precedes the hierarchy, the hierarchy precedes the values, and the initial dump
    // is bracketed. A format whose header has to come first cannot work otherwise.
    EXPECT_LT(indexOf("header"), indexOf("enter:tb"));
    EXPECT_LT(indexOf("enter:tb"), indexOf("enddecl"));
    EXPECT_LT(indexOf("enddecl"), indexOf("time:0"));
    EXPECT_LT(indexOf("time:0"), indexOf("dumpstart"));
    EXPECT_LT(indexOf("dumpstart"), indexOf("dumpend"));
}

TEST_F(WaveformWriterTest, OpenDumpsEverySignalAtTimeZero) {
    // A viewer must show the state the design starts from rather than leaving traces undefined
    // until their first change. A four-state object starts at X, so that is what appears.
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordedSignals(), 2U);
    EXPECT_EQ(countOf("change:"), 2U);
    EXPECT_NE(indexOf("change:1=xxxxxxxx"), writer->events.size());
}

TEST_F(WaveformWriterTest, AFailedOpenLeavesTheWriterClosed) {
    build(flatDesign(), "tb");
    writer->refuseToOpen = true;

    std::string error;
    EXPECT_FALSE(writer->open(outputPath(), error));
    EXPECT_EQ(error, "refused for the test");
    EXPECT_FALSE(writer->isOpen());

    // Nothing was written, and a later change must not be either.
    EXPECT_TRUE(writer->events.empty());
    report("/tb/clk");
    EXPECT_TRUE(writer->events.empty());
}

TEST_F(WaveformWriterTest, FlushWritesThePendingSlotWithoutClosing) {
    // A slot is normally written when the next time advance arrives, which is right for a file
    // being streamed and wrong the moment a run pauses: the last thing the design did would be
    // missing from the view until the kernel was told to carry on, which looks exactly like the
    // simulator having lost an event.
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());

    writer->onTimeAdvance(10);
    signal("/tb/count").fill(enzan::eir::Logic::One);
    report("/tb/count");

    EXPECT_EQ(countOf("time:10"), 0U) << "nothing is owed to the file until the slot ends";

    writer->flush();
    EXPECT_EQ(countOf("time:10"), 1U);
    EXPECT_NE(indexOf("change:1=11111111"), writer->events.size());
    EXPECT_TRUE(writer->isOpen()) << "flushing must not end the recording";

    // Flushing again writes nothing: the slot has already been accounted for.
    writer->flush();
    EXPECT_EQ(countOf("time:10"), 1U);

    // And the writer carries on normally afterwards.
    writer->onTimeAdvance(20);
    signal("/tb/count").fill(enzan::eir::Logic::Zero);
    report("/tb/count");
    writer->close();
    EXPECT_EQ(countOf("time:20"), 1U);
}

TEST_F(WaveformWriterTest, ChangesBeforeOpenAreIgnored) {
    build(flatDesign(), "tb");
    report("/tb/clk");
    EXPECT_TRUE(writer->events.empty());
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

TEST_F(WaveformWriterTest, NestedScopesAreEnteredAndLeftInBalance) {
    build(hierarchicalDesign(), "tb");
    ASSERT_TRUE(opened());

    EXPECT_EQ(countOf("enter:"), 2U) << "tb and u_dut";
    EXPECT_EQ(countOf("leave"), 2U) << "every scope must be closed";
    EXPECT_LT(indexOf("enter:u_dut"), indexOf("leave"));
}

TEST_F(WaveformWriterTest, SiblingScopesWithACommonPrefixAreNotNested) {
    // A plain prefix comparison would put `/tb/u_dut2` inside `/tb/u_dut`. The separator has to be
    // part of the comparison, and this is the one place that answer lives.
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.sourceName = "leaf";
    leaf.addObject(Object{.name = "q", .type = Type::logic()});

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    for (const char* name : {"u_dut", "u_dut2"}) {
        enzan::eir::Instance instance;
        instance.name = name;
        instance.definitionName = "leaf";
        parent.instances.push_back(std::move(instance));
    }

    Design design;
    design.addModule(std::move(leaf));
    design.addModule(std::move(parent));
    build(std::move(design), "tb");
    ASSERT_TRUE(opened());

    EXPECT_EQ(countOf("enter:"), 3U);
    EXPECT_EQ(countOf("leave"), 3U);

    // The first scope must close before the sibling opens, or the sibling is nested in it.
    EXPECT_LT(indexOf("leave"), indexOf("enter:u_dut2"));
}

TEST_F(WaveformWriterTest, ACollapsedPortIsDeclaredUnderBothNamesWithOneRecord) {
    // One slot, two real hierarchical names. Both are declared so that a viewer shows the DUT's
    // port inside the DUT, and both map to the same recorded index so a format can share storage.
    build(hierarchicalDesign(), "tb");
    ASSERT_TRUE(opened());

    const std::uint32_t shared = writer->recordIndexOf(signalIndex("/tb/drive"));
    EXPECT_NE(indexOf("declare:drive@" + std::to_string(shared)), writer->events.size());
    EXPECT_NE(indexOf("declare:in@" + std::to_string(shared)), writer->events.size());

    // Two names, but only one signal to dump.
    EXPECT_EQ(countOf("declare:"), 3U) << "drive, in, and the child's internal";
    EXPECT_EQ(writer->recordedSignals(), 2U);
    EXPECT_EQ(countOf("change:"), 2U);
}

// ---------------------------------------------------------------------------
// Selective capture
// ---------------------------------------------------------------------------

TEST_F(WaveformWriterTest, ScopeFilteringRecordsOnlyTheSelectedSubtree) {
    // Selective capture is a requirement, not a convenience: recording everything on a large design
    // exhausts memory within seconds of simulated time (section 7.1).
    build(hierarchicalDesign(), "tb");
    writer->setScope("/tb/u_dut");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordedSignals(), 1U) << "only the child's own object";
    EXPECT_EQ(countOf("declare:internal@0"), 1U);
    EXPECT_EQ(countOf("declare:drive"), 0U);
}

TEST_F(WaveformWriterTest, ScopeFilteringDoesNotCatchASiblingWithACommonPrefix) {
    ModuleDefinition leaf;
    leaf.name = "leaf";
    leaf.sourceName = "leaf";
    leaf.addObject(Object{.name = "q", .type = Type::logic()});

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    for (const char* name : {"u_dut", "u_dut2"}) {
        enzan::eir::Instance instance;
        instance.name = name;
        instance.definitionName = "leaf";
        parent.instances.push_back(std::move(instance));
    }

    Design design;
    design.addModule(std::move(leaf));
    design.addModule(std::move(parent));
    build(std::move(design), "tb");
    writer->setScope("/tb/u_dut");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordedSignals(), 1U) << "u_dut2 is a sibling, not a child";
}

TEST_F(WaveformWriterTest, SeveralScopesAreRecordedTogether) {
    // A selection built out of a subtree and one individual signal.
    build(hierarchicalDesign(), "tb");
    writer->addScope("/tb/u_dut");
    writer->addScope("/tb/drive");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordedSignals(), 2U);
    EXPECT_EQ(countOf("declare:internal@"
                      + std::to_string(writer->recordIndexOf(signalIndex("/tb/u_dut/internal")))),
              1U);
    EXPECT_EQ(
        countOf("declare:drive@" + std::to_string(writer->recordIndexOf(signalIndex("/tb/drive")))),
        1U);
}

TEST_F(WaveformWriterTest, AddingTheSameScopeTwiceRecordsItOnce) {
    build(hierarchicalDesign(), "tb");
    writer->addScope("/tb/u_dut");
    writer->addScope("/tb/u_dut");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordedSignals(), 1U);
}

TEST_F(WaveformWriterTest, AnEmptyScopeSubsumesTheOnesBeforeIt) {
    // `log -r /*` after a narrower selection means the whole design, so keeping the narrow one
    // as well would be dead weight in the filter and misleading to read.
    build(hierarchicalDesign(), "tb");
    writer->addScope("/tb/u_dut");
    writer->addScope("");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordedSignals(), 2U);
}

TEST_F(WaveformWriterTest, SetScopeReplacesWhatWasSelectedBefore) {
    build(hierarchicalDesign(), "tb");
    writer->addScope("/tb/drive");
    writer->setScope("/tb/u_dut");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordedSignals(), 1U);
    EXPECT_EQ(countOf("declare:internal@0"), 1U);
    EXPECT_EQ(countOf("declare:drive"), 0U);
}

TEST_F(WaveformWriterTest, AChangeToAnUnrecordedSignalIsIgnored) {
    build(hierarchicalDesign(), "tb");
    writer->setScope("/tb/u_dut");
    ASSERT_TRUE(opened());
    const std::size_t afterOpen = writer->events.size();

    writer->onTimeAdvance(10);
    signal("/tb/drive").fill(Logic::One);
    report("/tb/drive");
    writer->onTimeAdvance(20);

    EXPECT_EQ(writer->events.size(), afterOpen) << "nothing recorded changed";
}

// ---------------------------------------------------------------------------
// Time attribution and coalescing
// ---------------------------------------------------------------------------

TEST_F(WaveformWriterTest, ChangesBelongToTheSlotThatIsEndingNotTheOneBeginning) {
    // `onTimeAdvance` fires before anything at the new time runs, so the changes accumulated so far
    // belong to the previous timestamp. Getting this backwards shifts every trace by one slot,
    // which a viewer makes look like a design bug.
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());

    writer->onTimeAdvance(10);
    signal("/tb/count").fill(Logic::One);
    report("/tb/count");
    writer->onTimeAdvance(20);

    const std::size_t timestamp = indexOf("time:10");
    const std::size_t value = indexOf("change:1=11111111");
    ASSERT_NE(timestamp, writer->events.size());
    ASSERT_NE(value, writer->events.size());
    EXPECT_LT(timestamp, value);
    EXPECT_EQ(indexOf("time:20"), writer->events.size()) << "nothing changed at 20";
}

TEST_F(WaveformWriterTest, SeveralChangesInOneSlotCollapseToTheSettledValue) {
    // Delta-level intermediate values are noise: recording them makes a diff of two waveforms fail
    // constantly, which section 14.3 warns is how a differential suite gets switched off.
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());
    const std::size_t afterOpen = writer->events.size();

    writer->onTimeAdvance(10);
    for (const char* value : {"8'h01", "8'h02", "8'h03"}) {
        const BitVector intermediate = literal(value);
        enzan::eir::bitops::copy(signal("/tb/count"), intermediate.ref());
        report("/tb/count");
    }
    writer->onTimeAdvance(20);

    // One timestamp and one value, not three.
    EXPECT_EQ(writer->events.size() - afterOpen, 2U);
    EXPECT_NE(indexOf("change:1=00000011"), writer->events.size());
}

TEST_F(WaveformWriterTest, AChangeAtTimeZeroAppendsToTheInitialDump) {
    // Time zero's timestamp was already written by `open`, so repeating it would produce two `#0`
    // sections, which some readers reject.
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());

    signal("/tb/clk").set(0, Logic::One);
    report("/tb/clk");
    writer->onTimeAdvance(5);

    EXPECT_EQ(countOf("time:0"), 1U);
}

TEST_F(WaveformWriterTest, CloseFlushesTheLastSlotAndIsIdempotent) {
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());

    writer->onTimeAdvance(10);
    signal("/tb/clk").set(0, Logic::One);
    report("/tb/clk");

    writer->close();
    EXPECT_FALSE(writer->isOpen());
    EXPECT_NE(indexOf("time:10"), writer->events.size()) << "the last slot must still be written";
    EXPECT_EQ(countOf("close"), 1U);

    // Called again by the destructor, and by anyone who does not track whether they already did.
    writer->close();
    EXPECT_EQ(countOf("close"), 1U);
}

TEST_F(WaveformWriterTest, ADesignWithNoChangesStillProducesAValidHeader) {
    build(flatDesign(), "tb");
    ASSERT_TRUE(opened());
    writer->close();

    EXPECT_NE(indexOf("header"), writer->events.size());
    EXPECT_NE(indexOf("enddecl"), writer->events.size());
    EXPECT_EQ(countOf("close"), 1U);
}

// ---------------------------------------------------------------------------
// What is left out
// ---------------------------------------------------------------------------

TEST_F(WaveformWriterTest, AFileLeavesAMemoryOut) {
    build(wideDesign(), "tb");
    ASSERT_TRUE(opened());

    EXPECT_EQ(writer->recordIndexOf(signalIndex("/tb/memory")), RecordingWriter::NotRecorded);
    EXPECT_EQ(writer->recordedSignals(), 3U);
    EXPECT_TRUE(writer->tooWide().empty()) << "a memory is left out for what it is, not its width";
}

TEST_F(WaveformWriterTest, ASignalWiderThanTheLimitIsLeftOutAndNamed) {
    build(wideDesign(), "tb");
    writer->setWidthLimit(100);
    ASSERT_TRUE(opened());

    EXPECT_NE(writer->recordIndexOf(signalIndex("/tb/wide")), RecordingWriter::NotRecorded)
        << "a signal as wide as the limit is within it";
    EXPECT_EQ(writer->recordIndexOf(signalIndex("/tb/wider")), RecordingWriter::NotRecorded);
    EXPECT_EQ(countOf("declare:wider"), 0U);
    ASSERT_EQ(writer->tooWide().size(), 1U);
    EXPECT_EQ(writer->tooWide()[0], signalIndex("/tb/wider"));
    EXPECT_EQ(writer->describeTooWide(), "/tb/wider of 200 bits");
}

TEST_F(WaveformWriterTest, ThreeSignalsAreNamedAndMoreAreCounted) {
    build(wideDesign(), "tb");
    writer->setWidthLimit(1);
    ASSERT_TRUE(opened());
    EXPECT_EQ(writer->describeTooWide(),
              "2 signals, /tb/wide of 100 bits and /tb/wider of 200 bits");

    ModuleDefinition definition;
    definition.name = "tb";
    definition.sourceName = "tb";
    for (const char* name : {"a", "b", "c", "d"})
        definition.addObject(Object{.name = name, .type = Type::bits(2, false, true)});
    Design design;
    design.addModule(std::move(definition));
    build(std::move(design), "tb");
    writer->setWidthLimit(1);
    ASSERT_TRUE(opened());
    EXPECT_EQ(writer->describeTooWide(),
              "4 signals, among them /tb/a of 2 bits, /tb/b of 2 bits and /tb/c of 2 bits");
}

TEST_F(WaveformWriterTest, ALimitOfZeroRecordsAnyWidth) {
    build(wideDesign(), "tb");
    writer->setWidthLimit(0);
    ASSERT_TRUE(opened());
    EXPECT_NE(writer->recordIndexOf(signalIndex("/tb/wider")), RecordingWriter::NotRecorded);
    EXPECT_TRUE(writer->tooWide().empty());
}

} // namespace
