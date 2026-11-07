// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/Configuration.h"

#include "gui/TclSyntax.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#include <string_view>
#include <utility>

namespace enzan::gui {

namespace {

/// What `config` starts as: no theme command, which is the built-in dark theme.
///
/// For the reader who opens the file rather than the window: it says what the file is, what its
/// syntax is, what leaving the theme out does, and where a theme of their own comes from.
constexpr std::string_view DefaultConfiguration =
    R"(# Enzan's settings for its window, read each time a window opens. The window made this file
# the first time it opened.
#
# One command a line. The syntax is Tcl's, but nothing here is evaluated: `#` starts a comment,
# and braces hold a word with spaces in it, as in `theme {themes/my theme.tcl}`.
#
# The theme is a Tcl script, relative to this directory or as a full path, or one of the themes
# built into Enzan, as in `theme -builtin light`. Without a `theme` line the window uses the
# built-in dark theme. The window's View > Theme menu changes it here, keeping the rest of this
# file as it is, and its New Theme writes a copy of the theme in use into themes/ to edit.
)";

/// Where the themes the window lists and writes are, relative to the directory.
constexpr const char* ThemesDirectory = "themes";

/// Where `config`'s theme command lies in its text, from its first word to the end of its last.
struct ThemeCommandSpan {
    qsizetype start = 0;
    qsizetype end = 0;
};

/// Reads `text`'s commands, reporting into `parsed`, and finds the theme command's span.
std::optional<ThemeCommandSpan> readCommands(const QByteArray& text, ParsedConfiguration& parsed) {
    std::optional<ThemeCommandSpan> span;

    const auto error = [&parsed](int line, QString message) {
        parsed.errors.push_back(
            ConfigurationMessage{.line = line, .text = std::move(message), .isError = true});
    };

    const TclOrigin origin{.base = text.constData(), .baseLine = 1, .exact = true};
    std::optional<TclSyntaxError> malformed;
    splitTclCommands(
        text.constData(), static_cast<int>(text.size()), origin,
        [&](const TclCommand& command) {
            for (const TclWord& word : command.words) {
                if (word.expansion) {
                    error(command.line, QStringLiteral("`{*}` here would expand a variable or a "
                                                       "command's result, and nothing in this "
                                                       "file is evaluated"));
                    return true;
                }
            }

            const QString name = QString::fromUtf8(command.words.front().text);
            if (name != QStringLiteral("theme")) {
                parsed.warnings.push_back(ConfigurationMessage{
                    .line = command.line,
                    .text = QStringLiteral("'%1' is not a setting this build knows, and is skipped")
                                .arg(name)});
                return true;
            }

            // A theme of Enzan's own is named by an option, so that no file, whatever it is
            // called, can be taken for one.
            const bool builtIn =
                command.words.size() >= 2 && command.words[1].text == QByteArray("-builtin");
            if (command.words.size() != (builtIn ? 3U : 2U)) {
                error(command.line, QStringLiteral("`theme` names one theme: `theme <file>`, or "
                                                   "`theme -builtin <name>` for one built into "
                                                   "Enzan"));
                return true;
            }
            if (parsed.themeLine != 0) {
                error(command.line,
                      QStringLiteral("the theme is named twice, here and at line %1; it takes one")
                          .arg(parsed.themeLine));
                return true;
            }
            const TclWord& last = command.words.back();
            if (last.text.isEmpty()) {
                error(command.line, QStringLiteral("`theme` names no theme; leave the line out for "
                                                   "the built-in dark theme"));
                return true;
            }

            parsed.themeLine = command.line;
            const QString word = QString::fromUtf8(last.text);
            parsed.theme = builtIn ? ThemeName::ofBuiltIn(word) : ThemeName::ofFile(word);
            const TclWord& first = command.words.front();
            span = ThemeCommandSpan{.start = first.start - text.constData(),
                                    .end = last.start + last.size - text.constData()};
            return true;
        },
        malformed);

    if (malformed) error(malformed->line, std::move(malformed->text));
    if (!parsed.errors.isEmpty()) {
        parsed.theme = ThemeName();
        parsed.themeLine = 0;
    }
    return span;
}

/// Says that no theme is built in as `name`, naming those that are.
QString notBuiltIn(const QString& name) {
    QStringList names = Theme::builtInNames();
    const QString last = names.takeLast();
    const QString listed =
        names.isEmpty() ? last : QStringLiteral("%1 and %2").arg(names.join(u", "), last);
    return QStringLiteral("no theme is built into Enzan as '%1'; the built-in themes are %2")
        .arg(name, listed);
}

/// Writes `contents` to `path` whole, so that a write cut short leaves the old file rather than
/// half of the new one. Returns false having filled `error`.
bool writeWhole(const QString& path, const QByteArray& contents, QString& error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()
        || !file.commit()) {
        error = QStringLiteral("could not write '%1': %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace

ThemeName ThemeName::ofFile(const QString& path) {
    return ThemeName{.file = path, .builtIn = {}};
}

ThemeName ThemeName::ofBuiltIn(const QString& name) {
    return ThemeName{.file = {}, .builtIn = name};
}

bool ThemeName::isDefault() const {
    return file.isEmpty() && builtIn == QLatin1StringView(Theme::DefaultBuiltIn);
}

QString describe(const ConfigurationMessage& message) {
    if (message.file.isEmpty()) return message.text;
    if (message.line > 0) {
        return QStringLiteral("%1:%2: %3")
            .arg(message.file, QString::number(message.line), message.text);
    }
    return QStringLiteral("%1: %2").arg(message.file, message.text);
}

ParsedConfiguration parseConfiguration(const QByteArray& text) {
    ParsedConfiguration parsed;
    readCommands(text, parsed);
    return parsed;
}

std::optional<QByteArray> configurationWithTheme(const QByteArray& text, const ThemeName& theme) {
    ParsedConfiguration parsed;
    const std::optional<ThemeCommandSpan> span = readCommands(text, parsed);
    if (!parsed.errors.isEmpty()) return std::nullopt;

    QByteArray result = text;

    if (!theme.isDefault()) {
        const QByteArray command =
            (theme.isFile() ? QStringLiteral("theme %1").arg(quoteTclWord(theme.file))
                            : QStringLiteral("theme -builtin %1").arg(quoteTclWord(theme.builtIn)))
                .toUtf8();
        if (span) return result.replace(span->start, span->end - span->start, command);

        if (!result.isEmpty() && !result.endsWith('\n')) result += '\n';
        return result + command + '\n';
    }

    if (!span) return result;

    // The command goes with its line when nothing else is on it, so that choosing the default
    // theme leaves no blank line behind; otherwise only the command goes.
    qsizetype start = span->start;
    qsizetype end = span->end;
    const qsizetype lineStart = start == 0 ? 0 : result.lastIndexOf('\n', start - 1) + 1;
    qsizetype lineEnd = result.indexOf('\n', end);
    if (lineEnd < 0) lineEnd = result.size();

    const auto blank = [&result](qsizetype from, qsizetype to) {
        for (qsizetype index = from; index < to; ++index) {
            if (result[index] != ' ' && result[index] != '\t') return false;
        }
        return true;
    };
    if (blank(lineStart, start) && blank(end, lineEnd)) {
        start = lineStart;
        end = lineEnd < result.size() ? lineEnd + 1 : lineEnd;
    }
    return result.remove(start, end - start);
}

QString ConfigurationDirectory::standardPath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) return {};
    return base + QStringLiteral("/enzan");
}

ConfigurationDirectory::ConfigurationDirectory(QString path) : root(std::move(path)) {}

QString ConfigurationDirectory::configFile() const {
    return root.isEmpty() ? QString() : root + QStringLiteral("/config");
}

QString ConfigurationDirectory::layoutFile() const {
    return root.isEmpty() ? QString() : root + QStringLiteral("/layout.ini");
}

bool ConfigurationDirectory::createDefaults(QString& error) const {
    if (root.isEmpty()) {
        error = QStringLiteral("there is no home directory to keep settings in");
        return false;
    }
    if (QFileInfo::exists(configFile())) return true;

    if (!QDir().mkpath(root)) {
        error = QStringLiteral("could not make '%1'").arg(root);
        return false;
    }

    // No theme is written out beside it. The built-in one is in use, and a copy of it would be a
    // second dark theme in the menu, keeping the colours of the release that wrote it; New Theme
    // writes one when the reader asks for a theme to edit.
    return writeWhole(configFile(),
                      QByteArray(DefaultConfiguration.data(),
                                 static_cast<qsizetype>(DefaultConfiguration.size())),
                      error);
}

ParsedConfiguration ConfigurationDirectory::read() const {
    ParsedConfiguration parsed;
    if (root.isEmpty()) return parsed;

    QFile file(configFile());
    if (!file.exists()) return parsed;
    if (!file.open(QIODevice::ReadOnly)) {
        parsed.errors.push_back(ConfigurationMessage{
            .file = configFile(),
            .text = QStringLiteral("could not be read: %1").arg(file.errorString()),
            .isError = true});
        return parsed;
    }

    parsed = parseConfiguration(file.readAll());
    for (ConfigurationMessage& message : parsed.warnings) message.file = configFile();
    for (ConfigurationMessage& message : parsed.errors) message.file = configFile();
    return parsed;
}

QString ConfigurationDirectory::resolve(const QString& theme) const {
    if (QFileInfo(theme).isAbsolute()) return theme;
    return QDir(root).filePath(theme);
}

bool ConfigurationDirectory::load(const ThemeName& name, Theme& theme, QString& error) const {
    // Each changes nothing when it fails.
    if (name.isFile()) return theme.loadFile(resolve(name.file), error);
    if (theme.loadBuiltIn(name.builtIn)) return true;
    error = notBuiltIn(name.builtIn);
    return false;
}

QStringList ConfigurationDirectory::themes() const {
    QStringList names;
    if (root.isEmpty()) return names;

    const QDir themes(resolve(QString::fromLatin1(ThemesDirectory)));
    for (const QString& file :
         themes.entryList({QStringLiteral("*.tcl")}, QDir::Files | QDir::Readable, QDir::Name)) {
        names << QStringLiteral("%1/%2").arg(QString::fromLatin1(ThemesDirectory), file);
    }
    return names;
}

bool ConfigurationDirectory::setTheme(const ThemeName& theme, QString& error) const {
    if (root.isEmpty()) {
        error = QStringLiteral("there is no home directory to keep settings in");
        return false;
    }

    // A `config` that has gone missing since the window opened is begun again as the first start
    // begins it, so that it still explains itself.
    QByteArray text(DefaultConfiguration.data(),
                    static_cast<qsizetype>(DefaultConfiguration.size()));
    QFile file(configFile());
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("could not read '%1': %2").arg(configFile(), file.errorString());
            return false;
        }
        text = file.readAll();
        file.close();
    }

    const std::optional<QByteArray> rewritten = configurationWithTheme(text, theme);
    if (!rewritten) {
        const ParsedConfiguration parsed = parseConfiguration(text);
        ConfigurationMessage first = parsed.errors.value(0);
        first.file = configFile();
        error = QStringLiteral("it is not rewritten while it has a mistake in it: %1")
                    .arg(describe(first));
        return false;
    }
    if (*rewritten == text && file.exists()) return true;

    if (!QDir().mkpath(root)) {
        error = QStringLiteral("could not make '%1'").arg(root);
        return false;
    }
    return writeWhole(configFile(), *rewritten, error);
}

