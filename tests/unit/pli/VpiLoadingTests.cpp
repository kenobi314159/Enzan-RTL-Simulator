// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loading an application (DESIGN_GUIDE.md section 9.1), with real shared libraries built beside
// this test. The test executable exports the `vpi_*` routines the same way `enzan` does, so
// these also check that a library can resolve against a host that exports only those.

#include "pli/PliContext.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"

#include "support/Simulation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <string>

namespace {

using enzan::DiagCode;
using enzan::pli::PliContext;

class VpiLoading : public ::testing::Test {
protected:
    VpiLoading() : engine(sources), context(engine, {"enzan"}) {
        engine.addConsumer(consumer);
        context.setTranscript(&transcript);
    }

    [[nodiscard]] bool reported(DiagCode code) const {
        return std::ranges::find(consumer.codes, code) != consumer.codes.end();
    }

    enzan::SourceManager sources;
    enzan::DiagnosticEngine engine;
    enzan::test::CollectingConsumer consumer;
    std::ostringstream transcript;
    PliContext context;
};

TEST_F(VpiLoading, TheStartupRoutinesRunInOrder) {
    ASSERT_TRUE(context.load(ENZAN_TEST_STARTUP_PLUGIN));
    EXPECT_EQ(transcript.str(), "first startup routine ran\nsecond startup routine ran\n");
    EXPECT_TRUE(context.hasApplications());
}

TEST_F(VpiLoading, ANamedEntryRunsInsteadOfTheStartupRoutines) {
    ASSERT_TRUE(context.load(std::string(ENZAN_TEST_STARTUP_PLUGIN) + ":alternate_entry"));
    EXPECT_EQ(transcript.str(), "alternate entry ran\n");
}

TEST_F(VpiLoading, ALibraryThatCannotBeOpenedIsReported) {
    EXPECT_FALSE(context.load("/nonexistent/libnothing.so"));
    EXPECT_TRUE(reported(DiagCode::VpiLibraryUnloadable));
    EXPECT_FALSE(context.hasApplications());
}

TEST_F(VpiLoading, AnEntryTheLibraryLacksIsReported) {
    EXPECT_FALSE(context.load(std::string(ENZAN_TEST_STARTUP_PLUGIN) + ":no_such_entry"));
    EXPECT_TRUE(reported(DiagCode::VpiEntryNotFound));
    EXPECT_FALSE(context.hasApplications()) << "opened, but nothing in it was started";
}

TEST_F(VpiLoading, ALibraryWithNoStartupRoutinesIsReported) {
    EXPECT_FALSE(context.load(ENZAN_TEST_SILENT_PLUGIN));
    EXPECT_TRUE(reported(DiagCode::VpiNoStartupRoutines));
    EXPECT_FALSE(context.hasApplications()) << "opened, but nothing in it was started";
}

} // namespace
