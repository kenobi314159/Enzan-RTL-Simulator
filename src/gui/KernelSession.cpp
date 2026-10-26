// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/KernelSession.h"

#include "ipc/Codec.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace enzan::gui {

namespace {} // namespace

KernelSession::KernelSession(QObject* parent)
    : QObject(parent), database(std::make_unique<wavedb::WaveformDatabase>()) {}

KernelSession::~KernelSession() {
    teardown(QString());
}

bool KernelSession::isAttached() const {
    return connection.isOpen();
}

bool KernelSession::attach(const QString& socketPath, QString& error) {
    std::string reason;
    if (!ipc::connectTo(socketPath.toStdString(), connection, reason)) {
        error = QString::fromStdString(reason);
        return false;
    }

    watch();

    ipc::HelloMessage hello;
    hello.protocolVersion = ipc::ProtocolVersion;
    hello.clientName = QCoreApplication::applicationName().toStdString();
    send(ipc::MessageType::Hello, hello);
    return true;
}

void KernelSession::watch() {
    delete readNotifier;
    delete writeNotifier;

    readNotifier = new QSocketNotifier(connection.descriptor(), QSocketNotifier::Read, this);
    connect(readNotifier, &QSocketNotifier::activated, this, &KernelSession::onReadable);

    // Enabled only while something is queued. A write notifier left on fires continuously
    // whenever the socket is writable, which is almost always, and would spin the event loop.
    writeNotifier = new QSocketNotifier(connection.descriptor(), QSocketNotifier::Write, this);
    writeNotifier->setEnabled(false);
    connect(writeNotifier, &QSocketNotifier::activated, this, &KernelSession::onWritable);
}

void KernelSession::teardown(const QString& reason) {
    if (readNotifier != nullptr) {
        readNotifier->setEnabled(false);
        readNotifier->deleteLater();
        readNotifier = nullptr;
    }
    if (writeNotifier != nullptr) {
        writeNotifier->setEnabled(false);
        writeNotifier->deleteLater();
        writeNotifier = nullptr;
    }

    const bool wasOpen = connection.isOpen();
    connection.close();
    fetching = false;
    fetchAgain = false;

    if (wasOpen) Q_EMIT detached(reason);
}

void KernelSession::detach() {
    if (connection.isOpen()) {
        ipc::DetachMessage message;
        connection.send(ipc::MessageType::Detach, message);
        static_cast<void>(connection.flushBlocking());
    }
    teardown(tr("detached"));
}

void KernelSession::deposit(std::uint32_t signalIndex, const eir::BitVector& value) {
    ipc::DepositMessage message;
    message.signalIndex = signalIndex;
    message.value = value;
    send(ipc::MessageType::Deposit, message);
}

void KernelSession::force(std::uint32_t signalIndex, const eir::BitVector& value) {
    ipc::ForceMessage message;
    message.signalIndex = signalIndex;
    message.value = value;
    send(ipc::MessageType::Force, message);
}

void KernelSession::release(std::uint32_t signalIndex) {
    ipc::ForceMessage message;
    message.signalIndex = signalIndex;
    message.release = true;
    send(ipc::MessageType::Force, message);
}

void KernelSession::quitKernel() {
    if (connection.isOpen()) {
        ipc::QuitMessage message;
        connection.send(ipc::MessageType::Quit, message);
        static_cast<void>(connection.flushBlocking());
    }
    teardown(tr("the kernel was asked to exit"));
}

void KernelSession::flush() {
    if (!connection.isOpen()) return;

    if (!connection.flush()) {
        teardown(QString::fromStdString(connection.error()));
        return;
    }

    // The notifier is the only thing that will come back to finish a write the socket could not
    // take, so it goes on exactly when there is something left and off again as soon as there
    // is not.
    if (writeNotifier != nullptr) writeNotifier->setEnabled(connection.hasPendingOutput());
}

void KernelSession::onWritable() {
    flush();
}

void KernelSession::onReadable() {
    pump();
}

void KernelSession::pump() {
    if (!connection.isOpen()) return;

    if (!connection.receive()) {
        teardown(QString::fromStdString(connection.error()));
        return;
    }

    while (connection.isOpen()) {
        ipc::Frame frame;
        const ipc::ReceiveStatus status = connection.next(frame);
        if (status == ipc::ReceiveStatus::Empty) break;
        if (status == ipc::ReceiveStatus::Malformed) {
            teardown(tr("the kernel sent a message this build cannot read"));
            return;
        }
        dispatch(frame);
    }

    if (std::exchange(wavesPending, false)) Q_EMIT wavesChanged();
    flush();
}

