// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/DesignTreeModel.h"
#include "gui/KernelSession.h"
#include "gui/ObjectsModel.h"
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
class QTableView;
class QTreeView;

namespace enzan::gui {

/// The Enzan window.
///
/// The layout follows Modelsim, which is what users of this kind of tool expect (section 10.2):
/// Structure, Objects, Wave and Transcript, docked with `QDockWidget` so the arrangement can be
/// rearranged and is remembered between sessions.
///
/// The Source and Processes windows of section 10.2 are not here. Source browsing is only
/// useful with the breakpoints and stepping that make it a debugger rather than a file viewer,
/// and those arrive with M3; a window that showed source and could do nothing with it would be
/// a worse answer than an editor the user already has.
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

    void onScopeSelected(const QModelIndex& index);
    void onCursorsChanged();

    void addSelectedObjectsToWave();
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

    QLineEdit* runLengthEdit = nullptr;
    QLabel* statusTime = nullptr;
    QLabel* statusState = nullptr;
    QLabel* statusCursor = nullptr;

    QAction* runAction = nullptr;
    QAction* runAllAction = nullptr;
    QAction* stopAction = nullptr;
    QAction* restartAction = nullptr;
    QAction* followAction = nullptr;

    /// The signals currently subscribed to, so that a change of what is displayed sends only
    /// the difference rather than resubscribing to everything.
    QVector<std::uint32_t> subscribed;
};

} // namespace enzan::gui
