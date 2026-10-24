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

    flush();
}

void KernelSession::dispatch(const ipc::Frame& frame) {
    switch (static_cast<ipc::MessageType>(frame.type)) {
    case ipc::MessageType::Welcome: onWelcome(frame); return;
    case ipc::MessageType::Design: onDesign(frame); return;
    case ipc::MessageType::Objects: onObjects(frame); return;
    case ipc::MessageType::ValueChanges: onValueChanges(frame); return;
    case ipc::MessageType::History: onHistory(frame); return;
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

void KernelSession::onValueChanges(const ipc::Frame& frame) {
    ipc::ValueChangesMessage message;
    if (!ipc::decodePayload(frame.payload, message)) return;

    for (const ipc::ValueUpdate& update : message.updates) {
        const std::uint32_t record = recordFor(update.signalIndex, update.value.width());

        // A value that arrives out of order, or at a width the database was not told about,
        // would violate what the database is allowed to assume, so it is dropped rather than
        // appended. It means the two ends disagree, which shows up elsewhere as well.
        if (database->signal(record).width != update.value.width()) continue;
        if (database->changeCount(record) > 0
            && database->timeOfChange(record, database->changeCount(record) - 1) > message.time) {
            continue;
        }

        database->append(record, message.time, update.value);
    }

    database->setLatestTime(message.time);
    recordedTime = std::max(message.time, recordedTime);

    Q_EMIT wavesChanged();
}

void KernelSession::onHistory(const ipc::Frame& frame) {
    ipc::HistoryMessage message;
    if (!ipc::decodePayload(frame.payload, message)) return;

    const std::uint32_t record = recordFor(message.signalIndex, message.width);

    for (const ipc::HistoryEntry& entry : message.entries) {
        if (entry.value.width() != database->signal(record).width) continue;
        if (database->changeCount(record) > 0
            && database->timeOfChange(record, database->changeCount(record) - 1) > entry.time) {
            continue;
        }
        database->append(record, entry.time, entry.value);
    }

    database->setLatestTime(message.latestTime);
    recordedTime = std::max(message.latestTime, recordedTime);

    // A truncated answer says where it stopped, so the rest is asked for from there rather
    // than leaving a hole in the middle of the trace.
    if (!message.complete) {
        ipc::QueryHistoryMessage more;
        more.signalIndex = message.signalIndex;
        more.fromTime = message.latestTime;
        more.toTime = runtime::NoTime;
        send(ipc::MessageType::QueryHistory, more);
    }

    Q_EMIT wavesChanged();
}

void KernelSession::resetDatabase() {
    // Rebuilt rather than cleared: the database is append-only by design, which is what makes
    // it cheap, and a restart is the one event that invalidates everything in it.
    database = std::make_unique<wavedb::WaveformDatabase>();
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

    Q_EMIT runStateChanged();
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
}

void KernelSession::unsubscribe(const QVector<std::uint32_t>& signalIndices) {
    if (signalIndices.isEmpty()) return;

    ipc::UnsubscribeMessage message;
    message.signalIndices.assign(signalIndices.begin(), signalIndices.end());
    send(ipc::MessageType::Unsubscribe, message);
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
