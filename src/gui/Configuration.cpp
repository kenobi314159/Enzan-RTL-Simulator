// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/Configuration.h"

#include "common/Assert.h"
#include "gui/TclSyntax.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <string_view>
#include <utility>

namespace enzan::gui {

namespace {

/// What `config` starts as, before the theme command is added to it.
///
/// For the reader who opens the file rather than the window: it says what the file is, what its
/// syntax is, and what leaving the theme out does.
constexpr std::string_view DefaultConfiguration =
    R"(# Enzan's settings for its window, read each time a window opens. The window made this file
# the first time it opened.
#
# One command a line. The syntax is Tcl's, but nothing here is evaluated: `#` starts a comment,
# and braces hold a word with spaces in it, as in `theme {themes/my theme.tcl}`.
#
# The theme is a Tcl script, relative to this directory or as a full path. Without a `theme`
# line the window uses the theme built into Enzan. The window's View > Theme menu changes it here,
# keeping the rest of this file as it is.
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
    int themeLine = 0;

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

            if (command.words.size() != 2) {
                error(command.line, QStringLiteral("`theme` names one file: `theme <file>`"));
                return true;
            }
            if (themeLine != 0) {
                error(command.line,
                      QStringLiteral("the theme is named twice, here and at line %1; it takes one")
                          .arg(themeLine));
                return true;
            }
            const TclWord& file = command.words[1];
            if (file.text.isEmpty()) {
                error(command.line, QStringLiteral("`theme` names no file; leave the line out for "
                                                   "the theme built into Enzan"));
                return true;
            }

            themeLine = command.line;
            parsed.theme = QString::fromUtf8(file.text);
            const TclWord& first = command.words.front();
            span = ThemeCommandSpan{.start = first.start - text.constData(),
                                    .end = file.start + file.size - text.constData()};
            return true;
        },
        malformed);

    if (malformed) error(malformed->line, std::move(malformed->text));
    if (!parsed.errors.isEmpty()) parsed.theme.clear();
    return span;
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

std::optional<QByteArray> configurationWithTheme(const QByteArray& text, const QString& theme) {
    ParsedConfiguration parsed;
    const std::optional<ThemeCommandSpan> span = readCommands(text, parsed);
    if (!parsed.errors.isEmpty()) return std::nullopt;

    QByteArray result = text;

    if (!theme.isEmpty()) {
        const QByteArray command = (QStringLiteral("theme ") + quoteTclWord(theme)).toUtf8();
        if (span) return result.replace(span->start, span->end - span->start, command);

        if (!result.isEmpty() && !result.endsWith('\n')) result += '\n';
        return result + command + '\n';
    }

    if (!span) return result;

    // The command goes with its line when nothing else is on it, so that choosing the shipped
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

bool ConfigurationDirectory::createDefaults(QString& error) const {
    if (root.isEmpty()) {
        error = QStringLiteral("there is no home directory to keep settings in");
        return false;
    }
    if (QFileInfo::exists(configFile())) return true;

    const QString theme = resolve(QString::fromLatin1(DefaultTheme));
    if (!QDir().mkpath(QFileInfo(theme).path())) {
        error = QStringLiteral("could not make '%1'").arg(QFileInfo(theme).path());
        return false;
    }
    if (!QFileInfo::exists(theme) && !writeWhole(theme, Theme::builtInScript().toUtf8(), error))
        return false;

    const QByteArray header(DefaultConfiguration.data(),
                            static_cast<qsizetype>(DefaultConfiguration.size()));
    const std::optional<QByteArray> contents =
        configurationWithTheme(header, QString::fromLatin1(DefaultTheme));

    // The text is this file's own, so a header that did not read back is a broken build rather
    // than a misconfigured one.
    ENZAN_ASSERT(contents.has_value(), "the default configuration does not read back");
    return writeWhole(configFile(), contents.value_or(QByteArray()), error);
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

bool ConfigurationDirectory::setTheme(const QString& theme, QString& error) const {
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

    const QString theme =
        QStringLiteral("%1/%2.tcl").arg(QString::fromLatin1(ThemesDirectory), base);
    const QString path = resolve(theme);
    if (QFileInfo::exists(path)) {
        error = QStringLiteral("'%1' exists already, and is not replaced").arg(path);
        return std::nullopt;
    }
    if (!QDir().mkpath(QFileInfo(path).path())) {
        error = QStringLiteral("could not make '%1'").arg(QFileInfo(path).path());
        return std::nullopt;
    }
    if (!writeWhole(path, Theme::builtInScript().toUtf8(), error)) return std::nullopt;
    return theme;
}

ConfiguredTheme ConfigurationDirectory::loadTheme() const {
    ConfiguredTheme result;
    result.theme.loadBuiltIn();

    const ParsedConfiguration parsed = read();
    result.problems = parsed.warnings;
    result.problems += parsed.errors;
    if (parsed.theme.isEmpty()) return result;

    QString error;
    if (!result.theme.loadFile(resolve(parsed.theme), error)) {
        result.problems.push_back(ConfigurationMessage{.text = error, .isError = true});
    }
    return result;
}

} // namespace enzan::gui
