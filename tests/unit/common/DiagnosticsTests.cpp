// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Diagnostics.h"

#include "common/SourceManager.h"

#include <gtest/gtest.h>

#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using enzan::BufferId;
using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::DiagnosticSeverity;
using enzan::SourceLocation;
using enzan::SourceManager;
using enzan::SourceRange;
using enzan::TextDiagnosticConsumer;

/// Every code in the catalogue, generated from the catalogue itself so that a
/// newly added code is covered by the integrity tests below without anyone having
/// to remember to list it.
constexpr DiagCode AllCodes[] = {
#define ENZAN_DIAG(name, number, severity, format) DiagCode::name,
#include "common/DiagnosticCodes.def"
#undef ENZAN_DIAG
};

/// Collects what the engine delivers, so that tests can assert on the diagnostic
/// itself rather than on rendered text.
class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

// ---------------------------------------------------------------------------
// The catalogue
// ---------------------------------------------------------------------------

TEST(DiagnosticCatalogue, CodesRenderAsStableIdentifiers) {
    EXPECT_EQ(enzan::toString(DiagCode::CannotOpenFile), "ENZ0001");
    EXPECT_EQ(enzan::toString(DiagCode::NotYetImplemented), "ENZ9001");
}

TEST(DiagnosticCatalogue, EveryCodeRendersAsEnzPlusFourDigits) {
    for (const DiagCode code : AllCodes) {
        const std::string text = enzan::toString(code);
        ASSERT_GE(text.size(), 7U) << text;
        EXPECT_EQ(text.substr(0, 3), "ENZ") << text;
        for (const char digit : text.substr(3)) EXPECT_TRUE(digit >= '0' && digit <= '9') << text;
    }
}

TEST(DiagnosticCatalogue, CodeNumbersAreUnique) {
    // A reused number silently changes what a user's suppression list suppresses,
    // which is why DiagnosticCodes.def forbids it. Checking it here means the rule
    // is enforced rather than merely written down.
    std::set<unsigned> seen;
    for (const DiagCode code : AllCodes) {
        const auto number = static_cast<unsigned>(code);
        EXPECT_TRUE(seen.insert(number).second) << "ENZ" << number << " is used more than once";
    }
}

TEST(DiagnosticCatalogue, EveryCodeHasAMessageAndAMeaningfulSeverity) {
    for (const DiagCode code : AllCodes) {
        EXPECT_FALSE(enzan::formatString(code).empty()) << enzan::toString(code);

        // Ignored is a user's decision, never a catalogue default: a diagnostic
        // that is suppressed by definition should not exist.
        EXPECT_NE(enzan::defaultSeverity(code), DiagnosticSeverity::Ignored)
            << enzan::toString(code);
    }
}

TEST(DiagnosticCatalogue, KnownSeveritiesAreAsDocumented) {
    EXPECT_EQ(enzan::defaultSeverity(DiagCode::CannotOpenFile), DiagnosticSeverity::Error);
    EXPECT_EQ(enzan::defaultSeverity(DiagCode::TooManyErrors), DiagnosticSeverity::Fatal);
}

TEST(DiagnosticSeverityNames, AreTheWordsUsedInOutput) {
    EXPECT_EQ(enzan::toString(DiagnosticSeverity::Note), "note");
    EXPECT_EQ(enzan::toString(DiagnosticSeverity::Warning), "warning");
    EXPECT_EQ(enzan::toString(DiagnosticSeverity::Error), "error");
    EXPECT_EQ(enzan::toString(DiagnosticSeverity::Fatal), "fatal error");
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

class DiagnosticEngineTest : public ::testing::Test {
protected:
    DiagnosticEngineTest() : engine(sources) { engine.addConsumer(consumer); }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
};

TEST_F(DiagnosticEngineTest, FormatsAndDeliversADiagnostic) {
    engine.report(DiagCode::CannotOpenFile, SourceLocation(), "fifo.sv", "No such file");

    ASSERT_EQ(consumer.received.size(), 1U);
    const Diagnostic& diagnostic = consumer.received.front();
    EXPECT_EQ(diagnostic.code, DiagCode::CannotOpenFile);
    EXPECT_EQ(diagnostic.severity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.message, "cannot open file 'fifo.sv': No such file");
    EXPECT_FALSE(diagnostic.location.valid());
}

TEST_F(DiagnosticEngineTest, CarriesTheReportedLocation) {
    const BufferId buffer = sources.addBuffer("a.sv", "wire x;\n");
    const SourceLocation location(buffer, 5);

    engine.report(DiagCode::NotYetImplemented, location, "nets");

    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().location, location);
}

