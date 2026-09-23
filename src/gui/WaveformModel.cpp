// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/WaveformModel.h"

#include "wavedb/WaveformDatabase.h"

#include <QDataStream>
#include <QIODevice>
#include <QMimeData>

#include <algorithm>
#include <utility>

namespace enzan::gui {

namespace {

/// The drag payload a wave row travels in. Private to this model: rows are moved within one
/// window, and a format another application could parse would be a promise to keep it stable.
constexpr const char* RowMimeType = "application/x-enzan-wave-rows";

} // namespace

WaveformModel::WaveformModel(const KernelSession& session, const Theme& themeToUse, QObject* parent)
    : QAbstractItemModel(parent), kernelSession(&session), theme(&themeToUse) {}

// ---------------------------------------------------------------------------
// Tree bookkeeping
// ---------------------------------------------------------------------------

int WaveformModel::rowOf(int rowId) const {
    if (rowId < 0 || rowId >= rows.size()) return -1;

    const QVector<int>& siblings =
        rows[rowId].parent < 0 ? roots : rows[rows[rowId].parent].children;
    return static_cast<int>(siblings.indexOf(rowId));
}

QVector<int>& WaveformModel::siblingsOf(int rowId) {
    return rows[rowId].parent < 0 ? roots : rows[rows[rowId].parent].children;
}

const WaveRow* WaveformModel::row(int rowId) const {
    if (rowId < 0 || rowId >= rows.size() || !rows[rowId].alive) return nullptr;
    return &rows[rowId];
}

int WaveformModel::rowIdOf(const QModelIndex& index) const {
    if (!index.isValid()) return -1;

    const int rowId = static_cast<int>(index.internalId());
    return rowId >= 0 && rowId < rows.size() && rows[rowId].alive ? rowId : -1;
}

QModelIndex WaveformModel::indexOfRow(int rowId) const {
    const int position = rowOf(rowId);
    if (position < 0) return {};
    return createIndex(position, 0, static_cast<quintptr>(rowId));
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

int WaveformModel::addSignal(const SignalInfo& info, int parentGroup) {
    WaveRow added;
    added.kind = WaveRow::Kind::Signal;
    added.label = info.name.isEmpty() ? info.path : info.name;
    added.signalIndex = info.signalIndex;
    added.signalWidth = info.width;
    added.parent = row(parentGroup) != nullptr && rows[parentGroup].kind == WaveRow::Kind::Group
                       ? parentGroup
                       : -1;

    const int rowId = static_cast<int>(rows.size());
    const int parentRow = added.parent;

    // The position is taken by value and the list is reached again after the row is stored.
    // Holding a reference into `rows` across the `push_back` below would leave it dangling the
    // moment the vector grew — which it does, silently, and then the child is appended to
    // freed memory and the parent appears to have no children at all.
    const int at = parentRow < 0 ? static_cast<int>(roots.size())
                                 : static_cast<int>(rows[parentRow].children.size());

    beginInsertRows(parentRow < 0 ? QModelIndex() : indexOfRow(parentRow), at, at);
    rows.push_back(added);
    (parentRow < 0 ? roots : rows[parentRow].children).push_back(rowId);
    endInsertRows();

    Q_EMIT displayedSignalsChanged();
    return rowId;
}

int WaveformModel::addGroup(const QString& name, int parentGroup) {
    WaveRow added;
    added.kind = WaveRow::Kind::Group;
    added.label = name;
    added.parent = row(parentGroup) != nullptr && rows[parentGroup].kind == WaveRow::Kind::Group
                       ? parentGroup
                       : -1;

    const int rowId = static_cast<int>(rows.size());
    const int parentRow = added.parent;
    const int at = parentRow < 0 ? static_cast<int>(roots.size())
                                 : static_cast<int>(rows[parentRow].children.size());

    beginInsertRows(parentRow < 0 ? QModelIndex() : indexOfRow(parentRow), at, at);
    rows.push_back(added);
    (parentRow < 0 ? roots : rows[parentRow].children).push_back(rowId);
    endInsertRows();

    return rowId;
}

int WaveformModel::addSlice(int rowId, std::uint32_t high, std::uint32_t low) {
    const WaveRow* source = row(rowId);
    if (source == nullptr || source->kind != WaveRow::Kind::Signal) return -1;

    const std::uint32_t top = std::max(high, low);
    const std::uint32_t bottom = std::min(high, low);
    if (top >= source->signalWidth) return -1;

    WaveRow added = *source;
    added.children.clear();
    added.sliced = true;
    added.highBit = top;
    added.lowBit = bottom;
    added.label = top == bottom
                      ? QStringLiteral("%1[%2]").arg(source->label).arg(top)
                      : QStringLiteral("%1[%2:%3]").arg(source->label).arg(top).arg(bottom);

    // Placed beside the signal it came from rather than at the end, because that is where the
    // user is looking when they ask for it.
    const int parentGroup = source->parent;
    const int at = rowOf(rowId) + 1;

    const int newId = static_cast<int>(rows.size());
    beginInsertRows(parentGroup < 0 ? QModelIndex() : indexOfRow(parentGroup), at, at);
    rows.push_back(added);
    (parentGroup < 0 ? roots : rows[parentGroup].children).insert(at, newId);
    endInsertRows();

    return newId;
}

void WaveformModel::killSubtree(int rowId) {
    rows[rowId].alive = false;
    for (const int child : rows[rowId].children) killSubtree(child);
    rows[rowId].children.clear();
}

void WaveformModel::removeRow(int rowId) {
    if (row(rowId) == nullptr) return;

    const int parentGroup = rows[rowId].parent;
    const int position = rowOf(rowId);
    if (position < 0) return;

    beginRemoveRows(parentGroup < 0 ? QModelIndex() : indexOfRow(parentGroup), position, position);
    siblingsOf(rowId).removeAt(position);
    killSubtree(rowId);
    endRemoveRows();

    Q_EMIT displayedSignalsChanged();
}

void WaveformModel::clear() {
    beginResetModel();
    rows.clear();
    roots.clear();
    endResetModel();

    Q_EMIT displayedSignalsChanged();
}

void WaveformModel::setRadix(int rowId, eir::Radix radix) {
    if (row(rowId) == nullptr) return;

    rows[rowId].radix = radix;
    const QModelIndex changed = indexOfRow(rowId);
    Q_EMIT dataChanged(changed, changed.siblingAtColumn(ColumnCount - 1), {Qt::DisplayRole});
}

void WaveformModel::setRadixRecursive(int rowId, eir::Radix radix) {
    if (row(rowId) == nullptr) return;

    setRadix(rowId, radix);
    for (const int child : rows[rowId].children) setRadixRecursive(child, radix);
}

void WaveformModel::setLabel(int rowId, const QString& label) {
    if (row(rowId) == nullptr || label.isEmpty()) return;

    rows[rowId].label = label;
    const QModelIndex changed = indexOfRow(rowId);
    Q_EMIT dataChanged(changed, changed, {Qt::DisplayRole, Qt::EditRole});
}

void WaveformModel::setHighlight(int rowId, int slot) {
    if (row(rowId) == nullptr) return;

    rows[rowId].highlight = std::clamp(slot, 0, HighlightSlots);
    const QModelIndex changed = indexOfRow(rowId);
    Q_EMIT dataChanged(changed, changed.siblingAtColumn(ColumnCount - 1),
                       {Qt::BackgroundRole, HighlightRole});
}

std::pair<std::uint32_t, std::uint32_t> WaveformModel::bitRange(const WaveRow& waveRow) {
    if (waveRow.sliced) return {waveRow.highBit, waveRow.lowBit};
    return {waveRow.signalWidth == 0 ? 0 : waveRow.signalWidth - 1, 0};
}

bool WaveformModel::canUnpack(int rowId) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr || waveRow->kind != WaveRow::Kind::Signal) return false;
    if (waveRow->unpacked) return false;

