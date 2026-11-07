// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the Tcl-driven theme.
//
// The one that matters most is the completeness check: widget code holds no colour literals, so
// there is no fallback in C++ for a key a theme file forgot. A key added to the catalogue
// without being added to every shipped theme would show up as an invisible widget on somebody's
// screen, and this is what catches it instead.

#include "gui/Theme.h"

#include <QPalette>

#include <gtest/gtest.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

using enzan::gui::Theme;
using enzan::gui::ThemeColor;
using enzan::gui::ThemeFont;

/// The contrast between two colours as WCAG 2 measures it, from 1 to 21.
double contrast(const QColor& first, const QColor& second) {
    const auto luminance = [](const QColor& colour) {
        const auto channel = [](double value) {
            return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return (0.2126 * channel(static_cast<double>(colour.redF())))
               + (0.7152 * channel(static_cast<double>(colour.greenF())))
               + (0.0722 * channel(static_cast<double>(colour.blueF())));
    };
    const double lighter = std::max(luminance(first), luminance(second));
    const double darker = std::min(luminance(first), luminance(second));
    return (lighter + 0.05) / (darker + 0.05);
}

TEST(ThemeTests, EveryShippedThemeDefinesEveryColourOnItsOwn) {
    // Evaluated alone rather than over the default, so that a key one theme forgot is not hidden
    // by the default's colour for it — a dark patch in a light window.
    for (const QString& name : Theme::builtInNames()) {
        Theme theme;
        QString error;
        ASSERT_TRUE(theme.evaluate(Theme::builtInScript(name), error))
            << name.toStdString() << ": " << error.toStdString();
        EXPECT_TRUE(theme.isComplete())
            << "the shipped theme '" << name.toStdString() << "' does not set '"
            << theme.firstMissingKey().toStdString() << "'; add it to src/gui/themes/"
            << name.toStdString() << ".tcl";
    }
}

TEST(ThemeTests, TheDarkThemeIsTheDefaultAndALightOneIsShippedBesideIt) {
    const QStringList names = Theme::builtInNames();
    ASSERT_FALSE(names.isEmpty());
    EXPECT_EQ(names.front(), QString::fromLatin1(Theme::DefaultBuiltIn));
    EXPECT_EQ(names.front(), QStringLiteral("dark"));
    EXPECT_TRUE(names.contains(QStringLiteral("light")));

    Theme byDefault;
    byDefault.loadBuiltIn();
    Theme dark;
    ASSERT_TRUE(dark.loadBuiltIn(QStringLiteral("dark")));
    EXPECT_EQ(byDefault.color(ThemeColor::WaveTrace), dark.color(ThemeColor::WaveTrace));
    EXPECT_EQ(Theme::builtInScript(), Theme::builtInScript(QStringLiteral("dark")));
}

TEST(ThemeTests, TheLightThemeIsLightExceptForWaveform) {
    Theme light;
    ASSERT_TRUE(light.loadBuiltIn(QStringLiteral("light")));
    for (const ThemeColor ground : {ThemeColor::WindowBackground, ThemeColor::PanelBackground,
                                    ThemeColor::SourceBackground}) {
        EXPECT_GT(light.color(ground).lightnessF(), 0.9F) << enzan::gui::themeKey(ground);
    }
    for (const ThemeColor ground : {ThemeColor::WaveBackground}) {
        EXPECT_LT(light.color(ground).lightnessF(), 0.1F) << enzan::gui::themeKey(ground);
    }
    EXPECT_LT(light.color(ThemeColor::PanelForeground).lightnessF(), 0.3F);
}

TEST(ThemeTests, EveryShippedThemeIsReadable) {
    // Text at the contrast WCAG asks of body text, and lines and marks — a trace, a cursor, a
    // line number — at the contrast it asks of what is not text. Held for every shipped theme,
    // because a theme whose traces vanish into its own background is worse than none.
    struct Pair {
        ThemeColor foreground;
        ThemeColor background;
        double least;
    };

    constexpr double Text = 4.5;
    constexpr double Mark = 3.0;
    const Pair pairs[] = {
        {.foreground = ThemeColor::WindowForeground,
         .background = ThemeColor::WindowBackground,
         .least = Text},
        {.foreground = ThemeColor::PanelForeground,
         .background = ThemeColor::PanelBackground,
         .least = Text},
        {.foreground = ThemeColor::HeaderForeground,
         .background = ThemeColor::HeaderBackground,
         .least = Text},
        // The hint an empty field shows, on the field.
        {.foreground = ThemeColor::HeaderForeground,
         .background = ThemeColor::PanelBackground,
         .least = Text},
        {.foreground = ThemeColor::SelectionForeground,
         .background = ThemeColor::SelectionBackground,
         .least = Text},
        {.foreground = ThemeColor::ControlForeground,
         .background = ThemeColor::ControlBackground,
         .least = Text},
        {.foreground = ThemeColor::TranscriptForeground,
         .background = ThemeColor::PanelBackground,
         .least = Text},
        {.foreground = ThemeColor::TranscriptNote,
         .background = ThemeColor::PanelBackground,
         .least = Text},
        {.foreground = ThemeColor::TranscriptWarning,
         .background = ThemeColor::PanelBackground,
         .least = Text},
        {.foreground = ThemeColor::TranscriptError,
         .background = ThemeColor::PanelBackground,
         .least = Text},
        {.foreground = ThemeColor::SourceForeground,
         .background = ThemeColor::SourceBackground,
         .least = Text},
        {.foreground = ThemeColor::SourceGutterForeground,
         .background = ThemeColor::SourceGutterBackground,
         .least = Mark},
        {.foreground = ThemeColor::WaveText,
         .background = ThemeColor::WaveBackground,
         .least = Text},
        {.foreground = ThemeColor::WaveText,
         .background = ThemeColor::WaveSelectedRow,
         .least = Text},
        {.foreground = ThemeColor::WaveRulerForeground,
         .background = ThemeColor::WaveRulerBackground,
         .least = Text},
        {.foreground = ThemeColor::WaveCursorText,
         .background = ThemeColor::WaveCursor,
         .least = Text},
        {.foreground = ThemeColor::WaveCursorText,
         .background = ThemeColor::WaveCursorLocked,
         .least = Text},
        {.foreground = ThemeColor::WaveTrace,
         .background = ThemeColor::WaveBackground,
         .least = Mark},
        {.foreground = ThemeColor::WaveTrace,
         .background = ThemeColor::WaveSelectedRow,
         .least = Mark},
        {.foreground = ThemeColor::WaveTrace,
         .background = ThemeColor::WaveBeyondRecorded,
         .least = Mark},
        {.foreground = ThemeColor::WaveUnknown,
         .background = ThemeColor::WaveBackground,
         .least = Mark},
        {.foreground = ThemeColor::WaveHighImpedance,
         .background = ThemeColor::WaveBackground,
         .least = Mark},
        {.foreground = ThemeColor::WaveCursor,
         .background = ThemeColor::WaveBackground,
         .least = Mark},
        {.foreground = ThemeColor::WaveCursorLocked,
         .background = ThemeColor::WaveBackground,
         .least = Mark},
    };

    for (const QString& name : Theme::builtInNames()) {
        Theme theme;
        ASSERT_TRUE(theme.loadBuiltIn(name));
        for (const Pair& pair : pairs) {
            EXPECT_GE(contrast(theme.color(pair.foreground), theme.color(pair.background)),
                      pair.least)
                << name.toStdString() << ": " << enzan::gui::themeKey(pair.foreground) << " on "
                << enzan::gui::themeKey(pair.background);
        }
        for (int slot = 1; slot <= enzan::gui::HighlightSlots; ++slot) {
            const QColor highlight = theme.color(enzan::gui::highlightColor(slot));
            EXPECT_GE(contrast(theme.color(ThemeColor::WaveTrace), highlight), Mark)
                << name.toStdString() << ": the trace on highlight " << slot;
            EXPECT_GE(contrast(theme.color(ThemeColor::WaveHighlightText), highlight), Text)
                << name.toStdString() << ": a name on highlight " << slot;
        }
    }
}

TEST(ThemeTests, AThemeNoneIsBuiltInUnderChangesNothing) {
    Theme theme;
    theme.loadBuiltIn();
    const QColor trace = theme.color(ThemeColor::WaveTrace);

    EXPECT_FALSE(theme.loadBuiltIn(QStringLiteral("sepia")));
    EXPECT_EQ(theme.color(ThemeColor::WaveTrace), trace);
    EXPECT_TRUE(Theme::builtInScript(QStringLiteral("sepia")).isEmpty());
    EXPECT_TRUE(Theme::builtInScript(QStringLiteral("../dark")).isEmpty());
}

TEST(ThemeTests, TheDefaultThemeHasABlackWaveBackgroundAndLightGreenTraces) {
    // The waveform view's two colours that are part of the design: a high-contrast black
    // background and light green traces.
    Theme theme;
    theme.loadBuiltIn();

    EXPECT_EQ(theme.color(ThemeColor::WaveBackground), QColor(Qt::black));

    const QColor trace = theme.color(ThemeColor::WaveTrace);
    EXPECT_GT(trace.green(), trace.red());
    EXPECT_GT(trace.green(), trace.blue());
    EXPECT_GT(trace.lightness(), 100) << "the trace colour is meant to be light green";
}

TEST(ThemeTests, AUserThemeIsLayeredOverTheShippedOne) {
    // A theme that set three colours and left the rest undefined would be useless, so a user
    // script overlays the built-in one rather than replacing it.
    Theme theme;
    theme.loadBuiltIn();
    const QColor untouched = theme.color(ThemeColor::WaveCursor);

    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme wave.trace #ff00ff"), error))
        << error.toStdString();

    EXPECT_EQ(theme.color(ThemeColor::WaveTrace), QColor(0xFF, 0x00, 0xFF));
    EXPECT_EQ(theme.color(ThemeColor::WaveCursor), untouched);
    EXPECT_TRUE(theme.isComplete());
}

TEST(ThemeTests, ColourNamesAndAlphaAreAccepted) {
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme accent red\n"
                                              "theme wave.grid #80123456\n"),
                               error))
        << error.toStdString();

    EXPECT_EQ(theme.color(ThemeColor::Accent), QColor(Qt::red));
    EXPECT_EQ(theme.color(ThemeColor::WaveGrid).alpha(), 0x80);
}

