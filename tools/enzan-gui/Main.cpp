// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point for the graphical front end.
//
// The GUI is a separate process from the kernel (DESIGN_GUIDE.md section 2.2) and talks to it
// over a socket. Nothing here links the simulator: the two are built apart, and a change that
// made this executable depend on `runtime` or on a frontend would quietly undo the separation
// that lets a local window drive a kernel on a compute server.

#include "common/Version.h"
#include "gui/KernelSession.h"
#include "gui/MainWindow.h"
#include "gui/Theme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

#include <cstdlib>
#include <optional>
#include <string_view>

namespace {

/// Where a user's theme is looked for when none is named on the command line.
///
/// One well-known place rather than a search path: a colour that came from a file the user
/// cannot immediately point at is worse than no colour at all.
QString defaultThemePath() {
    const QString configuration =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configuration.isEmpty()) return {};
    return configuration + QStringLiteral("/theme.tcl");
}

/// What `--help` prints.
///
/// Short because this is not a program a user drives. `enzan --gui` starts it and passes it a
/// socket; the options here are the ones that reach it that way (DESIGN_GUIDE.md section 8.4).
/// Written out rather than generated for the reason 8.5 gives: a generated text does not match
/// the one beside it.
constexpr std::string_view UsageText =
    R"(Usage: enzan-gui --attach <socket>

Enzan's graphical front end. This is what `enzan --gui` starts, not a program to run directly:
the kernel analyses, elaborates, binds a socket and hands it here. Reach it through
`enzan --gui` and its options rather than through this.

Options:
  -h, --help              Print this message and exit.
  -V, --version           Print version and build information and exit.
      --attach <socket>   Attach to a kernel listening on this Unix domain socket.
      --theme <file>      Load colours from this Tcl theme script. The default is
                          $XDG_CONFIG_HOME/enzan-gui/theme.tcl when it exists.
      --write-theme <file>
                          Write the built-in theme to this file and exit.
      --wave-list <file>  Open the wave window with this signal list in it.

The normative specification is DESIGN_GUIDE.md in the source tree.
)";

