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
#include "gui/Configuration.h"
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
#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeView>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

using enzan::gui::DesignTreeModel;
using enzan::gui::KernelSession;
using enzan::gui::MainWindow;
using enzan::gui::ObjectsModel;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveRow;
using enzan::ipc::SimulationState;

/// The two scopes `guiTestDesign` elaborates to, outermost first. Functions rather than constants,
/// whose construction before `main` could throw where nothing could catch it.
QString topScope() {
    return QStringLiteral("/tb");
}

QString childScope() {
    return QStringLiteral("/tb/u_dut");
}

class MainWindowTest : public enzan::test::SessionFixture {
protected:
    void SetUp() override {
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
        for (const QDockWidget* dock : window->findChildren<QDockWidget*>()) {
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

    /// The wave window's list of names, which is the tree whose model is the wave model.
    [[nodiscard]] QTreeView* waveRows() const {
        for (QTreeView* view : window->findChildren<QTreeView*>()) {
            if (qobject_cast<WaveformModel*>(view->model()) != nullptr) return view;
        }
        return nullptr;
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

    /// The toolbar's run length, which is the field whose tooltip says how long a run is.
    [[nodiscard]] QLineEdit* runLength() const {
        for (QLineEdit* edit : window->findChildren<QLineEdit*>()) {
            if (edit->toolTip().startsWith(QStringLiteral("How long"))) return edit;
        }
        return nullptr;
    }

    /// Keeps the window's event loop until `done` or `milliseconds` have passed, for a test whose
    /// kernel serves on a thread of its own rather than in `pump`'s turns. Returns what `done`
    /// last said.
    template<typename Predicate>
    static bool waitFor(const Predicate& done, int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < milliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return done();
    }

    Theme theme;
    std::unique_ptr<MainWindow> window;
};

// ---------------------------------------------------------------------------
// Grouped
// ---------------------------------------------------------------------------

TEST_F(MainWindowTest, AGroupedInsertNamesTheGroupAfterTheScopesPath) {
    const QVector<std::uint32_t> expected = signalsOf(topScope());
    ASSERT_FALSE(expected.isEmpty());

    insert(topScope(), QStringLiteral("Add to Wave"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 1);

    const WaveRow* group = waves()->row(rows.front());
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->kind, WaveRow::Kind::Group);

    // The path, not the leaf name: two instances of one module would otherwise give two groups
    // called the same thing, which is the case grouping exists to tell apart.
    EXPECT_EQ(group->label, topScope());
    EXPECT_EQ(signalsUnder(rows.front()), expected);
}

TEST_F(MainWindowTest, AGroupedRecursiveInsertNestsTheGroupsAsTheDesignIs) {
    QVector<std::uint32_t> expected = signalsOf(topScope()) + signalsOf(childScope());
    std::ranges::sort(expected);

    insert(topScope(), QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    const QVector<int> rows = topRows();
    ASSERT_EQ(rows.size(), 1) << "one group for the subtree";

    const WaveRow* top = waves()->row(rows.front());
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->kind, WaveRow::Kind::Group);
    EXPECT_EQ(top->label, topScope());
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

    EXPECT_EQ(signalsUnder(top->children[0]), signalsOf(topScope()));
    EXPECT_EQ(signalsUnder(top->children[1]), signalsOf(childScope()));
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
    tree->setCurrentIndex(design()->indexOfPath(topScope()));
    Q_EMIT tree->clicked(design()->indexOfPath(topScope()));
    ASSERT_TRUE(pumpUntil([this] { return objects()->scope() == topScope(); }));

    const QVector<std::uint32_t> expected = signalsOf(topScope()) + signalsOf(childScope());
    insert(topScope(), QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(waitForSignals(static_cast<int>(expected.size())));

    EXPECT_EQ(objects()->scope(), topScope());
}

TEST_F(MainWindowTest, NoGroupIsLeftEmpty) {
    // A group named after a scope with nothing in it says only that the scope was asked about,
    // so an insert drops it; SignalListTests pins that on a hierarchy with empty scopes in it.
    // Both of the fixture's scopes declare signals, so what this pins is the other half of that
    // rule: a group that *was* filled must survive. An over-eager removal — checking before the
    // reply arrived, say — would take every group with it.
    QVector<std::uint32_t> expected = signalsOf(topScope()) + signalsOf(childScope());
    std::ranges::sort(expected);

    insert(topScope(), QStringLiteral("Add to Wave Recursive"));
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
    insert(topScope(), QStringLiteral("Add to Wave Recursive"));
    ASSERT_TRUE(
        waitForSignals(static_cast<int>((signalsOf(topScope()) + signalsOf(childScope())).size())));

    std::atomic<bool> serving = true;
    std::thread kernel([this, &serving] {
        while (serving && server->step(10)) {}
    });

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

// ---------------------------------------------------------------------------
// A new group
// ---------------------------------------------------------------------------

class NewGroupTest : public MainWindowTest {
protected:
    /// Fills the wave window as Add to Wave Recursive does: `/tb`'s group, holding `$top`'s with
    /// clk, count and state, then `u_dut`'s.
    void SetUp() override {
        MainWindowTest::SetUp();
        insert(topScope(), QStringLiteral("Add to Wave Recursive"));
        ASSERT_TRUE(waitForSignals(
            static_cast<int>(signalsOf(topScope()).size() + signalsOf(childScope()).size())));

        tb = topRows().front();
        ASSERT_EQ(waves()->row(tb)->children.size(), 2);
        own = waves()->row(tb)->children[0];
        dut = waves()->row(tb)->children[1];
        ASSERT_EQ(waves()->row(own)->children.size(), 3);
    }

    /// Adds `rowId` to the wave list's selection, after whatever is selected already.
    void addToSelection(int rowId) const {
        waveRows()->selectionModel()->select(
            waves()->indexOfRow(rowId), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }

    /// Collects the selection into a new group, answering the dialog as a reader would. Returns
    /// the group, the first row the command adds.
    int groupSelection() {
        QAction* action = actionCalled(QStringLiteral("New Group…"));
        if (action == nullptr) {
            ADD_FAILURE() << "no New Group action";
            return -1;
        }

        // The dialog runs an event loop of its own, so it is answered from inside that loop.
        QTimer::singleShot(0, window.get(), [this] {
            if (auto* dialog = window->findChild<QInputDialog*>()) dialog->accept();
        });

        int made = -1;
        const QMetaObject::Connection watch =
            QObject::connect(waves(), &QAbstractItemModel::rowsInserted,
                             [this, &made](const QModelIndex& parent, int first, int /*last*/) {
                                 if (made < 0)
                                     made = waves()->rowIdOf(waves()->index(first, 0, parent));
                             });
        action->trigger();
        QObject::disconnect(watch);
        return made;
    }

    int tb = -1;
    int own = -1;
    int dut = -1;
};

TEST_F(NewGroupTest, TakesThePlaceOfTheRowsItCollects) {
    const QVector<int> signalRows = waves()->row(own)->children;
    addToSelection(signalRows[1]);
    addToSelection(signalRows[2]);

    const int made = groupSelection();
    ASSERT_NE(waves()->row(made), nullptr);
    EXPECT_EQ(waves()->row(made)->kind, WaveRow::Kind::Group);

    EXPECT_EQ(waves()->row(own)->children, (QVector<int>{signalRows[0], made}));
    EXPECT_EQ(waves()->row(made)->children, (QVector<int>{signalRows[1], signalRows[2]}));
    EXPECT_EQ(waves()->row(tb)->children, (QVector<int>{own, dut}));
}

TEST_F(NewGroupTest, RowsAtSeveralDepthsAreCollectedWhereTheTopmostWas) {
    // The deeper row is clicked first, so a group put where the first click was would land in
    // `u_dut`'s group rather than where `$top`'s stood.
    const int q = waves()->row(dut)->children.front();
    addToSelection(q);
    addToSelection(own);

    const int made = groupSelection();
    ASSERT_NE(waves()->row(made), nullptr);

    EXPECT_EQ(waves()->row(tb)->children, (QVector<int>{made, dut}));
    EXPECT_EQ(waves()->row(made)->children, (QVector<int>{q, own}));
}

TEST_F(NewGroupTest, AGroupCollectedWithOneOfItsRowsIsCollectedWhole) {
    // Put where `q` was, the group would be inside `u_dut`'s, which could then not be moved into
    // it, and would be left holding the group rather than held by it.
    const int q = waves()->row(dut)->children.front();
    addToSelection(q);
    addToSelection(dut);

    const int made = groupSelection();
    ASSERT_NE(waves()->row(made), nullptr);

    EXPECT_EQ(waves()->row(tb)->children, (QVector<int>{own, made}));
    EXPECT_EQ(waves()->row(made)->children, (QVector<int>{q, dut}));
}

// ---------------------------------------------------------------------------
// Changing the theme
// ---------------------------------------------------------------------------

/// The shipped theme with `script` over it.
Theme themeWith(const char* script) {
    Theme result;
    result.loadBuiltIn();
    QString error;
    EXPECT_TRUE(result.evaluate(QString::fromUtf8(script), error)) << error.toStdString();
    return result;
}

TEST_F(MainWindowTest, ANewThemeReachesTheWholeApplication) {
    window->setTheme(themeWith("theme window.background #0a0b0c\n"
                               "theme header.background #0d0e0f\n"));

    EXPECT_EQ(QApplication::palette().color(QPalette::Window), QColor(0x0A, 0x0B, 0x0C));
    EXPECT_TRUE(qApp->styleSheet().contains(QStringLiteral("#ff0d0e0f"), Qt::CaseInsensitive))
        << "the style sheet still has the old theme's header";
    EXPECT_EQ(window->currentTheme().color(enzan::gui::ThemeColor::WindowBackground),
              QColor(0x0A, 0x0B, 0x0C));
}

TEST_F(MainWindowTest, AThemeThatSetsNoSizesGivesTheDesktopsBack) {
    // Sizes are worked out from the desktop's fonts every time. Worked out from the fonts the
    // window had, a theme setting none would keep whatever the theme before it set.
    const QTreeView* rows = waveRows();
    ASSERT_NE(rows, nullptr);
    const double desktop = rows->font().pointSizeF();
    const double desktopWindow = QApplication::font().pointSizeF();

    window->setTheme(themeWith("font window 20\nfont wave 24\n"));
    EXPECT_EQ(rows->font().pointSizeF(), 24.0);
    EXPECT_EQ(QApplication::font().pointSizeF(), 20.0);
    EXPECT_EQ(window->findChild<enzan::gui::WaveformView*>()->font(), rows->font());

    window->setTheme(themeWith(""));
    EXPECT_EQ(rows->font().pointSizeF(), desktop);
    EXPECT_EQ(QApplication::font().pointSizeF(), desktopWindow);
}

/// The widgets of `window` whose text is not `points` in size, named in one line, leaving out
/// those the window gives a size of their own — the signal list, the traces and the transcript —
/// and what is inside them. Empty when there are none.
std::string widgetsNotAt(const QWidget& window, double points) {
    QStringList wrong;
    if (window.font().pointSizeF() != points) wrong << QStringLiteral("the window itself");
    for (const QWidget* widget : window.findChildren<QWidget*>()) {
        bool sizedApart = false;
        for (const QWidget* outer = widget; outer != nullptr && outer != &window;
             outer = outer->parentWidget()) {
            sizedApart = sizedApart || outer->testAttribute(Qt::WA_SetFont);
        }
        if (!sizedApart && widget->font().pointSizeF() != points) {
            wrong << QStringLiteral("%1 '%2' at %3")
                         .arg(QString::fromLatin1(widget->metaObject()->className()),
                              widget->objectName(), QString::number(widget->font().pointSizeF()));
        }
    }
    return wrong.join(QStringLiteral(", ")).toStdString();
}

TEST_F(MainWindowTest, TheWindowsSizeReachesEveryPaneOfAWindowOpenedWithIt) {
    // As the application starts: the window's style sheet is the first, and the window's widgets
    // are made before its theme is applied. The Structure and Objects windows, the menus and the
    // toolbar kept the desktop's size.
    window.reset();
    qApp->setStyleSheet(QString());

    MainWindow large(*session, themeWith("font window 20\nfont wave 24\n"));
    EXPECT_EQ(widgetsNotAt(large, 20.0), "");

    large.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(widgetsNotAt(large, 20.0), "");
}

/// How many rows of `dock`'s title bar its title covers, read from the dock as drawn: the rows of
/// the bar's left half, away from its buttons, with a pixel far from the bar's `background`.
int titleTextHeight(QDockWidget& dock, const QColor& background) {
    const QImage image = dock.grab().toImage();
    const int bar = dock.widget()->geometry().top();
    int rows = 0;
    for (int y = 0; y < bar; ++y) {
        for (int x = 0; x < image.width() / 2; ++x) {
            if (std::abs(image.pixelColor(x, y).lightness() - background.lightness()) > 40) {
                ++rows;
                break;
            }
        }
    }
    return rows;
}

TEST_F(MainWindowTest, ADocksTitleIsTheWindowsSize) {
    // Qt drew a title in the font the application had when the dock was made, in a bar as tall
    // as the theme's size, so a theme made the bars taller and left the titles as they were.
    auto* dock = window->findChild<QDockWidget*>(QStringLiteral("structure"));
    ASSERT_NE(dock, nullptr);
    window->show();

    const Theme small = themeWith("font window 10\n");
    window->setTheme(small);
    QCoreApplication::processEvents();
    const int smallTitle =
        titleTextHeight(*dock, small.color(enzan::gui::ThemeColor::HeaderBackground));

    const Theme large = themeWith("font window 24\n");
    window->setTheme(large);
    QCoreApplication::processEvents();
    const int largeTitle =
        titleTextHeight(*dock, large.color(enzan::gui::ThemeColor::HeaderBackground));

    ASSERT_GT(smallTitle, 0) << "no title was found to measure";
    EXPECT_GT(largeTitle, smallTitle * 3 / 2)
        << "a title of " << smallTitle << " rows at 10 " << "points was " << largeTitle << " at 24";
}

TEST_F(MainWindowTest, TheWindowsSizeReachesEveryPaneWhenTheThemeChanges) {
    // Each pane the theme does not size apart was a theme behind: going from a theme with large
    // sizes to one with none made those panes large just as the signal list went back.
    const double desktop = QApplication::font().pointSizeF();
    window->show();
    QCoreApplication::processEvents();

    window->setTheme(themeWith("font window 20\nfont wave 24\nfont transcript 22\n"));
    EXPECT_EQ(widgetsNotAt(*window, 20.0), "");
    QCoreApplication::processEvents();
    EXPECT_EQ(widgetsNotAt(*window, 20.0), "");

    window->setTheme(themeWith(""));
    EXPECT_EQ(widgetsNotAt(*window, desktop), "");
    QCoreApplication::processEvents();
    EXPECT_EQ(widgetsNotAt(*window, desktop), "");
}

TEST_F(MainWindowTest, ANewThemeRecoloursTheLinesAlreadyInTheTranscript) {
    // Each line remembers which of the theme's colours it is in, so a note written under the old
    // theme takes the new theme's colour for notes, and every line of a note that had several.
    window->setTheme(themeWith(""));
    window->note(QStringLiteral("first of two\nsecond of two"));

    window->setTheme(themeWith("theme transcript.note #010203\n"));

    const auto* text = window->findChild<QPlainTextEdit*>();
    ASSERT_NE(text, nullptr);
    int found = 0;
    for (QTextBlock block = text->document()->begin(); block.isValid(); block = block.next()) {
        if (!block.text().endsWith(QStringLiteral("of two"))) continue;
        ++found;
        EXPECT_EQ(block.begin().fragment().charFormat().foreground().color(), QColor(1, 2, 3))
            << block.text().toStdString();
    }
    EXPECT_EQ(found, 2);
}

TEST_F(MainWindowTest, TheHighlightSwatchesAreTheThemesColours) {
    window->setTheme(themeWith("theme wave.highlight.2 #102030\n"));

    const QMenu* highlight = nullptr;
    for (const QMenu* menu : window->findChildren<QMenu*>()) {
        if (menu->title() == QStringLiteral("Highlight")) highlight = menu;
    }
    ASSERT_NE(highlight, nullptr);
    ASSERT_GE(highlight->actions().size(), 2);

    const QImage swatch = highlight->actions().at(1)->icon().pixmap(12, 12).toImage();
    EXPECT_EQ(swatch.pixelColor(6, 6), QColor(0x10, 0x20, 0x30));
}

TEST_F(MainWindowTest, AProblemWithTheSettingsIsReportedAgainstItsLine) {
    window->report(enzan::gui::ConfigurationMessage{
        .file = QStringLiteral("/home/reader/.config/enzan/config"),
        .line = 3,
        .text = QStringLiteral("'layout' is not a setting this build knows, and is skipped")});

    const auto* text = window->findChild<QPlainTextEdit*>();
    ASSERT_NE(text, nullptr);
    EXPECT_TRUE(text->toPlainText().contains(
        QStringLiteral("/home/reader/.config/enzan/config:3: 'layout' is not a setting")))
        << text->toPlainText().toStdString();
}

// ---------------------------------------------------------------------------
// The Theme menu
// ---------------------------------------------------------------------------

/// A window with a configuration directory of its own, and its Theme menu.
class ThemeMenuTest : public MainWindowTest {
protected:
    void SetUp() override {
        MainWindowTest::SetUp();
        ASSERT_TRUE(home.isValid());
        directory = enzan::gui::ConfigurationDirectory(home.filePath(QStringLiteral("enzan")));
        QString error;
        ASSERT_TRUE(directory.createDefaults(error)) << error.toStdString();
        writeTheme(QStringLiteral("themes/pale.tcl"), "theme window.background #0a0b0c\n");
    }

    void TearDown() override {
        chooser = nullptr;
        MainWindowTest::TearDown();
    }

    void writeTheme(const QString& name, const QByteArray& script) const {
        QDir().mkpath(QFileInfo(directory.resolve(name)).path());
        QFile file(directory.resolve(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << name.toStdString();
        file.write(script);
    }

    void open() {
        chooser = std::make_unique<MainWindow>(*session, directory.loadTheme().theme, directory);
    }

    /// The Theme menu, filled as it is when it opens.
    [[nodiscard]] QMenu* menu() const {
        for (QMenu* candidate : chooser->findChildren<QMenu*>()) {
            if (candidate->title() == QStringLiteral("Theme")) {
                Q_EMIT candidate->aboutToShow();
                return candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] QAction* entry(const QString& text) const {
        const QMenu* themes = menu();
        if (themes == nullptr) return nullptr;
        for (QAction* candidate : themes->actions()) {
            if (candidate->text() == text) return candidate;
        }
        return nullptr;
    }

    [[nodiscard]] enzan::gui::ThemeName configured() const { return directory.read().theme; }

    [[nodiscard]] QString transcript() const {
        const auto* text = chooser->findChild<QPlainTextEdit*>();
        return text == nullptr ? QString() : text->toPlainText();
    }

    QTemporaryDir home;
    enzan::gui::ConfigurationDirectory directory;
    std::unique_ptr<MainWindow> chooser;
};

TEST_F(ThemeMenuTest, OffersTheBuiltInThemeAndEveryFileInThemes) {
    open();
    const QMenu* themes = menu();
    ASSERT_NE(themes, nullptr);

    QStringList texts;
    for (const QAction* action : themes->actions()) {
        texts << (action->isSeparator() ? QStringLiteral("-") : action->text());
    }
    EXPECT_EQ(texts,
              (QStringList{QStringLiteral("Dark (built-in)"), QStringLiteral("Light (built-in)"),
                           QStringLiteral("pale"), QStringLiteral("-"),
                           QStringLiteral("Reload Theme"), QStringLiteral("New Theme…")}));

    EXPECT_TRUE(entry(QStringLiteral("Dark (built-in)"))->isChecked())
        << "config names no theme, which is the built-in dark one";
}

TEST_F(ThemeMenuTest, AThemeDroppedInIsOfferedWhenTheMenuOpens) {
    open();
    writeTheme(QStringLiteral("themes/later.tcl"), "theme accent #123456\n");
    EXPECT_NE(entry(QStringLiteral("later")), nullptr);
}

TEST_F(ThemeMenuTest, ChoosingAThemeColoursTheWindowAndKeepsIt) {
    open();
    entry(QStringLiteral("pale"))->trigger();

    EXPECT_EQ(QApplication::palette().color(QPalette::Window), QColor(0x0A, 0x0B, 0x0C));
    EXPECT_EQ(configured().file, QStringLiteral("themes/pale.tcl"));
    EXPECT_TRUE(entry(QStringLiteral("pale"))->isChecked());

    // The default built-in theme is chosen by leaving the theme out.
    entry(QStringLiteral("Dark (built-in)"))->trigger();
    Theme byDefault;
    byDefault.loadBuiltIn();
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              byDefault.color(enzan::gui::ThemeColor::WindowBackground));
    EXPECT_TRUE(configured().isDefault());
    EXPECT_TRUE(entry(QStringLiteral("Dark (built-in)"))->isChecked());
}

TEST_F(ThemeMenuTest, TheLightThemeIsChosenFromTheMenuAndKept) {
    open();
    entry(QStringLiteral("Light (built-in)"))->trigger();

    Theme light;
    ASSERT_TRUE(light.loadBuiltIn(QStringLiteral("light")));
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              light.color(enzan::gui::ThemeColor::WindowBackground));
    EXPECT_EQ(configured(), enzan::gui::ThemeName::ofBuiltIn(QStringLiteral("light")));
    EXPECT_TRUE(entry(QStringLiteral("Light (built-in)"))->isChecked());

    // And the next window opens with it.
    chooser = nullptr;
    open();
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              light.color(enzan::gui::ThemeColor::WindowBackground));
    EXPECT_TRUE(entry(QStringLiteral("Light (built-in)"))->isChecked());
}

TEST_F(ThemeMenuTest, AThemeThatDoesNotLoadChangesNothing) {
    writeTheme(QStringLiteral("themes/broken.tcl"), "theme window.nonsense #000000\n");
    open();
    const QColor before = QApplication::palette().color(QPalette::Window);

    entry(QStringLiteral("broken"))->trigger();

    EXPECT_EQ(QApplication::palette().color(QPalette::Window), before);
    EXPECT_TRUE(configured().isDefault());
    EXPECT_TRUE(entry(QStringLiteral("Dark (built-in)"))->isChecked());
    EXPECT_TRUE(transcript().contains(QStringLiteral("broken.tcl:1: ")))
        << transcript().toStdString();
}

TEST_F(ThemeMenuTest, ReloadReadsTheThemeInUseAgain) {
    open();
    entry(QStringLiteral("pale"))->trigger();
    writeTheme(QStringLiteral("themes/pale.tcl"), "theme window.background #0d0e0f\n");

    entry(QStringLiteral("Reload Theme"))->trigger();

    EXPECT_EQ(QApplication::palette().color(QPalette::Window), QColor(0x0D, 0x0E, 0x0F));
}

TEST_F(ThemeMenuTest, NewThemeWritesACopyOfTheThemeInUseAndUsesIt) {
    // With the light theme in use, a new theme starts from the light theme: what the reader sees
    // is what they go on to edit.
    open();
    entry(QStringLiteral("Light (built-in)"))->trigger();
    entry(QStringLiteral("New Theme…"))->trigger();

    auto* dialog = chooser->findChild<QInputDialog*>();
    ASSERT_NE(dialog, nullptr);
    dialog->setTextValue(QStringLiteral("mine"));
    dialog->accept();

    QFile written(directory.resolve(QStringLiteral("themes/mine.tcl")));
    ASSERT_TRUE(written.open(QIODevice::ReadOnly));
    EXPECT_EQ(written.readAll(), Theme::builtInScript(QStringLiteral("light")).toUtf8());
    EXPECT_EQ(configured().file, QStringLiteral("themes/mine.tcl"));
    EXPECT_TRUE(entry(QStringLiteral("mine"))->isChecked());
}

TEST_F(ThemeMenuTest, AWindowWithNoDirectoryOffersOnlyTheBuiltInThemes) {
    directory = enzan::gui::ConfigurationDirectory();
    open();

    EXPECT_NE(entry(QStringLiteral("Dark (built-in)")), nullptr);
    EXPECT_NE(entry(QStringLiteral("Light (built-in)")), nullptr);
    EXPECT_EQ(entry(QStringLiteral("dark")), nullptr);
    ASSERT_NE(entry(QStringLiteral("New Theme…")), nullptr);
    EXPECT_FALSE(entry(QStringLiteral("New Theme…"))->isEnabled());
}

TEST(MainWindowLayout, IsKeptInTheConfigurationDirectory) {
    // Beside `config`, and nowhere else: a window with no directory restores nothing, which is
    // also what keeps every other test's window at its first size whoever runs the suite.
    const QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const enzan::gui::ConfigurationDirectory directory(home.filePath(QStringLiteral("enzan")));
    QString error;
    ASSERT_TRUE(directory.createDefaults(error)) << error.toStdString();

    Theme theme;
    theme.loadBuiltIn();
    enzan::gui::KernelSession idle;
    {
        MainWindow first(idle, theme, directory);
        first.show();
        first.resize(700, 500);
        QCoreApplication::processEvents();
        first.close();
    }
    EXPECT_TRUE(QFileInfo::exists(directory.layoutFile()));

    const MainWindow second(idle, theme, directory);
    EXPECT_EQ(second.size(), QSize(700, 500));
}

TEST_F(MainWindowTest, TheRunLengthTakesWhatRunTakes) {
    // The spellings `--run` takes, read by the same function: a count of the run's unit, or a time
    // with a unit of its own. The fixture's design runs in nanoseconds, and has an event every
    // five of them, which is where a run that ends between events leaves the time.
    QLineEdit* length = runLength();
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

TEST_F(MainWindowTest, AllInTheRunLengthRunsUntilTheDesignFinishes) {
    // As `--run all` does. The fixture's clock never stops, so the run goes on until it is
    // stopped, and the kernel serves on a thread of its own: a run is carried out within one step
    // of it, and `pump` would never come back.
    QLineEdit* length = runLength();
    QAction* run = actionCalled(QStringLiteral("Run"));
    ASSERT_NE(length, nullptr);
    ASSERT_NE(run, nullptr);

    std::atomic<bool> serving = true;
    std::thread kernel([this, &serving] {
        while (serving && server->step(10)) {}
    });

    length->setText(QStringLiteral("all"));
    run->trigger();
    const bool started =
        waitFor([this] { return session->state() == SimulationState::Running; }, 5000);
    session->stop();
    const bool stopped =
        waitFor([this] { return session->state() == SimulationState::Idle; }, 20000);

    serving = false;
    kernel.join();
    EXPECT_TRUE(started) << "'all' did not start a run";
    EXPECT_TRUE(stopped) << "the run of 'all' did not end when stopped";
}

TEST_F(MainWindowTest, ARunAskedForBeforeTheWelcomeIsCountedInTheKernelsUnit) {
    // What `enzan --gui --run 20ns` does: the window is asked for the run as it opens, before the
    // kernel has said that the run counts nanoseconds. Counted then, in the session's default of
    // picoseconds, it was a run of twenty thousand of them. Only one client may attach at a time,
    // so the fixture's steps aside.
    session->detach();
    pump();

    KernelSession late;
    MainWindow lateWindow(late, theme);
    QString error;
    ASSERT_TRUE(late.attach(socketPath, error)) << error.toStdString();
    ASSERT_FALSE(late.isWelcomed()) << "the greeting is a round trip away";
    lateWindow.run(QStringLiteral("20ns"));

    EXPECT_TRUE(pumpUntil([&late] {
        return late.isWelcomed() && late.currentTime() == 20
               && late.state() != SimulationState::Running;
    })) << late.currentTime();
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

TEST_F(MainWindowTest, ASubmenuOpensAsAChildOfTheMenuItIsOpenedFrom) {
    // Radix and Highlight are entries of the wave list's own menu and of Edit. On Wayland a menu
    // must be the child of the menu it opens from, and one given the main window instead is
    // moved there by Qt with a warning on the terminal. Each is opened from both, the second
    // time after the first has closed it, since Qt makes a menu's window again as it reopens.
    QTreeView* rows = waveRows();
    ASSERT_NE(rows, nullptr);
    QMenu* edit = nullptr;
    for (const QAction* entry : window->menuBar()->actions()) {
        if (entry->text() == QStringLiteral("&Edit")) edit = entry->menu();
    }
    ASSERT_NE(edit, nullptr);

    // The wave list's menu, built from its actions as Qt builds it on a right-click, and titled
    // only to be named below.
    QMenu context(QStringLiteral("the wave list's menu"), rows);
    context.addActions(rows->actions());

    // A main window with no native window of its own could not be anybody's parent.
    window->show();

    for (QMenu* host : {edit, &context}) {
        for (const QString& title : {QStringLiteral("Radix"), QStringLiteral("Highlight")}) {
            QAction* entry = nullptr;
            for (QAction* candidate : host->actions()) {
                if (candidate->menu() != nullptr && candidate->text() == title) entry = candidate;
            }
            ASSERT_NE(entry, nullptr) << title.toStdString();

            host->popup(window->mapToGlobal(QPoint(20, 20)));
            host->setActiveAction(entry);
            QMenu* submenu = entry->menu();
            ASSERT_TRUE(submenu->isVisible()) << title.toStdString();
            ASSERT_NE(submenu->windowHandle(), nullptr);
            EXPECT_EQ(submenu->windowHandle()->transientParent(), host->windowHandle())
                << title.toStdString() << " opened from " << host->title().toStdString();

            submenu->hide();
            host->hide();
        }
    }
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
