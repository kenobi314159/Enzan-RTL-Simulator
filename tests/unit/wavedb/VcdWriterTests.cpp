// SPDX-License-Identifier: GPL-3.0-or-later
//
// The writer is driven directly through the listener interface rather than by running a
// simulation. A VCD file is the artefact every external viewer and every differential
// comparison depends on, so its shape is worth testing without a kernel in the way: a
// malformed header and a scheduling bug would otherwise be hard to tell apart.

#include "wavedb/VcdWriter.h"

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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
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
using enzan::wavedb::VcdWriter;

class DiscardingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const enzan::Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
    }

    std::vector<std::string> messages;
};

/// Elaborates a hand-built design and gives the test an arena and a writer over it.
class WriterFixture : public ::testing::Test {
protected:
    WriterFixture() : engine(sources) { engine.addConsumer(consumer); }

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory = std::filesystem::temp_directory_path()
                    / ("enzan-vcd-" + std::string(info->name()) + '-' + std::to_string(::getpid()));
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        ASSERT_TRUE(std::filesystem::create_directories(directory, error)) << error.message();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    /// Builds the image for `design` with `top` as its top level.
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
    }

    /// Returns a mutable view of a signal, so a test can change it as the kernel would.
    [[nodiscard]] enzan::eir::BitRef signal(std::string_view path) {
        const std::uint32_t index = image->findSignal(path);
        EXPECT_NE(index, Image::NoSignal) << path;
        const auto& info = image->signals()[index];
        return arena.at(info.wordOffset, info.bitWidth);
    }

    [[nodiscard]] std::uint32_t signalIndex(std::string_view path) const {
        return image->findSignal(path);
    }

    [[nodiscard]] std::filesystem::path outputPath() const { return directory / "dump.vcd"; }

    [[nodiscard]] std::string readOutput() const {
        std::ifstream file(outputPath());
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    /// Splits the output into lines, for assertions that care about structure.
    [[nodiscard]] std::vector<std::string> outputLines() const {
        std::vector<std::string> lines;
        std::istringstream file(readOutput());
        std::string line;
        while (std::getline(file, line)) lines.push_back(line);
        return lines;
    }

    [[nodiscard]] bool contains(std::string_view fragment) const {
        return readOutput().find(fragment) != std::string::npos;
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

// ---------------------------------------------------------------------------
// The header
// ---------------------------------------------------------------------------

TEST_F(WriterFixture, WritesAHeaderWithVersionTimescaleAndScopes) {
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale(-12));
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    EXPECT_TRUE(contains("$version Enzan "));
    EXPECT_TRUE(contains("$timescale 1ps $end"));
    EXPECT_TRUE(contains("$scope module tb $end"));
    EXPECT_TRUE(contains("$upscope $end"));
    EXPECT_TRUE(contains("$enddefinitions $end"));
}

TEST_F(WriterFixture, DeclaresEverySignalWithItsWidth) {
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale(-9));
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    EXPECT_EQ(writer.recordedSignals(), 2U);
    EXPECT_TRUE(contains(" 1 ")) << "the one-bit signal";
    EXPECT_TRUE(contains("clk $end"));
    EXPECT_TRUE(contains("count $end"));

    bool declaredWidth = false;
    for (const std::string& line : outputLines())
        declaredWidth = declaredWidth || line.starts_with("$var wire 8 ");
    EXPECT_TRUE(declaredWidth) << readOutput();
}

TEST_F(WriterFixture, TimescaleFollowsTheGlobalPrecision) {
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale(-15));
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    EXPECT_TRUE(contains("$timescale 1fs $end"));
}

TEST_F(WriterFixture, DumpsInitialValuesAtTimeZero) {
    // A viewer must show the state the design starts from rather than leaving every trace
    // undefined until its first change. A four-state object starts at X, so that is what
    // appears.
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    EXPECT_TRUE(contains("#0"));
    EXPECT_TRUE(contains("$dumpvars"));
    EXPECT_TRUE(contains("bxxxxxxxx ")) << readOutput();
}

