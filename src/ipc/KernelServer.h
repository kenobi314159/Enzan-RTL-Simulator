// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/Diagnostics.h"
#include "control/Session.h"
#include "ipc/ChangePublisher.h"
#include "ipc/Protocol.h"
#include "ipc/Transport.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"
#include "wavedb/WaveformDatabase.h"
#include "wavedb/WaveformRecorder.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

namespace enzan::ipc {

/// How the served kernel is configured.
struct ServerOptions {
    /// The version line sent in the handshake, so that the GUI's title bar and its bug reports
    /// name the kernel rather than the GUI.
    std::string kernelVersion;

    runtime::TimeScale timeScale;
    std::uint32_t deltaLimit = 10'000;

    /// Hierarchical prefix the waveform database records, empty for the whole design.
    std::string waveformScope;

    wavedb::DatabaseOptions database;

    /// Where transcript output is echoed in addition to being sent to the client. Null means
    /// the standard output: a kernel serving a socket is still a program in a terminal, and
    /// swallowing its `$display` output because nothing had attached would be surprising.
    std::ostream* localTranscript = nullptr;
};

/// Evaluates control-language text on behalf of an attached client.
///
/// An interface the caller implements rather than an interpreter the server owns, so that
/// `ipc_server` does not link the Tcl embedding: the same server serves a client whether or not
/// the process driving it has an interpreter, and a front end whose command entry is answered
/// with "this kernel takes no commands" is better than one built against a dependency it does
/// not need.
class CommandEvaluator {
public:
    CommandEvaluator() = default;
    CommandEvaluator(const CommandEvaluator&) = delete;
    CommandEvaluator& operator=(const CommandEvaluator&) = delete;
    CommandEvaluator(CommandEvaluator&&) = delete;
    CommandEvaluator& operator=(CommandEvaluator&&) = delete;
    virtual ~CommandEvaluator() = default;

    /// Runs `script` and returns whether it succeeded.
    ///
    /// `result` receives what the command returned, which a prompt echoes. `error` receives the
    /// message on failure. Anything the command *printed* has already gone to the transcript by
    /// the ordinary route and must not be repeated here.
    virtual bool evaluate(std::string_view script, std::string& result, std::string& error) = 0;
};

/// Serves one elaborated design to one attached client.
///
/// This is the kernel end of the split described in section 2.2. The simulation belongs to a
/// `control::Session` — the same one the Tcl vocabulary drives — and the server adds the live
/// waveform database, the recorder filling it and the answers to a client's requests.
///
/// **One session, whichever surface is driving.** A served kernel, a `-do` script and a batch
/// run are the same pipeline; a server that built a kernel of its own would mean two, and a
/// command typed at a front end would then be acting on a different simulation from the one on
/// screen.
///
/// Everything that makes the split worth having lives here: the kernel runs perfectly well with
/// nobody attached, a client may attach and detach while a simulation is in progress, and a
/// client that stops reading is left behind rather than allowed to stall the run.
///
/// **A run is interruptible, not chopped up.** `run` is called once for the whole requested
/// span and the server answers the kernel's interrupt poll from inside it, servicing the socket
/// between time slots. Slicing the run into short fixed spans instead would have been simpler
/// and wrong: it would make responsiveness depend on how much simulated time the client asked
/// for, and it would make the kernel's progress depend on the client's.
///
/// One client at a time. A second attach is refused with a message rather than queued, because
/// two clients with different logging scopes and subscriptions would need the kernel to serve
/// two views of the same run, and nothing asks for that yet.
class KernelServer final : public runtime::RunInterrupt {
public:
    /// `sessionToServe` and `diagnostics` must outlive the server, and the session must
    /// already have been elaborated.
    KernelServer(control::Session& sessionToServe, DiagnosticEngine& diagnosticEngine,
                 ServerOptions serverOptions);

    KernelServer(const KernelServer&) = delete;
    KernelServer& operator=(const KernelServer&) = delete;
    KernelServer(KernelServer&&) = delete;
    KernelServer& operator=(KernelServer&&) = delete;

    ~KernelServer() override;

    /// Puts the design at time zero and begins recording.
    ///
    /// Separate from the constructor because it is also what `restart` does, and because the
    /// order — initialise, then start recording — is what makes the values captured at time
    /// zero the ones the design starts from rather than an empty arena.
    void start();

