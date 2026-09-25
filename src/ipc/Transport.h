// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ipc/Codec.h"
#include "ipc/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace enzan::ipc {

/// A file descriptor that closes itself.
///
/// The transport juggles several descriptors across error paths where an early return is the
/// normal outcome, and a leaked one in a kernel that runs for hours is not a theoretical
/// problem. Section 13.2 asks for RAII everywhere rather than manual cleanup, and this is the
/// smallest thing that provides it.
class Descriptor {
public:
    Descriptor() = default;

    explicit Descriptor(int fileDescriptor) noexcept : value(fileDescriptor) {}

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    Descriptor(Descriptor&& other) noexcept : value(std::exchange(other.value, -1)) {}

    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) reset(std::exchange(other.value, -1));
        return *this;
    }

    ~Descriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return value; }

    [[nodiscard]] bool valid() const noexcept { return value >= 0; }

    void reset(int fileDescriptor = -1) noexcept;

    /// Gives up ownership and returns the descriptor.
    [[nodiscard]] int release() noexcept { return std::exchange(value, -1); }

private:
    int value = -1;
};

/// What `Connection::next` found.
enum class ReceiveStatus : std::uint8_t {
    /// A whole message is available in `frame`.
    Message,
    /// Nothing complete has arrived yet.
    Empty,
    /// The stream cannot be parsed. The connection must be closed; there is no way to
    /// resynchronise a length-prefixed stream whose length is wrong.
    Malformed,
};

/// One end of a connection, with buffering in both directions.
///
/// The connection never blocks. Section 2.2 requires that a stalled or slow GUI never stall
/// the kernel, and a blocking write is exactly how that happens: a client that stops reading
/// fills the socket buffer, and the kernel sits in `write` until it comes back. Output is
/// queued here instead and drained whenever the socket will take it, so a client that stops
/// reading costs memory in the kernel and nothing else.
class Connection {
public:
    Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    ~Connection() = default;

    /// Takes ownership of an already-connected socket and puts it in non-blocking mode.
    static Connection adopt(Descriptor socket);

    [[nodiscard]] bool isOpen() const noexcept { return handle.valid(); }

    /// The descriptor, for a `poll` set or a Qt socket notifier. Never written to directly.
    [[nodiscard]] int descriptor() const noexcept { return handle.get(); }

    /// Queues a message. Does not write to the socket; call `flush`.
    template<typename Message>
    void send(MessageType type, Message&& message) {
        if (!handle.valid()) return;
        encode(type, message, outgoing);
    }

    /// Writes as much queued output as the socket will accept.
    ///
    /// Returns false once the connection has broken, having filled `error`. A full socket is
    /// not a failure: the unwritten remainder stays queued for the next call.
    bool flush();

    [[nodiscard]] bool hasPendingOutput() const noexcept { return outOffset < outgoing.size(); }

    [[nodiscard]] std::size_t pendingOutputBytes() const noexcept {
        return outgoing.size() - outOffset;
    }

    /// Reads whatever has arrived into the input buffer.
    ///
    /// Returns false when the peer has closed the connection or the socket failed, having
    /// filled `error`. Having nothing to read is success with nothing added.
    bool receive();

    /// Hands over the next complete message.
    ///
    /// `frame.payload` points into this connection's input buffer and stays valid until the
    /// next call to `next` or `receive`, which is where the consumed bytes are dropped.
    /// Deferring that is what lets a caller decode in place rather than copying every message
    /// out first.
    [[nodiscard]] ReceiveStatus next(Frame& frame);

    /// Closes the socket. Anything still queued for sending is discarded.
    void close() noexcept;

    /// Blocks until the socket is readable, `timeoutMilliseconds` elapses, or forever when the
    /// timeout is negative. Returns true when there is something to read.
    [[nodiscard]] bool waitReadable(int timeoutMilliseconds) const;

    /// Blocks until queued output has all been written or the connection broke.
    ///
    /// For the few places where the cost of blocking is acceptable and losing the message is
    /// not: sending a farewell just before exiting, and tests.
    bool flushBlocking();

    [[nodiscard]] const std::string& error() const noexcept { return lastError; }

private:
    /// Drops the bytes handed out by the previous `next`.
    void dropConsumed();

    Descriptor handle;

    std::vector<std::byte> outgoing;
    std::size_t outOffset = 0;

    std::vector<std::byte> incoming;

    /// Bytes the previous `next` returned, to be dropped before the next one is looked for.
    std::size_t pendingConsume = 0;

    std::string lastError;
};

/// A listening Unix domain socket.
///
/// Unix domain rather than TCP: the socket's path is the name a user passes to `--listen` and
/// to `enzan-gui --attach`, filesystem permissions are the access control, and a remote attach
/// is done by forwarding the socket over ssh, which is how the same feature works in the tools
/// users already have.
class Listener {
public:
    Listener() = default;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) noexcept = default;
    Listener& operator=(Listener&&) noexcept = default;

    /// Closes the socket and removes its path.
    ~Listener();

    /// Binds and listens on `socketPath`. Returns false having filled `error`.
    ///
    /// A path left behind by a kernel that died is removed and reused, but only after a
    /// connection attempt proves nothing is listening on it. Unlinking first without checking
    /// would let a second kernel silently steal a socket a running one is serving.
    bool listen(const std::filesystem::path& socketPath, std::string& error);

    /// Accepts a waiting connection. Returns false when none is waiting or the accept failed;
    /// `error` is empty in the first case and filled in the second.
    bool accept(Connection& accepted, std::string& error);

    [[nodiscard]] bool isListening() const noexcept { return handle.valid(); }

    [[nodiscard]] int descriptor() const noexcept { return handle.get(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return boundPath; }

    void close() noexcept;

private:
    Descriptor handle;
    std::filesystem::path boundPath;
};

/// Connects to a kernel listening on `socketPath`. Returns false having filled `error`.
bool connectTo(const std::filesystem::path& socketPath, Connection& connection, std::string& error);

} // namespace enzan::ipc
