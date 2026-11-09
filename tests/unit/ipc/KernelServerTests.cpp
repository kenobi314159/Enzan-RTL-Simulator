// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the kernel end of the kernel/GUI split.
//
// A real client over a real socket pair, driving a real simulation. The behaviour worth pinning
// is the behaviour separating the kernel from the front end is for, and that nothing else can
// check: a run that can be stopped while it is in progress, a client that can attach part-way
// through and still see what it missed, a client that can leave without ending the simulation,
// and a restart that puts the design back at time zero without losing what the user was watching.

#include "ipc/KernelServer.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "ipc/Protocol.h"
#include "ipc/Transport.h"
#include "runtime/Image.h"
#include "runtime/Time.h"
#include "session/Session.h"
#include "wavedb/WaveformWriter.h"

#include "support/Literals.h"
#include "support/ProcessBuilder.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::eir::Design;
using enzan::eir::Instruction;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Opcode;
using enzan::eir::Operand;
using enzan::eir::ProcessKind;
using enzan::eir::Type;
using enzan::ipc::ByeMessage;
using enzan::ipc::ChangeCursor;
using enzan::ipc::ChangeEntry;
using enzan::ipc::ChangesMessage;
using enzan::ipc::Connection;
using enzan::ipc::DepositMessage;
using enzan::ipc::Descriptor;
using enzan::ipc::DesignMessage;
using enzan::ipc::FailureMessage;
using enzan::ipc::FetchChangesMessage;
using enzan::ipc::ForceMessage;
using enzan::ipc::Frame;
using enzan::ipc::HelloMessage;
using enzan::ipc::KernelServer;
using enzan::ipc::MessageType;
using enzan::ipc::ObjectsMessage;
using enzan::ipc::ProtocolVersion;
using enzan::ipc::ReceiveStatus;
using enzan::ipc::RunMessage;
using enzan::ipc::RunStateMessage;
using enzan::ipc::ServerOptions;
using enzan::ipc::SignalChanges;
using enzan::ipc::SimulationState;
using enzan::ipc::SubscribeMessage;
using enzan::ipc::TranscriptMessage;
using enzan::ipc::WelcomeMessage;
using enzan::runtime::TimeScale;
using enzan::session::Session;
using enzan::test::literal;
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

/// A design of one std_ulogic signal, held as its code, a byte, as the VHDL frontend holds it:
/// '1' at first and '0' from time twenty.
Design codedDesign() {
    ProcessBuilder driver(ProcessKind::Initial, "once");
    const std::uint32_t delay = driver.reg(Type::bits(32, false, false));
    const std::uint32_t value = driver.reg(Type::bits(8, false, false));
    const std::uint32_t twenty = driver.constant("32'd20");
    const std::uint32_t zero = driver.constant("8'd2");

    driver.block("entry");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(delay), Operand::constant(twenty)));
    driver.emit(Instruction::make(Opcode::WaitTime, {}, Operand::reg(delay), Operand::block(1)));
    driver.block("after");
    driver.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(zero)));
    driver.store(0, value);
    driver.emit(Instruction::make(Opcode::Return));

    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(Object{.name = "ready",
                         .type = Type::bits(8, false, false),
                         .initialValue = enzan::eir::BitVector::parse("8'd3"),
                         .encoding = enzan::eir::Encoding::VhdlLogic});
    top.processes.push_back(driver.finish());

    Design design;
    design.addModule(std::move(top));
    return design;
}

/// codedDesign's, with a std_logic_vector of a null range beside its signal: a placeholder bit
/// that holds nothing, as the VHDL frontend declares one.
Design nullDesign() {
    const Design design = codedDesign();
    ModuleDefinition top = design.modules().front();
    top.addObject(Object{.name = "nothing",
                         .type = Type::bits(1, false, false),
                         .encoding = enzan::eir::Encoding::VhdlLogic,
                         .holdsNothing = true});
    Design withNull;
    withNull.addModule(std::move(top));
    return withNull;
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

/// Stand-in source text for the debugging tests, one line per line number.
constexpr std::string_view DebugSource = "one\ntwo\nthree\nfour\nfive\n";

/// The byte offset of 1-based `line` in DebugSource.
constexpr std::uint32_t debugLineOffset(std::uint32_t line) {
    constexpr std::uint32_t Offsets[] = {0, 0, 4, 8, 14, 19};
    return Offsets[line];
}

/// A design whose two statements sit on lines 2 and 3 of `buffer`.
///
/// Instructions carry their source location the way the frontend stamps them, which is what a
/// breakpoint on a line resolves against. Built by hand rather than parsed, so that a failure
/// here is the server's and not the frontend's.
Design debuggableDesign(enzan::BufferId buffer) {
    const auto at = [buffer](std::uint32_t line) {
        return enzan::SourceLocation(buffer, debugLineOffset(line));
    };

    ProcessBuilder builder(ProcessKind::Initial, "stepper");
    const std::uint32_t value = builder.reg(Type::bits(4, false, true));
    const std::uint32_t first = builder.constant("4'd3");
    const std::uint32_t second = builder.constant("4'd5");

    builder.block("entry");
    builder.at(at(2));
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(first)));
    builder.store(0, value);

    builder.at(at(3));
    builder.emit(
        Instruction::make(Opcode::LoadConstant, Operand::reg(value), Operand::constant(second)));
    builder.store(0, value);

    builder.at(at(4));
    builder.emit(Instruction::make(Opcode::Return));

    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.location = at(1);
    top.addObject(Object{.name = "q",
                         .type = Type::bits(4, false, true),
                         .initialValue = enzan::eir::BitVector::parse("4'd0")});
    top.processes.push_back(builder.finish());

    Design design;
    design.addModule(std::move(top));
    return design;
}

/// Objects of several shapes, as the frontends describe them: SystemVerilog's packed and unpacked
/// arrays, a VHDL array of vectors and a vector of 200 bits, with a child whose port, declared
/// flat, is collapsed onto the packed array.
Design shapedDesign() {
    using enzan::eir::ArrayDimension;
    using enzan::eir::VhdlType;

    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "flat",
                           .type = Type::bits(32, false, true),
                           .packedDimensions = {ArrayDimension{.left = 31, .right = 0}}});
    child.ports.push_back(enzan::eir::Port{
        .name = "flat", .direction = enzan::eir::PortDirection::Input, .objectIndex = 0});

    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    // logic [3:0][7:0] lanes
    top.addObject(Object{.name = "lanes",
                         .type = Type::bits(32, false, true),
                         .packedDimensions = {ArrayDimension{.left = 3, .right = 0},
                                              ArrayDimension{.left = 7, .right = 0}}});
    // logic [7:0] memory [0:3]
    top.addObject(Object{.name = "memory",
                         .type = Type::bits(32, false, true),
                         .unpackedDimensions = {ArrayDimension{.left = 0, .right = 3}},
                         .packedDimensions = {ArrayDimension{.left = 7, .right = 0}},
                         .elementWidth = 8});
    // array (0 to 1) of std_logic_vector(3 downto 0), eight codes of a byte each
    top.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Enumeration,
                                     .name = "STD_ULOGIC",
                                     .width = 8,
                                     .encoding = enzan::eir::Encoding::VhdlLogic});
    top.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                     .name = "STD_LOGIC_VECTOR",
                                     .width = 32,
                                     .ranges = {{.left = 3, .right = 0, .ascending = false}},
                                     .element = 0});
    top.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Array,
                                     .name = "NIBBLES",
                                     .width = 64,
                                     .ranges = {{.left = 0, .right = 1, .ascending = true}},
                                     .element = 1});
    top.addObject(Object{.name = "nibbles",
                         .type = Type::bits(64, false, false),
                         .initialValue = enzan::eir::BitVector::parse("64'h0203020302030203"),
                         .encoding = enzan::eir::Encoding::VhdlLogic,
                         .vhdlType = 2});
    // logic [199:0] wide
    top.addObject(Object{.name = "wide",
                         .type = Type::bits(200, false, true),
                         .packedDimensions = {ArrayDimension{.left = 199, .right = 0}}});

    enzan::eir::Instance instance;
    instance.name = "u_dut";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 0});
    top.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(top));
    return design;
}

