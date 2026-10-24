// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/WaveformModel.h"

#include "gui/SignalList.h"

#include "wavedb/WaveformDatabase.h"

#include <QApplication>
#include <QClipboard>
#include <QDataStream>
#include <QIODevice>
#include <QMimeData>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <QStringList>

namespace enzan::gui {

namespace {

/// The drag payload a wave row travels in. Private to this model: rows are moved within one
/// window, and a format another application could parse would be a promise to keep it stable.
constexpr const char* RowMimeType = "application/x-enzan-wave-rows";

/// `value` as an array of `dimensions` reads: the list of its elements along the outermost,
/// `{11}{22}`, each read by the dimensions below it, or one number once the elements are single
/// bits or there are no dimensions left (DESIGN_GUIDE.md section 10.7).
QString renderShaped(const eir::BitVector& value, std::span<const eir::ArrayDimension> dimensions,
                     eir::Radix radix) {
    if (!dimensions.empty() && value.width() % dimensions.front().size() == 0) {
        const std::uint32_t count = dimensions.front().size();
        const std::uint32_t width = value.width() / count;
        if (width > 1) {
            QString rendered;
            eir::BitVector element = eir::BitVector::filled(width, eir::Logic::Zero);
            for (std::uint32_t position = 0; position < count; ++position) {
                // The left element is the highest (DESIGN_GUIDE.md section 4.3).
                const std::uint32_t low = (count - 1 - position) * width;
                for (std::uint32_t bit = 0; bit < width; ++bit)
                    element.set(bit, value.get(low + bit));
                rendered += u'{' + renderShaped(element, dimensions.subspan(1), radix) + u'}';
            }
            return rendered;
        }
    }
    return QString::fromStdString(eir::toString(value.ref(), radix));
}

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

WaveRow WaveformModel::rowFor(const SignalInfo& info) {
    WaveRow made;
    made.kind = info.isParameter() ? WaveRow::Kind::Parameter : WaveRow::Kind::Signal;
    made.label = info.name.isEmpty() ? info.path : info.name;
    made.path = info.path;
    made.signalIndex = info.signalIndex;
    made.signalWidth = info.width;
    made.dimensions = info.dimensions;
    made.constantValue = info.constantValue;
    made.localParameter = info.isLocalParameter;
    return made;
}

WaveRow WaveformModel::sliceOf(const WaveRow& source, std::uint32_t high, std::uint32_t low) {
    const std::uint32_t top = std::max(high, low);
    const std::uint32_t bottom = std::min(high, low);

    WaveRow made = source;
    made.children.clear();
    made.parent = -1;
    made.unpacked = false;
    made.dimensions.clear();
    made.sliced = true;
    made.highBit = top;
    made.lowBit = bottom;
    made.label = top == bottom ? QStringLiteral("%1[%2]").arg(source.label).arg(top)
                               : QStringLiteral("%1[%2:%3]").arg(source.label).arg(top).arg(bottom);
    return made;
}

WaveRow WaveformModel::bitOf(const WaveRow& source, std::uint32_t bit) {
    WaveRow made;
    made.kind = WaveRow::Kind::Signal;
    made.path = source.path;
    made.signalIndex = source.signalIndex;
    made.signalWidth = source.signalWidth;
    made.radix = source.radix;
    made.highlight = source.highlight;
    made.sliced = true;
    made.highBit = bit;
    made.lowBit = bit;
    made.label = QStringLiteral("%1[%2]").arg(source.label).arg(bit);
    return made;
}

WaveRow WaveformModel::elementOf(const WaveRow& source, std::uint32_t position) {
    const eir::ArrayDimension outer = source.dimensions.front();
    const auto [high, low] = bitRange(source);
    const std::uint32_t width = (high - low + 1) / outer.size();
    const std::int64_t index = outer.left >= outer.right ? std::int64_t{outer.left} - position
                                                         : std::int64_t{outer.left} + position;

    WaveRow made;
    made.kind = WaveRow::Kind::Signal;
    made.path = source.path;
    made.signalIndex = source.signalIndex;
    made.signalWidth = source.signalWidth;
    made.radix = source.radix;
    made.highlight = source.highlight;
    made.sliced = true;
    made.highBit = high - (position * width);
    made.lowBit = made.highBit - width + 1;
    made.dimensions.assign(source.dimensions.begin() + 1, source.dimensions.end());
    made.label = QStringLiteral("%1[%2]").arg(source.label).arg(index);
    return made;
}

std::vector<WaveRow> WaveformModel::unpackedRows(const WaveRow& source) {
    std::vector<WaveRow> made;
    const auto [high, low] = bitRange(source);

    if (!source.dimensions.empty() && (high - low + 1) % source.dimensions.front().size() == 0) {
        const std::uint32_t count = source.dimensions.front().size();
        made.reserve(count);
        for (std::uint32_t position = 0; position < count; ++position)
            made.push_back(elementOf(source, position));
        return made;
    }

    made.reserve(high - low + 1);
    for (std::uint32_t bit = high + 1; bit-- > low;) made.push_back(bitOf(source, bit));
    return made;
}

int WaveformModel::addSignal(const SignalInfo& info, int parentGroup) {
    WaveRow added = rowFor(info);
    added.parent = showsChildren(parentGroup) ? parentGroup : -1;

    const int rowId = static_cast<int>(rows.size());
    const int parentRow = added.parent;

    // The position is taken by value and the list is reached again after the row is stored.
    // Holding a reference into `rows` across the `push_back` below would leave it dangling the
    // moment the vector grew — which it does, silently, and then the child is appended to
    // freed memory and the row appears to have no children at all.
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
    added.parent = showsChildren(parentGroup) ? parentGroup : -1;

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

    if (std::max(high, low) >= source->signalWidth) return -1;

    WaveRow added = sliceOf(*source, high, low);

    // Placed beside the signal it came from rather than at the end, because that is where the
    // user is looking when they ask for it.
    const int parentGroup = source->parent;
    added.parent = parentGroup;
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

    notifyValueChangedUpwards(parentGroup);
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

    // Asking for a radix is asking to be shown the number, so it turns naming off. Otherwise a
    // reader picking "binary" on a state register would see no change and no reason why.
    rows[rowId].symbolic = false;
    setRadix(rowId, radix);
    for (const int child : rows[rowId].children) setRadixRecursive(child, radix);
}

void WaveformModel::setSymbolicRecursive(int rowId, bool symbolic) {
    if (row(rowId) == nullptr) return;

    rows[rowId].symbolic = symbolic;
    const QModelIndex changed = indexOfRow(rowId);
    Q_EMIT dataChanged(changed.siblingAtColumn(ValueColumn), changed.siblingAtColumn(ValueColumn),
                       {Qt::DisplayRole});
    notifyValueChangedUpwards(rowId);

    for (const int child : rows[rowId].children) setSymbolicRecursive(child, symbolic);
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

    std::vector<WaveRow> made = unpackedRows(rows[rowId]);

    beginInsertRows(indexOfRow(rowId), 0, static_cast<int>(made.size()) - 1);

    rows[rowId].unpacked = true;
    for (WaveRow& child : made) {
        child.parent = rowId;

        const int childId = static_cast<int>(rows.size());
        rows.push_back(std::move(child));
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
    if (targetGroup >= 0 && !showsChildren(targetGroup)) return false;

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
        notifyValueChangedUpwards(sourceParent);
        notifyValueChangedUpwards(targetGroup);
    }

    return true;
}

void WaveformModel::notifyValueChangedUpwards(int fromRow) {
    for (int walk = fromRow; walk >= 0; walk = rows[walk].parent) {
        const QModelIndex changed = indexOfRow(walk);
        if (!changed.isValid()) return;

        Q_EMIT dataChanged(changed.siblingAtColumn(ValueColumn),
                           changed.siblingAtColumn(ValueColumn), {Qt::DisplayRole});
    }
}

void WaveformModel::writeRow(QDataStream& stream, int rowId) const {
    const WaveRow& waveRow = rows[rowId];

    stream << static_cast<quint8>(waveRow.kind) << waveRow.label << waveRow.path
           << static_cast<quint32>(waveRow.signalIndex) << static_cast<quint32>(waveRow.signalWidth)
           << static_cast<quint8>(waveRow.radix) << waveRow.symbolic << waveRow.sliced
           << static_cast<quint32>(waveRow.highBit) << static_cast<quint32>(waveRow.lowBit)
           << static_cast<qint32>(waveRow.highlight) << waveRow.unpacked << waveRow.localParameter;

    stream << static_cast<quint32>(waveRow.dimensions.size());
    for (const eir::ArrayDimension& dimension : waveRow.dimensions)
        stream << static_cast<qint32>(dimension.left) << static_cast<qint32>(dimension.right);

    // A parameter's value travels with it, because there is nowhere for the other end to look
    // it up: it has no signal index, and the scope it came from may not even be the one the
    // reader is looking at by then.
    stream << waveRow.constantValue.has_value();
    if (waveRow.constantValue.has_value()) {
        stream << QString::fromStdString(
            eir::toString(waveRow.constantValue->ref(), eir::Radix::Binary))
               << waveRow.constantValue->width();
    }

    stream << static_cast<quint32>(waveRow.children.size());

    for (const int child : waveRow.children) writeRow(stream, child);
}

bool WaveformModel::readPending(QDataStream& stream, PendingRow& pending) {
    quint8 kind = 0;
    quint32 signalIndex = 0;
    quint32 signalWidth = 1;
    quint8 radix = 0;
    quint32 highBit = 0;
    quint32 lowBit = 0;
    qint32 highlight = 0;
    quint32 childCount = 0;

    bool hasConstant = false;
    QString constantBits;
    quint32 constantWidth = 0;

    stream >> kind >> pending.row.label >> pending.row.path >> signalIndex >> signalWidth >> radix
        >> pending.row.symbolic >> pending.row.sliced >> highBit >> lowBit >> highlight
        >> pending.row.unpacked >> pending.row.localParameter;
    if (stream.status() != QDataStream::Ok) return false;

    quint32 dimensionCount = 0;
    stream >> dimensionCount;
    if (stream.status() != QDataStream::Ok
        || std::cmp_greater(dimensionCount, stream.device()->bytesAvailable()))
        return false;
    pending.row.dimensions.resize(dimensionCount);
    for (eir::ArrayDimension& dimension : pending.row.dimensions) {
        qint32 left = 0;
        qint32 right = 0;
        stream >> left >> right;
        dimension = eir::ArrayDimension{.left = left, .right = right};
    }

    stream >> hasConstant;
    if (stream.status() != QDataStream::Ok) return false;

    if (hasConstant) {
        stream >> constantBits >> constantWidth;
        if (stream.status() != QDataStream::Ok) return false;

        pending.row.constantValue = eir::BitVector::parse(
            QStringLiteral("%1'b%2").arg(constantWidth).arg(constantBits).toStdString());
    }

    stream >> childCount;
    if (stream.status() != QDataStream::Ok) return false;

    switch (static_cast<WaveRow::Kind>(kind)) {
    case WaveRow::Kind::Group: pending.row.kind = WaveRow::Kind::Group; break;

    // A parameter that arrived without its value is not one, whatever the clipboard said. It
    // becomes a signal row over signal index zero, which shows a dash rather than a wrong
    // number — the same thing any row with nothing behind it shows.
    case WaveRow::Kind::Parameter:
        pending.row.kind = pending.row.constantValue.has_value() ? WaveRow::Kind::Parameter
                                                                 : WaveRow::Kind::Signal;
        break;

    default: pending.row.kind = WaveRow::Kind::Signal; break;
    }
    pending.row.signalIndex = signalIndex;
    pending.row.signalWidth = signalWidth;
    pending.row.radix = radix <= static_cast<quint8>(eir::Radix::Ascii)
                            ? static_cast<eir::Radix>(radix)
                            : eir::Radix::Hexadecimal;
    pending.row.highBit = highBit;
    pending.row.lowBit = lowBit;
    pending.row.highlight = std::clamp(static_cast<int>(highlight), 0, HighlightSlots);

    // A count off a clipboard is untrusted in the same way a count off a socket is: every
    // child costs bytes to describe, so one larger than what is left cannot be honest.
    if (std::cmp_greater(childCount, stream.device()->bytesAvailable())) return false;

    pending.children.resize(childCount);
    for (PendingRow& child : pending.children) {
        if (!readPending(stream, child)) return false;
    }

    return true;
}

int WaveformModel::materialise(const PendingRow& pending, int parentRow, int position) {
    WaveRow added = pending.row;
    added.children.clear();
    added.parent = parentRow;

    const int rowId = static_cast<int>(rows.size());
    rows.push_back(added);

    QVector<int>& siblings = parentRow < 0 ? roots : rows[parentRow].children;
    if (position >= 0 && position <= siblings.size()) {
        siblings.insert(position, rowId);
    } else {
        siblings.push_back(rowId);
    }

    for (const PendingRow& child : pending.children) materialise(child, rowId, -1);
    return rowId;
}

void WaveformModel::copySignalRows(const QVector<int>& rowIds) const {
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);

