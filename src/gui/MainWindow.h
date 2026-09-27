// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/DesignTreeModel.h"
#include "gui/KernelSession.h"
#include "gui/ObjectsModel.h"
#include "gui/SourceView.h"
#include "gui/Theme.h"
#include "gui/TranscriptView.h"
#include "gui/WaveformModel.h"
#include "gui/WaveformView.h"

#include <QMainWindow>
#include <QString>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QTabWidget;
class QTableView;
class QTreeView;

namespace enzan::gui {

/// The Enzan window.
///
/// The layout follows Modelsim, which is what users of this kind of tool expect (section 10.2):
/// Structure, Objects, Wave and Transcript, docked with `QDockWidget` so the arrangement can be
/// rearranged and is remembered between sessions.
///
/// The Processes window of section 10.2 is not here yet. The Source window is, with the
/// breakpoints and stepping that make it a debugger rather than a file viewer: those are what a
/// source window is *for*, which is why it did not exist before M3.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// `session` and `theme` must outlive the window.
    MainWindow(KernelSession& session, const Theme& theme, QWidget* parent = nullptr);

    /// Writes a note into the transcript, for the messages the front end makes itself.
    void note(const QString& text);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // Plain member functions rather than slots: every connection below uses the
    // function-pointer form, which needs no annotation and checks signatures at compile time.
    void onWelcomed();
    void onDesignChanged();
    void onObjectsReceived(const QString& instancePath,
                           const QVector<enzan::gui::SignalInfo>& objects);
    void onRunStateChanged();
    void onWavesChanged();
    void onWavesCleared();
    void onDetached(const QString& reason);
    void onRequestFailed(const QString& reason);
    void onBreakpointsChanged();
    void onSourceReceived(const QString& path, const QString& text, bool found);

    void onScopeSelected(const QModelIndex& index);
    void onCursorsChanged();

    void addSelectedObjectsToWave();

    /// Adds the signals of the scope the reader right-clicked in the Structure window.
    ///
    /// `recursive` takes everything below it as well; `grouped` puts each scope's signals in a
    /// group of their own, named by the scope's path.
    ///
    /// Grouped recursion is deliberately *flat*: one group per module, siblings of each other,
    /// rather than groups nested the way the design is. The hierarchy is what the Structure
    /// window is for, and a reader who asks a subtree into the wave window is asking to compare
    /// its modules side by side — which nesting would put behind a disclosure arrow each.
    void insertScopeSignals(bool recursive, bool grouped);

    /// Adds a scope's signals to `group`, or to the top level when it is -1.
    void insertObjectsIntoWave(const QVector<enzan::gui::SignalInfo>& objects, int group);

    void groupSelectedWaves();

    /// Opens the name editor on the row the reader is on.
    ///
    /// The same editor `F2` opens, rather than a dialog of its own: renaming is an edit of the
    /// name column, and offering it twice in two different shapes would be two things to keep
    /// in step for no gain.
    void renameCurrentWave();

    /// Shows or hides the contents of every selected row that has contents to show.
    ///
    /// One action rather than two, because a row is either packed or unpacked and a menu
    /// offering both would always have one entry that did nothing. Unpacking a vector for the
    /// first time creates its bits; after that this is the same expand and collapse the
    /// disclosure arrow does (DESIGN_GUIDE.md section 10.7).
    void togglePackSelectedWaves();

    void removeSelectedWaves();
    void setSelectedWaveRadix(eir::Radix radix);
    void setSelectedHighlight(int slot);
    void copySelectedWaves();
    void pasteWaves();
    void addCursor();

    void runForRequestedTime();
    void runUntilFinished();
    void stopRun();
    void restartRun();
    void stepOneLine();

    /// Puts a breakpoint on a line, or reports why it could not go there.
    void requestBreakpoint(const QString& file, std::uint32_t line);

    /// Opens the source of the scope the reader clicked, at its declaration.
    void showSourceOfScope(const QString& instancePath);

    /// Writes a value into the selected signal, asking for it first.
    ///
    /// One dialog for both, with `force` deciding which of them it performs, because the two
    /// differ only in whether the value is held afterwards and a reader choosing between them
    /// is choosing between two sentences rather than two operations.
    void depositOrForceSelectedObject(bool force);

    /// Releases every selected signal that is being forced.
    void releaseSelectedObjects();

    void buildDocks();
    void buildToolBar();
    void buildMenus();
    void updateActions();
    void updateSubscription();
    void restoreLayout();
    void saveLayout();

    /// The wave rows the user has selected, as model row numbers.
    [[nodiscard]] QVector<int> selectedWaveRows() const;

    KernelSession* session;
    const Theme* theme;

    DesignTreeModel* designModel = nullptr;
    ObjectsModel* objectsModel = nullptr;
    WaveformModel* waveModel = nullptr;

    QTreeView* structureView = nullptr;
    QTableView* objectsView = nullptr;
    QTreeView* waveRowsView = nullptr;
    WaveformView* waveView = nullptr;
    TranscriptView* transcriptView = nullptr;
    SourceView* sourceView = nullptr;

    /// The central area's tabs: Wave and Source. Each needs the whole width, so they share it
    /// in turn rather than side by side.
    QTabWidget* panes = nullptr;

    QLineEdit* runLengthEdit = nullptr;
    QLabel* statusTime = nullptr;
    QLabel* statusState = nullptr;
    QLabel* statusCursor = nullptr;

    QAction* runAction = nullptr;
    QAction* runAllAction = nullptr;
    QAction* stopAction = nullptr;
    QAction* restartAction = nullptr;
    QAction* followAction = nullptr;
    QAction* stepAction = nullptr;

    /// The signals currently subscribed to, so that a change of what is displayed sends only
    /// the difference rather than resubscribing to everything.
    QVector<std::uint32_t> subscribed;

    /// The scope the Objects window is showing, so that a reply meant for an insert does not
    /// replace its contents.
    ///
    /// A scope's objects arrive by the one message whoever asked for them, and a recursive
    /// insert asks about every scope in a subtree. Without this, the pane would end up showing
    /// whichever module answered last — a module the reader never clicked.
    QString objectsScope;

    /// A scope whose objects have been asked for on behalf of an insert, and the group they are
    /// destined for.
    ///
    /// Insertion cannot be done where the menu is clicked: the objects of a scope come from the
    /// kernel, which may be on another machine (DESIGN_GUIDE.md section 2.2). The *groups* are
    /// made immediately and in tree order, though, so that they sit in the order the Structure
    /// window shows however the replies are interleaved; only their contents wait.
    struct PendingInsert {
        QString scope;

        /// The group to fill, or -1 to add at the top level.
        int group = -1;
    };

    QVector<PendingInsert> pendingInserts;
};

} // namespace enzan::gui
