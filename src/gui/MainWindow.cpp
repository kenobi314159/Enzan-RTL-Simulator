// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/MainWindow.h"

#include "common/Diagnostics.h"
#include "common/Version.h"
#include "eir/BitOps.h"
#include "gui/TclSyntax.h"
#include "runtime/Time.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

namespace enzan::gui {

namespace {

/// Settings keys for the remembered layout.
constexpr const char* GeometryKey = "layout/geometry";
constexpr const char* WindowStateKey = "layout/state";

/// Radix names, in the order the menus offer them. Hexadecimal is first because it is the
/// default for multi-bit values (section 10.3).
struct RadixChoice {
    const char* name;
    eir::Radix radix;
};

constexpr RadixChoice RadixChoices[] = {
    {.name = "Hexadecimal", .radix = eir::Radix::Hexadecimal},
    {.name = "Decimal", .radix = eir::Radix::Decimal},
    {.name = "Octal", .radix = eir::Radix::Octal},
    {.name = "Binary", .radix = eir::Radix::Binary},
    {.name = "ASCII", .radix = eir::Radix::Ascii},
};

/// Makes `action` one of `scope`'s, its keys acting while the focus is anywhere inside `scope`
/// and nowhere else.
///
/// Qt's default would make them keys of the whole window, and Delete pressed in the Structure
/// window removed whatever was selected in the wave list.
QAction* scopeTo(QAction* action, QWidget* scope) {
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    scope->addAction(action);
    return action;
}

/// Puts an action's key in its tooltip, which Qt does not do by itself. A toolbar button is where
/// a reader first meets a command, and its key is worth meeting there too.
void showKeyInToolTip(QAction* action) {
    if (action->shortcut().isEmpty()) return;
    action->setToolTip(
        QStringLiteral("%1 (%2)").arg(action->toolTip(), keyText(action->shortcut())));
}

/// The desktop's font, as it was before any theme was applied, which every size a theme gives is
/// worked out from.
///
/// Read once, before the first theme replaces it: afterwards the application's font is the last
/// theme's, and a size worked out from there would keep what a theme set after a theme that sets
/// none had replaced it.
const QFont& desktopFont() {
    static const QFont font = QApplication::font();
    return font;
}

} // namespace

MainWindow::MainWindow(KernelSession& kernelSession, Theme initialTheme,
                       ConfigurationDirectory settingsDirectory, QWidget* parent)
    : QMainWindow(parent),
      session(&kernelSession),
      theme(std::move(initialTheme)),
      configuration(std::move(settingsDirectory)),
      themeName(configuration.read().theme) {
    setWindowTitle(tr("Enzan"));

    designModel = new DesignTreeModel(this);
    objectsModel = new ObjectsModel(*session, this);
    waveModel = new WaveformModel(*session, theme, this);

    buildDocks();
    buildToolBar();
    buildMenus();
    applyTheme();

    statusTime = new QLabel(this);
    statusState = new QLabel(this);
    statusCursor = new QLabel(this);
    statusBar()->addWidget(statusState, 1);
    statusBar()->addPermanentWidget(statusCursor);
    statusBar()->addPermanentWidget(statusTime);

    connect(session, &KernelSession::welcomed, this, &MainWindow::onWelcomed);
    connect(session, &KernelSession::designChanged, this, &MainWindow::onDesignChanged);
    connect(session, &KernelSession::objectsReceived, this, &MainWindow::onObjectsReceived);
    connect(session, &KernelSession::runStateChanged, this, &MainWindow::onRunStateChanged);
    connect(session, &KernelSession::wavesChanged, this, &MainWindow::onWavesChanged);
    connect(session, &KernelSession::wavesCleared, this, &MainWindow::onWavesCleared);
    connect(session, &KernelSession::detached, this, &MainWindow::onDetached);
    connect(session, &KernelSession::requestFailed, this, &MainWindow::onRequestFailed);
    connect(session, &KernelSession::transcriptReceived, transcriptView,
            &TranscriptView::appendOutput);
    connect(session, &KernelSession::diagnosticReceived, transcriptView,
            &TranscriptView::appendDiagnostic);

    connect(waveModel, &WaveformModel::displayedSignalsChanged, this,
            &MainWindow::updateSubscription);

    restoreLayout();

    // The window may be created before the session connects or after it already has. Which of
    // those happens is an accident of how the program was started, and it must not decide
    // whether anything is on screen, so whatever the session already knows is taken now.
    if (session->isAttached()) onWelcomed();
    if (!session->instances().isEmpty()) onDesignChanged();
    onRunStateChanged();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void MainWindow::buildDocks() {
    // --- Structure ---------------------------------------------------------

    structureView = new QTreeView(this);
    structureView->setModel(designModel);
    structureView->setHeaderHidden(false);
    structureView->setUniformRowHeights(true);
    structureView->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(structureView, &QTreeView::clicked, this, &MainWindow::onScopeSelected);

    // Adding a whole module's signals from here rather than selecting them in the Objects
    // window: the reader has just found the module in the hierarchy, and making them click it,
    // move to another pane, select everything and add it is four steps to say one thing.
    //
    // Two entries rather than one with a modifier, because each is a different question — this
    // module or its subtree — and a menu that hides half of its answers behind a held key is a
    // menu that only its author knows the whole of. Ctrl+W is the Objects window's key for adding
    // too, so the key means "add what is selected here" in either.
    struct ScopeInsert {
        const char* label = nullptr;
        QKeyCombination key;
        bool recursive = false;
    };

    for (const ScopeInsert& entry :
         {ScopeInsert{.label = "Add to Wave", .key = Qt::CTRL | Qt::Key_W, .recursive = false},
          ScopeInsert{.label = "Add to Wave Recursive",
                      .key = Qt::CTRL | Qt::SHIFT | Qt::Key_W,
                      .recursive = true}}) {
        auto* action = new QAction(tr(entry.label), structureView);
        action->setShortcut(QKeySequence(entry.key));
        const bool recursive = entry.recursive;
        connect(action, &QAction::triggered, this,
                [this, recursive] { insertScopeSignals(recursive); });
        scopeTo(action, structureView);
    }

    auto* structureDock = new QDockWidget(tr("Structure"), this);
    structureDock->setObjectName(QStringLiteral("structure"));
    structureDock->setWidget(structureView);
    addDockWidget(Qt::LeftDockWidgetArea, structureDock);

    // --- Objects -----------------------------------------------------------

    objectsView = new QTableView(this);
    objectsView->setModel(objectsModel);
    objectsView->setSelectionBehavior(QAbstractItemView::SelectRows);
    objectsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    objectsView->verticalHeader()->setVisible(false);
    objectsView->horizontalHeader()->setStretchLastSection(true);
    objectsView->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(objectsView, &QTableView::doubleClicked, this,
            [this](const QModelIndex&) { addSelectedObjectsToWave(); });

    auto* addToWave = new QAction(tr("Add to Wave"), objectsView);
    addToWave->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(addToWave, &QAction::triggered, this, &MainWindow::addSelectedObjectsToWave);
    scopeTo(addToWave, objectsView);

    // `examine`, `force` and `deposit` are required from the GUI as well as from Tcl (section
    // 10.4). Examining is what the window already does — the value column *is* an examine that
    // keeps itself up to date — so what is added here is the two that write.
    auto* writeSeparator = new QAction(objectsView);
    writeSeparator->setSeparator(true);
    objectsView->addAction(writeSeparator);

    auto* depositAction = new QAction(tr("Deposit…"), objectsView);
    depositAction->setToolTip(tr("Write a value now; the next thing that drives it overwrites"));
    connect(depositAction, &QAction::triggered, this,
            [this] { depositOrForceSelectedObject(false); });
    objectsView->addAction(depositAction);

    auto* forceAction = new QAction(tr("Force…"), objectsView);
    forceAction->setToolTip(tr("Hold a value against the design until it is released"));
    connect(forceAction, &QAction::triggered, this, [this] { depositOrForceSelectedObject(true); });
    objectsView->addAction(forceAction);

    auto* releaseAction = new QAction(tr("Release"), objectsView);
    connect(releaseAction, &QAction::triggered, this, &MainWindow::releaseSelectedObjects);
    objectsView->addAction(releaseAction);

    auto* objectsDock = new QDockWidget(tr("Objects"), this);
    objectsDock->setObjectName(QStringLiteral("objects"));
    objectsDock->setWidget(objectsView);
    addDockWidget(Qt::LeftDockWidgetArea, objectsDock);

    // --- Wave --------------------------------------------------------------
    //
    // The names and the traces are two widgets in a splitter rather than one widget painting
    // both. The tree then supplies the row geometry — through scrolling, through collapsing a
    // group and through a change of font — and the canvas never has to know about any of it.

    waveRowsView = new QTreeView(this);
    waveRowsView->setModel(waveModel);
    waveRowsView->setUniformRowHeights(true);
    waveRowsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    waveRowsView->setSelectionBehavior(QAbstractItemView::SelectRows);
    waveRowsView->setContextMenuPolicy(Qt::ActionsContextMenu);
    waveRowsView->setEditTriggers(QAbstractItemView::EditKeyPressed
                                  | QAbstractItemView::SelectedClicked);
    waveRowsView->header()->setStretchLastSection(true);

    // A name column wide enough for a name. Unpacking a bus indents its bits, and a tree that
    // elided `count[7]` to `count…` would make the one thing those rows exist to say the first
    // thing it dropped; the shallow indent buys back most of the same width.
    waveRowsView->setIndentation(12);
    waveRowsView->setColumnWidth(WaveformModel::NameColumn, 220);

    // Rows are reordered by dragging them, including into and out of groups. The order is the
    // reader's and nothing re-sorts it behind them (section 10.7).
    waveRowsView->setDragEnabled(true);
    waveRowsView->setAcceptDrops(true);
    waveRowsView->setDropIndicatorShown(true);
    waveRowsView->setDragDropMode(QAbstractItemView::InternalMove);
    waveRowsView->setDefaultDropAction(Qt::MoveAction);

    waveView = new WaveformView(*session, *waveModel, theme, this);
    waveView->setRowView(waveRowsView);
    connect(waveView, &WaveformView::cursorsChanged, this, &MainWindow::onCursorsChanged);

    // The canvas has no vertical scroll bar of its own; the tree's is the one that matters, so
    // the tree keeps it and the canvas follows.
    waveRowsView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    waveTab = new QSplitter(Qt::Horizontal, this);
    waveTab->addWidget(waveRowsView);
    waveTab->addWidget(waveView);
    waveTab->setStretchFactor(0, 0);
    waveTab->setStretchFactor(1, 1);
    waveTab->setSizes({340, 900});

    // Wave and Source are tabs of the central area rather than a dock beside it, which is what
    // Modelsim does and what the two need: each wants the whole width. A waveform squeezed
    // beside an eighty-column listing leaves too little of either to read, and a reader is
    // looking at one or the other.
    panes = new QTabWidget(this);
    panes->setDocumentMode(true);
    panes->addTab(waveTab, tr("Wave"));
    setCentralWidget(panes);

    // The wave list's commands, which are both its own menu and the window's Edit menu: one set
    // of actions in one order, so that the two menus cannot come to differ. Each acts on the
    // selected rows, and its key works while the focus is anywhere in the Wave tab, because the
    // names and the traces are one window to the reader (section 10.7).
    const auto addWaveAction = [this](const QString& text, const QKeySequence& shortcut,
                                      auto slot) {
        auto* action = new QAction(text, waveRowsView);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, slot);
        waveRowsView->addAction(action);
        return scopeTo(action, waveTab);
    };
    const auto addWaveSeparator = [this] {
        auto* separator = new QAction(waveRowsView);
        separator->setSeparator(true);
        waveRowsView->addAction(separator);
    };

    addWaveAction(tr("Rename…"), QKeySequence(Qt::Key_F2), &MainWindow::renameCurrentWave);
    addWaveAction(tr("New Group…"), QKeySequence(Qt::CTRL | Qt::Key_G),
                  &MainWindow::groupSelectedWaves);
    addWaveAction(tr("Pack/Unpack"), QKeySequence(Qt::CTRL | Qt::Key_U),
                  &MainWindow::togglePackSelectedWaves);
    addWaveSeparator();

    // A submenu each rather than an entry per choice: twelve lines for two decisions made the
    // row's menu longer than the window's.
    //
    // Symbolic first, because it is what an enumerated signal shows by default and a reader
    // looking for the way back to it should not have to read past four radixes to find it. It
    // does nothing to a row whose type is not an enumeration, which is the honest answer: there
    // are no names to show.
    auto* radixMenu = new QMenu(tr("Radix"), waveRowsView);
    connect(radixMenu->addAction(tr("Symbolic")), &QAction::triggered, this, [this] {
        for (const int rowId : selectedWaveRows()) waveModel->setSymbolicRecursive(rowId, true);
        waveView->update();
    });
    for (const RadixChoice& choice : RadixChoices) {
        const eir::Radix radix = choice.radix;
        connect(radixMenu->addAction(tr(choice.name)), &QAction::triggered, this,
                [this, radix] { setSelectedWaveRadix(radix); });
    }
    waveRowsView->addAction(radixMenu->menuAction());

    // The palette is the theme's (section 10.5), so each entry carries its actual colour, which
    // `applyTheme` paints, and is numbered as `-highlight` numbers it in a signal list file
    // (section 10.8).
    auto* highlightMenu = new QMenu(tr("Highlight"), waveRowsView);
    for (int slot = 1; slot <= HighlightSlots; ++slot) {
        QAction* entry = highlightMenu->addAction(QString::number(slot));
        connect(entry, &QAction::triggered, this, [this, slot] { setSelectedHighlight(slot); });
        highlightActions.at(static_cast<std::size_t>(slot - 1)) = entry;
    }
    connect(highlightMenu->addAction(tr("None")), &QAction::triggered, this,
            [this] { setSelectedHighlight(0); });
    waveRowsView->addAction(highlightMenu->menuAction());
    addWaveSeparator();

    addWaveAction(tr("Copy"), QKeySequence::Copy, &MainWindow::copySelectedWaves);
    addWaveAction(tr("Paste"), QKeySequence::Paste, &MainWindow::pasteWaves);
    addWaveAction(tr("Remove"), QKeySequence(Qt::Key_Delete), &MainWindow::removeSelectedWaves);

    // --- Transcript --------------------------------------------------------

    transcriptView = new TranscriptView(theme, this);

    auto* transcriptDock = new QDockWidget(tr("Transcript"), this);
    transcriptDock->setObjectName(QStringLiteral("transcript"));
    transcriptDock->setWidget(transcriptView);
    addDockWidget(Qt::BottomDockWidgetArea, transcriptDock);
}

void MainWindow::setTheme(const Theme& replacement) {
    theme = replacement;
    applyTheme();
}

void MainWindow::applyTheme() {
    // The palette carries nearly all of the theme, and the style sheet only what a palette
    // cannot say. Keeping backgrounds out of the style sheet is what lets the signal list and
    // the wave canvas move what they have already drawn when they scroll, rather than drawing
    // it all again (DESIGN_GUIDE.md section 10.5).
    QApplication::setPalette(theme.palette());
    qApp->setStyleSheet(theme.styleSheet());

    const QFont& desktop = desktopFont();
    QApplication::setFont(theme.font(ThemeFont::Window, desktop));

    // One font for the names and the traces. The tree's sets how tall a row is, and the canvas
    // measures its labels against its own, so a size given to only one of them would put each
    // trace beside a row of another height.
    const QFont waveFont = theme.font(ThemeFont::Wave, desktop);
    waveRowsView->setFont(waveFont);
    waveView->setFont(waveFont);

    for (int slot = 1; slot <= HighlightSlots; ++slot) {
        QPixmap swatch(12, 12);
        swatch.fill(theme.color(highlightColor(slot)));
        highlightActions.at(static_cast<std::size_t>(slot - 1))->setIcon(QIcon(swatch));
    }

    // A highlighted name is the model's background, read as the list paints, and a trace is a
    // picture the canvas keeps; each is told to look again.
    waveRowsView->viewport()->update();
    waveView->themeChanged();
    transcriptView->applyTheme();
}

void MainWindow::fillThemeMenu() {
    // The entries are the menu's and go with `clear`, and leave the group as they go; the two
    // commands below them are the window's and stay.
    themeMenu->clear();

    const auto offer = [this](const QString& label, const QString& name) {
        QAction* entry = themeMenu->addAction(label);
        entry->setCheckable(true);
        entry->setChecked(name == themeName);
        themeChoices->addAction(entry);
        connect(entry, &QAction::triggered, this, [this, name] { chooseTheme(name); });
    };

    offer(tr("Built-in"), QString());
    const QStringList themes = configuration.themes();
    for (const QString& name : themes) offer(QFileInfo(name).completeBaseName(), name);

    // A theme `config` names from somewhere other than `themes/` is the one in use all the same,
    // and is offered by the name `config` gives it.
    if (!themeName.isEmpty() && !themes.contains(themeName)) offer(themeName, themeName);

    themeMenu->addSeparator();
    themeMenu->addAction(reloadThemeAction);
    themeMenu->addAction(newThemeAction);
    newThemeAction->setEnabled(!configuration.path().isEmpty());
}

bool MainWindow::useTheme(const QString& name) {
    Theme loaded;
    loaded.loadBuiltIn();

    QString error;
    if (!name.isEmpty() && !loaded.loadFile(configuration.resolve(name), error)) {
        report(ConfigurationMessage{.text = error, .isError = true});
        note(tr("The theme is as it was."));
        return false;
    }

    setTheme(loaded);
    return true;
}

void MainWindow::chooseTheme(const QString& name) {
    if (!useTheme(name)) return;
    themeName = name;

    QString error;
    if (!configuration.setTheme(name, error)) {
        report(ConfigurationMessage{
            .file = configuration.configFile(),
            .text = tr("the theme is used until the window closes, and not kept: %1").arg(error)});
    }
}

void MainWindow::reloadTheme() {
    if (!useTheme(themeName)) return;
    note(themeName.isEmpty()
             ? tr("The built-in theme is in use.")
             : tr("The theme was read again from %1.").arg(configuration.resolve(themeName)));
}

void MainWindow::askForNewTheme() {
    auto* dialog = new QInputDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("New Theme"));
    dialog->setLabelText(tr("The built-in theme is written to a file of this name in %1, to be "
                            "edited, and used:")
                             .arg(configuration.resolve(QStringLiteral("themes"))));
    connect(dialog, &QInputDialog::textValueSelected, this, &MainWindow::createTheme);
    dialog->open();
}

