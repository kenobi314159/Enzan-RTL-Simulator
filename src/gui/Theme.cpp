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
              "Enzan requires Tcl 8.6 or later (DESIGN_GUIDE.md section 10.5)");

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

ThemeColor highlightColor(int slot) noexcept {
    ENZAN_ASSERT(slot >= 1 && slot <= HighlightSlots, "no such highlight slot");
    return static_cast<ThemeColor>(static_cast<int>(ThemeColor::WaveHighlight1) + slot - 1);
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

QPalette Theme::palette() const {
    QPalette result;

    // The window chrome, and the text on it.
    result.setColor(QPalette::Window, color(ThemeColor::WindowBackground));
    result.setColor(QPalette::WindowText, color(ThemeColor::WindowForeground));

    // What an item view, a text area or any other scrolling surface is made of. This is the
    // one that matters most for cost: as a style sheet rule it made every scroll of the signal
    // list a full repaint.
    result.setColor(QPalette::Base, color(ThemeColor::PanelBackground));
    result.setColor(QPalette::AlternateBase, color(ThemeColor::PanelAlternateBackground));
    result.setColor(QPalette::Text, color(ThemeColor::PanelForeground));

    result.setColor(QPalette::Button, color(ThemeColor::ControlBackground));
    result.setColor(QPalette::ButtonText, color(ThemeColor::ControlForeground));
    result.setColor(QPalette::BrightText, color(ThemeColor::Accent));

    result.setColor(QPalette::Highlight, color(ThemeColor::SelectionBackground));
    result.setColor(QPalette::HighlightedText, color(ThemeColor::SelectionForeground));

    result.setColor(QPalette::ToolTipBase, color(ThemeColor::HeaderBackground));
    result.setColor(QPalette::ToolTipText, color(ThemeColor::HeaderForeground));

    result.setColor(QPalette::Link, color(ThemeColor::Accent));
    result.setColor(QPalette::LinkVisited, color(ThemeColor::Accent));

    // The frame around a scrolling surface. These four are where the ordinary style takes a
    // frame's colour from, and going through them rather than through a style sheet border is
    // not a matter of taste: a style-sheet border on `QAbstractScrollArea` is what stopped the
    // signal list from scrolling by moving what it had drawn, and it alone was three quarters
    // of what a scroll of a tall window cost.
    result.setColor(QPalette::Mid, color(ThemeColor::PanelBorder));
    result.setColor(QPalette::Dark, color(ThemeColor::PanelBorder));
    result.setColor(QPalette::Shadow, color(ThemeColor::PanelBorder));
    result.setColor(QPalette::Light, color(ThemeColor::PanelBorder));

    // A disabled control reads as the header foreground, which is the theme's quiet text.
    result.setColor(QPalette::Disabled, QPalette::Text, color(ThemeColor::HeaderForeground));
    result.setColor(QPalette::Disabled, QPalette::WindowText, color(ThemeColor::HeaderForeground));
    result.setColor(QPalette::Disabled, QPalette::ButtonText, color(ThemeColor::HeaderForeground));

    return result;
}

QString Theme::styleSheet() const {
    const auto name = [this](ThemeColor which) { return color(which).name(QColor::HexArgb); };

    // Written out here rather than shipped as a resource because a style sheet with colours in
    // it would be a second place colours live, and section 10.5 allows exactly one.
    //
    // **Nothing here may style a scrolling surface — not its background, not its border.**
    // Either one makes Qt draw that widget through the style sheet's box model, and a widget
    // drawn that way cannot scroll by moving what it already has; it repaints instead. On a
    // list of two hundred signals in a tall window that was the difference between 1.7 ms and
    // 6.2 ms per line scrolled, and the border rule on its own was nearly all of it.
    //
    // Anything a palette can say goes through `palette`. What is left here is the shape of
    // things a palette has no entry for: padding, the dock title, the scroll bar's parts.
    return QStringLiteral(R"(
QMainWindow::separator { background-color: %3; width: 3px; height: 3px; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background-color: %4; color: %5; padding: 4px 8px; }
QHeaderView::section {
    background-color: %4; color: %5; border: 0px; border-right: 1px solid %3;
    border-bottom: 1px solid %3; padding: 3px 6px;
}
QToolBar { background-color: %4; border: 0px; spacing: 4px; padding: 3px; }
QToolButton, QPushButton, QLineEdit, QComboBox, QSpinBox {
    border: 1px solid %6; padding: 3px 8px;
}
QToolButton:hover, QPushButton:hover { border-color: %7; }
QMenuBar, QMenu { background-color: %4; color: %2; }
QMenu::item:selected, QMenuBar::item:selected { background-color: %8; color: %9; }
QStatusBar { background-color: %4; color: %5; }
QSplitter::handle { background-color: %3; }
QScrollBar:vertical, QScrollBar:horizontal { background: %1; border: 0px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background: %6; min-height: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; }
)")
        .arg(name(ThemeColor::PanelBackground), name(ThemeColor::WindowForeground),
             name(ThemeColor::PanelBorder), name(ThemeColor::HeaderBackground),
             name(ThemeColor::HeaderForeground), name(ThemeColor::ControlBorder),
             name(ThemeColor::Accent), name(ThemeColor::SelectionBackground),
             name(ThemeColor::SelectionForeground));
}

} // namespace enzan::gui