TEST(ThemeTests, AnUnknownKeyIsNamedRatherThanIgnored) {
    // Silently dropping a key is how a user spends an afternoon wondering why their colour did
    // not take effect.
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    EXPECT_FALSE(theme.evaluate(QStringLiteral("theme wave.sparkle #112233"), error));
    EXPECT_TRUE(error.contains(QStringLiteral("wave.sparkle"))) << error.toStdString();
}

TEST(ThemeTests, AColourThatIsNotAColourIsReported) {
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    EXPECT_FALSE(theme.evaluate(QStringLiteral("theme accent bananas"), error));
    EXPECT_TRUE(error.contains(QStringLiteral("bananas"))) << error.toStdString();
}

TEST(ThemeTests, TheShippedThemeLeavesEveryFontAtTheDesktopsSize) {
    // How large text should be is the reader's screen's to say, so nothing is set until a theme
    // sets it, and a font is given back as it came.
    Theme theme;
    theme.loadBuiltIn();

    for (const ThemeFont area :
         {ThemeFont::Window, ThemeFont::Wave, ThemeFont::Transcript, ThemeFont::Source}) {
        EXPECT_FALSE(theme.fontSize(area).has_value()) << static_cast<int>(area);
    }
    QFont desktop;
    desktop.setPointSizeF(9.5);
    EXPECT_EQ(theme.font(ThemeFont::Wave, desktop).pointSizeF(), 9.5);
}