void MainWindow::createTheme(const QString& name) {
    QString error;
    const std::optional<QString> written = configuration.writeTheme(name, error);
    if (!written) {
        report(ConfigurationMessage{.text = error, .isError = true});
        return;
    }

    chooseTheme(*written);
    note(tr("%1 is the built-in theme, written out to be edited. Reload Theme shows an edit.")
             .arg(configuration.resolve(*written)));
}

void MainWindow::buildToolBar() {
    auto* bar = addToolBar(tr("Toolbar"));
    bar->setObjectName(QStringLiteral("simulation"));
    bar->setMovable(true);

    // A debugger's keys, F5 to go and Shift+F5 to stop, and the window's rather than a pane's: a
    // run is started and stopped from wherever the reader happens to be. Restart is the hardest
    // of them to press by accident, because it throws away the recorded waveform.
    runAction = bar->addAction(tr("Run"), this, &MainWindow::runForRequestedTime);
    runAction->setShortcut(QKeySequence(Qt::Key_F5));

    runLengthEdit = new QLineEdit(QStringLiteral("100"), bar);
    runLengthEdit->setFixedWidth(80);
    runLengthEdit->setToolTip(tr("How long each Run advances the simulation: a count of the "
                                 "run's time unit, as 100, or a time with its unit, as 10ns or "
                                 "1.5us"));
    connect(runLengthEdit, &QLineEdit::returnPressed, runAction, &QAction::trigger);
    bar->addWidget(runLengthEdit);

    runAllAction = bar->addAction(tr("Run All"), this, &MainWindow::runUntilFinished);
    runAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F5));
    stopAction = bar->addAction(tr("Stop"), this, &MainWindow::stopRun);
    stopAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    restartAction = bar->addAction(tr("Restart"), this, &MainWindow::restartRun);
    restartAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F5));

    // Zooming in and out by steps is left to the wheel and the keys; fitting and zooming to the
    // cursors are the two a reader reaches for with the mouse already in hand.
    bar->addSeparator();
    zoomFitAction =
        scopeTo(bar->addAction(tr("Zoom Fit"), waveView, &WaveformView::zoomFit), waveTab);
    zoomFitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F));
    zoomCursorsAction = scopeTo(
        bar->addAction(tr("Zoom to Cursors"), waveView, &WaveformView::zoomToCursors), waveTab);
    zoomCursorsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));

    bar->addSeparator();
    addCursorAction =
        scopeTo(bar->addAction(tr("Add Cursor"), this, &MainWindow::addCursor), waveTab);
    addCursorAction->setShortcut(QKeySequence(Qt::Key_Insert));

    followAction = bar->addAction(tr("Follow Tail"));
    followAction->setIconText(tr("Follow"));
    followAction->setCheckable(true);
    followAction->setChecked(true);
    followAction->setToolTip(tr("Keep the right-hand edge at the latest simulated time"));
    connect(followAction, &QAction::toggled, waveView, &WaveformView::setFollowTail);

    for (QAction* action : {runAction, runAllAction, stopAction, restartAction, zoomFitAction,
                            zoomCursorsAction, addCursorAction}) {
        showKeyInToolTip(action);
    }
}

