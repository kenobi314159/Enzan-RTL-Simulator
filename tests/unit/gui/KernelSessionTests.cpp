// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the GUI's end of the connection, against a real kernel over a real socket.
//
// This is the milestone's exit criterion written as a test: a design runs interactively, signals
// are added from the client, and the waveform progresses live. Everything below is some part of
// that sentence.

#include "gui/KernelSession.h"

#include "gui/ObjectsModel.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QSignalSpy>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "eir/BitVector.h"

namespace {

using enzan::gui::KernelSession;
using enzan::gui::ObjectsModel;
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
    EXPECT_EQ(objects[0].recording, enzan::ipc::Recording::Recorded);
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
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_EQ(state.enumeration->nameOf(*enzan::eir::BitVector::parse("3'd2")),
              QStringLiteral("S_SEND"));
    EXPECT_TRUE(state.enumeration->nameOf(*enzan::eir::BitVector::parse("3'd7")).isEmpty());
    // NOLINTEND(bugprone-unchecked-optional-access)

    // A signal whose type is not an enumeration carries nothing, rather than an empty one that
    // every reader of this field would then have to tell apart from a real one.
    EXPECT_EQ(objects[1].enumeration, nullptr);
}

TEST_F(SessionFixture, AScopeListsItsParametersAfterItsSignals) {
    const QVector<SignalInfo> objects = objectsOf(QStringLiteral("/tb/u_dut"));

    // The child's one signal, then its parameter and its localparam in declaration order.
    ASSERT_EQ(objects.size(), 3);
    EXPECT_EQ(objects[0].name, QStringLiteral("q"));
    EXPECT_FALSE(objects[0].isParameter());

    EXPECT_EQ(objects[1].name, QStringLiteral("WIDTH"));
    ASSERT_TRUE(objects[1].isParameter());
    EXPECT_FALSE(objects[1].isLocalParameter);
    EXPECT_TRUE(objects[1].constantValue);
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_EQ(objects[1].constantValue->ref().toUInt64(), 4U);
    // NOLINTEND(bugprone-unchecked-optional-access)
    EXPECT_EQ(objects[1].path, QStringLiteral("/tb/u_dut/WIDTH"));

    EXPECT_EQ(objects[2].name, QStringLiteral("DEPTH"));
    ASSERT_TRUE(objects[2].isParameter());
    EXPECT_TRUE(objects[2].isLocalParameter) << "a localparam is told apart from a parameter";
    EXPECT_TRUE(objects[2].constantValue);
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_EQ(objects[2].constantValue->ref().toUInt64(), 16U);
    // NOLINTEND(bugprone-unchecked-optional-access)

    // A parameter belongs to the scope that declared it, and the top level declared none.
    for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
        EXPECT_FALSE(info.isParameter()) << info.name.toStdString();
    }
}

TEST_F(SessionFixture, ANullArrayIsShownAsOneWithNoValue) {
    // A VHDL signal of a null range is listed with no bits, which `logic [%1:0]` would have
    // written as the width less one, wrapped round.
    ObjectsModel model(*session);
    SignalInfo nothing;
    nothing.name = QStringLiteral("nothing");
    nothing.path = QStringLiteral("/tb/nothing");
    nothing.width = 0;
    nothing.recording = enzan::ipc::Recording::Recorded;
    model.setObjects(QStringLiteral("/tb"), {nothing});

    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.data(model.index(0, ObjectsModel::TypeColumn), Qt::DisplayRole).toString(),
              QStringLiteral("null array"));
    EXPECT_TRUE(model.data(model.index(0, ObjectsModel::ValueColumn), Qt::DisplayRole)
                    .toString()
                    .isEmpty());
}

