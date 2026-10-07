// SPDX-License-Identifier: GPL-3.0-or-later
//
// FST is a compressed binary container, so there is a limit to what a unit test can say about one
// without a reader. What it can check is the contract — the file exists, is structurally plausible,
// and the error paths behave — and that is what this file does.
//
// Whether the *contents* are correct is answered by `golden.counter.fst` and `golden.fifo.fst`,
// which put Enzan's FST through GTKWave's own `fst2vcd` and compare the result against the VCD
// written from the same run. That is a far stronger statement than anything here could make, and it
// is why these tests do not try to decode the format themselves.

#include "wavedb/FstWriter.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "elab/Elaborator.h"
#include "runtime/Arena.h"
#include "runtime/Image.h"
#include "wavedb/VcdWriter.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

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
using enzan::wavedb::FstWriter;
using enzan::wavedb::VcdWriter;

class DiscardingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const enzan::Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
    }

    std::vector<std::string> messages;
};

class FstWriterTest : public ::testing::Test {
protected:
    FstWriterTest() : engine(sources) { engine.addConsumer(consumer); }

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory = std::filesystem::temp_directory_path()
                    / ("enzan-fst-" + std::string(info->name()) + '-' + std::to_string(::getpid()));
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        ASSERT_TRUE(std::filesystem::create_directories(directory, error)) << error.message();

        ModuleDefinition child;
        child.name = "child";
        child.sourceName = "child";
        child.addObject(Object{.name = "in", .type = Type::bits(8, false, true)});
        child.addObject(Object{.name = "internal", .type = Type::logic()});
        child.ports.push_back(enzan::eir::Port{
            .name = "in", .direction = enzan::eir::PortDirection::Input, .objectIndex = 0});

        ModuleDefinition parent;
        parent.name = "tb";
        parent.sourceName = "tb";
        parent.addObject(Object{.name = "drive", .type = Type::bits(8, false, true)});
        enzan::eir::Instance instance;
        instance.name = "u_dut";
        instance.definitionName = "child";
        instance.connections.push_back(
            enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0});
        parent.instances.push_back(std::move(instance));

        analysed.addModule(std::move(child));
        analysed.addModule(std::move(parent));

        Elaborator elaborator(analysed, engine);
        elaborated = elaborator.elaborate("tb");
        ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

        Compiler compiler(engine);
        compiled = compiler.compile(elaborated->units());
        ASSERT_NE(compiled, nullptr);

        image = std::make_unique<Image>(*elaborated, *compiled);
        arena.resize(image->arenaWords());
        image->initializeState(arena);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
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

    [[nodiscard]] std::filesystem::path outputPath() const { return directory / "wave.fst"; }

    /// Reads the first byte of a file, or -1 when it cannot be read.
    static int firstByte(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return -1;
        const int value = file.get();
        return file.good() || file.eof() ? value : -1;
    }

    static std::uintmax_t sizeOf(const std::filesystem::path& path) {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        return error ? 0 : size;
    }

    SourceManager sources;
    DiagnosticEngine engine;
    DiscardingConsumer consumer;

    Design analysed;
    std::unique_ptr<ElaboratedDesign> elaborated;
    std::unique_ptr<CompiledDesign> compiled;
    std::unique_ptr<Image> image;
    Arena arena;
    std::filesystem::path directory;
};

TEST_F(FstWriterTest, WritesAFileThatStartsWithTheHeaderBlock) {
    // An FST is a sequence of tagged blocks, and the first is the header, whose tag is zero. That
    // is the cheapest check that something FST-shaped came out rather than an empty or truncated
    // file — which is what a writer that never closed would leave behind.
    {
        FstWriter writer(*image, arena, TimeScale(-9));
        std::string error;
        ASSERT_TRUE(writer.open(outputPath(), error)) << error;
        EXPECT_TRUE(writer.isOpen());
        writer.close();
    }

    EXPECT_GT(sizeOf(outputPath()), 0U);
    EXPECT_EQ(firstByte(outputPath()), 0) << "the first block should be the header";
}

TEST_F(FstWriterTest, RecordsEverySignalByDefault) {
    FstWriter writer(*image, arena, TimeScale(-9));
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    // Two slots, because the child's port collapsed onto the parent's object. Three *names*, but a
    // name is not a signal.
    EXPECT_EQ(writer.recordedSignals(), 2U);
}