void MainWindow::buildMenus() {
    // --- File --------------------------------------------------------------

    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("Load Signal List…"), this, &MainWindow::chooseSignalListToLoad)
        ->setShortcut(QKeySequence::Open);
    file->addAction(tr("Save Signal List"), this, &MainWindow::saveSignalListToItsFile)
        ->setShortcut(QKeySequence::Save);
    file->addAction(tr("Save Signal List As…"), this, &MainWindow::chooseSignalListToSave)
        ->setShortcut(QKeySequence::SaveAs);
    file->addSeparator();

    // Closing the window only ever detaches, and is safe to give a key: whether the kernel goes
    // with it is decided by how the session was started (section 8.4). Ending the simulation is
    // an entry of its own with no key, because a kernel found with `--attach` is one somebody
    // meant to outlive the window.
    file->addAction(tr("Close Window"), this, [this] { close(); })
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    file->addAction(tr("End Simulation and Close"), this, [this] {
        session->quitKernel();
        close();
    });

    // --- Edit --------------------------------------------------------------

    // The wave list's own menu, the same actions in the same order (buildDocks).
    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addActions(waveRowsView->actions());

    // --- View --------------------------------------------------------------

    // Ctrl zooms and Shift pans, from the keyboard as on the wheel (section 10.7), which leaves
    // the bare keys to the signal list: `+` and `-` pack and unpack there, and letters find a row.
    QMenu* view = menuBar()->addMenu(tr("&View"));
    scopeTo(view->addAction(tr("Zoom In"), waveView, &WaveformView::zoomIn), waveTab)
        ->setShortcuts(
            {QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    scopeTo(view->addAction(tr("Zoom Out"), waveView, &WaveformView::zoomOut), waveTab)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    view->addAction(zoomFitAction);
    view->addAction(zoomCursorsAction);
    scopeTo(view->addAction(tr("Pan Left"), waveView, [this] { waveView->panStep(false); }),
            waveTab)
        ->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Left));
    scopeTo(view->addAction(tr("Pan Right"), waveView, [this] { waveView->panStep(true); }),
            waveTab)
        ->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Right));
    view->addAction(followAction);
    view->addSeparator();
    for (const QDockWidget* dock : findChildren<QDockWidget*>()) {
        view->addAction(dock->toggleViewAction());
    }
    for (const QToolBar* bar : findChildren<QToolBar*>()) view->addAction(bar->toggleViewAction());
    view->addSeparator();

    // The themes in the configuration directory, the built-in one first, and the two things a
    // reader editing one needs: a new file to edit, and the file in use read again (section 10.5).
    themeMenu = view->addMenu(tr("Theme"));
    themeChoices = new QActionGroup(themeMenu);
    reloadThemeAction = new QAction(tr("Reload Theme"), this);
    connect(reloadThemeAction, &QAction::triggered, this, &MainWindow::reloadTheme);
    newThemeAction = new QAction(tr("New Theme…"), this);
    connect(newThemeAction, &QAction::triggered, this, &MainWindow::askForNewTheme);
    connect(themeMenu, &QMenu::aboutToShow, this, &MainWindow::fillThemeMenu);
    fillThemeMenu();

    view->addSeparator();
    view->addAction(transcriptView->clearAction());

    // --- Cursor ------------------------------------------------------------

    // The keyboard's way to the cursors. A key cannot point at a cursor the way a right-click on
    // its label does, so these act on the active one, which is drawn heavier than the rest and is
    // the one Tab is already moving (section 10.7).
    QMenu* cursor = menuBar()->addMenu(tr("&Cursor"));
    cursor->addAction(addCursorAction);
    cursor->addSeparator();

    // Tab is the view's own key rather than these entries', because a shortcut would take it from
    // the name editor as well, where it finishes the edit. The menu is told the key after a tab
    // character, which shows it without binding it.
    cursor->addAction(tr("Next Edge") + u'\t'
                          + QKeySequence(Qt::Key_Tab).toString(QKeySequence::NativeText),
                      waveView, [this] { waveView->stepActiveCursorToEdge(true); });
    cursor->addAction(
        tr("Previous Edge") + u'\t'
            + QKeySequence(Qt::SHIFT | Qt::Key_Tab).toString(QKeySequence::NativeText),
        waveView, [this] { waveView->stepActiveCursorToEdge(false); });
    scopeTo(cursor->addAction(tr("Cursor to Start"), waveView,
                              [this] { waveView->placeActiveCursor(0); }),
            waveTab)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Home));
    scopeTo(cursor->addAction(tr("Cursor to End"), waveView,
                              [this] { waveView->placeActiveCursor(session->heldTime()); }),
            waveTab)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_End));
    cursor->addSeparator();

    // `,` and `.` are the only punctuation typed without AltGr or Shift on the common Latin
    // layouts, and on the US one they carry `<` and `>`, which is the direction each goes.
    scopeTo(cursor->addAction(tr("Previous Cursor"), waveView,
                              [this] { waveView->activateNeighbourCursor(false); }),
            waveTab)
        ->setShortcut(QKeySequence(Qt::Key_Comma));
    scopeTo(cursor->addAction(tr("Next Cursor"), waveView,
                              [this] { waveView->activateNeighbourCursor(true); }),
            waveTab)
        ->setShortcut(QKeySequence(Qt::Key_Period));

    QAction* lock = scopeTo(
        cursor->addAction(tr("Lock Active Cursor"), this, &MainWindow::toggleActiveCursorLock),
        waveTab);
    lock->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    QAction* remove = cursor->addAction(tr("Remove Active Cursor"), waveView, [this] {
        waveView->removeCursor(waveView->activeCursor());
    });

    // Each entry says what it will do, as the label's own menu does, rather than leaving the reader
    // to work out the active cursor's state from the colour of its line.
    connect(cursor, &QMenu::aboutToShow, this, [this, lock, remove] {
        const int index = waveView->activeCursor();
        const bool locked =
            index >= 0 && index < waveView->cursors().size() && waveView->cursors()[index].locked;
        lock->setText(locked ? tr("Unlock Active Cursor") : tr("Lock Active Cursor"));
        remove->setEnabled(waveView->cursors().size() > 1);
    });

    // --- Simulation --------------------------------------------------------

    QMenu* simulation = menuBar()->addMenu(tr("&Simulation"));
    simulation->addAction(runAction);
    simulation->addAction(runAllAction);
    simulation->addAction(stopAction);
    simulation->addAction(restartAction);

    // --- Help --------------------------------------------------------------

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Keyboard Shortcuts…"), this, &MainWindow::showShortcuts)
        ->setShortcut(QKeySequence::HelpContents);
    help->addAction(tr("About Enzan"), this, &MainWindow::showAbout);
}

