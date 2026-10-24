// SPDX-License-Identifier: GPL-3.0-or-later
//
// A kernel, a socket and a session, wired together the way the two processes really are.
//
// The GUI's half is small and the kernel's half is tested elsewhere; what is worth testing here
// is the join, and a fake kernel would not test it. So the fixture elaborates a real design,
// serves it over a real Unix domain socket, and attaches a real `KernelSession` to it — the only
// difference from the shipped arrangement being that both ends live in one process and the
// fixture takes turns driving them.

#ifndef ENZAN_TESTS_UNIT_GUI_SESSIONFIXTURE_H
#define ENZAN_TESTS_UNIT_GUI_SESSIONFIXTURE_H

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
#include "gui/KernelSession.h"
#include "ipc/KernelServer.h"
#include "runtime/Image.h"
#include "runtime/Time.h"
#include "session/Session.h"

#include "support/ProcessBuilder.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace enzan::test {

/// A design with a clock that toggles every five time units and an eight-bit counter that
/// follows it, inside a child instance. Enough hierarchy for the Structure window to have
/// something to show, and enough activity for a trace to have a shape.
inline eir::Design guiTestDesign() {
    ProcessBuilder clock(eir::ProcessKind::Initial, "clock");
    const std::uint32_t delay = clock.reg(eir::Type::bits(32, false, false));
    const std::uint32_t level = clock.reg(eir::Type::logic());
    const std::uint32_t inverted = clock.reg(eir::Type::logic());
    const std::uint32_t five = clock.constant("32'd5");

    clock.block("wait");
    clock.emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(delay),
                                      eir::Operand::constant(five)));
    clock.emit(eir::Instruction::make(eir::Opcode::WaitTime, {}, eir::Operand::reg(delay),
                                      eir::Operand::block(1)));
    clock.block("toggle");
    clock.readObject(level, 0);
    clock.emit(eir::Instruction::make(eir::Opcode::Not, eir::Operand::reg(inverted),
                                      eir::Operand::reg(level)));
    clock.store(0, inverted);
    clock.emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(0)));

    ProcessBuilder counter(eir::ProcessKind::AlwaysFf, "counter");
    const std::uint32_t current = counter.reg(eir::Type::bits(8, false, true));
    const std::uint32_t one = counter.reg(eir::Type::bits(8, false, true));
    const std::uint32_t sum = counter.reg(eir::Type::bits(8, false, true));
    const std::uint32_t oneConstant = counter.constant("8'd1");

    counter.sensitive(0, eir::EdgeKind::Posedge);
    counter.block("entry");
    counter.readObject(current, 1);
    counter.emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(one),
                                        eir::Operand::constant(oneConstant)));
    counter.emit(eir::Instruction::make(eir::Opcode::Add, eir::Operand::reg(sum),
                                        eir::Operand::reg(current), eir::Operand::reg(one)));
    counter.storeNonBlocking(1, sum);
    counter.emit(eir::Instruction::make(eir::Opcode::Return));

    eir::ModuleDefinition child;
    child.name = "child";
    child.sourceName = "child";
    child.addObject(eir::Object{.name = "q", .type = eir::Type::bits(4, false, true)});

    // A parameter and a localparam, so that the path from a definition through the protocol to
    // a row of the wave window has something to carry. They belong to the child, which is what
    // makes them a different scope's from the signals above.
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    child.parameters.push_back(eir::Parameter{
        .name = "WIDTH", .value = *eir::BitVector::parse("32'd4"), .isLocal = false});
    child.parameters.push_back(eir::Parameter{
        .name = "DEPTH", .value = *eir::BitVector::parse("32'd16"), .isLocal = true});
    // NOLINTEND(bugprone-unchecked-optional-access)

    eir::ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.addObject(eir::Object{// NOLINTBEGIN(bugprone-unchecked-optional-access)
                              .name = "clk",
                              .type = eir::Type::logic(),
                              .initialValue = eir::BitVector::parse("1'b0")});
    // NOLINTEND(bugprone-unchecked-optional-access)
    top.addObject(eir::Object{.name = "count",
                              .type = eir::Type::bits(8, false, true),
                              // NOLINTBEGIN(bugprone-unchecked-optional-access)
                              .initialValue = eir::BitVector::parse("8'd0")});
    // NOLINTEND(bugprone-unchecked-optional-access)

    // An enumerated signal, so that the path from a frontend's enumeration through elaboration
    // and the protocol to what the wave window writes on a trace has something to carry. It is
    // deliberately not driven: what is under test is how a value is *shown*, and a signal that
    // sits at its initial value shows the same thing at every time.
    const std::uint32_t stateEnum = top.addEnum(eir::EnumDefinition{
        .name = "state_t",
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        .members = {eir::EnumMember{.name = "S_IDLE", .value = *eir::BitVector::parse("3'd0")},
                    eir::EnumMember{.name = "S_ARM", .value = *eir::BitVector::parse("3'd1")},
                    eir::EnumMember{.name = "S_SEND", .value = *eir::BitVector::parse("3'd2")}}});
    // NOLINTEND(bugprone-unchecked-optional-access)
    top.addObject(eir::Object{.name = "state",
                              .type = eir::Type::bits(3, false, true),
                              // NOLINTBEGIN(bugprone-unchecked-optional-access)
                              .initialValue = eir::BitVector::parse("3'd2"),
                              // NOLINTEND(bugprone-unchecked-optional-access)
                              .enumIndex = stateEnum});
    top.processes.push_back(clock.finish());
    top.processes.push_back(counter.finish());

    eir::Instance instance;
    instance.name = "u_dut";
    instance.definitionName = "child";
    top.instances.push_back(std::move(instance));

    eir::Design design;
    design.addModule(std::move(child));
    design.addModule(std::move(top));
    return design;
}

