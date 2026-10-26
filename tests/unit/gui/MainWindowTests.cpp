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

#include "common/Version.h"
#include "gui/DesignTreeModel.h"
#include "gui/ObjectsModel.h"
#include "gui/ShortcutSheet.h"
#include "gui/Theme.h"
#include "gui/TranscriptView.h"
#include "gui/WaveformModel.h"
#include "gui/WaveformView.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QTest>
#include <QTextBrowser>
#include <QTreeView>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

using enzan::gui::DesignTreeModel;
using enzan::gui::MainWindow;
using enzan::gui::ObjectsModel;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveRow;
using enzan::ipc::SimulationState;

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

    /// The window's action whose text is `text`, or null.
    [[nodiscard]] QAction* actionCalled(const QString& text) const {
        for (QAction* candidate : window->findChildren<QAction*>()) {
            if (candidate->text() == text) return candidate;
        }
        return nullptr;
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

    insert(Top, QStringLiteral("Add to Wave"));
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

TEST_F(MainWindowTest, AGroupedRecursiveInsertNestsTheGroupsAsTheDesignIs) {
    QVector<std::uint32_t> expected = signalsOf(Top) + signalsOf(Child);
    std::ranges::sort(expected);

    insert(Top, QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 1) << "one group for the subtree";

    const WaveRow* top = waves()->row(rows.front());
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->kind, WaveRow::Kind::Group);
    EXPECT_EQ(top->label, Top);
    ASSERT_EQ(top->children.size(), 2) << "the scope's own signals, then its instance";

    // In the order the Structure window shows them, which is the order the tree arrived in —
    // not the order the kernel happened to answer the queries in. Inside the subtree's group an
    // instance is named by its name: the group around it already says where it is.
    const WaveRow* own = waves()->row(top->children[0]);
    const WaveRow* child = waves()->row(top->children[1]);
    ASSERT_NE(own, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(own->kind, WaveRow::Kind::Group);
    EXPECT_EQ(own->label, QStringLiteral("$top"));
    EXPECT_EQ(child->kind, WaveRow::Kind::Group);
    EXPECT_EQ(child->label, QStringLiteral("u_dut"));

    EXPECT_EQ(signalsUnder(top->children[0]), signalsOf(Top));
    EXPECT_EQ(signalsUnder(top->children[1]), signalsOf(Child));
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
    insert(Top, QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    EXPECT_EQ(objects()->scope(), Top);
}

TEST_F(MainWindowTest, NoGroupIsLeftEmpty) {
    // A group named after a scope with nothing in it says only that the scope was asked about,
    // so an insert drops it; SignalListTests pins that on a hierarchy with empty scopes in it.
    // Both of the fixture's scopes declare signals, so what this pins is the other half of that
    // rule: a group that *was* filled must survive. An over-eager removal — checking before the
    // reply arrived, say — would take every group with it.
    QVector<std::uint32_t> expected = signalsOf(Top) + signalsOf(Child);
    std::ranges::sort(expected);

    insert(Top, QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    QVector<int> rows = topRows();
    EXPECT_EQ(rows.size(), 1);
    int groups = 0;
    while (!rows.isEmpty()) {
        const WaveRow* row = waves()->row(rows.takeLast());
        ASSERT_NE(row, nullptr);
        if (row->kind != WaveRow::Kind::Group) continue;
        ++groups;
        EXPECT_FALSE(row->children.isEmpty()) << row->label.toStdString() << " is an empty group";
        rows += row->children;
    }
    EXPECT_EQ(groups, 3) << "the subtree's, its own signals' and its instance's";
}

TEST_F(MainWindowTest, StopEndsARunTheWindowStarted) {
    // The kernel serves on a thread of its own, as it does in a process of its own, so the window
    // keeps its event loop while the design runs, as it does for a reader pressing Stop. The
    // fixture's clock never stops, so the run ends only because the Stop reached the kernel, and
    // the window says so only once it has taken in what the run sent before it. With the views
    // refreshed for each of the clock's time slots, it took in a tenth of what the kernel sent
    // and fell behind for good, so the end of the run never reached it.
    const auto action = [this](const QString& text) -> QAction* {
        for (QAction* candidate : window->findChildren<QAction*>()) {
            if (candidate->text() == text) return candidate;
        }
        return nullptr;
    };
    QAction* runAll = action(QStringLiteral("Run All"));
    QAction* stop = action(QStringLiteral("Stop"));
    ASSERT_NE(runAll, nullptr);
    ASSERT_NE(stop, nullptr);

    // Streamed signals are what the window has to keep up with.
    insert(Top, QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>((signalsOf(Top) + signalsOf(Child)).size())));

    std::atomic<bool> serving = true;
    std::thread kernel([this, &serving] {
        while (serving && server->step(10)) {}
    });
    const auto waitFor = [](const auto& done, int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < milliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return done();
    };

    runAll->trigger();
    EXPECT_TRUE(waitFor([this] { return session->state() == SimulationState::Running; }, 5000))
        << "the run never started";
    waitFor([] { return false; }, 500);
    ASSERT_TRUE(stop->isEnabled()) << "Stop is offered while the design runs";
    stop->trigger();
    const bool stopped =
        waitFor([this] { return session->state() == SimulationState::Idle; }, 20000);

    serving = false;
    kernel.join();
    EXPECT_TRUE(stopped) << "the window went on showing a run the Stop had ended";
    EXPECT_GT(server->kernel().time(), 0U) << "the run had not actually started";
    EXPECT_EQ(session->latestTime(), server->kernel().time())
        << "the window has not taken in everything the run sent";
}

TEST_F(MainWindowTest, TheWaveFontIsTheNamesAndTheTraces) {
    // One size for both, so that each name stays level with its trace: the tree's font sets how
    // tall a row is, and the canvas measures its labels against its own. The transcript has a
    // size of its own, in its fixed-width font.
    Theme sized;
    sized.loadBuiltIn();
    QString error;
    ASSERT_TRUE(sized.evaluate(QStringLiteral("font wave 24\nfont transcript 15"), error))
        << error.toStdString();
    const MainWindow large(*session, sized);

    const auto rowsOf = [](const MainWindow& of) -> const QTreeView* {
        for (const QTreeView* view : of.findChildren<QTreeView*>()) {
            if (qobject_cast<WaveformModel*>(view->model()) != nullptr) return view;
        }
        return nullptr;
    };
    const QTreeView* rows = rowsOf(large);
    const QTreeView* plainRows = rowsOf(*window);
    const auto* canvas = large.findChild<enzan::gui::WaveformView*>();
    ASSERT_NE(rows, nullptr);
    ASSERT_NE(plainRows, nullptr);
    ASSERT_NE(canvas, nullptr);

    EXPECT_EQ(rows->font().pointSizeF(), 24.0);
    EXPECT_EQ(canvas->font(), rows->font());
    EXPECT_GT(QFontMetrics(rows->font()).height(), QFontMetrics(plainRows->font()).height())
        << "a larger size makes taller rows";

    const auto* transcriptView = large.findChild<enzan::gui::TranscriptView*>();
    ASSERT_NE(transcriptView, nullptr);
    const auto* text = transcriptView->findChild<QPlainTextEdit*>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->font().pointSizeF(), 15.0);
    EXPECT_EQ(text->font().family(), QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
}

TEST_F(MainWindowTest, TheRunLengthTakesWhatRunTakes) {
    // The spellings `--run` takes, read by the same function (section 8.5): a count of the run's
    // unit, or a time with a unit of its own. The fixture's design runs in nanoseconds, and has an
    // event every five of them, which is where a run that ends between events leaves the time.
    QLineEdit* length = nullptr;
    for (QLineEdit* edit : window->findChildren<QLineEdit*>()) {
        if (edit->toolTip().startsWith(QStringLiteral("How long"))) length = edit;
    }
    QAction* run = nullptr;
    for (QAction* candidate : window->findChildren<QAction*>()) {
        if (candidate->text() == QStringLiteral("Run")) run = candidate;
    }
    const auto* transcriptView = window->findChild<enzan::gui::TranscriptView*>();
    ASSERT_NE(length, nullptr);
    ASSERT_NE(run, nullptr);
    ASSERT_NE(transcriptView, nullptr);
    const auto* text = transcriptView->findChild<QPlainTextEdit*>();
    ASSERT_NE(text, nullptr);

    const auto runFor = [&](const QString& written, std::uint64_t until) {
        length->setText(written);
        run->trigger();
        return pumpUntil([&] {
            return session->currentTime() == until
                   && session->state() != enzan::ipc::SimulationState::Running;
        });
    };
    EXPECT_TRUE(runFor(QStringLiteral("20ns"), 20)) << session->currentTime();
    EXPECT_TRUE(runFor(QStringLiteral("15"), 35)) << session->currentTime();
    EXPECT_TRUE(runFor(QStringLiteral("0.005us"), 40)) << session->currentTime();

    // Refused with a note saying why, and nothing run.
    length->setText(QStringLiteral("1.5"));
    run->trigger();
    length->setText(QStringLiteral("1ps"));
    run->trigger();
    pump();
    EXPECT_EQ(session->currentTime(), 40U);
    EXPECT_TRUE(text->toPlainText().contains(QStringLiteral("'1.5' is not a length of time")))
        << text->toPlainText().toStdString();
    EXPECT_TRUE(text->toPlainText().contains(
        QStringLiteral("'1ps' is not a whole number of the run's time unit, ns")))
        << text->toPlainText().toStdString();

    // Enter in the field runs, which is what typing a length is for.
    length->setText(QStringLiteral("5"));
    QTest::keyClick(length, Qt::Key_Return);
    EXPECT_TRUE(pumpUntil([&] {
        return session->currentTime() == 45
               && session->state() != enzan::ipc::SimulationState::Running;
    })) << session->currentTime();
}

TEST_F(MainWindowTest, TheTranscriptIsClearedByOneCommand) {
    // The transcript's own menu and the window's View menu offer the same action.
    auto* transcriptView = window->findChild<enzan::gui::TranscriptView*>();
    ASSERT_NE(transcriptView, nullptr);
    const auto* text = transcriptView->findChild<QPlainTextEdit*>();
    ASSERT_NE(text, nullptr);
    window->note(QStringLiteral("something to clear"));
    ASSERT_FALSE(text->toPlainText().isEmpty());

    bool inViewMenu = false;
    for (const QAction* entry : window->menuBar()->actions()) {
        if (entry->menu() != nullptr && entry->text() == QStringLiteral("&View"))
            inViewMenu = entry->menu()->actions().contains(transcriptView->clearAction());
    }
    EXPECT_TRUE(inViewMenu);

    transcriptView->clearAction()->trigger();
    EXPECT_TRUE(text->toPlainText().isEmpty());
}

TEST_F(MainWindowTest, TheShortcutSheetIsReadFromTheActions) {
    QAction* shortcuts = actionCalled(QStringLiteral("Keyboard Shortcuts…"));
    ASSERT_NE(shortcuts, nullptr);
    EXPECT_EQ(shortcuts->shortcut(), QKeySequence(QKeySequence::HelpContents));

    const auto sheetText = [this, shortcuts] {
        shortcuts->trigger();
        const auto* sheet = window->findChild<enzan::gui::ShortcutSheet*>();
        if (sheet == nullptr || !sheet->isVisible()) return QString();
        const auto* browser = sheet->findChild<QTextBrowser*>();
        return browser != nullptr ? browser->toPlainText() : QString();
    };

    const QString text = sheetText();
    for (const QString& expected :
         {QStringLiteral("Zoom Fit"), QStringLiteral("Next Edge"),
          QStringLiteral("Add to Wave Recursive"), QStringLiteral("Ctrl+Shift+W"),
          QStringLiteral("Ctrl+Wheel"), QStringLiteral("Type a name")}) {
        EXPECT_TRUE(text.contains(expected)) << expected.toStdString();
    }
    EXPECT_EQ(text.count(QStringLiteral("Copy")), 1)
        << "the wave list's commands are the Edit menu's, and are listed once";

    // Read from the actions, so a key bound differently is listed differently.
    QAction* fit = actionCalled(QStringLiteral("Zoom Fit"));
    ASSERT_NE(fit, nullptr);
    fit->setShortcut(QKeySequence(Qt::Key_F9));
    EXPECT_TRUE(sheetText().contains(QStringLiteral("F9")));

    // And it can be closed with something on it, since a tiling window manager may draw no title
    // bar to close it with.
    auto* sheet = window->findChild<enzan::gui::ShortcutSheet*>();
    ASSERT_NE(sheet, nullptr);
    const auto* buttons = sheet->findChild<QDialogButtonBox*>();
    ASSERT_NE(buttons, nullptr);
    QPushButton* close = buttons->button(QDialogButtonBox::Close);
    ASSERT_NE(close, nullptr);
    close->click();
    EXPECT_FALSE(sheet->isVisible());
}

TEST_F(MainWindowTest, AToolbarButtonsKeyIsWrittenOut) {
    // "Add Cursor (Ins)" read as a cursor a nanosecond long.
    const QAction* add = actionCalled(QStringLiteral("Add Cursor"));
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->toolTip(), QStringLiteral("Add Cursor (Insert)"));
}

TEST_F(MainWindowTest, AboutNamesTheVersionOfEachHalf) {
    // Two programs, which may come from two builds when the kernel is on another machine.
    QAction* about = actionCalled(QStringLiteral("About Enzan"));
    ASSERT_NE(about, nullptr);
    about->trigger();

    const auto* box = window->findChild<QMessageBox*>();
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->isVisible());
    ASSERT_FALSE(session->kernelVersion().isEmpty());
    EXPECT_TRUE(box->text().contains(QString::fromStdString(enzan::versionLine()).toHtmlEscaped()));
    EXPECT_TRUE(box->text().contains(session->kernelVersion().toHtmlEscaped()));
}

} // namespace
