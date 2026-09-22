// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QString>

#include <array>
#include <cstdint>

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

/// The colours the GUI paints with, loaded from a Tcl script.
///
/// **No colour literal appears in widget code** (DESIGN_GUIDE.md section 10.5), and none
/// appears in C++ at all: the defaults live in the shipped `themes/dark.tcl`, which is compiled
/// in as a resource and loaded before any user theme. A default written in C++ would be exactly
/// the literal the rule forbids, and it would let a half-applied theme go unnoticed.
///
/// Tcl rather than a settings format of its own, because section 8.1 commits to Tcl as *the*
/// configuration language with no second format anywhere in the tool. The script gets one
/// command:
///
/// ```tcl
/// theme wave.background #000000
/// theme wave.trace      #7ee787
/// ```
///
/// A user theme is layered over the shipped one, so a script that sets three colours changes
/// three colours rather than leaving the rest undefined.
class Theme {
public:
    Theme();

    /// Loads the shipped dark theme, replacing everything. Cannot fail: the theme is compiled
    /// in, and a build whose own theme did not parse is broken rather than misconfigured, which
    /// is what the assertion in the implementation says.
    void loadBuiltIn();

    /// Evaluates `script`, overlaying whatever it sets on what is already loaded.
    ///
    /// Returns false having filled `error` with the interpreter's message, including the line
    /// number, so that a mistyped colour in a user's theme says where it is.
    bool evaluate(const QString& script, QString& error);

    /// Reads `path` and evaluates it. Returns false having filled `error`.
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

    /// The shipped theme's text, for a user who wants to copy it and change a colour.
    [[nodiscard]] static QString builtInScript();

    /// A Qt style sheet built from the loaded colours.
    ///
    /// Generated rather than written out as a resource for the same reason as everything else
    /// here: a style sheet with colours in it is a second place colours live.
    [[nodiscard]] QString styleSheet() const;

private:
    friend class ThemeCommand;

    /// Records one key, or reports an unknown one. Called by the Tcl command.
    bool set(const QString& key, const QString& value, QString& error);

    std::array<QColor, static_cast<std::size_t>(ThemeColor::Count)> colors;
    std::array<bool, static_cast<std::size_t>(ThemeColor::Count)> assigned{};
};

} // namespace enzan::gui
