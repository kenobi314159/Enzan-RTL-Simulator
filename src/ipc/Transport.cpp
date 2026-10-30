// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/Transport.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace enzan::ipc {

namespace {

/// Bytes read from the socket in one call. Large enough that a burst of value changes arrives
/// in a handful of reads, small enough to be a stack-free heap buffer per connection.
constexpr std::size_t ReadChunkBytes = std::size_t{64} * 1024;

/// Consumed output is dropped from the front of the queue only once this much has piled up,
/// so that a busy connection does not memmove its whole queue after every write.
constexpr std::size_t OutputCompactionThreshold = std::size_t{64} * 1024;

/// True when an errno means "nothing to do right now, try again later".
///
/// The two names are the same number on Linux and are allowed to differ elsewhere, so the
/// comparison is written once where they agree. Spelling both unconditionally is a tautology
/// the compiler is right to complain about rather than portability insurance.
bool wouldBlock(int code) noexcept {
#if EAGAIN == EWOULDBLOCK
    return code == EAGAIN;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

std::string describeErrno(std::string_view what, int code) {
    return std::format("{}: {}", what, std::generic_category().message(code));
}

/// Puts a descriptor into non-blocking mode.
bool makeNonBlocking(int fileDescriptor, std::string& error) {
    const int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags < 0) {
        error = describeErrno("could not read the socket's flags", errno);
        return false;
    }
    if (::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        error = describeErrno("could not put the socket into non-blocking mode", errno);
        return false;
    }
    return true;
}

/// Fills `address` with `path`, or explains why the path cannot be used.
///
/// The limit is the operating system's and not Enzan's, and hitting it with a plausible path
/// under a long temporary directory is easy, so it is worth a diagnostic that says the number
/// rather than a bind failing with "invalid argument".
bool fillAddress(sockaddr_un& address, const std::filesystem::path& path, std::string& error) {
    const std::string text = path.string();
    if (text.empty()) {
        error = "the socket path is empty";
        return false;
    }
    if (text.size() >= sizeof(address.sun_path)) {
        error = std::format("the socket path is {} characters; a Unix domain socket path must "
                            "be shorter than {}",
                            text.size(), sizeof(address.sun_path));
        return false;
    }

    address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(static_cast<void*>(address.sun_path), text.data(), text.size());
    return true;
}

/// True when something is already listening on `path`.
///
/// Used to tell a socket left behind by a kernel that died from one a running kernel is
/// serving. Connecting is the only way to find out: the file exists either way.
bool somethingIsListening(const std::filesystem::path& path) {
    sockaddr_un address{};
    std::string ignored;
    if (!fillAddress(address, path, ignored)) return false;

    const Descriptor probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!probe.valid()) return false;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API's own shape
    const int connected =
        ::connect(probe.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    return connected == 0;
}

} // namespace

