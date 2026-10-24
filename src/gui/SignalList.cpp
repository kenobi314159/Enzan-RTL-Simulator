// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/SignalList.h"

#include "gui/TclLibrary.h"
#include "gui/Theme.h"

#include <QSet>

#include <algorithm>
#include <array>
#include <utility>

#include <tcl.h>

namespace enzan::gui {

namespace {

/// Every radix a signal list can name, in the order a message lists them.
constexpr std::array ListedRadixes{eir::Radix::Binary, eir::Radix::Octal, eir::Radix::Decimal,
                                   eir::Radix::Hexadecimal, eir::Radix::Ascii};

/// How deeply groups may nest. Each level is a recursion here, and a file is untrusted input:
/// without a limit, a few kilobytes of opening braces would take the window down.
constexpr int MaximumDepth = 32;

/// Where the text being parsed lies in the file, so that a message can name a line.
struct Origin {
    /// The first character of the text lines are counted from.
    const char* base = nullptr;

    /// The line `base` is on.
    int baseLine = 1;

    /// False for a body that had to be decoded into a copy, whose lines no longer line up with
    /// the file's. Everything in it is then reported at the line the body starts on.
    bool exact = true;
};

int lineOf(const Origin& origin, const char* position) {
    if (!origin.exact || position == nullptr || position < origin.base) return origin.baseLine;
    return origin.baseLine + static_cast<int>(std::count(origin.base, position, '\n'));
}

/// A word as the file wrote it: backslash sequences decoded, and nothing substituted.
struct Word {
    QByteArray text;

    /// Where the word starts, for its line.
    const char* start = nullptr;

    /// For a word that was one piece of literal text — braced, or bare with nothing in it to
    /// decode — where that text is in the file. A body read from there keeps its line numbers.
    const char* literalStart = nullptr;
    int literalSize = 0;

    /// `{*}` in front of something that would need evaluating — a variable or a command. Tcl's
    /// parser expands `{*}` in front of literal text itself, into the words it plainly is, so
    /// only this kind ever reaches here.
    bool expansion = false;
};

std::vector<Word> wordsOf(const Tcl_Parse& parse) {
    std::vector<Word> words;
    const Tcl_Token* token = parse.tokenPtr;

    for (int index = 0; index < parse.numWords; ++index) {
        Word word;
        word.start = token->start;
        word.expansion = token->type == TCL_TOKEN_EXPAND_WORD;

        const Tcl_Token* component = token + 1;
        const Tcl_Token* end = component + token->numComponents;

        if (token->numComponents == 1 && component->type == TCL_TOKEN_TEXT) {
            word.literalStart = component->start;
            word.literalSize = component->size;
        }

        while (component < end) {
            switch (component->type) {
            case TCL_TOKEN_TEXT:
                word.text.append(component->start, component->size);
                ++component;
                break;

            case TCL_TOKEN_BS: {
                std::array<char, TCL_UTF_MAX + 1> decoded{};
                int read = 0;
                const int written = Tcl_UtfBackslash(component->start, &read, decoded.data());
                word.text.append(decoded.data(), written);
                ++component;
                break;
            }

            default:
                // A command or a variable substitution in Tcl, and in a signal list the
                // characters as written: `/tb/gen[0].q` is a path, not a call to `0`.
                word.text.append(component->start, component->size);
                component += 1 + component->numComponents;
                break;
            }
        }

        words.push_back(std::move(word));
        token = end;
    }

    return words;
}

QString describeParseError(int errorType) {
    switch (errorType) {
    case TCL_PARSE_QUOTE_EXTRA:
        return QStringLiteral("a quoted word is followed by something other than a space");
    case TCL_PARSE_BRACE_EXTRA:
        return QStringLiteral("a braced word is followed by something other than a space");
    case TCL_PARSE_MISSING_BRACE: return QStringLiteral("a brace opened here is never closed");
    case TCL_PARSE_MISSING_BRACKET:
        return QStringLiteral("a bracket opened here is never closed; a word's brackets must "
                              "balance unless it is in braces");
    case TCL_PARSE_MISSING_QUOTE: return QStringLiteral("a quote opened here is never closed");
    case TCL_PARSE_MISSING_VAR_BRACE:
        return QStringLiteral("`${` opened here is never closed; a word with `$` in it belongs "
                              "in braces");
    default: return QStringLiteral("this cannot be read as a command");
    }
}

class Parser {
public:
    explicit Parser(QVector<SignalListMessage>& messages) : errors(messages) {}