/// A child whose port is connected to an expression, with the storage a frontend makes for that
/// in the parent, named with a `$`, and a generate loop's genvar as one iteration sees it.
Design introducedDesign() {
    ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(Object{.name = "count", .type = Type::bits(4, false, true)});
    child.ports.push_back(enzan::eir::Port{
        .name = "count", .direction = enzan::eir::PortDirection::Output, .objectIndex = 0});

    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(Object{.name = "count_lin", .type = Type::bits(8, false, true)});
    top.addObject(Object{.name = "gen[0].u$count", .type = Type::bits(4, false, true)});
    top.parameters.push_back(enzan::eir::Parameter{.name = "gen[0].I",
                                                   .value = enzan::test::literal("32'd0"),
                                                   .isLocal = true,
                                                   .fromGenvar = true});
    top.parameters.push_back(enzan::eir::Parameter{
        .name = "gen[0].TWICE", .value = enzan::test::literal("32'd0"), .isLocal = true});

    enzan::eir::Instance instance;
    instance.name = "gen[0].u";
    instance.definitionName = "child";
    instance.connections.push_back(enzan::eir::PortConnection{.portIndex = 0, .objectIndex = 1});
    top.instances.push_back(std::move(instance));

    Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(top));
    return design;
}

/// A server with a client attached over a socket pair, and the helpers to talk to it.
class ServerFixture : public ::testing::Test {
protected:
    ServerFixture() : engine(sources) { engine.addConsumer(consumer); }

    void SetUp() override { rebuild(tickingDesign()); }

    /// Elaborates and serves `design`. Called again by a test that needs a design the
    /// fixture's own does not provide.
    ///
    /// The design goes through a `session::Session`, which is what the served kernel owns: a
    /// command typed at a front end and a request from one act on the same simulation, so a
    /// test that served an image of its own would be testing an arrangement that does not
    /// exist.
    ///
    /// `withVpiApplication` loads the test plugin first, as `--vpi` would.
    void rebuild(Design design, bool withVpiApplication = false) {
        rebuild(
            [&design](Session& fresh) {
                fresh.setDesign(std::make_unique<Design>(std::move(design)));
                return true;
            },
            withVpiApplication);
    }

    /// Analyses `files`, written into the workspace first, and serves what they elaborate to.
    void rebuildFrom(const std::vector<std::pair<std::string, std::string>>& files) {
        rebuild([this, &files](Session& fresh) {
#ifdef ENZAN_WITH_VHDL
            fresh.setVhdlLibraryDirectory(ENZAN_VHDL_LIBRARY_ROOT);
#endif
            Session::AnalysisRequest request;
            for (const auto& [name, text] : files) {
                workspace.write(name, text);
                request.files.push_back(workspace / name);
            }
            request.top = "tb";
            return fresh.analyse(request);
        });
    }

    /// Builds a session, has `load` give it a design, and serves what it elaborates to.
    template<typename Load>
    void rebuild(Load load, bool withVpiApplication = false) {
        // The old server before the old session: the server borrows the session's kernel, and
        // replacing the session first would leave it pointing at one that has gone.
        server.reset();
        session.reset();
        session = std::make_unique<Session>(sources, engine);
        session->setTimeScale(TimeScale(-9));
        if (withVpiApplication) {
            const std::vector<std::string> commandLine = {"enzan"};
            ASSERT_TRUE(session->loadVpiApplication(ENZAN_TEST_STARTUP_PLUGIN, commandLine));
        }
        ASSERT_TRUE(load(*session))
            << (consumer.messages.empty() ? "analysis failed" : consumer.messages.front());
        ASSERT_TRUE(session->elaborate("tb"))
            << (consumer.messages.empty() ? "elaboration failed" : consumer.messages.front());

        ServerOptions options;
        options.kernelVersion = "enzan test";
        options.timeScale = TimeScale(-9);
        options.localTranscript = &localTranscript;
        options.waveformScope = waveformScope;
        options.waveformWidthLimit = waveformWidthLimit;
        options.changesBytes = changesBytes;
        server = std::make_unique<KernelServer>(*session, engine, options);
        server->start();
    }