    quint32 count = 0;
    for (const int rowId : rowIds) {
        if (row(rowId) != nullptr) ++count;
    }

    stream << count;
    for (const int rowId : rowIds) {
        if (row(rowId) != nullptr) writeRow(stream, rowId);
    }

    // The system clipboard rather than a variable of this window's own: a copy then survives
    // whatever the reader does next, and the plain-text form below is there for whatever else
    // they paste into.
    QStringList paths;
    for (const int rowId : rowIds) {
        const WaveRow* waveRow = row(rowId);
        if (waveRow == nullptr) continue;

        const SignalInfo* info = kernelSession->signalInfo(waveRow->signalIndex);
        paths << (info != nullptr && waveRow->kind == WaveRow::Kind::Signal ? info->path
                                                                            : waveRow->label);
    }

    auto* data = new QMimeData;
    data->setData(QString::fromUtf8(RowMimeType), encoded);
    data->setText(paths.join(u'\n'));
    QApplication::clipboard()->setMimeData(data);
}

bool WaveformModel::clipboardHasRows() {
    const QMimeData* data = QApplication::clipboard()->mimeData();
    return data != nullptr && data->hasFormat(QString::fromUtf8(RowMimeType));
}

QVector<int> WaveformModel::pasteSignalRows(int targetGroup, int position) {
    QVector<int> pasted;

    const QMimeData* data = QApplication::clipboard()->mimeData();
    if (data == nullptr || !data->hasFormat(QString::fromUtf8(RowMimeType))) return pasted;

    QByteArray encoded = data->data(QString::fromUtf8(RowMimeType));
    QDataStream stream(&encoded, QIODevice::ReadOnly);

    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count == 0) return pasted;