void Descriptor::reset(int fileDescriptor) noexcept {
    if (value >= 0 && value != fileDescriptor) ::close(value);
    value = fileDescriptor;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

Connection Connection::adopt(Descriptor socket) {
    Connection connection;
    std::string ignored;

    // A blocking socket here would defeat the whole point of the class, so a descriptor that
    // cannot be made non-blocking is refused rather than used.
    if (socket.valid() && makeNonBlocking(socket.get(), ignored)) {
        connection.handle = std::move(socket);
    }
    return connection;
}

void Connection::close() noexcept {
    handle.reset();
    outgoing.clear();
    outOffset = 0;
    incoming.clear();
    pendingConsume = 0;
}

bool Connection::flush() {
    if (!handle.valid()) {
        lastError = "the connection is closed";
        return false;
    }

    while (outOffset < outgoing.size()) {
        const std::size_t remaining = outgoing.size() - outOffset;

        // MSG_NOSIGNAL rather than a SIGPIPE handler: a peer that goes away must make the write
        // fail, not kill the kernel. A simulation lost because a GUI was closed would be the exact
        // failure the two processes are separated to avoid.
        const ssize_t written =
            ::send(handle.get(), outgoing.data() + outOffset, remaining, MSG_NOSIGNAL);

        if (written > 0) {
            outOffset += static_cast<std::size_t>(written);
            continue;
        }

        // `errno` only means anything after a negative return, so it is read only there. A
        // zero-length write on a stream socket with bytes to send does not happen; treating it
        // as "nothing more will go out now" is the safe reading if it ever did.
        if (written == 0) break;
        if (errno == EINTR) continue;
        if (wouldBlock(errno)) break;

        lastError = describeErrno("the connection could not be written to", errno);
        handle.reset();
        return false;
    }

    if (outOffset == outgoing.size()) {
        outgoing.clear();
        outOffset = 0;
    } else if (outOffset >= OutputCompactionThreshold) {
        outgoing.erase(outgoing.begin(), outgoing.begin() + static_cast<std::ptrdiff_t>(outOffset));
        outOffset = 0;
    }

    return true;
}

bool Connection::flushBlocking() {
    while (hasPendingOutput()) {
        if (!flush()) return false;
        if (!hasPendingOutput()) break;

        pollfd waiting{.fd = handle.get(), .events = POLLOUT, .revents = 0};
        if (::poll(&waiting, 1, -1) < 0) {
            if (errno == EINTR) continue;
            lastError = describeErrno("waiting for the connection to drain failed", errno);
            return false;
        }
    }
    return true;
}

bool Connection::receive() {
    if (!handle.valid()) {
        lastError = "the connection is closed";
        return false;
    }

    dropConsumed();

    while (true) {
        const std::size_t at = incoming.size();
        incoming.resize(at + ReadChunkBytes);

        const ssize_t read = ::recv(handle.get(), incoming.data() + at, ReadChunkBytes, 0);
        if (read > 0) {
            incoming.resize(at + static_cast<std::size_t>(read));
            // A short read means the socket is drained; asking again would only return EAGAIN.
            if (std::cmp_less(read, ReadChunkBytes)) return true;
            continue;
        }

        incoming.resize(at);

        if (read == 0) {
            lastError = "the peer closed the connection";
            handle.reset();
            return false;
        }
        if (errno == EINTR) continue;
        if (wouldBlock(errno)) return true;

        lastError = describeErrno("the connection could not be read", errno);
        handle.reset();
        return false;
    }
}

void Connection::dropConsumed() {
    if (pendingConsume == 0) return;
    incoming.erase(incoming.begin(),
                   incoming.begin() + static_cast<std::ptrdiff_t>(pendingConsume));
    pendingConsume = 0;
}

ReceiveStatus Connection::next(Frame& frame) {
    dropConsumed();

    std::size_t consumed = 0;
    switch (tryReadFrame(incoming, frame, consumed)) {
    case FrameStatus::Complete:
        // The payload points into `incoming` and must stay valid while the caller decodes it,
        // so the bytes are dropped at the start of the next call rather than now.
        pendingConsume = consumed;
        return ReceiveStatus::Message;
    case FrameStatus::Incomplete: return ReceiveStatus::Empty;
    case FrameStatus::Malformed:
        lastError = "the peer sent a frame this build cannot parse";
        return ReceiveStatus::Malformed;
    }
    return ReceiveStatus::Malformed;
}

bool Connection::waitReadable(int timeoutMilliseconds) const {
    if (!handle.valid()) return false;

    pollfd waiting{.fd = handle.get(), .events = POLLIN, .revents = 0};
    while (true) {
        const int ready = ::poll(&waiting, 1, timeoutMilliseconds);
        if (ready < 0 && errno == EINTR) continue;
        return ready > 0;
    }
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

Listener::~Listener() {
    close();
}

void Listener::close() noexcept {
    if (handle.valid()) {
        handle.reset();

        // The path is removed with the socket. Leaving it behind would make the next kernel
        // pay the stale-socket dance below for no reason, and would litter the user's
        // directory with files that do nothing.
        std::error_code ignored;
        std::filesystem::remove(boundPath, ignored);
    }
    boundPath.clear();
}

bool Listener::listen(const std::filesystem::path& socketPath, std::string& error) {
    error.clear();
    close();

    sockaddr_un address{};
    if (!fillAddress(address, socketPath, error)) return false;

    // A path left behind by a kernel that died is reusable; one a running kernel is serving is
    // not. Probing tells the two apart, and getting this wrong in either direction is bad: a
    // refusal that never clears strands the user, and an unlink that does not check steals a
    // live session's socket.
    std::error_code existence;
    if (std::filesystem::exists(socketPath, existence)) {
        if (somethingIsListening(socketPath)) {
            error =
                std::format("'{}' is already being served by another kernel", socketPath.string());
            return false;
        }

        std::error_code removal;
        if (!std::filesystem::remove(socketPath, removal)) {
            error = std::format("'{}' is left over from an earlier run and could not be "
                                "removed: {}",
                                socketPath.string(), removal.message());
            return false;
        }
    }

    Descriptor socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        error = describeErrno("could not create a Unix domain socket", errno);
        return false;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API's own shape
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        error = describeErrno(std::format("could not bind '{}'", socketPath.string()), errno);
        return false;
    }

    // One client at a time is what a kernel serves, but the backlog is what decides whether a
    // second attach is refused promptly or hangs, so it is not zero.
    if (::listen(socket.get(), 4) < 0) {
        error = describeErrno("could not listen on the socket", errno);
        std::error_code ignored;
        std::filesystem::remove(socketPath, ignored);
        return false;
    }

    if (!makeNonBlocking(socket.get(), error)) {
        std::error_code ignored;
        std::filesystem::remove(socketPath, ignored);
        return false;
    }

    handle = std::move(socket);
    boundPath = socketPath;
    return true;
}

bool Listener::accept(Connection& accepted, std::string& error) {
    error.clear();
    if (!handle.valid()) {
        error = "nothing is listening";
        return false;
    }

    while (true) {
        Descriptor incoming(::accept4(handle.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (incoming.valid()) {
            accepted = Connection::adopt(std::move(incoming));
            return accepted.isOpen();
        }

        if (errno == EINTR) continue;
        if (wouldBlock(errno)) return false;

        error = describeErrno("could not accept a connection", errno);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------

bool connectTo(const std::filesystem::path& socketPath, Connection& connection,
               std::string& error) {
    error.clear();

    sockaddr_un address{};
    if (!fillAddress(address, socketPath, error)) return false;

    Descriptor socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        error = describeErrno("could not create a Unix domain socket", errno);
        return false;
    }

    // Connected while still blocking, so that a kernel which is not there fails here with a
    // clear message rather than in the middle of the first read.
    while (true) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API's shape
        if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address))
            == 0) {
            break;
        }
        if (errno == EINTR) continue;

        error = describeErrno(std::format("could not connect to '{}'", socketPath.string()), errno);
        return false;
    }

    connection = Connection::adopt(std::move(socket));
    if (!connection.isOpen()) {
        error = "the connection could not be put into non-blocking mode";
        return false;
    }
    return true;
}

} // namespace enzan::ipc
