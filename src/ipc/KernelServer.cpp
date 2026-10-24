// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/KernelServer.h"

#include "common/SourceManager.h"
#include "eir/Module.h"
#include "elab/ElaboratedDesign.h"

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

#include <poll.h>

namespace enzan::ipc {

namespace {

/// Returns the scope part of a hierarchical name: everything before the last separator.
std::string_view scopeOf(std::string_view path) {
    const std::size_t separator = path.rfind('/');
    return separator == std::string_view::npos ? std::string_view() : path.substr(0, separator);
}

/// Returns the leaf part of a hierarchical name.
std::string_view leafOf(std::string_view path) {
    const std::size_t separator = path.rfind('/');
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

/// Maps a finished run onto what the client is told the kernel is now doing.
SimulationState stateAfter(runtime::RunStatus status) {
    switch (status) {
    // Both mean the kernel stopped where it was asked to and can be told to carry on.
    case runtime::RunStatus::TimeLimitReached:
    case runtime::RunStatus::Interrupted: return SimulationState::Idle;

    // `$finish` and running out of events are both ends of the simulation; only a restart
    // brings the design back.
    case runtime::RunStatus::Quiescent:
    case runtime::RunStatus::Finished: return SimulationState::Finished;

    case runtime::RunStatus::Error: return SimulationState::Failed;
    }
    return SimulationState::Failed;
}

} // namespace

// ---------------------------------------------------------------------------
// Relays
// ---------------------------------------------------------------------------

std::string KernelServer::TranscriptRelay::take() {
    echoTo->flush();
    return std::exchange(pending, {});
}

std::streambuf::int_type KernelServer::TranscriptRelay::overflow(int_type character) {
    if (traits_type::eq_int_type(character, traits_type::eof())) {
        return traits_type::not_eof(character);
    }

    const char text = traits_type::to_char_type(character);
    if (collecting) pending.push_back(text);
    echoTo->put(text);
    if (!collecting) echoTo->flush();
    return character;
}

std::streamsize KernelServer::TranscriptRelay::xsputn(const char* text, std::streamsize count) {
    if (collecting) pending.append(text, static_cast<std::size_t>(count));
    echoTo->write(text, count);

    // Not collecting means the echo is the captured standard output and is the transcript's
    // only route. It is flushed as it is written, because what an application writes goes to
    // the same descriptor unbuffered by this stream, and a line held back here would arrive
    // after lines written later.
    if (!collecting) echoTo->flush();
    return count;
}

void KernelServer::DiagnosticRelay::consume(const Diagnostic& diagnostic) {
    DiagnosticMessage message;
    message.code = static_cast<std::uint16_t>(diagnostic.code);
    message.severity = static_cast<std::uint8_t>(diagnostic.severity);
    message.message = diagnostic.message;

    // The parts travel separately rather than as rendered text, so that the GUI can show a
    // diagnostic the way its own windows want to — clicking through to the source line, for
    // instance — which is the point of section 10.6's single engine.
    if (diagnostic.location.valid()) {
        message.file = std::string(sources->bufferName(diagnostic.location.buffer()));
        message.line = sources->lineNumber(diagnostic.location);
        message.column = sources->columnNumber(diagnostic.location);
    }

    for (const DiagnosticNote& note : diagnostic.notes) message.notes.push_back(note.message);

    pending.push_back(std::move(message));
}

std::vector<DiagnosticMessage> KernelServer::DiagnosticRelay::take() {
    return std::exchange(pending, {});
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

KernelServer::KernelServer(session::Session& sessionToServe, DiagnosticEngine& diagnosticEngine,
                           ServerOptions serverOptions)
    : session(&sessionToServe),
      image(sessionToServe.designImage()),
      diagnostics(&diagnosticEngine),
      options(std::move(serverOptions)),
      transcriptRelay(options.localTranscript != nullptr ? *options.localTranscript : std::cout),
      transcriptStream(&transcriptRelay),
      diagnosticRelay(diagnosticEngine.sourceManager()) {
    diagnostics->addConsumer(diagnosticRelay);
}

KernelServer::~KernelServer() {
    capture.stop();
    diagnostics->removeConsumer(diagnosticRelay);
    detachFromKernel();

    // The relay is about to go, so the session must stop writing into it.
    session->setTranscript(nullptr);
}

void KernelServer::detachFromKernel() {
    // The kernel belongs to the session and outlives this server, so every registration made
    // against it has to be taken back. A listener destroyed while still registered is a
    // dangling pointer the *next* run calls through — which is a crash in the middle of a
    // simulation with nothing in the transcript to explain it.
    if (simulationKernel != nullptr) {
        if (recorder != nullptr) simulationKernel->removeListener(*recorder);
        if (publisher != nullptr) simulationKernel->removeListener(*publisher);
        simulationKernel->setInterrupt(nullptr);
    }

    // The recorder flushes into the database as it closes, so the order is spelled out rather
    // than left to the member declarations.
    publisher.reset();
    recorder.reset();
    database.reset();
}

KernelServer::Borrow KernelServer::renewKernelBorrow() {
    // Asked of the session's own counter, never of the addresses. Freeing one elaboration and
    // building the next allocates the same sizes in the same order, so the new kernel commonly
    // lands exactly where the old one was: on this machine the image moved and the kernel did
    // not, on CI neither moved, and a comparison of pointers therefore answered "nothing has
    // changed" about a kernel that had been destroyed and replaced.
    if (session->elaborationGeneration() == borrowedGeneration) return Borrow::Unchanged;

    // The kernel that was being served has been destroyed, and every registration made against
    // it went with it. The pointers are dropped *before* anything else, because `detachFromKernel`
    // would otherwise take those registrations back through them; and the publisher goes with
    // them rather than being carried over, because `start` restores what it had subscribed to
    // and a different elaboration numbers its signals differently.
    simulationKernel = nullptr;
    image = nullptr;
    publisher.reset();
    recorder.reset();
    database.reset();

    if (session->simulationKernel() == nullptr || session->designImage() == nullptr) {
        borrowedGeneration = session->elaborationGeneration();
        simulationState = SimulationState::Failed;
        closeClient("a command discarded the design, so there is nothing left to serve");

        // And the process stops rather than waiting for another client. Every answer the server
        // gives reads the kernel, so a server that stayed listening with none would greet the
        // next client and then fail on its first request; there is nothing here to serve and
        // saying so by exiting is the honest end.
        exitRequested = true;
        return Borrow::Gone;
    }

    // Everything a client holds describes the design that has gone — the instance tree, the
    // signal indices it subscribed to, the history in its database — so it is sent the new tree
    // and starts again from it, exactly as a newly attached client does.
    start();
    onQueryDesign();
    return Borrow::Renewed;
}

void KernelServer::start() {
    // A restart keeps what the user asked to watch. Losing the wave window's contents because
    // the design was reset would make the button useless for the thing it is used for, which
    // is watching the same signals from the beginning again.
    std::vector<std::uint32_t> keepSubscribed;
    if (publisher != nullptr) keepSubscribed = publisher->subscribedSignals();

    detachFromKernel();

    // The session's kernel, put back to time zero. `initialize` resets everything a run
    // accumulates — the arena, the queues, the time wheel, each process's resume point — so a
    // kernel of the server's own would buy nothing and would mean two simulations for the two
    // surfaces driving them.
    image = session->designImage();
    simulationKernel = session->simulationKernel();
    borrowedGeneration = session->elaborationGeneration();
    ENZAN_ASSERT(simulationKernel != nullptr && image != nullptr,
                 "a server needs an elaborated session");

    simulationKernel->setInterrupt(this);

    // While this server is serving, the session's output belongs to the client. The session may
    // have been built before the server existed — it has to be, since the server needs it
    // elaborated — so this cannot be an option handed to its constructor.
    session->setTranscript(&transcriptStream);
    simulationKernel->initialize();

    // Recorder and publisher are both started after `initialize`, so that the values they see
    // at time zero are the ones the design begins from rather than an empty arena.
    database = std::make_unique<wavedb::WaveformDatabase>(options.database);
    recorder = std::make_unique<wavedb::WaveformRecorder>(*image, simulationKernel->arena(),
                                                          options.timeScale, *database);
    recorder->setScope(options.waveformScope);
    recorder->setWidthLimit(options.waveformWidthLimit);
    recorder->start();
    simulationKernel->addListener(*recorder);

    // Said once for what it leaves out, not again at every restart, which leaves out the same.
    const std::span<const std::uint32_t> tooWide = recorder->tooWide();
    if (!tooWide.empty() && !std::ranges::equal(tooWide, reportedTooWide)) {
        diagnostics->report(DiagCode::SignalsTooWideToRecord, SourceLocation(), "the wave window",
                            recorder->maximumWidth(), recorder->describeTooWide());
    }
    reportedTooWide.assign(tooWide.begin(), tooWide.end());

    publisher =
        std::make_unique<ChangePublisher>(*image, simulationKernel->arena(), options.timeScale);
    publisher->start();
    publisher->subscribe(keepSubscribed);
    simulationKernel->addListener(*publisher);

    simulationState = SimulationState::Idle;
    processExitStatus = 0;
    runRequested = false;
    stopRequested = false;
}

// ---------------------------------------------------------------------------
// Attaching
// ---------------------------------------------------------------------------

bool KernelServer::listen(const std::filesystem::path& socketPath, std::string& error) {
    return listener.listen(socketPath, error);
}

bool KernelServer::captureOutput(std::string& error) {
    if (!capture.start(error)) return false;

    // When the relay echoes to the standard output, that is captured from now on, and the
    // capture carries the kernel's transcript to the client in the order everything reached the
    // terminal. An echo going anywhere else is not captured, and the relay keeps its own copy.
    if (options.localTranscript == nullptr) transcriptRelay.stopCollecting();
    return true;
}

void KernelServer::attach(Connection client) {
    connection = std::move(client);
    greeted = false;

    // A new client starts from nothing: it has not asked for any signal yet, and it must not
    // be handed the transcript or the changes the previous one had not collected.
    if (publisher != nullptr) {
        publisher->clearSubscriptions();
        static_cast<void>(publisher->takeBatches());
    }
    static_cast<void>(transcriptRelay.take());
    static_cast<void>(capture.take());
    static_cast<void>(diagnosticRelay.take());
}

void KernelServer::closeClient(std::string reason) {
    if (connection.isOpen()) {
        ByeMessage bye;
        bye.reason = std::move(reason);
        connection.send(MessageType::Bye, bye);

        // Blocking here is acceptable and nowhere else: the farewell is the last thing this
        // connection will ever carry, and a client that never learns why it was dropped has
        // nothing to show the user but a dead window.
        static_cast<void>(connection.flushBlocking());
    }

    connection.close();
    greeted = false;
    if (publisher != nullptr) publisher->clearSubscriptions();

    // Nothing else happens here, and that is the point. A kernel with a socket to listen on
    // carries on with its simulation and waits for the next client, which is what `detach`
    // means in section 2.2; one that was handed its client directly has nowhere for another to
    // come from, and `step` stops for that reason rather than because leaving is fatal.
}

// ---------------------------------------------------------------------------
// The serving loop
// ---------------------------------------------------------------------------

bool KernelServer::step(int timeoutMilliseconds) {
    if (exitRequested) return false;
    if (!listener.isListening() && !connection.isOpen()) return false;

    // Waiting is skipped when there is already work queued, so that a run asked for in the
    // same round starts immediately rather than after the timeout.
    if (!runRequested) {
        std::array<pollfd, 3> waiting{};
        nfds_t count = 0;

        if (listener.isListening()) {
            waiting[count++] = pollfd{.fd = listener.descriptor(), .events = POLLIN, .revents = 0};
        }
        if (connection.isOpen()) {
            short events = POLLIN;
            if (connection.hasPendingOutput()) events |= POLLOUT;
            waiting[count++] =
                pollfd{.fd = connection.descriptor(), .events = events, .revents = 0};
        }
        // Captured output that arrives while nothing else is happening — the last lines of a
        // run, still on their way through the capture when the run returned — is forwarded
        // when it arrives rather than when something else next wakes the server.
        if (capture.active()) {
            waiting[count++] =
                pollfd{.fd = capture.wakeDescriptor(), .events = POLLIN, .revents = 0};
        }
        if (count > 0) static_cast<void>(::poll(waiting.data(), count, timeoutMilliseconds));
    }

    if (listener.isListening()) {
        Connection accepted;
        std::string error;
        if (listener.accept(accepted, error)) {
            if (connection.isOpen()) {
                // Refused with a reason rather than queued. Two clients would need the kernel
                // to serve two logging scopes and two subscription sets over one run, and
                // nothing asks for that yet.
                ByeMessage bye;
                bye.reason = "another client is already attached to this kernel";
                accepted.send(MessageType::Bye, bye);
                static_cast<void>(accepted.flushBlocking());
                accepted.close();
            } else {
                attach(std::move(accepted));
            }
        }
    }

    readRequests();
    if (runRequested && !exitRequested) performRun();
    pump();

    return !exitRequested;
}

int KernelServer::serve() {
    // No timeout: with nothing to do there is nothing to wake up for, and a kernel spinning on
    // an idle socket would burn a core on a compute server for no reason at all.
    while (step(-1)) {}
    return processExitStatus;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

void KernelServer::readRequests() {
    if (!connection.isOpen()) return;

    if (!connection.receive()) {
        closeClient(connection.error());
        return;
    }

    while (connection.isOpen()) {
        Frame frame;
        const ReceiveStatus status = connection.next(frame);
        if (status == ReceiveStatus::Empty) return;
        if (status == ReceiveStatus::Malformed) {
            closeClient("the client sent a frame this kernel cannot parse");
            return;
        }
        handle(frame);
    }
}

void KernelServer::handle(const Frame& frame) {
    const auto type = static_cast<MessageType>(frame.type);

    // Nothing is answered before the handshake. A peer that starts talking without one is
    // either a different protocol or a different version, and guessing which would mean
    // acting on a message that may not mean what it appears to.
    if (!greeted && type != MessageType::Hello) {
        closeClient(std::format("'{}' arrived before the handshake", toString(type)));
        return;
    }

    switch (type) {
    case MessageType::Hello: onHello(frame); return;
    case MessageType::QueryDesign: onQueryDesign(); return;
    case MessageType::QueryObjects: onQueryObjects(frame); return;
    case MessageType::Subscribe: onSubscribe(frame); return;
    case MessageType::Unsubscribe: onUnsubscribe(frame); return;
    case MessageType::QueryHistory: onQueryHistory(frame); return;
    case MessageType::Run: onRun(frame); return;

    case MessageType::Stop:
        // Latched rather than acted on, so that a stop which arrives between a run being asked
        // for and the run beginning is not lost. The latch is cleared when the *next* run is
        // requested, not when one starts, which is the difference between "stop what is
        // happening" and "stop whatever happens next".
        stopRequested = true;
        if (!running) sendRunState("stopped");
        return;

    case MessageType::Restart: onRestart(); return;

    case MessageType::Quit:
        stopRequested = true;
        exitRequested = true;
        return;

    case MessageType::Detach: closeClient("the client detached"); return;

    case MessageType::Deposit: onDeposit(frame); return;
    case MessageType::Force: onForce(frame); return;

    default:
        // An event arriving from the client, or a request from a newer build. Saying so beats
        // ignoring it: the client is waiting for an answer that would never come.
        sendFailure(std::format("'{}' is not a request this kernel answers", toString(type)));
        return;
    }
}

void KernelServer::onHello(const Frame& frame) {
    HelloMessage hello;
    if (!decodePayload(frame.payload, hello)) {
        closeClient("the handshake could not be decoded");
        return;
    }

    if (hello.protocolVersion != ProtocolVersion) {
        sendFailure(std::format(
            "this kernel speaks protocol version {} and the client speaks {}; both ends must "
            "come from the same build",
            ProtocolVersion, hello.protocolVersion));
        closeClient("protocol version mismatch");
        return;
    }

    greeted = true;

    WelcomeMessage welcome;
    welcome.protocolVersion = ProtocolVersion;
    welcome.kernelVersion = options.kernelVersion;
    welcome.topModule = image->elaboratedDesign().topModuleName();
    welcome.timeExponent = options.timeScale.precisionExponent();
    welcome.currentTime = simulationKernel->time();
    welcome.latestRecordedTime = latestRecordedTime();
    welcome.state = simulationState;
    welcome.widthLimit = options.waveformWidthLimit;
    connection.send(MessageType::Welcome, welcome);

    transcriptStream << "-- " << (hello.clientName.empty() ? "a client" : hello.clientName)
                     << " attached\n";
}

void KernelServer::onQueryDesign() {
    DesignMessage message;
    const std::span<const elab::Instance> instances = image->elaboratedDesign().instances();
    message.instances.reserve(instances.size());

    const SourceManager& sources = diagnostics->sourceManager();

    // The source list is collected from where the definitions actually are, not from every
    // buffer the manager holds: a kernel driven from a script has the script in there too, and
    // offering it to a source window as a design file would be a lie about what was analysed.
    std::vector<std::string> files;

    for (const elab::Instance& instance : instances) {
        const eir::ModuleDefinition& definition =
            image->analysedDesign().module(instance.definitionIndex);

        std::string file;
        std::uint32_t line = 0;
        if (definition.location.valid()) {
            file = sources.bufferName(definition.location.buffer());
            line = sources.lineNumber(definition.location);
            if (std::ranges::find(files, file) == files.end()) files.push_back(file);
        }

        message.instances.push_back(InstanceEntry{.path = instance.path,
                                                  .name = instance.name,
                                                  .parent = instance.parent,
                                                  .file = std::move(file),
                                                  .line = line});
    }

    message.sourceFiles = std::move(files);
    connection.send(MessageType::Design, message);
}

void KernelServer::onDeposit(const Frame& frame) {
    DepositMessage request;
    if (!decodePayload(frame.payload, request)) {
        sendFailure("a deposit could not be decoded");
        return;
    }

    if (running) {
        sendFailure("a run is in progress; stop it before writing a signal");
        return;
    }
    if (request.signalIndex >= image->signals().size()) {
        sendFailure(std::format("{} is not a signal in this design", request.signalIndex));
        return;
    }

    // Through the session, which takes the value as the signal shows it: a std_logic's '1' is
    // what `1` deposited into one means.
    std::string error;
    if (!session->deposit(request.signalIndex, request.value.ref(), error)) {
        sendFailure(error);
        return;
    }

    // The publisher is told so that the window showing the signal updates, because a deposit
    // between runs produces no time advance for the change to be batched into.
    publisher->flush();
}

void KernelServer::onForce(const Frame& frame) {
    ForceMessage request;
    if (!decodePayload(frame.payload, request)) {
        sendFailure("a force could not be decoded");
        return;
    }

    if (running) {
        sendFailure("a run is in progress; stop it before writing a signal");
        return;
    }
    if (request.signalIndex >= image->signals().size()) {
        sendFailure(std::format("{} is not a signal in this design", request.signalIndex));
        return;
    }

    if (request.release) {
        // Releasing something that was not forced is not an error: a window clearing a set of
        // signals it may or may not have forced should not have to remember which.
        simulationKernel->release(request.signalIndex);
    } else {
        std::string error;
        if (!session->force(request.signalIndex, request.value.ref(), error)) {
            sendFailure(error);
            return;
        }
    }

    publisher->flush();
}

void KernelServer::onQueryObjects(const Frame& frame) {
    QueryObjectsMessage request;
    if (!decodePayload(frame.payload, request)) {
        sendFailure("a query for a scope's objects could not be decoded");
        return;
    }

    if (!image->elaboratedDesign().findInstance(request.instancePath).has_value()) {
        sendFailure(std::format("'{}' is not a scope in this design", request.instancePath));
        return;
    }

    ObjectsMessage message;
    message.instancePath = request.instancePath;

    const elab::ElaboratedDesign& elaborated = image->elaboratedDesign();

    // Only the enumerations this scope actually uses are sent, renumbered from the kernel's
    // table into the message's own. The kernel's table covers the whole design, and a scope
    // query answered with all of it would carry the names of every state machine in the design
    // every time a reader clicked a module.
    std::vector<std::uint32_t> enumSlotFor;
    const auto enumEntryFor = [&](std::uint32_t slotEnum) {
        if (slotEnum == elab::Slot::NoEnum) return ObjectEntry::NoEnum;

        const auto found = std::ranges::find(enumSlotFor, slotEnum);
        if (found != enumSlotFor.end()) {
            return static_cast<std::uint32_t>(found - enumSlotFor.begin());
        }

        const eir::EnumDefinition& definition = elaborated.enumerations()[slotEnum];

        EnumEntry entry;
        entry.name = definition.name;
        for (const eir::EnumMember& member : definition.members) {
            entry.members.push_back(EnumMemberEntry{.name = member.name, .value = member.value});
        }

        enumSlotFor.push_back(slotEnum);
        message.enums.push_back(std::move(entry));
        return static_cast<std::uint32_t>(message.enums.size() - 1);
    };

    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    const std::uint32_t instanceIndex = *elaborated.findInstance(request.instancePath);
    // NOLINTEND(bugprone-unchecked-optional-access)
    const elab::Instance& instance = elaborated.instances()[instanceIndex];
    const eir::ModuleDefinition& definition =
        image->analysedDesign().module(instance.definitionIndex);

    // The dimensions a name is declared with, by the definition declaring it here: a port
    // collapsed onto its actual is one slot under two names, and each may have a shape of its
    // own. None for a name that is no object of this definition's own, a hierarchical
    // reference's, or whose dimensions do not divide the bits sent for it.
    const auto dimensionsOf = [&definition, &instance](std::string_view leaf,
                                                       std::uint32_t signalIndex,
                                                       std::uint32_t width) {
        const std::optional<std::uint32_t> objectIndex = definition.findObject(leaf);
        if (!objectIndex.has_value() || instance.objectSlots[*objectIndex] != signalIndex)
            return std::vector<eir::ArrayDimension>{};
        std::vector<eir::ArrayDimension> dimensions =
            eir::declaredDimensions(definition, definition.objects[*objectIndex]);
        std::uint64_t elements = 1;
        for (const eir::ArrayDimension& dimension : dimensions) elements *= dimension.size();
        if (width == 0 || width % elements != 0) dimensions.clear();
        return dimensions;
    };

    const auto recordingOf = [this](std::uint32_t signalIndex) {
        if (database != nullptr
            && database->recordIndexOf(signalIndex) != wavedb::WaveformDatabase::NotRecorded)
            return Recording::Recorded;
        if (recorder != nullptr && std::ranges::binary_search(recorder->tooWide(), signalIndex))
            return Recording::TooWide;
        return Recording::OutsideScope;
    };

    // Every name a slot has is offered, not only its canonical one. A collapsed port makes one
    // slot visible in two scopes and both names are real, so a user looking inside a DUT finds
    // its ports where they expect them.
    for (std::uint32_t signalIndex = 0; signalIndex < image->signals().size(); ++signalIndex) {
        // As the values sent for it are, shown rather than stored: a std_logic is one bit, not
        // the byte of its code.
        const std::uint32_t width = publisher->widthOf(signalIndex);

        for (const std::string& name : elaborated.slotNames(signalIndex)) {
            if (scopeOf(name) != request.instancePath) continue;
            message.objects.push_back(
                ObjectEntry{.signalIndex = signalIndex,
                            .name = std::string(leafOf(name)),
                            .path = name,
                            .width = width,
                            .recording = recordingOf(signalIndex),
                            .dimensions = dimensionsOf(leafOf(name), signalIndex, width),
                            .enumIndex = enumEntryFor(elaborated.slots()[signalIndex].enumIndex)});
        }
    }

    // The scope's parameters come from its definition rather than from any slot: they have no
    // storage, and their value is the same at every time. Which definition matters — a module
    // instantiated three times at three settings is three definitions, and each carries what
    // its own parameters were resolved to.

    for (const eir::Parameter& parameter : definition.parameters) {
        message.parameters.push_back(
            ParameterEntry{.name = parameter.name,
                           .path = request.instancePath + '/' + parameter.name,
                           .value = parameter.value,
                           .isLocal = parameter.isLocal});
    }

    connection.send(MessageType::Objects, message);
}

void KernelServer::onSubscribe(const Frame& frame) {
    SubscribeMessage request;
    if (!decodePayload(frame.payload, request)) {
        sendFailure("a subscription request could not be decoded");
        return;
    }

    publisher->subscribe(request.signalIndices);

    // Catching up is part of subscribing. A signal dragged into the wave window halfway
    // through a run must show what it has been doing, not start from a blank trace.
    for (const std::uint32_t signalIndex : request.signalIndices) {
        if (signalIndex >= image->signals().size()) continue;
        sendHistory(signalIndex, 0, runtime::NoTime);
    }
}

void KernelServer::onUnsubscribe(const Frame& frame) {
    UnsubscribeMessage request;
    if (!decodePayload(frame.payload, request)) {
        sendFailure("an unsubscribe request could not be decoded");
        return;
    }
    publisher->unsubscribe(request.signalIndices);
}

void KernelServer::onQueryHistory(const Frame& frame) {
    QueryHistoryMessage request;
    if (!decodePayload(frame.payload, request)) {
        sendFailure("a history request could not be decoded");
        return;
    }

    if (request.signalIndex >= image->signals().size()) {
        sendFailure(std::format("there is no signal {} in this design", request.signalIndex));
        return;
    }

    sendHistory(request.signalIndex, request.fromTime, request.toTime);
}

void KernelServer::onRun(const Frame& frame) {
    RunMessage request;
    if (!decodePayload(frame.payload, request)) {
        sendFailure("a run request could not be decoded");
        return;
    }

    if (running) {
        sendFailure("a run is already in progress; stop it first");
        return;
    }
    if (simulationState == SimulationState::Finished
        || simulationState == SimulationState::Failed) {
        sendFailure("the design has finished; restart it to run again");
        return;
    }

    requestedRun = request;
    runRequested = true;

    // Asking to run clears a stop from an earlier one, so that a latch nobody consumed cannot
    // kill a run the user asked for afterwards.
    stopRequested = false;
}

void KernelServer::onRestart() {
    if (running) {
        sendFailure("a run is in progress; stop it before restarting");
        return;
    }

    // The same refusal as `Session::restart`, made here because a restart from the client puts
    // the kernel back to time zero itself rather than through the session (section 9.5).
    if (session->hasVpiApplication()) {
        sendFailure("a design with a VPI application loaded cannot be restarted");
        return;
    }

    start();

    // The client learns from the time going backwards that everything it holds is stale, so
    // the state comes first and the fresh traces follow it.
    sendRunState("restarted");
    for (const std::uint32_t signalIndex : publisher->subscribedSignals()) {
        sendHistory(signalIndex, 0, runtime::NoTime);
    }
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void KernelServer::performRun() {
    runRequested = false;

    runtime::SimTime until = runtime::NoTime;
    if (!requestedRun.untilFinished) {
        const runtime::SimTime now = simulationKernel->time();
        if (requestedRun.duration > runtime::NoTime - now) {
            sendFailure("the requested run would advance simulated time past the 64-bit range");
            return;
        }
        until = now + requestedRun.duration;
    }

    // Announced before it starts, so that a client's Run button changes the moment the kernel
    // begins rather than when it next has something to say.
    simulationState = SimulationState::Running;
    sendRunState({});
    pump();

    running = true;
    lastPump = std::chrono::steady_clock::now();

    const runtime::RunStatus status = simulationKernel->run(until);

    running = false;

    simulationState = stateAfter(status);
    if (status == runtime::RunStatus::Error) {
        processExitStatus = 2;
    } else {
        processExitStatus = simulationKernel->exitStatus();
    }

    // The slot the run ended on is written out before anyone is told the run is over. A slot
    // is otherwise recorded when the *next* one begins, so without this the last thing the
    // design did would be missing from the view until it was told to carry on — which looks
    // exactly like the simulator having lost an event.
    recorder->flush();
    publisher->flush();

    // The time axis extends to where the run reached, whether or not anything changed there —
    // and to where it was *asked* to reach when it got there with time to spare. A design whose
    // next event is beyond the limit leaves simulated time where it was, which is correct and
    // would make a `run 100ns` look on screen as though nothing had happened.
    runtime::SimTime reached = simulationKernel->time();
    if (status == runtime::RunStatus::TimeLimitReached) reached = std::max(reached, until);
    database->setLatestTime(reached);

    sendRunState(std::string(runtime::toString(status)));
}

bool KernelServer::shouldInterrupt() {
    // Throttled rather than answered at every time slot. A design with many short slots would
    // otherwise spend a system call on each one, and a hundredth of a second is far below
    // anything a user can perceive in a Stop button.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPump >= PumpInterval) {
        lastPump = now;
        readRequests();
        pump();
    }

    return stopRequested || exitRequested;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void KernelServer::pump() {
    // The relay first: taking from it flushes the standard output, which is what puts the
    // kernel's own lines into the capture.
    std::string transcript = transcriptRelay.take();
    if (capture.active()) transcript += capture.take();
    std::vector<DiagnosticMessage> reported = diagnosticRelay.take();
    std::vector<ValueChangesMessage> batches;
    if (publisher != nullptr) batches = publisher->takeBatches();

    // Everything is drained whether or not anyone is listening, because a kernel running with
    // no client attached must not accumulate a session's worth of output against the chance
    // that one turns up.
    if (!connection.isOpen() || !greeted) return;

    if (!transcript.empty()) {
        TranscriptMessage message;
        message.text = std::move(transcript);
        connection.send(MessageType::Transcript, message);
    }

    for (DiagnosticMessage& message : reported) {
        connection.send(MessageType::Diagnostic, message);
    }

    for (ValueChangesMessage& message : batches) {
        connection.send(MessageType::ValueChanges, message);
    }

    if (!connection.flush()) closeClient(connection.error());
}

void KernelServer::sendRunState(std::string reason) {
    if (!connection.isOpen() || !greeted) return;

    RunStateMessage message;
    message.state = simulationState;
    message.time = simulationKernel->time();
    message.latestRecordedTime = latestRecordedTime();
    message.reason = std::move(reason);

    connection.send(MessageType::RunState, message);
}

void KernelServer::sendFailure(std::string reason) {
    if (!connection.isOpen()) return;

    FailureMessage message;
    message.reason = std::move(reason);
    connection.send(MessageType::Failure, message);
}

void KernelServer::sendHistory(std::uint32_t signalIndex, runtime::SimTime from,
                               runtime::SimTime to) {
    if (!connection.isOpen() || !greeted) return;

    // A signal of no bits, a VHDL signal of a null range, has neither a past nor a value now.
    if (publisher->widthOf(signalIndex) == 0) return;

    HistoryMessage message;
    message.signalIndex = signalIndex;
    message.width = publisher->widthOf(signalIndex);
    message.latestTime = latestRecordedTime();

    const std::uint32_t record = database != nullptr ? database->recordIndexOf(signalIndex)
                                                     : wavedb::WaveformDatabase::NotRecorded;

    if (record == wavedb::WaveformDatabase::NotRecorded) {
        // Outside the logging scope: there is no past to send, only what the signal holds
        // right now. The client already knows from the object listing that it is not being
        // recorded, so an empty trace and an unrecorded one do not look the same.
        message.entries.push_back(HistoryEntry{.time = simulationKernel->time(),
                                               .value = publisher->currentValue(signalIndex)});
        connection.send(MessageType::History, message);
        return;
    }

    std::vector<wavedb::Change> changes;
    database->changesIn(record, from, to, changes);

    if (const std::size_t limit = historyEntriesFor(message.width); changes.size() > limit) {
        // Truncated rather than split across an unbounded number of messages. `latestTime`
        // then says where the answer stops, and the client asks again from there.
        changes.erase(changes.begin() + static_cast<std::ptrdiff_t>(limit), changes.end());
        message.complete = false;
        message.latestTime = changes.back().time;
    }

    message.entries.reserve(changes.size());
    for (wavedb::Change& change : changes) {
        message.entries.push_back(
            HistoryEntry{.time = change.time, .value = std::move(change.value)});
    }

    connection.send(MessageType::History, message);
}

std::size_t KernelServer::historyEntriesFor(std::uint32_t width) noexcept {
    // A time, and a value as the codec writes one: its width, then both planes of its words.
    const std::size_t entryBytes = sizeof(std::uint64_t) + sizeof(std::uint32_t)
                                   + (eir::storageWordsFor(width) * sizeof(eir::Word));
    return std::clamp<std::size_t>(MaxHistoryBytes / entryBytes, 1, MaxHistoryEntries);
}

runtime::SimTime KernelServer::latestRecordedTime() const noexcept {
    const runtime::SimTime recorded = database != nullptr ? database->latestTime() : 0;
    const runtime::SimTime now = simulationKernel != nullptr ? simulationKernel->time() : 0;
    return std::max(recorded, now);
}

} // namespace enzan::ipc
