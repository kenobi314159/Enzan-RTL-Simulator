// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/Configuration.h"
#include "gui/DesignTreeModel.h"
#include "gui/KernelSession.h"
#include "gui/ObjectsModel.h"
#include "gui/ShortcutSheet.h"
#include "gui/SignalList.h"
#include "gui/Theme.h"
#include "gui/TranscriptView.h"
#include "gui/WaveformModel.h"
#include "gui/WaveformView.h"
#include "runtime/Time.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QString>
#include <QStringList>

#include <array>
#include <vector>

class QAction;
class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QSplitter;
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
/// The Processes window of section 10.2 is not here yet.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// `kernelSession` must outlive the window. The window keeps its own copy of `initialTheme`,
    /// which `setTheme` replaces. `directory` is where the Theme menu finds themes and records the
    /// one chosen, and where the layout is kept (DESIGN_GUIDE.md section 10.9); none at all leaves
    /// the menu only the built-in theme and the window its first size.
    MainWindow(KernelSession& kernelSession, const Theme& initialTheme,
               ConfigurationDirectory directory = ConfigurationDirectory(),
               QWidget* parent = nullptr);

    /// Colours the window with `replacement`, as though it had opened with it.
    ///
    /// Everything that took a colour or a size from the old theme takes it from the new one:
    /// the palette and the style sheet, the fonts, the traces already drawn, the highlight
    /// swatches, and every line already in the transcript. Nothing else changes — not the rows,
    /// the cursors, the zoom or the run.
    void setTheme(const Theme& replacement);

    /// The theme the window is coloured with.
    [[nodiscard]] const Theme& currentTheme() const noexcept { return theme; }

    /// Writes a note into the transcript, for the messages the front end makes itself.
    void note(const QString& text);

    /// Reports a problem in the configuration directory (DESIGN_GUIDE.md section 10.9) in the
    /// transcript, against the file and line it is in.
    void report(const ConfigurationMessage& problem);

    /// Replaces the wave window's list with the signal list in `path` (DESIGN_GUIDE.md section
    /// 10.8).
    ///
    /// The file is read and checked here; its rows arrive once the kernel has described every
    /// scope it names, and not before the design itself has arrived, so this may be called
    /// as soon as the window exists. Returns false, having said why in the transcript, when the
    /// file cannot be read or has a mistake in it, and the list is then left as it was.
    bool loadSignalList(const QString& path);

    /// Writes the wave window's list to `path`. Returns false, having said why, when it cannot.
    bool saveSignalList(const QString& path);

    /// Runs the design for `text`, read as `--run` reads it (DESIGN_GUIDE.md section 8.5): a count
    /// of the run's time unit, a time with a unit of its own, or `all`, until the design finishes.
    ///
    /// What is no length of time is refused at once, with a note saying why. A run asked for
    /// before the kernel has welcomed the window waits for the welcome, because that is where the
    /// run's time unit comes from: `--run` is read as the window opens, before the kernel has said
    /// anything, and a time counted in a unit the kernel has not named would run for another
    /// length than the one asked for. So this may be called as soon as the window exists.
    void run(const QString& text);

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

    /// Adds the signals of the scope the reader right-clicked in the Structure window, in a
    /// group named by the scope's path. `recursive` takes everything below it as well, in
    /// groups nested the way the design nests its instances.
    ///
    /// Performed as the signal list command `add wave ?-r? <scope>`, which section 10.8 defines
    /// as this menu entry: the command is what runs, so the two cannot come to disagree, and it
    /// is what the transcript shows.
    void insertScopeSignals(bool recursive);

    /// Asks for a signal list file to load or a place to save one.
    void chooseSignalListToLoad();
    void chooseSignalListToSave();

    /// Writes the list back to the file it was last loaded from or saved to, and asks for one
    /// only when there is none yet.
    ///
    /// Arranging the window, saving it and running again is the loop a signal list exists for
    /// (section 10.8), so after the first time saving asks nothing.
    void saveSignalListToItsFile();

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
    void toggleActiveCursorLock();

    /// Shows the Keyboard Shortcuts window, read afresh from the actions so that it lists what
    /// the window is bound to now.
    void showShortcuts();

    /// Shows the versions of both halves, which can come from two builds: a window attached to
    /// a kernel on another machine is the case section 2.2 exists for.
    void showAbout();

    void runActionExec();

    /// Asks the kernel for the run `length` is, now that the run's time unit is known. `text` is
    /// how it was written, for saying what is wrong with it.
    void startRun(const runtime::RunLength& length, const QString& text);

    void runUntilFinished();
    void stopRun();
    void restartRun();

    /// Writes a signal list command into the transcript, as the command that does what the reader
    /// just did, ready to be copied into a file (section 10.8).
    void echo(const QString& command);

    /// Reports a problem in a signal list, against the file and line it is on.
    void reportSignalList(const QString& file, const SignalListMessage& message, bool isError);

    /// A signal list on its way into the wave window: read, and waiting for the kernel to
    /// describe the scopes it names.
    ///
    /// Insertion cannot happen where it is asked for: the objects of a scope come from the
    /// kernel, which may be on another machine (DESIGN_GUIDE.md section 2.2).
    struct PendingLoad {
        /// The file it came from, or empty for a menu entry.
        QString file;

        std::vector<SignalListEntry> entries;

        /// Scopes asked about and not yet answered.
        QSet<QString> waiting;

        QHash<QString, QVector<SignalInfo>> objects;

        /// True for a file, which replaces the list; false for a menu entry, which adds to it.
        bool replace = false;
    };

    /// Asks the kernel about every scope `load` needs and queues it.
    void startLoad(PendingLoad load);

    /// Puts every load at the front of the queue that has all its answers into the window.
    ///
    /// In the order they were asked for, whatever order the kernel answers in, so that two
    /// inserts made one after the other land one after the other.
    void applyFinishedLoads();

    void finishLoad(const PendingLoad& load);

    /// Writes a value into the selected signal, asking for it first.
    ///
    /// One dialog for both, with `force` deciding which of them it performs, because the two
    /// differ only in whether the value is held afterwards and a reader choosing between them
    /// is choosing between two sentences rather than two operations.
    void depositOrForceSelectedObject(bool force);

    /// Releases every selected signal that is being forced.
    void releaseSelectedObjects();

    void buildDocks();

    /// Gives the application and every widget the theme's colours and sizes.
    ///
    /// The one way a theme reaches the window, on opening as on every change afterwards, so the
    /// two cannot come to differ. Sizes are worked out from the desktop's fonts each time rather
    /// than from the fonts the window has now, so that a theme setting none gives the desktop's
    /// back after one that set them.
    void applyTheme();

    /// Fills the Theme menu from the configuration directory as it is when the menu opens, so
    /// that a theme put into `themes/` is offered without the window being opened again.
    void fillThemeMenu();

    /// Colours the window with the theme `name`. Returns false, having said why and changed
    /// nothing, when it does not load.
    bool useTheme(const ThemeName& name);

    /// Uses the theme `name` and names it in `config`, so that the next window opens with it.
    void chooseTheme(const ThemeName& name);

    /// Reads the theme in use again, for a reader who has just edited it.
    void reloadTheme();

    /// Asks for a name and writes a copy of the theme in use under it, as a theme to edit, and
    /// uses it. Asked in a dialog that is opened rather than run, as the About box is.
    void askForNewTheme();
    void createTheme(const QString& name);

    void buildToolBar();
    void buildMenus();
    void updateActions();
    void updateSubscription();
    void restoreLayout();
    void saveLayout();

    /// The wave rows the user has selected, as model row numbers.
    [[nodiscard]] QVector<int> selectedWaveRows() const;

    KernelSession* session;

    /// What the models, the canvas and the transcript paint with. They hold its address, so it
    /// is replaced in place and never moved.
    Theme theme;

    ConfigurationDirectory configuration;

    /// The theme in use, as `config` names it: the one the Theme menu shows as chosen, Reload
    /// Theme reads again and New Theme copies. Kept here rather than read back from `config`, so
    /// that a choice the directory could not record is still the one in use.
    ThemeName themeName;

    QMenu* themeMenu = nullptr;
    QActionGroup* themeChoices = nullptr;
    QAction* reloadThemeAction = nullptr;
    QAction* newThemeAction = nullptr;

    DesignTreeModel* designModel = nullptr;
    ObjectsModel* objectsModel = nullptr;
    WaveformModel* waveModel = nullptr;

    QTreeView* structureView = nullptr;
    QTableView* objectsView = nullptr;
    QTreeView* waveRowsView = nullptr;
    WaveformView* waveView = nullptr;
    TranscriptView* transcriptView = nullptr;

    /// The Wave tab: the names and the traces beside them. Its keys are the wave window's,
    /// whichever of the two has the focus.
    QSplitter* waveTab = nullptr;

    /// The central area's tabs. Each needs the whole width, so they share it in turn rather
    /// than side by side.
    QTabWidget* panes = nullptr;

    ShortcutSheet* shortcutSheet = nullptr;

    QLineEdit* runLengthEdit = nullptr;
    QLabel* statusTime = nullptr;
    QLabel* statusState = nullptr;
    QLabel* statusCursor = nullptr;

    QAction* runAction = nullptr;
    QAction* runAllAction = nullptr;
    QAction* stopAction = nullptr;
    QAction* restartAction = nullptr;
    QAction* followAction = nullptr;
    QAction* zoomFitAction = nullptr;
    QAction* zoomCursorsAction = nullptr;
    QAction* addCursorAction = nullptr;

    /// The Highlight menu's entries, whose icons are swatches of the theme's colours.
    std::array<QAction*, HighlightSlots> highlightActions{};

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

    /// Signal lists waiting for the kernel, in the order they were asked for.
    QVector<PendingLoad> pendingLoads;

    /// A signal list asked for before the design arrived, loaded when it does.
    QString deferredSignalList;

    /// A run asked for before the kernel welcomed the window, run when it does.
    QString deferredRun;

    /// The file the wave window's list was last loaded from or saved to, which Save writes back
    /// to. Empty until there is one; a file that loaded nothing is not it.
    QString signalListFile;

    /// The scopes of the design the window last showed, to tell a new design from the same one
    /// sent again.
    QStringList knownScopes;
};

} // namespace enzan::gui
