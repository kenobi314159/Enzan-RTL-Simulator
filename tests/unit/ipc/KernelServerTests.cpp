// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the kernel end of the kernel/GUI split.
//
// A real client over a real socket pair, driving a real simulation. The behaviour worth pinning
// is the behaviour section 2.2 asks for and that nothing else can check: a run that can be
// stopped while it is in progress, a client that can attach part-way through and still see what
// it missed, a client that can leave without ending the simulation, and a restart that puts the
// design back at time zero without losing what the user was watching.

#include "ipc/KernelServer.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "elab/Elaborator.h"
#include "ipc/Protocol.h"
#include "ipc/Transport.h"
#include "runtime/Image.h"
#include "runtime/Time.h"

#include "support/Literals.h"
#include "support/ProcessBuilder.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>

namespace {

using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::codegen::CompiledDesign;
using enzan::codegen::Compiler;
using enzan::eir::Design;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::elab::ElaboratedDesign;
using enzan::elab::Elaborator;
using enzan::ipc::ByeMessage;
using enzan::ipc::Connection;
using enzan::ipc::Descriptor;
using enzan::ipc::DesignMessage;
using enzan::ipc::FailureMessage;
using enzan::ipc::Frame;
using enzan::ipc::HelloMessage;
using enzan::ipc::HistoryMessage;
using enzan::ipc::KernelServer;
using enzan::ipc::MessageType;
using enzan::ipc::ObjectsMessage;
using enzan::ipc::ProtocolVersion;
using enzan::ipc::QueryHistoryMessage;
using enzan::ipc::ReceiveStatus;
using enzan::ipc::RunMessage;
using enzan::ipc::RunStateMessage;
using enzan::ipc::ServerOptions;
using enzan::ipc::SimulationState;
using enzan::ipc::SubscribeMessage;
using enzan::ipc::TranscriptMessage;
using enzan::ipc::ValueChangesMessage;
using enzan::ipc::WelcomeMessage;
using enzan::runtime::Image;
using enzan::runtime::TimeScale;
using enzan::test::ProcessBuilder;

class DiscardingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const enzan::Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
    }

    std::vector<std::string> messages;
};

/// A design whose clock toggles every five time units for ever, inside one child instance.
///
/// Two scopes and a signal that never stops changing, which is what the scope queries and the
/// live streaming both need in order to say anything.
Design tickingDesign() {
    ProcessBuilder driver(ProcessKind::Initial, "clock");
    const std::uint32_t delay = driver.reg(Type::bits(32, false, false));
    const std::uint32_t value = driver.reg(Type::logic());
    const std::uint32_t inverted = driver.reg(Type::logic());
    const std::uint32_t five = driver.constant("32'd5");

    driver.block("wait");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(five)));
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    driver.block("toggle");
    driver.readObject(value, 0);
    driver.emit(Instruction::make(Opcode::Not, Operand::reg(inverted), Operand::reg(value)));
    driver.store(0, inverted);
    driver.emit(Instruction::make(Opcode::Jump, {}, Operand::block(0)));

    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "q", .type = enzan::eir::Type::bits(4, false, true)});

    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(Object{.name = "clk",
                         .type = Type::logic(),
                         .initialValue = enzan::eir::BitVector::parse("1'b0")});
    top.processes.push_back(driver.finish());

    enzan::eir::Instance instance;
    instance.name = "u_dut";
    instance.definitionName = "child";
    top.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(top));
    return design;
}

/// A design whose only process waits twenty time units and then stops for good.
Design settlingDesign() {
    ProcessBuilder driver(ProcessKind::Initial, "once");
    const std::uint32_t delay = driver.reg(Type::bits(32, false, false));
    const std::uint32_t value = driver.reg(Type::logic());
    const std::uint32_t twenty = driver.constant("32'd20");
    const std::uint32_t one = driver.constant("1'b1");

    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(twenty)));
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    driver.block("after");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(one)));
    driver.store(0, value);
    driver.emit(Instruction::make(Opcode::Return));

    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(Object{.name = "clk",
                         .type = Type::logic(),
                         .initialValue = enzan::eir::BitVector::parse("1'b0")});
    top.processes.push_back(driver.finish());

    Design design;
    design.addModule(std::move(top));
    return design;
}