    /// Reads the commands in `text` into `out`. Returns false when the text could not be split
    /// into commands at all, after which nothing more is read.
    bool parse(const char* text, int size, const Origin& origin, int depth,
               std::vector<SignalListEntry>& out) {
        const char* cursor = text;
        int remaining = size;

        while (remaining > 0) {
            Tcl_Parse parse;
            if (Tcl_ParseCommand(nullptr, cursor, remaining, 0, &parse) != TCL_OK) {
                error(lineOf(origin, parse.term), describeParseError(parse.errorType));
                Tcl_FreeParse(&parse);
                return false;
            }

            const std::vector<Word> words = wordsOf(parse);
            const int line = lineOf(origin, parse.commandStart);
            const char* next = parse.commandStart + parse.commandSize;
            Tcl_FreeParse(&parse);

            if (!words.empty() && !command(words, line, origin, depth, out)) return false;

            // A parse always consumes something while there is text left; the test is here so
            // that a parser that one day did not would stop rather than spin.
            if (next <= cursor) break;
            remaining -= static_cast<int>(next - cursor);
            cursor = next;
        }

        return true;
    }

private:
    void error(int line, QString text) {
        errors.push_back(SignalListMessage{.line = line, .text = std::move(text)});
    }

    /// Reads a body — of a group, or of `-contents` — as a signal list of its own.
    bool body(const Word& word, const Origin& origin, int line, int depth,
              std::vector<SignalListEntry>& out) {
        if (depth >= MaximumDepth) {
            error(line, QStringLiteral("groups are nested more than %1 deep").arg(MaximumDepth));
            return false;
        }

        if (word.literalStart != nullptr) {
            return parse(word.literalStart, word.literalSize, origin, depth + 1, out);
        }

        // Decoded, so its characters are a copy and its lines are reported as the body's own.
        const Origin copy{.base = word.text.constData(), .baseLine = line, .exact = false};
        return parse(word.text.constData(), static_cast<int>(word.text.size()), copy, depth + 1,
                     out);
    }

    bool command(const std::vector<Word>& words, int line, const Origin& origin, int depth,
                 std::vector<SignalListEntry>& out) {
        for (const Word& word : words) {
            if (word.expansion) {
                error(line, QStringLiteral("`{*}` here would expand a variable or a command's "
                                           "result, and nothing in a signal list is evaluated"));
                return true;
            }
        }

        const QString name = QString::fromUtf8(words[0].text);
        if (name == QStringLiteral("add")) {
            if (words.size() < 2 || words[1].text != "wave") {
                error(line, QStringLiteral("`add` is always `add wave <path>`"));
                return true;
            }
            return addWave(words, line, origin, depth, out);
        }
        if (name == QStringLiteral("group")) return group(words, line, origin, depth, out);

        error(line, QStringLiteral("'%1' is not a signal list command: there are two, `add "
                                   "wave` and `group`, and nothing in a signal list is evaluated")
                        .arg(name));
        return true;
    }

    /// Reads the value an option takes. Returns false having reported that it is missing.
    bool valueOf(const std::vector<Word>& words, std::size_t& index, int line,
                 const QString& option) {
        if (index + 1 >= words.size()) {
            error(line, QStringLiteral("%1 needs a value").arg(option));
            return false;
        }
        ++index;
        return true;
    }