    /// Registers who answers a client's control-language lines, or clears it with null.
    ///
    /// Not owned. A server with none answers a command with a failure naming the reason, which
    /// is what a kernel with no interpreter built in should say.
    void setCommandEvaluator(CommandEvaluator* evaluator) noexcept { commands = evaluator; }

    /// Binds a socket for clients to attach to. Returns false having filled `error`.
    bool listen(const std::filesystem::path& socketPath, std::string& error);

    [[nodiscard]] const std::filesystem::path& socketPath() const noexcept {
        return listener.path();
    }

    /// Takes over an already-connected client and performs no handshake of its own.
    ///
    /// For a GUI that started the kernel itself over a socket pair, and for tests. The client
    /// still sends Hello; this only says where the bytes come from.
    void attach(Connection client);

    [[nodiscard]] bool hasClient() const noexcept { return connection.isOpen(); }

    /// Performs one round: accept, read and answer requests, perform a requested run, and
    /// write whatever is owed to the client.
    ///
    /// Blocks for at most `timeoutMilliseconds` waiting for something to happen, except while
    /// a run is in progress, which it carries out to completion or interruption. Returns false
    /// once the server has been asked to exit.
    bool step(int timeoutMilliseconds);

    /// Repeats `step` until it says to stop. Returns the process exit status.
    int serve();

    [[nodiscard]] SimulationState state() const noexcept { return simulationState; }

    [[nodiscard]] const wavedb::WaveformDatabase& waveforms() const noexcept { return *database; }

    [[nodiscard]] runtime::Kernel& kernel() noexcept { return *simulationKernel; }

    /// Where output has to be written to reach the attached client.
    ///
    /// Anything else driving the same session — the interpreter answering a typed command —
    /// points here too, so that what it prints arrives in the client's transcript rather than
    /// on a terminal nobody is looking at.
    [[nodiscard]] std::ostream& transcript() noexcept { return transcriptStream; }

    /// The status `$finish` or `$fatal` asked for, or 2 when a run failed.
    [[nodiscard]] int exitStatus() const noexcept { return processExitStatus; }

    /// Entries a single History message will carry before it is marked incomplete.
    static constexpr std::size_t MaxHistoryEntries = 200'000;

    // Public, matching `RunInterrupt`. Narrowing an override's visibility is legal and
    // confusing: the kernel calls it through the base class either way.
    bool shouldInterrupt() override;

private:
    // --- plumbing ----------------------------------------------------------

    /// Forwards `$display` output to the client while echoing it locally.
    ///
    /// A stream buffer rather than a listener because that is the shape the kernel already
    /// offers: `KernelOptions::transcript` is an `std::ostream*`, and the kernel has no idea
    /// anything is attached.
    class TranscriptRelay final : public std::streambuf {
    public:
        explicit TranscriptRelay(std::ostream& echo) noexcept : echoTo(&echo) {}

        /// Removes and returns everything written since the last call.
        std::string take();

    protected:
        int_type overflow(int_type character) override;
        std::streamsize xsputn(const char* text, std::streamsize count) override;

    private:
        std::ostream* echoTo;
        std::string pending;
    };

    /// Collects diagnostics for forwarding, flattened for the wire.
    class DiagnosticRelay final : public DiagnosticConsumer {
    public:
        explicit DiagnosticRelay(const SourceManager& sourceManager) noexcept
            : sources(&sourceManager) {}

        void consume(const Diagnostic& diagnostic) override;

        std::vector<DiagnosticMessage> take();

    private:
        const SourceManager* sources;
        std::vector<DiagnosticMessage> pending;
    };

    // --- request handling --------------------------------------------------

    void readRequests();
    void handle(const Frame& frame);

    void onHello(const Frame& frame);
    void onQueryDesign();
    void onQueryObjects(const Frame& frame);
    void onSubscribe(const Frame& frame);
    void onUnsubscribe(const Frame& frame);
    void onQueryHistory(const Frame& frame);
    void onRun(const Frame& frame);
    void onRestart();
    void onQuerySource(const Frame& frame);
    void onSetBreakpoint(const Frame& frame);
    void onClearBreakpoint(const Frame& frame);
    void onEnableBreakpoint(const Frame& frame);
    void onStep(const Frame& frame);
    void onDeposit(const Frame& frame);
    void onForce(const Frame& frame);
    void onCommand(const Frame& frame);

