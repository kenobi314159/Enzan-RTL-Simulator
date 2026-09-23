// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/KernelSession.h"
#include "gui/Theme.h"
#include "runtime/Time.h"

#include <QAbstractItemModel>
#include <QString>
#include <QVector>

class QDataStream;

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

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
/// **A row with children shows what its children show, joined together** (section 10.7). One
/// rule covers three things that would otherwise be three special cases: a bus that has been
/// unpacked and had some of its bits deleted shows what is left of it; a group of one-bit
/// signals behaves as a vector; and a group of equal-width vectors is a dimension above that,
/// which is what makes multi-dimensional arrays fall out of nesting rather than needing their
/// own mechanism. A group whose children are not all the same width shows nothing, because a
/// number joined out of unrelated signals is noise rather than information.
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

    /// True when a row's value comes from its children rather than from a signal of its own.
    [[nodiscard]] bool showsChildren(int rowId) const;

    /// Bits the row displays: its own, or its children's added up when it shows theirs.
    [[nodiscard]] std::uint32_t displayedWidth(int rowId) const;

    /// True when a row's children are exactly its own bits, all of them, in order.
    ///
    /// The state a bus is in straight after unpacking. Its joined value is then its own value,
    /// so it can be drawn from its own trace instead of by merging every child's — which is
    /// what stops unpacking a wide bus from costing as much again to draw.
    [[nodiscard]] bool childrenReconstituteParent(int rowId) const;

    /// Copies rows, and everything below them, to the clipboard.
    void copyRows(const QVector<int>& rowIds) const;

    /// Pastes the clipboard into `targetGroup` at `position`, or at the end when negative.
    /// Returns the rows created, in the order they were added.
    QVector<int> pasteRows(int targetGroup, int position);

    /// True when the clipboard holds rows this window put there.
    [[nodiscard]] static bool clipboardHasRows();

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

    /// Renders what a row shows at `time`, or a dash when there is nothing to show.
    ///
    /// A row whose children are single bits renders as one number, which is what makes an
    /// unpacked bus read as the bus. A row whose children are wider than a bit renders as the
    /// list of them, `{001122}{112233}`, and because that recurses a group of such groups
    /// reads `{{11}{22}}{{22}{33}}` — the shape of the thing rather than a number with the
    /// boundaries filed off (DESIGN_GUIDE.md section 10.7).
    [[nodiscard]] QString renderValue(int rowId, runtime::SimTime time) const;

    /// The next time after `time` at which anything the row shows changes, or `NoTime`.
    ///
    /// Defined for every row, not only for one backed by a signal: a group shows a value on
    /// its line, so it has edges to step between like anything else. A row's edges are its
    /// children's put together, and a candidate at which the row's *displayed* value did not
    /// actually change is skipped — a bit of a counter must not stop at every count.
    [[nodiscard]] runtime::SimTime nextEdgeAfter(int rowId, runtime::SimTime time) const;

    [[nodiscard]] runtime::SimTime previousEdgeBefore(int rowId, runtime::SimTime time) const;

    /// The row's edge closest to `time`, or `NoTime` when it has none.
    [[nodiscard]] runtime::SimTime nearestEdge(int rowId, runtime::SimTime time) const;

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

    /// The value a row's children make between them, or nothing when they cannot make one.
    [[nodiscard]] std::optional<eir::BitVector> combinedValueAt(int rowId,
                                                                runtime::SimTime time) const;

    /// True when every child of `rowId` shows exactly one bit, which is when the row reads as
    /// a number rather than as a list.
    [[nodiscard]] bool childrenAreBits(int rowId) const;

    /// True when the row's value at `time` differs from its value an instant earlier.
    [[nodiscard]] bool isEdgeAt(int rowId, runtime::SimTime time) const;

    /// The next or previous time at which *something* in the row changes, without checking
    /// whether what the row displays changed with it.
    [[nodiscard]] runtime::SimTime nextRawEdgeAfter(int rowId, runtime::SimTime time) const;

    [[nodiscard]] runtime::SimTime previousRawEdgeBefore(int rowId, runtime::SimTime time) const;

    /// How many candidate edges are examined before the search gives up and stops at the last
    /// one it looked at.
    ///
    /// A row showing a slice of a signal changes less often than the signal does, and there is
    /// no index of when a single bit turns over — so the search walks the signal's transitions.
    /// The budget keeps one keypress bounded on a trace with millions of them; past it the
    /// cursor lands on a real transition of something in the row, which is a worse answer than
    /// the exact one and a far better answer than a frozen window.
    static constexpr int EdgeSearchBudget = 512;

    /// Tells `fromRow` and everything above it that the value they show has changed.
    ///
    /// A row's value is part of every value above it (section 10.7), so a structural change
    /// anywhere below has to travel up. Without this, deleting a bit would leave its bus
    /// showing the value it had while the bit was still there.
    void notifyValueChangedUpwards(int fromRow);

    /// A subtree read off the clipboard and not yet in the model.
    ///
    /// Reading and inserting are separate so that a truncated clipboard changes nothing, and
    /// so that inserting can be done row by row: a reset would make the view forget every
    /// expansion in the window, which is not what pasting one signal should cost.
    struct PendingRow {
        WaveRow row;
        std::vector<PendingRow> children;
    };

    void writeRow(QDataStream& stream, int rowId) const;

    /// Reads one subtree. Returns false when the clipboard ran out or made no sense.
    static bool readPending(QDataStream& stream, PendingRow& pending);

    /// Puts a subtree into the model under `parentRow` at `position`, or at the end when that
    /// is negative. Returns the row created.
    int materialise(const PendingRow& pending, int parentRow, int position);

    const KernelSession* kernelSession;
    const Theme* theme;
    QVector<WaveRow> rows;
    QVector<int> roots;
    runtime::SimTime cursorTime = 0;
};

} // namespace enzan::gui