    /// Reads an option common to `add wave` and `group`. Returns false when `option` is not one
    /// of them, and sets `failed` when it is one but its value is wrong.
    bool sharedOption(const std::vector<Word>& words, std::size_t& index, int line,
                      SignalListEntry& entry, bool& failed) {
        const QString option = QString::fromUtf8(words[index].text);

        if (option == QStringLiteral("-radix")) {
            if (!valueOf(words, index, line, option)) {
                failed = true;
                return true;
            }
            entry.radix = radixFromWord(QString::fromUtf8(words[index].text));
            if (!entry.radix.has_value()) {
                QStringList known;
                for (const eir::Radix radix : ListedRadixes) known << radixWord(radix);
                error(line, QStringLiteral("'%1' is not a radix; it is one of %2")
                                .arg(QString::fromUtf8(words[index].text), known.join(u", ")));
                failed = true;
            }
            return true;
        }

        if (option == QStringLiteral("-symbolic")) {
            entry.symbolic = true;
            return true;
        }

        if (option == QStringLiteral("-highlight")) {
            if (!valueOf(words, index, line, option)) {
                failed = true;
                return true;
            }
            bool number = false;
            const int slot = QString::fromUtf8(words[index].text).toInt(&number);
            if (!number || slot < 1 || slot > HighlightSlots) {
                error(line, QStringLiteral("-highlight is a number from 1 to %1, not '%2'")
                                .arg(HighlightSlots)
                                .arg(QString::fromUtf8(words[index].text)));
                failed = true;
            }
            entry.highlight = slot;
            return true;
        }

        return false;
    }

    bool addWave(const std::vector<Word>& words, int line, const Origin& origin, int depth,
                 std::vector<SignalListEntry>& out) {
        SignalListEntry entry;
        entry.kind = SignalListEntry::Kind::AddWave;
        entry.line = line;

        bool failed = false;
        const Word* contents = nullptr;
        std::size_t index = 2;

        for (; index < words.size() && words[index].text.startsWith('-'); ++index) {
            if (sharedOption(words, index, line, entry, failed)) continue;

            const QString option = QString::fromUtf8(words[index].text);
            if (option == QStringLiteral("-label")) {
                if (!valueOf(words, index, line, option)) return true;
                entry.label = QString::fromUtf8(words[index].text);
            } else if (option == QStringLiteral("-expand")) {
                entry.expand = true;
            } else if (option == QStringLiteral("-r")) {
                entry.recursive = true;
            } else if (option == QStringLiteral("-contents")) {
                if (!valueOf(words, index, line, option)) return true;
                entry.contents = true;
                contents = &words[index];
            } else {
                error(line, QStringLiteral("'%1' is not an option of `add wave`").arg(option));
                failed = true;
            }
        }

        for (; index < words.size(); ++index) entry.paths << QString::fromUtf8(words[index].text);

        if (entry.paths.isEmpty()) {
            error(line, QStringLiteral("`add wave` needs at least one path"));
            failed = true;
        }
        if (entry.label.has_value() && entry.paths.size() > 1) {
            error(line,
                  QStringLiteral("-label names one row, and this adds %1").arg(entry.paths.size()));
            failed = true;
        }
        if (entry.contents && entry.paths.size() > 1) {
            error(line, QStringLiteral("-contents describes one signal's rows, and this names %1 "
                                       "signals")
                            .arg(entry.paths.size()));
            failed = true;
        }
        if (entry.contents && entry.expand) {
            error(line, QStringLiteral("-expand and -contents both say what is beneath a signal; "
                                       "give one"));
            failed = true;
        }
        if (entry.recursive && (entry.label.has_value() || entry.expand || entry.contents)) {
            error(line, QStringLiteral("-r adds a scope's subtree, which -label, -expand and "
                                       "-contents do not apply to"));
            failed = true;
        }

        if (contents != nullptr && !body(*contents, origin, line, depth, entry.body)) return false;

        if (!failed) out.push_back(std::move(entry));
        return true;
    }

    bool group(const std::vector<Word>& words, int line, const Origin& origin, int depth,
               std::vector<SignalListEntry>& out) {
        SignalListEntry entry;
        entry.kind = SignalListEntry::Kind::Group;
        entry.line = line;

        bool failed = false;
        std::size_t index = 1;
        for (; index < words.size() && words[index].text.startsWith('-'); ++index) {
            if (sharedOption(words, index, line, entry, failed)) continue;
            error(line, QStringLiteral("'%1' is not an option of `group`")
                            .arg(QString::fromUtf8(words[index].text)));
            failed = true;
        }

        if (words.size() - index != 2) {
            error(line, QStringLiteral("`group` is `group ?options? name body`"));
            return true;
        }

        entry.name = QString::fromUtf8(words[index].text);
        if (!body(words[index + 1], origin, line, depth, entry.body)) return false;

        if (!failed) out.push_back(std::move(entry));
        return true;
    }