TEST_F(DiagnosticEngineTest, CountsBySeverity) {
    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "one");
    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "two");

    EXPECT_EQ(engine.count(DiagnosticSeverity::Error), 2U);
    EXPECT_EQ(engine.errorCount(), 2U);
    EXPECT_EQ(engine.warningCount(), 0U);
    EXPECT_TRUE(engine.hasErrors());
}

TEST_F(DiagnosticEngineTest, FatalCountsTowardsErrors) {
    engine.report(DiagCode::InternalError, SourceLocation(), "broken");

    EXPECT_EQ(engine.count(DiagnosticSeverity::Fatal), 1U);
    EXPECT_EQ(engine.errorCount(), 1U);
    EXPECT_TRUE(engine.hasErrors());
}

TEST_F(DiagnosticEngineTest, NotesAreKeptInTheOrderTheyWereAdded) {
    const BufferId buffer = sources.addBuffer("a.sv", "abc\n");

    engine.report(DiagCode::NotYetImplemented, SourceLocation(buffer, 0), "thing")
        .note(SourceLocation(buffer, 1), "first")
        .note(SourceLocation(), "second");

    ASSERT_EQ(consumer.received.size(), 1U);
    const std::vector<enzan::DiagnosticNote>& notes = consumer.received.front().notes;
    ASSERT_EQ(notes.size(), 2U);
    EXPECT_EQ(notes[0].message, "first");
    EXPECT_EQ(notes[1].message, "second");
    EXPECT_FALSE(notes[1].location.valid());
}

TEST_F(DiagnosticEngineTest, HighlightRangeIsCarriedThrough) {
    const BufferId buffer = sources.addBuffer("a.sv", "abcdef\n");
    const SourceRange range(SourceLocation(buffer, 1), SourceLocation(buffer, 4));

    engine.report(DiagCode::NotYetImplemented, SourceLocation(buffer, 1), "thing").highlight(range);

    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().range, range);
}

TEST_F(DiagnosticEngineTest, AMovedBuilderReportsExactlyOnce) {
    // The builder reports when it is destroyed, so moving one out of a helper
    // function must not double-report.
    const auto makeReport = [this] {
        return engine.report(DiagCode::NotYetImplemented, SourceLocation(), "moved");
    };
    { const enzan::DiagnosticBuilder builder = makeReport(); }

    EXPECT_EQ(consumer.received.size(), 1U);
}

TEST_F(DiagnosticEngineTest, MismatchedArgumentsProduceAPlaceholderInsteadOfThrowing) {
    // A caller that does not match the catalogue's format string is a defect, but
    // it surfaces while something else is already going wrong, so the report must
    // still reach the user rather than terminating the process.
    EXPECT_NO_THROW(engine.report(DiagCode::CannotOpenFile, SourceLocation()));

    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_NE(consumer.received.front().message.find("ENZ0001"), std::string::npos);
    EXPECT_NE(consumer.received.front().message.find("do not match"), std::string::npos);
    EXPECT_EQ(engine.errorCount(), 1U);
}

// ---------------------------------------------------------------------------
// Consumers
// ---------------------------------------------------------------------------

TEST_F(DiagnosticEngineTest, DeliversToConsumersInRegistrationOrder) {
    // Order is observable in the transcript, so it is part of the determinism guarantee.
    std::vector<int> order;

    class RecordingConsumer final : public enzan::DiagnosticConsumer {
    public:
        RecordingConsumer(std::vector<int>& target, int identifier) : log(target), id(identifier) {}

        void consume(const Diagnostic& /*diagnostic*/) override { log.push_back(id); }

    private:
        std::vector<int>& log;
        int id;
    };

    RecordingConsumer first(order, 1);
    RecordingConsumer second(order, 2);
    engine.addConsumer(first);
    engine.addConsumer(second);

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");

    EXPECT_EQ(order, std::vector<int>({1, 2}));
}

