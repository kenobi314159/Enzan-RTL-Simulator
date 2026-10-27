// SPDX-License-Identifier: GPL-3.0-or-later
//
// Signal list files in a real window against the real session of SessionFixture
// (DESIGN_GUIDE.md section 10.8).
//
// Through the window rather than the model, because what is worth testing is the join: a list
// names scopes whose objects come from the kernel a message at a time, and the rows must end up
// as the file describes them whatever order those messages arrive in. The file format on its
// own is SignalListTests.

#include "gui/DesignTreeModel.h"
#include "gui/KernelSession.h"
#include "gui/MainWindow.h"
#include "gui/ObjectsModel.h"
#include "gui/Theme.h"
#include "gui/WaveformModel.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QDockWidget>
#include <QFile>
#include <QItemSelectionModel>
#include <QPlainTextEdit>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeView>

#include <algorithm>
#include <memory>

namespace {

using enzan::eir::Radix;
using enzan::gui::DesignTreeModel;
using enzan::gui::KernelSession;
using enzan::gui::MainWindow;
using enzan::gui::ObjectsModel;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveRow;

class SignalListLoadTest : public enzan::test::SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();
        theme.loadBuiltIn();
        window = std::make_unique<MainWindow>(*session, theme);

        // The fixture waits for the hierarchy before the window exists, so the window missed it.
        session->queryDesign();
        ASSERT_TRUE(pumpUntil([this] { return design()->rowCount() > 0; }));
        ASSERT_TRUE(files.isValid());
    }

    void TearDown() override {
        window.reset();
        SessionFixture::TearDown();
    }

    [[nodiscard]] WaveformModel* waves() const { return window->findChild<WaveformModel*>(); }

    [[nodiscard]] DesignTreeModel* design() const { return window->findChild<DesignTreeModel*>(); }

    [[nodiscard]] QString transcript() const {
        const auto* text = window->findChild<QPlainTextEdit*>();
        return text != nullptr ? text->toPlainText() : QString();
    }

    /// Writes `text` to a file of its own and returns its path.
    QString write(const QString& name, const QByteArray& text) {
        const QString path = files.filePath(name);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(text);
        return path;
    }

    /// Loads `text` and waits for the load to finish, which the transcript's note marks.
    ::testing::AssertionResult load(const QByteArray& text) {
        const QString path = write(QStringLiteral("list-%1.tcl").arg(++loads), text);
        const bool accepted = window->loadSignalList(path);
        if (accepted && pumpUntil([this, &path] {
                return transcript().contains(QStringLiteral("Loaded the signal list %1").arg(path));
            })) {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure() << "the transcript says:\n"
                                             << transcript().toStdString();
    }

    [[nodiscard]] QVector<int> topRows() const {
        QVector<int> rows;
        for (int index = 0; index < waves()->rowCount(); ++index)
            rows.append(waves()->data(waves()->index(index, 0), WaveformModel::RowIdRole).toInt());
        return rows;
    }

    [[nodiscard]] const WaveRow& rowAt(int rowId) const { return *waves()->row(rowId); }

    [[nodiscard]] SignalInfo info(const QString& scope, const QString& name) {
        for (const SignalInfo& candidate : objectsOf(scope)) {
            if (candidate.name == name) return candidate;
        }
        ADD_FAILURE() << "no object " << name.toStdString() << " in " << scope.toStdString();
        return {};
    }

    /// Triggers the Structure window's `label` entry on `scope`.
    void insert(const QString& scope, const QString& label) {
        QTreeView* tree = nullptr;
        for (const QDockWidget* dock : window->findChildren<QDockWidget*>()) {
            if (dock->objectName() == QStringLiteral("structure"))
                tree = qobject_cast<QTreeView*>(dock->widget());
        }
        ASSERT_NE(tree, nullptr);
        tree->setCurrentIndex(design()->indexOfPath(scope));
        for (QAction* action : tree->actions()) {
            if (action->text() == label) action->trigger();
        }
    }

    /// File → Save Signal List, the one that writes back rather than asking.
    [[nodiscard]] QAction* saveAction() const {
        for (QAction* action : window->findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("Save Signal List")) return action;
        }
        return nullptr;
    }

    [[nodiscard]] static QByteArray contentsOf(const QString& path) {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
        return file.readAll();
    }

    Theme theme;
    std::unique_ptr<MainWindow> window;
    QTemporaryDir files;
    int loads = 0;
};

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_F(SignalListLoadTest, ALoadedListHasTheRowsItDescribes) {
    ASSERT_TRUE(load(R"(
        add wave /tb/clk
        add wave -radix dec -label n /tb/count
        add wave /tb/count[3:0]
        group Control {
            add wave -radix bin -symbolic /tb/state
            add wave /tb/u_dut/WIDTH
        }
        add wave -highlight 2 -expand /tb/u_dut/q
    )"));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 5);

    EXPECT_EQ(rowAt(rows[0]).label, QStringLiteral("clk"));

    EXPECT_EQ(rowAt(rows[1]).label, QStringLiteral("n"));
    EXPECT_EQ(rowAt(rows[1]).radix, Radix::Decimal);
    EXPECT_FALSE(rowAt(rows[1]).symbolic) << "a radix asks for the number, as the menu does";

    EXPECT_TRUE(rowAt(rows[2]).sliced);
    EXPECT_EQ(rowAt(rows[2]).highBit, 3U);
    EXPECT_EQ(rowAt(rows[2]).lowBit, 0U);
    EXPECT_EQ(rowAt(rows[2]).label, QStringLiteral("count[3:0]"));

    const WaveRow& control = rowAt(rows[3]);
    EXPECT_EQ(control.kind, WaveRow::Kind::Group);
    ASSERT_EQ(control.children.size(), 2);
    EXPECT_EQ(rowAt(control.children[0]).radix, Radix::Binary);
    EXPECT_TRUE(rowAt(control.children[0]).symbolic) << "-symbolic applies after -radix";
    EXPECT_EQ(rowAt(control.children[1]).kind, WaveRow::Kind::Parameter);

    const WaveRow& q = rowAt(rows[4]);
    EXPECT_TRUE(q.unpacked);
    ASSERT_EQ(q.children.size(), 4) << "one row per bit, as unpacking makes them";
    EXPECT_EQ(rowAt(q.children.front()).highBit, 3U);
    EXPECT_EQ(rowAt(q.children.front()).highlight, 2) << "each bit takes the signal's highlight";
}

TEST_F(SignalListLoadTest, AScopeAddsWhatTheMenuEntryAdds) {
    insert(QStringLiteral("/tb"), QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(pumpUntil([this] { return topRows().size() == 1; }))
        << topRows().size() << " rows; the transcript says:\n"
        << transcript().toStdString();
    const QString byMenu = waves()->writeSignalList();

    ASSERT_TRUE(load("add wave -r /tb\n"));
    EXPECT_EQ(waves()->writeSignalList(), byMenu);
}

TEST_F(SignalListLoadTest, ALabelNamesTheGroupAScopeAdds) {
    ASSERT_TRUE(load("add wave -label Top /tb\n"));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 1) << "the scope alone, not its subtree";
    EXPECT_EQ(rowAt(rows[0]).kind, WaveRow::Kind::Group);
    EXPECT_EQ(rowAt(rows[0]).label, QStringLiteral("Top"));
    EXPECT_FALSE(rowAt(rows[0]).children.isEmpty());
}

TEST_F(SignalListLoadTest, AMissingPathIsSkippedWithAWarningNamingItsLine) {
    ASSERT_TRUE(load("add wave /tb/clk\n"
                     "add wave /tb/no_such_signal\n"
                     "add wave /tb/count\n"));

    EXPECT_EQ(topRows().size(), 2) << "the rest load";
    EXPECT_TRUE(transcript().contains(QStringLiteral(":2: /tb/no_such_signal names nothing")))
        << transcript().toStdString();
}

TEST_F(SignalListLoadTest, AMistakeLoadsNothingAndKeepsTheList) {
    waves()->addSignal(info(QStringLiteral("/tb"), QStringLiteral("clk")));

    const QString path = write(QStringLiteral("bad.tcl"), "add wave /tb/count\n"
                                                          "add wave -colour red /tb/clk\n");
    EXPECT_FALSE(window->loadSignalList(path));
    pump();

    ASSERT_EQ(topRows().size(), 1) << "the list the reader had is still there";
    EXPECT_EQ(rowAt(topRows().front()).label, QStringLiteral("clk"));
    EXPECT_TRUE(transcript().contains(QStringLiteral("bad.tcl:2: '-colour'")))
        << transcript().toStdString();
}

TEST_F(SignalListLoadTest, AMistakeFoundOnlyOnceThePathsAreKnownLoadsNothingEither) {
    // Whether `-expand` is right depends on whether its path is a scope,
    // which the parser cannot know; it is still a mistake in the file, not a missing path.
    waves()->addSignal(info(QStringLiteral("/tb"), QStringLiteral("clk")));

    const QString path = write(QStringLiteral("scope.tcl"), "add wave -expand /tb\n");
    ASSERT_TRUE(window->loadSignalList(path));
    ASSERT_TRUE(pumpUntil([this] { return transcript().contains(QStringLiteral("Nothing was")); }));

    EXPECT_EQ(topRows().size(), 1);
}

TEST_F(SignalListLoadTest, AListNamedBeforeTheDesignArrivesLoadsWhenItDoes) {
    // What `enzan run --gui --wave-list` does: the window reads the file before the kernel has
    // described anything. Only one client may attach at a time, so the fixture's steps aside.
    session->detach();
    pump();

    KernelSession late;
    MainWindow lateWindow(late, theme);
    const QString path = write(QStringLiteral("startup.tcl"), "add wave /tb/clk /tb/count\n");
    ASSERT_TRUE(lateWindow.loadSignalList(path)) << "read and checked, and waiting";

    QString error;
    ASSERT_TRUE(late.attach(socketPath, error)) << error.toStdString();
    auto* lateWaves = lateWindow.findChild<WaveformModel*>();
    ASSERT_TRUE(pumpUntil([lateWaves] { return lateWaves->rowCount() == 2; }));
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

TEST_F(SignalListLoadTest, ASavedListLoadsBackAsItWasAndSavesTheSameBytes) {
    WaveformModel* model = waves();

    // Something of everything a row can carry.
    const int clock = model->addSignal(info(QStringLiteral("/tb"), QStringLiteral("clk")));
    model->setHighlight(clock, 1);

    const int group = model->addGroup(QStringLiteral("State and parameters"));
    model->addSignal(info(QStringLiteral("/tb"), QStringLiteral("state")), group);
    model->addSignal(info(QStringLiteral("/tb/u_dut"), QStringLiteral("WIDTH")), group);
    model->addGroup(QStringLiteral("empty"), group);

    const int bus = model->addSignal(info(QStringLiteral("/tb"), QStringLiteral("count")));
    model->setLabel(bus, QStringLiteral("the {count}"));
    model->setRadixRecursive(bus, Radix::Decimal);
    model->unpack(bus);
    model->removeRow(model->row(bus)->children[1]);
    model->setRadix(model->row(bus)->children[0], Radix::Binary);
    static_cast<void>(model->moveRowsTo({model->row(bus)->children[2]}, bus, 0));

    const int q = model->addSignal(info(QStringLiteral("/tb/u_dut"), QStringLiteral("q")));
    model->unpack(q);

    const int slice = model->addSlice(
        model->addSignal(info(QStringLiteral("/tb"), QStringLiteral("count"))), 5, 2);
    model->setLabel(slice, QStringLiteral("middle"));

    const QString saved = files.filePath(QStringLiteral("saved.tcl"));
    ASSERT_TRUE(window->saveSignalList(saved));
    QFile first(saved);
    ASSERT_TRUE(first.open(QIODevice::ReadOnly));
    const QByteArray written = first.readAll();
    first.close();

    // What the window had made of it, to compare the reload against.
    const QString before = model->writeSignalList();
    EXPECT_TRUE(before.contains(QStringLiteral("-contents"))) << before.toStdString();
    EXPECT_TRUE(before.contains(QStringLiteral("-expand"))) << before.toStdString();

    ASSERT_TRUE(window->loadSignalList(saved));
    ASSERT_TRUE(pumpUntil([this, &saved] {
        return transcript().contains(QStringLiteral("Loaded the signal list %1").arg(saved));
    }));

    EXPECT_EQ(model->writeSignalList(), before);

    ASSERT_TRUE(window->saveSignalList(saved));
    QFile second(saved);
    ASSERT_TRUE(second.open(QIODevice::ReadOnly));
    EXPECT_EQ(second.readAll(), written) << "the same window saves the same bytes";
}

TEST_F(SignalListLoadTest, SaveWritesBackToTheFileTheListCameFrom) {
    // Arranging, saving and running again is the loop a signal list is for, so Save asks nothing
    // once the list has a file: the one it was loaded from, or last saved to.
    ASSERT_TRUE(load("add wave /tb/clk\n"));
    const QString loaded = files.filePath(QStringLiteral("list-1.tcl"));
    waves()->addSignal(info(QStringLiteral("/tb"), QStringLiteral("count")));

    ASSERT_NE(saveAction(), nullptr);
    saveAction()->trigger();
    EXPECT_EQ(contentsOf(loaded), waves()->writeSignalList().toUtf8());

    const QString elsewhere = files.filePath(QStringLiteral("elsewhere.tcl"));
    ASSERT_TRUE(window->saveSignalList(elsewhere));
    waves()->addSignal(info(QStringLiteral("/tb"), QStringLiteral("state")));
    saveAction()->trigger();
    EXPECT_EQ(contentsOf(elsewhere), waves()->writeSignalList().toUtf8())
        << "after a save to another file, that file is the list's";
    EXPECT_NE(contentsOf(loaded), contentsOf(elsewhere));
}

TEST_F(SignalListLoadTest, AFileThatLoadedNothingIsNotWhereSaveWrites) {
    // Its mistake left the window's list as it was, and that list is not the file's.
    const QString kept = files.filePath(QStringLiteral("kept.tcl"));
    waves()->addSignal(info(QStringLiteral("/tb"), QStringLiteral("clk")));
    ASSERT_TRUE(window->saveSignalList(kept));

    const QByteArray mistaken = "add wave -colour red /tb/clk\n";
    const QString bad = write(QStringLiteral("bad.tcl"), mistaken);
    EXPECT_FALSE(window->loadSignalList(bad));
    const QString scope = write(QStringLiteral("scope.tcl"), "add wave -expand /tb\n");
    ASSERT_TRUE(window->loadSignalList(scope));
    ASSERT_TRUE(pumpUntil([this] { return transcript().contains(QStringLiteral("Nothing was")); }));

    waves()->addSignal(info(QStringLiteral("/tb"), QStringLiteral("count")));
    ASSERT_NE(saveAction(), nullptr);
    saveAction()->trigger();
    EXPECT_EQ(contentsOf(bad), mistaken);
    EXPECT_EQ(contentsOf(scope), QByteArray("add wave -expand /tb\n"));
    EXPECT_EQ(contentsOf(kept), waves()->writeSignalList().toUtf8());
}

// ---------------------------------------------------------------------------
// Selects are the design's indices
// ---------------------------------------------------------------------------

/// The window over arrayTestDesign: `logic [1:0][3:0] lanes` = 8'h5a, `logic [3:0] memory [0:1]`
/// = 8'hc3 and `logic [0:3] up` = 4'b1000.
class ArraySignalListTest : public SignalListLoadTest {
protected:
    bool loadDesign(enzan::session::Session& into) override {
        into.setDesign(std::make_unique<enzan::eir::Design>(enzan::test::arrayTestDesign()));
        return true;
    }

    [[nodiscard]] QString valueOf(int rowId) const { return waves()->renderValue(rowId, 0); }
};

TEST_F(ArraySignalListTest, ASelectNamesAnElementOrABitAsTheSourceIndexesIt) {
    ASSERT_TRUE(load(R"(
        add wave /tb/lanes[1] /tb/lanes[0][3] /tb/memory[1] /tb/memory[0][2:1] /tb/up[0]
        add wave /tb/lanes[1:0] /tb/count[3:0]
    )"));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 7);

    // lanes[1] is the left element, the high nibble of 8'h5a.
    EXPECT_EQ(rowAt(rows[0]).label, QStringLiteral("lanes[1]"));
    EXPECT_EQ(valueOf(rows[0]), QStringLiteral("5"));
    EXPECT_EQ(rowAt(rows[1]).label, QStringLiteral("lanes[0][3]"));
    EXPECT_EQ(valueOf(rows[1]), QStringLiteral("1"));

    // memory[1] is the right element of [0:1], the low nibble of 8'hc3.
    EXPECT_EQ(valueOf(rows[2]), QStringLiteral("3"));
    EXPECT_EQ(rowAt(rows[3]).label, QStringLiteral("memory[0][2:1]"));
    EXPECT_EQ(valueOf(rows[3]), QStringLiteral("2")) << "bits 2 and 1 of 4'hc";

    // up[0] is the most significant bit of logic [0:3].
    EXPECT_EQ(valueOf(rows[4]), QStringLiteral("1"));

    // A part-select of an array keeps it an array, of the elements selected.
    EXPECT_EQ(valueOf(rows[5]), QStringLiteral("{5}{a}"));
    ASSERT_TRUE(waves()->canUnpack(rows[5]));
    waves()->unpack(rows[5]);
    EXPECT_EQ(rowAt(rowAt(rows[5]).children[0]).label, QStringLiteral("lanes[1:0][1]"));

    // A vector with no declared range is indexed by its bits, as before.
    EXPECT_EQ(rowAt(rows[6]).highBit, 3U);
    EXPECT_EQ(rowAt(rows[6]).lowBit, 0U);
}

TEST_F(ArraySignalListTest, ASelectOutsideTheDeclaredRangeIsAWarning) {
    ASSERT_TRUE(load(R"(
        add wave /tb/lanes[2] /tb/up[3:0] /tb/lanes[1:0][3] /tb/memory[0]
    )"));

    EXPECT_TRUE(transcript().contains(
        QStringLiteral("/tb/lanes[2] selects outside /tb/lanes, which is declared [1:0][3:0]")))
        << transcript().toStdString();
    EXPECT_TRUE(transcript().contains(QStringLiteral("/tb/up[3:0] selects outside")))
        << "a part-select runs as its dimension is declared";
    EXPECT_TRUE(transcript().contains(QStringLiteral("/tb/lanes[1:0][3] selects outside")))
        << "a part-select is the last select";
    ASSERT_EQ(topRows().size(), 1) << "and the rest of the list still loads";
    EXPECT_EQ(rowAt(topRows()[0]).label, QStringLiteral("memory[0]"));
}

TEST_F(ArraySignalListTest, AnArrayTakenApartIsSavedByItsIndicesAndLoadsBackTheSame) {
    WaveformModel* model = waves();
    const int lanes = model->addSignal(info(QStringLiteral("/tb"), QStringLiteral("lanes")));
    model->unpack(lanes);
    const int high = model->row(lanes)->children[0];
    const int low = model->row(lanes)->children[1];
    model->unpack(low);
    model->removeRow(model->row(low)->children[0]);
    model->setRadixRecursive(high, Radix::Binary);

    const int memory = model->addSignal(info(QStringLiteral("/tb"), QStringLiteral("memory")));
    model->unpack(memory);

    const QString before = model->writeSignalList();
    EXPECT_TRUE(before.contains(QStringLiteral("add wave -radix bin /tb/lanes[1]")))
        << before.toStdString();
    EXPECT_TRUE(before.contains(QStringLiteral("add wave /tb/lanes[0][2]")))
        << before.toStdString();
    EXPECT_TRUE(before.contains(QStringLiteral("add wave -expand /tb/memory")))
        << before.toStdString();

    ASSERT_TRUE(load(before.toUtf8()));
    EXPECT_EQ(model->writeSignalList(), before);

    // What came back is an element again, that unpacks into its bits.
    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 2);
    const int reloaded = rowAt(rows[0]).children[0];
    EXPECT_EQ(valueOf(reloaded), QStringLiteral("0101"));
    EXPECT_EQ(valueOf(rows[1]), QStringLiteral("{c}{3}"));
    ASSERT_TRUE(model->canUnpack(reloaded));
    model->unpack(reloaded);
    EXPECT_EQ(rowAt(rowAt(reloaded).children[0]).label, QStringLiteral("lanes[1][3]"));
}

// ---------------------------------------------------------------------------
// The transcript says what was done
// ---------------------------------------------------------------------------

TEST_F(SignalListLoadTest, TheMenuEntriesEchoTheirCommand) {
    insert(QStringLiteral("/tb"), QStringLiteral("Add to Wave"));
    insert(QStringLiteral("/tb/u_dut"), QStringLiteral("Add to Wave Recursive"));
    pump();

    EXPECT_TRUE(transcript().contains(QStringLiteral("add wave /tb\n")))
        << transcript().toStdString();
    EXPECT_TRUE(transcript().contains(QStringLiteral("add wave -r /tb/u_dut")));
}

TEST_F(SignalListLoadTest, AddingFromTheObjectsWindowEchoesItsCommand) {
    auto* objectsView = window->findChild<QTableView*>();
    auto* objects = window->findChild<ObjectsModel*>();
    ASSERT_NE(objectsView, nullptr);
    ASSERT_TRUE(pumpUntil([objects] { return objects->rowCount() > 0; }));

    int clockRow = -1;
    for (int row = 0; row < objects->rowCount(); ++row) {
        if (objects->objectAt(row)->name == QStringLiteral("clk")) clockRow = row;
    }
    ASSERT_GE(clockRow, 0);

    const QModelIndex index = objects->index(clockRow, 0);
    objectsView->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect
                                                     | QItemSelectionModel::Rows);
    Q_EMIT objectsView->doubleClicked(index);

    EXPECT_TRUE(transcript().contains(QStringLiteral("add wave /tb/clk")))
        << transcript().toStdString();
}

} // namespace