void KernelSession::dispatch(const ipc::Frame& frame) {
    switch (static_cast<ipc::MessageType>(frame.type)) {
    case ipc::MessageType::Welcome: onWelcome(frame); return;
    case ipc::MessageType::Design: onDesign(frame); return;
    case ipc::MessageType::Objects: onObjects(frame); return;
    case ipc::MessageType::Changes: onChanges(frame); return;
    case ipc::MessageType::Progress: onProgress(frame); return;
    case ipc::MessageType::RunState: onRunState(frame); return;

    case ipc::MessageType::Transcript: {
        ipc::TranscriptMessage message;
        if (ipc::decodePayload(frame.payload, message)) {
            Q_EMIT transcriptReceived(QString::fromStdString(message.text));
        }
        return;
    }

    case ipc::MessageType::Diagnostic: {
        ipc::DiagnosticMessage message;
        if (ipc::decodePayload(frame.payload, message)) {
            Q_EMIT diagnosticReceived(message.severity, QString::fromStdString(message.message),
                                      QString::fromStdString(message.file),
                                      static_cast<int>(message.line));
        }
        return;
    }

    case ipc::MessageType::Failure: {
        ipc::FailureMessage message;
        if (ipc::decodePayload(frame.payload, message)) {
            Q_EMIT requestFailed(QString::fromStdString(message.reason));
        }
        return;
    }

    case ipc::MessageType::Bye: {
        ipc::ByeMessage message;
        const QString reason = ipc::decodePayload(frame.payload, message)
                                   ? QString::fromStdString(message.reason)
                                   : tr("the kernel closed the connection");
        teardown(reason);
        return;
    }

    default:
        // A request arriving from the kernel, or a message from a newer build. Neither is
        // actionable and neither is worth dropping the connection over.
        return;
    }
}

void KernelSession::onWelcome(const ipc::Frame& frame) {
    ipc::WelcomeMessage message;
    if (!ipc::decodePayload(frame.payload, message)) {
        teardown(tr("the kernel's greeting could not be read"));
        return;
    }

    if (message.protocolVersion != ipc::ProtocolVersion) {
        teardown(tr("this front end speaks protocol version %1 and the kernel speaks %2; both "
                    "must come from the same build")
                     .arg(ipc::ProtocolVersion)
                     .arg(message.protocolVersion));
        return;
    }

    top = QString::fromStdString(message.topModule);
    version = QString::fromStdString(message.kernelVersion);
    scale = runtime::TimeScale(message.timeExponent);
    recordedWidthLimit = message.widthLimit;
    kernelTime = message.currentTime;
    recordedTime = message.latestRecordedTime;
    simulationState = message.state;

    Q_EMIT welcomed();
    Q_EMIT runStateChanged();
    queryDesign();
}

void KernelSession::onDesign(const ipc::Frame& frame) {
    ipc::DesignMessage message;
    if (!ipc::decodePayload(frame.payload, message)) return;

    designInstances.clear();
    designInstances.reserve(static_cast<qsizetype>(message.instances.size()));
    for (ipc::InstanceEntry& entry : message.instances) {
        designInstances.push_back(std::move(entry));
    }

    designSources.clear();
    for (const std::string& file : message.sourceFiles)
        designSources.push_back(QString::fromStdString(file));

    Q_EMIT designChanged();
}

void KernelSession::onObjects(const ipc::Frame& frame) {
    ipc::ObjectsMessage message;
    if (!ipc::decodePayload(frame.payload, message)) return;

    // Built once per message and shared by every signal that names it, which is how the
    // kernel sends it and what stops a scope of thirty state-machine signals carrying thirty
    // copies of the same six names.
    std::vector<std::shared_ptr<const EnumInfo>> enums;
    enums.reserve(message.enums.size());
    for (const ipc::EnumEntry& entry : message.enums) {
        auto info = std::make_shared<EnumInfo>();
        info->name = QString::fromStdString(entry.name);
        for (const ipc::EnumMemberEntry& member : entry.members) {
            info->members.push_back(
                EnumMemberInfo{.name = QString::fromStdString(member.name), .value = member.value});
        }
        enums.push_back(std::move(info));
    }

    QVector<SignalInfo> objects;
    objects.reserve(static_cast<qsizetype>(message.objects.size()));

    for (const ipc::ObjectEntry& entry : message.objects) {
        SignalInfo info;
        info.signalIndex = entry.signalIndex;
        info.name = QString::fromStdString(entry.name);
        info.path = QString::fromStdString(entry.path);
        info.width = entry.width;
        info.recording = entry.recording;
        info.dimensions = entry.dimensions;
        if (entry.enumIndex < enums.size()) info.enumeration = enums[entry.enumIndex];

        knownSignals[entry.signalIndex] = info;
        objects.push_back(info);
    }

    // Parameters come after the signals, in declaration order. After, because a reader opens a
    // scope looking for its signals; in the list rather than beside it, because a parameter is
    // a row in the wave window like anything else once it is put there.
    for (const ipc::ParameterEntry& entry : message.parameters) {
        SignalInfo info;
        info.name = QString::fromStdString(entry.name);
        info.path = QString::fromStdString(entry.path);
        info.width = entry.value.width();
        info.constantValue = entry.value;
        info.isLocalParameter = entry.isLocal;

        // Deliberately not put in `knownSignals`: a parameter has no signal index, and every
        // one of them would claim index zero.
        objects.push_back(std::move(info));
    }

    Q_EMIT objectsReceived(QString::fromStdString(message.instancePath), objects);
}