TEST_F(DiagnosticEngineTest, RemovedConsumersStopReceiving) {
    engine.removeConsumer(consumer);
    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");

    EXPECT_TRUE(consumer.received.empty());
    EXPECT_EQ(engine.errorCount(), 1U) << "counting is independent of delivery";
}

TEST_F(DiagnosticEngineTest, RemovingAnUnregisteredConsumerIsHarmless) {
    CollectingConsumer stranger;
    engine.removeConsumer(stranger);

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");
    EXPECT_EQ(consumer.received.size(), 1U);
}

// ---------------------------------------------------------------------------
// Severity remapping and suppression
// ---------------------------------------------------------------------------

TEST_F(DiagnosticEngineTest, SeverityCanBeRemapped) {
    ASSERT_TRUE(engine.setSeverity(DiagCode::NotYetImplemented, DiagnosticSeverity::Warning));
    EXPECT_EQ(engine.severity(DiagCode::NotYetImplemented), DiagnosticSeverity::Warning);

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");

    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(engine.warningCount(), 1U);
    EXPECT_EQ(engine.errorCount(), 0U);
    EXPECT_FALSE(engine.hasErrors());
}

TEST_F(DiagnosticEngineTest, SuppressedCodesAreNeitherDeliveredNorCounted) {
    ASSERT_TRUE(engine.suppress(DiagCode::NotYetImplemented));

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");

    EXPECT_TRUE(consumer.received.empty());
    EXPECT_EQ(engine.errorCount(), 0U);
}

TEST_F(DiagnosticEngineTest, SuppressionDoesNotAffectOtherCodes) {
    ASSERT_TRUE(engine.suppress(DiagCode::NotYetImplemented));

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");
    engine.report(DiagCode::NotARegularFile, SourceLocation(), "/tmp");

    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().code, DiagCode::NotARegularFile);
}

TEST_F(DiagnosticEngineTest, FatalDiagnosticsCannotBeDowngradedOrSuppressed) {
    // Downgrading a Fatal turns a clean stop into wrong results, which is the
    // failure mode the project weighs most heavily.
    EXPECT_FALSE(engine.setSeverity(DiagCode::InternalError, DiagnosticSeverity::Warning));
    EXPECT_FALSE(engine.suppress(DiagCode::InternalError));
    EXPECT_EQ(engine.severity(DiagCode::InternalError), DiagnosticSeverity::Fatal);

    engine.report(DiagCode::InternalError, SourceLocation(), "still fatal");

    ASSERT_EQ(consumer.received.size(), 1U);
    EXPECT_EQ(consumer.received.front().severity, DiagnosticSeverity::Fatal);
}

TEST_F(DiagnosticEngineTest, ClearingAnOverrideRestoresTheCatalogueSeverity) {
    ASSERT_TRUE(engine.setSeverity(DiagCode::NotYetImplemented, DiagnosticSeverity::Warning));
    engine.clearSeverity(DiagCode::NotYetImplemented);

    EXPECT_EQ(engine.severity(DiagCode::NotYetImplemented), DiagnosticSeverity::Error);
}

TEST_F(DiagnosticEngineTest, ClearingAnAbsentOverrideIsHarmless) {
    engine.clearSeverity(DiagCode::NotYetImplemented);
    EXPECT_EQ(engine.severity(DiagCode::NotYetImplemented), DiagnosticSeverity::Error);
}

// ---------------------------------------------------------------------------
// The error limit
// ---------------------------------------------------------------------------

TEST_F(DiagnosticEngineTest, NoLimitIsAppliedByDefault) {
    EXPECT_EQ(engine.errorLimit(), 0U);

    for (int index = 0; index < 50; ++index)
        engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");

    EXPECT_EQ(engine.errorCount(), 50U);
    EXPECT_FALSE(engine.errorLimitReached());
}

