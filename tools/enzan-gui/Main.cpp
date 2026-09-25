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
#include <QTextStream>

#include <cstdlib>

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

/// Finds the kernel to launch: beside this executable first, then on the path.
///
/// Beside first so that a build tree runs its own kernel rather than an installed one, which
/// is what anyone working on Enzan expects and what makes testing a change possible at all.
QString findKernel() {
    QString neighbour = QCoreApplication::applicationDirPath() + QStringLiteral("/enzan");
    if (QFileInfo(neighbour).isExecutable()) return neighbour;
    return QStringLiteral("enzan");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("enzan-gui"));
    QCoreApplication::setOrganizationName(QStringLiteral("Enzan"));
    QCoreApplication::setApplicationVersion(QString::fromStdString(enzan::versionLine()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Enzan's graphical debugging environment.\n\n"
                       "With source files, a kernel is started for them. With --attach, this "
                       "connects to a kernel already running under `enzan --listen`, which is "
                       "how a local window drives a simulation on another machine over a "
                       "forwarded socket."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption attachOption(
        QStringLiteral("attach"),
        QStringLiteral("Attach to a kernel listening on this Unix domain socket."),
        QStringLiteral("socket"));
    const QCommandLineOption topOption(QStringLiteral("top"),
                                       QStringLiteral("Module to elaborate as the top level."),
                                       QStringLiteral("module"));
    const QCommandLineOption timescaleOption(
        QStringLiteral("timescale"),
        QStringLiteral("Time unit for delays and reported times: s, ms, us, ns, ps or fs."),
        QStringLiteral("unit"));
    const QCommandLineOption scopeOption(
        QStringLiteral("wave-scope"),
        QStringLiteral("Record only this subtree, for example /tb/u_dut."), QStringLiteral("path"));
    const QCommandLineOption themeOption(QStringLiteral("theme"),
                                         QStringLiteral("Load colours from this Tcl theme script."),
                                         QStringLiteral("file"));
    const QCommandLineOption writeThemeOption(
        QStringLiteral("write-theme"),
        QStringLiteral("Write the built-in theme to this file and exit, as a starting point "
                       "for one of your own."),
        QStringLiteral("file"));
    const QCommandLineOption kernelOption(QStringLiteral("kernel"),
                                          QStringLiteral("Path to the enzan kernel to start."),
                                          QStringLiteral("program"));

    parser.addOption(attachOption);
    parser.addOption(topOption);
    parser.addOption(timescaleOption);
    parser.addOption(scopeOption);
    parser.addOption(themeOption);
    parser.addOption(writeThemeOption);
    parser.addOption(kernelOption);
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("HDL source files to simulate."),
                                 QStringLiteral("[file.sv...]"));
    parser.process(application);

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
    } else if (!parser.positionalArguments().isEmpty()) {
        QStringList arguments;
        if (parser.isSet(topOption))
            arguments << QStringLiteral("--top") << parser.value(topOption);
        if (parser.isSet(timescaleOption)) {
            arguments << QStringLiteral("--timescale") << parser.value(timescaleOption);
        }
        if (parser.isSet(scopeOption)) {
            arguments << QStringLiteral("--wave-scope") << parser.value(scopeOption);
        }
        arguments << parser.positionalArguments();

        const QString kernel =
            parser.isSet(kernelOption) ? parser.value(kernelOption) : findKernel();
        if (!session.launch(kernel, arguments, error)) {
            window.note(QStringLiteral("Could not start the kernel: %1").arg(error));
        }
    } else {
        // Neither a socket nor sources. The window is up and useful — it explains itself —
        // rather than the program exiting with a usage message at someone who double-clicked.
        window.note(QStringLiteral(
            "Nothing to simulate yet. Start this with HDL sources to run them, or with "
            "--attach <socket> to connect to a kernel already running under `enzan --listen`."));
    }

    return QApplication::exec();
}