QString SignalInfo::declaredRanges() const {
    QString ranges;
    std::uint64_t elements = 1;
    for (const eir::ArrayDimension& dimension : dimensions) {
        ranges += QStringLiteral("[%1:%2]").arg(dimension.left).arg(dimension.right);
        elements *= dimension.size();
    }
    if (const std::uint64_t element = width / elements; element > 1)
        ranges += QStringLiteral("[%1:0]").arg(element - 1);
    return ranges;
}

QString EnumInfo::nameOf(const eir::BitVector& value) const {
    // A linear scan over a handful of members. Two members may share a value, which IEEE 1800
    // allows; the first wins, so the answer is the one the reader wrote first.
    for (const EnumMemberInfo& member : members) {
        if (member.value == value) return member.name;
    }
    return {};
}

const SignalInfo* KernelSession::signalInfo(std::uint32_t signalIndex) const {
    const auto found = knownSignals.find(signalIndex);
    return found == knownSignals.end() ? nullptr : &found->second;
}

std::uint32_t KernelSession::recordFor(std::uint32_t signalIndex, std::uint32_t width) {
    const std::uint32_t existing = database->recordIndexOf(signalIndex);
    if (existing != wavedb::WaveformDatabase::NotRecorded) return existing;

    const SignalInfo* info = signalInfo(signalIndex);
    const std::string path = info != nullptr ? info->path.toStdString() : std::string();
    return database->addSignal(signalIndex, path, width);
}

void KernelSession::onChanges(const ipc::Frame& frame) {
    fetching = false;
    ipc::ChangesMessage message;
    if (!ipc::decodePayload(frame.payload, message)) return;

    bool behind = false;
    bool diverged = false;
    for (const ipc::SignalChanges& changes : message.signals) {
        // Not shown any more, or shown again since and asked for afresh.
        if (std::ranges::find(shownSignals, changes.signalIndex) == shownSignals.end()) continue;
        if (changes.width == 0) {
            completeTo[changes.signalIndex] = changes.completeTo;
            continue;
        }

        const std::uint32_t record = recordFor(changes.signalIndex, changes.width);

        // A value at a width the database was not told about would violate what the database is
        // allowed to assume. It means the two ends disagree, so the signal is left as it is, and
        // counted complete rather than holding every other trace back with it.
        const auto agrees = [&changes](const ipc::ChangeEntry& entry) {
            return entry.value.width() == changes.width;
        };
        if (database->signal(record).width != changes.width
            || !std::ranges::all_of(changes.entries, agrees)) {
            completeTo[changes.signalIndex] = changes.completeTo;
            continue;
        }

        // An answer to a cursor this copy no longer has, sent before a restart emptied it, is
        // dropped and the signal asked for again. A kernel holding fewer changes than this copy
        // has started again without the time saying so, and the copy starts again with it.
        if (changes.first != database->changeCount(record)) {
            diverged = diverged || changes.first < database->changeCount(record);
            behind = true;
            continue;
        }
        for (const ipc::ChangeEntry& entry : changes.entries)
            database->append(record, entry.time, entry.value);
        completeTo[changes.signalIndex] = changes.completeTo;
        if (database->changeCount(record) < changes.recorded) behind = true;
        wavesPending = true;
    }

    if (diverged) {
        resetDatabase();
        Q_EMIT wavesCleared();
    }
    advanceExtent();
    if (behind || std::exchange(fetchAgain, false)) fetchChanges();
}