    void TearDown() override {
        client.close();

        // The server borrows the session's kernel, so it goes first.
        server.reset();
        session.reset();
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
                .type = frame.type,
                .payload = std::vector<std::byte>(frame.payload.begin(), frame.payload.end())});
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
        return std::ranges::any_of(received, [type](const Captured& captured) {
            return captured.type == static_cast<std::uint16_t>(type);
        });
    }

    /// Asks for the changes of `signals` beyond what the client holds of each, as a client fetches
    /// them, keeps what comes in `held`, and returns the answer.
    ChangesMessage fetch(const std::vector<std::uint32_t>& signals) {
        FetchChangesMessage message;
        for (const std::uint32_t signal : signals)
            message.signals.push_back(
                ChangeCursor{.signalIndex = signal, .held = held[signal].size()});
        request(MessageType::FetchChanges, message);
        auto answer = expect<ChangesMessage>(MessageType::Changes);
        for (const SignalChanges& changes : answer.signals) {
            std::vector<ChangeEntry>& copy = held[changes.signalIndex];
            if (changes.first == copy.size())
                copy.insert(copy.end(), changes.entries.begin(), changes.entries.end());
        }
        return answer;
    }

    /// Everything the kernel holds of `signal`, fetched until the client has caught up.
    const std::vector<ChangeEntry>& fetchAll(std::uint32_t signal) {
        for (int round = 0; round < 1000; ++round) {
            const ChangesMessage answer = fetch({signal});
            if (answer.signals.size() != 1 || held[signal].size() >= answer.signals[0].recorded)
                break;
        }
        return held[signal];
    }

    /// The kernel signal index for a hierarchical path.
    [[nodiscard]] std::uint32_t signalIndex(std::string_view path) const {
        return session->designImage()->findSignal(path);
    }

    struct Captured {
        std::uint16_t type;
        std::vector<std::byte> payload;
    };

    SourceManager sources;
    DiagnosticEngine engine;
    DiscardingConsumer consumer;
    std::ostringstream localTranscript;

    std::unique_ptr<Session> session;
    std::unique_ptr<KernelServer> server;
    enzan::test::Workspace workspace;

    /// The scope the served database records, read by `rebuild`.
    std::string waveformScope;

    /// The widest signal the served database records, read by `rebuild`.
    std::uint32_t waveformWidthLimit = enzan::wavedb::WaveformWriter::DefaultWidthLimit;

    /// The bytes of one answer of changes, read by `rebuild`.
    std::size_t changesBytes = ServerOptions{}.changesBytes;

    /// The client's copy of each signal's recorded changes, as `fetch` builds it.
    std::map<std::uint32_t, std::vector<ChangeEntry>> held;

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
    // correctness comes before the convenience of carrying on.
    std::array<int, 2> descriptors{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()), 0);
    client = Connection::adopt(Descriptor(descriptors[0]));
    server->attach(Connection::adopt(Descriptor(descriptors[1])));

    HelloMessage hello;
    hello.protocolVersion = ProtocolVersion + 1;
    request(MessageType::Hello, hello);

    const auto failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("protocol version"), std::string::npos) << failure.reason;

    const auto bye = expect<ByeMessage>(MessageType::Bye);
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

    const auto design = expect<DesignMessage>(MessageType::Design);
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

    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    EXPECT_EQ(objects.instancePath, "/tb");
    ASSERT_EQ(objects.objects.size(), 1U);
    EXPECT_EQ(objects.objects[0].name, "clk");
    EXPECT_EQ(objects.objects[0].path, "/tb/clk");
    EXPECT_EQ(objects.objects[0].width, 1U);
    EXPECT_EQ(objects.objects[0].recording, enzan::ipc::Recording::Recorded)
        << "the whole design is in the logging scope";
}

TEST_F(ServerFixture, AnObjectIsListedWithTheDimensionsItsNameIsDeclaredWith) {
    using enzan::eir::ArrayDimension;
    using enzan::ipc::Recording;
    using Dimensions = std::vector<ArrayDimension>;

    waveformWidthLimit = 100;
    rebuild(shapedDesign());
    EXPECT_EQ(connectClient().widthLimit, 100U);

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);
    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    const auto named = [&objects](std::string_view name) -> const enzan::ipc::ObjectEntry& {
        const auto found = std::ranges::find(objects.objects, name, &enzan::ipc::ObjectEntry::name);
        EXPECT_NE(found, objects.objects.end()) << name;
        return *found;
    };

    EXPECT_EQ(named("lanes").dimensions, (Dimensions{ArrayDimension{.left = 3, .right = 0},
                                                     ArrayDimension{.left = 7, .right = 0}}));
    EXPECT_EQ(named("lanes").recording, Recording::Recorded);

    // An unpacked array is recorded for the window, though a file leaves it out.
    EXPECT_EQ(named("memory").dimensions, (Dimensions{ArrayDimension{.left = 0, .right = 3},
                                                      ArrayDimension{.left = 7, .right = 0}}));
    EXPECT_EQ(named("memory").recording, Recording::Recorded);

    // A VHDL array divides the bits it shows as, one for each std_logic, not their codes.
    EXPECT_EQ(named("nibbles").width, 8U);
    EXPECT_EQ(named("nibbles").dimensions, (Dimensions{ArrayDimension{.left = 0, .right = 1},
                                                       ArrayDimension{.left = 3, .right = 0}}));

    EXPECT_EQ(named("wide").recording, Recording::TooWide);
    EXPECT_EQ(named("wide").dimensions, (Dimensions{ArrayDimension{.left = 199, .right = 0}}));

    // The child's port is the same signal, declared flat there, and listed as it is declared.
    query.instancePath = "/tb/u_dut";
    request(MessageType::QueryObjects, query);
    const auto inside = expect<ObjectsMessage>(MessageType::Objects);
    ASSERT_EQ(inside.objects.size(), 1U);
    EXPECT_EQ(inside.objects[0].signalIndex, named("lanes").signalIndex);
    EXPECT_EQ(inside.objects[0].dimensions, (Dimensions{ArrayDimension{.left = 31, .right = 0}}));
}

#if defined(ENZAN_WITH_SYSTEMVERILOG) || defined(ENZAN_WITH_VHDL)
/// The dimensions each object of `/tb` is listed with, by name.
std::map<std::string, std::vector<enzan::eir::ArrayDimension>, std::less<>>
dimensionsByName(const ObjectsMessage& objects) {
    std::map<std::string, std::vector<enzan::eir::ArrayDimension>, std::less<>> found;
    for (const enzan::ipc::ObjectEntry& entry : objects.objects)
        found.emplace(entry.name, entry.dimensions);
    return found;
}
#endif