    // Read first, insert second. A truncated clipboard then changes nothing — and, the reason
    // this is not one `beginResetModel`, the rows already in the window are never torn down,
    // so the view keeps every expansion the reader had open. Pasting one signal should not
    // cost them the arrangement they had built.
    std::vector<PendingRow> arriving(count);
    for (PendingRow& pending : arriving) {
        if (!readPending(stream, pending)) return pasted;
    }

    return insertSignalRows(arriving, targetGroup, position);
}

QVector<int> WaveformModel::insertSignalRows(const std::vector<PendingRow>& pending,
                                             int targetGroup, int position) {
    QVector<int> pasted;
    if (targetGroup >= 0 && !showsChildren(targetGroup)) targetGroup = -1;

    const QModelIndex parentIndex = targetGroup < 0 ? QModelIndex() : indexOfRow(targetGroup);
    for (const PendingRow& arriving : pending) {
        const int siblingCount = targetGroup < 0
                                     ? static_cast<int>(roots.size())
                                     : static_cast<int>(rows[targetGroup].children.size());

        // Put where the paste was aimed rather than at the end, which is what makes pasting
        // beside the row you copied do the obvious thing.
        const int at = position >= 0
                           ? std::min(position + static_cast<int>(pasted.size()), siblingCount)
                           : siblingCount;

        beginInsertRows(parentIndex, at, at);
        pasted.push_back(materialise(arriving, targetGroup, at));
        endInsertRows();
    }

    notifyValueChangedUpwards(targetGroup);
    Q_EMIT displayedSignalsChanged();
    return pasted;
}