    QVector<SignalListMessage>& errors;
};

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

/// The scope part of a path: everything before the last separator.
QString scopeOf(const QString& path) {
    const qsizetype separator = path.lastIndexOf(u'/');
    return separator <= 0 ? QString() : path.left(separator);
}

/// A path that ends in `[h:l]` or `[n]`, split into the signal's path and the range.
struct RangedPath {
    QString path;
    std::uint32_t high = 0;
    std::uint32_t low = 0;
};

std::optional<RangedPath> splitRange(const QString& path) {
    if (!path.endsWith(u']')) return std::nullopt;
    const qsizetype open = path.lastIndexOf(u'[');
    if (open <= 0) return std::nullopt;

    const QString inside = path.mid(open + 1, path.size() - open - 2);
    const QStringList bounds = inside.split(u':');
    if (bounds.size() > 2) return std::nullopt;

    bool highOk = false;
    bool lowOk = false;
    const std::uint32_t high = bounds.front().trimmed().toUInt(&highOk);
    const std::uint32_t low = bounds.back().trimmed().toUInt(&lowOk);
    if (!highOk || !lowOk) return std::nullopt;

    return RangedPath{.path = path.left(open), .high = high, .low = low};
}

class Resolver {
public:
    Resolver(const QVector<ipc::InstanceEntry>& instanceList,
             const QHash<QString, QVector<SignalInfo>>& scopeObjects, ResolvedSignalList& output)
        : instances(instanceList), objects(scopeObjects), result(output) {
        for (const ipc::InstanceEntry& instance : instances)
            scopes.insert(QString::fromStdString(instance.path));
    }

    void entries(const std::vector<SignalListEntry>& list,
                 std::vector<WaveformModel::PendingRow>& out) {
        for (const SignalListEntry& entry : list) {
            if (entry.kind == SignalListEntry::Kind::Group) {
                WaveformModel::PendingRow group;
                group.row.kind = WaveRow::Kind::Group;
                group.row.label = entry.name;
                apply(entry, group.row);
                entries(entry.body, group.children);
                out.push_back(std::move(group));
                continue;
            }

            for (const QString& path : entry.paths) {
                if (scopes.contains(path)) {
                    scope(entry, path, out);
                } else {
                    signal(entry, path, out);
                }
            }
        }
    }

private:
    void warn(int line, QString text) {
        result.warnings.push_back(SignalListMessage{.line = line, .text = std::move(text)});
    }

    void error(int line, QString text) {
        result.errors.push_back(SignalListMessage{.line = line, .text = std::move(text)});
    }

    /// The options every row an entry adds carries.
    static void apply(const SignalListEntry& entry, WaveRow& row) {
        // A radix asks for the number, as choosing one from the menu does; `-symbolic` asks
        // for the names after all, wherever it is written.
        if (entry.radix.has_value()) {
            row.radix = *entry.radix;
            row.symbolic = false;
        }
        if (entry.symbolic) row.symbolic = true;
        if (entry.highlight != 0) row.highlight = entry.highlight;
    }

    void scope(const SignalListEntry& entry, const QString& path,
               std::vector<WaveformModel::PendingRow>& out) {
        if (entry.expand || entry.contents) {
            error(entry.line, QStringLiteral("%1 is a scope; -expand and -contents apply "
                                             "to a signal")
                                  .arg(path));
            return;
        }

        // The subtree in the Structure window's order, as *Add to wave recursive* takes it.
        const QString prefix = path + u'/';
        for (const ipc::InstanceEntry& instance : instances) {
            const QString candidate = QString::fromStdString(instance.path);
            if (candidate != path && !(entry.recursive && candidate.startsWith(prefix))) continue;

            const QVector<SignalInfo> found = objects.value(candidate);

            // A scope with nothing in it leaves no group behind, as the menu entry does.
            if (found.isEmpty()) continue;

            WaveformModel::PendingRow group;
            group.row.kind = WaveRow::Kind::Group;
            group.row.label = (entry.label.has_value()) ? *entry.label : candidate;
            apply(entry, group.row);
            for (const SignalInfo& info : found) {
                WaveformModel::PendingRow child;
                child.row = WaveformModel::rowFor(info);
                apply(entry, child.row);
                group.children.push_back(std::move(child));
            }
            out.push_back(std::move(group));
        }
    }