    /// Sends the whole breakpoint list, which is what every change to it is answered with.
    void sendBreakpoints();

    /// Takes back every registration made against the session's kernel, then drops the
    /// database, the recorder and the publisher.
    void detachFromKernel();

    /// What `renewKernelBorrow` found.
    enum class Borrow : std::uint8_t {
        /// The session still holds the kernel this server was serving.
        Unchanged,
        /// It holds a different one, which this server is now serving from time zero.
        Renewed,
        /// It holds none, and the client has been told and dropped.
        Gone,
    };

    /// Re-borrows the session's kernel when something has replaced the one being served.
    ///
    /// The server borrows a kernel the session owns, and a command is free to destroy it:
    /// `vsim` replaces the whole elaboration and so does anything a script calls it from. Every
    /// path that reads the kernel after an evaluation has to come through here first, because
    /// the alternative is reading a freed object.
    ///
    /// The question is asked of the session's elaboration generation, never of the pointers:
    /// the replacement routinely lands at the address the old one just released, which makes a
    /// pointer comparison answer "unchanged" about a kernel that has gone, on some machines and
    /// not others.
    Borrow renewKernelBorrow();

    /// Sends everything the client is owed and writes what the socket will take.
    void pump();

    /// Performs the run the client asked for, which is where most of a session's time goes.
    void performRun();

    void sendRunState(std::string reason);
    void sendFailure(std::string reason);
    void closeClient(std::string reason);

    /// Sends a signal's recorded past, or its current value when it is not being recorded.
    void sendHistory(std::uint32_t signalIndex, runtime::SimTime from, runtime::SimTime to);

    [[nodiscard]] runtime::SimTime latestRecordedTime() const noexcept;

    control::Session* session;
    const runtime::Image* image;
    DiagnosticEngine* diagnostics;
    ServerOptions options;
    CommandEvaluator* commands = nullptr;

    TranscriptRelay transcriptRelay;
    std::ostream transcriptStream;
    DiagnosticRelay diagnosticRelay;

    /// The session's kernel, not the server's. Borrowed for as long as the session holds it,
    /// which is what makes a command typed at a front end act on the simulation on screen.
    runtime::Kernel* simulationKernel = nullptr;

    /// The session's elaboration generation when this borrow was taken.
    ///
    /// The only sound way to ask whether the borrow still holds; `Session::elaborationGeneration`
    /// says why comparing `simulationKernel` against the session's own pointer is not.
    std::uint64_t borrowedGeneration = 0;

    // Recreated by `start`: a restarted design begins at time zero with nothing recorded, and
    // the database is where "nothing recorded" lives.
    std::unique_ptr<wavedb::WaveformDatabase> database;
    std::unique_ptr<wavedb::WaveformRecorder> recorder;
    std::unique_ptr<ChangePublisher> publisher;

    Listener listener;
    Connection connection;

    /// False until the client's Hello has been accepted. Nothing else is answered before it.
    bool greeted = false;

    SimulationState simulationState = SimulationState::Idle;
    int processExitStatus = 0;

    /// Set by a Run request and cleared by `performRun`.
    bool runRequested = false;
    RunMessage requestedRun;

    /// Source lines still to step, set by a Step request.
    ///
    /// A step goes through the same `performRun` as a Run request rather than reaching the
    /// kernel by a path of its own, so that there is one place that decides about interrupts,
    /// transcripts and run state. What makes it a step is this count and the kernel being
    /// asked for one before each run.
    std::uint32_t stepsRequested = 0;

    bool running = false;
    bool stopRequested = false;
    bool exitRequested = false;

    /// When the socket was last serviced from inside a run.
    ///
    /// The poll is throttled rather than performed at every time slot: a design with many short
    /// slots would otherwise spend a system call on each one, and a tenth of a frame is far
    /// below what anyone can see.
    std::chrono::steady_clock::time_point lastPump;

    static constexpr std::chrono::milliseconds PumpInterval{10};
};

} // namespace enzan::ipc
