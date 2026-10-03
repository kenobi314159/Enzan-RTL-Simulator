// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/KernelSession.h"
#include "gui/WaveformModel.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

namespace enzan::gui {

/// One command of a signal list file (DESIGN_GUIDE.md section 10.8), as read, before anything
/// in it has been looked up in a design.
struct SignalListEntry {
    enum class Kind : std::uint8_t { AddWave, Group };

    Kind kind = Kind::AddWave;

    /// The line the command starts on, for messages.
    int line = 0;

    /// `add wave`: the paths, as written. What each names — a signal, a range of one, or a
    /// scope — is only known once it is looked up.
    QStringList paths;

    /// `group`: the group's name.
    QString name;

    std::optional<eir::Radix> radix;
    bool symbolic = false;
    std::optional<QString> label;
    int highlight = 0;
    bool recursive = false;
    bool expand = false;

    /// True when `-contents` was given, so that an empty body can be told from none.
    bool contents = false;

    /// A group's rows, or the rows `-contents` puts beneath an unpacked signal.
    std::vector<SignalListEntry> body;
};

/// A problem found in a signal list, and the line it is on.
struct SignalListMessage {
    int line = 0;
    QString text;
};

struct ParsedSignalList {
    std::vector<SignalListEntry> entries;

    /// Mistakes in the file. When there are any, nothing in it is loaded (section 10.8).
    QVector<SignalListMessage> errors;
};

/// Reads a signal list.
///
/// Split into commands by Tcl's own parser and never evaluated: quoting, braces, backslashes,
/// line continuation and comments are Tcl's, and nothing is substituted, so `$` and `[…]` are
/// ordinary characters and `/tb/gen[0].q` needs no braces.
[[nodiscard]] ParsedSignalList parseSignalList(const QByteArray& text);

/// Writes `word` so that `parseSignalList` reads it back unchanged, quoting it only when it
/// must be.
[[nodiscard]] QString quoteSignalListWord(const QString& word);

/// The word `-radix` takes for `radix`.
[[nodiscard]] QString radixWord(eir::Radix radix);

/// The radix a `-radix` word names, or nothing.
[[nodiscard]] std::optional<eir::Radix> radixFromWord(const QString& word);

/// The scopes whose objects `entries` need before they can be resolved, each once, in the order
/// they are first needed.
[[nodiscard]] QStringList scopesNeededBy(const std::vector<SignalListEntry>& entries,
                                         const QVector<ipc::InstanceEntry>& instances);

/// A signal list turned into rows for the wave window.
struct ResolvedSignalList {
    std::vector<WaveformModel::PendingRow> rows;

    /// Paths this design lacks. Their rows are skipped and the rest load.
    QVector<SignalListMessage> warnings;

    /// Commands that make no sense for what their paths turned out to name, such as `-label`
    /// on a scope. When there are any, nothing is loaded.
    QVector<SignalListMessage> errors;
};

/// Builds the rows `entries` describe, from the objects of every scope `scopesNeededBy` named.
///
/// A scope path produces exactly what the Structure window's *Add to wave* produces: a group
/// named by the scope's path holding every object of the scope, or nothing for a scope with no
/// objects. Section 10.8 defines the command as that menu entry, not beside it.
[[nodiscard]] ResolvedSignalList
resolveSignalList(const std::vector<SignalListEntry>& entries,
                  const QVector<ipc::InstanceEntry>& instances,
                  const QHash<QString, QVector<SignalInfo>>& objects);

} // namespace enzan::gui
