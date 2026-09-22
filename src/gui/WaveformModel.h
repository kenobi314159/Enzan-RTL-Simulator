// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/KernelSession.h"
#include "gui/Theme.h"
#include "runtime/Time.h"

#include <QAbstractItemModel>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>
#include <utility>

namespace enzan::gui {

/// One row of the wave window: a trace, or a group holding traces.
struct WaveRow {
    enum class Kind : std::uint8_t { Group, Signal };

    Kind kind = Kind::Signal;

    /// What the row is labelled with. A signal's label starts as its leaf name and may be
    /// renamed, because a user watching four `q` signals from four instances needs to tell
    /// them apart.
    QString label;

    std::uint32_t signalIndex = 0;

    /// Full width of the underlying signal, whatever this row displays of it.
    std::uint32_t signalWidth = 1;

    eir::Radix radix = eir::Radix::Hexadecimal;

    /// Set when this row shows only part of the signal.
    ///
    /// Section 10.3 requires extracting a bit range into a separately displayed signal. It is a
    /// property of the row rather than a new trace, so the kernel streams the signal once
    /// however many slices of it are on screen.
    bool sliced = false;
    std::uint32_t highBit = 0;
    std::uint32_t lowBit = 0;

    /// Background slot 1 through `HighlightSlots`, or 0 for the default (section 10.7).
    int highlight = 0;

    /// True once a signal row's bits have been created as children below it.
    bool unpacked = false;

    int parent = -1;
    QVector<int> children;

    /// False for a row whose underlying storage was removed; such rows are skipped.
    bool alive = true;
};

/// The contents of the Wave window.
///
/// A tree rather than a list, because grouping is one of section 10.3's required features and a
/// group that could not be collapsed would not earn its place. Rows are held in one flat vector
/// with parent and child indices: a `QModelIndex` then carries a stable row number rather than
/// a pointer, so adding a signal does not invalidate the indices of the ones already there.
///
/// The model holds only what to draw. The values come from the session's database, which is why
/// nothing here is copied out of it: a trace with a million transitions is drawn from the
/// database's index, and a copy per row would defeat that entirely.
class WaveformModel : public QAbstractItemModel {
    Q_OBJECT

public:
    // Plain constants rather than an enumeration: these are column *numbers* passed to Qt's
    // `int` interfaces, and an enumeration would only earn a cast at every use.
    static constexpr int NameColumn = 0;
    static constexpr int ValueColumn = 1;
    static constexpr int ColumnCount = 2;

    /// The row number behind an index, for `data(..., RowIdRole)`.
    static constexpr int RowIdRole = Qt::UserRole + 1;

    /// The row's highlight slot, for `data(..., HighlightRole)`.
    static constexpr int HighlightRole = Qt::UserRole + 2;

    /// `session` and `theme` must both outlive the model.
    ///
    /// The theme is here because Qt asks a model for `Qt::BackgroundRole`, and section 10.5
    /// allows exactly one place for a colour to come from.
    WaveformModel(const KernelSession& session, const Theme& theme, QObject* parent = nullptr);

    // --- editing -----------------------------------------------------------

    /// Adds a trace, inside `parentGroup` when it names one. Returns its row number.
    int addSignal(const SignalInfo& info, int parentGroup = -1);

    /// Adds an empty group. Returns its row number.
    int addGroup(const QString& name, int parentGroup = -1);

    /// Adds a row showing bits `[high:low]` of a signal already displayed by `rowId`.
    ///
    /// Returns the new row, or -1 when the range is not inside the signal.
    int addSlice(int rowId, std::uint32_t high, std::uint32_t low);

    /// Removes a row and, for a group, everything in it.
    void removeRow(int rowId);

    void clear();

    void setRadix(int rowId, eir::Radix radix);

    /// Sets the radix of a group's contents, or of one row.
    void setRadixRecursive(int rowId, eir::Radix radix);

    void setLabel(int rowId, const QString& label);

    /// Sets a row's background to highlight slot `slot`, or to the default with 0.
    void setHighlight(int rowId, int slot);

    /// True when a row shows more than one bit and has not been unpacked yet.
    [[nodiscard]] bool canUnpack(int rowId) const;

    /// Creates one child row per bit of `rowId`, most significant first.
    ///
    /// Unpacking is a property of the display: the bits are rows over the same signal, so the
    /// kernel still streams it once however many of them are on screen. Arrays gain further
    /// levels later, which is why this hangs children off a row rather than being a special
    /// case of the top level.
    void unpack(int rowId);

    /// Moves rows to `targetGroup` — -1 for the top level — inserting at `position`, or at the
    /// end when `position` is negative. Returns false when the move would make a cycle.
    bool moveRowsTo(const QVector<int>& rowIds, int targetGroup, int position);

    /// Moves `rowId` into `group`, or to the top level when `group` is -1.
    void moveToGroup(int rowId, int group);

    /// The bits a row shows, high first. Equal for a single bit, and the whole signal for a
    /// row that has not been sliced.
    [[nodiscard]] static std::pair<std::uint32_t, std::uint32_t> bitRange(const WaveRow& waveRow);

    // --- reading -----------------------------------------------------------

    [[nodiscard]] const WaveRow* row(int rowId) const;

    /// The row number an index refers to, or -1.
    [[nodiscard]] int rowIdOf(const QModelIndex& index) const;

    /// A model index for `rowId`, or an invalid index.
    [[nodiscard]] QModelIndex indexOfRow(int rowId) const;

    /// Every signal displayed anywhere, without duplicates, in row order.
    [[nodiscard]] QVector<std::uint32_t> displayedSignals() const;

    /// The value a row shows at `time`, already narrowed to its bit range.
    [[nodiscard]] std::optional<eir::BitVector> valueAt(int rowId, runtime::SimTime time) const;

    /// Renders what `valueAt` returns, or a dash when there is nothing to show.
    [[nodiscard]] QString renderValue(int rowId, runtime::SimTime time) const;

    /// Narrows a whole-signal value to the row's bit range. Returns it unchanged when the row
    /// shows the whole signal.
    [[nodiscard]] static eir::BitVector slice(const WaveRow& waveRow, const eir::BitVector& value);

    /// Changes the time the value column reads at.
    void setCursorTime(runtime::SimTime time);

    void refreshValues();

    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

    [[nodiscard]] bool hasChildren(const QModelIndex& parent = {}) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;

Q_SIGNALS:
    /// The set of displayed signals changed, so the subscription should be brought up to date.
    void displayedSignalsChanged();

private:
    [[nodiscard]] int rowOf(int rowId) const;

    [[nodiscard]] QVector<int>& siblingsOf(int rowId);

    void detach(int rowId);

    /// Marks a row and its descendants dead and takes them out of the tree.
    void killSubtree(int rowId);

    /// True when `candidate` is `rowId` or lies below it.
    [[nodiscard]] bool isDescendantOf(int candidate, int rowId) const;

    const KernelSession* kernelSession;
    const Theme* theme;
    QVector<WaveRow> rows;
    QVector<int> roots;
    runtime::SimTime cursorTime = 0;
};

} // namespace enzan::gui