/// Answers `--help` and `--version` before Qt's parser sees the command line.
///
/// Ahead of `QCommandLineParser` because this calls neither `addHelpOption` nor
/// `addVersionOption`: those generate the divergent help text and bind `-v`, where the rest of
/// the tool spells it `-V` (8.5).
std::optional<int> answerHelpOrVersion(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);

        if (argument == "-h" || argument == "--help") {
            QTextStream(stdout) << QString::fromUtf8(UsageText.data(),
                                                     static_cast<qsizetype>(UsageText.size()));
            return EXIT_SUCCESS;
        }
        if (argument == "-V" || argument == "--version") {
            QTextStream(stdout) << QString::fromStdString(enzan::versionNotice("enzan-gui"));
            return EXIT_SUCCESS;
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    // Before the QApplication, so that `--help` and `--version` answer with no display attached
    // and without Qt reporting that it could not connect to one.
    if (const std::optional<int> status = answerHelpOrVersion(argc, argv)) return *status;

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("enzan-gui"));
    QCoreApplication::setOrganizationName(QStringLiteral("Enzan"));
    QCoreApplication::setApplicationVersion(QString::fromStdString(enzan::versionLine()));

    // Neither `addHelpOption` nor `addVersionOption`: both are answered above, from one text
    // that matches the kernel's. Adding them here would put `-v` and `--help-all` back.
    QCommandLineParser parser;

    const QCommandLineOption attachOption(
        QStringLiteral("attach"),
        QStringLiteral("Attach to a kernel listening on this Unix domain socket."),
        QStringLiteral("socket"));
    const QCommandLineOption themeOption(QStringLiteral("theme"),
                                         QStringLiteral("Load colours from this Tcl theme script."),
                                         QStringLiteral("file"));
    const QCommandLineOption writeThemeOption(
        QStringLiteral("write-theme"),
        QStringLiteral("Write the built-in theme to this file and exit, as a starting point "
                       "for one of your own."),
        QStringLiteral("file"));
    const QCommandLineOption waveListOption(
        QStringLiteral("wave-list"),
        QStringLiteral("Open the wave window with this signal list in it."),
        QStringLiteral("file"));
    parser.addOption(attachOption);
    parser.addOption(themeOption);
    parser.addOption(writeThemeOption);
    parser.addOption(waveListOption);
    // `enzan run --gui ...` forwards its command word, which this process has no use for: it
    // starts a kernel, and the kernel is the thing that runs. Dropped rather than refused, so
    // that both spellings of the same request work (DESIGN_GUIDE.md section 8.4).
    QStringList forwarded;
    for (const QString& argument : QCoreApplication::arguments()) {
        if (argument == QStringLiteral("run") || argument == QStringLiteral("elaborate")) continue;
        forwarded << argument;
    }
    parser.process(forwarded);

    // --- the theme ---------------------------------------------------------

    enzan::gui::Theme theme;
    theme.loadBuiltIn();

    if (parser.isSet(writeThemeOption)) {
        const QString path = parser.value(writeThemeOption);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(stderr) << "enzan-gui: could not write '" << path
                                << "': " << file.errorString() << '\n';
            return EXIT_FAILURE;
        }
        file.write(enzan::gui::Theme::builtInScript().toUtf8());
        QTextStream(stdout) << "wrote the built-in theme to " << path << '\n';
        return EXIT_SUCCESS;
    }

    QString themePath = parser.value(themeOption);
    if (themePath.isEmpty()) {
        const QString fallback = defaultThemePath();
        if (!fallback.isEmpty() && QFile::exists(fallback)) themePath = fallback;
    }

    QString themeError;
    if (!themePath.isEmpty() && !theme.loadFile(themePath, themeError)) {
        // Reported and carried on from, not fatal: a mistyped colour should not stop someone
        // debugging, and the built-in theme is already loaded underneath.
        QTextStream(stderr) << "enzan-gui: " << themePath << ": " << themeError << '\n';
    }

    QApplication::setStyle(QStringLiteral("Fusion"));
    // The palette carries nearly all of the theme, and the style sheet only what a palette
    // cannot say. Keeping backgrounds out of the style sheet is what lets the signal list and
    // the wave canvas move what they have already drawn when they scroll, rather than drawing
    // it all again (DESIGN_GUIDE.md section 10.5).
    QApplication::setPalette(theme.palette());
    application.setStyleSheet(theme.styleSheet());

    // --- the session -------------------------------------------------------

    enzan::gui::KernelSession session;
    enzan::gui::MainWindow window(session, theme);
    window.show();

    if (!themeError.isEmpty()) {
        window.note(QStringLiteral("%1: %2").arg(themePath, themeError));
    }

    QString error;
    if (parser.isSet(attachOption)) {
        if (!session.attach(parser.value(attachOption), error)) {
            window.note(QStringLiteral("Could not attach to %1: %2")
                            .arg(parser.value(attachOption), error));
        }
    }

    // After attaching, though the rows wait for the design to arrive: the file is read and
    // checked now, so that a mistake in it is reported as the window opens. One that cannot be
    // loaded leaves the window empty, as a theme that cannot be read leaves the built-in
    // colours (DESIGN_GUIDE.md section 10.8).
    if (parser.isSet(waveListOption) && !window.loadSignalList(parser.value(waveListOption))) {
        QTextStream(stderr) << "enzan-gui: " << parser.value(waveListOption)
                            << ": not loaded; the window's transcript says why\n";
    }

    if (!parser.isSet(attachOption)) {
        // No socket. The window comes up and explains itself rather than exiting with a usage
        // message at someone who reached this binary without meaning to.
        window.note(QStringLiteral(
            "Nothing to show. This is the front end `enzan --gui` starts; run that instead, or "
            "pass --attach <socket> to reach a kernel already listening."));
    }

    return QApplication::exec();
}