/// A server with a client attached over a socket pair, and the helpers to talk to it.
class ServerFixture : public ::testing::Test {
protected:
    ServerFixture() : engine(sources) { engine.addConsumer(consumer); }

    void SetUp() override { rebuild(tickingDesign()); }

    /// Elaborates, compiles and serves `design`. Called again by a test that needs a design
    /// the fixture's own does not provide.
    void rebuild(Design design) {
        analysed = std::move(design);

        Elaborator elaborator(analysed, engine);
        elaborated = elaborator.elaborate("tb");
        ASSERT_NE(elaborated, nullptr) << consumer.messages.front();

        Compiler compiler(engine);
        compiled = compiler.compile(analysed);
        ASSERT_NE(compiled, nullptr);

        image = std::make_unique<Image>(analysed, *elaborated, *compiled);

        ServerOptions options;
        options.kernelVersion = "enzan test";
        options.timeScale = TimeScale(-9);
        options.localTranscript = &localTranscript;
        server = std::make_unique<KernelServer>(*image, engine, options);
        server->start();
    }

    void TearDown() override {
        client.close();
        server.reset();
    }

    /// Connects a client and completes the handshake.
    WelcomeMessage connectClient() {
        std::array<int, 2> descriptors{-1, -1};
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()), 0);

        client = Connection::adopt(Descriptor(descriptors[0]));
        server->attach(Connection::adopt(Descriptor(descriptors[1])));

        HelloMessage hello;
        hello.protocolVersion = ProtocolVersion;
        hello.clientName = "the test";
        request(MessageType::Hello, hello);

        return expect<WelcomeMessage>(MessageType::Welcome);
    }

    /// Sends a request and lets the server answer it.
    template<typename Message>
    void request(MessageType type, Message& message) {
        client.send(type, message);
        EXPECT_TRUE(client.flushBlocking()) << client.error();
        pumpServer();
    }

    /// Runs the server until it has nothing left to do, then collects what it sent.
    void pumpServer() {
        // A short timeout rather than none, because a step that has already been woken by the
        // previous round must not wait for a new reason to run.
        for (int round = 0; round < 8; ++round) static_cast<void>(server->step(5));
        collect();
    }

    /// Reads everything waiting on the client socket into `received`.
    void collect() {
        if (!client.isOpen()) return;
        if (!client.receive()) return;

        while (true) {
            Frame frame;
            const ReceiveStatus status = client.next(frame);
            if (status != ReceiveStatus::Message) return;
            received.push_back(Captured{
                frame.type, std::vector<std::byte>(frame.payload.begin(), frame.payload.end())});
        }
    }

    /// Removes and decodes the first message of `type`, failing when there is none.
    template<typename Message>
    Message expect(MessageType type) {
        Message message;
        for (std::size_t index = 0; index < received.size(); ++index) {
            if (received[index].type != static_cast<std::uint16_t>(type)) continue;
            EXPECT_TRUE(enzan::ipc::decodePayload(received[index].payload, message));
            received.erase(received.begin() + static_cast<std::ptrdiff_t>(index));
            return message;
        }
        ADD_FAILURE() << "no " << enzan::ipc::toString(type) << " message arrived";
        return message;
    }

    /// Removes and decodes every message of `type`, oldest first.
    template<typename Message>
    std::vector<Message> expectAll(MessageType type) {
        std::vector<Message> messages;
        std::vector<Captured> kept;
        for (Captured& captured : received) {
            if (captured.type != static_cast<std::uint16_t>(type)) {
                kept.push_back(std::move(captured));
                continue;
            }
            Message message;
            EXPECT_TRUE(enzan::ipc::decodePayload(captured.payload, message));
            messages.push_back(std::move(message));
        }
        received = std::move(kept);
        return messages;
    }

    [[nodiscard]] bool sawAny(MessageType type) const {
        for (const Captured& captured : received) {
            if (captured.type == static_cast<std::uint16_t>(type)) return true;
        }
        return false;
    }

    /// The kernel signal index for a hierarchical path.
    [[nodiscard]] std::uint32_t signalIndex(std::string_view path) const {
        return image->findSignal(path);
    }

    struct Captured {
        std::uint16_t type;
        std::vector<std::byte> payload;
    };

    SourceManager sources;
    DiagnosticEngine engine;
    DiscardingConsumer consumer;
    std::ostringstream localTranscript;

    Design analysed;
    std::unique_ptr<ElaboratedDesign> elaborated;
    std::unique_ptr<CompiledDesign> compiled;
    std::unique_ptr<Image> image;
    std::unique_ptr<KernelServer> server;

    Connection client;
    std::vector<Captured> received;
};

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, TheHandshakeDescribesTheDesign) {
    const WelcomeMessage welcome = connectClient();

    EXPECT_EQ(welcome.protocolVersion, ProtocolVersion);
    EXPECT_EQ(welcome.kernelVersion, "enzan test");
    EXPECT_EQ(welcome.topModule, "tb");
    EXPECT_EQ(welcome.timeExponent, -9);
    EXPECT_EQ(welcome.currentTime, 0U);
    EXPECT_EQ(welcome.state, SimulationState::Idle);
}

