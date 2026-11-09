// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringView>

#include <cstdint>
#include <vector>

namespace enzan::gui {

/// How a run of a program's output looks, as a terminal's codes set it.
struct TerminalStyle {
    /// The transcript's own colour rather than a terminal colour.
    static constexpr int NoColor = -1;

    /// The terminal colour the text is in, 0 to `TerminalColors` - 1 in the codes' order, black to
    /// white and then the bright ones, or `NoColor`.
    int color = NoColor;
    bool bold = false;
    bool italic = false;
    bool underline = false;

    friend bool operator==(const TerminalStyle&, const TerminalStyle&) = default;
};

/// Text of one style.
struct TerminalRun {
    QString text;
    TerminalStyle style;
};

/// Reads what a program wrote for a terminal: splits it into runs of one style by the codes that
/// colour it (SGR, ECMA-48 section 8.3.117) and drops every other escape sequence, such as a
/// cursor's movement or a window's title, which a transcript has no use for.
///
/// **A colour is one of a terminal's sixteen**, which the theme gives (DESIGN_GUIDE.md section
/// 10.5). One a code names otherwise, of the 256 or by its red, green and blue, is taken to the one
/// of the sixteen it is nearest: each channel on or off at its middle, as the eight are the corners
/// of the colour cube, and bright when its brightest channel is near the top. Background colours,
/// reverse video and the rest are dropped.
///
/// It keeps its state from one call to the next, since a program's output arrives in whatever
/// pieces were read, a code may be split between two, and a colour stays set until a code resets
/// it, whatever lines come between.
class TerminalDecoder {
public:
    /// The runs `text` holds, in order, after whatever came before it. A code not yet finished
    /// at its end is finished by the next call.
    std::vector<TerminalRun> decode(QStringView text);

    /// Forgets the style and any code begun, as a cleared transcript starts again.
    void reset();

private:
    /// Applies a finished colour code's parameters to `style`.
    void applySelection();

    enum class State : std::uint8_t {
        /// Text.
        Text,
        /// After an escape.
        Escape,
        /// In a control sequence, `ESC [`, up to its final byte.
        Control,
        /// In a string, an operating system command `ESC ]` or its kin, up to its terminator.
        String,
        /// After an escape in a string, which a `\` ends.
        StringEscape,
    };

    State state = State::Text;
    QString parameters;
    bool privateParameters = false;
    TerminalStyle style;
};

} // namespace enzan::gui