TEST_F(DiagnosticEngineTest, ReachingTheErrorLimitReportsItAndStops) {
    engine.setErrorLimit(2);

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "one");
    EXPECT_FALSE(engine.errorLimitReached());

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "two");
    ASSERT_TRUE(engine.errorLimitReached());

    // The two errors plus the overflow report itself.
    ASSERT_EQ(consumer.received.size(), 3U);
    EXPECT_EQ(consumer.received.back().code, DiagCode::TooManyErrors);
    EXPECT_EQ(consumer.received.back().severity, DiagnosticSeverity::Fatal);

    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "three");
    EXPECT_EQ(consumer.received.size(), 3U) << "reporting must stop once the limit is reached";
}

TEST_F(DiagnosticEngineTest, WarningsDoNotCountTowardsTheErrorLimit) {
    ASSERT_TRUE(engine.setSeverity(DiagCode::NotYetImplemented, DiagnosticSeverity::Warning));
    engine.setErrorLimit(1);

    for (int index = 0; index < 5; ++index)
        engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");

    EXPECT_FALSE(engine.errorLimitReached());
    EXPECT_EQ(engine.warningCount(), 5U);
}

TEST_F(DiagnosticEngineTest, ResetClearsCountsButKeepsOverridesAndConsumers) {
    ASSERT_TRUE(engine.setSeverity(DiagCode::NotYetImplemented, DiagnosticSeverity::Warning));
    engine.setErrorLimit(1);
    engine.report(DiagCode::NotARegularFile, SourceLocation(), "/tmp");
    ASSERT_TRUE(engine.errorLimitReached());

    engine.reset();

    EXPECT_EQ(engine.errorCount(), 0U);
    EXPECT_EQ(engine.warningCount(), 0U);
    EXPECT_FALSE(engine.errorLimitReached());
    EXPECT_EQ(engine.severity(DiagCode::NotYetImplemented), DiagnosticSeverity::Warning)
        << "overrides belong to the session, not to a run";

    const std::size_t before = consumer.received.size();
    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "x");
    EXPECT_EQ(consumer.received.size(), before + 1);
}

// ---------------------------------------------------------------------------
// Text rendering
// ---------------------------------------------------------------------------

class TextDiagnosticTest : public ::testing::Test {
protected:
    TextDiagnosticTest() : engine(sources), consumer(sources, output) {
        engine.addConsumer(consumer);
    }

    std::string rendered() const { return output.str(); }

    SourceManager sources;
    DiagnosticEngine engine;
    std::ostringstream output;
    TextDiagnosticConsumer consumer;
};

TEST_F(TextDiagnosticTest, RendersPositionSeverityMessageAndCode) {
    const BufferId buffer = sources.addBuffer("counter.sv", "module counter;\nendmodule\n");

    engine.report(DiagCode::CannotOpenFile, SourceLocation(buffer, 7), "fifo.sv", "No such file");

    EXPECT_EQ(rendered(),
              "counter.sv:1:8: error: cannot open file 'fifo.sv': No such file [ENZ0001]\n"
              "   1 | module counter;\n"
              "     |        ^\n");
}

TEST_F(TextDiagnosticTest, UnderlinesAHighlightedRange) {
    const BufferId buffer = sources.addBuffer("a.sv", "wire [7:0] result;\n");
    const SourceLocation start(buffer, 11);

    engine.report(DiagCode::NotYetImplemented, start, "that")
        .highlight(SourceRange(start, start.advanced(6)));

    EXPECT_EQ(rendered(), "a.sv:1:12: error: that is not yet implemented [ENZ9001]\n"
                          "   1 | wire [7:0] result;\n"
                          "     |            ^~~~~~\n");
}

TEST_F(TextDiagnosticTest, ExpandsTabsSoTheCaretStaysAligned) {
    // The caret has to line up with the text above it, and the terminal's real
    // tab stops are unknowable, so a fixed width is assumed.
    const BufferId buffer = sources.addBuffer("a.sv", "\twire x;\n");

    engine.report(DiagCode::NotYetImplemented, SourceLocation(buffer, 6), "that");

    EXPECT_EQ(rendered(), std::string("a.sv:1:7: error: that is not yet implemented [ENZ9001]\n")
                              + "   1 | " + std::string(TextDiagnosticConsumer::TabWidth, ' ')
                              + "wire x;\n" + "     | "
                              + std::string(TextDiagnosticConsumer::TabWidth + 5, ' ') + "^\n");
}