/// guiTestDesign with arrays beside its signals in `/tb`, as the SystemVerilog frontend describes
/// them: `logic [1:0][3:0] lanes` holding 8'h5a, `logic [3:0] memory [0:1]` holding 8'hc3, and
/// `logic [0:3] up` holding 4'b1000.
inline eir::Design arrayTestDesign() {
    const eir::Design plain = guiTestDesign();
    eir::Design design;
    for (eir::ModuleDefinition module : plain.modules()) {
        if (module.name == "tb") {
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            module.addObject(eir::Object{
                .name = "lanes",
                .type = eir::Type::bits(8, false, true),
                .initialValue = eir::BitVector::parse("8'h5a"),
                .packedDimensions = {{.left = 1, .right = 0}, {.left = 3, .right = 0}}});
            module.addObject(eir::Object{.name = "memory",
                                         .type = eir::Type::bits(8, false, true),
                                         .initialValue = eir::BitVector::parse("8'hc3"),
                                         .unpackedDimensions = {{.left = 0, .right = 1}},
                                         .packedDimensions = {{.left = 3, .right = 0}},
                                         .elementWidth = 4});
            module.addObject(eir::Object{.name = "up",
                                         .type = eir::Type::bits(4, false, true),
                                         .initialValue = eir::BitVector::parse("4'b1000"),
                                         .packedDimensions = {{.left = 0, .right = 3}}});
            // NOLINTEND(bugprone-unchecked-optional-access)
        }
        design.addModule(std::move(module));
    }
    return design;
}

/// Collects diagnostics so that a failure in the fixture says what went wrong.
class FixtureConsumer final : public DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { messages.push_back(diagnostic.message); }

    std::vector<std::string> messages;
};

/// A served kernel with a GUI session attached to it.
class SessionFixture : public ::testing::Test {
protected:
    SessionFixture() : engine(sources) { engine.addConsumer(consumer); }