    [[nodiscard]] const SignalInfo* find(const QString& path) const {
        const auto found = objects.constFind(scopeOf(path));
        if (found == objects.constEnd()) return nullptr;
        for (const SignalInfo& info : *found) {
            if (info.path == path) return &info;
        }
        return nullptr;
    }

    void signal(const SignalListEntry& entry, const QString& path,
                std::vector<WaveformModel::PendingRow>& out) {
        if (entry.recursive) {
            error(entry.line,
                  QStringLiteral("-r adds a scope's subtree, and %1 is not a scope").arg(path));
            return;
        }

        WaveformModel::PendingRow pending;

        // The whole path first, so that a name ending in brackets is never taken for a range.
        if (const SignalInfo* info = find(path)) {
            pending.row = WaveformModel::rowFor(*info);
        } else if (const std::optional<RangedPath> ranged = splitRange(path)) {
            const SignalInfo* signalInfo = find(ranged->path);
            if (signalInfo == nullptr) {
                warn(entry.line, QStringLiteral("%1 names nothing in this design").arg(path));
                return;
            }
            const WaveRow whole = WaveformModel::rowFor(*signalInfo);
            if (whole.kind != WaveRow::Kind::Signal
                || std::max(ranged->high, ranged->low) >= whole.signalWidth) {
                warn(entry.line, QStringLiteral("%1 is outside %2, which is %3 bits wide")
                                     .arg(path, ranged->path)
                                     .arg(whole.signalWidth));
                return;
            }
            pending.row = WaveformModel::sliceOf(whole, ranged->high, ranged->low);
        } else {
            warn(entry.line, QStringLiteral("%1 names nothing in this design").arg(path));
            return;
        }

        apply(entry, pending.row);
        if (entry.label.has_value()) pending.row.label = *entry.label;

        if (entry.expand || entry.contents) {
            if (pending.row.kind != WaveRow::Kind::Signal) {
                error(entry.line,
                      QStringLiteral("%1 is a parameter, which has no bits to unpack").arg(path));
                return;
            }
            pending.row.unpacked = true;
        }

        if (entry.expand) {
            // What unpacking makes, made the same way: each element or bit takes the radix, the
            // highlight and the name the signal has by then.
            for (WaveRow& made : WaveformModel::unpackedRows(pending.row)) {
                WaveformModel::PendingRow child;
                child.row = std::move(made);
                pending.children.push_back(std::move(child));
            }
        }

        if (entry.contents) entries(entry.body, pending.children);

        out.push_back(std::move(pending));
    }

