// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

namespace enzan::gui {

/// Where the text being split lies in its file, so that a message can name a line.
struct TclOrigin {
    /// The first character of the text lines are counted from.
    const char* base = nullptr;

    /// The line `base` is on.
    int baseLine = 1;

    /// False for text that had to be decoded into a copy, whose lines no longer line up with
    /// the file's. Everything in it is then reported at `baseLine`.
    bool exact = true;
};

/// A word as the file wrote it: backslash sequences decoded, and nothing substituted.
struct TclWord {
    QByteArray text;

    /// Where the word lies in the text, its braces or quotes included: for its line, and for a
    /// writer that replaces it where it stands.
    const char* start = nullptr;
    int size = 0;

    /// For a word that was one piece of literal text — braced, or bare with nothing in it to
    /// decode — where that text is in the file. A body read from there keeps its line numbers.
    const char* literalStart = nullptr;
    int literalSize = 0;

    /// `{*}` in front of something that would need evaluating — a variable or a command. Tcl's
    /// parser expands `{*}` in front of literal text itself, into the words it plainly is, so
    /// only this kind ever reaches here.
    bool expansion = false;
};

/// One command, and the line it starts on.
struct TclCommand {
    std::vector<TclWord> words;
    int line = 0;
};

/// Why text could not be split into commands, and the line where it went wrong.
struct TclSyntaxError {
    int line = 0;
    QString text;
};

/// Splits `size` bytes at `text` into commands with Tcl's own parser, evaluating nothing.
///
/// For a file that has Tcl's syntax and is not a Tcl program: quoting, braces, backslashes, line
/// continuation and comments behave as in Tcl, without a lexer of Enzan's to keep in step with
/// it, and nothing is substituted, so `$` and `[…]` are ordinary characters.
///
/// Calls `each` with every command that has a word, in order, and stops when it returns false.
/// Returns true when every command was read. Returns false when `each` stopped, or when the text
/// could not be split at all, which alone fills `malformed`.
bool splitTclCommands(const char* text, int size, const TclOrigin& origin,
                      const std::function<bool(const TclCommand&)>& each,
                      std::optional<TclSyntaxError>& malformed);

/// Writes `word` so that `splitTclCommands` reads it back unchanged, quoting it only when it
/// must be.
[[nodiscard]] QString quoteTclWord(const QString& word);

} // namespace enzan::gui