// ---------------------------------------------------------------------------
// Signal list files
// ---------------------------------------------------------------------------

QString WaveformModel::writeSignalList() const {
    // Nothing that varies between saves — no date, no user — so that saving an unchanged window
    // writes the same bytes (DESIGN_GUIDE.md section 10.8).
    QString text = QStringLiteral("# Enzan signal list (DESIGN_GUIDE.md section 10.8)\n");
    for (const int root : roots) writeSignalListRow(text, root, 0);
    return text;
}

bool WaveformModel::childrenAreAsUnpacked(int rowId) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr || !waveRow->unpacked) return false;

    const std::vector<WaveRow> made = unpackedRows(*waveRow);
    if (std::cmp_not_equal(made.size(), waveRow->children.size())) return false;

    for (std::size_t position = 0; position < made.size(); ++position) {
        const WaveRow& child = rows[waveRow->children[static_cast<qsizetype>(position)]];
        const WaveRow& expected = made[position];
        if (child.kind != expected.kind || child.signalIndex != expected.signalIndex
            || !child.sliced || child.highBit != expected.highBit || child.lowBit != expected.lowBit
            || child.label != expected.label || child.path != expected.path
            || child.radix != expected.radix || child.symbolic != expected.symbolic
            || child.highlight != expected.highlight || child.dimensions != expected.dimensions
            || child.unpacked)
            return false;
    }
    return true;
}

