// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the wire format.
//
// Two things are being pinned. The first is that every message survives the round trip with
// every field intact, which is the property a shared `serialize` is meant to guarantee and which
// would otherwise only be discovered when a GUI and a kernel from different builds met. The
// second is that a truncated, oversized or simply wrong stream is refused rather than
// half-decoded: a client's socket is untrusted input even when the client is friendly.

#include "ipc/Protocol.h"

#include "ipc/Codec.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using enzan::eir::ArrayDimension;
using enzan::eir::BitVector;
using enzan::eir::Logic;
using enzan::ipc::ByeMessage;
using enzan::ipc::ChangeCursor;
using enzan::ipc::ChangeEntry;
using enzan::ipc::ChangesMessage;
using enzan::ipc::DepositMessage;
using enzan::ipc::DesignMessage;
using enzan::ipc::DiagnosticMessage;
using enzan::ipc::FailureMessage;
using enzan::ipc::FetchChangesMessage;
using enzan::ipc::ForceMessage;
using enzan::ipc::Frame;
using enzan::ipc::FrameStatus;
using enzan::ipc::InstanceEntry;
using enzan::ipc::MessageType;
using enzan::ipc::MessageWriter;
using enzan::ipc::ObjectEntry;
using enzan::ipc::ObjectsMessage;
using enzan::ipc::ProgressMessage;
using enzan::ipc::ProtocolVersion;
using enzan::ipc::Recording;
using enzan::ipc::RunMessage;
using enzan::ipc::RunStateMessage;
using enzan::ipc::SignalChanges;
using enzan::ipc::SimulationState;
using enzan::ipc::SubscribeMessage;
using enzan::ipc::TranscriptMessage;
using enzan::ipc::WelcomeMessage;
using enzan::test::literal;

