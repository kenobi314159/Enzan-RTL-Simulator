// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/Theme.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace enzan::gui {

/// A problem found in the configuration directory, against the file and line it is in.
struct ConfigurationMessage {
    /// The file, or empty when `text` names it already, as a theme's error does.
    QString file;

    /// The line, or 0 when the problem is not on one.
    int line = 0;

    QString text;

    /// True when what the file says was not used; false for a command that was skipped while
    /// the rest of the file applied.
    bool isError = false;
};

/// `message` as one line, `file:line: text`, for a terminal.
[[nodiscard]] QString describe(const ConfigurationMessage& message);

/// A theme as `config` names it: a file, or one of the themes built into Enzan.
///
/// One or the other, never both. A default-made name is the default built-in theme, which
/// `config` names by naming no theme at all.
struct ThemeName {
    /// The file, relative to the directory or absolute; empty for a built-in theme.
    QString file;

    /// The built-in theme's name, when `file` is empty.
    QString builtIn = QString::fromLatin1(Theme::DefaultBuiltIn);

    /// The theme in `path`, relative to the directory or absolute.
    [[nodiscard]] static ThemeName ofFile(const QString& path);

    /// The theme built into Enzan as `name`.
    [[nodiscard]] static ThemeName ofBuiltIn(const QString& name);

    [[nodiscard]] bool isFile() const noexcept { return !file.isEmpty(); }

    /// True for the default built-in theme.
    [[nodiscard]] bool isDefault() const;

    friend bool operator==(const ThemeName&, const ThemeName&) = default;
};

/// What a `config` file says.
struct ParsedConfiguration {
    /// The theme `config` names; the default built-in one when it names none.
    ThemeName theme;

    /// The line `theme` is on, or 0 when there is none, for a theme that does not load.
    int themeLine = 0;

    /// Commands this build does not know. Each is skipped and the rest applies, so that a file a
    /// newer Enzan wrote still works in an older one.
    QVector<ConfigurationMessage> warnings;

    /// Mistakes in the file. When there are any, nothing in it is used.
    QVector<ConfigurationMessage> errors;
};

/// Reads a `config` file.
///
/// Split into commands by `splitTclCommands` and never evaluated, as a signal list is: the
/// syntax is Tcl's and the language is not, which is what lets the window write it back.
[[nodiscard]] ParsedConfiguration parseConfiguration(const QByteArray& text);

/// `text` naming `theme` as its theme.
///
/// The `theme` command is rewritten where it stands, added at the end when there is none, or
/// removed with its line for the default built-in theme; everything else is kept byte for byte,
/// so that a file the reader has annotated stays theirs. Nothing when `text` has a mistake in it,
/// since a file whose commands cannot all be read cannot be rewritten without guessing at what it
/// meant.
[[nodiscard]] std::optional<QByteArray> configurationWithTheme(const QByteArray& text,
                                                               const ThemeName& theme);

/// The theme a configuration directory names, loaded, and what went wrong on the way.
struct ConfiguredTheme {
    /// The theme to colour the window with: the one `config` names, or the default built-in
    /// one when it names none or it did not load.
    Theme theme;

    QVector<ConfigurationMessage> problems;
};

/// The directory the window keeps its settings in.
///
/// Only the front end reads or writes it. Nothing in it may change what a simulation computes or
/// records, so the kernel never looks at it, and a run is reproduced from its command line and
/// its files alone.
class ConfigurationDirectory {
public:
    /// The theme `config` names when the window makes it, relative to the directory.
    static constexpr const char* DefaultTheme = "themes/dark.tcl";

    /// `$XDG_CONFIG_HOME/enzan`, or `~/.config/enzan` when that is unset; empty when there is no
    /// home directory to put it in.
    [[nodiscard]] static QString standardPath();

    /// The directory at `path`, which need not exist yet. An empty path is no directory at all:
    /// nothing is read from it and nothing written.
    explicit ConfigurationDirectory(QString path = {});

    [[nodiscard]] const QString& path() const noexcept { return root; }

    [[nodiscard]] QString configFile() const;

    /// Where the window keeps the layout of its docks and its size between sessions.
    ///
    /// Qt's own record of the docks rather than a setting anyone writes, so it is a file of its
    /// own beside `config` and in Qt's format: deleting it opens the window as it first opened.
    [[nodiscard]] QString layoutFile() const;

    /// Makes what the window starts from, when `config` does not exist: `config`, naming
    /// `DefaultTheme`, and the shipped theme at `DefaultTheme` unless a file is there already.
    ///
    /// Nothing that exists is replaced. Returns false having filled `error` when the directory
    /// or a file in it could not be made.
    bool createDefaults(QString& error) const;

    /// Reads `config`. A directory with no `config` names no theme, which is no mistake.
    [[nodiscard]] ParsedConfiguration read() const;

    /// The path of the file `config` names as `theme`.
    [[nodiscard]] QString resolve(const QString& theme) const;

    /// Loads the theme `name` into `theme`: a file over the default built-in theme, or a built-in
    /// theme on its own.
    ///
    /// Returns false, having changed nothing and filled `error`, when the file cannot be read or
    /// does not load, or when no theme is built in under the name.
    bool load(const ThemeName& name, Theme& theme, QString& error) const;

    /// The themes in `themes/`, named as `config` names them, in order of name.
    [[nodiscard]] QStringList themes() const;

    /// Names `theme` in `config`, keeping the rest of the file as it is
    /// (`configurationWithTheme`).
    ///
    /// Returns false having filled `error` when there is no directory, when `config` has a
    /// mistake in it, or when it cannot be written.
    bool setTheme(const ThemeName& theme, QString& error) const;

    /// Writes a copy of the theme `from` to `themes/<name>.tcl`, as a theme to edit, and gives
    /// how `config` names the copy: a built-in theme's text, or the file's.
    ///
    /// Nothing, having filled `error`, when `name` is not a name for a file, when a theme of that
    /// name exists — which is never replaced — or when `from` cannot be read or the copy cannot
    /// be written.
    [[nodiscard]] std::optional<QString> writeTheme(const QString& name, const ThemeName& from,
                                                    QString& error) const;

    /// The theme `config` names, loaded.
    ///
    /// Whatever goes wrong leaves the default theme and says why: the window is opened with
    /// colours whatever the directory holds, because a reader who cannot see the design for a
    /// mistyped colour has lost more than the colour.
    [[nodiscard]] ConfiguredTheme loadTheme() const;

private:
    QString root;
};

} // namespace enzan::gui
