// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the socket transport, over real sockets rather than a stand-in.
//
// A fake would not exercise the parts that actually go wrong: a stream that hands over half a
// message, a socket that refuses a write because its buffer is full, a peer that disappears, and
// a socket path left behind by a kernel that died. All four are here.

#include "ipc/Transport.h"

#include "ipc/Protocol.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using enzan::ipc::ChangesMessage;
using enzan::ipc::Connection;
using enzan::ipc::Descriptor;
using enzan::ipc::Frame;
using enzan::ipc::Listener;
using enzan::ipc::MessageType;
using enzan::ipc::ReceiveStatus;
using enzan::ipc::TranscriptMessage;

/// A pair of connected sockets, which is a connection without a listener in the way.
struct SocketPair {
    SocketPair() {
        std::array<int, 2> descriptors{-1, -1};
        const int made = ::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data());
        EXPECT_EQ(made, 0) << "could not create a socket pair";
        left = Connection::adopt(Descriptor(descriptors[0]));
        right = Connection::adopt(Descriptor(descriptors[1]));
    }

    Connection left;
    Connection right;
};

/// Pushes everything queued on `from` across and reads it into `to`.
void transfer(Connection& from, Connection& to) {
    ASSERT_TRUE(from.flushBlocking()) << from.error();
    ASSERT_TRUE(to.waitReadable(2000)) << "nothing arrived within two seconds";
    ASSERT_TRUE(to.receive()) << to.error();
}

/// Reads one transcript message, failing the test when there is not exactly one.
std::string takeTranscript(Connection& connection) {
    Frame frame;
    EXPECT_EQ(connection.next(frame), ReceiveStatus::Message);
    EXPECT_EQ(frame.type, static_cast<std::uint16_t>(MessageType::Transcript));

    TranscriptMessage message;
    EXPECT_TRUE(enzan::ipc::decodePayload(frame.payload, message));
    return message.text;
}

/// A directory the test owns, removed when it goes out of scope.
class ScratchDirectory {
public:
    ScratchDirectory() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        // Kept short on purpose: a Unix domain socket path must fit in 108 bytes, and a test
        // that failed only because its own name was long would be a poor way to learn that.
        location = std::filesystem::temp_directory_path()
                   / ("enz-ipc-" + std::to_string(::getpid()) + '-'
                      + std::to_string(std::hash<std::string>{}(info->name()) % 100000));
        std::error_code error;
        std::filesystem::remove_all(location, error);
        std::filesystem::create_directories(location, error);
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;

    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(location, error);
    }

    [[nodiscard]] std::filesystem::path socketPath() const { return location / "kernel.sock"; }

private:
    std::filesystem::path location;
};

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

TEST(Descriptor, ClosesWhatItOwnsAndNotWhatItGaveAway) {
    std::array<int, 2> descriptors{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()), 0);

    int released = -1;
    {
        Descriptor owner(descriptors[0]);
        EXPECT_TRUE(owner.valid());
        released = owner.release();
        EXPECT_FALSE(owner.valid());
    }

    // Released, so still open: writing to it must succeed.
    EXPECT_EQ(::write(released, "x", 1), 1);
    ::close(released);
    ::close(descriptors[1]);
}

TEST(Descriptor, MovingTransfersOwnership) {
    std::array<int, 2> descriptors{-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()), 0);

    Descriptor first(descriptors[0]);
    const Descriptor second(std::move(first));

    // Reading the moved-from object is the point: a descriptor that kept its value after being
    // moved out of would be closed twice.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) — see above
    EXPECT_FALSE(first.valid());
    EXPECT_EQ(second.get(), descriptors[0]);
    ::close(descriptors[1]);
}

// ---------------------------------------------------------------------------
// Sending and receiving
// ---------------------------------------------------------------------------

TEST(Connection, AMessageCrossesIntact) {
    SocketPair pair;

    TranscriptMessage sent;
    sent.text = "count = 42";
    pair.left.send(MessageType::Transcript, sent);
    EXPECT_TRUE(pair.left.hasPendingOutput());

    transfer(pair.left, pair.right);

    EXPECT_EQ(takeTranscript(pair.right), "count = 42");
    EXPECT_FALSE(pair.left.hasPendingOutput());
}

TEST(Connection, SeveralMessagesArriveInOrder) {
    SocketPair pair;

    for (int index = 0; index < 5; ++index) {
        TranscriptMessage message;
        message.text = "line " + std::to_string(index);
        pair.left.send(MessageType::Transcript, message);
    }

    transfer(pair.left, pair.right);

    for (int index = 0; index < 5; ++index) {
        EXPECT_EQ(takeTranscript(pair.right), "line " + std::to_string(index));
    }

    Frame frame;
    EXPECT_EQ(pair.right.next(frame), ReceiveStatus::Empty);
}

