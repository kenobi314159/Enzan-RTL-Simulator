// SPDX-License-Identifier: GPL-3.0-or-later
//
// The configuration directory: what the window makes the first time it opens, what `config` may
// say, and how the window rewrites it without disturbing the rest of the file. Every directory
// here is a temporary one; GuiTestMain points the standard location at one too, so nothing here
// can reach the settings of whoever runs the suite.

#include "gui/Configuration.h"

#include "gui/Theme.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <optional>
#include <string>

namespace {

using enzan::gui::ConfigurationDirectory;
using enzan::gui::configurationWithTheme;
using enzan::gui::ConfiguredTheme;
using enzan::gui::describe;
using enzan::gui::parseConfiguration;
using enzan::gui::ParsedConfiguration;
using enzan::gui::Theme;
using enzan::gui::ThemeColor;
using enzan::gui::ThemeName;

QByteArray contentsOf(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void write(const QString& path, const QByteArray& contents) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
    file.write(contents);
}

std::string firstError(const ParsedConfiguration& parsed) {
    if (parsed.errors.isEmpty()) return "no error";
    return describe(parsed.errors.front()).toStdString();
}

class ConfigurationTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(home.isValid()); }

    /// A directory inside the temporary one that does not exist yet, as on a first start.
    [[nodiscard]] QString fresh() const { return home.filePath(QStringLiteral("enzan")); }

    QTemporaryDir home;
};

// ---------------------------------------------------------------------------
// Where it is
// ---------------------------------------------------------------------------

TEST_F(ConfigurationTest, TheStandardPlaceIsEnzanInTheConfigurationHome) {
    // GuiTestMain set XDG_CONFIG_HOME, which is what a user who has moved their settings
    // elsewhere sets too.
    const QString configurationHome = qEnvironmentVariable("XDG_CONFIG_HOME");
    ASSERT_FALSE(configurationHome.isEmpty());
    EXPECT_EQ(ConfigurationDirectory::standardPath(), configurationHome + QStringLiteral("/enzan"));
}

// ---------------------------------------------------------------------------
// The first start
// ---------------------------------------------------------------------------

TEST_F(ConfigurationTest, TheFirstStartWritesTheShippedThemeAndNamesIt) {
    const ConfigurationDirectory directory(fresh());
    QString error;
    ASSERT_TRUE(directory.createDefaults(error)) << error.toStdString();

    EXPECT_EQ(contentsOf(directory.resolve(QStringLiteral("themes/dark.tcl"))),
              Theme::builtInScript().toUtf8());

    const ParsedConfiguration parsed = directory.read();
    EXPECT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    EXPECT_TRUE(parsed.warnings.isEmpty());
    EXPECT_EQ(parsed.theme.file, QStringLiteral("themes/dark.tcl"));

    const ConfiguredTheme configured = directory.loadTheme();
    EXPECT_TRUE(configured.problems.isEmpty());
    Theme builtIn;
    builtIn.loadBuiltIn();
    EXPECT_EQ(configured.theme.color(ThemeColor::WaveTrace), builtIn.color(ThemeColor::WaveTrace));
}

TEST_F(ConfigurationTest, NothingThatExistsIsReplaced) {
    // The directory is the reader's. A theme they edited survives a missing `config`, and a
    // `config` they wrote survives every start after the first.
    const ConfigurationDirectory directory(fresh());
    const QString theme = directory.resolve(QStringLiteral("themes/dark.tcl"));
    write(theme, "theme wave.trace #010203\n");

    QString error;
    ASSERT_TRUE(directory.createDefaults(error)) << error.toStdString();
    EXPECT_EQ(contentsOf(theme), QByteArray("theme wave.trace #010203\n"));
    EXPECT_EQ(directory.read().theme.file, QStringLiteral("themes/dark.tcl"));

    write(directory.configFile(), "# mine\n");
    ASSERT_TRUE(directory.createDefaults(error)) << error.toStdString();
    EXPECT_EQ(contentsOf(directory.configFile()), QByteArray("# mine\n"));
}

TEST_F(ConfigurationTest, ADirectoryThatCannotBeMadeIsReported) {
    // A file where the directory's parent should be: true whoever runs the test, which a
    // directory without write permission is not.
    write(home.filePath(QStringLiteral("blocked")), "a file, not a directory\n");
    const ConfigurationDirectory directory(home.filePath(QStringLiteral("blocked/enzan")));

    QString error;
    EXPECT_FALSE(directory.createDefaults(error));
    EXPECT_FALSE(error.isEmpty());

    // And the window still has a theme to open with.
    const ConfiguredTheme configured = directory.loadTheme();
    EXPECT_TRUE(configured.theme.isComplete());
    EXPECT_TRUE(configured.problems.isEmpty());
}