TEST_F(ServerFixture, AVersionMismatchIsRefusedRatherThanNegotiated) {
    // Two builds that half-understand each other would show the user wrong values, and
    // section 1.2 puts correctness above the convenience of carrying on.
    std::array<int, 2> descriptors{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()), 0);
    client = Connection::adopt(Descriptor(descriptors[0]));
    server->attach(Connection::adopt(Descriptor(descriptors[1])));

    HelloMessage hello;
    hello.protocolVersion = ProtocolVersion + 1;
    request(MessageType::Hello, hello);

    const FailureMessage failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("protocol version"), std::string::npos) << failure.reason;

    const ByeMessage bye = expect<ByeMessage>(MessageType::Bye);
    EXPECT_NE(bye.reason.find("version mismatch"), std::string::npos) << bye.reason;
    EXPECT_FALSE(server->hasClient());
}

TEST_F(ServerFixture, NothingIsAnsweredBeforeTheHandshake) {
    std::array<int, 2> descriptors{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()), 0);
    client = Connection::adopt(Descriptor(descriptors[0]));
    server->attach(Connection::adopt(Descriptor(descriptors[1])));

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);

    EXPECT_FALSE(sawAny(MessageType::RunState));
    EXPECT_FALSE(server->hasClient());
    EXPECT_EQ(server->kernel().time(), 0U) << "a request before the handshake must not run";
}

// ---------------------------------------------------------------------------
// Describing the design
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, TheHierarchyIsSentWholeAndKeepsItsShape) {
    connectClient();

    enzan::ipc::QueryDesignMessage query;
    request(MessageType::QueryDesign, query);

    const DesignMessage design = expect<DesignMessage>(MessageType::Design);
    ASSERT_EQ(design.instances.size(), 2U);
    EXPECT_EQ(design.instances[0].path, "/tb");
    EXPECT_EQ(design.instances[0].parent, enzan::ipc::InstanceEntry::NoParent);
    EXPECT_EQ(design.instances[1].path, "/tb/u_dut");
    EXPECT_EQ(design.instances[1].parent, 0U);
}

TEST_F(ServerFixture, AScopeListsItsOwnObjects) {
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);

    const ObjectsMessage objects = expect<ObjectsMessage>(MessageType::Objects);
    EXPECT_EQ(objects.instancePath, "/tb");
    ASSERT_EQ(objects.objects.size(), 1U);
    EXPECT_EQ(objects.objects[0].name, "clk");
    EXPECT_EQ(objects.objects[0].path, "/tb/clk");
    EXPECT_EQ(objects.objects[0].width, 1U);
    EXPECT_TRUE(objects.objects[0].recorded) << "the whole design is in the logging scope";
}

