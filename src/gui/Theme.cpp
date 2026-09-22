// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/Theme.h"

#include "common/Assert.h"

#include <QFile>

#include <algorithm>
#include <mutex>

#include <tcl.h>

// Section 8.1 asks for Tcl 8.6 or later. Checked here rather than in CMake because CMake's
// FindTCL module reports no version at all, so the header is the only thing that knows.
static_assert(TCL_MAJOR_VERSION > 8 || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 6),
              "Enzan requires Tcl 8.6 or later (DESIGN_GUIDE.md section 8.1)");

namespace enzan::gui {

/// Grants the Tcl command access to `Theme::set`, which is private so that nothing else can
/// bypass the catalogue's spelling of a key.
class ThemeCommand {
public:
    static bool apply(Theme& theme, const QString& key, const QString& value, QString& error) {
        return theme.set(key, value, error);
    }
};

namespace {

constexpr const char* KeyNames[] = {
#define ENZAN_THEME_COLOR(name, key) key,
#include "gui/ThemeColors.def"
#undef ENZAN_THEME_COLOR
};

constexpr std::size_t ColorCount = static_cast<std::size_t>(ThemeColor::Count);

static_assert(std::size(KeyNames) == ColorCount, "every ThemeColor needs a key");

/// The `theme` command a theme script is given.
///
/// One command and nothing else: a theme is a list of colours, and an interpreter that also
/// offered file access or process control would make "copy this theme from the internet" a
/// considerably more exciting decision than it should be.
int themeCommandProc(ClientData data, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "key colour");
        return TCL_ERROR;
    }

    auto* theme = static_cast<Theme*>(data);
    const QString key = QString::fromUtf8(Tcl_GetString(objv[1]));
    const QString value = QString::fromUtf8(Tcl_GetString(objv[2]));

    QString error;
    if (!ThemeCommand::apply(*theme, key, value, error)) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(error.toUtf8().constData(), -1));
        return TCL_ERROR;
    }
    return TCL_OK;
}

/// An interpreter that cleans up after itself on every path out of `evaluate`.
class Interpreter {
public:
    Interpreter() {
        // Tcl asks to be told about the executable once per process before anything else. It
        // is what initialises the library's encodings, and skipping it leaves a handful of
        // things quietly not working.
        static std::once_flag initialised;
        std::call_once(initialised, [] { Tcl_FindExecutable(nullptr); });

        handle = Tcl_CreateInterp();
    }

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;
    Interpreter(Interpreter&&) = delete;
    Interpreter& operator=(Interpreter&&) = delete;

    ~Interpreter() {
        if (handle != nullptr) Tcl_DeleteInterp(handle);
    }

    [[nodiscard]] Tcl_Interp* get() const noexcept { return handle; }

private:
    Tcl_Interp* handle = nullptr;
};

} // namespace

const char* themeKey(ThemeColor color) noexcept {
    const auto index = static_cast<std::size_t>(color);
    ENZAN_ASSERT(index < ColorCount, "no such theme colour");
    return KeyNames[index];
}

Theme::Theme() {
    colors.fill(QColor());
}

