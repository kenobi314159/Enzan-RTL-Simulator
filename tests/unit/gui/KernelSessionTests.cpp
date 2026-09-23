// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the GUI's end of the connection, against a real kernel over a real socket.
//
// This is the milestone's exit criterion written as a test: a design runs interactively, signals
// are added from the client, and the waveform progresses live. Everything below is some part of
// that sentence.

#include "gui/KernelSession.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QSignalSpy>

#include <optional>

#include "eir/BitVector.h"

namespace {

using enzan::gui::KernelSession;
using enzan::gui::SignalInfo;
using enzan::test::SessionFixture;

TEST_F(SessionFixture, TheHandshakeDescribesTheKernel) {
    EXPECT_TRUE(session->isAttached());
    EXPECT_EQ(session->topModule(), QStringLiteral("tb"));
    EXPECT_EQ(session->kernelVersion(), QStringLiteral("enzan test"));
    EXPECT_EQ(session->timeScale().precisionExponent(), -9);
    EXPECT_EQ(session->state(), enzan::ipc::SimulationState::Idle);
    EXPECT_EQ(session->currentTime(), 0U);
}

TEST_F(SessionFixture, TheHierarchyArrivesWholeWithItsShape) {
    ASSERT_EQ(session->instances().size(), 2);
    EXPECT_EQ(session->instances()[0].path, "/tb");
    EXPECT_EQ(session->instances()[0].parent, enzan::ipc::InstanceEntry::NoParent);
    EXPECT_EQ(session->instances()[1].path, "/tb/u_dut");
    EXPECT_EQ(session->instances()[1].parent, 0U);
}

TEST_F(SessionFixture, AScopeListsItsObjects) {
    const QVector<SignalInfo> objects = objectsOf(QStringLiteral("/tb"));

    ASSERT_EQ(objects.size(), 3);
    EXPECT_EQ(objects[0].name, QStringLiteral("clk"));
    EXPECT_EQ(objects[0].width, 1U);
    EXPECT_TRUE(objects[0].recorded);
    EXPECT_EQ(objects[1].name, QStringLiteral("count"));
    EXPECT_EQ(objects[1].width, 8U);
    EXPECT_EQ(objects[2].name, QStringLiteral("state"));
    EXPECT_EQ(objects[2].width, 3U);
}

TEST_F(SessionFixture, AnEnumeratedSignalArrivesWithTheNamesOfItsValues) {
    const QVector<SignalInfo> objects = objectsOf(QStringLiteral("/tb"));
    ASSERT_EQ(objects.size(), 3);

    const SignalInfo& state = objects[2];
    ASSERT_NE(state.enumeration, nullptr) << "an enumerated signal has to arrive with its names";
    EXPECT_EQ(state.enumeration->name, QStringLiteral("state_t"));

    ASSERT_EQ(state.enumeration->members.size(), 3);
    EXPECT_EQ(state.enumeration->members[0].name, QStringLiteral("S_IDLE"));
    EXPECT_EQ(state.enumeration->nameOf(*enzan::eir::BitVector::parse("3'd2")),
              QStringLiteral("S_SEND"));
    EXPECT_TRUE(state.enumeration->nameOf(*enzan::eir::BitVector::parse("3'd7")).isEmpty());

    // A signal whose type is not an enumeration carries nothing, rather than an empty one that
    // every reader of this field would then have to tell apart from a real one.
    EXPECT_EQ(objects[1].enumeration, nullptr);
}

TEST_F(SessionFixture, SubscribingBringsTheSignalsPastWithIt) {
    // A signal dragged into the wave window halfway through a run has to show what it has been
    // doing, not start from a blank trace.
    session->run(100);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 100; }));

    const std::uint32_t clock = signalIndex("/tb/clk");
    session->subscribe({clock});
    ASSERT_TRUE(pumpUntil([this, clock] {
        const std::uint32_t record = session->waveforms().recordIndexOf(clock);
        return record != enzan::wavedb::WaveformDatabase::NotRecorded
               && session->waveforms().changeCount(record) > 10;
    }));

    const std::uint32_t record = session->waveforms().recordIndexOf(clock);
    EXPECT_EQ(session->waveforms().timeOfChange(record, 0), 0U);
    EXPECT_GE(session->waveforms().changeCount(record), 20U)
        << "a clock that toggled twenty times arrived with less than its whole past";
}