void WaveformModel::writeSignalListRow(QString& text, int rowId, int depth) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr) return;

    const QString indent(static_cast<qsizetype>(depth) * 4, u' ');

    // A body is written in braces with its rows indented beneath, and an empty one as `{}`.
    const auto body = [this, &indent, depth](const QVector<int>& children) {
        if (children.isEmpty()) return QStringLiteral("{}");
        QString inner = QStringLiteral("{\n");
        for (const int child : children) writeSignalListRow(inner, child, depth + 1);
        return inner + indent + u'}';
    };

    // The radix as the menu sets it: choosing one turns the names off, and `-symbolic` turns
    // them back on. A row as it was added — hexadecimal, names on — needs neither.
    QStringList words;
    if (!waveRow->symbolic) {
        words << QStringLiteral("-radix") << radixWord(waveRow->radix);
    } else if (waveRow->radix != eir::Radix::Hexadecimal) {
        words << QStringLiteral("-radix") << radixWord(waveRow->radix)
              << QStringLiteral("-symbolic");
    }

    if (waveRow->kind == WaveRow::Kind::Group) {
        if (waveRow->highlight != 0)
            words << QStringLiteral("-highlight") << QString::number(waveRow->highlight);
        words.prepend(QStringLiteral("group"));
        words << quoteSignalListWord(waveRow->label) << body(waveRow->children);
        text += indent + words.join(u' ') + u'\n';
        return;
    }

    QString path = waveRow->path;
    if (path.isEmpty() && waveRow->kind == WaveRow::Kind::Signal) {
        if (const SignalInfo* info = kernelSession->signalInfo(waveRow->signalIndex))
            path = info->path;
    }

    // A row that cannot be named cannot be written; leaving it out beats writing a line that
    // loads as something else.
    if (path.isEmpty()) return;

    QString leaf = path.mid(path.lastIndexOf(u'/') + 1);
    if (leaf.isEmpty()) leaf = path;
    QString range;
    if (waveRow->sliced) {
        range = waveRow->highBit == waveRow->lowBit
                    ? QStringLiteral("[%1]").arg(waveRow->highBit)
                    : QStringLiteral("[%1:%2]").arg(waveRow->highBit).arg(waveRow->lowBit);
    }

    // The label only when the reader changed it: one the window would have made again is not
    // worth a word, and leaving it out is what keeps a hand-written list short.
    if (waveRow->label != leaf + range)
        words << QStringLiteral("-label") << quoteSignalListWord(waveRow->label);
    if (waveRow->highlight != 0)
        words << QStringLiteral("-highlight") << QString::number(waveRow->highlight);

    if (waveRow->unpacked) {
        if (childrenAreAsUnpacked(rowId)) {
            words << QStringLiteral("-expand");
        } else {
            words << QStringLiteral("-contents") << body(waveRow->children);
        }
    }

    words.prepend(QStringLiteral("add wave"));
    words << quoteSignalListWord(path + range);
    text += indent + words.join(u' ') + u'\n';
}

void WaveformModel::detach(int rowId) {
    const int position = rowOf(rowId);
    if (position >= 0) siblingsOf(rowId).removeAt(position);
}