#ifdef ENZAN_WITH_SYSTEMVERILOG
TEST_F(ServerFixture, ASystemVerilogArrayIsListedWithTheDimensionsItsSourceDeclares) {
    // The shapes of a testbench's counters, packed and unpacked, as large_design_00 declares
    // them, and the others an array can take.
    rebuildFrom({{"tb.sv", R"(
        module tb;
            logic [3:0][17:0] count_rec;
            logic [17:0] count_rec0 [3:0];
            logic count_rec1 [3:0][17:0];
            int words [0:1];
            logic [0:3] up;
            logic flag;
            typedef struct packed { logic [3:0] a; logic [3:0] b; } pair_t;
            pair_t [1:0] pairs;
        endmodule
    )"}});
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);
    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    const auto shapes = dimensionsByName(objects);

    using enzan::eir::ArrayDimension;
    using Dimensions = std::vector<ArrayDimension>;
    const Dimensions counters{ArrayDimension{.left = 3, .right = 0},
                              ArrayDimension{.left = 17, .right = 0}};
    EXPECT_EQ(shapes.at("count_rec"), counters);
    EXPECT_EQ(shapes.at("count_rec0"), counters) << "unpacked then packed, as declared";
    EXPECT_EQ(shapes.at("count_rec1"), counters) << "two unpacked dimensions of bits";
    EXPECT_EQ(shapes.at("words"), (Dimensions{ArrayDimension{.left = 0, .right = 1}}));
    EXPECT_EQ(shapes.at("up"), (Dimensions{ArrayDimension{.left = 0, .right = 3}}));
    EXPECT_TRUE(shapes.at("flag").empty());
    EXPECT_EQ(shapes.at("pairs"), (Dimensions{ArrayDimension{.left = 1, .right = 0}}));

    // Every one of them is recorded for the window, the unpacked ones too.
    for (const enzan::ipc::ObjectEntry& entry : objects.objects)
        EXPECT_EQ(entry.recording, enzan::ipc::Recording::Recorded) << entry.name;
}
#endif

#ifdef ENZAN_WITH_VHDL
TEST_F(ServerFixture, AVhdlArrayIsListedWithTheDimensionsItsTypeDeclares) {
    rebuildFrom({{"tb.vhd", R"(
        library ieee;
        use ieee.std_logic_1164.all;
        entity tb is
        end entity;
        architecture a of tb is
            type nibbles_t is array (0 to 1) of std_logic_vector(3 downto 0);
            type grid_t is array (1 to 2, 3 downto 0) of std_logic;
            type integers_t is array (2 downto 0) of integer;
            signal nibbles : nibbles_t := (x"a", x"5");
            signal grid : grid_t;
            signal integers : integers_t;
            signal up : std_logic_vector(0 to 7);
            signal flag : std_logic;
        begin
        end architecture;
    )"}});
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);
    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    const auto shapes = dimensionsByName(objects);

    using enzan::eir::ArrayDimension;
    using Dimensions = std::vector<ArrayDimension>;
    EXPECT_EQ(shapes.at("nibbles"), (Dimensions{ArrayDimension{.left = 0, .right = 1},
                                                ArrayDimension{.left = 3, .right = 0}}));
    EXPECT_EQ(shapes.at("grid"), (Dimensions{ArrayDimension{.left = 1, .right = 2},
                                             ArrayDimension{.left = 3, .right = 0}}));
    EXPECT_EQ(shapes.at("integers"), (Dimensions{ArrayDimension{.left = 2, .right = 0}}));
    EXPECT_EQ(shapes.at("up"), (Dimensions{ArrayDimension{.left = 0, .right = 7}}));
    EXPECT_TRUE(shapes.at("flag").empty());

    const auto nibbles =
        std::ranges::find(objects.objects, "nibbles", &enzan::ipc::ObjectEntry::name);
    ASSERT_NE(nibbles, objects.objects.end());
    EXPECT_EQ(nibbles->width, 8U) << "a bit for each std_logic, not a byte for each code";
}
#endif

TEST_F(ServerFixture, AMemoryAndASignalTooWideToRecordAreRecordedOnceShown) {
    // The database leaves the wide one out of what it records of its own accord; the reader put
    // it in the window all the same, and its trace from then on is all the window can show of it.
    waveformWidthLimit = 100;
    rebuild(shapedDesign());
    connectClient();

    SubscribeMessage subscribe;
    subscribe.signalIndices = {signalIndex("/tb/memory"), signalIndex("/tb/wide")};
    request(MessageType::Subscribe, subscribe);

    for (const auto& [path, value] :
         {std::pair{"/tb/memory", "32'h01020304"}, std::pair{"/tb/wide", "200'h5"}}) {
        DepositMessage deposit;
        deposit.signalIndex = signalIndex(path);
        deposit.value = literal(value);
        request(MessageType::Deposit, deposit);
    }

    const std::vector<ChangeEntry>& memory = fetchAll(signalIndex("/tb/memory"));
    ASSERT_FALSE(memory.empty());
    EXPECT_EQ(memory.back().value, literal("32'h01020304"));
    const std::vector<ChangeEntry>& wide = fetchAll(signalIndex("/tb/wide"));
    ASSERT_FALSE(wide.empty());
    EXPECT_EQ(wide.back().value, literal("200'h5"));
}

TEST_F(ServerFixture, WhatTheWindowLeavesOutForItsWidthIsReportedOnce) {
    waveformWidthLimit = 100;
    rebuild(shapedDesign());
    const auto warnings = [this] {
        return std::ranges::count_if(consumer.messages, [](const std::string& message) {
            return message.starts_with("the wave window records no signal wider than 100 bits");
        });
    };
    ASSERT_EQ(warnings(), 1) << (consumer.messages.empty() ? "" : consumer.messages.back());
    EXPECT_NE(consumer.messages.back().find("/tb/wide of 200 bits"), std::string::npos)
        << consumer.messages.back();

    // A restart leaves out the same, and is not told again.
    connectClient();
    enzan::ipc::RestartMessage restart;
    request(MessageType::Restart, restart);
    EXPECT_EQ(warnings(), 1);
}

TEST_F(ServerFixture, NeitherAMadeUpNameNorAGenvarsCopyIsListed) {
    rebuild(introducedDesign());
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);
    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    ASSERT_EQ(objects.objects.size(), 1U);
    EXPECT_EQ(objects.objects[0].name, "count_lin");
    ASSERT_EQ(objects.parameters.size(), 1U) << "a localparam of the block is the reader's";
    EXPECT_EQ(objects.parameters[0].name, "gen[0].TWICE");

    // The storage is the child's port, and is listed, and recorded, as that.
    query.instancePath = "/tb/gen[0].u";
    request(MessageType::QueryObjects, query);
    const auto inside = expect<ObjectsMessage>(MessageType::Objects);
    ASSERT_EQ(inside.objects.size(), 1U);
    EXPECT_EQ(inside.objects[0].name, "count");
    EXPECT_EQ(inside.objects[0].signalIndex, signalIndex("/tb/gen[0].u$count"));
    EXPECT_EQ(inside.objects[0].recording, enzan::ipc::Recording::Recorded);
}

#ifdef ENZAN_WITH_SYSTEMVERILOG
TEST_F(ServerFixture, AGenerateLoopOfCountersListsWhatItsSourceDeclares) {
    // large_design_00's shape: a counter per iteration, its port connected to an element.
    rebuildFrom({{"tb.sv", R"(
        module counter #(parameter int WIDTH = 4) (input logic clk, output logic [WIDTH-1:0] count);
            always_ff @(posedge clk) count <= count + 1'b1;
        endmodule

        module tb;
            logic clk = 1'b0;
            logic [1:0][3:0] count_lin;
            genvar I;
            for (I = 0; I < 2; I++) begin : cntr_lin_gen
                counter cntr_i (.clk(clk), .count(count_lin[I]));
            end
        endmodule
    )"}});
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);
    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    std::vector<std::string> names;
    names.reserve(objects.objects.size());
    for (const enzan::ipc::ObjectEntry& entry : objects.objects) names.push_back(entry.name);
    for (const enzan::ipc::ParameterEntry& entry : objects.parameters) names.push_back(entry.name);
    std::ranges::sort(names);
    EXPECT_EQ(names, (std::vector<std::string>{"clk", "count_lin"}));

    query.instancePath = "/tb/cntr_lin_gen[1].cntr_i";
    request(MessageType::QueryObjects, query);
    const auto inside = expect<ObjectsMessage>(MessageType::Objects);
    const auto count = std::ranges::find(inside.objects, "count", &enzan::ipc::ObjectEntry::name);
    ASSERT_NE(count, inside.objects.end());
    EXPECT_EQ(count->recording, enzan::ipc::Recording::Recorded);
}
#endif

TEST_F(ServerFixture, AnUnknownScopeIsAFailureRatherThanAnEmptyAnswer) {
    // An empty list and a mistyped path must not look the same: the user would conclude the
    // scope had no signals rather than that it does not exist.
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb/nowhere";
    request(MessageType::QueryObjects, query);

    const auto failure = expect<FailureMessage>(MessageType::Failure);
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

    const auto states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_GE(states.size(), 2U)
        << "the client must learn when a run starts, not only when it ends";
    EXPECT_EQ(states.front().state, SimulationState::Running);
    EXPECT_EQ(states.back().state, SimulationState::Idle);
    EXPECT_EQ(states.back().time, 100U);
    EXPECT_EQ(server->kernel().time(), 100U);
}

TEST_F(ServerFixture, AShownSignalsChangesAreFetchedAsTheyWereRecorded) {
    connectClient();

    SubscribeMessage subscribe;
    subscribe.signalIndices = {signalIndex("/tb/clk")};
    request(MessageType::Subscribe, subscribe);

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    EXPECT_FALSE(sawAny(MessageType::Changes)) << "the kernel sends no value it is not asked for";

    // One settled time slot a change, in increasing time: delta-level intermediate values must
    // never reach the view. And caught up, the copy is complete to the end.
    const std::vector<ChangeEntry>& clock = fetchAll(signalIndex("/tb/clk"));
    ASSERT_GE(clock.size(), 20U) << "a clock that toggled twenty times has a past";
    EXPECT_EQ(clock.front().time, 0U);
    for (std::size_t index = 1; index < clock.size(); ++index) {
        EXPECT_GT(clock[index].time, clock[index - 1].time);
        EXPECT_NE(clock[index].value, clock[index - 1].value);
    }
    const ChangesMessage settled = fetch({signalIndex("/tb/clk")});
    ASSERT_EQ(settled.signals.size(), 1U);
    EXPECT_TRUE(settled.signals[0].entries.empty());
    EXPECT_EQ(settled.signals[0].completeTo, 100U);
}

TEST_F(ServerFixture, ARunSaysHowFarItHasGotAndSendsNoValues) {
    // The client pulls what it shows; a run tells it there is more and nothing else, which is
    // what keeps a client that has fallen behind from costing the kernel a queue on the socket.
    connectClient();

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);

    EXPECT_FALSE(sawAny(MessageType::Changes));
    const auto states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(states.back().latestRecordedTime, 100U);
}

TEST_F(ServerFixture, StopEndsARunThatIsAlreadyInProgress) {
    // Stopping a run from the front end, in its most visible form. The design asked for runs for
    // ever, so this test only ends because the Stop reaches the kernel's interrupt while it is
    // simulating. The stop is sent from another thread for the same reason: the server is inside
    // `run` and will not return until it arrives.
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

    const auto states = expectAll<RunStateMessage>(MessageType::RunState);
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

    const auto states = expectAll<RunStateMessage>(MessageType::RunState);
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

    const auto states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_FALSE(states.empty());
    EXPECT_EQ(states.back().state, SimulationState::Finished);

    RunMessage again;
    again.duration = 10;
    request(MessageType::Run, again);
    const auto failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("restart"), std::string::npos) << failure.reason;

    enzan::ipc::RestartMessage restart;
    request(MessageType::Restart, restart);
    EXPECT_EQ(server->kernel().time(), 0U);

    request(MessageType::Run, run);
    EXPECT_FALSE(sawAny(MessageType::Failure));
    EXPECT_EQ(server->kernel().time(), 20U) << "the design must run again from the beginning";
}

