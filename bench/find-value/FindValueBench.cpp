// SPDX-License-Identifier: GPL-3.0-or-later
//
// How long the wave window takes to look for a value along a whole trace.
//
// A search for a value is a scan, since no index says when a row holds one, so its cost is per
// edge and a trace's length is what a reader waits for. This measures the worst case, a value the
// trace never takes, so that every edge is examined: a kernel runs counters for as many changes
// as are asked for, the window's session fetches every one of them over the socket as it does in
// use, and the model is searched as the Find field searches it.
//
//     enzan-bench-find-value [changes]
//
// It prints the edges examined and the nanoseconds each cost, searched forwards and backwards, for
// a row of a signal, a slice of it, the signal unpacked with a bit taken out, and groups of one,
// two and three signals. Run it from an optimised build: a debug build measures what the
// optimiser removes.

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
#include <QLatin1String>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

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

/// Counters on a clock of period 10: `count` and `other` move together on its rising edge, one
/// change each for every ten time units, and `falling` on its falling edge, half way between.
constexpr const char* Design = R"(
module tb;
    logic clk = 1'b0;
    logic [31:0] count = 32'd0;
    logic [31:0] other = 32'd0;
    logic [31:0] falling = 32'd0;
    always #5 clk = ~clk;
    always @(posedge clk) begin
        count <= count + 32'd1;
        other <= other + 32'd3;
    end
    always @(negedge clk) falling <= falling + 32'd1;
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

/// Times a search of `rowId` for `value` each way along the whole trace, and prints each against
/// the edges the row has.
void measure(const char* what, std::uint64_t edges, const gui::WaveformModel& model, int rowId,
             const eir::BitVector& value, runtime::SimTime held) {
    for (const bool forward : {true, false}) {
        const auto start = std::chrono::steady_clock::now();
        const gui::WaveformModel::Match match =
            forward ? model.nextMatchAfter(rowId, 0, value, held, INT_MAX)
                    : model.previousMatchBefore(rowId, held, value, held, INT_MAX);
        const auto elapsed = std::chrono::steady_clock::now() - start;

        const double nanoseconds = std::chrono::duration<double, std::nano>(elapsed).count();
        std::printf("%-26s %-9s %10llu edges %10.1f ms %8.1f ns/edge  %s\n", what,
                    forward ? "forwards" : "backwards", static_cast<unsigned long long>(edges),
                    nanoseconds / 1e6, nanoseconds / static_cast<double>(edges),
                    match.outcome == gui::WaveformModel::Match::Outcome::Absent ? "absent"
                                                                                : "UNEXPECTED");
    }
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

    QVector<gui::SignalInfo> objects;
    QObject::connect(
        &session, &gui::KernelSession::objectsReceived,
        [&](const QString&, const QVector<gui::SignalInfo>& received) { objects = received; });
    session.queryObjects(QStringLiteral("/tb"));
    if (!pumpUntil(server, [&] { return !objects.isEmpty(); })) return 1;

    const auto named = [&](const char* name) {
        for (const gui::SignalInfo& info : objects) {
            if (info.name == QLatin1String(name)) return std::optional(info);
        }
        return std::optional<gui::SignalInfo>();
    };
    const std::optional<gui::SignalInfo> foundCount = named("count");
    const std::optional<gui::SignalInfo> foundOther = named("other");
    const std::optional<gui::SignalInfo> foundFalling = named("falling");
    if (!foundCount.has_value() || !foundOther.has_value() || !foundFalling.has_value()) return 1;
    const gui::SignalInfo& count = *foundCount;
    const gui::SignalInfo& other = *foundOther;
    const gui::SignalInfo& falling = *foundFalling;

    // Shown before the run, so that they are recorded from the start.
    session.subscribe({count.signalIndex, other.signalIndex, falling.signalIndex});
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
    std::printf("%llu changes of each counter simulated and fetched in %.1f s\n",
                static_cast<unsigned long long>(edges),
                static_cast<double>(setup.elapsed()) / 1000.0);

    gui::Theme theme;
    theme.loadBuiltIn();
    gui::WaveformModel model(session, theme);

    // Values the counters never reach, so that every edge is examined.
    const runtime::SimTime held = session.heldTime();
    const auto never = [](std::uint32_t bits) {
        return eir::BitVector::filled(bits, eir::Logic::One);
    };

    const int whole = model.addSignal(count);
    measure("signal", edges, model, whole, never(32), held);
    measure("slice [31:24]", edges, model, model.addSlice(whole, 31, 24), never(8), held);

    // The counter unpacked with a bit taken out shows what is left of it, joined from its bits.
    const int bus = model.addSignal(count);
    model.unpack(bus);
    model.removeRow(model.rowIdOf(model.index(31, 0, model.indexOfRow(bus))));
    measure("bus without bit 0", edges, model, bus, never(31), held);

    const int one = model.addGroup(QStringLiteral("one"));
    model.addSignal(count, one);
    measure("group of 1 signal", edges, model, one, never(32), held);

    const int two = model.addGroup(QStringLiteral("two"));
    model.addSignal(count, two);
    model.addSignal(other, two);
    measure("group of 2, same times", edges, model, two, never(64), held);

    // The falling counter changes between the others, so the group has twice their edges.
    const int three = model.addGroup(QStringLiteral("three"));
    model.addSignal(count, three);
    model.addSignal(other, three);
    model.addSignal(falling, three);
    measure("group of 3, apart in time", 2 * edges, model, three, never(96), held);
    return 0;
}

} // namespace

} // namespace enzan::bench

int main(int argc, char** argv) {
    return enzan::bench::findValue(argc, argv);
}