void WaveformModel::moveToGroup(int rowId, int group) {
    // Delegated rather than duplicated. The older implementation here reset the model, which
    // made the view forget every expansion in the window — so collecting two signals into a
    // group packed every other bus on screen.
    static_cast<void>(moveRowsTo({rowId}, group, -1));
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

bool WaveformModel::showsChildren(int rowId) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr) return false;

    // `unpacked` rather than "has children", so that a bus whose bits have *all* been deleted
    // shows nothing rather than quietly reverting to the whole value the reader just took
    // apart. A group always shows its children, which is what a group is.
    return waveRow->kind == WaveRow::Kind::Group || waveRow->unpacked;
}

std::uint32_t WaveformModel::displayedWidth(int rowId) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr) return 0;

    if (showsChildren(rowId)) {
        std::uint32_t total = 0;
        for (const int child : waveRow->children) total += displayedWidth(child);
        return total;
    }

    if (waveRow->kind == WaveRow::Kind::Parameter) {
        return waveRow->constantValue.has_value() ? waveRow->constantValue->width() : 0;
    }

    if (waveRow->kind != WaveRow::Kind::Signal) return 0;

    const auto [high, low] = bitRange(*waveRow);
    return high - low + 1;
}

bool WaveformModel::childrenReconstituteParent(int rowId) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr || waveRow->kind != WaveRow::Kind::Signal) return false;
    if (waveRow->children.isEmpty()) return false;

    const auto [high, low] = bitRange(*waveRow);
    std::uint32_t expected = high;

    for (int position = 0; position < waveRow->children.size(); ++position) {
        const int childId = waveRow->children[position];
        const WaveRow* child = row(childId);
        if (child == nullptr || child->kind != WaveRow::Kind::Signal) return false;
        if (child->signalIndex != waveRow->signalIndex) return false;
        if (!child->sliced || child->highBit != expected || child->lowBit > child->highBit)
            return false;

        // An element is whole when it shows its own bits, unpacked or not: an element taken
        // apart and put back is still the element.
        if (!child->children.isEmpty() && !childrenReconstituteParent(childId)) return false;
        if (showsChildren(childId) && child->children.isEmpty()) return false;

        if (child->lowBit == low) return position == waveRow->children.size() - 1;
        if (child->lowBit < low) return false;
        expected = child->lowBit - 1;
    }

    return false;
}

std::optional<eir::BitVector> WaveformModel::combinedValueAt(int rowId,
                                                             runtime::SimTime time) const {
    const WaveRow& waveRow = rows[rowId];

    // Children of any width join. Whether the result *reads* as one number or as a list of
    // them is `renderValue`'s business; here it is simply all of their bits in order, which is
    // what the trace needs in order to know whether anything in the row is unknown.
    QVector<eir::BitVector> parts;
    parts.reserve(waveRow.children.size());

    std::uint32_t total = 0;
    for (const int child : waveRow.children) {
        const std::optional<eir::BitVector> value = valueAt(child, time);
        if (!value.has_value()) return std::nullopt;

        total += value.value().width();
        parts.push_back(value.value());
    }

    if (parts.isEmpty()) return std::nullopt;

    // The first child is the most significant, which is the order the list shows them in and
    // the order unpacking creates them in.
    eir::BitVector joined = eir::BitVector::filled(total, eir::Logic::Zero);

    std::uint32_t bit = total;
    for (const eir::BitVector& part : parts) {
        for (std::uint32_t index = part.width(); index-- > 0;) {
            joined.set(--bit, part.get(index));
        }
    }

    return joined;
}

std::optional<eir::BitVector> WaveformModel::valueAt(int rowId, runtime::SimTime time) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr) return std::nullopt;

    // A row with children shows what its children show, joined. Skipped when the children are
    // simply the row's own bits with none missing, because the answer is then the row's own
    // value and reading it directly is cheaper.
    if (showsChildren(rowId) && !childrenReconstituteParent(rowId)) {
        return combinedValueAt(rowId, time);
    }

    // A parameter is the same at every time, which is the whole of what makes it one.
    if (waveRow->kind == WaveRow::Kind::Parameter) return waveRow->constantValue;

    if (waveRow->kind != WaveRow::Kind::Signal) return std::nullopt;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();
    const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) return std::nullopt;

    const std::optional<eir::BitVector> whole = database.valueAt(record, time);
    if (!whole.has_value()) return std::nullopt;

    return slice(*waveRow, *whole);
}

