// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ipc/Protocol.h"
#include "ipc/Transport.h"
#include "runtime/Time.h"
#include "wavedb/WaveformDatabase.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <map>
#include <memory>

class QSocketNotifier;
class QProcess;

namespace enzan::gui {

/// What the GUI knows about one signal.
struct SignalInfo {
    std::uint32_t signalIndex = 0;
    QString name;
    QString path;
    std::uint32_t width = 1;

    /// False when the kernel is not recording this signal, so that "nothing has happened yet"
    /// and "nothing is being kept" can be told apart on screen.
    bool recorded = false;
};

/// The GUI's end of the connection to a kernel.
///
/// Wraps `ipc::Connection` in Qt's event loop through a socket notifier, so that nothing here
/// ever blocks the interface: the requirement in section 2.2 that a slow GUI must not stall the
/// kernel has a mirror image, which is that a busy kernel must not freeze the GUI. Both come
/// from the same arrangement — neither side waits for the other.
///
/// The session holds its own `wavedb::WaveformDatabase`, the same class the kernel fills, fed
/// from the protocol instead of from a state arena. That is what lets the view ask for a pixel
/// column's worth of summary without a round trip, and it is why the database was written
/// against a signal list rather than against an `Image`.
class KernelSession : public QObject {
    Q_OBJECT

public:
    explicit KernelSession(QObject* parent = nullptr);
    ~KernelSession() override;

    /// Connects to a kernel already listening on `socketPath` and sends the handshake.
    bool attach(const QString& socketPath, QString& error);

    /// Starts `program` with `arguments` plus a `--listen` on a private socket, then attaches.
    ///
    /// The everyday path: a user runs `enzan-gui design.sv` and gets a kernel without having to
    /// arrange one. A kernel started this way is asked to quit when the window closes, because
    /// it has no life of its own; one attached to with `--attach` is only detached from.
    bool launch(const QString& program, const QStringList& arguments, QString& error);

    void detach();

    [[nodiscard]] bool isAttached() const;

    /// True when this session started the kernel, and so is responsible for ending it.
    [[nodiscard]] bool ownsKernel() const noexcept { return kernelProcess != nullptr; }

    // --- requests ----------------------------------------------------------

    void queryDesign();
    void queryObjects(const QString& instancePath);
    void subscribe(const QVector<std::uint32_t>& signalIndices);
    void unsubscribe(const QVector<std::uint32_t>& signalIndices);
    void run(std::uint64_t duration);
    void runUntilFinished();
    void stop();
    void restart();

    /// Asks the kernel to exit, then waits briefly for it to do so.
    void quitKernel();

    // --- what the session knows --------------------------------------------

    [[nodiscard]] const wavedb::WaveformDatabase& waveforms() const noexcept { return *database; }

    [[nodiscard]] const QVector<ipc::InstanceEntry>& instances() const noexcept {
        return designInstances;
    }

    /// What is known about a signal, or nothing when the GUI has not seen it listed.
    [[nodiscard]] const SignalInfo* signalInfo(std::uint32_t signalIndex) const;

    [[nodiscard]] ipc::SimulationState state() const noexcept { return simulationState; }

    [[nodiscard]] runtime::SimTime currentTime() const noexcept { return kernelTime; }

    [[nodiscard]] runtime::SimTime latestTime() const noexcept { return recordedTime; }

    [[nodiscard]] runtime::TimeScale timeScale() const noexcept { return scale; }

    [[nodiscard]] const QString& topModule() const noexcept { return top; }

    [[nodiscard]] const QString& kernelVersion() const noexcept { return version; }

Q_SIGNALS:
    /// The handshake completed and the kernel described itself.
    void welcomed();

    /// The instance tree arrived; `instances()` holds it.
    void designChanged();

    /// The objects of `instancePath` arrived.
    void objectsReceived(const QString& instancePath,
                         const QVector<enzan::gui::SignalInfo>& objects);

    /// New values reached the database. The views redraw.
    void wavesChanged();

    /// The design was restarted, so everything the views hold is stale.
    void wavesCleared();

    void runStateChanged();

    void transcriptReceived(const QString& text);

    /// A diagnostic, with its severity as `DiagnosticSeverity` numbers it.
    void diagnosticReceived(int severity, const QString& text, const QString& file, int line);

    /// A request could not be honoured. The connection is still up.
    void requestFailed(const QString& reason);

    void detached(const QString& reason);

private:
    // Plain member functions rather than slots: everything here is connected with the
    // function-pointer form of `connect`, which needs no `Q_SLOTS` annotation and gives a
    // compile error rather than a run-time warning when a signature does not match.
    void onReadable();
    void onWritable();

    /// Reads whatever has arrived and dispatches it.
    void pump();

    void dispatch(const ipc::Frame& frame);
    void onWelcome(const ipc::Frame& frame);
    void onDesign(const ipc::Frame& frame);
    void onObjects(const ipc::Frame& frame);
    void onValueChanges(const ipc::Frame& frame);
    void onHistory(const ipc::Frame& frame);
    void onRunState(const ipc::Frame& frame);

    /// Sends a request and starts draining the socket.
    template<typename Message>
    void send(ipc::MessageType type, Message& message) {
        if (!connection.isOpen()) return;
        connection.send(type, message);
        flush();
    }

    void flush();

    /// Starts watching the connection's descriptor. Called once a connection exists.
    void watch();

    void teardown(const QString& reason);

    /// Declares a signal to the database if it is not there yet.
    std::uint32_t recordFor(std::uint32_t signalIndex, std::uint32_t width);

    /// Throws away every trace, for a restart.
    void resetDatabase();

    ipc::Connection connection;
    QSocketNotifier* readNotifier = nullptr;
    QSocketNotifier* writeNotifier = nullptr;
    QProcess* kernelProcess = nullptr;

    std::unique_ptr<wavedb::WaveformDatabase> database;

    QVector<ipc::InstanceEntry> designInstances;

    /// Signal metadata by kernel index. Ordered rather than hash-based, because what the GUI
    /// lists is observable and section 3.8 makes observable ordering reproducible.
    std::map<std::uint32_t, SignalInfo> knownSignals;

    QString top;
    QString version;
    runtime::TimeScale scale;
    runtime::SimTime kernelTime = 0;
    runtime::SimTime recordedTime = 0;
    ipc::SimulationState simulationState = ipc::SimulationState::Idle;

    /// The socket path this session created for a kernel it launched, removed on the way out.
    QString launchedSocketPath;
};

} // namespace enzan::gui