void MainWindow::showShortcuts() {
    // Every menu of the window, then the commands only a pane's own menu offers. The wave list's
    // are the Edit menu's, so they are listed there and not again.
    QSet<const QAction*> seen;
    QVector<ShortcutSection> sections;
    for (const QAction* entry : menuBar()->actions()) {
        if (entry->menu() == nullptr) continue;
        QString title = entry->text();
        sections.append(sectionOf(title.remove(u'&'), entry->menu()->actions(), seen));
    }
    sections.append(sectionOf(tr("Structure"), structureView->actions(), seen));
    sections.append(sectionOf(tr("Objects"), objectsView->actions(), seen));
    sections.append({.title = tr("Wave window"), .entries = WaveformView::gestures()});

    if (shortcutSheet == nullptr) shortcutSheet = new ShortcutSheet(this);
    shortcutSheet->setSections(sections);
    shortcutSheet->show();
    shortcutSheet->raise();
    shortcutSheet->activateWindow();
}

void MainWindow::showAbout() {
    const QString kernel = session->isAttached() ? session->kernelVersion() : tr("not attached");
    const QString text =
        tr("<p><b>Enzan RTL Simulator</b></p><p>Window: %1<br>Kernel: %2</p>"
           "<p>Licensed under the GNU General Public License, version 3 or later.</p>")
            .arg(QString::fromStdString(versionLine()).toHtmlEscaped(), kernel.toHtmlEscaped());

    // Opened rather than run, so that no event loop of its own holds up the socket while it is
    // up, and the traces go on arriving behind it.
    auto* box =
        new QMessageBox(QMessageBox::NoIcon, tr("About Enzan"), text, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

// ---------------------------------------------------------------------------
// Session events
// ---------------------------------------------------------------------------

void MainWindow::onWelcomed() {
    setWindowTitle(tr("Enzan — %1").arg(session->topModule()));
    note(tr("Attached to %1, top level %2").arg(session->kernelVersion(), session->topModule()));
    updateActions();
}

void MainWindow::onDesignChanged() {
    // A new design's scopes are not the old one's, so a load still waiting on a reply is
    // waiting for a scope that may no longer exist. A file is said so, so that the reader knows
    // to load it again; a menu entry was about the old design and has nothing to redo.
    //
    // Only when the hierarchy really is different. The same tree arrives again whenever anything
    // asks for it, and dropping every load in flight each time would lose them for nothing.
    QStringList scopes;
    for (const ipc::InstanceEntry& instance : session->instances())
        scopes << QString::fromStdString(instance.path);
    if (scopes != knownScopes) {
        for (const PendingLoad& load : pendingLoads) {
            if (!load.file.isEmpty())
                note(tr("The design changed while %1 was loading; load it again").arg(load.file));
        }
        pendingLoads.clear();
        knownScopes = scopes;
    }
    objectsScope.clear();

    designModel->setInstances(session->instances());
    structureView->expandToDepth(1);

    // The top level is selected for the user, because opening a window onto an empty Objects
    // pane and expecting them to guess the first click is a poor greeting.
    if (!session->instances().isEmpty()) {
        const QString top = QString::fromStdString(session->instances().front().path);
        const QModelIndex index = designModel->indexOfPath(top);
        if (index.isValid()) {
            structureView->setCurrentIndex(index);
            onScopeSelected(index);
        }
    }

    // A list named before there was a design to find its paths in — on the command line, which
    // is read before the kernel has said anything.
    if (!deferredSignalList.isEmpty()) {
        const QString path = std::exchange(deferredSignalList, QString());
        static_cast<void>(loadSignalList(path));
    }
}

void MainWindow::onScopeSelected(const QModelIndex& index) {
    const QString path = designModel->pathAt(index);
    if (path.isEmpty()) return;

    // Recorded before the request, because the reply is what decides whether the pane is
    // repainted and a reply for some other scope must not be able to.
    objectsScope = path;
    session->queryObjects(path);
}

void MainWindow::onObjectsReceived(const QString& instancePath,
                                   const QVector<SignalInfo>& objects) {
    // Every load waiting on this scope is served first. More than one may be — a reader can
    // ask for a subtree twice — so each is given the answer rather than only the first.
    bool served = false;
    for (PendingLoad& load : pendingLoads) {
        if (!load.waiting.remove(instancePath)) continue;
        load.objects.insert(instancePath, objects);
        served = true;
    }
    if (served) applyFinishedLoads();

    // And the Objects window, but only when this is the scope it is showing. A recursive insert
    // asks about every scope in a subtree, and without this test the pane would end up showing
    // whichever of them answered last — a module the reader never clicked.
    if (instancePath != objectsScope) return;

    objectsModel->setObjects(instancePath, objects);
    objectsModel->setCursorTime(waveView->activeCursorTime());
    objectsView->resizeColumnsToContents();
}

void MainWindow::onRunStateChanged() {
    waveView->extentChanged();
    updateActions();

    statusTime->setText(
        tr("Now: %1").arg(TimeAxis::format(session->currentTime(), session->timeScale())));

    switch (session->state()) {
    case ipc::SimulationState::Idle: statusState->setText(tr("Ready")); break;
    case ipc::SimulationState::Running: statusState->setText(tr("Running…")); break;
    case ipc::SimulationState::Finished: statusState->setText(tr("Finished")); break;
    case ipc::SimulationState::Failed: statusState->setText(tr("Failed")); break;
    }
}

void MainWindow::onWavesChanged() {
    waveView->extentChanged();
    waveModel->refreshValues();
    objectsModel->refreshValues();
}

void MainWindow::onWavesCleared() {
    // The rows stay: the user asked to watch these signals, and a restart is them asking to
    // watch the same ones again from the beginning. Only the values are gone.
    note(tr("The design was restarted; the recorded waveform was discarded"));
    waveModel->refreshValues();
    objectsModel->refreshValues();
    waveView->extentChanged();
}

void MainWindow::onDetached(const QString& reason) {
    note(reason.isEmpty() ? tr("Disconnected from the kernel")
                          : tr("Disconnected from the kernel: %1").arg(reason));
    updateActions();
}

void MainWindow::onRequestFailed(const QString& reason) {
    note(tr("The kernel refused a request: %1").arg(reason));
}

void MainWindow::onCursorsChanged() {
    const runtime::SimTime time = waveView->activeCursorTime();
    waveModel->setCursorTime(time);
    objectsModel->setCursorTime(time);
    statusCursor->setText(tr("Cursor: %1").arg(TimeAxis::format(time, session->timeScale())));
}

void MainWindow::note(const QString& text) {
    transcriptView->appendNote(QStringLiteral("-- ") + text);
}

void MainWindow::report(const ConfigurationMessage& problem) {
    const DiagnosticSeverity severity =
        problem.isError ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
    transcriptView->appendDiagnostic(static_cast<int>(severity), problem.text, problem.file,
                                     problem.line);
}

// ---------------------------------------------------------------------------
// Wave window editing
// ---------------------------------------------------------------------------

QVector<int> MainWindow::selectedWaveRows() const {
    QVector<int> selected;
    for (const QModelIndex& index : waveRowsView->selectionModel()->selectedRows()) {
        const int rowId = waveModel->rowIdOf(index);
        if (rowId >= 0) selected.push_back(rowId);
    }
    return selected;
}

void MainWindow::addSelectedObjectsToWave() {
    const QModelIndexList selected = objectsView->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;

    QStringList paths;
    for (const QModelIndex& index : selected) {
        const SignalInfo* info = objectsModel->objectAt(index.row());
        if (info == nullptr) continue;
        waveModel->addSignal(*info);
        paths << quoteTclWord(info->path);
    }
    if (!paths.isEmpty()) echo(QStringLiteral("add wave ") + paths.join(u' '));

    // Deliberately nothing else. Expanding everything here — which is what this used to do —
    // unpacked every bus in the window each time a signal was added, throwing away whatever
    // the reader had chosen to have open. A new signal arrives packed, and what was already
    // there stays as it was.
}

void MainWindow::insertScopeSignals(bool recursive) {
    const QString root = designModel->pathAt(structureView->currentIndex());
    if (root.isEmpty()) return;

    SignalListEntry command;
    command.paths << root;
    command.recursive = recursive;

    echo(QStringLiteral("add wave ") + (recursive ? QStringLiteral("-r ") : QString())
         + quoteTclWord(root));

    PendingLoad load;
    load.entries.push_back(std::move(command));
    startLoad(std::move(load));
}

// ---------------------------------------------------------------------------
// Signal list files
// ---------------------------------------------------------------------------

void MainWindow::echo(const QString& command) {
    transcriptView->appendNote(command);
}

void MainWindow::reportSignalList(const QString& file, const SignalListMessage& message,
                                  bool isError) {
    const DiagnosticSeverity severity =
        isError ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
    transcriptView->appendDiagnostic(static_cast<int>(severity), message.text, file, message.line);
}

bool MainWindow::loadSignalList(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        note(tr("The signal list %1 could not be read: %2").arg(path, file.errorString()));
        return false;
    }

    // Read and checked now, whether or not there is a design yet, so that a mistake in the file
    // is reported as the window opens rather than whenever the kernel gets round to answering.
    ParsedSignalList parsed = parseSignalList(file.readAll());
    if (!parsed.errors.isEmpty()) {
        for (const SignalListMessage& message : parsed.errors)
            reportSignalList(path, message, true);
        note(tr("Nothing was loaded from %1").arg(path));
        return false;
    }

    if (session->instances().isEmpty()) {
        deferredSignalList = path;
        return true;
    }

    PendingLoad load;
    load.file = path;
    load.entries = std::move(parsed.entries);
    load.replace = true;
    startLoad(std::move(load));
    return true;
}

bool MainWindow::saveSignalList(const QString& path) {
    // Written aside and moved into place, so that a save that fails half-way leaves the list
    // that was there rather than half of the new one.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(waveModel->writeSignalList().toUtf8()) < 0
        || !file.commit()) {
        note(tr("The signal list could not be written to %1: %2").arg(path, file.errorString()));
        return false;
    }

    signalListFile = path;
    note(tr("Saved the signal list to %1").arg(path));
    return true;
}

