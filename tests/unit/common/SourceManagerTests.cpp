// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/SourceManager.h"

#include "common/SourceLocation.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using enzan::BufferId;
using enzan::FileReadStatus;
using enzan::SourceLocation;
using enzan::SourceManager;

// ---------------------------------------------------------------------------
// Identifier and location value types
// ---------------------------------------------------------------------------

TEST(BufferId, DefaultConstructedIsInvalid) {
    EXPECT_FALSE(BufferId().valid());
}

TEST(BufferId, IndexRoundTrips) {
    EXPECT_EQ(BufferId::fromIndex(0).index(), 0U);
    EXPECT_EQ(BufferId::fromIndex(41).index(), 41U);
    EXPECT_TRUE(BufferId::fromIndex(0).valid());
}

TEST(SourceLocation, DefaultConstructedIsInvalid) {
    EXPECT_FALSE(SourceLocation().valid());
    EXPECT_EQ(SourceLocation().offset(), 0U);
}

TEST(SourceLocation, OrdersByBufferThenOffset) {
    const SourceLocation first(BufferId::fromIndex(0), 10);
    const SourceLocation later(BufferId::fromIndex(0), 20);
    const SourceLocation otherBuffer(BufferId::fromIndex(1), 0);

    EXPECT_LT(first, later);
    EXPECT_LT(later, otherBuffer);
    EXPECT_EQ(first, SourceLocation(BufferId::fromIndex(0), 10));
}

TEST(SourceLocation, AdvancedMovesWithinTheSameBuffer) {
    const SourceLocation start(BufferId::fromIndex(3), 100);
    const SourceLocation moved = start.advanced(5);

    EXPECT_EQ(moved.buffer(), start.buffer());
    EXPECT_EQ(moved.offset(), 105U);
}

TEST(SourceRange, ValidityRules) {
    const BufferId buffer = BufferId::fromIndex(0);
    const BufferId other = BufferId::fromIndex(1);

    EXPECT_FALSE(enzan::SourceRange().valid());
    EXPECT_TRUE(enzan::SourceRange(SourceLocation(buffer, 0), SourceLocation(buffer, 4)).valid());

    // An empty range denotes a zero-width position and is legitimate.
    EXPECT_TRUE(enzan::SourceRange(SourceLocation(buffer, 4), SourceLocation(buffer, 4)).valid());

    // Reversed endpoints and cross-buffer ranges are not representable.
    EXPECT_FALSE(enzan::SourceRange(SourceLocation(buffer, 5), SourceLocation(buffer, 4)).valid());
    EXPECT_FALSE(enzan::SourceRange(SourceLocation(buffer, 0), SourceLocation(other, 4)).valid());
}

TEST(SourceRange, LengthIsZeroWhenInvalid) {
    const BufferId buffer = BufferId::fromIndex(0);

    EXPECT_EQ(enzan::SourceRange(SourceLocation(buffer, 2), SourceLocation(buffer, 7)).length(),
              5U);
    EXPECT_EQ(enzan::SourceRange(SourceLocation(buffer, 7), SourceLocation(buffer, 2)).length(),
              0U);
    EXPECT_EQ(enzan::SourceRange().length(), 0U);
}

// ---------------------------------------------------------------------------
// In-memory buffers
// ---------------------------------------------------------------------------

TEST(SourceManager, StartsEmpty) {
    const SourceManager sources;
    EXPECT_EQ(sources.bufferCount(), 0U);
}

TEST(SourceManager, AddBufferRoundTripsNameAndText) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("design.sv", "module m; endmodule\n");

    ASSERT_TRUE(buffer.valid());
    EXPECT_EQ(sources.bufferCount(), 1U);
    EXPECT_EQ(sources.bufferName(buffer), "design.sv");
    EXPECT_EQ(sources.bufferText(buffer), "module m; endmodule\n");
}