    const QVector<ipc::InstanceEntry>& instances;
    const QHash<QString, QVector<SignalInfo>>& objects;
    ResolvedSignalList& result;
    QSet<QString> scopes;
};

void collectScopes(const std::vector<SignalListEntry>& entries,
                   const QVector<ipc::InstanceEntry>& instances, const QSet<QString>& scopes,
                   QStringList& needed) {
    // Only real scopes. The kernel answers a query for anything else with a failure rather
    // than with objects, and a load waiting for that answer would wait for ever; a path whose
    // scope does not exist is reported when the list is resolved, like any other missing path.
    const auto need = [&needed, &scopes](const QString& scope) {
        if (scopes.contains(scope) && !needed.contains(scope)) needed << scope;
    };

    for (const SignalListEntry& entry : entries) {
        for (const QString& path : entry.paths) {
            if (!scopes.contains(path)) {
                // A signal, or a range of one; either way its scope is what holds it.
                need(scopeOf(path));
                if (const std::optional<RangedPath> ranged = splitRange(path))
                    need(scopeOf(ranged->path));
                continue;
            }

            const QString prefix = path + u'/';
            for (const ipc::InstanceEntry& instance : instances) {
                const QString candidate = QString::fromStdString(instance.path);
                if (candidate == path || (entry.recursive && candidate.startsWith(prefix)))
                    need(candidate);
            }
        }
        collectScopes(entry.body, instances, scopes, needed);
    }
}

} // namespace

ParsedSignalList parseSignalList(const QByteArray& text) {
    initialiseTcl();

    ParsedSignalList result;
    Parser parser(result.errors);
    const Origin origin{.base = text.constData(), .baseLine = 1, .exact = true};
    if (!parser.parse(text.constData(), static_cast<int>(text.size()), origin, 0, result.entries))
        result.entries.clear();
    return result;
}

QString quoteSignalListWord(const QString& word) {
    if (word.isEmpty()) return QStringLiteral("{}");

    // Bare when nothing in it means anything to the parser. Brackets are characters like any
    // other in a signal list, but Tcl's parser still matches them, so they must balance.
    bool bare = !word.startsWith(u'#');
    int brackets = 0;
    for (const QChar character : word) {
        if (character.isSpace() || character == u'{' || character == u'}' || character == u'"'
            || character == u'\\' || character == u';' || character == u'$') {
            bare = false;
        }
        if (character == u'[') ++brackets;
        if (character == u']' && --brackets < 0) bare = false;
    }
    if (brackets != 0) bare = false;
    if (bare) return word;

    // Braced when its own braces balance: everything inside braces is literal, which is how a
    // label with spaces in it reads best.
    int depth = 0;
    bool balanced = !word.contains(u'\\');
    for (const QChar character : word) {
        if (character == u'{') ++depth;
        if (character == u'}' && --depth < 0) balanced = false;
    }
    if (balanced && depth == 0) return u'{' + word + u'}';

    // Otherwise each character the parser would act on is escaped.
    QString escaped;
    for (qsizetype index = 0; index < word.size(); ++index) {
        const QChar character = word[index];
        if (character == u'\n') {
            escaped += QStringLiteral("\\n");
        } else if (character == u'\t') {
            escaped += QStringLiteral("\\t");
        } else if (character == u'\r') {
            escaped += QStringLiteral("\\r");
        } else if (character.isSpace() || character == u'{' || character == u'}'
                   || character == u'"' || character == u'\\' || character == u';'
                   || character == u'$' || character == u'[' || character == u']'
                   || (index == 0 && character == u'#')) {
            escaped += u'\\';
            escaped += character;
        } else {
            escaped += character;
        }
    }
    return escaped;
}

QString radixWord(eir::Radix radix) {
    switch (radix) {
    case eir::Radix::Binary: return QStringLiteral("bin");
    case eir::Radix::Octal: return QStringLiteral("oct");
    case eir::Radix::Decimal: return QStringLiteral("dec");
    case eir::Radix::Hexadecimal: return QStringLiteral("hex");
    case eir::Radix::Ascii: return QStringLiteral("ascii");
    }
    return QStringLiteral("hex");
}

std::optional<eir::Radix> radixFromWord(const QString& word) {
    for (const eir::Radix radix : ListedRadixes) {
        if (word == radixWord(radix)) return radix;
    }
    return std::nullopt;
}

QStringList scopesNeededBy(const std::vector<SignalListEntry>& entries,
                           const QVector<ipc::InstanceEntry>& instances) {
    QSet<QString> scopes;
    for (const ipc::InstanceEntry& instance : instances)
        scopes.insert(QString::fromStdString(instance.path));

    QStringList needed;
    collectScopes(entries, instances, scopes, needed);
    return needed;
}

ResolvedSignalList resolveSignalList(const std::vector<SignalListEntry>& entries,
                                     const QVector<ipc::InstanceEntry>& instances,
                                     const QHash<QString, QVector<SignalInfo>>& objects) {
    ResolvedSignalList result;
    Resolver resolver(instances, objects, result);
    resolver.entries(entries, result.rows);
    return result;
}

} // namespace enzan::gui