void MainWindow::chooseSignalListToLoad() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Load Signal List"),
                                                      QFileInfo(signalListFile).path());
    if (!path.isEmpty()) static_cast<void>(loadSignalList(path));
}

void MainWindow::chooseSignalListToSave() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Signal List"), signalListFile);
    if (!path.isEmpty()) static_cast<void>(saveSignalList(path));
}

void MainWindow::saveSignalListToItsFile() {
    if (signalListFile.isEmpty()) {
        chooseSignalListToSave();
        return;
    }
    static_cast<void>(saveSignalList(signalListFile));
}

void MainWindow::startLoad(PendingLoad load) {
    const QStringList scopes = scopesNeededBy(load.entries, session->instances());
    load.waiting = QSet<QString>(scopes.cbegin(), scopes.cend());
    pendingLoads.push_back(std::move(load));

    for (const QString& scope : scopes) session->queryObjects(scope);
    applyFinishedLoads();
}

void MainWindow::applyFinishedLoads() {
    while (!pendingLoads.isEmpty() && pendingLoads.front().waiting.isEmpty()) {
        const PendingLoad load = pendingLoads.takeFirst();
        finishLoad(load);
    }
}

void MainWindow::finishLoad(const PendingLoad& load) {
    const ResolvedSignalList resolved =
        resolveSignalList(load.entries, session->instances(), load.objects);

    for (const SignalListMessage& message : resolved.warnings)
        reportSignalList(load.file, message, false);

    if (!resolved.errors.isEmpty()) {
        for (const SignalListMessage& message : resolved.errors)
            reportSignalList(load.file, message, true);
        if (!load.file.isEmpty()) note(tr("Nothing was loaded from %1").arg(load.file));
        return;
    }

    if (load.replace) waveModel->clear();

    // Inserted the way a paste inserts, so nothing already in the window collapses or expands
    // (DESIGN_GUIDE.md section 10.7). Groups are not expanded either: a list of two hundred
    // signals opened all at once is a wall, and the groups are there to be opened one by one.
    static_cast<void>(waveModel->insertSignalRows(resolved.rows, -1, -1));

    if (!load.file.isEmpty()) {
        signalListFile = load.file;
        note(tr("Loaded the signal list %1").arg(load.file));
    }
}