/// Encodes `message`, reads the frame back and decodes it into a fresh object.
///
/// Going through the frame rather than straight through the codec is deliberate: the length the
/// writer patches in afterwards and the length the reader trusts are the same number, and a
/// mistake in either is only visible when both run.
template<typename Message>
Message roundTrip(MessageType type, Message& message) {
    std::vector<std::byte> buffer;
    enzan::ipc::encode(type, message, buffer);

    Frame frame;
    std::size_t consumed = 0;
    EXPECT_EQ(enzan::ipc::tryReadFrame(buffer, frame, consumed), FrameStatus::Complete);
    EXPECT_EQ(consumed, buffer.size()) << "the frame did not describe the whole buffer";
    EXPECT_EQ(frame.type, static_cast<std::uint16_t>(type));

    Message decoded;
    EXPECT_TRUE(enzan::ipc::decodePayload(frame.payload, decoded));
    return decoded;
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(Protocol, WelcomeSurvivesTheRoundTrip) {
    WelcomeMessage sent;
    sent.protocolVersion = ProtocolVersion;
    sent.kernelVersion = "enzan 0.1.0-dev";
    sent.topModule = "tb";
    sent.timeExponent = -9;
    sent.currentTime = 1'234'567'890'123ULL;
    sent.latestRecordedTime = 2'000'000ULL;
    sent.state = SimulationState::Running;
    sent.widthLimit = 65536;

    const WelcomeMessage got = roundTrip(MessageType::Welcome, sent);

    EXPECT_EQ(got.protocolVersion, sent.protocolVersion);
    EXPECT_EQ(got.kernelVersion, sent.kernelVersion);
    EXPECT_EQ(got.topModule, sent.topModule);
    EXPECT_EQ(got.timeExponent, -9) << "a negative exponent must not arrive as 247";
    EXPECT_EQ(got.currentTime, sent.currentTime);
    EXPECT_EQ(got.latestRecordedTime, sent.latestRecordedTime);
    EXPECT_EQ(got.state, SimulationState::Running);
    EXPECT_EQ(got.widthLimit, 65536U);
}

TEST(Protocol, TheDesignTreeSurvivesTheRoundTrip) {
    DesignMessage sent;
    sent.instances.push_back(
        InstanceEntry{.path = "/tb", .name = "tb", .file = "tb.sv", .line = 3});
    sent.instances.push_back(InstanceEntry{
        .path = "/tb/u_dut", .name = "u_dut", .parent = 0, .file = "counter.sv", .line = 7});
    sent.instances.push_back(
        InstanceEntry{.path = "/tb/u_dut/u_inner", .name = "u_inner", .parent = 1});
    sent.sourceFiles = {"tb.sv", "counter.sv"};

    const DesignMessage got = roundTrip(MessageType::Design, sent);

    ASSERT_EQ(got.instances.size(), 3U);
    EXPECT_EQ(got.instances[0].parent, InstanceEntry::NoParent);
    EXPECT_EQ(got.instances[2].path, "/tb/u_dut/u_inner");
    EXPECT_EQ(got.instances[2].parent, 1U);

    // Where a scope's module is declared, so that clicking it can open its source.
    EXPECT_EQ(got.instances[1].file, "counter.sv");
    EXPECT_EQ(got.instances[1].line, 7U);
    EXPECT_EQ(got.instances[2].line, 0U) << "an unknown position stays unknown";
    EXPECT_EQ(got.sourceFiles, sent.sourceFiles);
}

TEST(Protocol, ObjectsSurviveTheRoundTrip) {
    ObjectsMessage sent;
    sent.instancePath = "/tb";
    sent.objects.push_back(ObjectEntry{.signalIndex = 4,
                                       .name = "clk",
                                       .path = "/tb/clk",
                                       .width = 1,
                                       .recording = Recording::Recorded});
    sent.objects.push_back(ObjectEntry{.signalIndex = 9,
                                       .name = "count",
                                       .path = "/tb/count",
                                       .width = 32,
                                       .recording = Recording::OutsideScope});
    // logic [7:0] grid [0:1][-1:-2], whose bounds may be negative
    sent.objects.push_back(ObjectEntry{.signalIndex = 11,
                                       .name = "grid",
                                       .path = "/tb/grid",
                                       .width = 32,
                                       .recording = Recording::TooWide,
                                       .dimensions = {ArrayDimension{.left = 0, .right = 1},
                                                      ArrayDimension{.left = -1, .right = -2},
                                                      ArrayDimension{.left = 7, .right = 0}}});

    const ObjectsMessage got = roundTrip(MessageType::Objects, sent);

    EXPECT_EQ(got.instancePath, "/tb");
    ASSERT_EQ(got.objects.size(), 3U);
    EXPECT_EQ(got.objects[0].name, "clk");
    EXPECT_EQ(got.objects[0].recording, Recording::Recorded);
    EXPECT_TRUE(got.objects[0].dimensions.empty());
    EXPECT_EQ(got.objects[1].width, 32U);
    EXPECT_EQ(got.objects[1].recording, Recording::OutsideScope);
    EXPECT_EQ(got.objects[2].recording, Recording::TooWide);
    EXPECT_EQ(got.objects[2].dimensions, sent.objects[2].dimensions);
}

TEST(Protocol, FourStateValuesSurviveTheRoundTrip) {
    // The whole reason the two-plane encoding crosses the socket unchanged: an X or a Z that
    // arrived as a 0 would be a wrong simulation result shown to the user as a fact.
    ChangesMessage sent;
    sent.signals.push_back(
        SignalChanges{.signalIndex = 1,
                      .width = 8,
                      .first = 0,
                      .recorded = 1,
                      .completeTo = 4'000,
                      .entries = {ChangeEntry{.time = 4'000, .value = literal("8'b10xz01x1")}}});
    sent.signals.push_back(
        SignalChanges{.signalIndex = 7,
                      .width = 4,
                      .first = 0,
                      .recorded = 1,
                      .completeTo = 4'000,
                      .entries = {ChangeEntry{.time = 4'000, .value = literal("4'hf")}}});

    const ChangesMessage got = roundTrip(MessageType::Changes, sent);

    ASSERT_EQ(got.signals.size(), 2U);
    EXPECT_EQ(got.signals[0].signalIndex, 1U);
    ASSERT_EQ(got.signals[0].entries.size(), 1U);
    EXPECT_EQ(got.signals[0].entries[0].time, 4'000U);
    EXPECT_EQ(got.signals[0].entries[0].value, literal("8'b10xz01x1"));
    ASSERT_EQ(got.signals[1].entries.size(), 1U);
    EXPECT_EQ(got.signals[1].entries[0].value, literal("4'hf"));
}

TEST(Protocol, AValueWiderThanOneWordSurvivesTheRoundTrip) {
    BitVector wide = BitVector::filled(130, Logic::Zero);
    wide.set(0, Logic::One);
    wide.set(64, Logic::Unknown);
    wide.set(129, Logic::HighZ);
    ChangesMessage sent;
    sent.signals.push_back(SignalChanges{.signalIndex = 0,
                                         .width = 130,
                                         .first = 0,
                                         .recorded = 1,
                                         .completeTo = 1,
                                         .entries = {ChangeEntry{.time = 1, .value = wide}}});

    const ChangesMessage got = roundTrip(MessageType::Changes, sent);

    ASSERT_EQ(got.signals.size(), 1U);
    ASSERT_EQ(got.signals[0].entries.size(), 1U);
    const BitVector& value = got.signals[0].entries[0].value;
    EXPECT_EQ(value.width(), 130U);
    EXPECT_EQ(value.get(0), Logic::One);
    EXPECT_EQ(value.get(64), Logic::Unknown);
    EXPECT_EQ(value.get(129), Logic::HighZ);
    EXPECT_EQ(value.get(1), Logic::Zero);
}

TEST(Protocol, AnAnswerOfChangesKeepsWhereItStandsInTheSignal) {
    // Which changes these are, how many the kernel holds and how far the copy is complete are
    // what the client needs to know whether to ask again and how far to draw.
    ChangesMessage sent;
    sent.signals.push_back(
        SignalChanges{.signalIndex = 3,
                      .width = 4,
                      .first = 2,
                      .recorded = 9,
                      .completeTo = 30,
                      .entries = {ChangeEntry{.time = 10, .value = literal("4'bxxxx")},
                                  ChangeEntry{.time = 20, .value = literal("4'h3")}}});

    const ChangesMessage got = roundTrip(MessageType::Changes, sent);

    ASSERT_EQ(got.signals.size(), 1U);
    const SignalChanges& changes = got.signals[0];
    EXPECT_EQ(changes.signalIndex, 3U);
    EXPECT_EQ(changes.width, 4U);
    EXPECT_EQ(changes.first, 2U);
    EXPECT_EQ(changes.recorded, 9U);
    EXPECT_EQ(changes.completeTo, 30U);
    ASSERT_EQ(changes.entries.size(), 2U);
    EXPECT_EQ(changes.entries[1].time, 20U);
    EXPECT_EQ(changes.entries[1].value, literal("4'h3"));
}

TEST(Protocol, DiagnosticsSurviveTheRoundTripWithTheirNotes) {
    DiagnosticMessage sent;
    sent.code = 3005;
    sent.severity = 3;
    sent.message = "port 'd' of 'fifo' is 8 bits but is connected to a 4-bit object";
    sent.file = "/designs/fifo.sv";
    sent.line = 42;
    sent.column = 17;
    sent.notes = {"the port is declared here", "the connection is made here"};

    const DiagnosticMessage got = roundTrip(MessageType::Diagnostic, sent);

    EXPECT_EQ(got.code, 3005);
    EXPECT_EQ(got.severity, 3);
    EXPECT_EQ(got.message, sent.message);
    EXPECT_EQ(got.file, sent.file);
    EXPECT_EQ(got.line, 42U);
    EXPECT_EQ(got.column, 17U);
    ASSERT_EQ(got.notes.size(), 2U);
    EXPECT_EQ(got.notes[1], "the connection is made here");
}

TEST(Protocol, SmallMessagesSurviveTheRoundTrip) {
    SubscribeMessage subscribe;
    subscribe.signalIndices = {1, 2, 3, 99};
    EXPECT_EQ(roundTrip(MessageType::Subscribe, subscribe).signalIndices, subscribe.signalIndices);

    RunMessage run;
    run.duration = 100'000;
    run.untilFinished = true;
    const RunMessage runBack = roundTrip(MessageType::Run, run);
    EXPECT_EQ(runBack.duration, 100'000U);
    EXPECT_TRUE(runBack.untilFinished);

    FetchChangesMessage fetch;
    fetch.signals = {ChangeCursor{.signalIndex = 8, .held = 5},
                     ChangeCursor{.signalIndex = 2, .held = 0}};
    const FetchChangesMessage fetchBack = roundTrip(MessageType::FetchChanges, fetch);
    ASSERT_EQ(fetchBack.signals.size(), 2U);
    EXPECT_EQ(fetchBack.signals[0].signalIndex, 8U);
    EXPECT_EQ(fetchBack.signals[0].held, 5U);

    ProgressMessage progress;
    progress.time = 70;
    progress.latestRecordedTime = 75;
    const ProgressMessage progressBack = roundTrip(MessageType::Progress, progress);
    EXPECT_EQ(progressBack.time, 70U);
    EXPECT_EQ(progressBack.latestRecordedTime, 75U);

    TranscriptMessage transcript;
    transcript.text = "count = 42\n";
    EXPECT_EQ(roundTrip(MessageType::Transcript, transcript).text, transcript.text);

    ByeMessage bye;
    bye.reason = "the kernel is exiting";
    EXPECT_EQ(roundTrip(MessageType::Bye, bye).reason, bye.reason);

    FailureMessage failure;
    failure.reason = "no such scope";
    EXPECT_EQ(roundTrip(MessageType::Failure, failure).reason, failure.reason);

    RunStateMessage state;
    state.state = SimulationState::Finished;
    state.time = 700;
    state.latestRecordedTime = 700;
    state.reason = "finished";
    const RunStateMessage stateBack = roundTrip(MessageType::RunState, state);
    EXPECT_EQ(stateBack.state, SimulationState::Finished);
    EXPECT_EQ(stateBack.reason, "finished");
}

TEST(Protocol, ADepositAndAForceSurviveTheRoundTrip) {
    DepositMessage deposit;
    deposit.signalIndex = 5;
    deposit.value = literal("8'hA5");
    const DepositMessage depositBack = roundTrip(MessageType::Deposit, deposit);
    EXPECT_EQ(depositBack.signalIndex, 5U);
    EXPECT_EQ(depositBack.value, deposit.value);

    ForceMessage force;
    force.signalIndex = 9;
    force.value = literal("4'b10xz");
    const ForceMessage forceBack = roundTrip(MessageType::Force, force);
    EXPECT_EQ(forceBack.signalIndex, 9U);
    EXPECT_EQ(forceBack.value, force.value) << "X and Z survive a force";
    EXPECT_FALSE(forceBack.release);

    ForceMessage release;
    release.signalIndex = 9;
    release.release = true;
    EXPECT_TRUE(roundTrip(MessageType::Force, release).release);
}

TEST(Protocol, AMessageWithNoFieldsStillFrames) {
    enzan::ipc::StopMessage stop;
    std::vector<std::byte> buffer;
    enzan::ipc::encode(MessageType::Stop, stop, buffer);

    EXPECT_EQ(buffer.size(), enzan::ipc::FrameHeaderBytes);

    Frame frame;
    std::size_t consumed = 0;
    EXPECT_EQ(enzan::ipc::tryReadFrame(buffer, frame, consumed), FrameStatus::Complete);
    EXPECT_EQ(frame.type, static_cast<std::uint16_t>(MessageType::Stop));
    EXPECT_TRUE(frame.payload.empty());
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

TEST(Protocol, SeveralMessagesInOneBufferAreReadInOrder) {
    // A stream socket hands over whatever has arrived, which is routinely several messages at
    // once, so reading must be driven by the lengths rather than by the read boundaries.
    std::vector<std::byte> buffer;
    TranscriptMessage first;
    first.text = "one";
    TranscriptMessage second;
    second.text = "two";
    enzan::ipc::encode(MessageType::Transcript, first, buffer);
    enzan::ipc::encode(MessageType::Transcript, second, buffer);

    std::span<const std::byte> rest(buffer);
    std::vector<std::string> texts;
    while (!rest.empty()) {
        Frame frame;
        std::size_t consumed = 0;
        ASSERT_EQ(enzan::ipc::tryReadFrame(rest, frame, consumed), FrameStatus::Complete);

        TranscriptMessage decoded;
        ASSERT_TRUE(enzan::ipc::decodePayload(frame.payload, decoded));
        texts.push_back(decoded.text);
        rest = rest.subspan(consumed);
    }

    ASSERT_EQ(texts.size(), 2U);
    EXPECT_EQ(texts[0], "one");
    EXPECT_EQ(texts[1], "two");
}

TEST(Protocol, APartialFrameAsksForMoreBytes) {
    TranscriptMessage message;
    message.text = "a reasonably long transcript line";
    std::vector<std::byte> buffer;
    enzan::ipc::encode(MessageType::Transcript, message, buffer);

    // Every proper prefix must ask for more rather than decoding something.
    for (std::size_t length = 0; length < buffer.size(); ++length) {
        Frame frame;
        std::size_t consumed = 0;
        const std::span<const std::byte> prefix(buffer.data(), length);
        EXPECT_EQ(enzan::ipc::tryReadFrame(prefix, frame, consumed), FrameStatus::Incomplete)
            << "a prefix of " << length << " bytes decoded as a whole frame";
    }
}

TEST(Protocol, AnAbsurdLengthIsMalformedRatherThanAllocated) {
    std::vector<std::byte> buffer(enzan::ipc::FrameHeaderBytes, std::byte{0xFF});

    Frame frame;
    std::size_t consumed = 0;
    EXPECT_EQ(enzan::ipc::tryReadFrame(buffer, frame, consumed), FrameStatus::Malformed);
    EXPECT_EQ(consumed, 0U);
}

// ---------------------------------------------------------------------------
// Refusing bad payloads
// ---------------------------------------------------------------------------

TEST(Protocol, ATruncatedPayloadFailsToDecode) {
    WelcomeMessage sent;
    sent.kernelVersion = "enzan 0.1.0-dev";
    sent.topModule = "tb";

    std::vector<std::byte> buffer;
    enzan::ipc::encode(MessageType::Welcome, sent, buffer);

    // Every payload prefix short of the whole thing must be refused. A decoder that returned
    // the fields it managed to read would hand the caller a message that looked usable.
    const std::span<const std::byte> payload(buffer.data() + enzan::ipc::FrameHeaderBytes,
                                             buffer.size() - enzan::ipc::FrameHeaderBytes);
    for (std::size_t length = 0; length < payload.size(); ++length) {
        WelcomeMessage decoded;
        EXPECT_FALSE(enzan::ipc::decodePayload(payload.subspan(0, length), decoded))
            << "a payload of " << length << " bytes decoded as a whole message";
    }
}

TEST(Protocol, TrailingBytesFailToDecode) {
    TranscriptMessage sent;
    sent.text = "hello";

    std::vector<std::byte> payload;
    MessageWriter writer(payload);
    serialize(writer, sent);
    payload.push_back(std::byte{0});

    TranscriptMessage decoded;
    EXPECT_FALSE(enzan::ipc::decodePayload(payload, decoded))
        << "a payload the sender and receiver disagree about must not be half-accepted";
}

TEST(Protocol, AnImpossibleStringLengthFailsToDecode) {
    const std::vector<std::byte> payload(4, std::byte{0xFF});

    TranscriptMessage decoded;
    EXPECT_FALSE(enzan::ipc::decodePayload(payload, decoded));
    EXPECT_TRUE(decoded.text.empty());
}

TEST(Protocol, AnImpossibleListCountFailsToDecode) {
    const std::vector<std::byte> payload(4, std::byte{0xFF});

    DesignMessage decoded;
    EXPECT_FALSE(enzan::ipc::decodePayload(payload, decoded));
    EXPECT_TRUE(decoded.instances.empty());
}

TEST(Protocol, AZeroWidthValueFailsToDecode) {
    // A bit vector must have at least one bit; letting a zero-width one through would trip an
    // assertion deep in the value engine instead of being reported as the protocol error it is.
    std::vector<std::byte> payload;
    MessageWriter writer(payload);
    std::uint32_t signals = 1;
    writer.u32(signals);
    std::uint32_t signalIndex = 0;
    writer.u32(signalIndex);
    std::uint32_t declared = 1;
    writer.u32(declared);
    std::uint64_t first = 0;
    writer.u64(first);
    std::uint64_t recorded = 1;
    writer.u64(recorded);
    std::uint64_t completeTo = 0;
    writer.u64(completeTo);
    std::uint32_t entries = 1;
    writer.u32(entries);
    std::uint64_t time = 0;
    writer.u64(time);
    std::uint32_t width = 0;
    writer.u32(width);

    ChangesMessage decoded;
    EXPECT_FALSE(enzan::ipc::decodePayload(payload, decoded));
}

TEST(Protocol, ABooleanThatIsNeitherTrueNorFalseFailsToDecode) {
    std::vector<std::byte> payload;
    MessageWriter writer(payload);
    std::uint64_t duration = 10;
    writer.u64(duration);
    payload.push_back(std::byte{7});

    RunMessage decoded;
    EXPECT_FALSE(enzan::ipc::decodePayload(payload, decoded));
}

TEST(Protocol, AnUnknownStateDecodesAsFailedRatherThanOutOfRange) {
    // Hand-built rather than round-tripped, because the point is a byte no enumerator has.
    // The rest of the payload has to be a whole RunStateMessage all the same, or the failure
    // under test would be indistinguishable from the reader running off the end.
    std::vector<std::byte> payload;
    payload.push_back(std::byte{200});
    MessageWriter writer(payload);
    std::uint64_t time = 0;
    writer.u64(time);
    writer.u64(time);
    std::string reason;
    writer.str(reason);

    RunStateMessage decoded;
    EXPECT_TRUE(enzan::ipc::decodePayload(payload, decoded));
    EXPECT_EQ(decoded.state, SimulationState::Failed)
        << "a state from a newer peer must not become an enumerator that does not exist";
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

TEST(Protocol, MessageNamesAreStableAndUnknownOnesSayTheirNumber) {
    EXPECT_EQ(enzan::ipc::toString(MessageType::Hello), "hello");
    EXPECT_EQ(enzan::ipc::toString(MessageType::FetchChanges), "fetch-changes");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) — the point of the test
    EXPECT_EQ(enzan::ipc::toString(static_cast<MessageType>(4242)), "message(4242)");
    EXPECT_EQ(enzan::ipc::toString(SimulationState::Idle), "idle");
}

} // namespace
