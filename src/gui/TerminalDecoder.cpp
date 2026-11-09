// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/TerminalDecoder.h"

#include <QList>

#include <algorithm>
#include <cstddef>
#include <optional>

namespace enzan::gui {

namespace {

constexpr char16_t EscapeCharacter = 0x1b;
constexpr char16_t Bell = 0x07;

/// The kinds of an extended colour: one of the 256, and one by its channels.
constexpr int IndexedColor = 5;
constexpr int DirectColor = 2;

/// The terminal colour nearest a colour of `red`, `green` and `blue`, 0 to 255 each: each channel
/// on or off at its middle, which picks one of the eight in the codes' order, red the first bit,
/// green the second and blue the third; and the bright one of the eight when its brightest
/// channel is near the top, or for black when it is a grey rather than black.
int nearestColor(int red, int green, int blue) {
    constexpr int Middle = 128;
    constexpr int NearTheTop = 224;
    constexpr int Grey = 64;
    constexpr int BrightOnes = 8;
    const int color =
        (red >= Middle ? 1 : 0) + (green >= Middle ? 2 : 0) + (blue >= Middle ? 4 : 0);
    const int brightest = std::max({red, green, blue});
    const bool bright = color == 0 ? brightest >= Grey : brightest >= NearTheTop;
    return bright ? color + BrightOnes : color;
}

/// The terminal colour nearest colour `index` of the 256: the sixteen themselves, then a cube of
/// six levels a channel, then 24 greys.
std::optional<int> colorOf256(int index) {
    constexpr int Sixteen = 16;
    constexpr int CubeEnd = 232;
    constexpr int Last = 255;
    if (index < 0 || index > Last) return std::nullopt;
    if (index < Sixteen) return index;
    if (index < CubeEnd) {
        const int cube = index - Sixteen;
        const auto level = [](int step) { return step == 0 ? 0 : 55 + (40 * step); };
        return nearestColor(level(cube / 36), level((cube / 6) % 6), level(cube % 6));
    }
    const int grey = 8 + (10 * (index - CubeEnd));
    return nearestColor(grey, grey, grey);
}

/// The terminal colour an extended colour's arguments name, `5;<index>` or `2;<r>;<g>;<b>`, the
/// latter with a colour space before the channels when given in subparameters; nothing when they
/// name none.
std::optional<int> extendedColor(const std::vector<int>& arguments) {
    if (arguments.empty()) return std::nullopt;
    if (arguments[0] == IndexedColor && arguments.size() >= 2) return colorOf256(arguments[1]);
    if (arguments[0] != DirectColor || arguments.size() < 4) return std::nullopt;
    const std::size_t first = arguments.size() >= 5 ? 2 : 1;
    const auto channel = [&](std::size_t at) { return std::clamp(arguments[at], 0, 255); };
    return nearestColor(channel(first), channel(first + 1), channel(first + 2));
}

} // namespace

std::vector<TerminalRun> TerminalDecoder::decode(QStringView text) {
    std::vector<TerminalRun> runs;
    QString current;
    const auto finishRun = [&] {
        if (current.isEmpty()) return;
        if (!runs.empty() && runs.back().style == style)
            runs.back().text += current;
        else
            runs.push_back(TerminalRun{.text = current, .style = style});
        current.clear();
    };

    for (const QChar character : text) {
        const char16_t unit = character.unicode();
        switch (state) {
        case State::Text:
            if (unit == EscapeCharacter) {
                finishRun();
                state = State::Escape;
            } else {
                current += character;
            }
            break;

        case State::Escape:
            if (unit == u'[') {
                state = State::Control;
                parameters.clear();
                privateParameters = false;
            } else if (unit == u']' || unit == u'P' || unit == u'X' || unit == u'^'
                       || unit == u'_') {
                state = State::String;
            } else {
                // A sequence of two characters, dropped whole; but a line break ends it unfinished
                // and stays, so that a stray escape cannot join two lines.
                state = State::Text;
                if (unit < u' ') current += character;
            }
            break;

        case State::Control:
            if (unit >= 0x40 && unit <= 0x7e) {
                if (unit == u'm') applySelection();
                state = State::Text;
            } else if (unit >= 0x30 && unit <= 0x3f) {
                // `<`, `=`, `>` and `?` mark a private sequence, which is no colour code.
                if (unit >= u'<') privateParameters = true;
                parameters += character;
            } else if (unit >= 0x20 && unit <= 0x2f) {
                // An intermediate byte makes it another function than SGR.
                privateParameters = true;
            } else {
                state = State::Text;
                current += character;
            }
            break;

        case State::String:
            if (unit == Bell) {
                state = State::Text;
            } else if (unit == EscapeCharacter) {
                state = State::StringEscape;
            } else if (unit == u'\n') {
                state = State::Text;
                current += character;
            }
            break;

        case State::StringEscape: state = unit == u'\\' ? State::Text : State::String; break;
        }
    }
    finishRun();
    return runs;
}

void TerminalDecoder::reset() {
    state = State::Text;
    parameters.clear();
    privateParameters = false;
    style = TerminalStyle();
}

void TerminalDecoder::applySelection() {
    if (privateParameters) return;

    // Each parameter with its subparameters, which a colon separates: `38;5;208` and `38:5:208`
    // say the same. An omitted number is -1, which for a parameter means 0.
    std::vector<std::vector<int>> groups;
    for (const QStringView parameter : QStringView(parameters).split(u';')) {
        std::vector<int> numbers;
        for (const QStringView number : parameter.split(u':'))
            numbers.push_back(number.isEmpty() ? -1 : number.toInt());
        groups.push_back(std::move(numbers));
    }

    constexpr int Reset = 0;
    constexpr int Bold = 1;
    constexpr int Italic = 3;
    constexpr int Underline = 4;
    constexpr int DoubleUnderline = 21;
    constexpr int NormalIntensity = 22;
    constexpr int NotItalic = 23;
    constexpr int NotUnderlined = 24;
    constexpr int Foreground = 30;
    constexpr int ExtendedForeground = 38;
    constexpr int DefaultForeground = 39;
    constexpr int ExtendedBackground = 48;
    constexpr int BrightForeground = 90;
    constexpr int Colors = 8;

    for (std::size_t at = 0; at < groups.size(); ++at) {
        const std::vector<int>& group = groups[at];
        const int code = std::max(group.front(), 0);
        if (code == Reset) {
            style = TerminalStyle();
        } else if (code == Bold) {
            style.bold = true;
        } else if (code == NormalIntensity) {
            style.bold = false;
        } else if (code == Italic) {
            style.italic = true;
        } else if (code == NotItalic) {
            style.italic = false;
        } else if (code == Underline) {
            // `4:0` is no underline; `4:3` a curly one, which is still one.
            style.underline = group.size() < 2 || group[1] != 0;
        } else if (code == DoubleUnderline) {
            style.underline = true;
        } else if (code == NotUnderlined) {
            style.underline = false;
        } else if (code >= Foreground && code < Foreground + Colors) {
            style.color = code - Foreground;
        } else if (code >= BrightForeground && code < BrightForeground + Colors) {
            style.color = code - BrightForeground + Colors;
        } else if (code == DefaultForeground) {
            style.color = TerminalStyle::NoColor;
        } else if (code == ExtendedForeground || code == ExtendedBackground) {
            // Its arguments are its subparameters, or else the parameters after it, which are
            // consumed either way, so that a background's do not read as codes of their own.
            std::vector<int> arguments(group.begin() + 1, group.end());
            if (arguments.empty() && at + 1 < groups.size()) {
                const int kind = groups[++at].front();
                arguments.push_back(kind);
                std::size_t more = 0;
                if (kind == IndexedColor) more = 1;
                if (kind == DirectColor) more = 3;
                for (std::size_t taken = 0; taken < more && at + 1 < groups.size(); ++taken)
                    arguments.push_back(groups[++at].front());
            }
            if (code == ExtendedForeground) {
                if (const std::optional<int> color = extendedColor(arguments)) style.color = *color;
            }
        }
        // Anything else, a background, reverse video or blinking among them, is not shown.
    }
}

} // namespace enzan::gui