TEST_F(ServerFixture, ARunPastTheEndOfTimeIsRefused) {
    // Time overflowing is detected and reported rather than wrapping silently. The
    // kernel must be some way into the run for the sum to overflow at all, so it is.
    connectClient();

    RunMessage first;
    first.duration = 50;
    request(MessageType::Run, first);
    ASSERT_EQ(server->kernel().time(), 50U);

    RunMessage tooFar;
    tooFar.duration = enzan::runtime::NoTime;
    request(MessageType::Run, tooFar);

    const auto failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("64-bit range"), std::string::npos) << failure.reason;
    EXPECT_EQ(server->kernel().time(), 50U);
}

// ---------------------------------------------------------------------------
// Attaching part-way through, and leaving
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, AClientAttachingMidRunSeesWhatItMissed) {
    // The reason the kernel keeps a waveform database: a GUI that attaches to a simulation
    // already in progress must show its past, not a blank window.
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
    subscribe.signalIndices = {signalIndex("/tb/clk")};
    request(MessageType::Subscribe, subscribe);

    const std::vector<ChangeEntry>& clock = fetchAll(signalIndex("/tb/clk"));
    EXPECT_GE(clock.size(), 20U) << "a clock that toggled twenty times has a past";
    ASSERT_FALSE(clock.empty());
    EXPECT_EQ(clock.front().time, 0U);
}

TEST_F(ServerFixture, DetachingReleasesTheClientAndLeavesTheSimulationIntact) {
    // The GUI must be able to detach without terminating the kernel, which is how a
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
    // A signal outside the logging scope, recorded only because it is shown, so that what is kept
    // across the restart is the showing and not the scope.
    waveformScope = "/tb/u_dut";
    rebuild(tickingDesign());
    connectClient();

    SubscribeMessage subscribe;
    subscribe.signalIndices = {signalIndex("/tb/clk")};
    request(MessageType::Subscribe, subscribe);

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    ASSERT_EQ(server->kernel().time(), 100U);
    received.clear();

    enzan::ipc::RestartMessage restart;
    request(MessageType::Restart, restart);

    const auto state = expect<RunStateMessage>(MessageType::RunState);
    EXPECT_EQ(state.time, 0U) << "time going backwards is how the client learns to discard";
    EXPECT_EQ(state.state, SimulationState::Idle);
    EXPECT_EQ(server->kernel().time(), 0U);

    // The signal is recorded again from time zero, for the client to fetch afresh: losing the
    // wave window's contents would make the button useless for what it is used for.
    held.clear();
    const std::vector<ChangeEntry>& clock = fetchAll(signalIndex("/tb/clk"));
    ASSERT_EQ(clock.size(), 1U);
    EXPECT_EQ(clock.front().time, 0U);

    // And the design runs again from the beginning.
    RunMessage again;
    again.duration = 40;
    request(MessageType::Run, again);
    EXPECT_EQ(server->kernel().time(), 40U);
    EXPECT_GE(fetchAll(signalIndex("/tb/clk")).size(), 8U);
}

TEST_F(ServerFixture, RestartIsRefusedWhileAVpiApplicationIsLoaded) {
    // The client's restart reaches the kernel without going through the session, so the server
    // has to make the session's refusal itself.
    rebuild(tickingDesign(), true);
    connectClient();

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    ASSERT_EQ(server->kernel().time(), 100U);

    enzan::ipc::RestartMessage restart;
    request(MessageType::Restart, restart);

    const auto failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("VPI"), std::string::npos) << failure.reason;
    EXPECT_EQ(server->kernel().time(), 100U) << "nothing was put back";
}

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, TheTranscriptGoesToTheClientAndToTheTerminal) {
    // A kernel serving a socket is still a program someone started in a terminal. Swallowing
    // its output because a GUI happened to be attached would be surprising.
    connectClient();

    const auto transcript = expect<TranscriptMessage>(MessageType::Transcript);
    EXPECT_NE(transcript.text.find("attached"), std::string::npos) << transcript.text;
    EXPECT_NE(localTranscript.str().find("attached"), std::string::npos) << localTranscript.str();
}

