// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>

namespace enzan::ipc {

/// Captures what this process writes to its standard output, for a served kernel's transcript.
///
/// A VPI application does not print through Enzan: cocotb's Python logging writes to
/// `sys.stdout` and its C++ layer to `stdout`, both straight to file descriptor 1, where the
/// transcript a front end shows cannot see them (DESIGN_GUIDE.md section 10.2). Capturing the
/// descriptor itself is what reaches them, and any other application, whatever it is written in.
///
/// **Nothing that reached the terminal stops reaching it.** Everything captured is copied to the
/// descriptor it was taken from, as it arrives, and only then offered to the transcript.
///
/// **An application sees the kind of output it had.** When standard output is a terminal it is
/// replaced by a pseudo-terminal rather than a pipe, because a program writing to a pipe buffers
/// in large blocks — Python does — and its lines would reach the window in bursts long after the
/// events they describe; a program that colours its output for a terminal goes on doing so.
/// The colour codes are stripped from what the transcript is given and kept in what the
/// terminal is.
///
/// **Standard error is captured with standard output only when the two are the same file**, as
/// they are on a terminal. Merging two destinations the user had kept apart would put errors in
/// a log file meant for output. Enzan's own `std::cerr` is turned back to the original standard
/// error while capturing, because its diagnostics reach a front end already, as diagnostics.
///
/// **A thread drains the capture.** A callback can write more than a pseudo-terminal or a pipe
/// holds in one go — a traceback, a report at the end of a run — and a write that cannot
/// complete blocks the thread making it. If that thread also had to drain the capture, which
/// it would at a time-slot boundary, it would wait for itself. The thread copies bytes and
/// touches no simulation state, so the kernel remains single-threaded in the sense of section
/// 6.4.
class OutputCapture {
public:
    OutputCapture() = default;
    OutputCapture(const OutputCapture&) = delete;
    OutputCapture& operator=(const OutputCapture&) = delete;
    OutputCapture(OutputCapture&&) = delete;
    OutputCapture& operator=(OutputCapture&&) = delete;

    /// Stops capturing, if it still is.
    ~OutputCapture();

    /// Starts capturing. Returns false having filled `error`, leaving the descriptors alone.
    ///
    /// Must be called after any child process that should not be captured has been started,
    /// since a child inherits the replaced descriptors.
    bool start(std::string& error);

    /// Puts the descriptors back and waits for everything already written to be copied.
    void stop();

    [[nodiscard]] bool active() const noexcept { return capturing; }

    /// Readable whenever there is captured text waiting, for a caller that polls.
    [[nodiscard]] int wakeDescriptor() const noexcept { return wakeRead; }

    /// Removes and returns everything captured since the last call, without colour codes.
    std::string take();

private:
    void drain();

    /// Copies what one read returned to the terminal and, stripped, to `pending`.
    void deliver(const char* bytes, std::size_t count);

    bool capturing = false;

    /// The descriptors that were standard output and standard error before capturing began.
    int originalOutput = -1;
    int originalError = -1;
    bool capturedError = false;

    /// What the thread reads, and the end the captured descriptors were pointed at.
    int readEnd = -1;
    int writeEnd = -1;

    /// Written by the thread when there is text to take, and by `stop` to end the thread.
    int wakeRead = -1;
    int wakeWrite = -1;
    int stopRead = -1;
    int stopWrite = -1;

    std::thread reader;
    std::mutex lock;
    std::string pending;

    /// Where the thread is in a colour code that may continue into the next read.
    enum class Escape : std::uint8_t { None, Started, Sequence };
    Escape escape = Escape::None;

    /// What `std::cerr` wrote to before capturing, restored by `stop`.
    std::streambuf* savedErrorBuffer = nullptr;
    std::unique_ptr<std::streambuf> errorBuffer;
};

} // namespace enzan::ipc