TEST_F(SessionFixture, AnArrayIsTypedByItsDimensions) {
    ObjectsModel model(*session);
    const auto shaped = [](const char* name, std::uint32_t width,
                           std::vector<enzan::eir::ArrayDimension> dimensions) {
        SignalInfo info;
        info.name = QString::fromUtf8(name);
        info.path = QStringLiteral("/tb/") + info.name;
        info.width = width;
        info.dimensions = std::move(dimensions);
        return info;
    };
    model.setObjects(QStringLiteral("/tb"),
                     {shaped("lanes", 32, {{.left = 3, .right = 0}, {.left = 7, .right = 0}}),
                      shaped("words", 128, {{.left = 0, .right = 3}}),
                      shaped("up", 8, {{.left = 0, .right = 7}}), shaped("bit", 1, {}),
                      shaped("plain", 8, {})});

    const auto type = [&model](int row) {
        return model.data(model.index(row, ObjectsModel::TypeColumn), Qt::DisplayRole).toString();
    };
    EXPECT_EQ(type(0), QStringLiteral("logic [3:0][7:0]"));
    EXPECT_EQ(type(1), QStringLiteral("logic [0:3][31:0]")) << "an int is an element of 32 bits";
    EXPECT_EQ(type(2), QStringLiteral("logic [0:7]"));
    EXPECT_EQ(type(3), QStringLiteral("logic"));
    EXPECT_EQ(type(4), QStringLiteral("logic [7:0]"));
}

TEST_F(SessionFixture, ASignalTooWideToLogSaysSo) {
    // Not the "not logged" of a signal outside the scope: widening the scope would not help.
    ObjectsModel model(*session);
    SignalInfo memory;
    memory.signalIndex = 99;
    memory.name = QStringLiteral("memory");
    memory.path = QStringLiteral("/tb/memory");
    memory.width = 1U << 20U;
    memory.recording = enzan::ipc::Recording::TooWide;
    SignalInfo outside = memory;
    outside.signalIndex = 98;
    outside.recording = enzan::ipc::Recording::OutsideScope;
    model.setObjects(QStringLiteral("/tb"), {memory, outside});

    EXPECT_EQ(model.data(model.index(0, ObjectsModel::ValueColumn), Qt::DisplayRole).toString(),
              QStringLiteral("too wide to log"));
    EXPECT_TRUE(model.data(model.index(0, ObjectsModel::NameColumn), Qt::ToolTipRole)
                    .toString()
                    .contains(QStringLiteral("--wave-max-bits")));
    EXPECT_EQ(model.data(model.index(1, ObjectsModel::ValueColumn), Qt::DisplayRole).toString(),
              QStringLiteral("not logged"));
}

/// The session over arrayTestDesign's arrays.
class ArraySessionTest : public SessionFixture {
protected:
    bool loadDesign(enzan::session::Session& into) override {
        into.setDesign(std::make_unique<enzan::eir::Design>(enzan::test::arrayTestDesign()));
        return true;
    }
};

TEST_F(ArraySessionTest, AnArrayReadsAsItsElementsInTheObjectsWindowToo) {
    const QVector<SignalInfo> objects = objectsOf(QStringLiteral("/tb"));
    ObjectsModel model(*session);
    model.setObjects(QStringLiteral("/tb"), objects);

    QVector<std::uint32_t> indices;
    for (const SignalInfo& info : objects) indices.push_back(info.signalIndex);
    session->subscribe(indices);
    ASSERT_TRUE(pumpUntil([this, &indices] {
        return std::ranges::all_of(indices, [this](std::uint32_t index) {
            return session->waveforms().recordIndexOf(index)
                   != enzan::wavedb::WaveformDatabase::NotRecorded;
        });
    }));

    const auto valueOf = [&model, &objects](const QString& name) {
        for (int row = 0; row < objects.size(); ++row) {
            if (objects[row].name == name)
                return model.data(model.index(row, ObjectsModel::ValueColumn), Qt::DisplayRole)
                    .toString();
        }
        return QString();
    };
    EXPECT_EQ(valueOf(QStringLiteral("lanes")), QStringLiteral("{5}{a}"));
    EXPECT_EQ(valueOf(QStringLiteral("memory")), QStringLiteral("{c}{3}"));
    EXPECT_EQ(valueOf(QStringLiteral("up")), QStringLiteral("8")) << "a vector is one number";
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
