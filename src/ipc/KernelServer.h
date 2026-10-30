// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/Diagnostics.h"
#include "ipc/OutputCapture.h"
#include "ipc/Protocol.h"
#include "ipc/Transport.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"
#include "session/Session.h"
#include "wavedb/WaveformDatabase.h"
#include "wavedb/WaveformRecorder.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <ostream>
#include <span>
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

    /// The widest signal the waveform database records, or 0 for any width.
    std::uint32_t waveformWidthLimit = wavedb::WaveformWriter::DefaultWidthLimit;

    wavedb::DatabaseOptions database;

    /// Bytes of entries one Changes answer carries at most, though one change always goes when
    /// there is one to send, however wide. Small enough that an answer is soon on its way and a
    /// client asking again finds the newest changes; one change of the widest signal is far
    /// below what a frame may hold, so no answer outgrows one.
    std::size_t changesBytes = std::size_t{4} << 20U;

    /// Where transcript output is echoed in addition to being sent to the client. Null means
    /// the standard output: a kernel serving a socket is still a program in a terminal, and
    /// swallowing its `$display` output because nothing had attached would be surprising.
    std::ostream* localTranscript = nullptr;
};

/// Serves one elaborated design to one attached client.
///
/// This is the kernel end of the split between the kernel and the front end. The simulation belongs
/// to a `session::Session` and the server adds the live waveform database, the recorder filling it
/// and the answers to a client's requests.
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
/// **The client pulls what it shows.** The server sends no values of its own accord: while a
/// run goes it says how far it has got (Progress), and the client asks for the changes it lacks
/// (FetchChanges), from the database the recorder fills, one bounded answer at a time. So what a
/// client has not taken yet is in the database, whose values spill to disk past its budget,
/// rather than in a queue on the socket that grows for as long as the client lags.
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
    KernelServer(session::Session& sessionToServe, DiagnosticEngine& diagnosticEngine,
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

    /// Makes what this process writes to its standard output part of the transcript, as well
    /// as of the terminal. Returns false having filled `error`.
    ///
    /// What a VPI application prints goes to the process's standard output and never through
    /// Enzan, so without this a front end shows none of it. Called once any child process that
    /// should not be captured — the front end `--gui` starts — has been started, because a child
    /// inherits the captured descriptors.
    bool captureOutput(std::string& error);

    /// The status `$finish` or `$fatal` asked for, or 2 when a run failed.
    [[nodiscard]] int exitStatus() const noexcept { return processExitStatus; }

    /// The bytes one change of a signal `width` bits wide takes in a Changes answer.
    [[nodiscard]] static std::size_t changeBytes(std::uint32_t width) noexcept;

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

        /// Stops keeping a copy of what is written, which still reaches the echo stream and is
        /// flushed there as it is written.
        ///
        /// For a capture: the echo stream is then the captured standard output, and keeping a
        /// copy as well would put every line in the transcript twice.
        void stopCollecting() noexcept { collecting = false; }

    protected:
        int_type overflow(int_type character) override;
        std::streamsize xsputn(const char* text, std::streamsize count) override;

    private:
        std::ostream* echoTo;
        std::string pending;
        bool collecting = true;
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
    void onFetchChanges(const Frame& frame);
    void onRun(const Frame& frame);
    void onRestart();
    void onDeposit(const Frame& frame);
    void onForce(const Frame& frame);

    /// Takes back every registration made against the session's kernel, then drops the
    /// database and the recorder.
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
    /// Re-elaborating replaces the whole simulation. Every
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

    /// Says how far the kernel has got, when that has moved since it was last said, or always
    /// when `recorded` says a change was recorded without time moving.
    void sendProgress(bool recorded);
    void sendFailure(std::string reason);
    void closeClient(std::string reason);

    [[nodiscard]] runtime::SimTime latestRecordedTime() const noexcept;

    session::Session* session;
    const runtime::Image* image;
    DiagnosticEngine* diagnostics;
    ServerOptions options;

    TranscriptRelay transcriptRelay;
    std::ostream transcriptStream;
    DiagnosticRelay diagnosticRelay;

    /// The process's standard output, once `captureOutput` has been asked for.
    OutputCapture capture;

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

    /// The signals the client shows, which the recorder records whatever its scope. Kept across a
    /// restart, which records them again from time zero, and forgotten when the client leaves.
    std::vector<bool> shown;

    /// What the last Progress said, so that one is sent only when it would say something new.
    ProgressMessage lastProgress;

    /// The signals the recorder last left out for their width, as they were reported.
    std::vector<std::uint32_t> reportedTooWide;

    Listener listener;
    Connection connection;

    /// False until the client's Hello has been accepted. Nothing else is answered before it.
    bool greeted = false;

    SimulationState simulationState = SimulationState::Idle;
    int processExitStatus = 0;

    /// Set by a Run request and cleared by `performRun`.
    bool runRequested = false;
    RunMessage requestedRun;

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