TEST(ThemeTests, AnAreaLeftUnsetTakesTheWindowsFontSize) {
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("font window 12\nfont wave 14.5"), error))
        << error.toStdString();

    EXPECT_EQ(theme.fontSize(ThemeFont::Wave), 14.5);
    EXPECT_EQ(theme.fontSize(ThemeFont::Transcript), 12.0);
    EXPECT_EQ(theme.fontSize(ThemeFont::Source), 12.0);
    EXPECT_EQ(theme.font(ThemeFont::Transcript, QFont()).pointSizeF(), 12.0);
}

TEST(ThemeTests, AFontSizeThatIsNotOneIsReported) {
    // As a colour that is not one is: named, with nothing set.
    const std::pair<const char*, const char*> cases[] = {
        {"font wave big", "'big' is not a font size"},
        {"font wave 0", "'0' is not a font size"},
        {"font wave -3", "'-3' is not a font size"},
        {"font wave 500", "'500' is not a font size"},
        {"font margins 10", "'margins' is not an area of the window"},
        {"font wave", "wrong # args"},
    };
    for (const auto& [script, expected] : cases) {
        Theme theme;
        theme.loadBuiltIn();
        QString error;
        EXPECT_FALSE(theme.evaluate(QString::fromUtf8(script), error)) << script;
        EXPECT_TRUE(error.contains(QString::fromUtf8(expected)))
            << script << ": " << error.toStdString();
        EXPECT_FALSE(theme.fontSize(ThemeFont::Wave).has_value()) << script;
    }
}