void KernelSession::onProgress(const ipc::Frame& frame) {
    ipc::ProgressMessage message;
    if (!ipc::decodePayload(frame.payload, message)) return;

    kernelTime = message.time;
    recordedTime = message.latestRecordedTime;
    database->setLatestTime(recordedTime);
    advanceExtent();
    Q_EMIT runStateChanged();
    fetchChanges();
}

void KernelSession::fetchChanges() {
    if (!connection.isOpen() || shownSignals.empty()) return;
    if (fetching) {
        fetchAgain = true;
        return;
    }

    ipc::FetchChangesMessage message;
    message.signals.reserve(shownSignals.size());
    for (const std::uint32_t signalIndex : shownSignals) {
        const std::uint32_t record = database->recordIndexOf(signalIndex);
        message.signals.push_back(
            ipc::ChangeCursor{.signalIndex = signalIndex,
                              .held = record == wavedb::WaveformDatabase::NotRecorded
                                          ? 0
                                          : database->changeCount(record)});
    }
    fetching = true;
    send(ipc::MessageType::FetchChanges, message);
}

void KernelSession::advanceExtent() {
    extent = std::max(extent, heldTime());
}

runtime::SimTime KernelSession::heldTime() const noexcept {
    if (shownSignals.empty()) return recordedTime;
    runtime::SimTime held = recordedTime;
    for (const std::uint32_t signalIndex : shownSignals) {
        const auto found = completeTo.find(signalIndex);
        held = std::min(held, found == completeTo.end() ? 0 : found->second);
    }
    return held;
}

void KernelSession::resetDatabase() {
    // Rebuilt rather than cleared: the database is append-only by design, which is what makes
    // it cheap, and a restart is the one event that invalidates everything in it.
    database = std::make_unique<wavedb::WaveformDatabase>();
    completeTo.clear();
    extent = 0;
}

void KernelSession::onRunState(const ipc::Frame& frame) {
    ipc::RunStateMessage message;
    if (!ipc::decodePayload(frame.payload, message)) return;

    // Simulated time never moves backwards during a run, so a time earlier than the last one
    // reported can only mean the design was restarted. One rule, rather than a flag that could
    // disagree with the time beside it.
    const bool restarted = message.time < kernelTime;

    simulationState = message.state;
    kernelTime = message.time;
    recordedTime = message.latestRecordedTime;

    if (restarted) {
        resetDatabase();
        Q_EMIT wavesCleared();
    }
    database->setLatestTime(recordedTime);
    advanceExtent();

    Q_EMIT runStateChanged();

    // What the run recorded last, or after a restart everything again from the start.
    fetchChanges();
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

void KernelSession::queryDesign() {
    ipc::QueryDesignMessage message;
    send(ipc::MessageType::QueryDesign, message);
}

void KernelSession::queryObjects(const QString& instancePath) {
    ipc::QueryObjectsMessage message;
    message.instancePath = instancePath.toStdString();
    send(ipc::MessageType::QueryObjects, message);
}

void KernelSession::subscribe(const QVector<std::uint32_t>& signalIndices) {
    if (signalIndices.isEmpty()) return;

    ipc::SubscribeMessage message;
    message.signalIndices.assign(signalIndices.begin(), signalIndices.end());
    send(ipc::MessageType::Subscribe, message);

    for (const std::uint32_t signalIndex : signalIndices) {
        if (std::ranges::find(shownSignals, signalIndex) == shownSignals.end())
            shownSignals.push_back(signalIndex);
    }
    // After the subscription, which the kernel answers first: by then it records them all.
    fetchChanges();
}

void KernelSession::unsubscribe(const QVector<std::uint32_t>& signalIndices) {
    if (signalIndices.isEmpty()) return;

    ipc::UnsubscribeMessage message;
    message.signalIndices.assign(signalIndices.begin(), signalIndices.end());
    send(ipc::MessageType::Unsubscribe, message);

    for (const std::uint32_t signalIndex : signalIndices) {
        std::erase(shownSignals, signalIndex);
        completeTo.erase(signalIndex);
    }
    // The one holding the others back may have gone.
    advanceExtent();
}

void KernelSession::run(std::uint64_t duration) {
    ipc::RunMessage message;
    message.duration = duration;
    send(ipc::MessageType::Run, message);
}

void KernelSession::runUntilFinished() {
    ipc::RunMessage message;
    message.untilFinished = true;
    send(ipc::MessageType::Run, message);
}

void KernelSession::stop() {
    ipc::StopMessage message;
    send(ipc::MessageType::Stop, message);
}

void KernelSession::restart() {
    ipc::RestartMessage message;
    send(ipc::MessageType::Restart, message);
}

} // namespace enzan::gui
