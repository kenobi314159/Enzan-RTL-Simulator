// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/OutputCapture.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace enzan::ipc {

namespace {

/// Writes all of `count` bytes, carrying on through interruptions and short writes.
void writeAll(int descriptor, const char* bytes, std::size_t count) {
    while (count > 0) {
        const ::ssize_t written = ::write(descriptor, bytes, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        bytes += written;
        count -= static_cast<std::size_t>(written);
    }
}

void closeIfOpen(int& descriptor) {
    if (descriptor >= 0) ::close(descriptor);
    descriptor = -1;
}

/// A stream buffer writing straight to a descriptor, for `std::cerr` while capturing.
class DescriptorBuffer final : public std::streambuf {
public:
    explicit DescriptorBuffer(int target) noexcept : descriptor(target) {}

protected:
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof()))
            return traits_type::not_eof(character);
        const char text = traits_type::to_char_type(character);
        writeAll(descriptor, &text, 1);
        return character;
    }

    std::streamsize xsputn(const char* text, std::streamsize count) override {
        writeAll(descriptor, text, static_cast<std::size_t>(count));
        return count;
    }

private:
    int descriptor;
};

/// True when descriptors 1 and 2 are the same file, as they are on a terminal or after `2>&1`.
bool sameFile(int first, int second) {
    struct stat firstStatus {};

    struct stat secondStatus {};

    if (::fstat(first, &firstStatus) != 0 || ::fstat(second, &secondStatus) != 0) return false;
    return firstStatus.st_dev == secondStatus.st_dev && firstStatus.st_ino == secondStatus.st_ino;
}

/// Opens a pseudo-terminal that behaves as the terminal `like` does for output, returning its
/// two ends in `controller` and `terminal`.
bool openPseudoTerminal(int like, int& controller, int& terminal, std::string& error) {
    controller = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (controller < 0 || ::grantpt(controller) != 0 || ::unlockpt(controller) != 0) {
        error = std::string("could not open a pseudo-terminal: ") + std::strerror(errno);
        closeIfOpen(controller);
        return false;
    }

    std::array<char, 256> name{};
    if (::ptsname_r(controller, name.data(), name.size()) != 0) {
        error = std::string("could not name a pseudo-terminal: ") + std::strerror(errno);
        closeIfOpen(controller);
        return false;
    }

    terminal = ::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (terminal < 0) {
        error = std::string("could not open a pseudo-terminal: ") + std::strerror(errno);
        closeIfOpen(controller);
        return false;
    }

    // No output processing: a terminal would turn each newline into a carriage return and a
    // newline on the way through, and the copy written to the real terminal would carry both.
    struct termios settings {};

    if (::tcgetattr(terminal, &settings) == 0) {
        settings.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        static_cast<void>(::tcsetattr(terminal, TCSANOW, &settings));
    }

    // The same size as the real terminal, for an application that lays its output out to fit.
    struct winsize size {};

    if (::ioctl(like, TIOCGWINSZ, &size) == 0)
        static_cast<void>(::ioctl(terminal, TIOCSWINSZ, &size));
    return true;
}

} // namespace

OutputCapture::~OutputCapture() {
    stop();
}

bool OutputCapture::start(std::string& error) {
    if (capturing) return true;

    // Whatever was buffered for the old destinations goes there first.
    std::cout.flush();
    std::cerr.flush();
    static_cast<void>(std::fflush(nullptr));

    std::array<int, 2> wake{-1, -1};
    std::array<int, 2> stopping{-1, -1};
    if (::pipe2(wake.data(), O_CLOEXEC | O_NONBLOCK) != 0
        || ::pipe2(stopping.data(), O_CLOEXEC) != 0) {
        error = std::string("could not create a pipe: ") + std::strerror(errno);
        for (int& descriptor : wake) closeIfOpen(descriptor);
        for (int& descriptor : stopping) closeIfOpen(descriptor);
        return false;
    }

    int reading = -1;
    int writing = -1;
    if (::isatty(STDOUT_FILENO) != 0) {
        if (!openPseudoTerminal(STDOUT_FILENO, reading, writing, error)) {
            for (int& descriptor : wake) closeIfOpen(descriptor);
            for (int& descriptor : stopping) closeIfOpen(descriptor);
            return false;
        }
    } else {
        std::array<int, 2> ends{-1, -1};
        if (::pipe2(ends.data(), O_CLOEXEC) != 0) {
            error = std::string("could not create a pipe: ") + std::strerror(errno);
            for (int& descriptor : wake) closeIfOpen(descriptor);
            for (int& descriptor : stopping) closeIfOpen(descriptor);
            return false;
        }
        reading = ends[0];
        writing = ends[1];
    }

    originalOutput = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    capturedError = sameFile(STDOUT_FILENO, STDERR_FILENO);
    if (capturedError) originalError = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);

    readEnd = reading;
    writeEnd = writing;
    wakeRead = wake[0];
    wakeWrite = wake[1];
    stopRead = stopping[0];
    stopWrite = stopping[1];

    static_cast<void>(::dup2(writeEnd, STDOUT_FILENO));
    if (capturedError) {
        static_cast<void>(::dup2(writeEnd, STDERR_FILENO));

        // Enzan's own diagnostics reach a front end as diagnostics already; writing them into
        // the capture as well would show each one twice.
        errorBuffer = std::make_unique<DescriptorBuffer>(originalError);
        savedErrorBuffer = std::cerr.rdbuf(errorBuffer.get());
    }

    escape = Escape::None;
    capturing = true;
    reader = std::thread(&OutputCapture::drain, this);
    return true;
}

