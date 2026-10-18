// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/SourceOrder.h"

#include "common/SourceManager.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

/// What the scan sees of a source: identifiers, in lower case unless extended, and the
/// punctuation the patterns below look for; everything else, a literal or another delimiter, is
/// one token that matches nothing.
struct Token {
    enum class Kind : std::uint8_t { Identifier, Dot, Other };
    Kind kind = Kind::Other;
    std::string text;
};

std::string lowered(std::string_view text) {
    std::string result(text);
    for (char& character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

bool isLetter(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) != 0;
}

bool isWordCharacter(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

/// The tokens of `text`, as far as finding unit names needs: comments, strings and character
/// literals are passed over, so that nothing inside one is taken for a name.
std::vector<Token> tokenise(std::string_view text) {
    std::vector<Token> tokens;
    const std::size_t size = text.size();
    std::size_t at = 0;
    const auto peek = [&](std::size_t offset) {
        return at + offset < size ? text[at + offset] : '\0';
    };
    const auto other = [&tokens](std::string_view what) {
        tokens.push_back(Token{.kind = Token::Kind::Other, .text = std::string(what)});
    };

    while (at < size) {
        const char character = text[at];
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            ++at;
        } else if (character == '-' && peek(1) == '-') {
            while (at < size && text[at] != '\n') ++at;
        } else if (character == '/' && peek(1) == '*') {
            at += 2;
            while (at < size && !(text[at] == '*' && peek(1) == '/')) ++at;
            at = at < size ? at + 2 : size;
        } else if (character == '"') {
            // A string, `""` standing for one quote inside it.
            ++at;
            while (at < size && text[at] != '\n') {
                if (text[at] == '"' && peek(1) == '"') {
                    at += 2;
                } else if (text[at] == '"') {
                    ++at;
                    break;
                } else {
                    ++at;
                }
            }
            other("\"");
        } else if (character == '\\') {
            // An extended identifier, `\\` standing for one backslash inside it, its case its own.
            const std::size_t start = at++;
            while (at < size && text[at] != '\n') {
                if (text[at] == '\\' && peek(1) == '\\') {
                    at += 2;
                } else if (text[at] == '\\') {
                    ++at;
                    break;
                } else {
                    ++at;
                }
            }
            tokens.push_back(Token{.kind = Token::Kind::Identifier,
                                   .text = std::string(text.substr(start, at - start))});
        } else if (character == '\'') {
            // A character literal, `'x'`, unless the tick follows a name or a closing bracket,
            // where it starts an attribute or a qualified expression.
            const bool tick = !tokens.empty()
                              && (tokens.back().kind == Token::Kind::Identifier
                                  || tokens.back().text == ")" || tokens.back().text == "]");
            if (!tick && peek(2) == '\'') {
                at += 3;
                other("'x'");
            } else {
                ++at;
                other("'");
            }
        } else if (isLetter(character)) {
            const std::size_t start = at;
            while (at < size && isWordCharacter(text[at])) ++at;
            tokens.push_back(Token{.kind = Token::Kind::Identifier,
                                   .text = lowered(text.substr(start, at - start))});
        } else if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
            // A number, based ones and their exponents among them: `16#FF#`, `1.5e3`.
            while (at < size && (isWordCharacter(text[at]) || text[at] == '#' || text[at] == '.'))
                ++at;
            other("0");
        } else if (character == '.') {
            tokens.push_back(Token{.kind = Token::Kind::Dot, .text = "."});
            ++at;
        } else {
            other(text.substr(at, 1));
            ++at;
        }
    }
    return tokens;
}

/// The primary units a source declares, and the units it names, by their names in lower case.
struct Scan {
    std::vector<std::string> declares;
    std::vector<std::string> names;
};

Scan scan(std::string_view text, const std::string& workLibrary) {
    const std::vector<Token> tokens = tokenise(text);
    const auto identifier = [&tokens](std::size_t index) {
        return index < tokens.size() && tokens[index].kind == Token::Kind::Identifier;
    };
    const auto is = [&tokens](std::size_t index, std::string_view word) {
        return index < tokens.size() && tokens[index].kind == Token::Kind::Identifier
               && tokens[index].text == word;
    };

    Scan result;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (!identifier(index)) continue;
        const std::string& word = tokens[index].text;

        // `work.x`, or the work library by its own name: a use clause's, an instantiation's, a
        // configuration's or a selected name's.
        if ((word == "work" || word == workLibrary) && index + 2 < tokens.size()
            && tokens[index + 1].kind == Token::Kind::Dot && identifier(index + 2)) {
            result.names.push_back(tokens[index + 2].text);
            continue;
        }
        if (index > 0 && is(index - 1, "end")) continue;

        if (word == "entity" && identifier(index + 1) && is(index + 2, "is")) {
            result.declares.push_back(tokens[index + 1].text);
        } else if (word == "package" && is(index + 1, "body") && identifier(index + 2)) {
            result.names.push_back(tokens[index + 2].text);
        } else if (word == "package" && identifier(index + 1) && is(index + 2, "is")) {
            result.declares.push_back(tokens[index + 1].text);
        } else if (word == "context" && identifier(index + 1) && is(index + 2, "is")) {
            result.declares.push_back(tokens[index + 1].text);
        } else if (word == "architecture" && identifier(index + 1) && is(index + 2, "of")
                   && identifier(index + 3)) {
            result.names.push_back(tokens[index + 3].text);
        } else if (word == "configuration" && identifier(index + 1) && is(index + 2, "of")
                   && identifier(index + 3)) {
            result.declares.push_back(tokens[index + 1].text);
            result.names.push_back(tokens[index + 3].text);
        }
    }
    return result;
}

} // namespace

std::vector<BufferId> dependencyOrder(std::span<const BufferId> sources,
                                      const SourceManager& manager, std::string_view workLibrary) {
    const std::string library = lowered(workLibrary);
    std::vector<Scan> scans;
    scans.reserve(sources.size());
    for (const BufferId buffer : sources)
        scans.push_back(scan(manager.bufferText(buffer), library));

    std::map<std::string, std::vector<std::size_t>> declaredBy;
    for (std::size_t index = 0; index < scans.size(); ++index) {
        for (const std::string& name : scans[index].declares) declaredBy[name].push_back(index);
    }
    std::vector<std::set<std::size_t>> needs(sources.size());
    for (std::size_t index = 0; index < scans.size(); ++index) {
        for (const std::string& name : scans[index].names) {
            const auto found = declaredBy.find(name);
            if (found == declaredBy.end()) continue;
            for (const std::size_t declarer : found->second)
                if (declarer != index) needs[index].insert(declarer);
        }
    }

    // Each step takes the first source given whose units' prerequisites are all taken, so that an
    // order that already works is kept; where none is ready, the sources name each other, and the
    // first of those left goes as it was given.
    std::vector<bool> taken(sources.size(), false);
    std::vector<BufferId> ordered;
    ordered.reserve(sources.size());
    while (ordered.size() < sources.size()) {
        std::size_t next = sources.size();
        std::size_t first = sources.size();
        for (std::size_t index = 0; index < sources.size() && next == sources.size(); ++index) {
            if (taken[index]) continue;
            if (first == sources.size()) first = index;
            bool ready = true;
            for (const std::size_t need : needs[index]) ready = ready && taken[need];
            if (ready) next = index;
        }
        if (next == sources.size()) next = first;
        taken[next] = true;
        ordered.push_back(sources[next]);
    }
    return ordered;
}

} // namespace enzan::frontend::vhdl
