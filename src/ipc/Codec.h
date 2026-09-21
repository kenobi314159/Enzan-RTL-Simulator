// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace enzan::ipc {

/// Bytes of frame header preceding every payload: a length, a type and two spare bytes.
constexpr std::size_t FrameHeaderBytes = 8;

/// Largest payload a frame may declare.
///
/// A length read off a socket is untrusted input even when the peer is friendly, because a
/// version mismatch or a half-written frame produces a plausible-looking enormous number. The
/// cap turns that into a protocol error instead of an allocation the machine cannot satisfy.
constexpr std::uint32_t MaxPayloadBytes = 64U << 20U;

/// Largest signal width a value may claim, for the same reason.
constexpr std::uint32_t MaxSignalBits = 1U << 24U;

/// Encodes protocol fields into a byte buffer.
///
/// The writer and the reader below present the same set of member functions, so one
/// `serialize` function per message serves both directions. That is the point of the shape:
/// an encoder and a decoder written separately drift, and a field added to one and forgotten
/// in the other is a bug that only appears when the two ends are different builds.
///
/// Everything is little-endian on the wire. Enzan is Linux-only on little-endian hardware
/// today, but a protocol that matched whatever the host happened to be would be a trap for
/// the remote-attach case section 2.2 calls for.
class MessageWriter {
public:
    explicit MessageWriter(std::vector<std::byte>& target) noexcept : out(&target) {}

    /// True for the reader, false here. Lets a shared `serialize` branch on direction in the
    /// rare case where it must, without either side needing its own copy.
    static constexpr bool IsReading = false;

    void u8(std::uint8_t& value);
    void i8(std::int8_t& value);
    void u16(std::uint16_t& value);
    void u32(std::uint32_t& value);
    void u64(std::uint64_t& value);
    void boolean(bool& value);
    void str(std::string& value);
    void bits(eir::BitVector& value);

    /// A length-prefixed sequence of fixed-width unsigned values.
    void u32List(std::vector<std::uint32_t>& items);

    /// A length-prefixed sequence of strings.
    void strList(std::vector<std::string>& items);

    /// A length-prefixed sequence of structures, each written by its own `serialize`.
    template<typename Element>
    void list(std::vector<Element>& items) {
        auto count = static_cast<std::uint32_t>(items.size());
        u32(count);
        for (Element& item : items) serialize(*this, item);
    }

    [[nodiscard]] bool failed() const noexcept { return false; }

private:
    std::vector<std::byte>* out;
};

/// Decodes protocol fields from a byte buffer. See MessageWriter.
///
/// Every read is bounds-checked and a failure is sticky: once the reader has run past the end
/// of the payload or seen a field it cannot accept, every later read is a no-op and `failed`
/// stays true. A decoder that reported only the first problem would leave the caller holding a
/// half-filled message that looked usable.
class MessageReader {
public:
    explicit MessageReader(std::span<const std::byte> source) noexcept : in(source) {}

    static constexpr bool IsReading = true;

    void u8(std::uint8_t& value);
    void i8(std::int8_t& value);
    void u16(std::uint16_t& value);
    void u32(std::uint32_t& value);
    void u64(std::uint64_t& value);
    void boolean(bool& value);
    void str(std::string& value);
    void bits(eir::BitVector& value);

    void u32List(std::vector<std::uint32_t>& items);
    void strList(std::vector<std::string>& items);

    template<typename Element>
    void list(std::vector<Element>& items) {
        std::uint32_t count = 0;
        u32(count);

        // Every element occupies at least one byte, so a count larger than what is left cannot
        // be honest. Checking before resizing is what stops a corrupt length from asking for an
        // allocation of gigabytes.
        if (broken || count > remaining()) {
            fail();
            items.clear();
            return;
        }

        items.clear();
        items.resize(count);
        for (Element& item : items) serialize(*this, item);
    }

    [[nodiscard]] bool failed() const noexcept { return broken; }

    /// True when the payload was consumed exactly, which is what a well-formed message does.
    ///
    /// Trailing bytes are treated as a failure rather than ignored: they mean the sender and
    /// the receiver disagree about the message, and silently accepting the part that parsed
    /// is how a version skew turns into wrong values on screen.
    [[nodiscard]] bool consumedEverything() const noexcept {
        return !broken && position == in.size();
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return in.size() - position; }

private:
    /// Copies `count` bytes into `target`, or fails.
    bool take(void* target, std::size_t count);

    void fail() noexcept { broken = true; }

    std::span<const std::byte> in;
    std::size_t position = 0;
    bool broken = false;
};

/// One framed message sitting in a buffer.
struct Frame {
    std::uint16_t type = 0;
    std::span<const std::byte> payload;
};

/// Writes a framed message into `out`, appending to whatever is already there.
///
/// The header is written first with a placeholder length and patched afterwards, so that a
/// message never has to be encoded twice to find out how long it is.
template<typename Message>
void encodeFrame(std::uint16_t type, Message&& message, std::vector<std::byte>& out) {
    const std::size_t headerAt = out.size();
    out.resize(headerAt + FrameHeaderBytes, std::byte{0});

    MessageWriter writer(out);
    serialize(writer, message);

    const auto payloadBytes = static_cast<std::uint32_t>(out.size() - headerAt - FrameHeaderBytes);
    for (std::size_t byte = 0; byte < 4; ++byte) {
        out[headerAt + byte] = static_cast<std::byte>((payloadBytes >> (8U * byte)) & 0xFFU);
    }
    out[headerAt + 4] = static_cast<std::byte>(type & 0xFFU);
    out[headerAt + 5] = static_cast<std::byte>((type >> 8U) & 0xFFU);
}

/// How `tryReadFrame` ended.
enum class FrameStatus : std::uint8_t {
    /// A whole frame was found; `frame` describes it and `consumed` bytes may be dropped.
    Complete,
    /// Not enough bytes yet. Read more and ask again.
    Incomplete,
    /// The header is not one this build can accept. The connection must be dropped: there is
    /// no way to resynchronise a length-prefixed stream whose length is wrong.
    Malformed,
};

/// Looks for one complete frame at the front of `buffer`.
FrameStatus tryReadFrame(std::span<const std::byte> buffer, Frame& frame, std::size_t& consumed);

/// Decodes a payload into `message`, requiring it to be consumed exactly.
template<typename Message>
bool decodePayload(std::span<const std::byte> payload, Message& message) {
    MessageReader reader(payload);
    serialize(reader, message);
    return reader.consumedEverything();
}

} // namespace enzan::ipc
