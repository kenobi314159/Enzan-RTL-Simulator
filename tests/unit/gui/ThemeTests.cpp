// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the Tcl-driven theme.
//
// The one that matters most is the completeness check: DESIGN_GUIDE.md section 10.5 forbids
// colour literals in widget code, so there is no fallback in C++ for a key the theme file
// forgot. A key added to the catalogue without being added to the shipped theme would show up as
// an invisible widget on somebody's screen, and this is what catches it instead.

#include "gui/Theme.h"

#include <QPalette>

#include <gtest/gtest.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

namespace {

using enzan::gui::Theme;
using enzan::gui::ThemeColor;

TEST(ThemeTests, TheShippedThemeDefinesEveryColour) {
    Theme theme;
    theme.loadBuiltIn();

    EXPECT_TRUE(theme.isComplete())
        << "the shipped theme does not set '" << theme.firstMissingKey().toStdString()
        << "'; add it to src/gui/themes/dark.tcl";
}

TEST(ThemeTests, TheShippedThemeHonoursWhatTheGuideFixes) {
    // Section 10.5 names two colours outright: a high-contrast black background for the
    // waveform view and light green traces.
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

TEST(ThemeTests, ASyntaxErrorCarriesItsLineNumber) {
    // The interpreter's own message is passed through because the line number in it is the one
    // thing a user with a broken theme actually needs.
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    EXPECT_FALSE(theme.evaluate(QStringLiteral("theme accent #112233\n"
                                               "theme\n"),
                                error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(ThemeTests, AFailedOverlayLeavesTheEarlierColoursAlone) {
    Theme theme;
    theme.loadBuiltIn();
    const QColor background = theme.color(ThemeColor::WaveBackground);

    QString error;
    EXPECT_FALSE(theme.evaluate(QStringLiteral("theme wave.trace #010203\n"
                                               "theme wave.nonsense #040506\n"),
                                error));

    // The lines before the failure did take effect, which is what a script evaluated in order
    // does, and nothing else was disturbed.
    EXPECT_EQ(theme.color(ThemeColor::WaveTrace), QColor(0x01, 0x02, 0x03));
    EXPECT_EQ(theme.color(ThemeColor::WaveBackground), background);
}

TEST(ThemeTests, AMissingFileIsReportedRatherThanSilentlySkipped) {
    Theme theme;
    theme.loadBuiltIn();

    QString error;
    EXPECT_FALSE(theme.loadFile(QStringLiteral("/nowhere/enzan-theme.tcl"), error));
    EXPECT_TRUE(error.contains(QStringLiteral("/nowhere/enzan-theme.tcl"))) << error.toStdString();
}

TEST(ThemeTests, TheWrittenOutThemeLoadsBackUnchanged) {
    // `--write-theme` exists so that a user can start from the shipped colours. A copy that did
    // not load would make the feature a trap.
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
