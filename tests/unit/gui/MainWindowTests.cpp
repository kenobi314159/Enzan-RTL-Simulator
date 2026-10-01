// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the Structure window's menu does to the Wave window.
//
// Driven through the real window against the real session of SessionFixture, because the thing
// worth testing is the join: a scope's signals come from the kernel a message at a time, and the
// window has to end up with them in the right groups in the right order however those messages
// are interleaved. A test that called the model directly would test the part that was never in
// doubt.

#include "gui/MainWindow.h"

#include "gui/DesignTreeModel.h"
#include "gui/ObjectsModel.h"
#include "gui/Theme.h"
#include "gui/WaveformModel.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QCoreApplication>
#include <QDockWidget>
#include <QString>
#include <QTreeView>

#include <algorithm>
#include <memory>

namespace {

using enzan::gui::DesignTreeModel;
using enzan::gui::MainWindow;
using enzan::gui::ObjectsModel;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveRow;

/// The two scopes `guiTestDesign` elaborates to, outermost first.
const QString Top = QStringLiteral("/tb");
const QString Child = QStringLiteral("/tb/u_dut");

class MainWindowTest : public enzan::test::SessionFixture {
protected:
    void SetUp() override {
        // A settings identity of the suite's own. `MainWindow` restores its layout on
        // construction, and under the real application's name it would restore whatever layout
        // the developer running this suite last saved — making the result depend on the machine.
        QCoreApplication::setOrganizationName(QStringLiteral("enzan-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("gui-tests"));

        SessionFixture::SetUp();

        window = std::make_unique<MainWindow>(*session, theme);

        // The fixture waits for the hierarchy before the window exists, so the window missed the
        // signal that carries it. Asking again is the same round trip the window makes when it
        // is the one attaching.
        session->queryDesign();
        ASSERT_TRUE(pumpUntil([this] { return design()->rowCount() > 0; }))
            << "the window never received the hierarchy";
    }

    void TearDown() override {
        window.reset();
        SessionFixture::TearDown();
    }

    // Found in the widget tree rather than exposed by the window: an accessor that existed only
    // for a test is an accessor production code can start using.
    [[nodiscard]] WaveformModel* waves() const { return window->findChild<WaveformModel*>(); }

    [[nodiscard]] DesignTreeModel* design() const { return window->findChild<DesignTreeModel*>(); }

    [[nodiscard]] ObjectsModel* objects() const { return window->findChild<ObjectsModel*>(); }

    /// The Structure window's tree, through the dock that names itself: the window has two tree
    /// views and only the docks are distinguishable.
    [[nodiscard]] QTreeView* structure() const {
        for (QDockWidget* dock : window->findChildren<QDockWidget*>()) {
            if (dock->objectName() == QStringLiteral("structure")) {
                return qobject_cast<QTreeView*>(dock->widget());
            }
        }
        return nullptr;
    }

    /// Selects `scope` in the Structure window and triggers its menu entry `label`.
    void insert(const QString& scope, const QString& label) {
        QTreeView* tree = structure();
        ASSERT_NE(tree, nullptr);

        const QModelIndex index = design()->indexOfPath(scope);
        ASSERT_TRUE(index.isValid()) << scope.toStdString() << " is not in the tree";
        tree->setCurrentIndex(index);

        QAction* found = nullptr;
        for (QAction* action : tree->actions()) {
            if (action->text() == label) found = action;
        }
        ASSERT_NE(found, nullptr) << "no menu entry called " << label.toStdString();
        found->trigger();
    }

    /// The signal indices a scope's objects cover, parameters left out.
    ///
    /// Taken from the kernel rather than written down, so that the expectations follow the
    /// fixture's design instead of a copy of it.
    [[nodiscard]] QVector<std::uint32_t> signalsOf(const QString& scope) {
        QVector<std::uint32_t> indices;
        for (const SignalInfo& info : objectsOf(scope)) {
            if (!info.isParameter()) indices.append(info.signalIndex);
        }
        std::ranges::sort(indices);
        return indices;
    }

    /// Every top-level row of the wave window, in order.
    [[nodiscard]] QVector<int> topRows() const {
        QVector<int> rows;
        for (int index = 0; index < waves()->rowCount(); ++index) {
            rows.append(waves()->data(waves()->index(index, 0), WaveformModel::RowIdRole).toInt());
        }
        return rows;
    }

    /// The signal indices a row and everything under it shows, sorted.
    [[nodiscard]] QVector<std::uint32_t> signalsUnder(int rowId) const {
        QVector<std::uint32_t> indices;
        const WaveRow* row = waves()->row(rowId);
        if (row == nullptr) return indices;

        if (row->kind == WaveRow::Kind::Signal) indices.append(row->signalIndex);
        for (const int child : row->children) indices.append(signalsUnder(child));

        std::ranges::sort(indices);
        return indices;
    }

    /// Waits until the wave window holds `count` signals anywhere in it.
    bool waitForSignals(int count) {
        return pumpUntil([this, count] { return waves()->displayedSignals().size() == count; });
    }

    Theme theme;
    std::unique_ptr<MainWindow> window;
};

// ---------------------------------------------------------------------------
// Grouped
// ---------------------------------------------------------------------------

TEST_F(MainWindowTest, AGroupedInsertNamesTheGroupAfterTheScopesPath) {
    const QVector<std::uint32_t> expected = signalsOf(Top);
    ASSERT_FALSE(expected.isEmpty());

    insert(Top, QStringLiteral("Add to wave"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 1);

    const WaveRow* group = waves()->row(rows.front());
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->kind, WaveRow::Kind::Group);

    // The path, not the leaf name: two instances of one module would otherwise give two groups
    // called the same thing, which is the case grouping exists to tell apart.
    EXPECT_EQ(group->label, Top);
    EXPECT_EQ(signalsUnder(rows.front()), expected);
}

TEST_F(MainWindowTest, AGroupedRecursiveInsertPutsTheGroupsSideBySide) {
    QVector<std::uint32_t> expected = signalsOf(Top) + signalsOf(Child);
    std::ranges::sort(expected);

    insert(Top, QStringLiteral("Add to wave recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 2) << "one group per scope, both at the top level";

    // Siblings, not nested: the hierarchy is what the Structure window is for, and a reader
    // comparing a subtree's modules should not have to open a group per level to see them.
    for (const int rowId : rows) {
        const WaveRow* row = waves()->row(rowId);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->kind, WaveRow::Kind::Group);
        EXPECT_EQ(row->parent, -1);
    }

    // In the order the Structure window shows them, which is the order the tree arrived in —
    // not the order the kernel happened to answer the queries in.
    EXPECT_EQ(waves()->row(rows[0])->label, Top);
    EXPECT_EQ(waves()->row(rows[1])->label, Child);

    EXPECT_EQ(signalsUnder(rows[0]), signalsOf(Top));
    EXPECT_EQ(signalsUnder(rows[1]), signalsOf(Child));
}

// ---------------------------------------------------------------------------
// What an insert must not disturb
// ---------------------------------------------------------------------------

TEST_F(MainWindowTest, AnInsertLeavesTheObjectsWindowOnTheScopeTheReaderClicked) {
    // Clicking a scope is what fills the Objects window, and a recursive insert asks the kernel
    // about every scope in a subtree. Those answers must not land in the pane: a reader who
    // clicked the top level and then inserted its subtree would otherwise find themselves
    // looking at a module they never chose.
    QTreeView* tree = structure();
    ASSERT_NE(tree, nullptr);
    tree->setCurrentIndex(design()->indexOfPath(Top));
    Q_EMIT tree->clicked(design()->indexOfPath(Top));
    ASSERT_TRUE(pumpUntil([this] { return objects()->scope() == Top; }));

    const QVector<std::uint32_t> expected = signalsOf(Top) + signalsOf(Child);
    insert(Top, QStringLiteral("Add to wave recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    EXPECT_EQ(objects()->scope(), Top);
}

TEST_F(MainWindowTest, NoGroupIsLeftEmpty) {
    // A group named after a scope with nothing in it says only that the scope was asked about,
    // so an insert drops it. Both of the fixture's scopes declare signals, so what this pins is
    // the other half of that rule: a group that *was* filled must survive. An over-eager removal
    // — checking before the reply arrived, say — would take every group with it.
    QVector<std::uint32_t> expected = signalsOf(Top) + signalsOf(Child);
    std::ranges::sort(expected);

    insert(Top, QStringLiteral("Add to wave recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    const QVector<int> rows = topRows();
    EXPECT_EQ(rows.size(), 2);
    for (const int rowId : rows) {
        EXPECT_FALSE(waves()->row(rowId)->children.isEmpty())
            << waves()->row(rowId)->label.toStdString() << " is an empty group";
    }
}

} // namespace