TEST_F(ConfigurationTest, NoHomeDirectoryIsNoDirectoryAtAll) {
    const ConfigurationDirectory directory;
    QString error;
    EXPECT_FALSE(directory.createDefaults(error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(directory.read().theme.isDefault());
    EXPECT_TRUE(directory.loadTheme().theme.isComplete());
}

// ---------------------------------------------------------------------------
// What `config` says
// ---------------------------------------------------------------------------

TEST_F(ConfigurationTest, AThemeIsRelativeToTheDirectoryUnlessItIsAbsolute) {
    const ConfigurationDirectory directory(fresh());
    EXPECT_EQ(directory.resolve(QStringLiteral("themes/x.tcl")),
              fresh() + QStringLiteral("/themes/x.tcl"));
    EXPECT_EQ(directory.resolve(QStringLiteral("/elsewhere/x.tcl")),
              QStringLiteral("/elsewhere/x.tcl"));
}

TEST_F(ConfigurationTest, NothingInConfigIsEvaluated) {
    const ParsedConfiguration parsed = parseConfiguration("theme $HOME/[pwd].tcl\n");
    EXPECT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    EXPECT_EQ(parsed.theme.file, QStringLiteral("$HOME/[pwd].tcl"));
}

TEST_F(ConfigurationTest, AnUnknownSettingIsSkippedAndTheRestApplies) {
    // A newer Enzan may write a setting this one has never heard of; the theme must survive it.
    const ParsedConfiguration parsed =
        parseConfiguration("layout tall\ntheme {themes/my theme.tcl}\n");
    EXPECT_TRUE(parsed.errors.isEmpty()) << firstError(parsed);
    ASSERT_EQ(parsed.warnings.size(), 1);
    EXPECT_EQ(parsed.warnings.front().line, 1);
    EXPECT_TRUE(parsed.warnings.front().text.contains(QStringLiteral("layout")));
    EXPECT_EQ(parsed.theme.file, QStringLiteral("themes/my theme.tcl"));
}

TEST_F(ConfigurationTest, AMistakeInAKnownSettingMeansNothingIsUsed) {
    const char* const mistakes[] = {
        "theme\n",       "theme a.tcl b.tcl\n",   "theme {}\n", "theme a.tcl\ntheme b.tcl\n",
        "theme {*}$x\n", "theme {never closed\n",
    };
    for (const char* text : mistakes) {
        const ParsedConfiguration parsed = parseConfiguration(text);
        EXPECT_FALSE(parsed.errors.isEmpty()) << text;
        EXPECT_TRUE(parsed.theme.isDefault())
            << text << " still named " << parsed.theme.file.toStdString();
    }

    const ParsedConfiguration twice = parseConfiguration("theme a.tcl\n\ntheme b.tcl\n");
    ASSERT_EQ(twice.errors.size(), 1);
    EXPECT_EQ(twice.errors.front().line, 3);
    EXPECT_TRUE(twice.errors.front().text.contains(QStringLiteral("line 1")))
        << twice.errors.front().text.toStdString();
}

TEST_F(ConfigurationTest, ABuiltInThemeIsNamedByAnOption) {
    const ParsedConfiguration light = parseConfiguration("\ntheme -builtin light\n");
    EXPECT_TRUE(light.errors.isEmpty()) << firstError(light);
    EXPECT_EQ(light.theme, ThemeName::ofBuiltIn(QStringLiteral("light")));
    EXPECT_FALSE(light.theme.isFile());
    EXPECT_EQ(light.themeLine, 2);

    // The default named outright is the default, as naming none is.
    EXPECT_TRUE(parseConfiguration("theme -builtin dark\n").theme.isDefault());
    EXPECT_TRUE(parseConfiguration("# nothing\n").theme.isDefault());

    // `-builtin` with no name is a mistake rather than a file called `-builtin`.
    EXPECT_FALSE(parseConfiguration("theme -builtin\n").errors.isEmpty());
    EXPECT_FALSE(parseConfiguration("theme -builtin {}\n").errors.isEmpty());
}

TEST_F(ConfigurationTest, TheLightThemeLoadsFromConfig) {
    const ConfigurationDirectory directory(fresh());
    write(directory.configFile(), "theme -builtin light\n");

    const ConfiguredTheme configured = directory.loadTheme();
    EXPECT_TRUE(configured.problems.isEmpty());
    Theme light;
    ASSERT_TRUE(light.loadBuiltIn(QStringLiteral("light")));
    EXPECT_EQ(configured.theme.color(ThemeColor::WaveBackground),
              light.color(ThemeColor::WaveBackground));
}

TEST_F(ConfigurationTest, ABuiltInThemeThisBuildLacksLeavesTheDefaultAndSaysWhere) {
    // A newer Enzan may build in a theme an older one has never heard of; the older one opens with
    // its default and names the line, as it does for a file that is missing.
    const ConfigurationDirectory directory(fresh());
    write(directory.configFile(), "# mine\ntheme -builtin sepia\n");

    const ConfiguredTheme configured = directory.loadTheme();
    Theme byDefault;
    byDefault.loadBuiltIn();
    EXPECT_EQ(configured.theme.color(ThemeColor::WaveTrace),
              byDefault.color(ThemeColor::WaveTrace));
    ASSERT_EQ(configured.problems.size(), 1);
    const QString message = describe(configured.problems.front());
    EXPECT_TRUE(message.startsWith(directory.configFile() + QStringLiteral(":2: ")))
        << message.toStdString();
    EXPECT_TRUE(message.contains(QStringLiteral("dark and light"))) << message.toStdString();
}

TEST_F(ConfigurationTest, ATroubledThemeLeavesTheShippedOneAndSaysWhere) {
    const ConfigurationDirectory directory(fresh());
    write(directory.configFile(), "theme themes/broken.tcl\n");
    write(directory.resolve(QStringLiteral("themes/broken.tcl")),
          "theme wave.trace #010203\ntheme wave.nonsense #040506\n");

    const ConfiguredTheme configured = directory.loadTheme();
    Theme builtIn;
    builtIn.loadBuiltIn();
    EXPECT_EQ(configured.theme.color(ThemeColor::WaveTrace), builtIn.color(ThemeColor::WaveTrace));
    ASSERT_EQ(configured.problems.size(), 1);
    EXPECT_TRUE(configured.problems.front().isError);
    EXPECT_TRUE(describe(configured.problems.front()).contains(QStringLiteral("broken.tcl:2: ")))
        << describe(configured.problems.front()).toStdString();
}

TEST_F(ConfigurationTest, AMistakeInConfigIsReportedAgainstItsLine) {
    const ConfigurationDirectory directory(fresh());
    write(directory.configFile(), "# fine\ntheme a.tcl b.tcl\n");

    const ConfiguredTheme configured = directory.loadTheme();
    EXPECT_TRUE(configured.theme.isComplete());
    ASSERT_EQ(configured.problems.size(), 1);
    EXPECT_EQ(describe(configured.problems.front()).left(directory.configFile().size() + 3),
              directory.configFile() + QStringLiteral(":2:"));
}

TEST_F(ConfigurationTest, AMissingThemeFileIsReported) {
    const ConfigurationDirectory directory(fresh());
    write(directory.configFile(), "theme themes/gone.tcl\n");

    const ConfiguredTheme configured = directory.loadTheme();
    EXPECT_TRUE(configured.theme.isComplete());
    ASSERT_EQ(configured.problems.size(), 1);
    EXPECT_TRUE(describe(configured.problems.front()).contains(QStringLiteral("gone.tcl")));
}

// ---------------------------------------------------------------------------
// What the Theme menu does to the directory
// ---------------------------------------------------------------------------

TEST_F(ConfigurationTest, TheThemesAreTheTclFilesInThemes) {
    const ConfigurationDirectory directory(fresh());
    write(directory.resolve(QStringLiteral("themes/b.tcl")), "\n");
    write(directory.resolve(QStringLiteral("themes/a.tcl")), "\n");
    write(directory.resolve(QStringLiteral("themes/notes.txt")), "\n");
    write(directory.resolve(QStringLiteral("themes/old/c.tcl")), "\n");

    EXPECT_EQ(directory.themes(),
              (QStringList{QStringLiteral("themes/a.tcl"), QStringLiteral("themes/b.tcl")}));
    EXPECT_TRUE(ConfigurationDirectory().themes().isEmpty());
}

TEST_F(ConfigurationTest, NamingAThemeKeepsTheRestOfConfig) {
    const ConfigurationDirectory directory(fresh());
    write(directory.configFile(), "# mine\ntheme themes/dark.tcl\nlayout tall\n");

    QString error;
    ASSERT_TRUE(directory.setTheme(ThemeName::ofFile(QStringLiteral("themes/light.tcl")), error))
        << error.toStdString();
    EXPECT_EQ(contentsOf(directory.configFile()),
              QByteArray("# mine\ntheme themes/light.tcl\nlayout tall\n"));
}

TEST_F(ConfigurationTest, AConfigThatHasGoneIsBegunAgain) {
    const ConfigurationDirectory directory(fresh());
    QString error;
    ASSERT_TRUE(directory.setTheme(ThemeName::ofFile(QStringLiteral("themes/light.tcl")), error))
        << error.toStdString();
    EXPECT_EQ(directory.read().theme.file, QStringLiteral("themes/light.tcl"));
    EXPECT_TRUE(contentsOf(directory.configFile()).startsWith("# Enzan's settings"));
}

TEST_F(ConfigurationTest, AConfigWithAMistakeIsNotRewritten) {
    const ConfigurationDirectory directory(fresh());
    write(directory.configFile(), "theme a.tcl b.tcl\n");

    QString error;
    EXPECT_FALSE(directory.setTheme(ThemeName::ofFile(QStringLiteral("themes/light.tcl")), error));
    EXPECT_TRUE(error.contains(directory.configFile() + QStringLiteral(":1:")))
        << error.toStdString();
    EXPECT_EQ(contentsOf(directory.configFile()), QByteArray("theme a.tcl b.tcl\n"));
}

TEST_F(ConfigurationTest, ANewThemeIsTheShippedOneUnderAName) {
    const ConfigurationDirectory directory(fresh());
    QString error;

    const std::optional<QString> written =
        directory.writeTheme(QStringLiteral("mine"), ThemeName(), error);
    ASSERT_TRUE(written.has_value()) << error.toStdString();
    const QString mine = written.value_or(QString());
    EXPECT_EQ(mine, QStringLiteral("themes/mine.tcl"));
    EXPECT_EQ(contentsOf(directory.resolve(mine)), Theme::builtInScript().toUtf8());

    // The extension may be given or not; a name already taken is never written over.
    write(directory.resolve(mine), "theme accent #123456\n");
    EXPECT_FALSE(directory.writeTheme(QStringLiteral("mine.tcl"), ThemeName(), error).has_value());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(contentsOf(directory.resolve(mine)), QByteArray("theme accent #123456\n"));
}

TEST_F(ConfigurationTest, ANewThemeIsACopyOfTheThemeItIsMadeFrom) {
    const ConfigurationDirectory directory(fresh());
    QString error;

    const std::optional<QString> light = directory.writeTheme(
        QStringLiteral("pale"), ThemeName::ofBuiltIn(QStringLiteral("light")), error);
    ASSERT_TRUE(light.has_value()) << error.toStdString();
    EXPECT_EQ(contentsOf(directory.resolve(light.value_or(QString()))),
              Theme::builtInScript(QStringLiteral("light")).toUtf8());

    write(directory.resolve(QStringLiteral("themes/edited.tcl")), "theme accent #123456\n");
    const std::optional<QString> copy = directory.writeTheme(
        QStringLiteral("again"), ThemeName::ofFile(QStringLiteral("themes/edited.tcl")), error);
    ASSERT_TRUE(copy.has_value()) << error.toStdString();
    EXPECT_EQ(contentsOf(directory.resolve(copy.value_or(QString()))),
              QByteArray("theme accent #123456\n"));

    EXPECT_FALSE(directory
                     .writeTheme(QStringLiteral("none"),
                                 ThemeName::ofBuiltIn(QStringLiteral("sepia")), error)
                     .has_value());
    EXPECT_FALSE(QFileInfo::exists(directory.resolve(QStringLiteral("themes/none.tcl"))));
}

TEST_F(ConfigurationTest, ANewThemeNeedsANameForAFileInThemes) {
    const ConfigurationDirectory directory(fresh());
    for (const QString& name : {QString(), QStringLiteral("  "), QStringLiteral("../escape"),
                                QStringLiteral("sub/dir"), QStringLiteral(".hidden")}) {
        QString error;
        EXPECT_FALSE(directory.writeTheme(name, ThemeName(), error).has_value())
            << name.toStdString();
        EXPECT_FALSE(error.isEmpty()) << name.toStdString();
    }
}

// ---------------------------------------------------------------------------
// Rewriting `config`
// ---------------------------------------------------------------------------

TEST_F(ConfigurationTest, ChoosingAThemeRewritesOnlyItsCommand) {
    // The reader's comments and settings this build does not know stay exactly as written.
    const QByteArray before("# my settings\nlayout tall\n  theme themes/dark.tcl  ;# the usual\n");
    const std::optional<QByteArray> after =
        configurationWithTheme(before, ThemeName::ofFile(QStringLiteral("themes/light.tcl")));
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after.value_or(QByteArray()),
              QByteArray("# my settings\nlayout tall\n  theme themes/light.tcl  ;# the usual\n"));
}

TEST_F(ConfigurationTest, ChoosingABuiltInThemeWritesItsOption) {
    EXPECT_EQ(configurationWithTheme("# c\ntheme themes/dark.tcl\n",
                                     ThemeName::ofBuiltIn(QStringLiteral("light"))),
              QByteArray("# c\ntheme -builtin light\n"));
    EXPECT_EQ(configurationWithTheme("# c\n", ThemeName::ofBuiltIn(QStringLiteral("light"))),
              QByteArray("# c\ntheme -builtin light\n"));
    EXPECT_EQ(configurationWithTheme("theme -builtin light\n", ThemeName()), QByteArray());
}

TEST_F(ConfigurationTest, ChoosingTheSameThemeWritesTheSameBytes) {
    const QByteArray before("theme {themes/my theme.tcl}\n# after\n");
    EXPECT_EQ(
        configurationWithTheme(before, ThemeName::ofFile(QStringLiteral("themes/my theme.tcl"))),
        before);
}

TEST_F(ConfigurationTest, AThemeIsAddedWhereThereWasNone) {
    EXPECT_EQ(configurationWithTheme("# nothing yet",
                                     ThemeName::ofFile(QStringLiteral("themes/a b.tcl"))),
              QByteArray("# nothing yet\ntheme {themes/a b.tcl}\n"));
    EXPECT_EQ(configurationWithTheme("", ThemeName::ofFile(QStringLiteral("themes/a.tcl"))),
              QByteArray("theme themes/a.tcl\n"));
}

TEST_F(ConfigurationTest, TheDefaultThemeIsChosenByLeavingTheCommandOut) {
    EXPECT_EQ(configurationWithTheme("# keep\ntheme themes/dark.tcl\n# this too\n", ThemeName()),
              QByteArray("# keep\n# this too\n"));
    EXPECT_EQ(configurationWithTheme("theme themes/dark.tcl", ThemeName()), QByteArray());

    // Something else on the command's line keeps the line.
    EXPECT_EQ(configurationWithTheme("theme themes/dark.tcl ;# old\n", ThemeName()),
              QByteArray(" ;# old\n"));
}

TEST_F(ConfigurationTest, AFileWithAMistakeIsNotRewritten) {
    // Its commands could not all be read, so where its theme is would be a guess.
    EXPECT_FALSE(configurationWithTheme("theme {never closed\n",
                                        ThemeName::ofFile(QStringLiteral("a.tcl"))));
    EXPECT_FALSE(
        configurationWithTheme("theme a.tcl b.tcl\n", ThemeName::ofFile(QStringLiteral("a.tcl"))));
}

TEST_F(ConfigurationTest, AnyThemeNameReadsBackAsItWasWritten) {
    for (const QString& theme :
         {QStringLiteral("themes/plain.tcl"), QStringLiteral("themes/two words.tcl"),
          QStringLiteral("themes/brace{.tcl"), QStringLiteral("/abs/$dollar.tcl"),
          QStringLiteral("#hash.tcl"), QStringLiteral("themes/ünïcode.tcl")}) {
        const std::optional<QByteArray> written =
            configurationWithTheme("# c\n", ThemeName::ofFile(theme));
        ASSERT_TRUE(written.has_value()) << theme.toStdString();
        const QByteArray text = written.value_or(QByteArray());
        const ParsedConfiguration parsed = parseConfiguration(text);
        EXPECT_TRUE(parsed.errors.isEmpty()) << text.toStdString() << ": " << firstError(parsed);
        EXPECT_EQ(parsed.theme.file, theme) << "written as " << text.toStdString();
    }
}

} // namespace