// ---------------------------------------------------------------------------
// Captured output
// ---------------------------------------------------------------------------

/// Points this process's standard output and standard error somewhere else for one test, and
/// puts them back, so that what a test captures is what it wrote and the terminal is left as it
/// was found.
class StandardStreams {
public:
    StandardStreams(int output, int error)
        : savedOutput(::dup(STDOUT_FILENO)), savedError(::dup(STDERR_FILENO)) {
        std::fflush(nullptr);
        ::dup2(output, STDOUT_FILENO);
        ::dup2(error, STDERR_FILENO);
    }

    StandardStreams(const StandardStreams&) = delete;
    StandardStreams& operator=(const StandardStreams&) = delete;
    StandardStreams(StandardStreams&&) = delete;
    StandardStreams& operator=(StandardStreams&&) = delete;

    ~StandardStreams() {
        std::fflush(nullptr);
        ::dup2(savedOutput, STDOUT_FILENO);
        ::dup2(savedError, STDERR_FILENO);
        ::close(savedOutput);
        ::close(savedError);
    }

private:
    int savedOutput;
    int savedError;
};

/// A file standing in for a terminal or a log, read back once the test is done with it.
class Scratch {
public:
    Scratch() {
        std::array<char, 32> name{};
        std::snprintf(name.data(), name.size(), "/tmp/enzan-capture-XXXXXX");
        descriptor = ::mkstemp(name.data());
        path = name.data();
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;

    ~Scratch() {
        ::close(descriptor);
        ::unlink(path.c_str());
    }

    [[nodiscard]] std::string contents() const {
        std::string text;
        std::array<char, 4096> buffer{};
        for (::ssize_t offset = 0;;) {
            const ::ssize_t count = ::pread(descriptor, buffer.data(), buffer.size(), offset);
            if (count <= 0) break;
            text.append(buffer.data(), static_cast<std::size_t>(count));
            offset += count;
        }
        return text;
    }

    int descriptor = -1;
    std::string path;
};

void writeTo(int descriptor, std::string_view text) {
    ASSERT_EQ(::write(descriptor, text.data(), text.size()), static_cast<::ssize_t>(text.size()));
}

class CaptureFixture : public ServerFixture {
protected:
    /// The capture is stopped before the streams are put back. Stopping restores the
    /// descriptors the capture found, so the other order would leave this test binary writing
    /// into a scratch file that no longer exists.
    void TearDown() override {
        server.reset();
        streams.reset();
        if (controller >= 0) ::close(controller);
        if (terminal >= 0) ::close(terminal);
        ServerFixture::TearDown();
    }

    /// Points standard output and standard error at `output` and `error`, connects a client
    /// and starts capturing.
    void capture(int output, int error) {
        streams.emplace(output, error);
        connectClient();
        std::string failure;
        ASSERT_TRUE(server->captureOutput(failure)) << failure;
    }

