// SPDX-License-Identifier: GPL-3.0-or-later
//
// How long the wave window takes to look for a value along a whole trace.
//
// A search for a value walks the row's edges, since no index says when a row holds one, so its
// cost is per edge and a trace's length is what a reader waits for. This measures the worst case,
// a value the trace never takes, so that every edge is examined: a kernel runs a counter for as
// many changes as are asked for, the window's session fetches every one of them over the socket
// as it does in use, and the model is searched as the Find field searches it.
//
//     enzan-bench-find-value [changes]
//
// It prints the edges examined and the nanoseconds each cost, for a row showing the whole
// counter, one showing a slice of it and a group holding it, searched forwards and backwards. Run
// it from an optimised build: a debug build measures what the optimiser removes.

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "gui/KernelSession.h"
#include "gui/Theme.h"
#include "gui/WaveformModel.h"
#include "ipc/KernelServer.h"
#include "runtime/Time.h"
#include "session/Session.h"
#include "wavedb/WaveformDatabase.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>

#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace enzan::bench {

namespace {

/// A counter that moves on every rising edge of a clock of period 10: one change of `count` for
/// every ten time units.
constexpr const char* Design = R"(
module tb;
    logic clk = 1'b0;
    logic [31:0] count = 32'd0;
    always #5 clk = ~clk;
    always @(posedge clk) count <= count + 32'd1;
endmodule
)";

class Collector final : public DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        std::fprintf(stderr, "%s\n", diagnostic.message.c_str());
    }
};

/// Gives both ends a turn until `done`, or a minute has gone by.
template<typename Predicate>
bool pumpUntil(ipc::KernelServer& server, Predicate done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 60'000) {
        static_cast<void>(server.step(1));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return done();
}

/// Times `search`, run once, and prints it against the edges the row has.
template<typename Search>
void measure(const char* what, std::uint64_t edges, Search search) {
    const auto start = std::chrono::steady_clock::now();
    const gui::WaveformModel::Match match = search();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const double nanoseconds = std::chrono::duration<double, std::nano>(elapsed).count();
    std::printf("%-28s %10llu edges %10.1f ms %8.1f ns/edge  %s\n", what,
                static_cast<unsigned long long>(edges), nanoseconds / 1e6,
                nanoseconds / static_cast<double>(edges),
                match.outcome == gui::WaveformModel::Match::Outcome::Absent ? "absent"
                                                                            : "UNEXPECTED");
}

int findValue(int argc, char** argv) {
    const std::uint64_t changes = argc > 1 ? std::stoull(argv[1]) : 2'000'000;

    const QCoreApplication application(argc, argv);

    const QTemporaryDir directory;
    const std::string source = directory.filePath(QStringLiteral("tb.sv")).toStdString();
    std::ofstream(source) << Design;

    SourceManager sources;
    DiagnosticEngine engine(sources);
    Collector collector;
    engine.addConsumer(collector);

    session::Session kernel(sources, engine);
    kernel.setTimeScale(runtime::TimeScale(-9));
    session::Session::AnalysisRequest request;
    request.files.emplace_back(source);
    if (!kernel.analyse(request) || !kernel.elaborate("tb")) return 1;

    ipc::ServerOptions options;
    options.kernelVersion = "enzan bench";
    options.timeScale = runtime::TimeScale(-9);
    std::ostringstream transcript;
    options.localTranscript = &transcript;
    ipc::KernelServer server(kernel, engine, options);
    server.start();

    std::string error;
    const QString socket = directory.filePath(QStringLiteral("k.sock"));
    if (!server.listen(socket.toStdString(), error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    gui::KernelSession session;
    QString attachError;
    if (!session.attach(socket, attachError)) return 1;
    if (!pumpUntil(server, [&] { return !session.instances().isEmpty(); })) return 1;

    std::optional<gui::SignalInfo> found;
    QObject::connect(&session, &gui::KernelSession::objectsReceived,
                     [&](const QString&, const QVector<gui::SignalInfo>& objects) {
                         for (const gui::SignalInfo& info : objects) {
                             if (info.name == QStringLiteral("count")) found = info;
                         }
                     });
    session.queryObjects(QStringLiteral("/tb"));
    if (!pumpUntil(server, [&] { return found.has_value(); }) || !found.has_value()) return 1;
    const gui::SignalInfo count = *found;

    // Shown before the run, so that it is recorded from the start.
    session.subscribe({count.signalIndex});
    const runtime::SimTime duration = changes * 10;

    QElapsedTimer setup;
    setup.start();
    session.run(duration);
    if (!pumpUntil(server, [&] {
            return session.currentTime() == duration && session.heldTime() == session.latestTime();
        })) {
        std::fprintf(stderr, "the window never caught up with the kernel\n");
        return 1;
    }

    const wavedb::WaveformDatabase& database = session.waveforms();
    const std::uint64_t edges = database.changeCount(database.recordIndexOf(count.signalIndex));
    std::printf("%llu changes simulated and fetched in %.1f s\n",
                static_cast<unsigned long long>(edges),
                static_cast<double>(setup.elapsed()) / 1000.0);

    gui::Theme theme;
    theme.loadBuiltIn();
    gui::WaveformModel model(session, theme);
    const int whole = model.addSignal(count);
    const int slice = model.addSlice(whole, 31, 24);

    // A group holding nothing but the counter shows what the counter shows, and is searched the
    // way any row with children is: the general way, against which the others compare.
    const int group = model.addGroup(QStringLiteral("group"));
    model.addSignal(count, group);

    // Values the counter never reaches, so that every edge is examined.
    const runtime::SimTime held = session.heldTime();
    const eir::BitVector never = eir::BitVector::filled(32, eir::Logic::One);
    const eir::BitVector neverTop = eir::BitVector::filled(8, eir::Logic::One);

    measure("whole signal, forwards", edges,
            [&] { return model.nextMatchAfter(whole, 0, never, held, INT_MAX); });
    measure("whole signal, backwards", edges,
            [&] { return model.previousMatchBefore(whole, held, never, held, INT_MAX); });
    measure("slice [31:24], forwards", edges,
            [&] { return model.nextMatchAfter(slice, 0, neverTop, held, INT_MAX); });
    measure("slice [31:24], backwards", edges,
            [&] { return model.previousMatchBefore(slice, held, neverTop, held, INT_MAX); });
    measure("group of it, forwards", edges,
            [&] { return model.nextMatchAfter(group, 0, never, held, INT_MAX); });
    measure("group of it, backwards", edges,
            [&] { return model.previousMatchBefore(group, held, never, held, INT_MAX); });
    return 0;
}

} // namespace

} // namespace enzan::bench

int main(int argc, char** argv) {
    return enzan::bench::findValue(argc, argv);
}
