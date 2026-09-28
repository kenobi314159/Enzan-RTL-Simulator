// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/MainWindow.h"

#include "eir/BitOps.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPixmap>
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
};

} // namespace

MainWindow::MainWindow(KernelSession& kernelSession, const Theme& themeToUse, QWidget* parent)
    : QMainWindow(parent), session(&kernelSession), theme(&themeToUse) {
    setWindowTitle(tr("Enzan"));

    designModel = new DesignTreeModel(this);
    objectsModel = new ObjectsModel(*session, this);
    waveModel = new WaveformModel(*session, *theme, this);

    buildDocks();
    buildToolBar();
    buildMenus();

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
    // Four entries rather than two with modifiers, because each is a different question — this
    // module or its subtree, kept together or not — and a menu that hides half of its answers
    // behind a held key is a menu that only its author knows the whole of.
    struct ScopeInsert {
        const char* label;
        bool recursive;
        bool grouped;
    };

    for (const ScopeInsert& entry : {ScopeInsert{"Add to wave", false, true},
                                     ScopeInsert{"Add to wave recursive", true, true}}) {
        auto* action = new QAction(tr(entry.label), structureView);
        const bool recursive = entry.recursive;
        const bool grouped = entry.grouped;
        connect(action, &QAction::triggered, this,
                [this, recursive, grouped] { insertScopeSignals(recursive, grouped); });
        structureView->addAction(action);
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
    objectsView->addAction(addToWave);

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

    waveView = new WaveformView(*session, *waveModel, *theme, this);
    waveView->setRowView(waveRowsView);
    connect(waveView, &WaveformView::cursorsChanged, this, &MainWindow::onCursorsChanged);

    // The canvas has no vertical scroll bar of its own; the tree's is the one that matters, so
    // the tree keeps it and the canvas follows.
    waveRowsView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    auto* waveSplitter = new QSplitter(Qt::Horizontal, this);
    waveSplitter->addWidget(waveRowsView);
    waveSplitter->addWidget(waveView);
    waveSplitter->setStretchFactor(0, 0);
    waveSplitter->setStretchFactor(1, 1);
    waveSplitter->setSizes({340, 900});

    // Wave and Source are tabs of the central area rather than a dock beside it, which is what
    // Modelsim does and what the two need: each wants the whole width. A waveform squeezed
    // beside an eighty-column listing leaves too little of either to read, and a reader is
    // looking at one or the other.
    panes = new QTabWidget(this);
    panes->setDocumentMode(true);
    panes->addTab(waveSplitter, tr("Wave"));
    setCentralWidget(panes);

    const auto addWaveAction = [this](const QString& text, const QKeySequence& shortcut,
                                      auto slot) {
        auto* action = new QAction(text, waveRowsView);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, slot);
        waveRowsView->addAction(action);
        return action;
    };

    addWaveAction(tr("New Group"), QKeySequence(Qt::CTRL | Qt::Key_G),
                  &MainWindow::groupSelectedWaves);
    addWaveAction(tr("Rename…"), QKeySequence(Qt::Key_F2), &MainWindow::renameCurrentWave);
    addWaveAction(tr("Pack/Unpack"), QKeySequence(Qt::CTRL | Qt::Key_U),
                  &MainWindow::togglePackSelectedWaves);
    addWaveAction(tr("Remove"), QKeySequence(Qt::Key_Delete), &MainWindow::removeSelectedWaves);

    auto* clipboardSeparator = new QAction(waveRowsView);
    clipboardSeparator->setSeparator(true);
    waveRowsView->addAction(clipboardSeparator);

    addWaveAction(tr("Copy"), QKeySequence::Copy, &MainWindow::copySelectedWaves);
    addWaveAction(tr("Paste"), QKeySequence::Paste, &MainWindow::pasteWaves);

    auto* radixSeparator = new QAction(waveRowsView);
    radixSeparator->setSeparator(true);
    waveRowsView->addAction(radixSeparator);

    // Symbolic first, because it is what an enumerated signal shows by default and a reader
    // looking for the way back to it should not have to read past four radixes to find it. It
    // does nothing to a row whose type is not an enumeration, which is the honest answer: there
    // are no names to show.
    auto* symbolic = new QAction(tr("Radix: Symbolic"), waveRowsView);
    connect(symbolic, &QAction::triggered, this, [this] {
        for (const int rowId : selectedWaveRows()) waveModel->setSymbolicRecursive(rowId, true);
        waveView->update();
    });
    waveRowsView->addAction(symbolic);

    for (const RadixChoice& choice : RadixChoices) {
        auto* action = new QAction(tr("Radix: %1").arg(tr(choice.name)), waveRowsView);
        const eir::Radix radix = choice.radix;
        connect(action, &QAction::triggered, this, [this, radix] { setSelectedWaveRadix(radix); });
        waveRowsView->addAction(action);
    }

    auto* highlightSeparator = new QAction(waveRowsView);
    highlightSeparator->setSeparator(true);
    waveRowsView->addAction(highlightSeparator);

    // The palette is the theme's (section 10.5), so each entry carries its actual colour: five
    // menu items called "Highlight 3" would tell a reader nothing about what they would get.
    for (int slot = 1; slot <= HighlightSlots; ++slot) {
        auto* action = new QAction(tr("Highlight %1").arg(slot), waveRowsView);

        QPixmap swatch(12, 12);
        swatch.fill(theme->color(highlightColor(slot)));
        action->setIcon(QIcon(swatch));

        connect(action, &QAction::triggered, this, [this, slot] { setSelectedHighlight(slot); });
        waveRowsView->addAction(action);
    }

    auto* unhighlight = new QAction(tr("Unhighlight"), waveRowsView);
    connect(unhighlight, &QAction::triggered, this, [this] { setSelectedHighlight(0); });
    waveRowsView->addAction(unhighlight);

    // --- Transcript --------------------------------------------------------

    transcriptView = new TranscriptView(*theme, this);

    auto* transcriptDock = new QDockWidget(tr("Transcript"), this);
    transcriptDock->setObjectName(QStringLiteral("transcript"));
    transcriptDock->setWidget(transcriptView);
    addDockWidget(Qt::BottomDockWidgetArea, transcriptDock);
}