TEST_F(ServerFixture, AnUnknownScopeIsAFailureRatherThanAnEmptyAnswer) {
    // An empty list and a mistyped path must not look the same: the user would conclude the
    // scope had no signals rather than that it does not exist.
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb/nowhere";
    request(MessageType::QueryObjects, query);

    const FailureMessage failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("not a scope"), std::string::npos) << failure.reason;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, ARunAdvancesTimeAndIsAnnouncedAtBothEnds) {
    connectClient();

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);

    const std::vector<RunStateMessage> states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_GE(states.size(), 2U)
        << "the client must learn when a run starts, not only when it ends";
    EXPECT_EQ(states.front().state, SimulationState::Running);
    EXPECT_EQ(states.back().state, SimulationState::Idle);
    EXPECT_EQ(states.back().time, 100U);
    EXPECT_EQ(server->kernel().time(), 100U);
}

TEST_F(ServerFixture, SubscribedSignalsStreamAsTheRunProgresses) {
    connectClient();

    SubscribeMessage subscribe;
    subscribe.signals = {signalIndex("/tb/clk")};
    request(MessageType::Subscribe, subscribe);

    // Subscribing catches the client up, so the history arrives before anything is run.
    const HistoryMessage caughtUp = expect<HistoryMessage>(MessageType::History);
    EXPECT_EQ(caughtUp.signalIndex, signalIndex("/tb/clk"));
    ASSERT_FALSE(caughtUp.entries.empty());
    EXPECT_EQ(caughtUp.entries.front().time, 0U);

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);

    const std::vector<ValueChangesMessage> batches =
        expectAll<ValueChangesMessage>(MessageType::ValueChanges);
    ASSERT_FALSE(batches.empty()) << "a subscribed signal that changed twenty times sent nothing";

    // Every batch is one settled time slot, in increasing time, and carries only what was
    // subscribed. Delta-level intermediate values must never reach the view (section 14.3).
    std::uint64_t previous = 0;
    for (const ValueChangesMessage& batch : batches) {
        EXPECT_GE(batch.time, previous);
        previous = batch.time;
        ASSERT_EQ(batch.updates.size(), 1U);
        EXPECT_EQ(batch.updates[0].signalIndex, signalIndex("/tb/clk"));
    }
    EXPECT_EQ(previous, 100U);
}

TEST_F(ServerFixture, AnUnsubscribedSignalDoesNotStream) {
    connectClient();

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);

    EXPECT_FALSE(sawAny(MessageType::ValueChanges))
        << "streaming a signal nobody asked for spends the socket on data the client discards";
}

TEST_F(ServerFixture, StopEndsARunThatIsAlreadyInProgress) {
    // Section 2.2's requirement in its most visible form. The design asked for runs for ever,
    // so this test only ends because the Stop reaches the kernel's interrupt while it is
    // simulating. The stop is sent from another thread for the same reason: the server is
    // inside `run` and will not return until it arrives.
    connectClient();

    RunMessage run;
    run.untilFinished = true;
    client.send(MessageType::Run, run);
    ASSERT_TRUE(client.flushBlocking()) << client.error();

    std::thread stopper([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        enzan::ipc::StopMessage stop;
        client.send(MessageType::Stop, stop);
        static_cast<void>(client.flushBlocking());
    });

    static_cast<void>(server->step(5));
    stopper.join();
    collect();

    const std::vector<RunStateMessage> states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(states.back().state, SimulationState::Idle);
    EXPECT_EQ(states.back().reason, "interrupted");
    EXPECT_GT(server->kernel().time(), 0U) << "the run had not actually started";

    // And the kernel is left in a state another run continues from rather than restarts.
    const std::uint64_t stoppedAt = server->kernel().time();
    RunMessage more;
    more.duration = 50;
    request(MessageType::Run, more);
    EXPECT_EQ(server->kernel().time(), stoppedAt + 50);
}

