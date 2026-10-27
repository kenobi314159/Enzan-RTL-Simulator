// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/TclSyntax.h"

#include "gui/TclLibrary.h"

#include <algorithm>
#include <array>
#include <utility>

#include <tcl.h>

namespace enzan::gui {

namespace {

int lineOf(const TclOrigin& origin, const char* position) {
    if (!origin.exact || position == nullptr || position < origin.base) return origin.baseLine;
    return origin.baseLine + static_cast<int>(std::count(origin.base, position, '\n'));
}

std::vector<TclWord> wordsOf(const Tcl_Parse& parse) {
    std::vector<TclWord> words;
    const Tcl_Token* token = parse.tokenPtr;

    for (int index = 0; index < parse.numWords; ++index) {
        TclWord word;
        word.start = token->start;
        word.size = token->size;
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
                // A command or a variable substitution in Tcl, and here the characters as
                // written: `/tb/gen[0].q` is a path, not a call to `0`.
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

} // namespace

bool splitTclCommands(const char* text, int size, const TclOrigin& origin,
                      const std::function<bool(const TclCommand&)>& each,
                      std::optional<TclSyntaxError>& malformed) {
    initialiseTcl();

    const char* cursor = text;
    int remaining = size;

    while (remaining > 0) {
        Tcl_Parse parse;
        if (Tcl_ParseCommand(nullptr, cursor, remaining, 0, &parse) != TCL_OK) {
            malformed = TclSyntaxError{.line = lineOf(origin, parse.term),
                                       .text = describeParseError(parse.errorType)};
            Tcl_FreeParse(&parse);
            return false;
        }

        TclCommand command{.words = wordsOf(parse), .line = lineOf(origin, parse.commandStart)};
        const char* next = parse.commandStart + parse.commandSize;
        Tcl_FreeParse(&parse);

        if (!command.words.empty() && !each(command)) return false;

        // A parse always consumes something while there is text left; the test is here so that
        // a parser that one day did not would stop rather than spin.
        if (next <= cursor) break;
        remaining -= static_cast<int>(next - cursor);
        cursor = next;
    }

    return true;
}

QString quoteTclWord(const QString& word) {
    if (word.isEmpty()) return QStringLiteral("{}");

    // Bare when nothing in it means anything to the parser. Brackets are characters like any
    // other here, but Tcl's parser still matches them, so they must balance.
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

} // namespace enzan::gui