TEST_F(WriterFixture, ReportsAPathThatCannotBeOpened) {
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    EXPECT_FALSE(writer.open(directory / "no-such-directory" / "dump.vcd", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(writer.isOpen());
}

// ---------------------------------------------------------------------------
// Value changes
// ---------------------------------------------------------------------------

TEST_F(WriterFixture, RecordsChangesAtTheirTimestamp) {
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    writer.onTimeAdvance(10);
    signal("/tb/clk").set(0, Logic::One);
    writer.onValueChange(signalIndex("/tb/clk"), signal("/tb/clk"));

    writer.onTimeAdvance(20);
    signal("/tb/clk").set(0, Logic::Zero);
    writer.onValueChange(signalIndex("/tb/clk"), signal("/tb/clk"));
    writer.close();

    const std::vector<std::string> lines = outputLines();
    const auto find = [&](std::string_view text) {
        return std::ranges::find(lines, text) != lines.end();
    };

    EXPECT_TRUE(find("#10"));
    EXPECT_TRUE(find("#20"));
    EXPECT_TRUE(contains("1!") || contains("1\"")) << readOutput();
}

TEST_F(WriterFixture, ChangesAreAttributedToTheSlotThatIsEndingNotTheOneBeginning) {
    // `onTimeAdvance` fires before anything at the new time runs, so the changes
    // accumulated so far belong to the previous timestamp. Getting this backwards shifts
    // every trace by one time slot, which is the kind of error a viewer makes look like a
    // design bug.
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    writer.onTimeAdvance(10);
    signal("/tb/count").fill(Logic::One);
    writer.onValueChange(signalIndex("/tb/count"), signal("/tb/count"));
    writer.onTimeAdvance(20);
    writer.close();

    const std::vector<std::string> lines = outputLines();
    std::size_t tenAt = lines.size();
    std::size_t valueAt = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index] == "#10") tenAt = index;
        if (lines[index].starts_with("b11111111 ")) valueAt = index;
    }

    ASSERT_LT(tenAt, lines.size()) << readOutput();
    ASSERT_LT(valueAt, lines.size()) << readOutput();
    EXPECT_LT(tenAt, valueAt) << "the value must follow its timestamp";
}

TEST_F(WriterFixture, SeveralChangesInOneSlotCollapseToTheSettledValue) {
    // Delta-level intermediate values are noise: recording them would make a diff of two
    // VCD files fail constantly, which section 14.3 warns is how a differential suite gets
    // switched off.
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    writer.onTimeAdvance(10);
    for (const char* value : {"8'h01", "8'h02", "8'h03"}) {
        const BitVector intermediate = literal(value);
        enzan::eir::bitops::copy(signal("/tb/count"), intermediate.ref());
        writer.onValueChange(signalIndex("/tb/count"), signal("/tb/count"));
    }
    writer.close();

    std::size_t emitted = 0;
    for (const std::string& line : outputLines()) {
        if (line.starts_with("b0000")) ++emitted;
    }
    EXPECT_EQ(emitted, 1U) << "only the settled value, once\n" << readOutput();
    EXPECT_TRUE(contains("b00000011 ")) << readOutput();
}

TEST_F(WriterFixture, NoTimestampIsEmittedForASlotWithNoChanges) {
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    writer.onTimeAdvance(10);
    writer.onTimeAdvance(20);
    writer.onTimeAdvance(30);
    writer.close();

    EXPECT_FALSE(contains("#10")) << readOutput();
    EXPECT_FALSE(contains("#30"));
}