    const auto [high, low] = bitRange(*waveRow);
    return high > low;
}

void WaveformModel::unpack(int rowId) {
    if (!canUnpack(rowId)) return;

    const auto [high, low] = bitRange(rows[rowId]);
    const auto count = static_cast<int>(high - low + 1);

    beginInsertRows(indexOfRow(rowId), 0, count - 1);

    rows[rowId].unpacked = true;
    for (std::uint32_t bit = high + 1; bit-- > low;) {
        WaveRow child;
        child.kind = WaveRow::Kind::Signal;
        child.signalIndex = rows[rowId].signalIndex;
        child.signalWidth = rows[rowId].signalWidth;
        child.radix = rows[rowId].radix;
        child.highlight = rows[rowId].highlight;
        child.sliced = true;
        child.highBit = bit;
        child.lowBit = bit;
        child.label = QStringLiteral("%1[%2]").arg(rows[rowId].label).arg(bit);
        child.parent = rowId;

        const int childId = static_cast<int>(rows.size());
        rows.push_back(child);
        rows[rowId].children.push_back(childId);
    }

    endInsertRows();
}

bool WaveformModel::isDescendantOf(int candidate, int rowId) const {
    for (int walk = candidate; walk >= 0; walk = rows[walk].parent) {
        if (walk == rowId) return true;
    }
    return false;
}

