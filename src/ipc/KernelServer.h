// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/Diagnostics.h"
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

/// Serves one elaborated design to one attached client.
///
/// This is the kernel end of the split described in section 2.2. It owns the simulation — the
/// `runtime::Kernel`, the live waveform database and the recorder filling it — and answers a
/// client's requests about it. Everything that makes the split worth having lives here: the
/// kernel runs perfectly well with nobody attached, a client may attach and detach while a
/// simulation is in progress, and a client that stops reading is left behind rather than
/// allowed to stall the run.
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
    /// `imageToServe` and `diagnostics` must outlive the server.
    KernelServer(const runtime::Image& imageToServe, DiagnosticEngine& diagnostics,
                 ServerOptions serverOptions);

    KernelServer(const KernelServer&) = delete;
    KernelServer& operator=(const KernelServer&) = delete;
    KernelServer(KernelServer&&) = delete;
    KernelServer& operator=(KernelServer&&) = delete;

    ~KernelServer() override;

    /// Creates the kernel, writes the time-zero state and begins recording.
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

    /// The status `$finish` or `$fatal` asked for, or 2 when a run failed.
    [[nodiscard]] int exitStatus() const noexcept { return processExitStatus; }

    /// Entries a single History message will carry before it is marked incomplete.
    static constexpr std::size_t MaxHistoryEntries = 200'000;

private:
    // --- the kernel's interrupt -------------------------------------------

    bool shouldInterrupt() override;

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

    const runtime::Image* image;
    DiagnosticEngine* diagnostics;
    ServerOptions options;

    TranscriptRelay transcriptRelay;
    std::ostream transcriptStream;
    DiagnosticRelay diagnosticRelay;

    // Recreated by `start`, because a restart has to reset the arena, the scheduler queues and
    // every process's resume point, and a fresh kernel is the only way to be sure it has.
    std::unique_ptr<runtime::Kernel> simulationKernel;
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

    bool running = false;
    bool stopRequested = false;
    bool exitRequested = false;

    /// When the socket was last serviced from inside a run.
    ///
    /// The poll is throttled rather than performed at every time slot: a design with many short
    /// slots would otherwise spend a system call on each one, and a tenth of a frame is far
    /// below what anyone can see.
    std::chrono::steady_clock::time_point lastPump{};

    static constexpr std::chrono::milliseconds PumpInterval{10};
};

} // namespace enzan::ipc