TEST(SourceManager, IdentifiersAreAssignedInInsertionOrder) {
    SourceManager sources;
    const BufferId first = sources.addBuffer("a.sv", "");
    const BufferId second = sources.addBuffer("b.sv", "");

    // Ordering by identifier must be reproducible, because diagnostics are
    // grouped by buffer (DESIGN_GUIDE.md section 3.8).
    EXPECT_LT(first, second);
    EXPECT_EQ(first.index(), 0U);
    EXPECT_EQ(second.index(), 1U);
}

TEST(SourceManager, TextViewsSurviveLaterAdditions) {
    // The manager holds buffers indirectly precisely so this holds. A short
    // string would otherwise live inside the vector element and be moved when the
    // vector grew, leaving every previously returned view dangling — and the
    // failure would appear far from its cause.
    SourceManager sources;
    const BufferId first = sources.addBuffer("first.sv", "short");
    const std::string_view text = sources.bufferText(first);
    const std::string_view name = sources.bufferName(first);

    for (std::size_t index = 0; index < 64; ++index)
        sources.addBuffer("filler.sv", std::string(index, 'x'));

    EXPECT_EQ(text, "short");
    EXPECT_EQ(name, "first.sv");
}

// ---------------------------------------------------------------------------
// Invalid identifiers must degrade, not crash
// ---------------------------------------------------------------------------

TEST(SourceManager, InvalidBufferIdYieldsEmptyResults) {
    const SourceManager sources;
    const BufferId invalid;

    EXPECT_TRUE(sources.bufferName(invalid).empty());
    EXPECT_TRUE(sources.bufferText(invalid).empty());
    EXPECT_EQ(sources.lineCount(invalid), 0U);
    EXPECT_FALSE(sources.lineStart(invalid, 1).valid());
}

TEST(SourceManager, OutOfRangeBufferIdYieldsEmptyResults) {
    SourceManager sources;
    sources.addBuffer("only.sv", "x");
    const BufferId beyond = BufferId::fromIndex(99);

    EXPECT_TRUE(sources.bufferName(beyond).empty());
    EXPECT_TRUE(sources.bufferText(beyond).empty());
    EXPECT_EQ(sources.lineCount(beyond), 0U);
}

TEST(SourceManager, InvalidLocationYieldsZeroPosition) {
    const SourceManager sources;
    const SourceLocation invalid;

    EXPECT_EQ(sources.lineNumber(invalid), 0U);
    EXPECT_EQ(sources.columnNumber(invalid), 0U);
    EXPECT_TRUE(sources.lineText(invalid).empty());
}

// ---------------------------------------------------------------------------
// Line and column mapping
// ---------------------------------------------------------------------------

TEST(SourceManager, EmptyBufferHasOneEmptyLine) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("empty.sv", "");

    EXPECT_EQ(sources.lineCount(buffer), 1U);
    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 0)), 1U);
    EXPECT_EQ(sources.columnNumber(SourceLocation(buffer, 0)), 1U);
    EXPECT_TRUE(sources.lineText(SourceLocation(buffer, 0)).empty());
}

TEST(SourceManager, MapsOffsetsToLinesAndColumns) {
    SourceManager sources;
    //                                     0123 456789
    const BufferId buffer = sources.addBuffer("test.sv", "abc\ndefg\nhi");

    EXPECT_EQ(sources.lineCount(buffer), 3U);

    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 0)), 1U);
    EXPECT_EQ(sources.columnNumber(SourceLocation(buffer, 0)), 1U);
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 0)), "abc");

    // The newline itself belongs to the line it terminates.
    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 3)), 1U);
    EXPECT_EQ(sources.columnNumber(SourceLocation(buffer, 3)), 4U);

    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 4)), 2U);
    EXPECT_EQ(sources.columnNumber(SourceLocation(buffer, 4)), 1U);
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 6)), "defg");
    EXPECT_EQ(sources.columnNumber(SourceLocation(buffer, 6)), 3U);

    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 9)), 3U);
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 9)), "hi");
}

