// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/ObjectsModel.h"

#include "wavedb/WaveformDatabase.h"

#include <cstdint>
#include <optional>

namespace enzan::gui {

namespace {

/// What the type column writes after `logic`: the signal's dimensions, `[63:0][17:0]`, then the
/// bits of an element wider than one, which has no dimension of its own, `int` or a record.
QString declaredRanges(const SignalInfo& info) {
    QString ranges;
    std::uint64_t elements = 1;
    for (const eir::ArrayDimension& dimension : info.dimensions) {
        ranges += QStringLiteral("[%1:%2]").arg(dimension.left).arg(dimension.right);
        elements *= dimension.size();
    }
    if (const std::uint64_t element = info.width / elements; element > 1)
        ranges += QStringLiteral("[%1:0]").arg(element - 1);
    return ranges.isEmpty() ? ranges : u' ' + ranges;
}

} // namespace

ObjectsModel::ObjectsModel(const KernelSession& session, QObject* parent)
    : QAbstractTableModel(parent), kernelSession(&session) {}

void ObjectsModel::setObjects(const QString& scope, const QVector<SignalInfo>& newObjects) {
    beginResetModel();
    scopePath = scope;
    objects = newObjects;
    endResetModel();
}

void ObjectsModel::setCursorTime(runtime::SimTime time) {
    if (time == cursorTime) return;
    cursorTime = time;
    refreshValues();
}

void ObjectsModel::refreshValues() {
    if (objects.isEmpty()) return;
    Q_EMIT dataChanged(index(0, ValueColumn),
                       index(static_cast<int>(objects.size()) - 1, ValueColumn), {Qt::DisplayRole});
}

void ObjectsModel::setRadix(eir::Radix newRadix) {
    if (newRadix == valueRadix) return;
    valueRadix = newRadix;
    refreshValues();
}

const SignalInfo* ObjectsModel::objectAt(int row) const {
    if (row < 0 || row >= objects.size()) return nullptr;
    return &objects[row];
}

int ObjectsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(objects.size());
}

int ObjectsModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QString ObjectsModel::valueOf(const SignalInfo& info) const {
    // A parameter is the same at every time, so the cursor does not come into it.
    if (info.isParameter() && info.constantValue) {
        return QString::fromStdString(eir::toString(info.constantValue->ref(), valueRadix));
    }

    // A null array has no bits, and so no value at any time.
    if (info.width == 0) return {};

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();

    const std::uint32_t record = database.recordIndexOf(info.signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) {
        // Told apart on purpose. A signal nobody has asked for has no trace here yet; one the
        // kernel is not logging never will, and showing the same blank for both would have the
        // user waiting for a value that is not coming.
        switch (info.recording) {
        case ipc::Recording::Recorded: return tr("—");
        case ipc::Recording::TooWide: return tr("too wide to log");
        case ipc::Recording::OutsideScope: break;
        }
        return tr("not logged");
    }

    const std::optional<eir::BitVector> value = database.valueAt(record, cursorTime);
    if (!value.has_value()) return tr("—");

    return QString::fromStdString(eir::toString(value->ref(), valueRadix));
}

QVariant ObjectsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= objects.size()) return {};

    const SignalInfo& info = objects[index.row()];

    if (role == SignalIndexRole) return info.signalIndex;

    if (role == Qt::ToolTipRole) {
        if (info.isParameter() || info.recording != ipc::Recording::TooWide) return info.path;
        return tr("%1\nWider than the %2 bits the kernel records; --wave-max-bits raises the limit")
            .arg(info.path)
            .arg(kernelSession->widthLimit());
    }

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
    case NameColumn: return info.name;
    case ValueColumn: return valueOf(info);
    case TypeColumn:
        // A parameter says which it is rather than how wide it is. Its width is whatever the
        // value needed, which is not something the reader wrote and not worth showing them.
        if (info.isParameter()) return info.isLocalParameter ? tr("localparam") : tr("parameter");
        if (info.width == 0) return tr("null array");
        return tr("logic%1").arg(declaredRanges(info));
    default: return {};
    }
}

QVariant ObjectsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};

    switch (section) {
    case NameColumn: return tr("Name");
    case ValueColumn: return tr("Value");
    case TypeColumn: return tr("Type");
    default: return {};
    }
}

} // namespace enzan::gui