TEST(ThemeTests, AnErrorCarriesItsLineNumber) {
    // The interpreter's message is the error alone, and the line it stopped at is the one thing
    // a user with a broken theme actually needs, so it is put in front.
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    EXPECT_FALSE(theme.evaluate(QStringLiteral("theme accent #112233\n"
                                               "theme\n"),
                                error));
    EXPECT_TRUE(error.startsWith(QStringLiteral("line 2: "))) << error.toStdString();
}

TEST(ThemeTests, AFailedScriptChangesNothing) {
    // Whole or not at all: the lines before the mistake do not take effect either, so a window
    // is never coloured partly by a theme that did not load.
    Theme theme;
    theme.loadBuiltIn();
    const QColor trace = theme.color(ThemeColor::WaveTrace);

    QString error;
    EXPECT_FALSE(theme.evaluate(QStringLiteral("theme wave.trace #010203\n"
                                               "font wave 30\n"
                                               "theme wave.nonsense #040506\n"),
                                error));

    EXPECT_EQ(theme.color(ThemeColor::WaveTrace), trace);
    EXPECT_FALSE(theme.fontSize(ThemeFont::Wave).has_value());
}

TEST(ThemeTests, AFileWithAMistakeChangesNothingAndSaysWhere) {
    const QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("broken.tcl"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("theme wave.trace #010203\n\ntheme wave.nonsense #040506\n");
    file.close();

    Theme theme;
    theme.loadBuiltIn();
    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme wave.cursor #0a0b0c"), error));

    EXPECT_FALSE(theme.loadFile(path, error));
    EXPECT_TRUE(error.startsWith(path + QStringLiteral(":3: "))) << error.toStdString();
    EXPECT_EQ(theme.color(ThemeColor::WaveCursor), QColor(0x0A, 0x0B, 0x0C))
        << "a file that did not load must leave what was there before";
}

TEST(ThemeTests, ALoadedFileReplacesWhatWasThereBefore) {
    // A theme file is the shipped theme with the file over it, not the file over whatever the
    // window happened to have, so switching from one theme to another leaves nothing of the first.
    const QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("mine.tcl"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("theme wave.trace #010203\n");
    file.close();

    Theme builtIn;
    builtIn.loadBuiltIn();

    Theme theme;
    theme.loadBuiltIn();
    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme wave.cursor #0a0b0c\nfont wave 30"), error));

    ASSERT_TRUE(theme.loadFile(path, error)) << error.toStdString();
    EXPECT_EQ(theme.color(ThemeColor::WaveTrace), QColor(0x01, 0x02, 0x03));
    EXPECT_EQ(theme.color(ThemeColor::WaveCursor), builtIn.color(ThemeColor::WaveCursor));
    EXPECT_FALSE(theme.fontSize(ThemeFont::Wave).has_value());
}

TEST(ThemeTests, AMissingFileIsReportedRatherThanSilentlySkipped) {
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    EXPECT_FALSE(theme.loadFile(QStringLiteral("/nowhere/enzan-theme.tcl"), error));
    EXPECT_TRUE(error.contains(QStringLiteral("/nowhere/enzan-theme.tcl"))) << error.toStdString();
}

TEST(ThemeTests, TheWrittenOutThemeLoadsBackUnchanged) {
    // New Theme writes the shipped theme out, so that a user can start from the shipped colours.
    // A copy that did not load would make that a trap.
    const QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("copy.tcl"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(Theme::builtInScript().toUtf8());
    file.close();

    Theme fromFile;
    QString error;
    ASSERT_TRUE(fromFile.loadFile(path, error)) << error.toStdString();
    EXPECT_TRUE(fromFile.isComplete());

    Theme builtIn;
    builtIn.loadBuiltIn();
    EXPECT_EQ(fromFile.color(ThemeColor::WaveTrace), builtIn.color(ThemeColor::WaveTrace));
}

TEST(ThemeTests, ThePaletteCarriesTheLoadedColours) {
    // Both the palette and the style sheet are generated rather than shipped, because either
    // one with colours written in it would be a second place colours live.
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme window.background #0a0b0c"), error));
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme selection.background #0d0e0f"), error));
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme panel.background #101112"), error));

    const QPalette palette = theme.palette();
    EXPECT_EQ(palette.color(QPalette::Window), QColor(QStringLiteral("#0a0b0c")));
    EXPECT_EQ(palette.color(QPalette::Highlight), QColor(QStringLiteral("#0d0e0f")));
    EXPECT_EQ(palette.color(QPalette::Base), QColor(QStringLiteral("#101112")))
        << "an item view's background is the palette's Base, which is what lets it scroll";
}

TEST(ThemeTests, AnEmptyFieldsHintIsTheThemesQuietText) {
    // Left to the style, the hint was a grey meant for a light window, which the Find value
    // field showed all but invisible on a dark theme's.
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme header.foreground #a1a2a3"), error));

    const QPalette palette = theme.palette();
    for (const QPalette::ColorGroup group :
         {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        EXPECT_EQ(palette.color(group, QPalette::PlaceholderText),
                  QColor(QStringLiteral("#a1a2a3")))
            << "colour group " << static_cast<int>(group);
    }
}

TEST(ThemeTests, TheStyleSheetCarriesTheColoursAPaletteCannot) {
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    ASSERT_TRUE(theme.evaluate(QStringLiteral("theme header.background #0a0b0c"), error));

    // The header and the dock title have no palette entry of their own, so they stay here.
    EXPECT_TRUE(theme.styleSheet().contains(QStringLiteral("#ff0a0b0c"), Qt::CaseInsensitive))
        << theme.styleSheet().toStdString();
}

TEST(ThemeTests, TheStyleSheetNeverStylesAScrollingSurface) {
    // This is a performance rule with a correctness-shaped test, and it is worth one. A style
    // sheet that gives a scrolling surface a background or a border makes Qt draw that widget
    // through the style sheet's box model, and a widget drawn that way cannot scroll by moving
    // what it already has — it repaints instead. Measured on a list of two hundred signals in
    // a tall window, a single `QAbstractScrollArea { border }` rule was three quarters of what
    // scrolling cost. It went into the palette; this says it stays there.
    Theme theme;
    theme.loadBuiltIn();

    const QString sheet = theme.styleSheet();
    for (const QString& selector :
         {QStringLiteral("QWidget"), QStringLiteral("QAbstractScrollArea"),
          QStringLiteral("QTreeView"), QStringLiteral("QTableView"), QStringLiteral("QListView"),
          QStringLiteral("QPlainTextEdit")}) {
        // A rule for one of these would begin a block; the sub-controls they have entries for,
        // such as `QHeaderView::section`, are named differently and are not affected.
        EXPECT_FALSE(sheet.contains(selector + QStringLiteral(" {")))
            << selector.toStdString() << " must not be styled: " << sheet.toStdString();
        EXPECT_FALSE(sheet.contains(selector + QStringLiteral(",")))
            << selector.toStdString() << " must not be styled: " << sheet.toStdString();
    }
}

TEST(ThemeTests, EveryCatalogueKeyHasANameThatLooksLikeOne) {
    for (int index = 0; index < static_cast<int>(ThemeColor::Count); ++index) {
        const QString key = QString::fromUtf8(enzan::gui::themeKey(static_cast<ThemeColor>(index)));
        EXPECT_FALSE(key.isEmpty());
        EXPECT_EQ(key, key.toLower()) << "theme keys are lowercase: " << key.toStdString();
    }
}

} // namespace
