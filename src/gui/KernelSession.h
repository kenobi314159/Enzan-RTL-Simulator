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
#include <optional>

class QSocketNotifier;

namespace enzan::gui {

/// What the GUI knows about one signal.
/// What an enumeration calls one of its values.
struct EnumMemberInfo {
    QString name;
    eir::BitVector value = eir::BitVector::filled(1, eir::Logic::Unknown);
};

/// The names an enumerated signal's type gives its values.
///
/// Shared between every signal of the same type rather than copied into each, which is the
/// shape the kernel sends it in: one scope's worth of state machines usually share a type.
struct EnumInfo {
    /// The type's declared name, or empty for an anonymous enumeration.
    QString name;
    QVector<EnumMemberInfo> members;

    /// The name for `value`, or an empty string when no member has it.
    [[nodiscard]] QString nameOf(const eir::BitVector& value) const;
};

/// One thing a scope contains, which is a signal or a parameter.
///
/// One type rather than two. A row in the objects window, a row in the wave list and an entry
/// on the clipboard all want the same things from it — a name, a path, a width and a value —
/// and the only difference is where the value comes from. Carrying that difference as a field
/// means the two places that actually read a value have to know about it and nothing else
/// does. `constantValue` is the discriminator: a parameter has one, a signal never does, and
/// `signalIndex` and `recorded` mean nothing on a parameter.
struct SignalInfo {
    std::uint32_t signalIndex = 0;
    QString name;
    QString path;
    std::uint32_t width = 1;

    /// A parameter's value, which is what it is at every time. Absent for a signal.
    ///
    /// A parameter has no slot, no trace and nothing to subscribe to: it is a fact about the
    /// elaborated design rather than a piece of its state (DESIGN_GUIDE.md section 10.7).
    std::optional<eir::BitVector> constantValue;

    /// True for a `localparam`, which is set inside the module rather than from outside it.
    /// Meaningless unless `constantValue` is set.
    bool isLocalParameter = false;

    /// True when this describes a parameter rather than a signal.
    [[nodiscard]] bool isParameter() const noexcept { return constantValue.has_value(); }

    /// False when the kernel is not recording this signal, so that "nothing has happened yet"
    /// and "nothing is being kept" can be told apart on screen.
    bool recorded = false;

    /// The names this signal's type gives its values, or nothing when its type is not an
    /// enumeration.
    ///
    /// Held by value rather than by index into a table of the session's, because a
    /// `SignalInfo` is copied into rows, into the objects window and into the clipboard, and
    /// an index would make every one of those depend on the table outliving it. An enumeration
    /// is a handful of short names.
    std::shared_ptr<const EnumInfo> enumeration;
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

    void detach();

    [[nodiscard]] bool isAttached() const;

    // --- requests ----------------------------------------------------------

    void queryDesign();
    void queryObjects(const QString& instancePath);
    void subscribe(const QVector<std::uint32_t>& signalIndices);
    void unsubscribe(const QVector<std::uint32_t>& signalIndices);
    void run(std::uint64_t duration);
    void runUntilFinished();
    void stop();
    void restart();

    /// Writes a signal once. The next thing that drives it overwrites the value.
    void deposit(std::uint32_t signalIndex, const eir::BitVector& value);

    /// Holds a signal at a value until `release`.
    void force(std::uint32_t signalIndex, const eir::BitVector& value);

    void release(std::uint32_t signalIndex);

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

    /// The source files the design was analysed from, in analysis order.
    [[nodiscard]] const QStringList& sourceFiles() const noexcept { return designSources; }

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

    std::unique_ptr<wavedb::WaveformDatabase> database;

    QVector<ipc::InstanceEntry> designInstances;
    QStringList designSources;

    /// Signal metadata by kernel index. Ordered rather than hash-based, because what the GUI
    /// lists is observable and section 3.8 makes observable ordering reproducible.
    std::map<std::uint32_t, SignalInfo> knownSignals;

    QString top;
    QString version;
    runtime::TimeScale scale;
    runtime::SimTime kernelTime = 0;
    runtime::SimTime recordedTime = 0;
    ipc::SimulationState simulationState = ipc::SimulationState::Idle;
};

} // namespace enzan::gui