void MainWindow::depositOrForceSelectedObject(bool force) {
    const QModelIndexList selected = objectsView->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;

    const SignalInfo* info = objectsModel->objectAt(selected.front().row());
    if (info == nullptr) return;

    if (info->isParameter()) {
        // A parameter is a fact about the elaborated design rather than a piece of its state
        // (section 10.7); there is no storage to write.
        note(tr("%1 is a parameter, so it has no value to write").arg(info->path));
        return;
    }
    if (info->width == 0) {
        note(tr("%1 is a null array, so it has no bits to write").arg(info->path));
        return;
    }

    bool accepted = false;
    const QString entered =
        QInputDialog::getText(this, force ? tr("Force Value") : tr("Deposit Value"),
                              tr("Value for %1 (%2 bits):").arg(info->path).arg(info->width),
                              QLineEdit::Normal, QString(), &accepted);
    if (!accepted || entered.isEmpty()) return;

    // Parsed here rather than sent as text, because the kernel's message carries a value and
    // the reader should be told about a typo while the dialog they typed it in is still in
    // mind.
    const std::optional<eir::BitVector> parsed = eir::BitVector::parse(entered.toStdString());
    if (!parsed.has_value()) {
        note(tr("'%1' is not a value Enzan understands").arg(entered));
        return;
    }

    // Widened or narrowed to the signal, so that `5` works without the reader counting bits.
    eir::BitVector sized = eir::BitVector::filled(info->width, eir::Logic::Zero);
    eir::bitops::extend(sized.mutableRef(), parsed->ref(), false);

    if (force) {
        session->force(info->signalIndex, sized);
        note(tr("Forced %1 to %2").arg(info->path, entered));
        return;
    }

    session->deposit(info->signalIndex, sized);
    note(tr("Deposited %1 into %2").arg(entered, info->path));
}