TEST_F(ServerFixture, AStopAskedForBeforeARunStartsIsNotLost) {
    // The two messages arrive together, so the stop is latched before the run begins. Clearing
    // the latch when a run starts rather than when one is asked for would drop it, and the
    // kernel would run on after the user had pressed Stop.
    connectClient();

    RunMessage run;
    run.untilFinished = true;
    client.send(MessageType::Run, run);
    enzan::ipc::StopMessage stop;
    client.send(MessageType::Stop, stop);
    ASSERT_TRUE(client.flushBlocking()) << client.error();

    pumpServer();

    const std::vector<RunStateMessage> states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(states.back().state, SimulationState::Idle);
    EXPECT_EQ(server->kernel().time(), 0U);
}

TEST_F(ServerFixture, ConsecutiveRunsContinueFromWhereTheLastStopped) {
    connectClient();

    RunMessage first;
    first.duration = 50;
    request(MessageType::Run, first);
    EXPECT_EQ(server->kernel().time(), 50U);

    RunMessage second;
    second.duration = 50;
    request(MessageType::Run, second);

    EXPECT_FALSE(sawAny(MessageType::Failure));
    EXPECT_EQ(server->kernel().time(), 100U);
}

TEST_F(ServerFixture, RunningAFinishedDesignIsRefusedUntilItIsRestarted) {
    rebuild(settlingDesign());
    connectClient();

    RunMessage run;
    run.untilFinished = true;
    request(MessageType::Run, run);

    const std::vector<RunStateMessage> states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(states.back().state, SimulationState::Finished);

    RunMessage again;
    again.duration = 10;
    request(MessageType::Run, again);
    const FailureMessage failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("restart"), std::string::npos) << failure.reason;

    enzan::ipc::RestartMessage restart;
    request(MessageType::Restart, restart);
    EXPECT_EQ(server->kernel().time(), 0U);

    request(MessageType::Run, run);
    EXPECT_FALSE(sawAny(MessageType::Failure));
    EXPECT_EQ(server->kernel().time(), 20U) << "the design must run again from the beginning";
}

TEST_F(ServerFixture, ARunPastTheEndOfTimeIsRefused) {
    // Section 3.1 requires overflow to be detected and reported rather than wrapping silently.
    // The kernel must be some way into the run for the sum to overflow at all, so it is.
    connectClient();

    RunMessage first;
    first.duration = 50;
    request(MessageType::Run, first);
    ASSERT_EQ(server->kernel().time(), 50U);

    RunMessage tooFar;
    tooFar.duration = enzan::runtime::NoTime;
    request(MessageType::Run, tooFar);

    const FailureMessage failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("64-bit range"), std::string::npos) << failure.reason;
    EXPECT_EQ(server->kernel().time(), 50U);
}

// ---------------------------------------------------------------------------
// Attaching part-way through, and leaving
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, AClientAttachingMidRunSeesWhatItMissed) {
    // The reason the kernel keeps a waveform database rather than only streaming: a GUI that
    // attaches to a simulation already in progress must show its past, not a blank window.
    connectClient();

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    received.clear();

    // A second client replaces the first, exactly as a reattach after a detach would.
    client.close();
    pumpServer();
    const WelcomeMessage welcome = connectClient();
    EXPECT_EQ(welcome.currentTime, 100U);
    EXPECT_GE(welcome.latestRecordedTime, 100U);

    SubscribeMessage subscribe;
    subscribe.signals = {signalIndex("/tb/clk")};
    request(MessageType::Subscribe, subscribe);

    const HistoryMessage history = expect<HistoryMessage>(MessageType::History);
    EXPECT_TRUE(history.complete);
    EXPECT_GE(history.entries.size(), 20U) << "a clock that toggled twenty times has a past";
    EXPECT_EQ(history.entries.front().time, 0U);
    EXPECT_EQ(history.latestTime, 100U);
}