TEST(Connection, AMessageSplitAcrossReadsIsReassembled) {
    // The property a length-prefixed stream exists for. A socket hands over whatever has
    // arrived, so a message written in one call routinely arrives in several, and a reader
    // that assumed otherwise would pass every small test and fail on a real design.
    SocketPair pair;

    ChangesMessage sent;
    sent.signals.emplace_back().signalIndex = 4;
    for (std::uint32_t index = 0; index < 500; ++index) {
        sent.signals.front().entries.push_back(enzan::ipc::ChangeEntry{
            .time = index, .value = enzan::eir::BitVector::fromUInt64(index, 64)});
    }

    std::vector<std::byte> encoded;
    enzan::ipc::encode(MessageType::Changes, sent, encoded);
    ASSERT_GT(encoded.size(), 1000U);

    // Written by hand in small pieces, because the connection's own writer hands the socket
    // everything it will take and the reader drains it; the arrival pattern has to be forced.
    constexpr std::size_t PieceBytes = 100;
    Frame frame;
    for (std::size_t at = 0; at + PieceBytes < encoded.size(); at += PieceBytes) {
        ASSERT_EQ(::write(pair.left.descriptor(), encoded.data() + at, PieceBytes),
                  static_cast<ssize_t>(PieceBytes));
        ASSERT_TRUE(pair.right.waitReadable(2000));
        ASSERT_TRUE(pair.right.receive()) << pair.right.error();
        ASSERT_EQ(pair.right.next(frame), ReceiveStatus::Empty)
            << "a partial message was handed over as a whole one";
    }

    const std::size_t written = (encoded.size() / PieceBytes) * PieceBytes;
    const std::size_t rest = encoded.size() - written;
    ASSERT_EQ(::write(pair.left.descriptor(), encoded.data() + written, rest),
              static_cast<ssize_t>(rest));
    ASSERT_TRUE(pair.right.waitReadable(2000));
    ASSERT_TRUE(pair.right.receive()) << pair.right.error();

    ASSERT_EQ(pair.right.next(frame), ReceiveStatus::Message);

    ChangesMessage got;
    ASSERT_TRUE(enzan::ipc::decodePayload(frame.payload, got));
    ASSERT_EQ(got.signals.size(), 1U);
    EXPECT_EQ(got.signals[0].signalIndex, 4U);
    ASSERT_EQ(got.signals[0].entries.size(), 500U);
    EXPECT_EQ(got.signals[0].entries[499].time, 499U);
    EXPECT_EQ(got.signals[0].entries[499].value, enzan::eir::BitVector::fromUInt64(499, 64));
}

TEST(Connection, AFullSocketQueuesRatherThanBlocking) {
    // Section 2.2's requirement in its most direct form: a client that has stopped reading must
    // cost the kernel memory and nothing else. If this blocked, the test would hang.
    SocketPair pair;

    for (int index = 0; index < 2000; ++index) {
        TranscriptMessage message;
        message.text = std::string(1000, 'x');
        pair.left.send(MessageType::Transcript, message);
    }

    ASSERT_TRUE(pair.left.flush()) << pair.left.error();
    EXPECT_TRUE(pair.left.hasPendingOutput()) << "the socket accepted two megabytes in one go";

    // Draining the reader lets the rest go out, which is what a client catching up looks like.
    unsigned rounds = 0;
    while (pair.left.hasPendingOutput() && rounds < 10'000) {
        ASSERT_TRUE(pair.right.receive()) << pair.right.error();
        ASSERT_TRUE(pair.left.flush()) << pair.left.error();
        ++rounds;
    }
    EXPECT_FALSE(pair.left.hasPendingOutput());
}