void MainWindow::buildToolBar() {
    auto* bar = addToolBar(tr("Simulation"));
    bar->setObjectName(QStringLiteral("simulation"));
    bar->setMovable(true);

    runAction = bar->addAction(tr("Run"), this, &MainWindow::runForRequestedTime);
    runAction->setShortcut(QKeySequence(Qt::Key_F5));

    runLengthEdit = new QLineEdit(QStringLiteral("100"), bar);
    runLengthEdit->setFixedWidth(80);
    runLengthEdit->setToolTip(tr("How long each Run advances the simulation, in time units"));
    bar->addWidget(runLengthEdit);

    runAllAction = bar->addAction(tr("Run All"), this, &MainWindow::runUntilFinished);
    stopAction = bar->addAction(tr("Stop"), this, &MainWindow::stopRun);
    restartAction = bar->addAction(tr("Restart"), this, &MainWindow::restartRun);

    bar->addSeparator();
    bar->addAction(tr("Zoom Fit"), waveView, &WaveformView::zoomFit);
    bar->addAction(tr("Zoom In"), waveView, &WaveformView::zoomIn);
    bar->addAction(tr("Zoom Out"), waveView, &WaveformView::zoomOut);
    bar->addAction(tr("Zoom Cursors"), waveView, &WaveformView::zoomToCursors);

    bar->addSeparator();
    bar->addAction(tr("Add Cursor"), this, &MainWindow::addCursor);

    followAction = bar->addAction(tr("Follow"));
    followAction->setCheckable(true);
    followAction->setChecked(true);
    followAction->setToolTip(tr("Keep the right-hand edge at the latest simulated time"));
    connect(followAction, &QAction::toggled, waveView, &WaveformView::setFollowTail);
}

void MainWindow::buildMenus() {
    QMenu* simulation = menuBar()->addMenu(tr("&Simulation"));
    simulation->addAction(runAction);
    simulation->addAction(runAllAction);
    simulation->addAction(stopAction);
    simulation->addAction(restartAction);
    simulation->addSeparator();

    // Detaching and quitting are separate on purpose: section 2.2 requires a front end to be
    // able to leave a kernel running, and a single Quit that always killed it would make a
    // long run on a compute server impossible to look in on safely.
    simulation->addAction(tr("Detach from Kernel"), this, [this] {
        session->detach();
        close();
    });
    simulation->addAction(tr("Quit Kernel and Exit"), this, [this] {
        session->quitKernel();
        close();
    });

    QMenu* wave = menuBar()->addMenu(tr("&Wave"));
    wave->addAction(tr("Add Cursor"), this, &MainWindow::addCursor);
    wave->addAction(tr("Lock Active Cursor"), this, [this] {
        const int index = waveView->activeCursor();
        if (index < 0 || index >= waveView->cursors().size()) return;
        waveView->setCursorLocked(index, !waveView->cursors()[index].locked);
    });
    wave->addAction(tr("Remove Active Cursor"), this,
                    [this] { waveView->removeCursor(waveView->activeCursor()); });
    wave->addSeparator();
    wave->addAction(tr("Clear Wave Window"), this, [this] { waveModel->clear(); });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    for (const QDockWidget* dock : findChildren<QDockWidget*>()) {
        view->addAction(dock->toggleViewAction());
    }
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
    // A new design's scopes are not the old one's, so an insert still waiting on a reply is
    // waiting for a scope that may no longer exist, and its group belongs to a window that is
    // about to be rebuilt.
    pendingInserts.clear();
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
    // Anything waiting on this scope is served first. More than one insert may be — a reader
    // can ask for a subtree twice — so every match is taken rather than the first.
    for (int index = 0; index < pendingInserts.size();) {
        if (pendingInserts[index].scope != instancePath) {
            ++index;
            continue;
        }

        const int group = pendingInserts[index].group;
        pendingInserts.removeAt(index);
        insertObjectsIntoWave(objects, group);
    }

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

    for (const QModelIndex& index : selected) {
        const SignalInfo* info = objectsModel->objectAt(index.row());
        if (info != nullptr) waveModel->addSignal(*info);
    }

    // Deliberately nothing else. Expanding everything here — which is what this used to do —
    // unpacked every bus in the window each time a signal was added, throwing away whatever
    // the reader had chosen to have open. A new signal arrives packed, and what was already
    // there stays as it was.
}