bool WaveformModel::childrenAreBits(int rowId) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr || waveRow->children.isEmpty()) return false;

    return std::ranges::all_of(waveRow->children,
                               [this](int child) { return displayedWidth(child) == 1; });
}

QString WaveformModel::renderValue(int rowId, runtime::SimTime time) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr) return {};

    // A row whose children are wider than a bit reads as the list of them. Joining a vector of
    // vectors into one number files off the boundaries that are the whole reason it has that
    // shape; a list keeps them, and nests when the children are lists themselves.
    if (showsChildren(rowId) && !childrenReconstituteParent(rowId) && !childrenAreBits(rowId)
        && !waveRow->children.isEmpty()) {
        QString rendered;
        for (const int child : waveRow->children) {
            rendered += u'{' + renderValue(child, time) + u'}';
        }
        return rendered;
    }

    const std::optional<eir::BitVector> value = valueAt(rowId, time);
    if (!value.has_value()) {
        // A group with nothing joinable in it shows nothing at all, rather than the dash that
        // means "this signal has no value yet": they are different situations. A parameter
        // always has a value, so it reaches neither.
        return waveRow->kind == WaveRow::Kind::Group ? QString() : QStringLiteral("—");
    }

    if (const EnumInfo* enumeration = enumerationOf(rowId); enumeration != nullptr) {
        // A value the enumeration does not name falls back to the number rather than to
        // nothing. That is what an X is, and what an encoding outside the declared set is, and
        // both are things the reader needs to see rather than be shielded from.
        const QString named = enumeration->nameOf(*value);
        if (!named.isEmpty()) return named;
    }

    // An array reads as the list of its elements whether it is unpacked or not, as its unpacked
    // rows read joined: one number for an array of vectors would file off exactly the
    // boundaries that are the reason it has that shape.
    return renderShaped(*value, waveRow->dimensions, waveRow->radix);
}

const EnumInfo* WaveformModel::enumerationOf(int rowId) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr || !waveRow->symbolic) return nullptr;
    if (waveRow->kind != WaveRow::Kind::Signal || waveRow->sliced) return nullptr;

    // An unpacked bus still shows its own whole value on its own line, so it keeps its names;
    // one that has lost a bit shows what is left, which no enumeration describes.
    if (showsChildren(rowId) && !childrenReconstituteParent(rowId)) return nullptr;

    const SignalInfo* info = kernelSession->signalInfo(waveRow->signalIndex);
    return info == nullptr ? nullptr : info->enumeration.get();
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

runtime::SimTime WaveformModel::nextRawEdgeAfter(int rowId, runtime::SimTime time) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr) return runtime::NoTime;

    if (showsChildren(rowId)) {
        runtime::SimTime soonest = runtime::NoTime;
        for (const int child : waveRow->children) {
            soonest = std::min(soonest, nextRawEdgeAfter(child, time));
        }
        return soonest;
    }

    if (waveRow->kind != WaveRow::Kind::Signal) return runtime::NoTime;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();
    const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) return runtime::NoTime;

    const std::uint64_t change = database.nextChangeAfter(record, time);
    return change == wavedb::NoChange ? runtime::NoTime : database.timeOfChange(record, change);
}

runtime::SimTime WaveformModel::previousRawEdgeBefore(int rowId, runtime::SimTime time) const {
    const WaveRow* waveRow = row(rowId);
    if (waveRow == nullptr) return runtime::NoTime;

    if (showsChildren(rowId)) {
        runtime::SimTime latest = runtime::NoTime;
        for (const int child : waveRow->children) {
            const runtime::SimTime edge = previousRawEdgeBefore(child, time);
            if (edge != runtime::NoTime && (latest == runtime::NoTime || edge > latest)) {
                latest = edge;
            }
        }
        return latest;
    }

    if (waveRow->kind != WaveRow::Kind::Signal) return runtime::NoTime;

    const wavedb::WaveformDatabase& database = kernelSession->waveforms();
    const std::uint32_t record = database.recordIndexOf(waveRow->signalIndex);
    if (record == wavedb::WaveformDatabase::NotRecorded) return runtime::NoTime;

    const std::uint64_t change = database.previousChangeBefore(record, time);
    return change == wavedb::NoChange ? runtime::NoTime : database.timeOfChange(record, change);
}

