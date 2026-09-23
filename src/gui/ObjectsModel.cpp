// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/ObjectsModel.h"

#include "wavedb/WaveformDatabase.h"

#include <optional>

namespace enzan::gui {

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
    if (info.isParameter()) {
        return QString::fromStdString(eir::toString(info.constantValue->ref(), valueRadix));
    }

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();

    const std::uint32_t record = database.recordIndexOf(info.signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) {
        // Told apart on purpose. A signal nobody has asked for has no trace here yet; one the
        // kernel is not logging never will, and showing the same blank for both would have the
        // user waiting for a value that is not coming.
        return info.recorded ? tr("—") : tr("not logged");
    }

    const std::optional<eir::BitVector> value = database.valueAt(record, cursorTime);
    if (!value.has_value()) return tr("—");

    return QString::fromStdString(eir::toString(value->ref(), valueRadix));
}

QVariant ObjectsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= objects.size()) return {};

    const SignalInfo& info = objects[index.row()];

    if (role == SignalIndexRole) return info.signalIndex;

    if (role == Qt::ToolTipRole) return info.path;

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
    case NameColumn: return info.name;
    case ValueColumn: return valueOf(info);
    case TypeColumn:
        // A parameter says which it is rather than how wide it is. Its width is whatever the
        // value needed, which is not something the reader wrote and not worth showing them.
        if (info.isParameter()) return info.isLocalParameter ? tr("localparam") : tr("parameter");
        return info.width == 1 ? tr("logic") : tr("logic [%1:0]").arg(info.width - 1);
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