TEST_F(TextDiagnosticTest, RendersADiagnosticWithNoLocation) {
    engine.report(DiagCode::NotYetImplemented, SourceLocation(), "VHDL analysis");

    EXPECT_EQ(rendered(), "enzan: error: VHDL analysis is not yet implemented [ENZ9001]\n");
}

TEST_F(TextDiagnosticTest, RendersNotesAfterTheDiagnostic) {
    const BufferId buffer = sources.addBuffer("a.sv", "abc\ndef\n");

    engine.report(DiagCode::NotYetImplemented, SourceLocation(buffer, 0), "that")
        .note(SourceLocation(buffer, 4), "see here")
        .note(SourceLocation(), "and this");

    EXPECT_EQ(rendered(), "a.sv:1:1: error: that is not yet implemented [ENZ9001]\n"
                          "   1 | abc\n"
                          "     | ^\n"
                          "a.sv:2:1: note: see here\n"
                          "   2 | def\n"
                          "     | ^\n"
                          "enzan: note: and this\n");
}

TEST_F(TextDiagnosticTest, SnippetsCanBeTurnedOff) {
    const BufferId buffer = sources.addBuffer("a.sv", "abc\n");
    consumer.setShowSourceSnippets(false);
    ASSERT_FALSE(consumer.showSourceSnippets());

    engine.report(DiagCode::NotYetImplemented, SourceLocation(buffer, 1), "that")
        .note(SourceLocation(buffer, 2), "here");

    EXPECT_EQ(rendered(), "a.sv:1:2: error: that is not yet implemented [ENZ9001]\n"
                          "a.sv:1:3: note: here\n");
}

TEST_F(TextDiagnosticTest, WidensTheGutterForLongFiles) {
    std::string text;
    for (int line = 0; line < 12000; ++line) text += "x\n";
    const BufferId buffer = sources.addBuffer("big.sv", text);

    const SourceLocation location = sources.lineStart(buffer, 11000);
    ASSERT_TRUE(location.valid());
    engine.report(DiagCode::NotYetImplemented, location, "that");

    EXPECT_NE(rendered().find("11000 | x\n"), std::string::npos) << rendered();
}

// ---------------------------------------------------------------------------
// reportFileReadFailure
// ---------------------------------------------------------------------------

TEST(ReportFileReadFailure, MapsEachCauseToItsOwnCode) {
    struct Case {
        enzan::FileReadStatus status;
        DiagCode expected;
    };

    // Success is included deliberately: a caller that forgot to check `ok()` must still get a
    // diagnostic rather than silence, because the alternative is a phase continuing with an
    // invalid buffer.
    constexpr Case Cases[] = {
        {.status = enzan::FileReadStatus::NotFound, .expected = DiagCode::CannotOpenFile},
        {.status = enzan::FileReadStatus::NotARegularFile, .expected = DiagCode::NotARegularFile},
        {.status = enzan::FileReadStatus::TooLarge, .expected = DiagCode::SourceFileTooLarge},
        {.status = enzan::FileReadStatus::PermissionDenied, .expected = DiagCode::FileReadFailed},
        {.status = enzan::FileReadStatus::ReadFailed, .expected = DiagCode::FileReadFailed},
        {.status = enzan::FileReadStatus::Success, .expected = DiagCode::FileReadFailed},
    };

    for (const Case& testCase : Cases) {
        const SourceManager sources;
        DiagnosticEngine engine(sources);
        CollectingConsumer consumer;
        engine.addConsumer(consumer);

        enzan::FileReadResult result;
        result.status = testCase.status;
        result.systemMessage = "no such file or directory";
        enzan::reportFileReadFailure(engine, "design.sv", result);

        ASSERT_EQ(consumer.received.size(), 1U);
        EXPECT_EQ(consumer.received.front().code, testCase.expected);
        EXPECT_NE(consumer.received.front().message.find("design.sv"), std::string::npos)
            << consumer.received.front().message;
    }
}

} // namespace
