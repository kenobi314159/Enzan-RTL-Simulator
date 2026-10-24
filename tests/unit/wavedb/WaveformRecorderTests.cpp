// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the listener that fills the live database from a running kernel.
//
// The recorder is the shared recording machinery with the database standing in for a file
// format, so what is worth testing here is the join: that the scope filter reaches the database,
// that a time slot's changes are attributed to the slot that ended, and — the point of sharing
// the machinery at all — that the database and the VCD written from the same run agree.

#include "wavedb/WaveformRecorder.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Type.h"
#include "elab/Elaborator.h"
#include "runtime/Arena.h"
#include "runtime/Image.h"
#include "wavedb/WaveformDatabase.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::codegen::CompiledDesign;
using enzan::codegen::Compiler;
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
using enzan::wavedb::WaveformDatabase;
using enzan::wavedb::WaveformRecorder;

class DiscardingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const enzan::Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
    }

    std::vector<std::string> messages;
};

/// A parent with one child instance, so that the scope filter has something to exclude.
Design twoScopeDesign() {
    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "q", .type = Type::bits(4, false, true)});

    ModuleDefinition parent;
    parent.name = "tb";
    parent.sourceName = "tb";
    parent.addObject(Object{.name = "clk", .type = Type::logic()});
    parent.addObject(Object{.name = "count", .type = Type::bits(8, false, true)});

    enzan::eir::Instance instance;
    instance.name = "u_dut";
    instance.definitionName = "child";
    parent.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(parent));
    return design;
}

/// A module with a memory, `logic [7:0] memory [0:3]`, and a vector of 200 bits.
Design arrayDesign() {
    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(Object{.name = "memory",
                         .type = Type::bits(32, false, true),
                         .unpackedDimensions = {enzan::eir::ArrayDimension{.left = 0, .right = 3}},
                         .elementWidth = 8});
    top.addObject(Object{.name = "wide", .type = Type::bits(200, false, true)});

    Design design;
    design.addModule(std::move(top));
    return design;
}

class WaveformRecorderTest : public ::testing::Test {
protected:
    WaveformRecorderTest() : engine(sources) { engine.addConsumer(consumer); }

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
        recorder = std::make_unique<WaveformRecorder>(*image, arena, TimeScale(-9), database);
    }

    [[nodiscard]] std::uint32_t signalIndex(std::string_view path) const {
        return image->findSignal(path);
    }

    /// Writes a value into the arena and reports the change, as the kernel would.
    void set(std::string_view path, std::string_view value) {
        const std::uint32_t index = signalIndex(path);
        ASSERT_NE(index, Image::NoSignal) << path;

        const auto& info = image->signals()[index];
        const enzan::eir::BitRef target = arena.at(info.wordOffset, info.bitWidth);
        const enzan::eir::BitVector parsed = literal(value);
        for (std::uint32_t bit = 0; bit < info.bitWidth; ++bit) {
            target.set(bit, bit < parsed.width() ? parsed.get(bit) : Logic::Zero);
        }

        recorder->onValueChange(index, target);
    }

    /// The database's index for a hierarchical path.
    [[nodiscard]] std::uint32_t recordOf(std::string_view path) const {
        return database.recordIndexOf(signalIndex(path));
    }

    SourceManager sources;
    DiagnosticEngine engine;
    DiscardingConsumer consumer;

    Design analysed;
    std::unique_ptr<ElaboratedDesign> elaborated;
    std::unique_ptr<CompiledDesign> compiled;
    std::unique_ptr<Image> image;
    Arena arena;
    WaveformDatabase database;
    std::unique_ptr<WaveformRecorder> recorder;
};

TEST_F(WaveformRecorderTest, StartDeclaresEverySignalAndDumpsTimeZero) {
    build(twoScopeDesign(), "tb");
    recorder->start();

    EXPECT_EQ(database.signalCount(), 3U);

    // A four-state object with no initialiser starts at X (section 3.9), and the viewer must be
    // able to show that rather than an empty trace.
    const std::uint32_t count = recordOf("/tb/count");
    ASSERT_NE(count, WaveformDatabase::NotRecorded);
    EXPECT_EQ(database.changeCount(count), 1U);
    EXPECT_EQ(database.timeOfChange(count, 0), 0U);
    EXPECT_EQ(database.valueAt(count, 0), std::optional(literal("8'bxxxxxxxx")));
}

TEST_F(WaveformRecorderTest, RecordsNothingBeforeStart) {
    build(twoScopeDesign(), "tb");

    set("/tb/clk", "1'b1");
    EXPECT_EQ(database.signalCount(), 0U);
}