bool WaveformModel::moveRowsTo(const QVector<int>& rowIds, int targetGroup, int position) {
    if (targetGroup >= 0
        && (row(targetGroup) == nullptr || rows[targetGroup].kind != WaveRow::Kind::Group)) {
        return false;
    }

    // A group dropped inside itself would make the tree a cycle and the view an unbounded
    // recursion, so the whole move is refused rather than half-performed.
    for (const int rowId : rowIds) {
        if (row(rowId) == nullptr) return false;
        if (targetGroup >= 0 && isDescendantOf(targetGroup, rowId)) return false;
    }

    int at = position;
    for (const int rowId : rowIds) {
        const int sourceParent = rows[rowId].parent;
        const int sourceRow = rowOf(rowId);
        if (sourceRow < 0) continue;

        const QModelIndex sourceIndex = sourceParent < 0 ? QModelIndex() : indexOfRow(sourceParent);
        beginRemoveRows(sourceIndex, sourceRow, sourceRow);
        siblingsOf(rowId).removeAt(sourceRow);
        endRemoveRows();

        // Taking a row out from before the insertion point moves that point up by one; not
        // accounting for it turns "drop between these two" into "drop one further down" every
        // time a row is dragged downwards within its own parent.
        if (sourceParent == targetGroup && at > sourceRow) --at;

        QVector<int>& destination = targetGroup < 0 ? roots : rows[targetGroup].children;
        const int insertAt =
            at < 0 || at > destination.size() ? static_cast<int>(destination.size()) : at;

        const QModelIndex targetIndex = targetGroup < 0 ? QModelIndex() : indexOfRow(targetGroup);
        beginInsertRows(targetIndex, insertAt, insertAt);
        rows[rowId].parent = targetGroup;
        destination.insert(insertAt, rowId);
        endInsertRows();

        at = insertAt + 1;
    }

    return true;
}

void WaveformModel::detach(int rowId) {
    const int position = rowOf(rowId);
    if (position >= 0) siblingsOf(rowId).removeAt(position);
}