    /// Everything the client has been sent as transcript, waiting up to two seconds for
    /// `wanted` to be part of it: the capture is drained on a thread of its own, so what was
    /// written arrives a moment later rather than within the step that follows.
    std::string transcriptContaining(std::string_view wanted) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (true) {
            pumpServer();
            for (const TranscriptMessage& message :
                 expectAll<TranscriptMessage>(MessageType::Transcript)) {
                collected += message.text;
            }
            if (collected.find(wanted) != std::string::npos) return collected;
            if (std::chrono::steady_clock::now() > deadline) return collected;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Scratch log;
    Scratch errors;
    std::optional<StandardStreams> streams;
    int controller = -1;
    int terminal = -1;
    std::string collected;
};

TEST_F(CaptureFixture, WhatTheProcessWritesReachesTheTranscriptAndTheTerminal) {
    // The point of capturing: a VPI application writes to the process's standard output, not
    // through Enzan, and cocotb's whole log is written that way.
    capture(log.descriptor, log.descriptor);
    writeTo(STDOUT_FILENO, "from an application\n");
    EXPECT_NE(transcriptContaining("from an application").find("from an application\n"),
              std::string::npos);

    server.reset();
    EXPECT_NE(log.contents().find("from an application\n"), std::string::npos)
        << "the terminal still gets what it always got";
}

TEST_F(CaptureFixture, StandardErrorIsCapturedWhenItIsTheSameFileAsStandardOutput) {
    capture(log.descriptor, log.descriptor);
    writeTo(STDERR_FILENO, "a traceback\n");
    EXPECT_NE(transcriptContaining("a traceback").find("a traceback"), std::string::npos);
}

TEST_F(CaptureFixture, StandardErrorKeptApartFromStandardOutputIsLeftAlone) {
    // Merging two destinations the user kept apart would put errors in a log meant for output.
    capture(log.descriptor, errors.descriptor);
    writeTo(STDERR_FILENO, "kept apart\n");
    writeTo(STDOUT_FILENO, "marker\n");
    EXPECT_EQ(transcriptContaining("marker").find("kept apart"), std::string::npos);

    server.reset();
    EXPECT_NE(errors.contents().find("kept apart"), std::string::npos);
    EXPECT_EQ(log.contents().find("kept apart"), std::string::npos);
}

TEST_F(CaptureFixture, TheKernelsOwnLinesAreNotSentTwice) {
    // With the echo going to standard output, the capture is what carries the kernel's own
    // transcript, and the relay must stop keeping a copy of it.
    ServerOptions options;
    options.kernelVersion = "enzan test";
    options.timeScale = TimeScale(-9);
    server.reset();
    server = std::make_unique<KernelServer>(*session, engine, options);
    server->start();

    capture(log.descriptor, log.descriptor);
    server->transcript() << "once only\n";
    writeTo(STDOUT_FILENO, "and then\n");

    const std::string transcript = transcriptContaining("and then");
    const std::size_t first = transcript.find("once only");
    ASSERT_NE(first, std::string::npos) << transcript;
    EXPECT_EQ(transcript.find("once only", first + 1), std::string::npos) << transcript;
    EXPECT_LT(first, transcript.find("and then")) << "in the order it was written";
}

TEST_F(CaptureFixture, ATerminalIsCapturedAsATerminalAndItsColoursReachBoth) {
    // A program writing to a pipe buffers in blocks, and its lines would reach the window long
    // after the events they describe; a terminal is captured with a terminal for that reason.
    ASSERT_EQ(::openpty(&controller, &terminal, nullptr, nullptr, nullptr), 0);
    capture(terminal, terminal);
    EXPECT_EQ(::isatty(STDOUT_FILENO), 1) << "what an application sees is still a terminal";

    writeTo(STDOUT_FILENO, "\x1b[32mpassed\x1b[0m\n");
    // The window shows the colours, as the terminal does.
    const std::string transcript = transcriptContaining("passed");
    EXPECT_NE(transcript.find("\x1b[32mpassed\x1b[0m\n"), std::string::npos) << transcript;

    server.reset();

    // The real terminal got the bytes as written, colour included.
    ::fcntl(controller, F_SETFL, ::fcntl(controller, F_GETFL) | O_NONBLOCK);
    std::string echoed;
    std::array<char, 4096> buffer{};
    for (::ssize_t count = 0; (count = ::read(controller, buffer.data(), buffer.size())) > 0;)
        echoed.append(buffer.data(), static_cast<std::size_t>(count));
    EXPECT_NE(echoed.find("\x1b[32mpassed\x1b[0m"), std::string::npos) << echoed;
}

TEST_F(CaptureFixture, MoreThanTheCaptureHoldsDoesNotStallTheWriter) {
    // One callback can write more than a pipe holds. The capture is drained on a thread of its
    // own because a writer that had to wait for its own thread to drain would wait for ever.
    capture(log.descriptor, log.descriptor);
    const std::string line(1023, 'x');
    for (int index = 0; index < 512; ++index) writeTo(STDOUT_FILENO, line + '\n');
    writeTo(STDOUT_FILENO, "the end\n");

    EXPECT_NE(transcriptContaining("the end").find("the end"), std::string::npos);
}

TEST_F(CaptureFixture, StoppingPutsTheDescriptorsBack) {
    capture(log.descriptor, log.descriptor);
    server.reset();

    writeTo(STDOUT_FILENO, "after the server\n");
    EXPECT_NE(log.contents().find("after the server\n"), std::string::npos);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

TEST_F(ServerFixture, AnAnswerSharesItsBytesAndSaysWhereEachCopyIsComplete) {
    // Room for five changes, fewer than the clock's past: had the first signal asked for taken all
    // it could, the one after it would wait on it. And the clock's copy is complete up to the first
    // change left out, which is as far as the client may draw it.
    changesBytes = 5 * KernelServer::changeBytes(1);
    rebuild(tickingDesign());
    connectClient();

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    received.clear();

    const std::uint32_t clk = signalIndex("/tb/clk");
    const std::uint32_t q = signalIndex("/tb/u_dut/q");
    const ChangesMessage first = fetch({clk, q});
    ASSERT_EQ(first.signals.size(), 2U);
    const SignalChanges& clock = first.signals[0];
    ASSERT_FALSE(clock.entries.empty());
    ASSERT_LT(clock.entries.size(), clock.recorded);
    EXPECT_EQ(clock.completeTo, clock.entries.back().time + 5);
    const SignalChanges& counter = first.signals[1];
    EXPECT_EQ(counter.entries.size(), counter.recorded)
        << "a share for each, not all for the first";
    EXPECT_EQ(counter.completeTo, 100U);

    // Asking again goes on from where the copy ends, until it is caught up and complete.
    for (int round = 0; round < 100 && held[clk].size() < clock.recorded; ++round)
        static_cast<void>(fetch({clk, q}));
    const ChangesMessage last = fetch({clk, q});
    for (const SignalChanges& changes : last.signals) {
        EXPECT_TRUE(changes.entries.empty());
        EXPECT_EQ(held[changes.signalIndex].size(), changes.recorded);
        EXPECT_EQ(changes.completeTo, 100U);
    }
}

TEST_F(ServerFixture, ARunSaysNothingOfItsProgressToAClientThatHasNotRead) {
    // Only the latest progress is worth reading, so none is queued behind output the client has
    // not read: a client that stops reading costs the socket no more however long a run goes on.
    connectClient();
    RunMessage before;
    before.duration = 1'000'000;
    request(MessageType::Run, before);
    received.clear();

    // An answer of the clock's whole past, megabytes, far more than the socket holds, and a run
    // asked for behind it, with the client reading none of it until the run is over.
    FetchChangesMessage everything;
    everything.signals = {ChangeCursor{.signalIndex = signalIndex("/tb/clk"), .held = 0}};
    client.send(MessageType::FetchChanges, everything);
    RunMessage run;
    run.duration = 2'000'000;
    client.send(MessageType::Run, run);
    ASSERT_TRUE(client.flushBlocking()) << client.error();
    for (int round = 0; round < 8; ++round) static_cast<void>(server->step(5));
    ASSERT_EQ(server->kernel().time(), 3'000'000U);

    const auto ended = [this] {
        return std::ranges::any_of(received, [](const Captured& captured) {
            RunStateMessage state;
            return captured.type == static_cast<std::uint16_t>(MessageType::RunState)
                   && enzan::ipc::decodePayload(captured.payload, state)
                   && state.state != SimulationState::Running;
        });
    };
    for (int round = 0; round < 10'000 && !ended(); ++round) {
        static_cast<void>(server->step(1));
        collect();
    }
    ASSERT_TRUE(ended());
    EXPECT_FALSE(sawAny(MessageType::Progress));
    EXPECT_EQ(expect<ChangesMessage>(MessageType::Changes).signals.at(0).entries.size(),
              ServerOptions{}.changesBytes / KernelServer::changeBytes(1));
}

TEST(KernelServerChanges, AnAnswerFitsAFrameWhateverItsSignalsWidths) {
    // An answer carries its budget's worth, or one change when one alone is larger; either has
    // to be a frame the client accepts.
    const std::size_t widest = KernelServer::changeBytes(enzan::ipc::MaxSignalBits);
    EXPECT_LT(ServerOptions{}.changesBytes + widest, enzan::ipc::MaxPayloadBytes);
    EXPECT_GT(KernelServer::changeBytes(4096), KernelServer::changeBytes(1));
}

TEST_F(ServerFixture, ASignalOfVhdlCodesIsListedAtTheWidthItsValuesComeAt) {
    // A std_ulogic is held as its code and its values are sent as the bit it stands for. A
    // client told the code's width keeps a trace eight bits wide and drops every one of them as
    // the wrong width, which left every std_logic of a VHDL design without a value or a wave.
    rebuild(codedDesign());
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);
    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    ASSERT_EQ(objects.objects.size(), 1U);
    EXPECT_EQ(objects.objects[0].width, 1U);

    SubscribeMessage subscribe;
    subscribe.signalIndices = {signalIndex("/tb/ready")};
    request(MessageType::Subscribe, subscribe);
    const ChangesMessage first = fetch({signalIndex("/tb/ready")});
    ASSERT_EQ(first.signals.size(), 1U);
    EXPECT_EQ(first.signals[0].width, 1U);
    ASSERT_EQ(first.signals[0].entries.size(), 1U);
    EXPECT_EQ(first.signals[0].entries[0].value.width(), 1U);
    EXPECT_EQ(
        enzan::eir::toString(first.signals[0].entries[0].value.ref(), enzan::eir::Radix::Binary),
        "1");

    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    const std::vector<ChangeEntry>& ready = fetchAll(signalIndex("/tb/ready"));
    ASSERT_EQ(ready.size(), 2U);
    EXPECT_EQ(ready.back().value.width(), 1U);
    EXPECT_EQ(enzan::eir::toString(ready.back().value.ref(), enzan::eir::Radix::Binary), "0");
}

TEST_F(ServerFixture, ASignalOfANullRangeIsListedWithNoBitsAndSendsNothing) {
    // Its one bit is a placeholder, shown as no bits at all; a value of it would be one of no
    // bits, which no bit vector can be, and the kernel stopped on it.
    rebuild(nullDesign());
    connectClient();

    enzan::ipc::QueryObjectsMessage query;
    query.instancePath = "/tb";
    request(MessageType::QueryObjects, query);
    const auto objects = expect<ObjectsMessage>(MessageType::Objects);
    ASSERT_EQ(objects.objects.size(), 2U);
    EXPECT_EQ(objects.objects[1].name, "nothing");
    EXPECT_EQ(objects.objects[1].width, 0U);

    SubscribeMessage subscribe;
    subscribe.signalIndices = {signalIndex("/tb/ready"), signalIndex("/tb/nothing")};
    request(MessageType::Subscribe, subscribe);
    const ChangesMessage answer = fetch({signalIndex("/tb/ready"), signalIndex("/tb/nothing")});
    ASSERT_EQ(answer.signals.size(), 2U);
    EXPECT_FALSE(answer.signals[0].entries.empty());
    EXPECT_EQ(answer.signals[1].width, 0U);
    EXPECT_TRUE(answer.signals[1].entries.empty());
    EXPECT_EQ(answer.signals[1].recorded, 0U);

    // And the kernel is still there to run.
    RunMessage run;
    run.duration = 100;
    request(MessageType::Run, run);
    const auto states = expectAll<RunStateMessage>(MessageType::RunState);
    ASSERT_GE(states.size(), 2U);
    EXPECT_EQ(states.front().state, SimulationState::Running);
    EXPECT_NE(states.back().state, SimulationState::Running);
}

TEST_F(ServerFixture, ADepositIntoAVhdlSignalIsTheBitsItShowsAs) {
    // As the window sends one, at the width the signal was listed at: `0` into a std_logic is
    // its '0', and nothing can be written into a null array.
    rebuild(nullDesign());
    connectClient();

    DepositMessage deposit;
    deposit.signalIndex = signalIndex("/tb/ready");
    deposit.value = literal("1'b0");
    request(MessageType::Deposit, deposit);
    EXPECT_EQ(session->examine(signalIndex("/tb/ready"), enzan::eir::Radix::Binary), "0");

    ForceMessage force;
    force.signalIndex = signalIndex("/tb/nothing");
    force.value = literal("1'b1");
    request(MessageType::Force, force);
    const auto failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("null array"), std::string::npos) << failure.reason;
}

TEST_F(ServerFixture, AskingAboutASignalThatDoesNotExistIsAFailure) {
    connectClient();

    FetchChangesMessage fetchNothing;
    fetchNothing.signals = {ChangeCursor{.signalIndex = 9999, .held = 0}};
    request(MessageType::FetchChanges, fetchNothing);

    const auto failure = expect<FailureMessage>(MessageType::Failure);
    EXPECT_NE(failure.reason.find("no signal"), std::string::npos) << failure.reason;
}

// ---------------------------------------------------------------------------
// Writing a signal from outside the simulation
// ---------------------------------------------------------------------------

/// A served design a client can write signals in.
class DebugFixture : public ServerFixture {
protected:
    void SetUp() override {
        sourceBuffer = sources.addBuffer("design.sv", std::string(DebugSource));
        rebuild(debuggableDesign(sourceBuffer));
        connectClient();
    }