QString Theme::builtInScript() {
    QFile file(QStringLiteral(":/enzan/themes/dark.tcl"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

void Theme::loadBuiltIn() {
    colors.fill(QColor());
    assigned.fill(false);

    QString error;
    const bool loaded = evaluate(builtInScript(), error);

    // The shipped theme is compiled into the binary. A build whose own theme does not parse, or
    // does not define every key the catalogue lists, is broken rather than misconfigured, and
    // saying so here beats a window full of black rectangles.
    ENZAN_ASSERT(loaded, "the built-in theme failed to load");
    ENZAN_ASSERT(isComplete(), "the built-in theme does not define every colour");
    static_cast<void>(loaded);
}

bool Theme::evaluate(const QString& script, QString& error) {
    error.clear();

    const Interpreter interpreter;
    if (interpreter.get() == nullptr) {
        error = QStringLiteral("a Tcl interpreter could not be created");
        return false;
    }

    Tcl_CreateObjCommand(interpreter.get(), "theme", &themeCommandProc, this, nullptr);

    const QByteArray text = script.toUtf8();
    if (Tcl_EvalEx(interpreter.get(), text.constData(), static_cast<int>(text.size()), 0)
        != TCL_OK) {
        // The interpreter's own message carries the line number, which is the one thing a user
        // with a mistyped colour actually needs.
        error = QString::fromUtf8(Tcl_GetStringResult(interpreter.get()));
        if (error.isEmpty()) error = QStringLiteral("the theme script failed");
        return false;
    }

    return true;
}

bool Theme::loadFile(const QString& path, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("could not read '%1': %2").arg(path, file.errorString());
        return false;
    }

    return evaluate(QString::fromUtf8(file.readAll()), error);
}

bool Theme::set(const QString& key, const QString& value, QString& error) {
    std::size_t index = ColorCount;
    for (std::size_t candidate = 0; candidate < ColorCount; ++candidate) {
        if (key == QLatin1StringView(KeyNames[candidate])) {
            index = candidate;
            break;
        }
    }

    if (index == ColorCount) {
        // Named rather than ignored. A theme silently dropping a key is how a user spends an
        // afternoon wondering why their colour did not take.
        error = QStringLiteral("'%1' is not a colour this build knows").arg(key);
        return false;
    }

    const QColor parsed(value);
    if (!parsed.isValid()) {
        error = QStringLiteral("'%1' is not a colour; expected #rrggbb, #aarrggbb or a name")
                    .arg(value);
        return false;
    }

    colors[index] = parsed;
    assigned[index] = true;
    return true;
}

bool Theme::isComplete() const noexcept {
    return std::ranges::all_of(assigned, [](bool wasSet) { return wasSet; });
}

QString Theme::firstMissingKey() const {
    for (std::size_t index = 0; index < ColorCount; ++index) {
        if (!assigned[index]) return QString::fromUtf8(KeyNames[index]);
    }
    return {};
}

QString Theme::styleSheet() const {
    const auto name = [this](ThemeColor which) { return color(which).name(QColor::HexArgb); };

    // Written out here rather than shipped as a resource because a style sheet with colours in
    // it would be a second place colours live, and section 10.5 allows exactly one.
    return QStringLiteral(R"(
QWidget { background-color: %1; color: %2; }
QMainWindow::separator { background-color: %5; width: 3px; height: 3px; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background-color: %6; color: %7; padding: 4px 8px; }
QAbstractScrollArea, QTreeView, QTableView, QPlainTextEdit, QListView {
    background-color: %3; color: %4; border: 1px solid %5;
}
QTreeView::item:selected, QTableView::item:selected, QListView::item:selected {
    background-color: %8; color: %9;
}
QHeaderView::section {
    background-color: %6; color: %7; border: 0px; border-right: 1px solid %5;
    border-bottom: 1px solid %5; padding: 3px 6px;
}
QToolBar { background-color: %6; border: 0px; spacing: 4px; padding: 3px; }
QToolButton, QPushButton, QLineEdit, QComboBox, QSpinBox {
    background-color: %10; color: %11; border: 1px solid %12; padding: 3px 8px;
}
QToolButton:hover, QPushButton:hover { border-color: %13; }
QToolButton:disabled, QPushButton:disabled, QLineEdit:disabled { color: %7; }
QMenuBar, QMenu { background-color: %6; color: %2; }
QMenu::item:selected, QMenuBar::item:selected { background-color: %8; color: %9; }
QStatusBar { background-color: %6; color: %7; }
QSplitter::handle { background-color: %5; }
QScrollBar:vertical, QScrollBar:horizontal { background: %3; border: 0px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background: %12; min-height: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; }
)")
        .arg(name(ThemeColor::WindowBackground), name(ThemeColor::WindowForeground),
             name(ThemeColor::PanelBackground), name(ThemeColor::PanelForeground),
             name(ThemeColor::PanelBorder), name(ThemeColor::HeaderBackground),
             name(ThemeColor::HeaderForeground), name(ThemeColor::SelectionBackground),
             name(ThemeColor::SelectionForeground))
        .arg(name(ThemeColor::ControlBackground), name(ThemeColor::ControlForeground),
             name(ThemeColor::ControlBorder), name(ThemeColor::Accent));
}

} // namespace enzan::gui