void OutputCapture::stop() {
    if (!capturing) return;

    std::cout.flush();
    std::cerr.flush();
    static_cast<void>(std::fflush(nullptr));

    // The descriptors go back first, so that nothing written from now on is captured, and the
    // capture's own writing end is closed, so that what is left in it is all there will be.
    static_cast<void>(::dup2(originalOutput, STDOUT_FILENO));
    if (capturedError) {
        static_cast<void>(::dup2(originalError, STDERR_FILENO));
        std::cerr.rdbuf(savedErrorBuffer);
        savedErrorBuffer = nullptr;
    }
    closeIfOpen(writeEnd);

    const char signal = 's';
    writeAll(stopWrite, &signal, 1);
    reader.join();

    errorBuffer.reset();
    closeIfOpen(readEnd);
    closeIfOpen(originalOutput);
    closeIfOpen(originalError);
    closeIfOpen(stopRead);
    closeIfOpen(stopWrite);
    closeIfOpen(wakeRead);
    closeIfOpen(wakeWrite);
    capturedError = false;
    capturing = false;
}

std::string OutputCapture::take() {
    // The wake-up is cleared before the text is taken, so that text arriving in between leaves
    // a wake-up behind it rather than being stranded with none.
    if (wakeRead >= 0) {
        std::array<char, 64> discard{};
        while (::read(wakeRead, discard.data(), discard.size()) > 0) {}
    }

    const std::scoped_lock guard(lock);
    return std::exchange(pending, {});
}

void OutputCapture::drain() {
    std::array<char, 4096> buffer{};
    std::array<pollfd, 2> waiting{pollfd{.fd = readEnd, .events = POLLIN, .revents = 0},
                                  pollfd{.fd = stopRead, .events = POLLIN, .revents = 0}};

    while (true) {
        if (::poll(waiting.data(), waiting.size(), -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }

        if ((waiting[0].revents & POLLIN) != 0) {
            const ::ssize_t count = ::read(readEnd, buffer.data(), buffer.size());
            if (count > 0) deliver(buffer.data(), static_cast<std::size_t>(count));
        }

        if ((waiting[1].revents & POLLIN) != 0) {
            // Stopping: every writer has gone, so what remains is all there is.
            const int flags = ::fcntl(readEnd, F_GETFL);
            static_cast<void>(::fcntl(readEnd, F_SETFL, flags | O_NONBLOCK));
            while (true) {
                const ::ssize_t count = ::read(readEnd, buffer.data(), buffer.size());
                if (count > 0) {
                    deliver(buffer.data(), static_cast<std::size_t>(count));
                    continue;
                }
                if (count < 0 && errno == EINTR) continue;
                return;
            }
        }
    }
}

void OutputCapture::deliver(const char* bytes, std::size_t count) {
    // The terminal first, exactly as it was written.
    writeAll(originalOutput, bytes, count);

    // Then the transcript, without the escape sequences that colour it on a terminal: a
    // control sequence is an escape and a `[`, then parameters, then one final byte between
    // `@` and `~`. Any other escape is dropped with the one character after it.
    std::string text;
    text.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const char character = bytes[index];
        switch (escape) {
        case Escape::None:
            if (character == '\x1b')
                escape = Escape::Started;
            else
                text.push_back(character);
            break;
        case Escape::Started: escape = character == '[' ? Escape::Sequence : Escape::None; break;
        case Escape::Sequence:
            if (character >= '@' && character <= '~') escape = Escape::None;
            break;
        }
    }
    if (text.empty()) return;

    {
        const std::scoped_lock guard(lock);
        pending += text;
    }
    const char signal = 'w';
    writeAll(wakeWrite, &signal, 1);
}

} // namespace enzan::ipc
