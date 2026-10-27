// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gui/Theme.h"

#include <QByteArray>
#include <QString>
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

/// What a `config` file says (DESIGN_GUIDE.md section 10.9).
struct ParsedConfiguration {
    /// The theme as `config` names it, relative to the directory or absolute; empty for the
    /// theme built into Enzan.
    QString theme;

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

/// `text` naming `theme` as its theme, or the theme built into Enzan when `theme` is empty.
///
/// The `theme` command is rewritten where it stands, added at the end when there is none, or
/// removed with its line; everything else is kept byte for byte, so that a file the reader has
/// annotated stays theirs. Nothing when `text` has a mistake in it, since a file whose commands
/// cannot all be read cannot be rewritten without guessing at what it meant.
[[nodiscard]] std::optional<QByteArray> configurationWithTheme(const QByteArray& text,
                                                               const QString& theme);

/// The theme a configuration directory names, loaded, and what went wrong on the way.
struct ConfiguredTheme {
    /// The theme to colour the window with: the one `config` names, or the theme built into
    /// Enzan when it names none or it did not load.
    Theme theme;

    QVector<ConfigurationMessage> problems;
};

/// The directory the window keeps its settings in (DESIGN_GUIDE.md section 10.9).
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

    /// Makes what the window starts from, when `config` does not exist: `config`, naming
    /// `DefaultTheme`, and the shipped theme at `DefaultTheme` unless a file is there already.
    ///
    /// Nothing that exists is replaced. Returns false having filled `error` when the directory
    /// or a file in it could not be made.
    bool createDefaults(QString& error) const;

    /// Reads `config`. A directory with no `config` names no theme, which is no mistake.
    [[nodiscard]] ParsedConfiguration read() const;

    /// The path of the theme `config` names as `theme`.
    [[nodiscard]] QString resolve(const QString& theme) const;

    /// The theme `config` names, loaded over the shipped one.
    ///
    /// Whatever goes wrong leaves the shipped theme and says why: the window is opened with
    /// colours whatever the directory holds, because a reader who cannot see the design for a
    /// mistyped colour has lost more than the colour.
    [[nodiscard]] ConfiguredTheme loadTheme() const;

private:
    QString root;
};

} // namespace enzan::gui
