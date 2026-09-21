// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/Codec.h"

#include <cstring>

namespace enzan::ipc {

namespace {

/// Appends `count` little-endian bytes of `value`.
void appendLittleEndian(std::vector<std::byte>& out, std::uint64_t value, std::size_t count) {
    for (std::size_t byte = 0; byte < count; ++byte) {
        out.push_back(static_cast<std::byte>((value >> (8U * byte)) & 0xFFU));
    }
}

/// Reads `count` little-endian bytes starting at `at`.
std::uint64_t readLittleEndian(std::span<const std::byte> in, std::size_t at, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < count; ++byte) {
        value |= static_cast<std::uint64_t>(in[at + byte]) << (8U * byte);
    }
    return value;
}

} // namespace

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void MessageWriter::u8(std::uint8_t& value) {
    out->push_back(static_cast<std::byte>(value));
}

void MessageWriter::i8(std::int8_t& value) {
    out->push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value)));
}

void MessageWriter::u16(std::uint16_t& value) {
    appendLittleEndian(*out, value, 2);
}

void MessageWriter::u32(std::uint32_t& value) {
    appendLittleEndian(*out, value, 4);
}

void MessageWriter::u64(std::uint64_t& value) {
    appendLittleEndian(*out, value, 8);
}

void MessageWriter::boolean(bool& value) {
    out->push_back(static_cast<std::byte>(value ? 1U : 0U));
}

void MessageWriter::str(std::string& value) {
    auto length = static_cast<std::uint32_t>(value.size());
    u32(length);
    for (const char character : value) {
        out->push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void MessageWriter::bits(eir::BitVector& value) {
    auto width = value.width();
    u32(width);

    // Both planes, in the order they sit in memory. That is deliberately the same layout the
    // state arena uses (DESIGN_GUIDE.md section 3.6), so a value crosses the socket without
    // being taken apart and put back together.
    const eir::ConstBitRef view = value.ref();
    const std::uint32_t words = eir::storageWordsFor(width);
    for (std::uint32_t word = 0; word < words; ++word) {
        appendLittleEndian(*out, view.aval()[word], 8);
    }
}

void MessageWriter::u32List(std::vector<std::uint32_t>& items) {
    auto count = static_cast<std::uint32_t>(items.size());
    u32(count);
    for (std::uint32_t& item : items) u32(item);
}

void MessageWriter::strList(std::vector<std::string>& items) {
    auto count = static_cast<std::uint32_t>(items.size());
    u32(count);
    for (std::string& item : items) str(item);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool MessageReader::take(void* target, std::size_t count) {
    if (broken || remaining() < count) {
        fail();
        return false;
    }
    std::memcpy(target, in.data() + position, count);
    position += count;
    return true;
}

void MessageReader::u8(std::uint8_t& value) {
    value = 0;
    take(&value, 1);
}

void MessageReader::i8(std::int8_t& value) {
    std::uint8_t raw = 0;
    u8(raw);
    value = static_cast<std::int8_t>(raw);
}

void MessageReader::u16(std::uint16_t& value) {
    value = 0;
    if (broken || remaining() < 2) {
        fail();
        return;
    }
    value = static_cast<std::uint16_t>(readLittleEndian(in, position, 2));
    position += 2;
}

void MessageReader::u32(std::uint32_t& value) {
    value = 0;
    if (broken || remaining() < 4) {
        fail();
        return;
    }
    value = static_cast<std::uint32_t>(readLittleEndian(in, position, 4));
    position += 4;
}

void MessageReader::u64(std::uint64_t& value) {
    value = 0;
    if (broken || remaining() < 8) {
        fail();
        return;
    }
    value = readLittleEndian(in, position, 8);
    position += 8;
}

void MessageReader::boolean(bool& value) {
    std::uint8_t raw = 0;
    u8(raw);

    // Anything but 0 and 1 means the sender and this build disagree about the field, which is
    // exactly the skew a sticky failure exists to catch.
    if (raw > 1) fail();
    value = raw == 1;
}

void MessageReader::str(std::string& value) {
    value.clear();

    std::uint32_t length = 0;
    u32(length);
    if (broken || length > remaining()) {
        fail();
        return;
    }

    value.resize(length);
    take(value.data(), length);
}

void MessageReader::bits(eir::BitVector& value) {
    std::uint32_t width = 0;
    u32(width);

    if (broken || width == 0 || width > MaxSignalBits) {
        fail();
        value = eir::BitVector::filled(1, eir::Logic::Unknown);
        return;
    }

    const std::uint32_t words = eir::storageWordsFor(width);
    if (remaining() < std::size_t{words} * 8) {
        fail();
        value = eir::BitVector::filled(1, eir::Logic::Unknown);
        return;
    }

    value = eir::BitVector::filled(width, eir::Logic::Zero);
    eir::Word* storage = value.mutableRef().aval();
    for (std::uint32_t word = 0; word < words; ++word) {
        storage[word] = readLittleEndian(in, position, 8);
        position += 8;
    }

    // The sender is expected to have sent canonical storage, but a value whose unused top bits
    // were set would make two equal values compare unequal everywhere downstream, so the
    // guarantee is re-established here rather than assumed.
    value.mutableRef().canonicalize();
}

void MessageReader::u32List(std::vector<std::uint32_t>& items) {
    items.clear();

    std::uint32_t count = 0;
    u32(count);
    if (broken || std::size_t{count} * 4 > remaining()) {
        fail();
        return;
    }

    items.resize(count);
    for (std::uint32_t& item : items) u32(item);
}

void MessageReader::strList(std::vector<std::string>& items) {
    items.clear();

    std::uint32_t count = 0;
    u32(count);

    // Every string costs at least its four-byte length prefix, so a count that could not fit
    // cannot be honest and must not become a resize.
    if (broken || std::size_t{count} * 4 > remaining()) {
        fail();
        return;
    }

    items.resize(count);
    for (std::string& item : items) str(item);
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

FrameStatus tryReadFrame(std::span<const std::byte> buffer, Frame& frame, std::size_t& consumed) {
    consumed = 0;
    if (buffer.size() < FrameHeaderBytes) return FrameStatus::Incomplete;

    const auto payloadBytes = static_cast<std::uint32_t>(readLittleEndian(buffer, 0, 4));
    if (payloadBytes > MaxPayloadBytes) return FrameStatus::Malformed;

    const std::size_t total = FrameHeaderBytes + payloadBytes;
    if (buffer.size() < total) return FrameStatus::Incomplete;

    frame.type = static_cast<std::uint16_t>(readLittleEndian(buffer, 4, 2));
    frame.payload = buffer.subspan(FrameHeaderBytes, payloadBytes);
    consumed = total;
    return FrameStatus::Complete;
}

} // namespace enzan::ipc