TEST_F(WriterFixture, SingleBitSignalsUseTheScalarForm) {
    build(flatDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    writer.onTimeAdvance(5);
    signal("/tb/clk").set(0, Logic::HighZ);
    writer.onValueChange(signalIndex("/tb/clk"), signal("/tb/clk"));
    writer.close();

    // A scalar change is `z!`, never `bz !`.
    bool scalar = false;
    for (const std::string& line : outputLines())
        scalar = scalar || (line.size() >= 2 && line[0] == 'z' && line[1] != ' ');
    EXPECT_TRUE(scalar) << readOutput();
}

// ---------------------------------------------------------------------------
// Hierarchy and selective capture
// ---------------------------------------------------------------------------

/// A parent with a child whose port is collapsed onto the parent's object.
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

TEST_F(WriterFixture, NestedScopesAreOpenedAndClosed) {
    build(hierarchicalDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    const std::vector<std::string> lines = outputLines();
    std::size_t opens = 0;
    std::size_t closes = 0;
    for (const std::string& line : lines) {
        if (line.starts_with("$scope module ")) ++opens;
        if (line == "$upscope $end") ++closes;
    }

    EXPECT_EQ(opens, 2U) << "tb and u_dut";
    EXPECT_EQ(closes, opens) << "every scope must be closed\n" << readOutput();
    EXPECT_TRUE(contains("$scope module u_dut $end"));
}

TEST_F(WriterFixture, SiblingScopesWithACommonPrefixAreNotNested) {
    // A plain prefix comparison would put `/tb/u_dut2` inside `/tb/u_dut`, nesting the
    // scopes incorrectly. The separator has to be part of the comparison.
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

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    // tb, u_dut, u_dut2 opened; all three closed.
    std::size_t opens = 0;
    std::size_t closes = 0;
    for (const std::string& line : outputLines()) {
        if (line.starts_with("$scope module ")) ++opens;
        if (line == "$upscope $end") ++closes;
    }
    EXPECT_EQ(opens, 3U);
    EXPECT_EQ(closes, 3U) << readOutput();

    // u_dut must be closed before u_dut2 opens, or the second is nested in the first.
    const std::vector<std::string> lines = outputLines();
    std::size_t firstClose = lines.size();
    std::size_t secondOpen = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (firstClose == lines.size() && lines[index] == "$upscope $end") firstClose = index;
        if (lines[index] == "$scope module u_dut2 $end") secondOpen = index;
    }
    EXPECT_LT(firstClose, secondOpen) << readOutput();
}

TEST_F(WriterFixture, ScopeFilteringDoesNotCatchASiblingWithACommonPrefix) {
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

    VcdWriter writer(*image, arena, TimeScale());
    writer.setScope("/tb/u_dut");
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    EXPECT_EQ(writer.recordedSignals(), 1U) << "u_dut2 is a sibling, not a child";
}

TEST_F(WriterFixture, ACollapsedPortIsDeclaredUnderBothOfItsNames) {
    // One slot, two real hierarchical names, one identifier. A viewer that showed the
    // DUT's port only under the parent's name would be confusing, and VCD supports several
    // $var declarations sharing an identifier exactly for this.
    build(hierarchicalDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    std::string driveIdentifier;
    std::string inIdentifier;
    for (const std::string& line : outputLines()) {
        if (!line.starts_with("$var ")) continue;
        std::istringstream fields(line);
        std::string keyword;
        std::string kind;
        std::string width;
        std::string identifier;
        std::string name;
        fields >> keyword >> kind >> width >> identifier >> name;
        if (name == "drive") driveIdentifier = identifier;
        if (name == "in") inIdentifier = identifier;
    }

    ASSERT_FALSE(driveIdentifier.empty()) << readOutput();
    ASSERT_FALSE(inIdentifier.empty()) << readOutput();
    EXPECT_EQ(driveIdentifier, inIdentifier);
}

TEST_F(WriterFixture, ScopeFilteringRecordsOnlyTheSelectedSubtree) {
    // Selective capture is a requirement, not a convenience: recording everything on a
    // large design exhausts memory within seconds of simulated time (section 7.1).
    build(hierarchicalDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    writer.setScope("/tb/u_dut");
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    EXPECT_EQ(writer.recordedSignals(), 1U) << "only the child's own object";
    EXPECT_TRUE(contains("internal $end"));
    EXPECT_FALSE(contains("drive $end")) << readOutput();
}

TEST_F(WriterFixture, AChangeToAnUnrecordedSignalIsIgnored) {
    build(hierarchicalDesign(), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    writer.setScope("/tb/u_dut");
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;

    writer.onTimeAdvance(10);
    signal("/tb/drive").fill(Logic::One);
    writer.onValueChange(signalIndex("/tb/drive"), signal("/tb/drive"));
    writer.close();

    EXPECT_FALSE(contains("#10")) << "nothing recorded changed\n" << readOutput();
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

TEST_F(WriterFixture, IdentifiersStayShortForManySignals) {
    // VCD identifiers are printable ASCII from '!' to '~', 94 symbols. A base-94 encoding
    // keeps them one character for the first 94 signals, which is what makes a file for a
    // large design mostly values rather than mostly identifiers.
    ModuleDefinition definition;
    definition.name = "tb";
    definition.sourceName = "tb";
    for (int index = 0; index < 200; ++index)
        definition.addObject(Object{.name = "s" + std::to_string(index), .type = Type::logic()});

    Design design;
    design.addModule(std::move(definition));
    build(std::move(design), "tb");

    VcdWriter writer(*image, arena, TimeScale());
    std::string error;
    ASSERT_TRUE(writer.open(outputPath(), error)) << error;
    writer.close();

    std::size_t oneCharacter = 0;
    std::size_t twoCharacters = 0;
    for (const std::string& line : outputLines()) {
        if (!line.starts_with("$var ")) continue;
        std::istringstream fields(line);
        std::string keyword;
        std::string kind;
        std::string width;
        std::string identifier;
        fields >> keyword >> kind >> width >> identifier;
        if (identifier.size() == 1)
            ++oneCharacter;
        else if (identifier.size() == 2)
            ++twoCharacters;
    }

    EXPECT_EQ(oneCharacter, 94U);
    EXPECT_EQ(twoCharacters, 200U - 94U);
}

} // namespace
