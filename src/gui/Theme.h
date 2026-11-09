// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <optional>

namespace enzan::gui {

/// Every colour the GUI asks for.
///
/// The enumerator is the name widget code uses; the string it maps to is the key a theme
/// script sets. The catalogue is ThemeColors.def.
enum class ThemeColor : std::uint8_t {
#define ENZAN_THEME_COLOR(name, key) name,
#include "gui/ThemeColors.def"
#undef ENZAN_THEME_COLOR
    Count,
};

/// Returns the theme-script key for `color`, for example `"wave.trace"`.
[[nodiscard]] const char* themeKey(ThemeColor color) noexcept;

/// How many backgrounds a wave row may be highlighted with.
constexpr int HighlightSlots = 5;

/// Returns the colour of highlight slot `slot`, which must be 1 through `HighlightSlots`.
///
/// Reached by adding to the first slot rather than through a table, which is why the catalogue
/// requires the five to stay adjacent and in order.
[[nodiscard]] ThemeColor highlightColor(int slot) noexcept;

/// How many colours a terminal's colour codes name: eight, and eight bright ones.
constexpr int TerminalColors = 16;

/// Returns the theme's colour for terminal colour `index`, 0 through `TerminalColors` - 1 in the
/// codes' order, black to white and then the bright ones.
///
/// Reached by adding to the first, which is why the catalogue requires the sixteen to stay
/// adjacent and in order.
[[nodiscard]] ThemeColor terminalColor(int index) noexcept;

/// The parts of the window a theme may give a font size of their own.
enum class ThemeFont : std::uint8_t {
    /// Everything not named below: the menus, the docks, the Structure and Objects windows.
    Window,
    /// The signal list and the traces beside it. One size for both keeps each name level with
    /// its trace, and it is what sets how tall a row is.
    Wave,
    /// The transcript.
    Transcript,
    /// The Source window.
    Source,
    Count,
};

/// The colours the GUI paints with, and the sizes of its fonts, loaded from a Tcl script.
///
/// **No colour literal appears in widget code** (DESIGN_GUIDE.md section 10.5), and none
/// appears in C++ at all: the colours live in the shipped themes, `themes/dark.tcl` and
/// `themes/light.tcl`, which are compiled in as resources. The dark one is the default and is
/// loaded before any user theme. A default written in C++ would be exactly the literal the rule
/// forbids, and it would let a half-applied theme go unnoticed.
///
/// Tcl rather than a settings format of its own, because Tcl is *the* configuration language, with
/// no second format anywhere in the tool. The script gets two commands:
///
/// ```tcl
/// theme wave.background #000000
/// theme wave.trace      #7ee787
/// font  wave            11
/// ```
///
/// A user theme is layered over the default one, so a script that sets three colours changes
/// three colours rather than leaving the rest undefined. It is taken whole or not at all: a
/// script that fails at its fortieth line changes nothing, rather than the thirty-nine before it.
///
/// **A font size is the desktop's unless a theme says otherwise.** The shipped themes set none,
/// because how large text should be is the reader's screen's and eyes' to say, which a
/// default written down here would only guess at. An area a theme leaves unset takes the
/// window's size, and the window the desktop's.
class Theme {
public:
    /// The compiled-in theme a window has when nothing names another.
    static constexpr const char* DefaultBuiltIn = "dark";

    Theme();

    /// Loads the default compiled-in theme, replacing everything. Cannot fail: the theme is
    /// compiled in, and a build whose own theme did not parse is broken rather than
    /// misconfigured, which is what the assertion in the implementation says.
    void loadBuiltIn();

    /// Loads the compiled-in theme called `name`, replacing everything.
    ///
    /// On its own, not over the default: each shipped theme sets every colour, which a test
    /// holds it to, so that a key one of them forgot shows up there rather than as a colour of
    /// another theme's in its window. Returns false, changing nothing, for a name no theme is
    /// built in under.
    bool loadBuiltIn(const QString& name);

