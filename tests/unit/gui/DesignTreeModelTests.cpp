// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the Structure window's model.
//
// The tree arrives as a flat, parent-indexed list, and turning that back into a tree is the kind
// of code that works on the shape it was written against and quietly breaks on another. The
// cases below are the shapes a real design produces: nesting, siblings, and an entry whose
// parent has not been seen yet.

#include "gui/DesignTreeModel.h"

#include "ipc/Protocol.h"

#include <gtest/gtest.h>

#include <QVector>

namespace {

using enzan::gui::DesignTreeModel;
using enzan::ipc::InstanceEntry;

QVector<InstanceEntry> flatDesign() {
    return {
        InstanceEntry{.path = "/tb", .name = "tb"},
        InstanceEntry{.path = "/tb/u_a", .name = "u_a", .parent = 0},
        InstanceEntry{.path = "/tb/u_b", .name = "u_b", .parent = 0},
        InstanceEntry{.path = "/tb/u_a/u_inner", .name = "u_inner", .parent = 1},
    };
}

TEST(DesignTreeModelTests, TheTreeKeepsItsShape) {
    DesignTreeModel model;
    model.setInstances(flatDesign());

    ASSERT_EQ(model.rowCount(), 1);

    const QModelIndex top = model.index(0, 0);
    EXPECT_EQ(model.data(top, Qt::DisplayRole).toString(), QStringLiteral("tb"));
    EXPECT_EQ(model.rowCount(top), 2);

    const QModelIndex first = model.index(0, 0, top);
    EXPECT_EQ(model.data(first, Qt::DisplayRole).toString(), QStringLiteral("u_a"));
    EXPECT_EQ(model.rowCount(first), 1);
    EXPECT_EQ(model.parent(first), top);

    const QModelIndex inner = model.index(0, 0, first);
    EXPECT_EQ(model.pathAt(inner), QStringLiteral("/tb/u_a/u_inner"));
    EXPECT_EQ(model.rowCount(inner), 0);
}

TEST(DesignTreeModelTests, APathResolvesToItsIndex) {
    DesignTreeModel model;
    model.setInstances(flatDesign());

    const QModelIndex found = model.indexOfPath(QStringLiteral("/tb/u_b"));
    ASSERT_TRUE(found.isValid());
    EXPECT_EQ(model.data(found, Qt::DisplayRole).toString(), QStringLiteral("u_b"));

    EXPECT_FALSE(model.indexOfPath(QStringLiteral("/tb/nowhere")).isValid());
}

TEST(DesignTreeModelTests, AParentThatArrivesLaterIsStillLinkedUp) {
    // The kernel sends elaboration order, which is depth-first, so in practice a parent always
    // comes first — but a model that depended on that would break the day the order changed.
    DesignTreeModel model;
    model.setInstances({
        InstanceEntry{.path = "/tb/u_a", .name = "u_a", .parent = 1},
        InstanceEntry{.path = "/tb", .name = "tb"},
    });

    ASSERT_EQ(model.rowCount(), 1);
    const QModelIndex top = model.index(0, 0);
    EXPECT_EQ(model.data(top, Qt::DisplayRole).toString(), QStringLiteral("tb"));
    EXPECT_EQ(model.rowCount(top), 1);
}

TEST(DesignTreeModelTests, AParentIndexThatIsNonsenseBecomesARoot) {
    // A malformed message must not produce an index that walks off the end of the vector every
    // time the view paints.
    DesignTreeModel model;
    model.setInstances({InstanceEntry{.path = "/tb", .name = "tb", .parent = 99}});

    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_FALSE(model.parent(model.index(0, 0)).isValid());
}

TEST(DesignTreeModelTests, ResettingReplacesTheWholeTree) {
    DesignTreeModel model;
    model.setInstances(flatDesign());
    model.setInstances({InstanceEntry{.path = "/other", .name = "other"}});

    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.pathAt(model.index(0, 0)), QStringLiteral("/other"));
}

TEST(DesignTreeModelTests, AnEmptyDesignIsEmptyRatherThanBroken) {
    DesignTreeModel model;
    model.setInstances({});

    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_FALSE(model.index(0, 0).isValid());
}

} // namespace