TEST_F(SessionFixture, TheWaveformProgressesWhileTheDesignRuns) {
    // The milestone's exit criterion: values reach the client as the simulation advances,
    // rather than only when it stops.
    const std::uint32_t counter = signalIndex("/tb/count");
    session->subscribe({counter});
    ASSERT_TRUE(pumpUntil([this, counter] {
        return session->waveforms().recordIndexOf(counter)
               != enzan::wavedb::WaveformDatabase::NotRecorded;
    }));

    const std::uint32_t record = session->waveforms().recordIndexOf(counter);
    const std::uint64_t before = session->waveforms().changeCount(record);

    session->run(200);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 200; }));

    EXPECT_GT(session->waveforms().changeCount(record), before);
    EXPECT_GE(session->latestTime(), 200U);

    // The counter increments on each rising edge of a clock with a ten-unit period, so after
    // two hundred units it has counted twenty of them.
    EXPECT_EQ(session->waveforms().valueAt(record, 200),
              std::optional(enzan::eir::BitVector::fromUInt64(20, 8)));
}

TEST_F(SessionFixture, AnUnsubscribedSignalDoesNotArrive) {
    session->run(100);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 100; }));

    EXPECT_EQ(session->waveforms().recordIndexOf(signalIndex("/tb/count")),
              enzan::wavedb::WaveformDatabase::NotRecorded);
}

TEST_F(SessionFixture, RunStateReachesTheClientAtBothEndsOfARun) {
    const QSignalSpy states(session.get(), &KernelSession::runStateChanged);

    session->run(50);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 50; }));

    EXPECT_GE(states.count(), 2) << "a client must learn when a run starts, not only when it ends";
    EXPECT_EQ(session->state(), enzan::ipc::SimulationState::Idle);
}

TEST_F(SessionFixture, TheTranscriptReachesTheClient) {
    // The attach line the kernel writes is the first thing every session produces, so it is
    // already there by the time the fixture has finished connecting.
    QSignalSpy lines(session.get(), &KernelSession::transcriptReceived);
    session->run(10);
    pumpUntil([&] { return lines.count() > 0; }, 1000);

    EXPECT_FALSE(transcript.str().empty()) << "the kernel wrote nothing locally either";
}

TEST_F(SessionFixture, ARestartDiscardsTheTracesAndSaysSo) {
    const std::uint32_t clock = signalIndex("/tb/clk");
    session->subscribe({clock});
    session->run(100);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 100; }));

    const std::uint32_t before = session->waveforms().recordIndexOf(clock);
    ASSERT_NE(before, enzan::wavedb::WaveformDatabase::NotRecorded);
    ASSERT_GT(session->waveforms().changeCount(before), 10U);

    QSignalSpy cleared(session.get(), &KernelSession::wavesCleared);
    session->restart();
    ASSERT_TRUE(pumpUntil([&] { return cleared.count() > 0; }));

    EXPECT_EQ(session->currentTime(), 0U);

    // What the client holds is gone, and the fresh time-zero value takes its place.
    const std::uint32_t after = session->waveforms().recordIndexOf(clock);
    ASSERT_NE(after, enzan::wavedb::WaveformDatabase::NotRecorded);
    EXPECT_LE(session->waveforms().changeCount(after), 2U);
    EXPECT_EQ(session->waveforms().timeOfChange(after, 0), 0U);
}

TEST_F(SessionFixture, ARefusedRequestIsReportedWithoutDroppingTheConnection) {
    QSignalSpy failures(session.get(), &KernelSession::requestFailed);

    session->queryObjects(QStringLiteral("/tb/nowhere"));
    ASSERT_TRUE(pumpUntil([&] { return failures.count() > 0; }));

    EXPECT_TRUE(session->isAttached()) << "a refused request is not a broken connection";
}

TEST_F(SessionFixture, DetachingLeavesTheKernelAlive) {
    // Section 2.2's requirement: a front end must be able to leave a long run alone.
    session->run(50);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 50; }));

    const QSignalSpy detached(session.get(), &KernelSession::detached);
    session->detach();
    pump();

    EXPECT_EQ(detached.count(), 1);
    EXPECT_FALSE(session->isAttached());
    EXPECT_EQ(server->kernel().time(), 50U) << "detaching disturbed the simulation";
    EXPECT_TRUE(server->step(0)) << "a listening kernel keeps waiting for the next client";
}

TEST_F(SessionFixture, ASecondClientCanAttachAfterTheFirstLeaves) {
    session->run(50);
    ASSERT_TRUE(pumpUntil([this] { return session->currentTime() == 50; }));
    session->detach();
    pump();

    enzan::gui::KernelSession second;
    QString error;
    ASSERT_TRUE(second.attach(socketPath, error)) << error.toStdString();

    ASSERT_TRUE(pumpUntil([&] { return !second.instances().isEmpty(); }));
    EXPECT_EQ(second.currentTime(), 50U) << "the second client must see the run in progress";
    EXPECT_EQ(second.topModule(), QStringLiteral("tb"));
}

} // namespace