    /// The value the client last fetched for `/tb/q`, in decimal.
    [[nodiscard]] std::string lastSeenQ() {
        const std::vector<ChangeEntry>& changes = fetchAll(signalIndex("/tb/q"));
        if (changes.empty()) return {};
        return enzan::eir::toString(changes.back().value.ref(), enzan::eir::Radix::Decimal);
    }

    enzan::BufferId sourceBuffer;
};

TEST_F(DebugFixture, TheDesignTreeNamesWhereEachScopeIsDeclared) {
    enzan::ipc::QueryDesignMessage query;
    request(MessageType::QueryDesign, query);

    const auto design = expect<DesignMessage>(MessageType::Design);
    ASSERT_EQ(design.instances.size(), 1U);
    EXPECT_EQ(design.instances[0].file, "design.sv");
    EXPECT_EQ(design.instances[0].line, 1U);
    EXPECT_EQ(design.sourceFiles, (std::vector<std::string>{"design.sv"}));
}

TEST_F(DebugFixture, ADepositIsWrittenAndTheClientSeesIt) {
    SubscribeMessage subscribe;
    subscribe.signalIndices = {signalIndex("/tb/q")};
    request(MessageType::Subscribe, subscribe);
    static_cast<void>(lastSeenQ());

    DepositMessage deposit;
    deposit.signalIndex = signalIndex("/tb/q");
    deposit.value = literal("4'd9");
    request(MessageType::Deposit, deposit);

    // A deposit between runs produces no time advance, so the change has to be recorded then,
    // and the client told that there is more to fetch, for the window showing it to move at all.
    EXPECT_TRUE(sawAny(MessageType::Progress));
    EXPECT_EQ(lastSeenQ(), "9");
}

TEST_F(DebugFixture, AForceHoldsAValueAgainstTheDesignAndAReleaseLetsItGo) {
    ForceMessage force;
    force.signalIndex = signalIndex("/tb/q");
    force.value = literal("4'd1");
    request(MessageType::Force, force);

    RunMessage run;
    run.untilFinished = true;
    request(MessageType::Run, run);
    static_cast<void>(expectAll<RunStateMessage>(MessageType::RunState));

    // The design wrote 3 and then 5; the force outranks both.
    EXPECT_EQ(lastSeenQ(), "1");

    ForceMessage release;
    release.signalIndex = signalIndex("/tb/q");
    release.release = true;
    request(MessageType::Force, release);
    EXPECT_FALSE(sawAny(MessageType::Failure));
}

TEST_F(DebugFixture, ReleasingSomethingThatWasNotForcedIsNotAnError) {
    ForceMessage release;
    release.signalIndex = signalIndex("/tb/q");
    release.release = true;
    request(MessageType::Force, release);

    EXPECT_FALSE(sawAny(MessageType::Failure));
}

TEST_F(DebugFixture, ADepositOrForceOnASignalThatDoesNotExistIsRefused) {
    DepositMessage deposit;
    deposit.signalIndex = 9999;
    deposit.value = literal("4'd1");
    request(MessageType::Deposit, deposit);
    EXPECT_NE(expect<FailureMessage>(MessageType::Failure).reason.find("9999"), std::string::npos);

    ForceMessage force;
    force.signalIndex = 9999;
    force.value = literal("4'd1");
    request(MessageType::Force, force);
    EXPECT_NE(expect<FailureMessage>(MessageType::Failure).reason.find("9999"), std::string::npos);
}

} // namespace