bool WaveformModel::isEdgeAt(int rowId, runtime::SimTime time) const {
    // An edge is a time at which the row's value differs from the value an instant earlier —
    // not one at which it differs from wherever the cursor happens to be. Asking the second
    // question makes stepping backwards skip the transition the cursor is standing just after,
    // because the value there is the one that transition produced.
    if (time == 0) return valueAt(rowId, 0).has_value();

    return valueAt(rowId, time) != valueAt(rowId, time - 1);
}

runtime::SimTime WaveformModel::nextEdgeAfter(int rowId, runtime::SimTime time) const {
    runtime::SimTime candidate = nextRawEdgeAfter(rowId, time);
    for (int examined = 0; candidate != runtime::NoTime && examined < EdgeSearchBudget;
         ++examined) {
        if (isEdgeAt(rowId, candidate)) return candidate;
        candidate = nextRawEdgeAfter(rowId, candidate);
    }

    return candidate;
}

runtime::SimTime WaveformModel::previousEdgeBefore(int rowId, runtime::SimTime time) const {
    runtime::SimTime candidate = previousRawEdgeBefore(rowId, time);
    for (int examined = 0; candidate != runtime::NoTime && examined < EdgeSearchBudget;
         ++examined) {
        if (isEdgeAt(rowId, candidate)) return candidate;
        candidate = previousRawEdgeBefore(rowId, candidate);
    }

    return candidate;
}

runtime::SimTime WaveformModel::nearestEdge(int rowId, runtime::SimTime time) const {
    const runtime::SimTime later = nextEdgeAfter(rowId, time);
    const runtime::SimTime earlier = previousEdgeBefore(rowId, time);

    if (later == runtime::NoTime) return earlier;
    if (earlier == runtime::NoTime) return later;

    // A tie goes to the earlier one, so that which side of an edge a click rounded to does not
    // decide the answer.
    return time - earlier <= later - time ? earlier : later;
}

void WaveformModel::setCursorTime(runtime::SimTime time) {
    if (time == cursorTime) return;
    cursorTime = time;
    refreshValues();
}

void WaveformModel::refreshValues() {
    if (rows.isEmpty()) return;
    refreshValuesUnder(QModelIndex());
}

void WaveformModel::refreshValuesUnder(const QModelIndex& parent) {
    const int count = rowCount(parent);
    if (count == 0) return;

    // The whole column at once. Working out which rows actually changed would mean reading
    // every value to find out, which is the work the refresh is trying to avoid. One range per
    // parent, because a range names rows under one parent only: the rows inside a group or an
    // unpacked signal would otherwise keep the value they showed until something else redrew
    // them.
    Q_EMIT dataChanged(index(0, ValueColumn, parent), index(count - 1, ValueColumn, parent),
                       {Qt::DisplayRole});
    for (int position = 0; position < count; ++position)
        refreshValuesUnder(index(position, 0, parent));
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
        if (info == nullptr) return waveRow.label;
        if (info->recording != ipc::Recording::TooWide) return info->path;

        // Its trace begins when it was added, because the kernel kept nothing of it before.
        return tr("%1\nWider than the %2 bits the kernel records, so shown only from when it "
                  "was added; --wave-max-bits raises the limit")
            .arg(info->path)
            .arg(kernelSession->widthLimit());
    }

    // A parameter says which kind it is, because a row that never changes looks like a signal
    // nobody has driven until something tells the reader otherwise.
    if (role == Qt::ToolTipRole && waveRow.kind == WaveRow::Kind::Parameter) {
        return waveRow.localParameter ? tr("localparam %1").arg(waveRow.label)
                                      : tr("parameter %1").arg(waveRow.label);
    }

    if (role != Qt::DisplayRole) return {};

    switch (index.column()) {
    case NameColumn: return waveRow.label;
    case ValueColumn: return renderValue(rowId, cursorTime);
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

    // Any row that shows its children accepts them, which is a group and an unpacked vector
    // alike: a vector is a group whose children happen to have been made for it, and one that
    // let rows be dragged out but not back in would not be the same kind of thing at all.
    // Dropping *between* rows is a drop on their parent, which is why the root is drop-enabled
    // above and a packed row — which has no children to drop among — is not.
    const int rowId = rowIdOf(index);
    if (showsChildren(rowId)) result |= Qt::ItemIsDropEnabled;

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