TEST(SourceManager, TerminatingNewlineDoesNotStartAnotherLine) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("test.sv", "a\nb\nc\n");

    EXPECT_EQ(sources.lineCount(buffer), 3U);
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 4)), "c");

    // A location at the very end of the buffer stays on the last real line.
    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 6)), 3U);
}

TEST(SourceManager, BlankLinesInTheMiddleAreCounted) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("test.sv", "a\n\nb\n");

    EXPECT_EQ(sources.lineCount(buffer), 3U);
    EXPECT_TRUE(sources.lineText(SourceLocation(buffer, 2)).empty());
    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 2)), 2U);
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 3)), "b");
}

TEST(SourceManager, StripsCarriageReturnFromLineText) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("crlf.sv", "module m;\r\nendmodule\r\n");

    EXPECT_EQ(sources.lineCount(buffer), 2U);
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 0)), "module m;");
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 11)), "endmodule");

    // Offsets still count the carriage return, because they are byte offsets into
    // the file exactly as it was read.
    EXPECT_EQ(sources.columnNumber(SourceLocation(buffer, 9)), 10U);
}

TEST(SourceManager, LoneCarriageReturnIsNotALineBreak) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("cr.sv", "a\rb\n");

    EXPECT_EQ(sources.lineCount(buffer), 1U);
    EXPECT_EQ(sources.lineText(SourceLocation(buffer, 0)), "a\rb");
}

TEST(SourceManager, OffsetBeyondTheEndClampsToTheLastLine) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("test.sv", "abc");

    // A malformed location must not read out of range: the position is
    // approximate, but reporting an error is more important than reporting it
    // perfectly.
    EXPECT_EQ(sources.lineNumber(SourceLocation(buffer, 1000)), 1U);
    EXPECT_EQ(sources.columnNumber(SourceLocation(buffer, 1000)), 4U);
}

TEST(SourceManager, LineStartLocatesTheBeginningOfEachLine) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("test.sv", "abc\ndefg\nhi");

    EXPECT_EQ(sources.lineStart(buffer, 1).offset(), 0U);
    EXPECT_EQ(sources.lineStart(buffer, 2).offset(), 4U);
    EXPECT_EQ(sources.lineStart(buffer, 3).offset(), 9U);
    EXPECT_EQ(sources.lineStart(buffer, 1).buffer(), buffer);
}

TEST(SourceManager, LineStartRejectsLinesOutsideTheBuffer) {
    SourceManager sources;
    const BufferId buffer = sources.addBuffer("test.sv", "abc\n");

    EXPECT_FALSE(sources.lineStart(buffer, 0).valid());
    EXPECT_FALSE(sources.lineStart(buffer, 2).valid());
    EXPECT_FALSE(sources.lineStart(buffer, 1000).valid());
}

TEST(SourceManager, MaxBufferSizeIsAddressableByASourceLocation) {
    // The limit exists because SourceLocation holds a 32-bit offset; if the two
    // ever disagree, a large file would be silently misattributed.
    EXPECT_LE(SourceManager::MaxBufferSize,
              static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
}

// ---------------------------------------------------------------------------
// Reading files
// ---------------------------------------------------------------------------

/// Gives each test its own temporary directory, named after the test so that a
/// leftover directory identifies what left it behind.
class SourceManagerFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory = std::filesystem::temp_directory_path()
                    / ("enzan-" + std::string(info->name()) + '-' + std::to_string(::getpid()));

        std::error_code error;
        std::filesystem::remove_all(directory, error);
        ASSERT_TRUE(std::filesystem::create_directories(directory, error)) << error.message();
    }

    void TearDown() override {
        // Restore permissions first: a test that made a file unreadable would
        // otherwise leave a directory that cannot be cleaned up.
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error))
            std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, error);
        std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path writeFile(std::string_view name, std::string_view content) {
        const std::filesystem::path path = directory / name;
        std::ofstream stream(path, std::ios::binary);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.close();
        EXPECT_TRUE(std::filesystem::exists(path));
        return path;
    }

    std::filesystem::path directory;
};

