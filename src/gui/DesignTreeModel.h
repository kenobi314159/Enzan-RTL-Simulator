// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ipc/Protocol.h"

#include <QAbstractItemModel>
#include <QString>
#include <QVector>

#include <cstdint>

namespace enzan::gui {

/// The design hierarchy, for the Structure window.
///
/// Built from the flat, parent-indexed list the kernel sends rather than from nested messages,
/// because the tree is proportional to the design's hierarchy and not to its state: sending it
/// whole costs one round trip, and a lazily-expanding tree would make the window's contents
/// depend on the order the user clicked in.
class DesignTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    /// The hierarchical path of an instance, for `data(..., PathRole)`.
    static constexpr int PathRole = Qt::UserRole + 1;

    explicit DesignTreeModel(QObject* parent = nullptr);

    /// Replaces the tree.
    void setInstances(const QVector<ipc::InstanceEntry>& instances);

    /// The index of the instance at `path`, or an invalid index.
    [[nodiscard]] QModelIndex indexOfPath(const QString& path) const;

    /// The hierarchical path an index names, or an empty string.
    [[nodiscard]] QString pathAt(const QModelIndex& index) const;

    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

private:
    struct Node {
        QString name;
        QString path;
        int parent = -1;
        QVector<int> children;
    };

    /// Position of `node` among its parent's children.
    [[nodiscard]] int rowOf(int node) const;

    QVector<Node> nodes;
    QVector<int> roots;
};

} // namespace enzan::gui