TEST(Connection, AClosedPeerIsReportedRatherThanFatal) {
    // Writing to a socket whose peer has gone would raise SIGPIPE and kill the kernel by
    // default. Losing a simulation because a GUI window was closed is exactly what separating
    // the processes is meant to prevent, so the write has to fail instead.
    SocketPair pair;
    pair.right.close();

    bool reported = false;
    for (int attempt = 0; attempt < 200 && !reported; ++attempt) {
        TranscriptMessage message;
        message.text = std::string(10'000, 'y');
        pair.left.send(MessageType::Transcript, message);
        reported = !pair.left.flush();
    }

    EXPECT_TRUE(reported) << "writing to a vanished peer never failed";
    EXPECT_FALSE(pair.left.isOpen());
    EXPECT_FALSE(pair.left.error().empty());
}

TEST(Connection, AnEndOfStreamIsReported) {
    SocketPair pair;
    pair.left.close();

    ASSERT_TRUE(pair.right.waitReadable(2000));
    EXPECT_FALSE(pair.right.receive());
    EXPECT_FALSE(pair.right.isOpen());
    EXPECT_EQ(pair.right.error(), "the peer closed the connection");
}

TEST(Connection, AMalformedFrameIsRefused) {
    SocketPair pair;

    // A length no build would ever send. There is no way to resynchronise a length-prefixed
    // stream once the length is wrong, so the only right answer is to say so and close.
    const std::vector<unsigned char> rubbish(enzan::ipc::FrameHeaderBytes, 0xFF);
    ASSERT_EQ(::write(pair.left.descriptor(), rubbish.data(), rubbish.size()),
              static_cast<ssize_t>(rubbish.size()));

    ASSERT_TRUE(pair.right.waitReadable(2000));
    ASSERT_TRUE(pair.right.receive());

    Frame frame;
    EXPECT_EQ(pair.right.next(frame), ReceiveStatus::Malformed);
    EXPECT_FALSE(pair.right.error().empty());
}

// ---------------------------------------------------------------------------
// Listening and connecting
// ---------------------------------------------------------------------------

TEST(Listener, AClientConnectsAndTalksToTheServer) {
    const ScratchDirectory scratch;
    Listener listener;
    std::string error;
    ASSERT_TRUE(listener.listen(scratch.socketPath(), error)) << error;
    EXPECT_TRUE(std::filesystem::exists(scratch.socketPath()));

    Connection client;
    ASSERT_TRUE(enzan::ipc::connectTo(scratch.socketPath(), client, error)) << error;

    Connection server;
    ASSERT_TRUE(listener.accept(server, error)) << error;
    EXPECT_TRUE(server.isOpen());

    TranscriptMessage request;
    request.text = "hello from the client";
    client.send(MessageType::Transcript, request);
    transfer(client, server);
    EXPECT_EQ(takeTranscript(server), "hello from the client");

    TranscriptMessage reply;
    reply.text = "hello from the kernel";
    server.send(MessageType::Transcript, reply);
    transfer(server, client);
    EXPECT_EQ(takeTranscript(client), "hello from the kernel");
}

TEST(Listener, AcceptingWithNobodyWaitingIsNotAnError) {
    const ScratchDirectory scratch;
    Listener listener;
    std::string error;
    ASSERT_TRUE(listener.listen(scratch.socketPath(), error)) << error;

    Connection accepted;
    EXPECT_FALSE(listener.accept(accepted, error));
    EXPECT_TRUE(error.empty()) << "nobody waiting is the ordinary case, not a failure";
    EXPECT_FALSE(accepted.isOpen());
}

TEST(Listener, ClosingRemovesTheSocketPath) {
    const ScratchDirectory scratch;
    {
        Listener listener;
        std::string error;
        ASSERT_TRUE(listener.listen(scratch.socketPath(), error)) << error;
        EXPECT_TRUE(std::filesystem::exists(scratch.socketPath()));
    }
    EXPECT_FALSE(std::filesystem::exists(scratch.socketPath()))
        << "a socket left behind makes the next kernel do the stale-path dance for nothing";
}

TEST(Listener, ASocketLeftByADeadKernelIsReused) {
    const ScratchDirectory scratch;

    // A plain file standing in for what a kernel that was killed leaves behind: the path
    // exists and nothing is serving it.
    {
        const std::ofstream stale(scratch.socketPath());
        ASSERT_TRUE(stale.good());
    }
    ASSERT_TRUE(std::filesystem::exists(scratch.socketPath()));

    Listener listener;
    std::string error;
    EXPECT_TRUE(listener.listen(scratch.socketPath(), error)) << error;
}

TEST(Listener, ASocketAnotherKernelIsServingIsRefused) {
    // The other half of the same decision. Unlinking without checking would let a second
    // kernel silently steal a socket a running one is serving, and the user would see a GUI
    // attach to a simulation that is not the one they started.
    const ScratchDirectory scratch;

    Listener first;
    std::string error;
    ASSERT_TRUE(first.listen(scratch.socketPath(), error)) << error;

    Listener second;
    EXPECT_FALSE(second.listen(scratch.socketPath(), error));
    EXPECT_NE(error.find("already being served"), std::string::npos) << error;

    // The refusal must leave the running kernel's socket alone.
    Connection client;
    std::string connectError;
    EXPECT_TRUE(enzan::ipc::connectTo(scratch.socketPath(), client, connectError)) << connectError;
}

TEST(Listener, ConnectingToNothingFailsWithTheReason) {
    const ScratchDirectory scratch;

    Connection client;
    std::string error;
    EXPECT_FALSE(enzan::ipc::connectTo(scratch.socketPath(), client, error));
    EXPECT_NE(error.find("could not connect"), std::string::npos) << error;
}

TEST(Listener, AnOverlongPathIsRefusedWithTheLimit) {
    Connection client;
    std::string error;
    const std::filesystem::path tooLong = "/tmp/" + std::string(200, 'p');

    EXPECT_FALSE(enzan::ipc::connectTo(tooLong, client, error));
    EXPECT_NE(error.find("must be shorter than"), std::string::npos) << error;
}

} // namespace