void MainWindow::releaseSelectedObjects() {
    const QModelIndexList selected = objectsView->selectionModel()->selectedRows();
    for (const QModelIndex& index : selected) {
        const SignalInfo* info = objectsModel->objectAt(index.row());
        if (info == nullptr || info->isParameter()) continue;

        // Releasing something that was not forced is not an error, so nothing checks first: a
        // reader clearing a handful of rows should not have to remember which they forced.
        session->release(info->signalIndex);
    }
}

void MainWindow::groupSelectedWaves() {
    const QVector<int> selected = selectedWaveRows();
    if (selected.isEmpty()) return;

    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New Group"), tr("Group name:"),
                                               QLineEdit::Normal, tr("Group"), &accepted);
    if (!accepted || name.isEmpty()) return;

    // Qt tracks expansion by position in the tree, so a row that moves is collapsed by the
    // move. Which of them were open is recorded first and put back afterwards, or collecting
    // an unpacked bus into a group would quietly pack it.
    QVector<int> wereExpanded;
    for (const int rowId : selected) {
        if (waveRowsView->isExpanded(waveModel->indexOfRow(rowId))) wereExpanded.push_back(rowId);
    }

    const int group = waveModel->addGroup(name);
    for (const int rowId : selected) waveModel->moveToGroup(rowId, group);

    // Only the group just made, so that the rows the reader was collecting are visible where
    // they landed and nothing else in the window moves.
    waveRowsView->expand(waveModel->indexOfRow(group));
    for (const int rowId : wereExpanded) waveRowsView->expand(waveModel->indexOfRow(rowId));
}

void MainWindow::renameCurrentWave() {
    const QModelIndex current = waveRowsView->currentIndex();
    if (!current.isValid()) {
        note(tr("Select a row to rename"));
        return;
    }

    // Always the name column, whichever one the reader happened to click: the value column is
    // not editable, and an action that silently did nothing there would read as a bug.
    waveRowsView->edit(current.siblingAtColumn(WaveformModel::NameColumn));
}

void MainWindow::togglePackSelectedWaves() {
    const QVector<int> selected = selectedWaveRows();
    if (selected.isEmpty()) {
        note(tr("Select a row to pack or unpack"));
        return;
    }

    bool acted = false;
    for (const int rowId : selected) {
        // A row that has neither children nor bits to make them from has nothing to pack.
        if (!waveModel->showsChildren(rowId) && !waveModel->canUnpack(rowId)) continue;

        const QModelIndex index = waveModel->indexOfRow(rowId);
        if (!index.isValid()) continue;

        // Expanding is what creates a vector's bits the first time, through `fetchMore`, so
        // unpacking and showing are the same act rather than two that have to agree.
        if (waveRowsView->isExpanded(index)) {
            waveRowsView->collapse(index);
        } else {
            waveRowsView->expand(index);
        }
        acted = true;
    }

    if (!acted) note(tr("Nothing in the selection can be packed or unpacked"));
}