    /// The names of the compiled-in themes, the default first and the rest in order of name.
    ///
    /// Read from the resources rather than listed here, so that a theme is shipped by adding its
    /// file to the build and nothing else.
    [[nodiscard]] static QStringList builtInNames();

    /// Evaluates `script`, overlaying whatever it sets on what is already loaded.
    ///
    /// All of it or none of it. On failure nothing has changed, and `error` holds the
    /// interpreter's message after the line it stopped at, so that a mistyped colour in a
    /// user's theme says where it is.
    bool evaluate(const QString& script, QString& error);

    /// Loads the shipped theme with the script in `path` over it, replacing everything.
    ///
    /// When the file cannot be read or its script fails, nothing has changed and `error` names
    /// the file, and the line where there is one.
    bool loadFile(const QString& path, QString& error);

    [[nodiscard]] QColor color(ThemeColor which) const noexcept {
        return colors.at(static_cast<std::size_t>(which));
    }

    /// True when every key in the catalogue has been set.
    ///
    /// The shipped theme always satisfies this; the check exists so that a key added to
    /// ThemeColors.def without being added to the theme file is caught by a test rather than
    /// by a black widget on someone's screen.
    [[nodiscard]] bool isComplete() const noexcept;

    /// The first key the catalogue lists that no loaded script has set, or an empty string.
    [[nodiscard]] QString firstMissingKey() const;

    /// The text of the compiled-in theme called `name`, or the default's when `name` is empty,
    /// for a user who wants to copy it and change a colour. Empty for a name no theme is built
    /// in under.
    [[nodiscard]] static QString builtInScript(const QString& name = {});

    /// The size in points `area`'s text is set to: its own, or else the window's. Nothing when
    /// neither is set, which leaves the desktop's.
    [[nodiscard]] std::optional<double> fontSize(ThemeFont area) const noexcept;

    /// `base` at `area`'s size, or `base` itself when the theme sets none.
    [[nodiscard]] QFont font(ThemeFont area, QFont base) const;

    /// The loaded colours as a palette, which is how nearly all of them reach the widgets.
    ///
    /// A palette rather than a style sheet wherever a palette can say it, because the two cost
    /// very different amounts. A style sheet rule that gives a widget a background makes Qt
    /// render that background on every repaint instead of moving what it already had, and it
    /// puts the whole paint through the style-sheet style; measured on a list of two hundred
    /// signals, a universal `QWidget { background-color }` rule and the one under it were
    /// three quarters of what scrolling the window cost. A palette is read by the ordinary
    /// style and costs nothing.
    [[nodiscard]] QPalette palette() const;

    /// A Qt style sheet for what a palette cannot express — borders, padding, the shapes of
    /// the scroll bar and the dock title.
    ///
    /// Generated rather than written out as a resource for the same reason as everything else
    /// here: a style sheet with colours in it is a second place colours live. It deliberately
    /// sets **no background on any item view, nor on `QWidget`**; see `palette`.
    [[nodiscard]] QString styleSheet() const;

private:
    friend class ThemeCommand;

    /// `evaluate`, with the line the script stopped at given apart from the message, or 0 when
    /// it did not get as far as a line.
    bool evaluateAt(const QString& script, QString& error, int& line);

    /// Records one key, or reports an unknown one. Called by the Tcl command.
    bool set(const QString& key, const QString& value, QString& error);

    /// Records one area's font size, or reports an unknown area or a size that is not one.
    /// Called by the Tcl command.
    bool setFont(const QString& area, const QString& size, QString& error);

    std::array<QColor, static_cast<std::size_t>(ThemeColor::Count)> colors;
    std::array<bool, static_cast<std::size_t>(ThemeColor::Count)> assigned{};
    std::array<std::optional<double>, static_cast<std::size_t>(ThemeFont::Count)> fontSizes{};
};

} // namespace enzan::gui