TEST_F(FstWriterTest, ScopeFilteringRecordsOnlyTheSelectedSubtree) {
    FstWriter writer(*image, arena, TimeScale(-9));
    writer.setScope("/tb/u_dut");

    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    EXPECT_EQ(writer.recordedSignals(), 1U);
}

TEST_F(FstWriterTest, ReportsAPathThatCannotBeOpened) {
    FstWriter writer(*image, arena, TimeScale(-9));

    std::string error;
    EXPECT_FALSE(writer.open(directory / "no-such-directory" / "wave.fst", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(writer.isOpen());
}

TEST_F(FstWriterTest, CloseIsIdempotentAndTheFileSurvivesIt) {
    FstWriter writer(*image, arena, TimeScale(-9));
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    writer.onTimeAdvance(10);
    signal("/tb/drive").fill(Logic::One);
    writer.onValueChange(signalIndex("/tb/drive"), signal("/tb/drive"));

    writer.close();
    const std::uintmax_t afterFirst = sizeOf(outputPath());
    EXPECT_GT(afterFirst, 0U);
    EXPECT_FALSE(writer.isOpen());

    // Closing again must not truncate the file or double-free the library's context — which the
    // destructor will also attempt.
    writer.close();
    EXPECT_EQ(sizeOf(outputPath()), afterFirst);
}

TEST_F(FstWriterTest, ChangesAfterCloseAreIgnored) {
    FstWriter writer(*image, arena, TimeScale(-9));
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    // The library's context is gone, so a late change must be dropped rather than passed to it.
    signal("/tb/drive").fill(Logic::One);
    writer.onValueChange(signalIndex("/tb/drive"), signal("/tb/drive"));
    writer.onTimeAdvance(20);
    SUCCEED();
}

TEST_F(FstWriterTest, BothWritersCanRecordTheSameRun) {
    // A batch run writes a VCD to diff and an FST to look at, from one simulation. The kernel
    // delivers to every registered listener, and the two writers must not interfere: they share the
    // arena read-only and own nothing between them.
    const std::filesystem::path vcdPath = directory / "wave.vcd";

    {
        FstWriter fst(*image, arena, TimeScale(-9));
        VcdWriter vcd(*image, arena, TimeScale(-9));

        std::string error;
        ASSERT_TRUE(fst.open(outputPath(), error)) << error;
        ASSERT_TRUE(vcd.open(vcdPath, error)) << error;

        fst.onTimeAdvance(10);
        vcd.onTimeAdvance(10);
        signal("/tb/drive").fill(Logic::One);
        fst.onValueChange(signalIndex("/tb/drive"), signal("/tb/drive"));
        vcd.onValueChange(signalIndex("/tb/drive"), signal("/tb/drive"));

        fst.close();
        vcd.close();
    }

    EXPECT_GT(sizeOf(outputPath()), 0U);
    EXPECT_GT(sizeOf(vcdPath), 0U);
    EXPECT_EQ(firstByte(outputPath()), 0);
}

TEST_F(FstWriterTest, OpeningTheSamePathTwiceOverwritesRatherThanAppends) {
    const std::uintmax_t first = [&] {
        FstWriter writer(*image, arena, TimeScale(-9));
        std::string error;
        EXPECT_TRUE(writer.open(outputPath(), error)) << error;
        for (int step = 1; step <= 20; ++step) {
            writer.onTimeAdvance(static_cast<std::uint64_t>(step) * 10);
            signal("/tb/drive").fill(step % 2 == 0 ? Logic::One : Logic::Zero);
            writer.onValueChange(signalIndex("/tb/drive"), signal("/tb/drive"));
        }
        writer.close();
        return sizeOf(outputPath());
    }();

    const std::uintmax_t second = [&] {
        FstWriter writer(*image, arena, TimeScale(-9));
        std::string error;
        EXPECT_TRUE(writer.open(outputPath(), error)) << error;
        writer.close();
        return sizeOf(outputPath());
    }();

    EXPECT_GT(first, 0U);
    EXPECT_GT(second, 0U);
    EXPECT_LT(second, first) << "the second run should replace the first, not extend it";
}

} // namespace