TEST_F(SourceManagerFileTest, ReadsAFileAndNamesItByPath) {
    const std::filesystem::path path = writeFile("counter.sv", "module counter;\nendmodule\n");

    SourceManager sources;
    const enzan::FileReadResult result = sources.readFile(path);

    ASSERT_TRUE(result.ok()) << result.systemMessage;
    EXPECT_EQ(result.status, FileReadStatus::Success);
    EXPECT_TRUE(result.systemMessage.empty());
    EXPECT_EQ(sources.bufferText(result.buffer), "module counter;\nendmodule\n");
    EXPECT_EQ(sources.bufferName(result.buffer), path.string());
    EXPECT_EQ(sources.lineCount(result.buffer), 2U);
}

TEST_F(SourceManagerFileTest, ReadsAnEmptyFile) {
    const std::filesystem::path path = writeFile("empty.sv", "");

    SourceManager sources;
    const enzan::FileReadResult result = sources.readFile(path);

    ASSERT_TRUE(result.ok()) << result.systemMessage;
    EXPECT_TRUE(sources.bufferText(result.buffer).empty());
    EXPECT_EQ(sources.lineCount(result.buffer), 1U);
}

TEST_F(SourceManagerFileTest, PreservesBytesExactly) {
    // No encoding conversion and no line-ending translation: offsets have to
    // correspond to the bytes on disk, or every diagnostic position drifts.
    const std::string content("a\0b\r\nc\xC3\xA9", 9);
    const std::filesystem::path path = writeFile("bytes.sv", content);

    SourceManager sources;
    const enzan::FileReadResult result = sources.readFile(path);

    ASSERT_TRUE(result.ok()) << result.systemMessage;
    EXPECT_EQ(sources.bufferText(result.buffer), content);
}

TEST_F(SourceManagerFileTest, ReadingTheSamePathTwiceProducesTwoBuffers) {
    const std::filesystem::path path = writeFile("twice.sv", "x\n");

    SourceManager sources;
    const enzan::FileReadResult first = sources.readFile(path);
    const enzan::FileReadResult second = sources.readFile(path);

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_NE(first.buffer, second.buffer);
    EXPECT_EQ(sources.bufferCount(), 2U);
}

TEST_F(SourceManagerFileTest, ReportsAMissingFileAsNotFound) {
    SourceManager sources;
    const enzan::FileReadResult result = sources.readFile(directory / "absent.sv");

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status, FileReadStatus::NotFound);
    EXPECT_FALSE(result.buffer.valid());
    EXPECT_FALSE(result.systemMessage.empty()) << "the system's reason must be preserved";
    EXPECT_EQ(sources.bufferCount(), 0U);
}

TEST_F(SourceManagerFileTest, ReportsADirectoryAsNotARegularFile) {
    // A directory opens successfully and then fails to read, so the type has to
    // be checked up front to produce a message that says what is actually wrong.
    SourceManager sources;
    const enzan::FileReadResult result = sources.readFile(directory);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status, FileReadStatus::NotARegularFile);
    EXPECT_FALSE(result.buffer.valid());
}

TEST_F(SourceManagerFileTest, ReportsAnUnreadableFileAsPermissionDenied) {
    if (::geteuid() == 0) GTEST_SKIP() << "file permissions do not restrict root";

    const std::filesystem::path path = writeFile("secret.sv", "x\n");
    std::error_code error;
    std::filesystem::permissions(path, std::filesystem::perms::none, error);
    ASSERT_FALSE(error) << error.message();

    SourceManager sources;
    const enzan::FileReadResult result = sources.readFile(path);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status, FileReadStatus::PermissionDenied);
    EXPECT_FALSE(result.buffer.valid());
}

} // namespace