std::optional<QString> ConfigurationDirectory::writeTheme(const QString& name,
                                                          const ThemeName& from,
                                                          QString& error) const {
    if (root.isEmpty()) {
        error = QStringLiteral("there is no home directory to keep settings in");
        return std::nullopt;
    }

    QString base = name.trimmed();
    if (base.endsWith(QStringLiteral(".tcl"))) base.chop(4);
    if (base.isEmpty() || base.startsWith(u'.') || base.contains(u'/')) {
        error = QStringLiteral("'%1' is not a name for a theme; it names a file in %2/, without "
                               "a '/' in it")
                    .arg(name, resolve(QString::fromLatin1(ThemesDirectory)));
        return std::nullopt;
    }

    QString theme = QStringLiteral("%1/%2.tcl").arg(QString::fromLatin1(ThemesDirectory), base);
    const QString path = resolve(theme);
    if (QFileInfo::exists(path)) {
        error = QStringLiteral("'%1' exists already, and is not replaced").arg(path);
        return std::nullopt;
    }
    if (!QDir().mkpath(QFileInfo(path).path())) {
        error = QStringLiteral("could not make '%1'").arg(QFileInfo(path).path());
        return std::nullopt;
    }

    QByteArray contents;
    if (from.isFile()) {
        QFile source(resolve(from.file));
        if (!source.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("could not read '%1': %2")
                        .arg(resolve(from.file), source.errorString());
            return std::nullopt;
        }
        contents = source.readAll();
    } else {
        contents = Theme::builtInScript(from.builtIn).toUtf8();
        if (contents.isEmpty()) {
            error = notBuiltIn(from.builtIn);
            return std::nullopt;
        }
    }

    if (!writeWhole(path, contents, error)) return std::nullopt;
    return theme;
}

ConfiguredTheme ConfigurationDirectory::loadTheme() const {
    ConfiguredTheme result;
    result.theme.loadBuiltIn();

    const ParsedConfiguration parsed = read();
    result.problems = parsed.warnings;
    result.problems += parsed.errors;
    if (parsed.theme.isDefault()) return result;

    // A file's error names the file and its line already; a built-in theme's is against the line
    // of `config` that named it.
    QString error;
    if (!load(parsed.theme, result.theme, error)) {
        result.problems.push_back(
            ConfigurationMessage{.file = parsed.theme.isFile() ? QString() : configFile(),
                                 .line = parsed.theme.isFile() ? 0 : parsed.themeLine,
                                 .text = error,
                                 .isError = true});
    }
    return result;
}

} // namespace enzan::gui