    void SetUp() override {
        // Through a `session::Session`, which is what a served kernel owns: one simulation for
        // every surface that drives it.
        kernelSession = std::make_unique<session::Session>(sources, engine);
        kernelSession->setTimeScale(runtime::TimeScale(-9));
        kernelSession->setDesign(std::make_unique<eir::Design>(designToServe()));
        ASSERT_TRUE(kernelSession->elaborate("tb"))
            << (consumer.messages.empty() ? "elaboration failed" : consumer.messages.front());

        ipc::ServerOptions options;
        options.kernelVersion = "enzan test";
        options.timeScale = runtime::TimeScale(-9);
        options.localTranscript = &transcript;
        server = std::make_unique<ipc::KernelServer>(*kernelSession, engine, options);
        server->start();

        ASSERT_TRUE(directory.isValid());
        socketPath = directory.filePath(QStringLiteral("k.sock"));

        std::string error;
        ASSERT_TRUE(server->listen(socketPath.toStdString(), error)) << error;

        session = std::make_unique<gui::KernelSession>();

        QString attachError;
        ASSERT_TRUE(session->attach(socketPath, attachError)) << attachError.toStdString();

        // The handshake and the hierarchy are a round trip each, and every test needs both.
        pumpUntil([this] { return !session->instances().isEmpty(); });
        ASSERT_FALSE(session->instances().isEmpty()) << "the design never arrived";
    }

    /// The design the kernel serves. A suite that needs another overrides this.
    [[nodiscard]] virtual eir::Design designToServe() const { return guiTestDesign(); }

    void TearDown() override {
        session.reset();

        // The server borrows the kernel from its session, so it goes first.
        server.reset();
        kernelSession.reset();
    }

    /// Gives both ends a turn: the server steps, then Qt delivers whatever arrived.
    void pump(int rounds = 20) {
        for (int round = 0; round < rounds; ++round) {
            static_cast<void>(server->step(1));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }

    /// Pumps until `done` is true or the time runs out. Returns what `done` last said.
    template<typename Predicate>
    bool pumpUntil(Predicate done, int milliseconds = 5000) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < milliseconds) pump(4);
        return done();
    }

    /// The kernel's signal index for a hierarchical path.
    [[nodiscard]] std::uint32_t signalIndex(std::string_view path) const {
        return kernelSession->designImage()->findSignal(path);
    }

    /// Asks for a scope's objects and waits for the answer to *that* scope.
    ///
    /// The path is compared rather than the first reply taken, because a scope's objects arrive
    /// by the same message whoever asked for them: with a `MainWindow` in the test, which asks
    /// about the top level as soon as it has the hierarchy, a reply meant for the window was
    /// being handed back here as though it were this scope's.
    ///
    /// Waiting on a flag rather than on the answer being non-empty, so that a scope which really
    /// declares nothing returns instead of spinning until the timeout.
    QVector<gui::SignalInfo> objectsOf(const QString& scope) {
        QVector<gui::SignalInfo> received;
        bool answered = false;
        const auto connection =
            QObject::connect(session.get(), &gui::KernelSession::objectsReceived,
                             [&](const QString& path, const QVector<gui::SignalInfo>& objects) {
                                 if (path != scope) return;
                                 received = objects;
                                 answered = true;
                             });

        session->queryObjects(scope);
        pumpUntil([&] { return answered; });
        QObject::disconnect(connection);
        return received;
    }

    SourceManager sources;
    DiagnosticEngine engine;
    FixtureConsumer consumer;
    std::ostringstream transcript;

    /// The kernel end's session, which owns the simulation the server serves.
    std::unique_ptr<session::Session> kernelSession;
    std::unique_ptr<ipc::KernelServer> server;
    std::unique_ptr<gui::KernelSession> session;

    QTemporaryDir directory;
    QString socketPath;
};

} // namespace enzan::test

#endif // ENZAN_TESTS_UNIT_GUI_SESSIONFIXTURE_H
