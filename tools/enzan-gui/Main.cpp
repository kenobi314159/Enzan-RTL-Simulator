// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point for the graphical front end.
//
// The GUI is a separate process from the kernel and talks to it over a socket. Nothing here
// links the simulator: the two are built apart, and a change that made this executable depend
// on `runtime` or on a frontend would quietly undo the separation that lets a local window
// drive a kernel on a compute server.

#include "common/Version.h"
#include "gui/Configuration.h"
#include "gui/KernelSession.h"
#include "gui/MainWindow.h"
#include "gui/Theme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QStringList>
#include <QTextStream>

#include <cstdlib>
#include <optional>
#include <string_view>

namespace {

/// What `--help` prints.
///
/// Short because this is not a program a user drives. `enzan --gui` starts it and passes it a
/// socket; the options here are the ones that reach it that way. Written out rather than
/// generated, because a generated text does not match the one beside it.
constexpr std::string_view UsageText =
    R"(Usage: enzan-gui --attach <socket>

Enzan's graphical front end. This is what `enzan --gui` starts, not a program to run directly:
the kernel analyses, elaborates, binds a socket and hands it here. Reach it through
`enzan --gui` and its options rather than through this.

Options:
  -h, --help              Print this message and exit.
  -V, --version           Print version and build information and exit.
      --attach <socket>   Attach to a kernel listening on this Unix domain socket.
      --wave-list <file>  Open the wave window with this signal list in it.
  -r, --run <time>        Run the design for this long once attached: a count of
                          the run's time unit, 100, or a time with its unit, 10ns
                          or 1.5us. `all` runs until the design finishes.
)";

/// Answers `--help` and `--version` before Qt's parser sees the command line.
///
/// Ahead of `QCommandLineParser` because this calls neither `addHelpOption` nor
/// `addVersionOption`: those generate the divergent help text and bind `-v`, where the rest of
/// the tool spells it `-V`.
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

    const QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("enzan-gui"));
    QCoreApplication::setApplicationVersion(QString::fromStdString(enzan::versionLine()));

    // Neither `addHelpOption` nor `addVersionOption`: both are answered above, from one text
    // that matches the kernel's. Adding them here would put `-v` and `--help-all` back.
    QCommandLineParser parser;

    const QCommandLineOption attachOption(
        QStringLiteral("attach"),
        QStringLiteral("Attach to a kernel listening on this Unix domain socket."),
        QStringLiteral("socket"));
    const QCommandLineOption waveListOption(
        QStringLiteral("wave-list"),
        QStringLiteral("Open the wave window with this signal list in it."),
        QStringLiteral("file"));
    const QCommandLineOption runOption(
        QStringList() << QStringLiteral("r") << QStringLiteral("run"),
        QStringLiteral("Run the design for this long once attached."), QStringLiteral("time"));
    parser.addOption(attachOption);
    parser.addOption(waveListOption);
    parser.addOption(runOption);
    // `enzan run --gui ...` forwards its command word, which this process has no use for: it
    // starts a kernel, and the kernel is the thing that runs. Dropped rather than refused, so
    // that both spellings of the same request work.
    QStringList forwarded;
    for (const QString& argument : QCoreApplication::arguments()) {
        if (argument == QStringLiteral("run") || argument == QStringLiteral("elaborate")) continue;
        forwarded << argument;
    }
    parser.process(forwarded);

    // --- the settings ------------------------------------------------------

    // Made the first time a window opens and read each time one does. Neither is fatal: a
    // mistyped colour should not stop someone debugging, and whatever goes wrong leaves the
    // built-in theme and is said in the transcript.
    const enzan::gui::ConfigurationDirectory settings(
        enzan::gui::ConfigurationDirectory::standardPath());
    QString settingsError;
    const bool settingsMade = settings.createDefaults(settingsError);
    const enzan::gui::ConfiguredTheme configured = settings.loadTheme();

    // The window gives the application the theme's palette, style sheet and fonts, as it does
    // whenever the theme changes afterwards.
    QApplication::setStyle(QStringLiteral("Fusion"));

    // --- the session -------------------------------------------------------

    enzan::gui::KernelSession session;
    enzan::gui::MainWindow window(session, configured.theme, settings);
    window.show();

    if (!settingsMade) {
        QTextStream(stderr) << "enzan-gui: settings are not kept: " << settingsError << '\n';
        window.note(QStringLiteral("Settings are not kept: %1. The window uses the theme built "
                                   "into Enzan.")
                        .arg(settingsError));
    }
    for (const enzan::gui::ConfigurationMessage& problem : configured.problems) {
        QTextStream(stderr) << "enzan-gui: " << enzan::gui::describe(problem) << '\n';
        window.report(problem);
    }

    QString error;
    bool attached = false;
    if (parser.isSet(attachOption)) {
        attached = session.attach(parser.value(attachOption), error);
        if (!attached) {
            window.note(QStringLiteral("Could not attach to %1: %2")
                            .arg(parser.value(attachOption), error));
        }
    }

    // After attaching, though the rows wait for the design to arrive: the file is read and
    // checked now, so that a mistake in it is reported as the window opens. One that cannot be
    // loaded leaves the window empty, as a theme that cannot be read leaves the built-in colours.
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
    } else if (attached && parser.isSet(runOption)) {
        // Asked for now and run once the kernel has said what unit it counts in, as a list named
        // here is loaded once the design has arrived.
        window.run(parser.value(runOption));
    }

    return QApplication::exec();
}
