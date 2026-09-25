// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/DesignTreeModel.h"

namespace enzan::gui {

DesignTreeModel::DesignTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void DesignTreeModel::setInstances(const QVector<ipc::InstanceEntry>& instances) {
    beginResetModel();

    nodes.clear();
    roots.clear();
    nodes.reserve(instances.size());

    for (const ipc::InstanceEntry& entry : instances) {
        Node node;
        node.name = QString::fromStdString(entry.name);
        node.path = QString::fromStdString(entry.path);
        node.parent =
            entry.parent == ipc::InstanceEntry::NoParent ? -1 : static_cast<int>(entry.parent);
        nodes.push_back(node);
    }

    // A second pass, because a parent index may refer to an entry this one has not reached
    // yet. The kernel sends elaboration order, which is depth-first, so in practice it never
    // does — but a model that depended on that would break the first time the order changed.
    for (int index = 0; index < nodes.size(); ++index) {
        const int parentIndex = nodes[index].parent;
        if (parentIndex >= 0 && parentIndex < nodes.size()) {
            nodes[parentIndex].children.push_back(index);
        } else {
            nodes[index].parent = -1;
            roots.push_back(index);
        }
    }

    endResetModel();
}

int DesignTreeModel::rowOf(int node) const {
    const QVector<int>& siblings =
        nodes[node].parent < 0 ? roots : nodes[nodes[node].parent].children;
    return static_cast<int>(siblings.indexOf(node));
}

QModelIndex DesignTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent)) return {};

    const QVector<int>& siblings =
        parent.isValid() ? nodes[static_cast<int>(parent.internalId())].children : roots;
    if (row >= siblings.size()) return {};

    return createIndex(row, column, static_cast<quintptr>(siblings[row]));
}

QModelIndex DesignTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};

    const int node = static_cast<int>(child.internalId());
    if (node < 0 || node >= nodes.size()) return {};

    const int parentIndex = nodes[node].parent;
    if (parentIndex < 0) return {};

    return createIndex(rowOf(parentIndex), 0, static_cast<quintptr>(parentIndex));
}

int DesignTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) return 0;
    if (!parent.isValid()) return static_cast<int>(roots.size());

    const int node = static_cast<int>(parent.internalId());
    if (node < 0 || node >= nodes.size()) return 0;
    return static_cast<int>(nodes[node].children.size());
}

int DesignTreeModel::columnCount(const QModelIndex& /*parent*/) const {
    return 1;
}

QVariant DesignTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};

    const int node = static_cast<int>(index.internalId());
    if (node < 0 || node >= nodes.size()) return {};

    switch (role) {
    case Qt::DisplayRole: return nodes[node].name;
    case Qt::ToolTipRole:
    case PathRole: return nodes[node].path;
    default: return {};
    }
}

QVariant DesignTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section != 0) return {};
    return tr("Instance");
}

QString DesignTreeModel::pathAt(const QModelIndex& index) const {
    return data(index, PathRole).toString();
}

QModelIndex DesignTreeModel::indexOfPath(const QString& path) const {
    for (int node = 0; node < nodes.size(); ++node) {
        if (nodes[node].path != path) continue;
        return createIndex(rowOf(node), 0, static_cast<quintptr>(node));
    }
    return {};
}

} // namespace enzan::gui