TEST_F(WaveformRecorderTest, ChangesBelongToTheSlotThatEnded) {
    // The classic off-by-one: a change reported during the slot at time 10 is recorded at 10,
    // not at the next timestamp the kernel announces. Getting it backwards shifts every trace.
    build(twoScopeDesign(), "tb");
    recorder->start();

    recorder->onTimeAdvance(10);
    set("/tb/count", "8'h2a");

    recorder->onTimeAdvance(20);
    set("/tb/count", "8'h2b");

    recorder->onTimeAdvance(30);

    const std::uint32_t count = recordOf("/tb/count");
    EXPECT_EQ(database.valueAt(count, 9), std::optional(literal("8'bxxxxxxxx")));
    EXPECT_EQ(database.valueAt(count, 10), std::optional(literal("8'h2a")));
    EXPECT_EQ(database.valueAt(count, 19), std::optional(literal("8'h2a")));
    EXPECT_EQ(database.valueAt(count, 20), std::optional(literal("8'h2b")));
}

TEST_F(WaveformRecorderTest, SeveralChangesInOneSlotCollapseToTheSettledValue) {
    // Delta-level noise must not reach the database: what a slot records is the value it
    // settled on, which is what makes a waveform diff meaningful (section 14.3).
    build(twoScopeDesign(), "tb");
    recorder->start();

    recorder->onTimeAdvance(10);
    set("/tb/count", "8'h01");
    set("/tb/count", "8'h02");
    set("/tb/count", "8'h03");
    recorder->onTimeAdvance(20);

    const std::uint32_t count = recordOf("/tb/count");
    EXPECT_EQ(database.changeCount(count), 2U) << "time zero, then one settled value at 10";
    EXPECT_EQ(database.valueAt(count, 10), std::optional(literal("8'h03")));
}

TEST_F(WaveformRecorderTest, TheScopeFilterReachesTheDatabase) {
    build(twoScopeDesign(), "tb");
    recorder->setScope("/tb/u_dut");
    recorder->start();

    EXPECT_EQ(database.signalCount(), 1U);
    EXPECT_NE(recordOf("/tb/u_dut/q"), WaveformDatabase::NotRecorded);
    EXPECT_EQ(recordOf("/tb/clk"), WaveformDatabase::NotRecorded);

    // A change outside the scope is not recorded, rather than recorded under a signal nobody
    // declared.
    set("/tb/clk", "1'b1");
    recorder->onTimeAdvance(10);
    EXPECT_EQ(database.signalCount(), 1U);
}

TEST_F(WaveformRecorderTest, ClosingFlushesThePendingSlot) {
    build(twoScopeDesign(), "tb");
    recorder->start();

    recorder->onTimeAdvance(10);
    set("/tb/count", "8'h7f");

    // The run ends without a further time advance; the last slot must still be recorded, or the
    // waveform loses whatever the design did on its final timestamp.
    recorder->close();

    const std::uint32_t count = recordOf("/tb/count");
    EXPECT_EQ(database.valueAt(count, 10), std::optional(literal("8'h7f")));
}

TEST_F(WaveformRecorderTest, RecordsEveryScopeWhenNoFilterIsSet) {
    build(twoScopeDesign(), "tb");
    recorder->start();

    EXPECT_NE(recordOf("/tb/clk"), WaveformDatabase::NotRecorded);
    EXPECT_NE(recordOf("/tb/count"), WaveformDatabase::NotRecorded);
    EXPECT_NE(recordOf("/tb/u_dut/q"), WaveformDatabase::NotRecorded);
}

TEST_F(WaveformRecorderTest, TheDatabaseRecordsAMemory) {
    // A file leaves one out; the wave window shows it as the array it is, so it has to be here.
    build(arrayDesign(), "tb");
    recorder->start();

    const std::uint32_t memory = recordOf("/tb/memory");
    ASSERT_NE(memory, WaveformDatabase::NotRecorded);
    recorder->onTimeAdvance(10);
    set("/tb/memory", "32'h01020304");
    recorder->onTimeAdvance(20);
    EXPECT_EQ(database.valueAt(memory, 10), std::optional(literal("32'h01020304")));
}

TEST_F(WaveformRecorderTest, TheDatabaseLeavesOutWhatIsTooWideAndSaysSo) {
    build(arrayDesign(), "tb");
    recorder->setWidthLimit(32);
    recorder->start();

    EXPECT_NE(recordOf("/tb/memory"), WaveformDatabase::NotRecorded);
    EXPECT_EQ(recordOf("/tb/wide"), WaveformDatabase::NotRecorded);
    ASSERT_EQ(recorder->tooWide().size(), 1U);
    EXPECT_EQ(recorder->tooWide()[0], signalIndex("/tb/wide"));
}

} // namespace