TEST_F(ServerFixture, DetachingReleasesTheClientAndLeavesTheSimulationIntact) {
    // Section 2.2 requires the GUI to detach without terminating the kernel, which is how a
    // long run on a compute server is looked in on and left alone again.
    connectClient();

    RunMessage run;
    run.duration = 50;
    request(MessageType::Run, run);

    enzan::ipc::DetachMessage detach;
    request(MessageType::Detach, detach);

    EXPECT_FALSE(server->hasClient());
    EXPECT_EQ(server->kernel().time(), 50U) << "detaching must not disturb the simulation";

    // This kernel was handed its client directly, so there is nowhere for another to come
    // from and serving stops. One with a socket to listen on would carry on waiting.
    EXPECT_FALSE(server->step(0));
}

TEST_F(ServerFixture, QuitStopsTheServer) {
    connectClient();

    enzan::ipc::QuitMessage quit;
    client.send(MessageType::Quit, quit);
    ASSERT_TRUE(client.flushBlocking()) << client.error();

    EXPECT_FALSE(server->step(5));
}

// ---------------------------------------------------------------------------
// Restarting
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, RestartReturnsToTimeZeroAndKeepsTheSubscriptions) {
    connectClient();

    SubscribeMessage subscribe;
    subscribe.signals = {signalIndex("/tb/clk")};
    request(MessageType::Subscribe, subscribe);

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    ASSERT_EQ(server->kernel().time(), 100U);
    received.clear();

    enzan::ipc::RestartMessage restart;
    request(MessageType::Restart, restart);

    const RunStateMessage state = expect<RunStateMessage>(MessageType::RunState);
    EXPECT_EQ(state.time, 0U) << "time going backwards is how the client learns to discard";
    EXPECT_EQ(state.state, SimulationState::Idle);
    EXPECT_EQ(server->kernel().time(), 0U);

    // The subscription survives, and the fresh trace is sent without being asked for: losing
    // the wave window's contents would make the button useless for what it is used for.
    const HistoryMessage history = expect<HistoryMessage>(MessageType::History);
    EXPECT_EQ(history.signalIndex, signalIndex("/tb/clk"));
    ASSERT_EQ(history.entries.size(), 1U);
    EXPECT_EQ(history.entries.front().time, 0U);

    // And the design runs again from the beginning.
    RunMessage again;
    again.duration = 40;
    request(MessageType::Run, again);
    EXPECT_EQ(server->kernel().time(), 40U);
}

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, TheTranscriptGoesToTheClientAndToTheTerminal) {
    // A kernel serving a socket is still a program someone started in a terminal. Swallowing
    // its output because a GUI happened to be attached would be surprising.
    connectClient();

    const TranscriptMessage transcript = expect<TranscriptMessage>(MessageType::Transcript);
    EXPECT_NE(transcript.text.find("attached"), std::string::npos) << transcript.text;
    EXPECT_NE(localTranscript.str().find("attached"), std::string::npos) << localTranscript.str();
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, HistoryHonoursTheRequestedRange) {
    connectClient();

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    received.clear();

    QueryHistoryMessage query;
    query.signalIndex = signalIndex("/tb/clk");
    query.fromTime = 30;
    query.toTime = 60;
    request(MessageType::QueryHistory, query);

    const HistoryMessage history = expect<HistoryMessage>(MessageType::History);
    ASSERT_FALSE(history.entries.empty());
    for (const enzan::ipc::HistoryEntry& entry : history.entries) {
        EXPECT_GE(entry.time, 30U);
        EXPECT_LT(entry.time, 60U);
    }
}

TEST_F(ServerFixture, AskingAboutASignalThatDoesNotExistIsAFailure) {
    connectClient();

    QueryHistoryMessage query;
    query.signalIndex = 9999;
    request(MessageType::QueryHistory, query);

    const FailureMessage failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("no signal"), std::string::npos) << failure.reason;
}

} // namespace
