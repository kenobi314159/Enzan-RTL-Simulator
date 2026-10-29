// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/KernelSession.h"
#include "runtime/Time.h"

#include <QAbstractTableModel>
#include <QString>
#include <QVector>

#include <cstdint>

namespace enzan::gui {

/// The signals and variables of one scope, for the Objects window.
///
/// The value column shows what the signal holds **at the active cursor**, not at the present
/// moment. That is the behaviour a debugger needs: the user puts a cursor on the edge where
/// something went wrong and reads the state there, and a column that always showed the end of
/// the run would make the window useless for the thing it is for.
class ObjectsModel : public QAbstractTableModel {
    Q_OBJECT

public:
    // Plain constants rather than an enumeration: these are column *numbers* passed to Qt's
    // `int` interfaces, and an enumeration would only earn a cast at every use.
    static constexpr int NameColumn = 0;
    static constexpr int ValueColumn = 1;
    static constexpr int TypeColumn = 2;
    static constexpr int ColumnCount = 3;

    /// The kernel's signal index, for `data(..., SignalIndexRole)`.
    static constexpr int SignalIndexRole = Qt::UserRole + 1;

    /// `session` must outlive the model.
    explicit ObjectsModel(const KernelSession& session, QObject* parent = nullptr);

    void setObjects(const QString& scopePath, const QVector<SignalInfo>& objects);

    /// Changes the time the value column reads at, and redraws it.
    void setCursorTime(runtime::SimTime time);

    /// Redraws the value column, for when new values have arrived.
    void refreshValues();

    /// The radix every value is shown in. Hexadecimal by default.
    void setRadix(eir::Radix radix);

    [[nodiscard]] eir::Radix radix() const noexcept { return valueRadix; }

    [[nodiscard]] const QString& scope() const noexcept { return scopePath; }

    [[nodiscard]] const SignalInfo* objectAt(int row) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

private:
    [[nodiscard]] QString valueOf(const SignalInfo& info) const;

    const KernelSession* kernelSession;
    QString scopePath;
    QVector<SignalInfo> objects;
    runtime::SimTime cursorTime = 0;
    eir::Radix valueRadix = eir::Radix::Hexadecimal;
};

} // namespace enzan::gui