void MainWindow::removeSelectedWaves() {
    // Removed back to front, because removing a row renumbers nothing but does change what is
    // selected, and working forwards through a shrinking selection drops every other row.
    QVector<int> selected = selectedWaveRows();
    std::ranges::sort(selected, std::greater<>());
    for (const int rowId : selected) waveModel->removeRow(rowId);
}

void MainWindow::setSelectedWaveRadix(eir::Radix radix) {
    for (const int rowId : selectedWaveRows()) waveModel->setRadixRecursive(rowId, radix);
    waveView->update();
}

void MainWindow::addCursor() {
    waveView->addCursor(tr("Cursor %1").arg(waveView->cursors().size() + 1),
                        waveView->activeCursorTime());
}

void MainWindow::toggleActiveCursorLock() {
    const int index = waveView->activeCursor();
    if (index < 0 || index >= waveView->cursors().size()) return;
    waveView->setCursorLocked(index, !waveView->cursors()[index].locked);
}

void MainWindow::copySelectedWaves() {
    const QVector<int> selected = selectedWaveRows();
    if (selected.isEmpty()) return;

    waveModel->copySignalRows(selected);
    note(tr("Copied %n row(s)", nullptr, static_cast<int>(selected.size())));
}

void MainWindow::pasteWaves() {
    if (!WaveformModel::clipboardHasRows()) {
        note(tr("There are no wave rows on the clipboard"));
        return;
    }

    // Pasted after whatever is current, at its level, because that is where the reader is
    // looking, and at the end of the window when nothing is. After a group too, rather than
    // inside it: pasting into a group left no way to paste after one, while dragging already
    // puts rows inside a group.
    const int current = waveModel->rowIdOf(waveRowsView->currentIndex());
    const WaveRow* currentRow = waveModel->row(current);

    int targetGroup = -1;
    int position = -1;
    if (currentRow != nullptr) {
        targetGroup = currentRow->parent;
        position = waveModel->indexOfRow(current).row() + 1;
    }

    const QVector<int> pasted = waveModel->pasteSignalRows(targetGroup, position);
    if (pasted.isEmpty()) return;

    waveRowsView->setCurrentIndex(waveModel->indexOfRow(pasted.front()));
}

void MainWindow::setSelectedHighlight(int slot) {
    for (const int rowId : selectedWaveRows()) waveModel->setHighlight(rowId, slot);
    waveView->update();
}

void MainWindow::updateSubscription() {
    const QVector<std::uint32_t> wanted = waveModel->displayedSignals();

    QVector<std::uint32_t> added;
    for (const std::uint32_t signalIndex : wanted) {
        if (!subscribed.contains(signalIndex)) added.push_back(signalIndex);
    }

    QVector<std::uint32_t> removed;
    for (const std::uint32_t signalIndex : subscribed) {
        if (!wanted.contains(signalIndex)) removed.push_back(signalIndex);
    }

    // Only the difference is sent. Resubscribing to everything whenever a row is added would
    // make the kernel resend every trace's history each time, which on a long run is the most
    // expensive thing the protocol can be asked to do.
    session->subscribe(added);
    session->unsubscribe(removed);
    subscribed = wanted;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void MainWindow::runForRequestedTime() {
    // Read as `--run` reads its value, by the same function, so that what the one takes the other
    // does too (DESIGN_GUIDE.md section 8.5).
    const QString text = runLengthEdit->text().trimmed();
    const std::optional<runtime::TimeLength> length = runtime::parseTimeLength(text.toStdString());
    if (!length) {
        note(tr("'%1' is not a length of time: a count of the run's time unit, 100, or a number "
                "with its unit, 10ns or 1.5us")
                 .arg(text));
        return;
    }

    runtime::SimTime duration = 0;
    switch (session->timeScale().countOf(*length, duration)) {
    case runtime::LengthFit::Exact: break;
    case runtime::LengthFit::NotWhole:
        note(tr("'%1' is not a whole number of the run's time unit, %2")
                 .arg(text, QString::fromStdString(session->timeScale().unitName())));
        return;
    case runtime::LengthFit::TooLong:
        note(tr("'%1' is longer than a run counts").arg(text));
        return;
    }
    if (duration == 0) {
        note(tr("'%1' is no time at all, and a run has to advance the simulation").arg(text));
        return;
    }

    followAction->setChecked(true);
    session->run(duration);
}

void MainWindow::runUntilFinished() {
    followAction->setChecked(true);
    session->runUntilFinished();
}

void MainWindow::stopRun() {
    session->stop();
}

void MainWindow::restartRun() {
    session->restart();
}

void MainWindow::updateActions() {
    const bool attached = session->isAttached();
    const bool running = session->state() == ipc::SimulationState::Running;
    const bool over = session->state() == ipc::SimulationState::Finished
                      || session->state() == ipc::SimulationState::Failed;

    runAction->setEnabled(attached && !running && !over);
    runAllAction->setEnabled(attached && !running && !over);
    stopAction->setEnabled(attached && running);
    restartAction->setEnabled(attached && !running);
    runLengthEdit->setEnabled(attached && !running && !over);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MainWindow::restoreLayout() {
    const QSettings settings;
    const QByteArray geometry = settings.value(QLatin1StringView(GeometryKey)).toByteArray();
    const QByteArray state = settings.value(QLatin1StringView(WindowStateKey)).toByteArray();

    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    else
        resize(1400, 900);

    if (!state.isEmpty()) restoreState(state);
}

void MainWindow::saveLayout() {
    QSettings settings;
    settings.setValue(QLatin1StringView(GeometryKey), saveGeometry());
    settings.setValue(QLatin1StringView(WindowStateKey), saveState());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveLayout();

    // Detached from and never ended. A window only ever attaches now (DESIGN_GUIDE.md section
    // 8.4), so whether the kernel should outlive it is the kernel's decision: one started by
    // `--gui` waits on this process and goes when it does, and one found with `--attach` keeps
    // running. Ending it from here would take that decision away from the flag the user chose.
    if (session->isAttached()) session->detach();

    QMainWindow::closeEvent(event);
}

} // namespace enzan::gui