void MainWindow::insertScopeSignals(bool recursive, bool grouped) {
    const QString root = designModel->pathAt(structureView->currentIndex());
    if (root.isEmpty()) return;

    // The scopes to take, in the order the kernel sent the tree — which is elaboration order,
    // and is the order the Structure window shows them in. Taken from the tree the window
    // already holds rather than asked for, so that the set is decided at once and the groups
    // can be made in it before any reply arrives.
    QStringList scopes;
    if (!recursive) {
        scopes << root;
    } else {
        const QString prefix = root + QLatin1Char('/');
        for (const ipc::InstanceEntry& instance : session->instances()) {
            const QString path = QString::fromStdString(instance.path);

            // The prefix is compared with the separator attached, so that `/tb/u_dut` does not
            // take `/tb/u_dut2` with it.
            if (path == root || path.startsWith(prefix)) scopes << path;
        }
    }

    for (const QString& scope : scopes) {
        // The group exists before its contents do, and every group of one insert is made before
        // any reply can arrive, which is what puts them in tree order rather than in the order
        // the kernel happened to answer. Flat, by the argument on `insertScopeSignals`.
        const int group = grouped ? waveModel->addGroup(scope) : -1;
        pendingInserts.append(PendingInsert{.scope = scope, .group = group});
        session->queryObjects(scope);
    }
}

void MainWindow::insertObjectsIntoWave(const QVector<SignalInfo>& objects, int group) {
    for (const SignalInfo& info : objects) {
        waveModel->addSignal(info, group);
    }

    // A module that declares no signals leaves no group behind. A row named after a scope with
    // nothing in it says only that the scope was asked about, which is not worth a line.
    if (group < 0) return;
    const WaveRow* groupRow = waveModel->row(group);
    if (groupRow != nullptr && groupRow->children.isEmpty()) waveModel->removeRow(group);
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

void MainWindow::copySelectedWaves() {
    const QVector<int> selected = selectedWaveRows();
    if (selected.isEmpty()) return;

    waveModel->copyRows(selected);
    note(tr("Copied %n row(s)", nullptr, static_cast<int>(selected.size())));
}

void MainWindow::pasteWaves() {
    if (!WaveformModel::clipboardHasRows()) {
        note(tr("There are no wave rows on the clipboard"));
        return;
    }

    // Pasted beside whatever is current — inside it when that is a group — because that is
    // where the reader is looking, and at the end of the window when nothing is.
    const int current = waveModel->rowIdOf(waveRowsView->currentIndex());
    const WaveRow* currentRow = waveModel->row(current);

    int targetGroup = -1;
    int position = -1;
    if (currentRow != nullptr) {
        if (!currentRow->children.isEmpty() || currentRow->kind == WaveRow::Kind::Group) {
            targetGroup = current;
        } else {
            targetGroup = currentRow->parent;
            position = waveModel->indexOfRow(current).row() + 1;
        }
    }

    const QVector<int> pasted = waveModel->pasteRows(targetGroup, position);
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
    bool valid = false;
    const quint64 duration = runLengthEdit->text().trimmed().toULongLong(&valid);
    if (!valid || duration == 0) {
        note(tr("'%1' is not a run length in whole time units").arg(runLengthEdit->text()));
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

    // A kernel this front end started has no reason to outlive it; one the user attached to is
    // theirs, and is left running. Getting this backwards would either orphan a process on
    // every exit or kill a simulation somebody was in the middle of.
    if (session->isAttached()) {
        if (session->ownsKernel()) {
            session->quitKernel();
        } else {
            session->detach();
        }
    }

    QMainWindow::closeEvent(event);
}

} // namespace enzan::gui