void WaveformModel::moveToGroup(int rowId, int group) {
    if (row(rowId) == nullptr) return;
    if (group >= 0 && (row(group) == nullptr || rows[group].kind != WaveRow::Kind::Group)) return;
    if (group == rowId) return;

    // A group cannot be moved inside itself, which would make the tree a cycle and the view an
    // infinite recursion.
    for (int walk = group; walk >= 0; walk = rows[walk].parent) {
        if (walk == rowId) return;
    }

    beginResetModel();
    detach(rowId);
    rows[rowId].parent = group;
    (group < 0 ? roots : rows[group].children).push_back(rowId);
    endResetModel();
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

eir::BitVector WaveformModel::slice(const WaveRow& waveRow, const eir::BitVector& value) {
    if (!waveRow.sliced) return value;

    const std::uint32_t width = waveRow.highBit - waveRow.lowBit + 1;
    eir::BitVector narrowed = eir::BitVector::filled(width, eir::Logic::Zero);
    for (std::uint32_t bit = 0; bit < width; ++bit) {
        const std::uint32_t source = waveRow.lowBit + bit;
        narrowed.set(bit, source < value.width() ? value.get(source) : eir::Logic::Unknown);
    }
    return narrowed;
}

std::optional<eir::BitVector> WaveformModel::valueAt(int rowId, runtime::SimTime time) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr || waveRow->kind != WaveRow::Kind::Signal) return std::nullopt;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();
    const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) return std::nullopt;

    const std::optional<eir::BitVector> whole = database.valueAt(record, time);
    if (!whole.has_value()) return std::nullopt;

    return slice(*waveRow, *whole);
}

QString WaveformModel::renderValue(int rowId, runtime::SimTime time) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr || waveRow->kind != WaveRow::Kind::Signal) return {};

    const std::optional<eir::BitVector> value = valueAt(rowId, time);
    if (!value.has_value()) return QStringLiteral("—");

    return QString::fromStdString(eir::toString(value->ref(), waveRow->radix));
}

void WaveformModel::setCursorTime(runtime::SimTime time) {
    if (time == cursorTime) return;
    cursorTime = time;
    refreshValues();
}

void WaveformModel::refreshValues() {
    if (rows.isEmpty()) return;

    // The whole column at once. Working out which rows actually changed would mean reading
    // every value to find out, which is the work the refresh is trying to avoid.
    Q_EMIT dataChanged(index(0, ValueColumn), index(rowCount() - 1, ValueColumn),
                       {Qt::DisplayRole});
}

QVector<std::uint32_t> WaveformModel::displayedSignals() const {
    QVector<std::uint32_t> signalIndices;
    for (const WaveRow& waveRow : rows) {
        if (!waveRow.alive || waveRow.kind != WaveRow::Kind::Signal) continue;
        if (!signalIndices.contains(waveRow.signalIndex))
            signalIndices.push_back(waveRow.signalIndex);
    }
    return signalIndices;
}

// ---------------------------------------------------------------------------
// QAbstractItemModel
// ---------------------------------------------------------------------------

QModelIndex WaveformModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent)) return {};

    const int parentId = rowIdOf(parent);
    const QVector<int>& siblings = parentId < 0 ? roots : rows[parentId].children;
    if (row >= siblings.size()) return {};

    return createIndex(row, column, static_cast<quintptr>(siblings[row]));
}

QModelIndex WaveformModel::parent(const QModelIndex& child) const {
    const int rowId = rowIdOf(child);
    if (rowId < 0) return {};

    const int parentId = rows[rowId].parent;
    if (parentId < 0) return {};

    return createIndex(rowOf(parentId), 0, static_cast<quintptr>(parentId));
}

int WaveformModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) return 0;

    const int parentId = rowIdOf(parent);
    if (parentId < 0) return parent.isValid() ? 0 : static_cast<int>(roots.size());
    return static_cast<int>(rows[parentId].children.size());
}

int WaveformModel::columnCount(const QModelIndex& /*parent*/) const {
    return ColumnCount;
}

QVariant WaveformModel::data(const QModelIndex& index, int role) const {
    const int rowId = rowIdOf(index);
    if (rowId < 0) return {};

    const WaveRow& waveRow = rows[rowId];

    if (role == RowIdRole) return rowId;

    if (role == HighlightRole) return waveRow.highlight;

    if (role == Qt::BackgroundRole) {
        if (waveRow.highlight == 0) return {};
        return theme->color(highlightColor(waveRow.highlight));
    }

    if (role == Qt::EditRole && index.column() == NameColumn) return waveRow.label;

    if (role == Qt::ToolTipRole && waveRow.kind == WaveRow::Kind::Signal) {
        const SignalInfo* info = kernelSession->signalInfo(waveRow.signalIndex);
        return info != nullptr ? info->path : waveRow.label;
    }

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
    case NameColumn: return waveRow.label;
    case ValueColumn:
        return waveRow.kind == WaveRow::Kind::Group ? QString() : renderValue(rowId, cursorTime);
    default: return {};
    }
}

bool WaveformModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const int rowId = rowIdOf(index);
    if (rowId < 0 || role != Qt::EditRole || index.column() != NameColumn) return false;

    const QString label = value.toString();
    if (label.isEmpty()) return false;

    setLabel(rowId, label);
    return true;
}

Qt::ItemFlags WaveformModel::flags(const QModelIndex& index) const {
    // The root accepts drops, which is what makes a row draggable back out to the top level.
    if (!index.isValid()) return Qt::ItemIsDropEnabled;

    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == NameColumn) result |= Qt::ItemIsEditable;

    // Only a group accepts a drop *onto* it. Dropping *between* rows is a drop on their
    // parent, which is why the root is drop-enabled above and a signal row is not: without
    // that, dragging a row onto another signal would silently do nothing.
    const int rowId = rowIdOf(index);
    if (rowId >= 0 && rows[rowId].kind == WaveRow::Kind::Group) result |= Qt::ItemIsDropEnabled;

    return result;
}

bool WaveformModel::hasChildren(const QModelIndex& parent) const {
    if (rowCount(parent) > 0) return true;

    // A row that can still be unpacked reports children it has not made yet, which is what
    // puts the expander in front of a multi-bit signal before anybody has clicked it.
    return canFetchMore(parent);
}

bool WaveformModel::canFetchMore(const QModelIndex& parent) const {
    return canUnpack(rowIdOf(parent));
}

void WaveformModel::fetchMore(const QModelIndex& parent) {
    unpack(rowIdOf(parent));
}

Qt::DropActions WaveformModel::supportedDropActions() const {
    // Move only. A copy would put two rows over one signal with no way to tell them apart,
    // which is what the bit-range extraction of section 10.3 is for and does properly.
    return Qt::MoveAction;
}

QStringList WaveformModel::mimeTypes() const {
    return {QString::fromUtf8(RowMimeType)};
}

QMimeData* WaveformModel::mimeData(const QModelIndexList& indexes) const {
    QVector<int> rowIds;
    for (const QModelIndex& index : indexes) {
        if (index.column() != NameColumn) continue;

        const int rowId = rowIdOf(index);
        if (rowId >= 0 && !rowIds.contains(rowId)) rowIds.push_back(rowId);
    }
    if (rowIds.isEmpty()) return nullptr;

    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    for (const int rowId : rowIds) stream << rowId;

    auto* data = new QMimeData;
    data->setData(QString::fromUtf8(RowMimeType), encoded);
    return data;
}

bool WaveformModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                 int /*column*/, const QModelIndex& parent) {
    if (action == Qt::IgnoreAction) return true;
    if (data == nullptr || !data->hasFormat(QString::fromUtf8(RowMimeType))) return false;

    QByteArray encoded = data->data(QString::fromUtf8(RowMimeType));
    QDataStream stream(&encoded, QIODevice::ReadOnly);

    QVector<int> rowIds;
    while (!stream.atEnd()) {
        int rowId = -1;
        stream >> rowId;
        if (rowId >= 0) rowIds.push_back(rowId);
    }
    if (rowIds.isEmpty()) return false;

    // A drop *onto* a row arrives with a negative row number, meaning "inside, position
    // unspecified"; a drop *between* rows carries the position it landed at.
    return moveRowsTo(rowIds, rowIdOf(parent), row);
}

QVariant WaveformModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};

    switch (section) {
    case NameColumn: return tr("Signal");
    case ValueColumn: return tr("Value");
    default: return {};
    }
}

} // namespace enzan::gui
